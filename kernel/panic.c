#include "panic.h"
#include "vga.h"
#include "kprintf.h"
#include "io.h"

static void banner(const char *message)
{
    vga_set_color(VGA_WHITE, VGA_RED);
    vga_clear();
    kprintf("\n  *** KERNEL PANIC ***\n\n  %s\n", message);
}

void panic(const char *message)
{
    cli();
    banner(message);
    kprintf("\n  System halted.\n");
    for (;;)
        hlt();
}

void panic_regs(const char *message, registers_t *regs)
{
    cli();
    banner(message);
    kprintf("\n  int=%d  err=0x%08x  eip=0x%08x  cs=0x%04x  eflags=0x%08x\n",
            regs->int_no, regs->err_code, regs->eip, regs->cs, regs->eflags);
    kprintf("  eax=0x%08x ebx=0x%08x ecx=0x%08x edx=0x%08x\n",
            regs->eax, regs->ebx, regs->ecx, regs->edx);
    kprintf("  esi=0x%08x edi=0x%08x ebp=0x%08x esp=0x%08x\n",
            regs->esi, regs->edi, regs->ebp, regs->esp_dummy);

    if (regs->int_no == 14) {            /* page fault - CR2 holds the address */
        uint32_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        kprintf("  faulting address: 0x%08x\n", cr2);
    }

    kprintf("\n  System halted.\n");
    for (;;)
        hlt();
}
