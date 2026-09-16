#include "users.h"
#include "console.h"
#include "vga.h"
#include "kprintf.h"
#include "string.h"
#include "fs.h"
#include "timer.h"
#include "version.h"

/* A small account database held entirely in RAM. There is no disk to persist
   it to, so the setup wizard runs on every boot - the point is the mechanism,
   not the storage. */

static user_t users[USER_MAX];
static user_t *current;
static uint32_t next_uid = 1000;

/* FNV-1a with a fixed salt and a few thousand rounds of stirring.
   This is obfuscation, NOT cryptography: a 32-bit digest is far too small to
   resist an offline attack. It exists so that passwords are not kept in the
   clear in memory, and nothing more. */
static uint32_t hash_password(const char *password)
{
    const char *salt = "LightningOS/v2";
    uint32_t hash = 2166136261u;

    for (const char *p = salt; *p; p++) {
        hash ^= (uint8_t)*p;
        hash *= 16777619u;
    }
    for (const char *p = password; *p; p++) {
        hash ^= (uint8_t)*p;
        hash *= 16777619u;
    }
    for (int round = 0; round < 4096; round++) {
        hash ^= hash >> 13;
        hash *= 16777619u;
        hash ^= (uint32_t)round;
    }
    return hash ? hash : 1u;        /* 0 is reserved for "no password set" */
}

static user_t *free_slot(void)
{
    for (int i = 0; i < USER_MAX; i++) {
        if (!users[i].active)
            return &users[i];
    }
    return NULL;
}

/* Keeps /etc/passwd in step with the table so that `cat /etc/passwd` shows
   something truthful. Password hashes deliberately stay out of it. */
static void sync_passwd_file(void)
{
    fs_node_t *file = fs_resolve("/etc/passwd");
    char line[160];
    bool first = true;

    if (!file)
        file = fs_create("/etc/passwd", FS_FILE);
    if (!file)
        return;

    for (int i = 0; i < USER_MAX; i++) {
        if (!users[i].active)
            continue;
        ksnprintf(line, sizeof(line), "%s:x:%u:%u:%s:%s:/bin/lsh\n",
                  users[i].name, users[i].uid, users[i].gid,
                  users[i].name, users[i].home);
        fs_write(file, line, !first);
        first = false;
    }
    if (first)
        fs_write(file, "", false);
}

void users_init(void)
{
    memset(users, 0, sizeof(users));
    current = NULL;
    next_uid = 1000;
}

bool users_setup_needed(void)
{
    return users_count() == 0;
}

int users_count(void)
{
    int count = 0;

    for (int i = 0; i < USER_MAX; i++) {
        if (users[i].active)
            count++;
    }
    return count;
}

user_t *users_at(int index)
{
    int seen = 0;

    for (int i = 0; i < USER_MAX; i++) {
        if (!users[i].active)
            continue;
        if (seen == index)
            return &users[i];
        seen++;
    }
    return NULL;
}

user_t *users_find(const char *name)
{
    if (!name || !*name)
        return NULL;

    for (int i = 0; i < USER_MAX; i++) {
        if (users[i].active && strcmp(users[i].name, name) == 0)
            return &users[i];
    }
    return NULL;
}

user_t *users_current(void) { return current; }

bool users_is_root(void)
{
    return current && current->uid == 0;
}

bool users_check_password(const user_t *user, const char *password)
{
    return user && user->pw_hash == hash_password(password ? password : "");
}

static bool valid_name(const char *name)
{
    if (!name || !*name || strlen(name) >= USER_NAME_MAX)
        return false;

    for (const char *p = name; *p; p++) {
        bool ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '_' || *p == '-';
        if (!ok)
            return false;
    }
    return !(name[0] >= '0' && name[0] <= '9');
}

int users_add(const char *name, const char *password, bool admin)
{
    if (!valid_name(name))
        return -1;
    if (users_find(name))
        return -2;
    if (!password || strlen(password) < PASSWORD_MIN)
        return -3;

    user_t *slot = free_slot();
    if (!slot)
        return -4;

    memset(slot, 0, sizeof(*slot));
    strncpy(slot->name, name, USER_NAME_MAX - 1);

    if (admin) {
        slot->uid = 0;
        slot->gid = 0;
        strcpy(slot->home, "/root");
    } else {
        slot->uid = next_uid;
        slot->gid = next_uid;
        next_uid++;
        ksnprintf(slot->home, USER_HOME_MAX, "/home/%s", name);
    }

    slot->pw_hash = hash_password(password);
    slot->active = true;

    if (!fs_resolve(slot->home)) {
        char path[FS_PATH_MAX];

        fs_create(slot->home, FS_DIR);
        ksnprintf(path, sizeof(path), "%s/readme.txt", slot->home);

        fs_node_t *readme = fs_create(path, FS_FILE);
        fs_write(readme,
                 "This home directory lives in RAM and is gone on reboot.\n"
                 "You may write here and in /tmp; the rest of the tree is\n"
                 "read-only unless you are root.\n"
                 "Try: echo hello > note.txt, then cat note.txt\n", false);
    }
    sync_passwd_file();
    return 0;
}

int users_delete(const char *name)
{
    user_t *user = users_find(name);

    if (!user)
        return -1;
    if (user->uid == 0)
        return -2;                  /* never remove the last administrator */
    if (user == current)
        return -3;

    user->active = false;
    sync_passwd_file();
    return 0;
}

int users_set_password(const char *name, const char *password)
{
    user_t *user = users_find(name);

    if (!user)
        return -1;
    if (!password || strlen(password) < PASSWORD_MIN)
        return -3;

    user->pw_hash = hash_password(password);
    return 0;
}

int users_switch(const char *name, const char *password)
{
    user_t *user = users_find(name);

    if (!user)
        return -1;
    if (!users_check_password(user, password))
        return -2;

    current = user;
    fs_set_cwd(fs_resolve(user->home));
    return 0;
}

void users_logout(void)
{
    current = NULL;
}

bool users_may_write(const char *abspath)
{
    if (users_is_root())
        return true;
    if (!current || !abspath)
        return false;

    size_t home_len = strlen(current->home);

    if (strncmp(abspath, current->home, home_len) == 0 &&
        (abspath[home_len] == '\0' || abspath[home_len] == '/'))
        return true;
    if (strncmp(abspath, "/tmp", 4) == 0 &&
        (abspath[4] == '\0' || abspath[4] == '/'))
        return true;
    return false;
}

/* ------------------------------------------------------------------------- */
/* interactive parts                                                          */
/* ------------------------------------------------------------------------- */

/* Asks for a password twice and only returns once both entries agree. */
static bool ask_new_password(const char *who, char *out, size_t size)
{
    char again[PASSWORD_MAX];

    for (;;) {
        kprintf("  New password for %s: ", who);
        if (!console_read_line(out, size, CONSOLE_ECHO_STARS, false, NULL))
            return false;

        if (strlen(out) < PASSWORD_MIN) {
            vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
            kprintf("  Password must be at least %d characters.\n", PASSWORD_MIN);
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            continue;
        }

        kprintf("  Repeat password:      ");
        if (!console_read_line(again, sizeof(again), CONSOLE_ECHO_STARS, false, NULL))
            return false;

        if (strcmp(out, again) != 0) {
            vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
            kprintf("  Passwords do not match, try again.\n");
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            continue;
        }
        return true;
    }
}

void users_setup_wizard(void)
{
    char password[PASSWORD_MAX];
    char name[USER_NAME_MAX];

    vga_clear();
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    kprintf("  %s %s first boot setup\n", LOS_NAME, LOS_VERSION);
    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    kprintf("  ------------------------------------------------------------\n");
    kprintf("  Accounts live in RAM, so this runs on every boot.\n\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    kprintf("  Step 1 of 2: the administrator account (root).\n\n");
    while (!ask_new_password("root", password, sizeof(password)))
        ;
    users_add("root", password, true);

    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    kprintf("\n  root created.\n\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    kprintf("  Step 2 of 2: an everyday account. Leave the name empty to skip.\n\n");
    for (;;) {
        kprintf("  Username: ");
        if (!console_read_line(name, sizeof(name), CONSOLE_ECHO_PLAIN, false, NULL))
            break;
        if (!name[0])
            break;

        if (!valid_name(name)) {
            vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
            kprintf("  Use letters, digits, '-' and '_', starting with a letter.\n");
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            continue;
        }
        if (users_find(name)) {
            vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
            kprintf("  That name is taken.\n");
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            continue;
        }

        if (!ask_new_password(name, password, sizeof(password)))
            continue;
        if (users_add(name, password, false) == 0) {
            vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
            kprintf("\n  %s created, home directory /home/%s\n", name, name);
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        }
        break;
    }

    memset(password, 0, sizeof(password));

    kprintf("\n  Setup complete. Press Enter to reach the login prompt.\n");
    console_read_line(name, sizeof(name), CONSOLE_ECHO_HIDDEN, false, NULL);
}

void users_login(void)
{
    char name[USER_NAME_MAX];
    char password[PASSWORD_MAX];

    for (;;) {
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        kprintf("\n%s %s (tty1)\n\n", LOS_NAME, LOS_VERSION);
        kprintf("lightning login: ");

        if (!console_read_line(name, sizeof(name), CONSOLE_ECHO_PLAIN, false, NULL))
            continue;
        if (!name[0])
            continue;

        kprintf("Password: ");
        console_read_line(password, sizeof(password), CONSOLE_ECHO_HIDDEN,
                          false, NULL);

        if (users_switch(name, password) == 0) {
            memset(password, 0, sizeof(password));
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            kprintf("\nLast login: now on tty1\n");
            return;
        }

        memset(password, 0, sizeof(password));
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        kprintf("\nLogin incorrect\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        sleep_ms(1500);
    }
}
