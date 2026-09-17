#include "persist.h"
#include "ata.h"
#include "fs.h"
#include "users.h"
#include "mem.h"
#include "string.h"
#include "kprintf.h"
#include "install.h"

/* Persistence is a snapshot, not a block-allocating filesystem: the whole
   tree plus the account table is serialised into one contiguous run of
   sectors on the data disk. The tree is small (kilobytes), so rewriting all
   of it costs a handful of PIO transfers and avoids an allocator, a journal
   and every consistency problem that comes with them.

   Layout on the data drive:
     LBA 0            superblock
     LBA 1 .. N       payload
*/

/* Where the snapshot lives depends on how the machine is put together:

     dedicated data disk   primary slave, from its sector 0
     installed on one disk the boot disk, from INSTALL_DATA_LBA

   Both are probed at mount time. Booting from the install medium disables
   persistence entirely - a live session must not start writing to somebody's
   hard disk on its own. */
#define MAX_PAYLOAD     (1024u * 1024u)     /* 1 MiB of snapshot space */
#define PERSIST_VERSION 1

#define KIND_FILE 1
#define KIND_DIR  2

typedef struct {
    char     magic[8];          /* "LOSFS\0\0\0" */
    uint32_t version;
    uint32_t payload_bytes;
    uint32_t node_count;
    uint32_t user_count;
    uint32_t checksum;
    uint32_t saves;
    uint8_t  reserved[512 - 32];
} __attribute__((packed)) super_t;

static const char MAGIC[8] = { 'L', 'O', 'S', 'F', 'S', 0, 0, 0 };

static persist_state_t state = PERSIST_NO_DISK;
static bool     dirty;
static uint32_t bytes_used;
static uint32_t save_count;

/* Set by persist_format(). The tree and the accounts are still sitting in RAM
   after a wipe, so without this the next save - on shutdown, or after any
   command that marks the tree dirty - would write them straight back and undo
   the format. The seal only clears on the next boot. */
static bool     sealed;

static ata_drive_t data_drive = ATA_SLAVE;
static uint32_t    data_base;               /* sector of the superblock */
static const char *data_where = "none";

#define SUPER_LBA   (data_base)
#define PAYLOAD_LBA (data_base + 1)
#define DATA_DRIVE  data_drive

static uint32_t checksum(const uint8_t *data, uint32_t length)
{
    uint32_t hash = 2166136261u;

    for (uint32_t i = 0; i < length; i++) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

static uint32_t sectors_for(uint32_t bytes)
{
    return (bytes + ATA_SECTOR_SIZE - 1) / ATA_SECTOR_SIZE;
}

/* --------------------------------------------------------------------- */
/* writing                                                               */
/* --------------------------------------------------------------------- */

typedef struct {
    uint8_t *buffer;
    uint32_t capacity;
    uint32_t used;
    bool     overflow;
} writer_t;

static void put_bytes(writer_t *w, const void *data, uint32_t length)
{
    if (w->used + length > w->capacity) {
        w->overflow = true;
        return;
    }
    memcpy(w->buffer + w->used, data, length);
    w->used += length;
}

static void put_u8(writer_t *w, uint8_t value)  { put_bytes(w, &value, 1); }
static void put_u16(writer_t *w, uint16_t value) { put_bytes(w, &value, 2); }
static void put_u32(writer_t *w, uint32_t value) { put_bytes(w, &value, 4); }

static void put_string(writer_t *w, const char *s)
{
    uint8_t length = (uint8_t)strlen(s);

    put_u8(w, length);
    put_bytes(w, s, length);
}

static void write_node(writer_t *w, fs_node_t *node, uint32_t *count)
{
    char path[FS_PATH_MAX];

    fs_abspath(node, path, sizeof(path));
    if (!path[0])
        return;                             /* the root itself is implicit */

    uint16_t path_len = (uint16_t)strlen(path);
    uint32_t data_len = (node->type == FS_FILE) ? (uint32_t)node->size : 0;

    put_u8(w, node->type == FS_DIR ? KIND_DIR : KIND_FILE);
    put_u16(w, path_len);
    put_u32(w, data_len);
    put_bytes(w, path, path_len);
    if (data_len && node->data)
        put_bytes(w, node->data, data_len);

    (*count)++;
}

/* Depth first, parents before children, so a restore can create each path
   knowing its parent already exists. */
static void write_tree(writer_t *w, fs_node_t *dir, uint32_t *count)
{
    for (fs_node_t *child = dir->children; child; child = child->next) {
        write_node(w, child, count);
        if (child->type == FS_DIR)
            write_tree(w, child, count);
    }
}

int persist_save(void)
{
    if (sealed)
        return -5;                          /* wiped, waiting for a reboot */
    if (state == PERSIST_NO_DISK)
        return 0;                           /* nothing to save to */

    uint8_t *buffer = (uint8_t *)kmalloc(MAX_PAYLOAD);
    if (!buffer)
        return -1;

    writer_t w = { buffer, MAX_PAYLOAD, 0, false };
    uint32_t user_count = 0;
    uint32_t node_count = 0;

    for (int i = 0; i < users_count(); i++) {
        user_t *user = users_at(i);
        if (!user)
            continue;
        put_string(&w, user->name);
        put_string(&w, user->home);
        put_u32(&w, user->uid);
        put_u32(&w, user->gid);
        put_u32(&w, user->pw_hash);
        user_count++;
    }

    write_tree(&w, fs_root(), &node_count);

    if (w.overflow) {
        kfree(buffer);
        return -2;                          /* snapshot outgrew the reserve */
    }

    /* Round the payload up to a whole number of sectors; the tail is padding
       and the superblock records the real length. */
    uint32_t sectors = sectors_for(w.used);
    if (sectors == 0)
        sectors = 1;
    uint32_t padded = sectors * ATA_SECTOR_SIZE;
    memset(buffer + w.used, 0, padded - w.used);

    super_t super;
    memset(&super, 0, sizeof(super));
    memcpy(super.magic, MAGIC, sizeof(MAGIC));
    super.version = PERSIST_VERSION;
    super.payload_bytes = w.used;
    super.node_count = node_count;
    super.user_count = user_count;
    super.checksum = checksum(buffer, w.used);
    super.saves = ++save_count;

    /* Payload first: if power is lost midway the old superblock still refers
       to the old payload, and a torn write is caught by the checksum. */
    int result = ata_write(DATA_DRIVE, PAYLOAD_LBA, sectors, buffer);
    kfree(buffer);
    if (result != 0)
        return -3;

    if (ata_write(DATA_DRIVE, SUPER_LBA, 1, &super) != 0)
        return -4;

    bytes_used = w.used;
    state = PERSIST_LOADED;
    dirty = false;

    /* The area is no longer blank, so stop describing it that way. */
    data_where = (data_base == 0) ? "primary slave, sector 0"
                                  : "boot disk, data area";
    return 0;
}

/* --------------------------------------------------------------------- */
/* reading                                                               */
/* --------------------------------------------------------------------- */

typedef struct {
    const uint8_t *buffer;
    uint32_t length;
    uint32_t pos;
    bool     bad;
} reader_t;

static bool take_bytes(reader_t *r, void *out, uint32_t length)
{
    if (r->pos + length > r->length) {
        r->bad = true;
        return false;
    }
    memcpy(out, r->buffer + r->pos, length);
    r->pos += length;
    return true;
}

static uint8_t take_u8(reader_t *r)
{
    uint8_t value = 0;
    take_bytes(r, &value, 1);
    return value;
}

static uint16_t take_u16(reader_t *r)
{
    uint16_t value = 0;
    take_bytes(r, &value, 2);
    return value;
}

static uint32_t take_u32(reader_t *r)
{
    uint32_t value = 0;
    take_bytes(r, &value, 4);
    return value;
}

static bool take_string(reader_t *r, char *out, uint32_t out_size)
{
    uint8_t length = take_u8(r);

    if (r->bad || length >= out_size) {
        r->bad = true;
        return false;
    }
    if (!take_bytes(r, out, length))
        return false;
    out[length] = '\0';
    return true;
}

/* Does `drive` carry one of our superblocks at `base`? */
static bool probe(ata_drive_t drive, uint32_t base)
{
    super_t super;

    if (!ata_present(drive))
        return false;
    if (ata_sector_count(drive) <= base + 1)
        return false;
    if (ata_read(drive, base, 1, &super) != 0)
        return false;
    return memcmp(super.magic, MAGIC, sizeof(MAGIC)) == 0;
}

/* Chooses where the snapshot lives. An existing snapshot always wins; with
   none to be found, a dedicated data disk is preferred over carving space out
   of the boot disk. */
static bool choose_location(void)
{
    if (probe(ATA_SLAVE, 0)) {
        data_drive = ATA_SLAVE;
        data_base = 0;
        data_where = "primary slave, sector 0";
        return true;
    }
    if (probe(ATA_MASTER, INSTALL_DATA_LBA)) {
        data_drive = ATA_MASTER;
        data_base = INSTALL_DATA_LBA;
        data_where = "boot disk, data area";
        return true;
    }

    if (ata_present(ATA_SLAVE)) {
        data_drive = ATA_SLAVE;
        data_base = 0;
        data_where = "primary slave, sector 0 (blank)";
        return true;
    }
    if (ata_present(ATA_MASTER) &&
        ata_sector_count(ATA_MASTER) > INSTALL_DATA_LBA + 64) {
        data_drive = ATA_MASTER;
        data_base = INSTALL_DATA_LBA;
        data_where = "boot disk, data area (blank)";
        return true;
    }
    return false;
}

persist_state_t persist_mount(void)
{
    super_t super;

    sealed = false;
    ata_init();

    /* A live session off the install medium stays in RAM. Writing to a disk
       the user has not chosen yet would be the wrong thing to do. */
    if (install_booted_from_medium()) {
        data_where = "none, running from the install medium";
        state = PERSIST_NO_DISK;
        return state;
    }

    if (!choose_location()) {
        data_where = "none";
        state = PERSIST_NO_DISK;
        return state;
    }

    if (ata_read(DATA_DRIVE, SUPER_LBA, 1, &super) != 0) {
        state = PERSIST_ERROR;
        return state;
    }

    if (memcmp(super.magic, MAGIC, sizeof(MAGIC)) != 0) {
        state = PERSIST_EMPTY;              /* a blank disk, not a broken one */
        return state;
    }

    if (super.version != PERSIST_VERSION || super.payload_bytes == 0 ||
        super.payload_bytes > MAX_PAYLOAD) {
        state = PERSIST_ERROR;
        return state;
    }

    uint32_t sectors = sectors_for(super.payload_bytes);
    uint8_t *buffer = (uint8_t *)kmalloc(sectors * ATA_SECTOR_SIZE);
    if (!buffer) {
        state = PERSIST_ERROR;
        return state;
    }

    if (ata_read(DATA_DRIVE, PAYLOAD_LBA, sectors, buffer) != 0) {
        kfree(buffer);
        state = PERSIST_ERROR;
        return state;
    }

    if (checksum(buffer, super.payload_bytes) != super.checksum) {
        kfree(buffer);
        state = PERSIST_ERROR;              /* torn or corrupted snapshot */
        return state;
    }

    reader_t r = { buffer, super.payload_bytes, 0, false };

    /* Everything checks out, so the built-in skeleton can go. */
    fs_reset();

    for (uint32_t i = 0; i < super.user_count && !r.bad; i++) {
        char name[USER_NAME_MAX];
        char home[USER_HOME_MAX];

        if (!take_string(&r, name, sizeof(name)))
            break;
        if (!take_string(&r, home, sizeof(home)))
            break;
        uint32_t uid = take_u32(&r);
        uint32_t gid = take_u32(&r);
        uint32_t hash = take_u32(&r);
        if (r.bad)
            break;
        users_import(name, home, uid, gid, hash);
    }

    for (uint32_t i = 0; i < super.node_count && !r.bad; i++) {
        char path[FS_PATH_MAX];
        uint8_t kind = take_u8(&r);
        uint16_t path_len = take_u16(&r);
        uint32_t data_len = take_u32(&r);

        if (r.bad || path_len == 0 || path_len >= FS_PATH_MAX) {
            r.bad = true;
            break;
        }
        if (!take_bytes(&r, path, path_len))
            break;
        path[path_len] = '\0';

        fs_node_t *node = fs_create(path, kind == KIND_DIR ? FS_DIR : FS_FILE);
        if (!node)
            node = fs_resolve(path);

        if (data_len) {
            if (r.pos + data_len > r.length) {
                r.bad = true;
                break;
            }
            if (node && node->type == FS_FILE)
                fs_write_n(node, (const char *)(r.buffer + r.pos), data_len,
                           false);
            r.pos += data_len;
        }
    }

    kfree(buffer);

    if (r.bad) {
        state = PERSIST_ERROR;
        return state;
    }

    bytes_used = super.payload_bytes;
    save_count = super.saves;
    state = PERSIST_LOADED;
    dirty = false;
    return state;
}

int persist_format(void)
{
    super_t super;

    if (state == PERSIST_NO_DISK)
        return -1;

    memset(&super, 0, sizeof(super));
    if (ata_write(DATA_DRIVE, SUPER_LBA, 1, &super) != 0)
        return -2;

    state = PERSIST_EMPTY;
    bytes_used = 0;
    save_count = 0;
    dirty = false;
    sealed = true;          /* nothing may write again before the reboot */
    return 0;
}

bool persist_sealed(void) { return sealed; }
const char *persist_where(void) { return data_where; }

void persist_mark_dirty(void) { dirty = true; }
bool persist_is_dirty(void)   { return dirty; }

int persist_flush(void)
{
    if (!dirty)
        return 0;
    return persist_save();
}

persist_state_t persist_state(void) { return state; }
bool     persist_available(void)        { return state != PERSIST_NO_DISK; }
uint32_t persist_bytes_used(void)       { return bytes_used; }
uint32_t persist_bytes_capacity(void)   { return MAX_PAYLOAD; }
uint32_t persist_saves(void)            { return save_count; }
