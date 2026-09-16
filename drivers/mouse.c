#include "mouse.h"
#include "vga.h"
#include "isr.h"
#include "io.h"

/* PS/2 mouse hanging off the second port of the 8042 controller. The only
   thing the kernel currently does with it is scroll the console, so the X/Y
   deltas are decoded but discarded. */

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_COMMAND 0x64

#define STATUS_OUTPUT_FULL 0x01
#define STATUS_INPUT_FULL  0x02

#define LINES_PER_NOTCH 3

static uint8_t packet[4];
static int     packet_index;
static int     packet_size = 3;
static int     device_id;
static bool    present;

/* The controller is slow; give up rather than hang forever if no mouse is
   attached and the status bits never settle. */
static bool wait_writable(void)
{
    for (int i = 0; i < 100000; i++) {
        if (!(inb(PS2_STATUS) & STATUS_INPUT_FULL))
            return true;
    }
    return false;
}

static bool wait_readable(void)
{
    for (int i = 0; i < 100000; i++) {
        if (inb(PS2_STATUS) & STATUS_OUTPUT_FULL)
            return true;
    }
    return false;
}

static int read_data(void)
{
    if (!wait_readable())
        return -1;
    return inb(PS2_DATA);
}

/* Commands for the mouse itself have to be prefixed with 0xD4, otherwise the
   controller would interpret them as its own. */
static int mouse_command(uint8_t value)
{
    if (!wait_writable())
        return -1;
    outb(PS2_COMMAND, 0xD4);
    if (!wait_writable())
        return -1;
    outb(PS2_DATA, value);
    return read_data();             /* 0xFA = ACK */
}

static int set_sample_rate(uint8_t rate)
{
    if (mouse_command(0xF3) != 0xFA)
        return -1;
    return mouse_command(rate);
}

/* The "magic knock": three sample rates in this exact order make an
   IntelliMouse switch from 3-byte to 4-byte packets with a Z axis. */
static void enable_wheel(void)
{
    set_sample_rate(200);
    set_sample_rate(100);
    set_sample_rate(80);

    if (mouse_command(0xF2) != 0xFA)
        return;
    device_id = read_data();
    if (device_id == 3)
        packet_size = 4;
}

static void mouse_callback(registers_t *regs)
{
    (void)regs;

    if (!(inb(PS2_STATUS) & STATUS_OUTPUT_FULL))
        return;

    uint8_t byte = inb(PS2_DATA);

    /* Bit 3 of the first byte is always set; use it to resynchronise after a
       dropped byte instead of decoding garbage forever. */
    if (packet_index == 0 && !(byte & 0x08))
        return;

    packet[packet_index++] = byte;
    if (packet_index < packet_size)
        return;
    packet_index = 0;

    if (packet_size < 4)
        return;                     /* no wheel on this device */

    int8_t z = (int8_t)(packet[3] & 0x0F);
    if (z & 0x08)
        z = (int8_t)(z | 0xF0);     /* sign extend the 4-bit field */

    if (z == 0)
        return;

    /* Wheel away from the user reports a negative Z and should reveal older
       output, which is what a positive view offset means. */
    vga_scroll_view(-z * LINES_PER_NOTCH);
}

void mouse_init(void)
{
    uint8_t status;

    if (!wait_writable())
        return;
    outb(PS2_COMMAND, 0xA8);                /* enable the auxiliary port */

    if (!wait_writable())
        return;
    outb(PS2_COMMAND, 0x20);                /* read the configuration byte */
    int config = read_data();
    if (config < 0)
        return;

    status = (uint8_t)config;
    status |= 0x02;                         /* IRQ12 on                   */
    status &= (uint8_t)~0x20;               /* clock the aux port         */

    if (!wait_writable())
        return;
    outb(PS2_COMMAND, 0x60);                /* write the configuration byte */
    if (!wait_writable())
        return;
    outb(PS2_DATA, status);

    if (mouse_command(0xF6) != 0xFA)        /* restore default settings */
        return;

    enable_wheel();

    if (mouse_command(0xF4) != 0xFA)        /* start reporting */
        return;

    present = true;
    packet_index = 0;
    irq_install_handler(12, mouse_callback);
    irq_set_masked(12, false);              /* IRQ12 lives on the slave PIC */
}

int  mouse_device_id(void) { return device_id; }
bool mouse_present(void)   { return present; }
