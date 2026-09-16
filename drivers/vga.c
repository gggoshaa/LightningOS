#include "vga.h"
#include "io.h"
#include "string.h"

#define VGA_MEMORY 0xB8000
#define CRTC_INDEX 0x3D4
#define CRTC_DATA  0x3D5

#define SCROLLBACK_LINES 512

static volatile uint16_t *const vram = (volatile uint16_t *)VGA_MEMORY;

/* Everything is written into `screen` first and only mirrored to video memory
   while the view is at the bottom. That way scrolling back can repaint the
   display from `scrollback` without destroying the live contents. */
static uint16_t screen[VGA_HEIGHT][VGA_WIDTH];
static uint16_t scrollback[SCROLLBACK_LINES][VGA_WIDTH];
static int  scrollback_head;      /* next slot to write */
static int  scrollback_count;     /* lines currently stored */
static int  view_offset;          /* 0 = live, N = N lines back */

static int   cursor_row;
static int   cursor_col;
static uint8_t attribute;

static inline uint16_t cell(char c, uint8_t attr)
{
    return (uint16_t)(uint8_t)c | ((uint16_t)attr << 8);
}

static void move_hardware_cursor(void)
{
    uint16_t pos;

    if (view_offset > 0)
        pos = VGA_WIDTH * VGA_HEIGHT;       /* park it off screen */
    else
        pos = (uint16_t)(cursor_row * VGA_WIDTH + cursor_col);

    outb(CRTC_INDEX, 0x0F);
    outb(CRTC_DATA, (uint8_t)(pos & 0xFF));
    outb(CRTC_INDEX, 0x0E);
    outb(CRTC_DATA, (uint8_t)((pos >> 8) & 0xFF));
}

/* Logical line `index` of the scrollback, 0 being the oldest kept line. */
static const uint16_t *scrollback_line(int index)
{
    int slot = (scrollback_head - scrollback_count + index + SCROLLBACK_LINES)
               % SCROLLBACK_LINES;
    return scrollback[slot];
}

static void repaint(void)
{
    for (int row = 0; row < VGA_HEIGHT; row++) {
        int index = scrollback_count - view_offset + row;
        const uint16_t *source;

        if (index < scrollback_count)
            source = scrollback_line(index);
        else
            source = screen[index - scrollback_count];

        for (int col = 0; col < VGA_WIDTH; col++)
            vram[row * VGA_WIDTH + col] = source[col];
    }
    move_hardware_cursor();
}

static void push_scrollback(const uint16_t *line)
{
    memcpy(scrollback[scrollback_head], line, VGA_WIDTH * sizeof(uint16_t));
    scrollback_head = (scrollback_head + 1) % SCROLLBACK_LINES;
    if (scrollback_count < SCROLLBACK_LINES)
        scrollback_count++;
}

static void scroll_up_one(void)
{
    push_scrollback(screen[0]);

    for (int row = 1; row < VGA_HEIGHT; row++)
        memcpy(screen[row - 1], screen[row], VGA_WIDTH * sizeof(uint16_t));
    for (int col = 0; col < VGA_WIDTH; col++)
        screen[VGA_HEIGHT - 1][col] = cell(' ', attribute);

    cursor_row = VGA_HEIGHT - 1;
}

void vga_set_color(uint8_t fg, uint8_t bg)
{
    attribute = (uint8_t)(fg | (bg << 4));
}

void vga_get_color(uint8_t *fg, uint8_t *bg)
{
    if (fg) *fg = (uint8_t)(attribute & 0x0F);
    if (bg) *bg = (uint8_t)((attribute >> 4) & 0x0F);
}

/* Moves whatever is on screen into the scrollback, the way a terminal keeps
   its history when you run `clear`. Trailing blank rows are dropped. */
static void archive_screen(void)
{
    int last = -1;

    for (int row = 0; row < VGA_HEIGHT; row++) {
        for (int col = 0; col < VGA_WIDTH; col++) {
            char c = (char)(screen[row][col] & 0xFF);
            if (c != ' ' && c != '\0') {
                last = row;
                break;
            }
        }
    }

    for (int row = 0; row <= last; row++)
        push_scrollback(screen[row]);
}

void vga_clear(void)
{
    archive_screen();

    for (int row = 0; row < VGA_HEIGHT; row++) {
        for (int col = 0; col < VGA_WIDTH; col++)
            screen[row][col] = cell(' ', attribute);
    }
    cursor_row = 0;
    cursor_col = 0;
    view_offset = 0;
    repaint();
}

void vga_init(void)
{
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    scrollback_head = 0;
    scrollback_count = 0;
    view_offset = 0;
    vga_clear();

    /* A visible block cursor spanning scanlines 14-15. */
    outb(CRTC_INDEX, 0x0A);
    outb(CRTC_DATA, 14);
    outb(CRTC_INDEX, 0x0B);
    outb(CRTC_DATA, 15);
}

void vga_putc(char c)
{
    /* New output always brings the reader back to the live screen. */
    if (view_offset != 0) {
        view_offset = 0;
        repaint();
    }

    switch (c) {
    case '\n':
        cursor_col = 0;
        cursor_row++;
        break;
    case '\r':
        cursor_col = 0;
        break;
    case '\t':
        cursor_col = (cursor_col + 4) & ~3;
        break;
    case '\b':
        if (cursor_col > 0) {
            cursor_col--;
        } else if (cursor_row > 0) {
            cursor_row--;
            cursor_col = VGA_WIDTH - 1;
        }
        screen[cursor_row][cursor_col] = cell(' ', attribute);
        vram[cursor_row * VGA_WIDTH + cursor_col] = screen[cursor_row][cursor_col];
        break;
    default:
        screen[cursor_row][cursor_col] = cell(c, attribute);
        vram[cursor_row * VGA_WIDTH + cursor_col] = screen[cursor_row][cursor_col];
        cursor_col++;
        break;
    }

    if (cursor_col >= VGA_WIDTH) {
        cursor_col = 0;
        cursor_row++;
    }

    if (cursor_row >= VGA_HEIGHT) {
        scroll_up_one();
        repaint();
        return;
    }

    move_hardware_cursor();
}

void vga_write(const char *s)
{
    while (*s)
        vga_putc(*s++);
}

void vga_set_cursor(int row, int col)
{
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (row >= VGA_HEIGHT) row = VGA_HEIGHT - 1;
    if (col >= VGA_WIDTH) col = VGA_WIDTH - 1;
    cursor_row = row;
    cursor_col = col;
    move_hardware_cursor();
}

int vga_row(void) { return cursor_row; }
int vga_col(void) { return cursor_col; }

void vga_scroll_view(int lines)
{
    int target = view_offset + lines;

    if (target < 0)
        target = 0;
    if (target > scrollback_count)
        target = scrollback_count;
    if (target == view_offset)
        return;

    view_offset = target;
    repaint();
}

void vga_scroll_to_bottom(void)
{
    if (view_offset == 0)
        return;
    view_offset = 0;
    repaint();
}

int vga_view_offset(void)     { return view_offset; }
int vga_scrollback_lines(void) { return scrollback_count; }
