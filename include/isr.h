#ifndef _LOS_ISR_H
#define _LOS_ISR_H

#include "types.h"

/* Register snapshot pushed by the stubs in kernel/interrupt.asm.
   The field order must match the push order used there. */
typedef struct {
    uint32_t ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, useresp, ss;
} registers_t;

typedef void (*isr_handler_t)(registers_t *regs);

/* The dispatchers return the stack pointer to resume on. Normally that is
   the frame they were given; the scheduler returns another task's stack. */
uint32_t isr_handler(registers_t *regs);
uint32_t irq_handler(registers_t *regs);
uint32_t yield_handler(registers_t *regs);

#define VECTOR_YIELD 0x30

void idt_init(void);
void irq_install_handler(int irq, isr_handler_t handler);
void irq_uninstall_handler(int irq);
void irq_set_masked(int irq, bool masked);

#define IRQ0_TIMER    32
#define IRQ1_KEYBOARD 33
#define IRQ12_MOUSE   44

#endif
