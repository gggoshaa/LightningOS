#include "isr.h"
#include "io.h"
#include "string.h"
#include "kprintf.h"
#include "panic.h"

#define IDT_ENTRIES 256

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

struct idt_entry {
    uint16_t base_low;
    uint16_t selector;
    uint8_t  always0;
    uint8_t  flags;
    uint16_t base_high;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr   idt_pointer;
static isr_handler_t    irq_handlers[16];

extern void idt_flush(struct idt_ptr *ptr);

extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);

extern void irq0(void);  extern void irq1(void);  extern void irq2(void);
extern void irq3(void);  extern void irq4(void);  extern void irq5(void);
extern void irq6(void);  extern void irq7(void);  extern void irq8(void);
extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void);
extern void irq15(void);

static const char *exception_names[32] = {
    "Divide by zero",
    "Debug",
    "Non-maskable interrupt",
    "Breakpoint",
    "Overflow",
    "Bound range exceeded",
    "Invalid opcode",
    "Device not available",
    "Double fault",
    "Coprocessor segment overrun",
    "Invalid TSS",
    "Segment not present",
    "Stack segment fault",
    "General protection fault",
    "Page fault",
    "Reserved",
    "x87 floating point exception",
    "Alignment check",
    "Machine check",
    "SIMD floating point exception",
    "Virtualization exception",
    "Control protection exception",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor injection exception",
    "Security exception",
};

static void set_gate(int index, uint32_t base, uint16_t selector, uint8_t flags)
{
    idt[index].base_low  = (uint16_t)(base & 0xFFFF);
    idt[index].base_high = (uint16_t)((base >> 16) & 0xFFFF);
    idt[index].selector  = selector;
    idt[index].always0   = 0;
    idt[index].flags     = flags;
}

/* The PIC powers up mapped over the CPU exception vectors, so move the master
   to 0x20 and the slave to 0x28. */
static void pic_remap(void)
{
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    outb(PIC1_COMMAND, 0x11); io_wait();    /* start init, ICW4 will follow */
    outb(PIC2_COMMAND, 0x11); io_wait();
    outb(PIC1_DATA, 0x20);    io_wait();    /* master vector offset */
    outb(PIC2_DATA, 0x28);    io_wait();    /* slave vector offset  */
    outb(PIC1_DATA, 0x04);    io_wait();    /* slave sits on IRQ2   */
    outb(PIC2_DATA, 0x02);    io_wait();
    outb(PIC1_DATA, 0x01);    io_wait();    /* 8086 mode            */
    outb(PIC2_DATA, 0x01);    io_wait();

    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}

void idt_init(void)
{
    idt_pointer.limit = (uint16_t)(sizeof(idt) - 1);
    idt_pointer.base  = (uint32_t)&idt;
    memset(&idt, 0, sizeof(idt));
    memset(&irq_handlers, 0, sizeof(irq_handlers));

    pic_remap();

    void (*stubs[48])(void) = {
        isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7,
        isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15,
        isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
        isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
        irq0,  irq1,  irq2,  irq3,  irq4,  irq5,  irq6,  irq7,
        irq8,  irq9,  irq10, irq11, irq12, irq13, irq14, irq15,
    };

    for (int i = 0; i < 48; i++)
        set_gate(i, (uint32_t)stubs[i], 0x08, 0x8E);   /* ring 0, 32-bit gate */

    idt_flush(&idt_pointer);

    /* Unmask only the lines we actually service: timer, keyboard, and IRQ2
       which is the cascade to the slave controller. Drivers open the rest
       themselves through irq_set_masked(). */
    outb(PIC1_DATA, (uint8_t)~0x07);
    outb(PIC2_DATA, 0xFF);
}

void irq_set_masked(int irq, bool masked)
{
    uint16_t port;
    uint8_t bit, mask;

    if (irq < 0 || irq >= 16)
        return;

    if (irq < 8) {
        port = PIC1_DATA;
        bit = (uint8_t)irq;
    } else {
        port = PIC2_DATA;
        bit = (uint8_t)(irq - 8);
    }

    mask = inb(port);
    if (masked)
        mask |= (uint8_t)(1 << bit);
    else
        mask &= (uint8_t)~(1 << bit);
    outb(port, mask);
}

void irq_install_handler(int irq, isr_handler_t handler)
{
    if (irq >= 0 && irq < 16)
        irq_handlers[irq] = handler;
}

void irq_uninstall_handler(int irq)
{
    if (irq >= 0 && irq < 16)
        irq_handlers[irq] = NULL;
}

void isr_handler(registers_t *regs)
{
    const char *name = (regs->int_no < 32)
                           ? exception_names[regs->int_no]
                           : "Unknown interrupt";

    panic_regs(name, regs);
}

void irq_handler(registers_t *regs)
{
    int irq = (int)regs->int_no - 32;

    if (irq >= 8)
        outb(PIC2_COMMAND, 0x20);       /* end of interrupt, slave  */
    outb(PIC1_COMMAND, 0x20);           /* end of interrupt, master */

    if (irq >= 0 && irq < 16 && irq_handlers[irq])
        irq_handlers[irq](regs);
}
