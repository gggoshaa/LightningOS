#ifndef _LOS_KPRINTF_H
#define _LOS_KPRINTF_H

#include "types.h"

/* Supports %s %c %d %i %u %x %X %p %% and a width of the form %5d / %05x. */
void kprintf(const char *fmt, ...);
void kvsnprintf(char *buf, size_t size, const char *fmt, va_list args);
void ksnprintf(char *buf, size_t size, const char *fmt, ...);

#endif
