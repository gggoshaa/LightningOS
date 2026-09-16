#ifndef _LOS_LOCK_H
#define _LOS_LOCK_H

#include "types.h"

/* On a uniprocessor kernel with no kernel preemption other than the timer,
   masking interrupts is a complete mutex: nothing else can be running.
   These save and restore the previous flag state so the pairs nest safely. */

static inline uint32_t irq_save(void)
{
    uint32_t flags;

    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(uint32_t flags)
{
    __asm__ volatile("pushl %0; popfl" : : "r"(flags) : "memory", "cc");
}

#endif
