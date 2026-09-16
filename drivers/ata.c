#include "ata.h"
#include "io.h"
#include "string.h"

/* Polled ATA PIO on the primary channel. PIO is slow, but it needs no DMA
   setup, no interrupt plumbing and no bus mastering, which makes it the right
   first disk driver for a kernel this size. */

#define ATA_IO_BASE   0x1F0
#define ATA_CTRL_BASE 0x3F6

#define ATA_REG_DATA      (ATA_IO_BASE + 0)
#define ATA_REG_FEATURES  (ATA_IO_BASE + 1)
#define ATA_REG_SECCOUNT  (ATA_IO_BASE + 2)
#define ATA_REG_LBA_LOW   (ATA_IO_BASE + 3)
#define ATA_REG_LBA_MID   (ATA_IO_BASE + 4)
#define ATA_REG_LBA_HIGH  (ATA_IO_BASE + 5)
#define ATA_REG_DRIVE     (ATA_IO_BASE + 6)
#define ATA_REG_STATUS    (ATA_IO_BASE + 7)
#define ATA_REG_COMMAND   (ATA_IO_BASE + 7)

#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DF   0x20
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

#define ATA_CMD_READ_PIO   0x20
#define ATA_CMD_WRITE_PIO  0x30
#define ATA_CMD_CACHE_FLUSH 0xE7
#define ATA_CMD_IDENTIFY   0xEC

#define POLL_LIMIT 1000000

typedef struct {
    bool     present;
    uint32_t sectors;
    char     model[41];
} drive_info_t;

static drive_info_t drives[2];

/* Reading the alternate status register takes ~100ns and has no side effects,
   so four reads are the conventional way to wait out the 400ns the spec
   requires after selecting a drive. */
static void io_delay_400ns(void)
{
    for (int i = 0; i < 4; i++)
        inb(ATA_CTRL_BASE);
}

static void select_drive(ata_drive_t drive, uint32_t lba)
{
    uint8_t value = (uint8_t)((drive == ATA_SLAVE ? 0xF0 : 0xE0) |
                              ((lba >> 24) & 0x0F));
    outb(ATA_REG_DRIVE, value);
    io_delay_400ns();
}

static int wait_not_busy(void)
{
    for (int i = 0; i < POLL_LIMIT; i++) {
        uint8_t status = inb(ATA_REG_STATUS);
        if (!(status & ATA_SR_BSY))
            return 0;
    }
    return -1;
}

static int wait_drq(void)
{
    for (int i = 0; i < POLL_LIMIT; i++) {
        uint8_t status = inb(ATA_REG_STATUS);

        if (status & (ATA_SR_ERR | ATA_SR_DF))
            return -1;
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ))
            return 0;
    }
    return -1;
}

/* The IDENTIFY strings come back as 16-bit words with the bytes swapped. */
static void copy_model(char *dst, const uint16_t *id)
{
    for (int i = 0; i < 20; i++) {
        dst[i * 2]     = (char)(id[27 + i] >> 8);
        dst[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    dst[40] = '\0';

    for (int i = 39; i >= 0 && (dst[i] == ' ' || dst[i] == '\0'); i--)
        dst[i] = '\0';
}

static void identify(ata_drive_t drive)
{
    uint16_t id[256];

    drives[drive].present = false;

    select_drive(drive, 0);
    outb(ATA_REG_SECCOUNT, 0);
    outb(ATA_REG_LBA_LOW, 0);
    outb(ATA_REG_LBA_MID, 0);
    outb(ATA_REG_LBA_HIGH, 0);
    outb(ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    io_delay_400ns();

    if (inb(ATA_REG_STATUS) == 0)
        return;                         /* nothing on this slot */

    if (wait_not_busy() != 0)
        return;

    /* A non-zero signature in the LBA mid/high registers means the device
       speaks ATAPI or SATA rather than plain ATA. */
    if (inb(ATA_REG_LBA_MID) != 0 || inb(ATA_REG_LBA_HIGH) != 0)
        return;

    if (wait_drq() != 0)
        return;

    for (int i = 0; i < 256; i++)
        id[i] = inw(ATA_REG_DATA);

    drives[drive].sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    if (drives[drive].sectors == 0)
        return;

    copy_model(drives[drive].model, id);
    drives[drive].present = true;
}

void ata_init(void)
{
    memset(drives, 0, sizeof(drives));

    outb(ATA_CTRL_BASE, 0x02);          /* nIEN: we poll, no IRQ14 wanted */
    identify(ATA_MASTER);
    identify(ATA_SLAVE);
}

bool ata_present(ata_drive_t drive)
{
    return (drive <= ATA_SLAVE) && drives[drive].present;
}

uint32_t ata_sector_count(ata_drive_t drive)
{
    return ata_present(drive) ? drives[drive].sectors : 0;
}

const char *ata_model(ata_drive_t drive)
{
    return ata_present(drive) ? drives[drive].model : "";
}

/* One command covers at most 255 sectors, so callers of ata_read/ata_write
   get their transfer split here. */
static int transfer_chunk(ata_drive_t drive, uint32_t lba, uint8_t count,
                          void *buffer, bool write)
{
    uint16_t *words = (uint16_t *)buffer;

    if (wait_not_busy() != 0)
        return -1;

    select_drive(drive, lba);
    outb(ATA_REG_FEATURES, 0);
    outb(ATA_REG_SECCOUNT, count);
    outb(ATA_REG_LBA_LOW, (uint8_t)(lba & 0xFF));
    outb(ATA_REG_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_REG_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_REG_COMMAND, write ? ATA_CMD_WRITE_PIO : ATA_CMD_READ_PIO);

    int sectors = (count == 0) ? 256 : count;

    for (int s = 0; s < sectors; s++) {
        if (wait_drq() != 0)
            return -1;

        if (write) {
            for (int i = 0; i < 256; i++)
                outw(ATA_REG_DATA, words[s * 256 + i]);
        } else {
            for (int i = 0; i < 256; i++)
                words[s * 256 + i] = inw(ATA_REG_DATA);
        }
    }

    if (write) {
        if (wait_not_busy() != 0)
            return -1;
        outb(ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
        if (wait_not_busy() != 0)
            return -1;
    }
    return 0;
}

static int transfer(ata_drive_t drive, uint32_t lba, uint32_t count,
                    void *buffer, bool write)
{
    uint8_t *bytes = (uint8_t *)buffer;

    if (!ata_present(drive))
        return -1;
    if (count == 0)
        return 0;
    if (lba + count > drives[drive].sectors)
        return -1;

    while (count > 0) {
        uint32_t chunk = count > 255 ? 255 : count;

        if (transfer_chunk(drive, lba, (uint8_t)chunk, bytes, write) != 0)
            return -1;

        lba += chunk;
        count -= chunk;
        bytes += chunk * ATA_SECTOR_SIZE;
    }
    return 0;
}

int ata_read(ata_drive_t drive, uint32_t lba, uint32_t count, void *buffer)
{
    return transfer(drive, lba, count, buffer, false);
}

int ata_write(ata_drive_t drive, uint32_t lba, uint32_t count, const void *buffer)
{
    return transfer(drive, lba, count, (void *)buffer, true);
}
