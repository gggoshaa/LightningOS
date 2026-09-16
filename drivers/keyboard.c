#include "keyboard.h"
#include "isr.h"
#include "io.h"

#define KBD_DATA   0x60
#define BUFFER_LEN 128

/* Scancode set 1, US layout. Index = scancode, value = unshifted character. */
static const char keymap[128] = {
     0,   27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
     0,   'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
     0,   '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
     0,   '*',  0,  ' ',
};

static const char keymap_shift[128] = {
     0,   27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
     0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
     0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
     0,   '*',  0,  ' ',
};

static int  buffer[BUFFER_LEN];
static volatile int head;
static volatile int tail;

static bool shift_down;
static bool caps_lock;
static bool ctrl_down;
static bool extended;       /* the previous byte was the 0xE0 prefix */

static void push(int key)
{
    int next = (head + 1) % BUFFER_LEN;

    if (next == tail)
        return;             /* buffer full - drop the keystroke */
    buffer[head] = key;
    head = next;
}

static void keyboard_callback(registers_t *regs)
{
    (void)regs;
    uint8_t scancode = inb(KBD_DATA);

    if (scancode == 0xE0) {
        extended = true;
        return;
    }

    if (extended) {
        extended = false;
        if (!(scancode & 0x80)) {
            switch (scancode) {
            case 0x48: push(KEY_UP);     return;
            case 0x50: push(KEY_DOWN);   return;
            case 0x4B: push(KEY_LEFT);   return;
            case 0x4D: push(KEY_RIGHT);  return;
            case 0x47: push(KEY_HOME);     return;
            case 0x4F: push(KEY_END);      return;
            case 0x53: push(KEY_DELETE);   return;
            case 0x49: push(KEY_PAGEUP);   return;
            case 0x51: push(KEY_PAGEDOWN); return;
            default: return;
            }
        }
        return;
    }

    if (scancode & 0x80) {                  /* key release */
        uint8_t made = (uint8_t)(scancode & 0x7F);
        if (made == 0x2A || made == 0x36)
            shift_down = false;
        else if (made == 0x1D)
            ctrl_down = false;
        return;
    }

    switch (scancode) {
    case 0x2A:
    case 0x36:
        shift_down = true;
        return;
    case 0x1D:
        ctrl_down = true;
        return;
    case 0x3A:
        caps_lock = !caps_lock;
        return;
    default:
        break;
    }

    if (scancode >= 128)
        return;

    char c = shift_down ? keymap_shift[scancode] : keymap[scancode];
    if (!c)
        return;

    if (caps_lock) {
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        else if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
    }

    if (ctrl_down) {
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 1);        /* Ctrl-A .. Ctrl-Z */
        else if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 1);
    }

    push((int)(unsigned char)c);
}

void keyboard_init(void)
{
    head = tail = 0;
    irq_install_handler(1, keyboard_callback);
}

int keyboard_poll(void)
{
    int key;

    if (head == tail)
        return 0;
    key = buffer[tail];
    tail = (tail + 1) % BUFFER_LEN;
    return key;
}

int keyboard_getchar(void)
{
    int key;

    while ((key = keyboard_poll()) == 0)
        __asm__ volatile("sti; hlt");
    return key;
}
