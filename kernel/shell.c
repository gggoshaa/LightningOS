#include "shell.h"
#include "vga.h"
#include "kprintf.h"
#include "string.h"
#include "console.h"
#include "timer.h"
#include "mem.h"
#include "fs.h"
#include "rtc.h"
#include "io.h"
#include "users.h"
#include "mouse.h"
#include "ata.h"
#include "persist.h"
#include "task.h"
#include "banner.h"
#include "version.h"

#define LINE_MAX 256
#define ARG_MAX  16

static bool session_open;

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

static void current_path(char *out, size_t size)
{
    fs_abspath(fs_cwd(), out, size);
    if (!out[0])
        strncpy(out, "/", size);
}

/* Shows the home directory as ~, the way a login shell would. */
static void display_path(char *out, size_t size)
{
    char path[FS_PATH_MAX];
    user_t *user = users_current();

    current_path(path, sizeof(path));

    if (user) {
        size_t home_len = strlen(user->home);
        if (strncmp(path, user->home, home_len) == 0 &&
            (path[home_len] == '\0' || path[home_len] == '/')) {
            ksnprintf(out, size, "~%s", path + home_len);
            return;
        }
    }
    strncpy(out, path, size - 1);
    out[size - 1] = '\0';
}

static void print_prompt(void)
{
    char shown[FS_PATH_MAX];
    user_t *user = users_current();
    bool root = users_is_root();

    display_path(shown, sizeof(shown));

    vga_set_color(root ? VGA_LIGHT_RED : VGA_LIGHT_GREEN, VGA_BLACK);
    kprintf("%s@lightning", user ? user->name : "nobody");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kprintf(":");
    vga_set_color(VGA_LIGHT_BLUE, VGA_BLACK);
    kprintf("%s", shown);
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kprintf("%s ", root ? "#" : "$");
}

static void print_error(const char *command, const char *message)
{
    vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
    kprintf("%s: %s\n", command, message);
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

/* Shifts rather than divides: without libgcc a 64-bit division would pull in
   __udivdi3, which does not exist in a freestanding kernel. */
static void human_size(uint64_t bytes, char *out, size_t size)
{
    if (bytes >= (1024 * 1024))
        ksnprintf(out, size, "%u MiB", (uint32_t)(bytes >> 20));
    else if (bytes >= 1024)
        ksnprintf(out, size, "%u KiB", (uint32_t)(bytes >> 10));
    else
        ksnprintf(out, size, "%u B", (uint32_t)bytes);
}

/* Absolute path of `path`, whether or not it exists yet. Existing nodes are
   resolved directly; for a new name the parent is resolved instead, so that
   ".." cannot be used to slip past the permission check. */
static bool target_abspath(const char *path, char *out, size_t size)
{
    fs_node_t *node = fs_resolve(path);

    if (node) {
        fs_abspath(node, out, size);
        if (!out[0])
            strncpy(out, "/", size);
        return true;
    }

    char buffer[FS_PATH_MAX];
    strncpy(buffer, path, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';

    char *slash = strrchr(buffer, '/');
    const char *name;
    fs_node_t *parent;

    if (slash) {
        *slash = '\0';
        name = slash + 1;
        parent = fs_resolve(buffer[0] ? buffer : "/");
    } else {
        name = buffer;
        parent = fs_cwd();
    }

    if (!parent || parent->type != FS_DIR || !*name)
        return false;

    char parent_path[FS_PATH_MAX];
    fs_abspath(parent, parent_path, sizeof(parent_path));
    if (!parent_path[0])
        strcpy(parent_path, "/");

    ksnprintf(out, size, "%s%s%s", parent_path,
              strcmp(parent_path, "/") == 0 ? "" : "/", name);
    return true;
}

static bool may_write(const char *command, const char *path)
{
    char abspath[FS_PATH_MAX];

    if (!target_abspath(path, abspath, sizeof(abspath))) {
        print_error(command, "no such file or directory");
        return false;
    }
    if (users_may_write(abspath))
        return true;

    print_error(command, "permission denied");
    return false;
}

static bool require_root(const char *command)
{
    if (users_is_root())
        return true;
    print_error(command, "only root may do that");
    return false;
}

static int parse_int(const char *s)
{
    int value = 0;

    for (; *s >= '0' && *s <= '9'; s++)
        value = value * 10 + (*s - '0');
    return value;
}

/* ------------------------------------------------------------------------- */
/* commands                                                                  */
/* ------------------------------------------------------------------------- */

void shell_banner(void)
{
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    for (int i = 0; los_logo[i]; i++)
        kprintf("  %s\n", los_logo[i]);
    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    kprintf("  %s %s \"%s\" on %s\n\n",
            LOS_NAME, LOS_VERSION, LOS_CODENAME, LOS_ARCH);
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

static void cmd_help(void)
{
    static const struct { const char *name, *help; } general[] = {
        { "help",      "show this list" },
        { "uname -a",  "kernel name, version and build date" },
        { "banner",    "print the logo" },
        { "clear",     "clear the screen" },
        { "history",   "recently entered commands" },
        { "color f b", "set foreground/background colour (0-15)" },
        { "free",      "heap and RAM usage" },
        { "meminfo",   "BIOS memory map" },
        { "lsdev",     "drivers and their state" },
        { "df",        "disks and snapshot usage" },
        { "sync",      "write the filesystem to disk now" },
        { "ps",        "running tasks and their CPU time" },
        { "spawn k s", "background task: worker|ticker, for s seconds" },
        { "kill pid",  "terminate a task" },
        { "uptime",    "time since boot" },
        { "date",      "read the CMOS real time clock" },
        { "sleep ms",  "busy wait on the timer" },
    };
    static const struct { const char *name, *help; } files[] = {
        { "ls [path]", "list a directory" },
        { "tree",      "show the directory tree" },
        { "cd <dir>",  "change the working directory" },
        { "pwd",       "print the working directory" },
        { "cat <f>",   "print a file" },
        { "echo ...",  "print text, supports > and >> redirection" },
        { "mkdir <d>", "create a directory" },
        { "touch <f>", "create an empty file" },
        { "rm <path>", "remove a file or directory" },
    };
    static const struct { const char *name, *help; } accounts[] = {
        { "whoami",    "print the current user" },
        { "id",        "print uid and gid" },
        { "users",     "list accounts" },
        { "su [user]", "switch user (default root)" },
        { "passwd [u]","change a password" },
        { "useradd u", "create an account (root only)" },
        { "userdel u", "remove an account (root only)" },
        { "mkfs",      "wipe the data disk and reboot (root only)" },
        { "logout",    "end the session, back to login" },
        { "reboot",    "restart the machine" },
        { "shutdown",  "power the machine off" },
    };

    kprintf("LightningOS shell (lsh). Built-in commands:\n");

    vga_set_color(VGA_YELLOW, VGA_BLACK);
    kprintf("\n system\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    for (size_t i = 0; i < sizeof(general) / sizeof(general[0]); i++) {
        vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        kprintf("  %-11s", general[i].name);
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        kprintf(" %s\n", general[i].help);
    }

    vga_set_color(VGA_YELLOW, VGA_BLACK);
    kprintf("\n files\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        kprintf("  %-11s", files[i].name);
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        kprintf(" %s\n", files[i].help);
    }

    vga_set_color(VGA_YELLOW, VGA_BLACK);
    kprintf("\n accounts and power\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    for (size_t i = 0; i < sizeof(accounts) / sizeof(accounts[0]); i++) {
        vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        kprintf("  %-11s", accounts[i].name);
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        kprintf(" %s\n", accounts[i].help);
    }

    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    kprintf("\n Scroll back with the mouse wheel or PageUp/PageDown.\n");
    kprintf(" Ctrl-L clears, Ctrl-C cancels a line, Ctrl-U erases it,\n");
    kprintf(" arrows browse the command history.\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

static void cmd_ls(int argc, char **argv)
{
    fs_node_t *dir = (argc > 1) ? fs_resolve(argv[1]) : fs_cwd();

    if (!dir) {
        print_error("ls", "no such file or directory");
        return;
    }

    if (dir->type == FS_FILE) {
        kprintf("%s\n", dir->name);
        return;
    }

    int files = 0, dirs = 0;
    for (fs_node_t *child = dir->children; child; child = child->next) {
        if (child->type == FS_DIR) {
            vga_set_color(VGA_LIGHT_BLUE, VGA_BLACK);
            kprintf("drwxr-xr-x  %6s  %s/\n", "-", child->name);
            dirs++;
        } else {
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            kprintf("-rw-r--r--  %6u  %s\n", (uint32_t)child->size, child->name);
            files++;
        }
    }
    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    kprintf("%d directories, %d files\n", dirs, files);
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

static void tree_walk(fs_node_t *dir, int depth)
{
    for (fs_node_t *child = dir->children; child; child = child->next) {
        for (int i = 0; i < depth; i++)
            kprintf("  ");
        if (child->type == FS_DIR) {
            vga_set_color(VGA_LIGHT_BLUE, VGA_BLACK);
            kprintf("+- %s/\n", child->name);
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            tree_walk(child, depth + 1);
        } else {
            kprintf("+- %s\n", child->name);
        }
    }
}

static void cmd_tree(int argc, char **argv)
{
    fs_node_t *dir = (argc > 1) ? fs_resolve(argv[1]) : fs_cwd();

    if (!dir || dir->type != FS_DIR) {
        print_error("tree", "not a directory");
        return;
    }
    kprintf(".\n");
    tree_walk(dir, 0);
    kprintf("\n%d nodes in the filesystem\n", fs_count_nodes());
}

static void cmd_cd(int argc, char **argv)
{
    user_t *user = users_current();
    fs_node_t *dir;

    if (argc < 2) {
        dir = user ? fs_resolve(user->home) : fs_root();
        fs_set_cwd(dir ? dir : fs_root());
        return;
    }

    dir = fs_resolve(argv[1]);
    if (!dir) {
        print_error("cd", "no such file or directory");
        return;
    }
    if (dir->type != FS_DIR) {
        print_error("cd", "not a directory");
        return;
    }
    fs_set_cwd(dir);
}

static void cmd_cat(int argc, char **argv)
{
    if (argc < 2) {
        print_error("cat", "usage: cat <file>");
        return;
    }

    fs_node_t *file = fs_resolve(argv[1]);
    if (!file) {
        print_error("cat", "no such file or directory");
        return;
    }
    if (file->type != FS_FILE) {
        print_error("cat", "is a directory");
        return;
    }
    if (file->data)
        vga_write(file->data);
    if (file->size && file->data[file->size - 1] != '\n')
        vga_putc('\n');
}

/* echo keeps the raw argument string so spacing survives, and handles
   `> file` / `>> file` redirection at the end of the line. */
static void cmd_echo(char *args)
{
    bool append = false;
    char *target = NULL;
    char *redirect = NULL;

    for (char *p = args; *p; p++) {
        if (*p == '>') {
            redirect = p;
            if (p[1] == '>') {
                append = true;
                target = p + 2;
            } else {
                target = p + 1;
            }
            break;
        }
    }

    if (!redirect) {
        kprintf("%s\n", args);
        return;
    }

    *redirect = '\0';
    while (*target == ' ')
        target++;
    for (char *p = redirect - 1; p >= args && *p == ' '; p--)
        *p = '\0';
    for (char *p = target + strlen(target) - 1; p >= target && *p == ' '; p--)
        *p = '\0';

    if (!*target) {
        print_error("echo", "missing redirection target");
        return;
    }
    if (!may_write("echo", target))
        return;

    fs_node_t *file = fs_resolve(target);
    if (!file)
        file = fs_create(target, FS_FILE);
    if (!file) {
        print_error("echo", "cannot create file");
        return;
    }
    if (file->type != FS_FILE) {
        print_error("echo", "is a directory");
        return;
    }

    char buffer[LINE_MAX + 2];
    ksnprintf(buffer, sizeof(buffer), "%s\n", args);
    if (fs_write(file, buffer, append) != 0)
        print_error("echo", "write failed");
    else
        persist_mark_dirty();
}

static void cmd_free(void)
{
    char total[24], used[24], avail[24], ram[24];

    human_size(mem_total_bytes(), ram, sizeof(ram));
    human_size(mem_heap_size(), total, sizeof(total));
    human_size(mem_heap_used(), used, sizeof(used));
    human_size(mem_heap_free(), avail, sizeof(avail));

    kprintf("Physical RAM reported by BIOS : %s\n", ram);
    kprintf("Kernel heap                   : %s\n", total);
    kprintf("  used                        : %s\n", used);
    kprintf("  free                        : %s\n", avail);
}

static void cmd_meminfo(void)
{
    int count = mem_region_count();

    if (count == 0) {
        kprintf("no BIOS memory map available\n");
        return;
    }

    kprintf("%-20s %-20s %s\n", "BASE", "LENGTH", "TYPE");
    for (int i = 0; i < count; i++) {
        uint64_t base = 0, len = 0;
        uint32_t type = 0;
        char length_text[24];

        mem_region_info(i, &base, &len, &type);
        human_size(len, length_text, sizeof(length_text));

        const char *name;
        switch (type) {
        case 1:  name = "usable";    break;
        case 2:  name = "reserved";  break;
        case 3:  name = "ACPI data"; break;
        case 4:  name = "ACPI NVS";  break;
        case 5:  name = "bad";       break;
        default: name = "unknown";   break;
        }

        kprintf("0x%08x%08x   %-20s %s\n",
                (uint32_t)(base >> 32), (uint32_t)base, length_text, name);
    }
}

static void cmd_lsdev(void)
{
    kprintf("%-12s %-22s %s\n", "DEVICE", "DRIVER", "STATE");
    kprintf("%-12s %-22s %s\n", "console", "VGA text 80x25",
            "active");
    kprintf("%-12s %-22s %d lines of scrollback\n", "scrollback", "vga",
            vga_scrollback_lines());
    kprintf("%-12s %-22s %s\n", "keyboard", "PS/2 set 1, IRQ1", "active");

    if (!mouse_present()) {
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        kprintf("%-12s %-22s %s\n", "mouse", "PS/2, IRQ12", "not detected");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    } else if (mouse_device_id() == 3) {
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        kprintf("%-12s %-22s %s\n", "mouse", "IntelliMouse, IRQ12",
                "wheel scrolling enabled");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    } else {
        kprintf("%-12s %-22s id=%d, no wheel\n", "mouse", "PS/2, IRQ12",
                mouse_device_id());
    }

    kprintf("%-12s %-22s %d Hz\n", "timer", "8253 PIT, IRQ0", TIMER_HZ);
    kprintf("%-12s %-22s %s\n", "rtc", "MC146818 CMOS", "read only");
    kprintf("%-12s %-22s %s\n", "serial", "16550 COM1 38400", "kernel log");
}

static void cmd_sync(void)
{
    if (!persist_available()) {
        print_error("sync", "no data disk attached, nothing to write to");
        return;
    }

    int result = persist_save();
    if (result == 0)
        kprintf("filesystem and accounts written to disk (%u bytes)\n",
                persist_bytes_used());
    else if (result == -2)
        print_error("sync", "snapshot is larger than the reserved area");
    else if (result == -5)
        print_error("sync", "the disk was wiped by mkfs, reboot first");
    else
        print_error("sync", "write failed");
}

static void cmd_df(void)
{
    const char *state;

    switch (persist_state()) {
    case PERSIST_LOADED: state = "mounted";                 break;
    case PERSIST_EMPTY:  state = "empty, never written";    break;
    case PERSIST_ERROR:  state = "unreadable";              break;
    default:             state = "no data disk";            break;
    }

    kprintf("Boot disk    : %s\n",
            ata_present(ATA_MASTER) ? ata_model(ATA_MASTER) : "none");
    if (ata_present(ATA_MASTER))
        kprintf("               %u sectors (%u MiB)\n",
                ata_sector_count(ATA_MASTER),
                ata_sector_count(ATA_MASTER) / 2048);

    kprintf("Data disk    : %s\n",
            ata_present(ATA_SLAVE) ? ata_model(ATA_SLAVE) : "none");
    if (ata_present(ATA_SLAVE))
        kprintf("               %u sectors (%u MiB)\n",
                ata_sector_count(ATA_SLAVE),
                ata_sector_count(ATA_SLAVE) / 2048);

    kprintf("Snapshot     : %s\n", state);
    kprintf("Location     : %s\n", persist_where());
    kprintf("Used         : %u of %u bytes reserved\n",
            persist_bytes_used(), persist_bytes_capacity());
    kprintf("Saves        : %u\n", persist_saves());
    kprintf("Unsaved      : %s\n", persist_is_dirty() ? "yes" : "no");
}

/* ------------------------------------------------------------------------- */
/* tasks                                                                     */
/* ------------------------------------------------------------------------- */

static void cmd_ps(void)
{
    /* 32-bit arithmetic throughout: a 64-bit division would need __udivdi3,
       which a freestanding kernel does not link against. */
    uint32_t total = (uint32_t)timer_ticks();

    kprintf("%4s %-14s %-9s %8s %7s %8s\n",
            "PID", "NAME", "STATE", "TICKS", "CPU%", "SWITCHES");

    for (int i = 0; i < task_count(); i++) {
        task_t *task = task_at(i);
        if (!task)
            continue;

        uint32_t ticks = (uint32_t)task->cpu_ticks;
        uint32_t percent = total ? (ticks / (total / 100 + 1)) : 0;

        if (task == task_current())
            vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        else if (task->state == TASK_SLEEPING)
            vga_set_color(VGA_DARK_GREY, VGA_BLACK);

        kprintf("%4u %-14s %-9s %8u %6u%% %8u\n",
                task->id, task->name, task_state_name(task->state),
                ticks, percent, (uint32_t)task->switches);
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    }

    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    kprintf("%d tasks, %u ticks since boot, preemptive round robin at %d Hz\n",
            task_count(), (uint32_t)total, TIMER_HZ);
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

/* A deliberately CPU-bound task that burns for a fixed number of seconds.
   Running a few of these and watching the shell stay responsive is the whole
   point of the demonstration. Measuring in seconds rather than iterations
   keeps the demo the same length whatever the host CPU does. */
static void worker_task(void *arg)
{
    volatile uint32_t sink = 0;
    uint32_t seconds = (uint32_t)(uintptr_t)arg;
    uint64_t deadline = timer_ticks() + (uint64_t)seconds * TIMER_HZ;

    while (timer_ticks() < deadline) {
        for (uint32_t i = 0; i < 100000; i++)
            sink += i;
    }
}

/* A task that spends its life asleep, to show that a sleeping task costs the
   scheduler nothing at all. */
static void ticker_task(void *arg)
{
    uint32_t seconds = (uint32_t)(uintptr_t)arg;

    for (uint32_t i = 0; i < seconds; i++)
        task_sleep_ms(1000);
}

static void cmd_spawn(int argc, char **argv)
{
    const char *kind = (argc > 1) ? argv[1] : "worker";
    int seconds = (argc > 2) ? parse_int(argv[2]) : 15;
    task_entry_t entry;

    if (seconds <= 0)
        seconds = 15;

    if (strcmp(kind, "worker") == 0) {
        entry = worker_task;
    } else if (strcmp(kind, "ticker") == 0) {
        entry = ticker_task;
    } else {
        print_error("spawn", "usage: spawn <worker|ticker> [seconds]");
        return;
    }

    task_t *task = task_spawn(kind, entry, (void *)(uintptr_t)seconds);
    if (!task) {
        print_error("spawn", "no free task slot or no memory for a stack");
        return;
    }
    kprintf("started %s as pid %u, running for %d seconds\n",
            kind, task->id, seconds);
}

static void cmd_kill(int argc, char **argv)
{
    if (argc < 2) {
        print_error("kill", "usage: kill <pid>");
        return;
    }

    uint32_t pid = (uint32_t)parse_int(argv[1]);

    switch (task_kill(pid)) {
    case 0:
        kprintf("pid %u terminated\n", pid);
        break;
    case -2:
        print_error("kill", "the idle task cannot be killed");
        break;
    default:
        print_error("kill", "no such task");
        break;
    }
}

static void cmd_uptime(void)
{
    uint32_t ticks = (uint32_t)timer_ticks();   /* 32 bits is good for 497 days */
    uint32_t seconds = ticks / TIMER_HZ;

    kprintf("up %u:%02u:%02u (%u ticks at %d Hz)\n",
            seconds / 3600, (seconds / 60) % 60, seconds % 60,
            ticks, TIMER_HZ);
}

static void cmd_date(void)
{
    rtc_time_t now;

    rtc_read(&now);
    kprintf("%04u-%02u-%02u %02u:%02u:%02u UTC\n",
            now.year, now.month, now.day, now.hour, now.minute, now.second);
}

static void cmd_uname(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "-a") == 0)
        kprintf("%s %s (%s) %s %s\n",
                LOS_NAME, LOS_VERSION, LOS_CODENAME, LOS_ARCH, LOS_BUILD);
    else
        kprintf("%s\n", LOS_NAME);
}

static void cmd_color(int argc, char **argv)
{
    if (argc < 3) {
        print_error("color", "usage: color <fg 0-15> <bg 0-15>");
        return;
    }

    int fg = parse_int(argv[1]);
    int bg = parse_int(argv[2]);

    if (fg < 0 || fg > 15 || bg < 0 || bg > 15) {
        print_error("color", "values must be between 0 and 15");
        return;
    }
    vga_set_color((uint8_t)fg, (uint8_t)bg);
    kprintf("colour set to fg=%d bg=%d\n", fg, bg);
}

static void cmd_history(void)
{
    int total = console_history_count();
    int start = total > 16 ? total - 16 : 0;

    for (int i = start; i < total; i++) {
        const char *entry = console_history_at(i);
        if (entry)
            kprintf("%4d  %s\n", i + 1, entry);
    }
}

/* ------------------------------------------------------------------------- */
/* accounts                                                                  */
/* ------------------------------------------------------------------------- */

static void cmd_users(void)
{
    kprintf("%-16s %6s %6s  %s\n", "USER", "UID", "GID", "HOME");
    for (int i = 0; i < users_count(); i++) {
        user_t *user = users_at(i);
        if (!user)
            continue;
        if (user == users_current())
            vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        kprintf("%-16s %6u %6u  %s%s\n", user->name, user->uid, user->gid,
                user->home, user == users_current() ? "   (you)" : "");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    }
}

/* Asks twice and only returns true when both entries match. */
static bool read_new_password(const char *who, char *out, size_t size)
{
    char again[PASSWORD_MAX];

    kprintf("New password for %s: ", who);
    if (!console_read_line(out, size, CONSOLE_ECHO_STARS, false, NULL))
        return false;
    if (strlen(out) < PASSWORD_MIN) {
        print_error("passwd", "password is too short");
        return false;
    }

    kprintf("Retype new password: ");
    if (!console_read_line(again, sizeof(again), CONSOLE_ECHO_STARS, false, NULL))
        return false;

    if (strcmp(out, again) != 0) {
        print_error("passwd", "passwords do not match");
        return false;
    }
    return true;
}

static void cmd_useradd(int argc, char **argv)
{
    char password[PASSWORD_MAX];

    if (!require_root("useradd"))
        return;
    if (argc < 2) {
        print_error("useradd", "usage: useradd <name>");
        return;
    }
    if (users_count() >= USER_MAX) {
        print_error("useradd", "the account table is full");
        return;
    }
    if (!read_new_password(argv[1], password, sizeof(password)))
        return;

    switch (users_add(argv[1], password, false)) {
    case 0:
        kprintf("created %s with home /home/%s\n", argv[1], argv[1]);
        persist_mark_dirty();
        break;
    case -1:
        print_error("useradd", "invalid user name");
        break;
    case -2:
        print_error("useradd", "that name is taken");
        break;
    case -3:
        print_error("useradd", "password is too short");
        break;
    default:
        print_error("useradd", "cannot create the account");
        break;
    }
    memset(password, 0, sizeof(password));
}

static void cmd_userdel(int argc, char **argv)
{
    if (!require_root("userdel"))
        return;
    if (argc < 2) {
        print_error("userdel", "usage: userdel <name>");
        return;
    }

    switch (users_delete(argv[1])) {
    case 0:
        kprintf("removed %s (the home directory is left in place)\n", argv[1]);
        persist_mark_dirty();
        break;
    case -1:
        print_error("userdel", "no such user");
        break;
    case -2:
        print_error("userdel", "root cannot be removed");
        break;
    case -3:
        print_error("userdel", "that account is logged in");
        break;
    default:
        print_error("userdel", "cannot remove the account");
        break;
    }
}

static void cmd_passwd(int argc, char **argv)
{
    char password[PASSWORD_MAX];
    char check[PASSWORD_MAX];
    user_t *me = users_current();
    const char *name = (argc > 1) ? argv[1] : (me ? me->name : "");

    if (!users_find(name)) {
        print_error("passwd", "no such user");
        return;
    }

    /* Changing somebody else's password is an administrator's job; changing
       your own requires proving you know the current one. */
    if (me && strcmp(name, me->name) != 0) {
        if (!require_root("passwd"))
            return;
    } else {
        kprintf("Current password: ");
        console_read_line(check, sizeof(check), CONSOLE_ECHO_HIDDEN, false, NULL);
        bool ok = users_check_password(me, check);
        memset(check, 0, sizeof(check));
        if (!ok) {
            print_error("passwd", "authentication failure");
            return;
        }
    }

    if (read_new_password(name, password, sizeof(password))) {
        if (users_set_password(name, password) == 0) {
            kprintf("password updated for %s\n", name);
            persist_mark_dirty();
        } else
            print_error("passwd", "cannot change the password");
    }
    memset(password, 0, sizeof(password));
}

static void cmd_su(int argc, char **argv)
{
    char password[PASSWORD_MAX];
    const char *name = (argc > 1) ? argv[1] : "root";

    if (!users_find(name)) {
        print_error("su", "no such user");
        return;
    }

    kprintf("Password: ");
    console_read_line(password, sizeof(password), CONSOLE_ECHO_HIDDEN, false, NULL);

    int result = users_switch(name, password);
    memset(password, 0, sizeof(password));

    if (result == 0)
        kprintf("now running as %s\n", name);
    else
        print_error("su", "authentication failure");
}

/* ------------------------------------------------------------------------- */
/* power                                                                     */
/* ------------------------------------------------------------------------- */

/* Restarts the machine without touching the disk. Never returns. */
static void machine_reboot(void)
{
    sleep_ms(400);

    /* Pulse the CPU reset line through the 8042 keyboard controller. */
    uint8_t status;
    do {
        status = inb(0x64);
        if (status & 0x01)
            inb(0x60);
    } while (status & 0x02);
    outb(0x64, 0xFE);

    cli();
    for (;;)
        hlt();
}

static void cmd_reboot(void)
{
    if (persist_is_dirty()) {
        kprintf("Writing unsaved changes to disk...\n");
        persist_save();
    }
    kprintf("Rebooting...\n");
    machine_reboot();
}

/* Wiping the disk leaves the old tree and accounts still live in RAM, so
   staying in the session would only invite the next save to write them back.
   The machine therefore restarts straight away, into first time setup. */
static void cmd_mkfs(void)
{
    if (!require_root("mkfs"))
        return;

    if (!persist_available()) {
        print_error("mkfs", "no data disk attached");
        return;
    }

    vga_set_color(VGA_YELLOW, VGA_BLACK);
    kprintf("This erases every file and account on the data disk.\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kprintf("Type YES to confirm: ");

    char answer[8];
    if (!console_read_line(answer, sizeof(answer), CONSOLE_ECHO_PLAIN, false,
                           NULL) || strcmp(answer, "YES") != 0) {
        kprintf("mkfs cancelled, nothing was written.\n");
        return;
    }

    if (persist_format() != 0) {
        print_error("mkfs", "the data disk could not be wiped");
        return;
    }

    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    kprintf("Data disk wiped. Restarting into first time setup...\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    machine_reboot();
}

static void cmd_shutdown(void)
{
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kprintf("Stopping the shell session...\n");
    if (persist_sealed()) {
        kprintf("Data disk was wiped by mkfs, leaving it empty.\n");
    } else if (persist_available()) {
        kprintf("Writing the filesystem to disk...\n");
        if (persist_save() != 0)
            print_error("shutdown", "the snapshot could not be written");
    } else {
        kprintf("No data disk, nothing to write.\n");
    }
    kprintf("Powering off.\n");
    sleep_ms(600);

    /* Each hypervisor watches a different port for the ACPI sleep command. */
    outw(0x604, 0x2000);        /* QEMU and most modern firmware */
    outw(0xB004, 0x2000);       /* Bochs                         */
    outw(0x4004, 0x3400);       /* VirtualBox                    */

    vga_set_color(VGA_YELLOW, VGA_BLACK);
    kprintf("\nACPI power off is not available on this machine.\n");
    kprintf("It is now safe to close the window.\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    cli();
    for (;;)
        hlt();
}

/* ------------------------------------------------------------------------- */
/* dispatch                                                                  */
/* ------------------------------------------------------------------------- */

static void execute(char *line)
{
    char raw[LINE_MAX];
    char *argv[ARG_MAX];
    int argc;

    strncpy(raw, line, LINE_MAX - 1);
    raw[LINE_MAX - 1] = '\0';

    argc = str_split(line, argv, ARG_MAX);
    if (argc == 0)
        return;

    const char *name = argv[0];

    if (strcmp(name, "help") == 0 || strcmp(name, "?") == 0) {
        cmd_help();
    } else if (strcmp(name, "ls") == 0 || strcmp(name, "dir") == 0) {
        cmd_ls(argc, argv);
    } else if (strcmp(name, "tree") == 0) {
        cmd_tree(argc, argv);
    } else if (strcmp(name, "cd") == 0) {
        cmd_cd(argc, argv);
    } else if (strcmp(name, "pwd") == 0) {
        char path[FS_PATH_MAX];
        current_path(path, sizeof(path));
        kprintf("%s\n", path);
    } else if (strcmp(name, "cat") == 0) {
        cmd_cat(argc, argv);
    } else if (strcmp(name, "echo") == 0) {
        char *args = raw + strlen("echo");
        while (*args == ' ')
            args++;
        cmd_echo(args);
    } else if (strcmp(name, "mkdir") == 0) {
        if (argc < 2)
            print_error("mkdir", "usage: mkdir <directory>");
        else if (may_write("mkdir", argv[1])) {
            if (fs_create(argv[1], FS_DIR))
                persist_mark_dirty();
            else
                print_error("mkdir", "cannot create directory");
        }
    } else if (strcmp(name, "touch") == 0) {
        if (argc < 2)
            print_error("touch", "usage: touch <file>");
        else if (!fs_resolve(argv[1]) && may_write("touch", argv[1])) {
            if (fs_create(argv[1], FS_FILE))
                persist_mark_dirty();
            else
                print_error("touch", "cannot create file");
        }
    } else if (strcmp(name, "rm") == 0) {
        if (argc < 2) {
            print_error("rm", "usage: rm <path>");
        } else if (may_write("rm", argv[1])) {
            int result = fs_remove(argv[1]);
            if (result == 0)
                persist_mark_dirty();
            else if (result == -2)
                print_error("rm", "cannot remove the current directory");
            else
                print_error("rm", "no such file or directory");
        }
    } else if (strcmp(name, "clear") == 0) {
        vga_clear();
    } else if (strcmp(name, "free") == 0) {
        cmd_free();
    } else if (strcmp(name, "meminfo") == 0) {
        cmd_meminfo();
    } else if (strcmp(name, "lsdev") == 0) {
        cmd_lsdev();
    } else if (strcmp(name, "sync") == 0) {
        cmd_sync();
    } else if (strcmp(name, "df") == 0) {
        cmd_df();
    } else if (strcmp(name, "ps") == 0) {
        cmd_ps();
    } else if (strcmp(name, "spawn") == 0) {
        cmd_spawn(argc, argv);
    } else if (strcmp(name, "kill") == 0) {
        cmd_kill(argc, argv);
    } else if (strcmp(name, "yield") == 0) {
        task_yield();
    } else if (strcmp(name, "mkfs") == 0) {
        cmd_mkfs();
    } else if (strcmp(name, "uptime") == 0) {
        cmd_uptime();
    } else if (strcmp(name, "date") == 0) {
        cmd_date();
    } else if (strcmp(name, "uname") == 0) {
        cmd_uname(argc, argv);
    } else if (strcmp(name, "whoami") == 0) {
        user_t *me = users_current();
        kprintf("%s\n", me ? me->name : "nobody");
    } else if (strcmp(name, "id") == 0) {
        user_t *me = users_current();
        if (me)
            kprintf("uid=%u(%s) gid=%u(%s) home=%s\n",
                    me->uid, me->name, me->gid, me->name, me->home);
    } else if (strcmp(name, "users") == 0 || strcmp(name, "who") == 0) {
        cmd_users();
    } else if (strcmp(name, "useradd") == 0) {
        cmd_useradd(argc, argv);
    } else if (strcmp(name, "userdel") == 0) {
        cmd_userdel(argc, argv);
    } else if (strcmp(name, "passwd") == 0) {
        cmd_passwd(argc, argv);
    } else if (strcmp(name, "su") == 0) {
        cmd_su(argc, argv);
    } else if (strcmp(name, "hostname") == 0) {
        kprintf("lightning\n");
    } else if (strcmp(name, "color") == 0) {
        cmd_color(argc, argv);
    } else if (strcmp(name, "history") == 0) {
        cmd_history();
    } else if (strcmp(name, "banner") == 0) {
        shell_banner();
    } else if (strcmp(name, "sleep") == 0) {
        sleep_ms(argc > 1 ? (uint32_t)parse_int(argv[1]) : 0);
    } else if (strcmp(name, "reboot") == 0) {
        cmd_reboot();
    } else if (strcmp(name, "shutdown") == 0 || strcmp(name, "poweroff") == 0 ||
               strcmp(name, "halt") == 0) {
        cmd_shutdown();
    } else if (strcmp(name, "logout") == 0 || strcmp(name, "exit") == 0) {
        session_open = false;
    } else {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        kprintf("lsh: command not found: %s\n", name);
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        kprintf("Type 'help' for the list of built-in commands.\n");
    }
}

void shell_run(void)
{
    char line[LINE_MAX];
    user_t *me = users_current();

    if (me) {
        fs_node_t *home = fs_resolve(me->home);
        fs_set_cwd(home ? home : fs_root());
    }

    vga_clear();
    shell_banner();

    fs_node_t *motd = fs_resolve("/etc/motd");
    if (motd && motd->data)
        vga_write(motd->data);
    if (!mouse_present())
        kprintf("No PS/2 mouse detected - use PageUp/PageDown to scroll.\n");
    kprintf("\n");

    session_open = true;
    while (session_open) {
        print_prompt();
        if (!console_read_line(line, sizeof(line), CONSOLE_ECHO_PLAIN, true,
                               print_prompt))
            continue;
        if (!line[0])
            continue;
        console_history_push(line);
        execute(line);

        /* Anything that changed the tree or the accounts is written out
           straight away, so pulling the plug loses at most nothing. */
        if (persist_flush() != 0)
            print_error("sync", "could not write changes to disk");
    }

    users_logout();
    kprintf("logout\n");
}
