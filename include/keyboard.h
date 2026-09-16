#ifndef _LOS_KEYBOARD_H
#define _LOS_KEYBOARD_H

#include "types.h"

/* Non-ASCII keys are reported as values above 0x80. */
#define KEY_UP     0x101
#define KEY_DOWN   0x102
#define KEY_LEFT   0x103
#define KEY_RIGHT  0x104
#define KEY_HOME     0x105
#define KEY_END      0x106
#define KEY_DELETE   0x107
#define KEY_PAGEUP   0x108
#define KEY_PAGEDOWN 0x109

void keyboard_init(void);
int  keyboard_getchar(void);     /* blocking */
int  keyboard_poll(void);        /* 0 if nothing is queued */

#endif
