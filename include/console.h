#ifndef _LOS_CONSOLE_H
#define _LOS_CONSOLE_H

#include "types.h"

/* Called to redraw the prompt after the screen is cleared with Ctrl-L. */
typedef void (*console_prompt_fn)(void);

typedef enum {
    CONSOLE_ECHO_PLAIN = 0,     /* show what is typed          */
    CONSOLE_ECHO_HIDDEN,        /* show nothing (passwords)    */
    CONSOLE_ECHO_STARS,         /* show one '*' per character  */
} console_echo_t;

/* Reads one line. Returns false if the user cancelled with Ctrl-C. */
bool console_read_line(char *out, size_t size, console_echo_t echo,
                       bool use_history, console_prompt_fn prompt);

void        console_history_push(const char *line);
int         console_history_count(void);
const char *console_history_at(int index);

#endif
