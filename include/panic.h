#ifndef _LOS_PANIC_H
#define _LOS_PANIC_H

#include "isr.h"

void panic(const char *message);
void panic_regs(const char *message, registers_t *regs);

#endif
