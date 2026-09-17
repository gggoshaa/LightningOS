#include "types.h"
#include "vga.h"
#include "serial.h"
#include "kprintf.h"
#include "gdt.h"
#include "isr.h"
#include "mem.h"
#include "timer.h"
#include "keyboard.h"
#include "mouse.h"
#include "fs.h"
#include "persist.h"
#include "ata.h"
#include "install.h"
#include "users.h"
#include "shell.h"
#include "task.h"
#include "panic.h"
#include "string.h"
#include "io.h"
#include "version.h"

/* The login prompt and the shell, as a task of their own. */
static void session_task(void *arg)
{
    (void)arg;

    for (;;) {
        users_login();
        shell_run();
    }
}

/* Linux-style "[ OK ] doing a thing" progress lines. */
static void step(const char *what)
{
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kprintf("[    ] %s", what);
    vga_set_cursor(vga_row(), 1);
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    kprintf(" OK ");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_set_cursor(vga_row() + 1, 0);
}

void kmain(void)
{
    vga_init();
    serial_init();

    vga_set_color(VGA_WHITE, VGA_BLACK);
    kprintf("%s %s \"%s\" booting on %s\n\n",
            LOS_NAME, LOS_RELEASE, LOS_CODENAME, LOS_ARCH);
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    step("VGA text console 80x25 with 512 lines of scrollback");
    step("Serial console on COM1");

    gdt_init();
    step("Global descriptor table");

    idt_init();
    step("Interrupt descriptor table and PIC remap");

    mem_init();
    step("Physical memory map and kernel heap");

    /* Before anything has had a chance to modify .data, so that what the
       installer writes out is the image as it came off the medium. */
    install_capture_image();
    step("Kernel image captured for the installer");

    timer_init();
    step("Programmable interval timer at 100 Hz");

    keyboard_init();
    step("PS/2 keyboard driver");

    /* The mouse is probed by polling the 8042 while IRQ12 is still masked,
       so it has to happen before interrupts come on. */
    mouse_init();
    if (mouse_present())
        step(mouse_device_id() == 3
                 ? "PS/2 mouse with scroll wheel"
                 : "PS/2 mouse without a wheel");
    else
        step("No PS/2 mouse (PageUp/PageDown still scroll)");

    fs_init();
    step("Root filesystem skeleton");

    users_init();
    step("Account database");

    /* Probing ATA and restoring the snapshot both work by polling, so this
       runs before interrupts are on. A restored snapshot replaces the
       skeleton the two calls above just built. */
    switch (persist_mount()) {
    case PERSIST_LOADED:
        step("Data disk mounted, previous session restored");
        break;
    case PERSIST_EMPTY:
        step("Data disk found, no snapshot on it yet");
        break;
    case PERSIST_ERROR:
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        step("Data disk unreadable, continuing in RAM only");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        break;
    default:
        step("No data disk, changes will not survive a reboot");
        break;
    }

    sti();
    step("Interrupts enabled");

    sleep_ms(500);

    /* Off the install medium the installer comes first. Choosing "try it
       without installing" falls through into an ordinary live session. */
    if (install_booted_from_medium())
        install_run_menu();

    if (users_setup_needed()) {
        users_setup_wizard();
        persist_save();         /* keep the freshly created accounts */
    }

    /* From here on this context is the idle task: it exists so the scheduler
       always has something to run, and it reclaims the stacks of tasks that
       have finished. Everything the user interacts with runs in the session
       task spawned below. */
    task_init();
    if (!task_spawn("session", session_task, NULL))
        panic("could not start the session task");

    for (;;) {
        task_reap();
        __asm__ volatile("sti; hlt");
    }
}
