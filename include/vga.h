#ifndef _LOS_VGA_H
#define _LOS_VGA_H

#include "types.h"

enum vga_color {
    VGA_BLACK = 0, VGA_BLUE, VGA_GREEN, VGA_CYAN,
    VGA_RED, VGA_MAGENTA, VGA_BROWN, VGA_LIGHT_GREY,
    VGA_DARK_GREY, VGA_LIGHT_BLUE, VGA_LIGHT_GREEN, VGA_LIGHT_CYAN,
    VGA_LIGHT_RED, VGA_LIGHT_MAGENTA, VGA_YELLOW, VGA_WHITE
};

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

void vga_init(void);
void vga_clear(void);
void vga_putc(char c);
void vga_write(const char *s);
void vga_set_color(uint8_t fg, uint8_t bg);
void vga_get_color(uint8_t *fg, uint8_t *bg);
void vga_set_cursor(int row, int col);
int  vga_row(void);
int  vga_col(void);

/* Scrollback. Positive `lines` moves the view towards older output, negative
   back towards the live screen. Any new output snaps the view to the bottom. */
void vga_scroll_view(int lines);
void vga_scroll_to_bottom(void);
int  vga_view_offset(void);
int  vga_scrollback_lines(void);

#endif
