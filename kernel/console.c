#include "console.h"
#include "vga.h"
#include "keyboard.h"
#include "string.h"

#define LINE_MAX     256
#define HISTORY_MAX  16
#define SCROLL_STEP  3
#define PAGE_STEP    (VGA_HEIGHT - 2)

static char history[HISTORY_MAX][LINE_MAX];
static int  history_total;      /* lines ever entered, not just the kept ones */

void console_history_push(const char *line)
{
    if (!line || !line[0])
        return;
    if (history_total > 0 &&
        strcmp(history[(history_total - 1) % HISTORY_MAX], line) == 0)
        return;

    strncpy(history[history_total % HISTORY_MAX], line, LINE_MAX - 1);
    history[history_total % HISTORY_MAX][LINE_MAX - 1] = '\0';
    history_total++;
}

int console_history_count(void)
{
    return history_total;
}

const char *console_history_at(int index)
{
    if (index < 0 || index >= history_total)
        return NULL;
    if (history_total > HISTORY_MAX && index < history_total - HISTORY_MAX)
        return NULL;
    return history[index % HISTORY_MAX];
}

static void echo_char(console_echo_t echo, char c)
{
    if (echo == CONSOLE_ECHO_PLAIN)
        vga_putc(c);
    else if (echo == CONSOLE_ECHO_STARS)
        vga_putc('*');
}

static void echo_string(console_echo_t echo, const char *s)
{
    while (*s)
        echo_char(echo, *s++);
}

/* Wipes the visible part of the line and redraws it from `line`. */
static void redraw(console_echo_t echo, const char *line, int *drawn)
{
    for (int i = 0; i < *drawn; i++)
        vga_putc('\b');
    echo_string(echo, line);
    *drawn = (echo == CONSOLE_ECHO_HIDDEN) ? 0 : (int)strlen(line);
}

bool console_read_line(char *out, size_t size, console_echo_t echo,
                       bool use_history, console_prompt_fn prompt)
{
    int length = 0;
    int drawn = 0;
    int browse = history_total;
    int limit = (int)(size < LINE_MAX ? size : LINE_MAX) - 1;

    out[0] = '\0';

    for (;;) {
        int key = keyboard_getchar();

        switch (key) {
        case '\n':
            vga_scroll_to_bottom();
            vga_putc('\n');
            out[length] = '\0';
            return true;

        case '\b':
            if (length > 0) {
                length--;
                out[length] = '\0';
                if (echo != CONSOLE_ECHO_HIDDEN) {
                    vga_putc('\b');
                    drawn--;
                }
            }
            continue;

        case 3:                         /* Ctrl-C */
            vga_scroll_to_bottom();
            vga_write("^C\n");
            out[0] = '\0';
            return false;

        case 12:                        /* Ctrl-L */
            vga_clear();
            if (prompt)
                prompt();
            drawn = 0;
            redraw(echo, out, &drawn);
            continue;

        case 21:                        /* Ctrl-U - discard the line */
            redraw(echo, "", &drawn);
            out[0] = '\0';
            length = 0;
            continue;

        case KEY_PAGEUP:
            vga_scroll_view(PAGE_STEP);
            continue;

        case KEY_PAGEDOWN:
            vga_scroll_view(-PAGE_STEP);
            continue;

        case KEY_UP:
        case KEY_DOWN: {
            if (!use_history) {
                /* Without history the arrows are free to scroll instead. */
                vga_scroll_view(key == KEY_UP ? SCROLL_STEP : -SCROLL_STEP);
                continue;
            }

            int target = (key == KEY_UP) ? browse - 1 : browse + 1;
            if (target < 0)
                continue;

            if (target >= history_total) {
                browse = history_total;
                redraw(echo, "", &drawn);
                out[0] = '\0';
                length = 0;
                continue;
            }

            const char *entry = console_history_at(target);
            if (!entry)
                continue;

            browse = target;
            strncpy(out, entry, (size_t)limit);
            out[limit] = '\0';
            length = (int)strlen(out);
            redraw(echo, out, &drawn);
            continue;
        }

        default:
            break;
        }

        if (key < 32 || key > 126)
            continue;

        if (length < limit) {
            vga_scroll_to_bottom();
            out[length++] = (char)key;
            out[length] = '\0';
            echo_char(echo, (char)key);
            if (echo != CONSOLE_ECHO_HIDDEN)
                drawn++;
        }
    }
}
