#include "install.h"
#include "ata.h"
#include "mem.h"
#include "vga.h"
#include "kprintf.h"
#include "string.h"
#include "console.h"
#include "timer.h"
#include "io.h"
#include "version.h"

/* Installing means writing three things onto a hard disk: the boot sector,
   the kernel image, and a blank data area.

   The boot sector cannot simply be copied from the running system, because
   when we boot off the CD the first sector of the loaded image is the El
   Torito stage, not the hard disk one. So the hard disk boot sector is baked
   into the kernel as a byte array (generated into bootsector.c at build time).

   The kernel image, on the other hand, is already in memory - that is what
   the loader put at 0x10000. Writing it back out is only correct if it has
   not been modified yet, so a pristine copy is taken before anything runs. */

extern const uint8_t los_boot_sector[512];

/* Provided by the linker script. */
extern uint8_t _start[];
extern uint8_t __image_end[];

#define BOOT_FLAG_ADDR 0x00007000
#define BOOT_FLAG_CD   0x4F534943      /* 'CISO', set by boot/cdboot.asm */

static uint8_t *image_copy;
static uint32_t image_bytes;

bool install_booted_from_medium(void)
{
    return *(volatile uint32_t *)BOOT_FLAG_ADDR == BOOT_FLAG_CD;
}

void install_capture_image(void)
{
    image_bytes = (uint32_t)(__image_end - _start);

    image_copy = (uint8_t *)kmalloc(image_bytes);
    if (image_copy)
        memcpy(image_copy, _start, image_bytes);
}

uint32_t install_image_bytes(void) { return image_bytes; }

uint32_t install_image_sectors(void)
{
    return (image_bytes + ATA_SECTOR_SIZE - 1) / ATA_SECTOR_SIZE;
}

bool install_find_target(ata_drive_t *drive)
{
    /* Booted from the CD, every ATA disk we can see is a candidate, and the
       primary master is the conventional place to install. */
    if (ata_present(ATA_MASTER)) {
        *drive = ATA_MASTER;
        return true;
    }
    if (ata_present(ATA_SLAVE)) {
        *drive = ATA_SLAVE;
        return true;
    }
    return false;
}

int install_to_disk(ata_drive_t drive)
{
    uint32_t sectors = install_image_sectors();

    if (!image_copy || image_bytes == 0)
        return -1;
    if (!ata_present(drive))
        return -2;
    if (sectors > INSTALL_KERNEL_MAX)
        return -3;          /* the boot sector would not read it all back */
    if (ata_sector_count(drive) < INSTALL_DATA_LBA + 64)
        return -4;          /* too small to hold kernel plus a data area */

    /* The boot sector. */
    if (ata_write(drive, 0, 1, los_boot_sector) != 0)
        return -5;

    /* The kernel, padded out to a whole sector. */
    uint32_t padded = sectors * ATA_SECTOR_SIZE;
    uint8_t *buffer = (uint8_t *)kmalloc(padded);
    if (!buffer)
        return -6;

    memcpy(buffer, image_copy, image_bytes);
    memset(buffer + image_bytes, 0, padded - image_bytes);

    int result = ata_write(drive, INSTALL_KERNEL_LBA, sectors, buffer);
    kfree(buffer);
    if (result != 0)
        return -7;

    /* A zeroed superblock, so the installed system comes up in first time
       setup rather than trying to read somebody else's leftovers. */
    uint8_t blank[ATA_SECTOR_SIZE];
    memset(blank, 0, sizeof(blank));
    if (ata_write(drive, INSTALL_DATA_LBA, 1, blank) != 0)
        return -8;

    return 0;
}

/* ------------------------------------------------------------------------- */
/* the installer screen                                                      */
/* ------------------------------------------------------------------------- */

static void header(void)
{
    char title[VGA_WIDTH + 1];

    /* Clear on black first: vga_clear() fills with the current attribute, so
       clearing while the title colours are set would paint the whole screen. */
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_clear();

    ksnprintf(title, sizeof(title), "  %s %s installer", LOS_NAME, LOS_RELEASE);
    size_t used = strlen(title);
    for (size_t i = used; i < VGA_WIDTH; i++)
        title[i] = ' ';
    title[VGA_WIDTH] = '\0';

    vga_set_color(VGA_WHITE, VGA_BLUE);
    vga_write(title);
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kprintf("\n");
}

static void describe_disk(ata_drive_t drive)
{
    uint32_t sectors = ata_sector_count(drive);

    kprintf("  %-14s %-22s %u sectors (%u MiB)\n",
            drive == ATA_MASTER ? "primary master" : "primary slave",
            ata_model(drive), sectors, sectors / 2048);
}

static void do_install(void)
{
    ata_drive_t target;
    char answer[8];

    header();

    if (!install_find_target(&target)) {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        kprintf("  No hard disk found.\n\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        kprintf("  Attach one to the primary IDE channel and start again.\n");
        kprintf("\n  Press Enter to go back.\n");
        console_read_line(answer, sizeof(answer), CONSOLE_ECHO_HIDDEN, false, NULL);
        return;
    }

    kprintf("  Target disk\n\n");
    describe_disk(target);

    kprintf("\n  Will be written:\n\n");
    kprintf("    sector %-6u  boot sector (512 bytes)\n", INSTALL_KERNEL_LBA - 1);
    kprintf("    sector %-6u  kernel, %u sectors (%u bytes)\n",
            INSTALL_KERNEL_LBA, install_image_sectors(), install_image_bytes());
    kprintf("    sector %-6u  data area, starts empty\n", INSTALL_DATA_LBA);

    vga_set_color(VGA_YELLOW, VGA_BLACK);
    kprintf("\n  Everything currently on that disk will be lost.\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kprintf("\n  Type YES to install, anything else to cancel: ");

    if (!console_read_line(answer, sizeof(answer), CONSOLE_ECHO_PLAIN, false, NULL) ||
        strcmp(answer, "YES") != 0) {
        kprintf("\n  Cancelled, nothing was written.\n");
        kprintf("\n  Press Enter to go back.\n");
        console_read_line(answer, sizeof(answer), CONSOLE_ECHO_HIDDEN, false, NULL);
        return;
    }

    kprintf("\n  Writing...\n");

    int result = install_to_disk(target);

    if (result == 0) {
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        kprintf("\n  Installation complete.\n\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        kprintf("  Remove the install medium and reboot. The installed system\n");
        kprintf("  will start its own first time setup.\n");
    } else {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        kprintf("\n  Installation failed (code %d).\n", result);
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        if (result == -3)
            kprintf("  The kernel grew past what the boot sector can load.\n");
        else if (result == -4)
            kprintf("  The disk is too small.\n");
        else
            kprintf("  The disk could not be written.\n");
    }

    kprintf("\n  Press Enter to go back to the menu.\n");
    console_read_line(answer, sizeof(answer), CONSOLE_ECHO_HIDDEN, false, NULL);
}

static void reboot_now(void)
{
    uint8_t status;

    kprintf("\n  Rebooting...\n");
    sleep_ms(500);

    do {
        status = inb(0x64);
        if (status & 0x01)
            inb(0x60);
    } while (status & 0x02);
    outb(0x64, 0xFE);

    __asm__ volatile("cli");
    for (;;)
        __asm__ volatile("hlt");
}

void install_run_menu(void)
{
    char choice[8];

    for (;;) {
        header();

        kprintf("  Booted from the install medium.\n\n");

        if (ata_present(ATA_MASTER) || ata_present(ATA_SLAVE)) {
            kprintf("  Disks found:\n\n");
            if (ata_present(ATA_MASTER))
                describe_disk(ATA_MASTER);
            if (ata_present(ATA_SLAVE))
                describe_disk(ATA_SLAVE);
        } else {
            vga_set_color(VGA_YELLOW, VGA_BLACK);
            kprintf("  No hard disk detected.\n");
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        }

        kprintf("\n");
        vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        kprintf("    1");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        kprintf("  Install %s onto a hard disk\n", LOS_NAME);
        vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        kprintf("    2");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        kprintf("  Try it without installing (changes are lost on reboot)\n");
        vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        kprintf("    3");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        kprintf("  Reboot\n");

        kprintf("\n  Choice: ");
        if (!console_read_line(choice, sizeof(choice), CONSOLE_ECHO_PLAIN,
                               false, NULL))
            continue;

        if (strcmp(choice, "1") == 0) {
            do_install();
        } else if (strcmp(choice, "2") == 0) {
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            vga_clear();
            return;                 /* fall through into the live system */
        } else if (strcmp(choice, "3") == 0) {
            reboot_now();
        }
    }
}
