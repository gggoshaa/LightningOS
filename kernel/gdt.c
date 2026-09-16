#include "gdt.h"
#include "string.h"

#define GDT_ENTRIES 5

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct gdt_entry gdt[GDT_ENTRIES];
static struct gdt_ptr   gdt_pointer;

extern void gdt_flush(struct gdt_ptr *ptr);

static void set_gate(int index, uint32_t base, uint32_t limit,
                     uint8_t access, uint8_t granularity)
{
    gdt[index].base_low    = (uint16_t)(base & 0xFFFF);
    gdt[index].base_middle = (uint8_t)((base >> 16) & 0xFF);
    gdt[index].base_high   = (uint8_t)((base >> 24) & 0xFF);
    gdt[index].limit_low   = (uint16_t)(limit & 0xFFFF);
    gdt[index].granularity = (uint8_t)(((limit >> 16) & 0x0F) | (granularity & 0xF0));
    gdt[index].access      = access;
}

/* A flat 4 GiB model: every segment covers the whole address space, so C code
   can treat pointers as plain physical addresses. */
void gdt_init(void)
{
    gdt_pointer.limit = (uint16_t)(sizeof(gdt) - 1);
    gdt_pointer.base  = (uint32_t)&gdt;

    set_gate(0, 0, 0x00000000, 0x00, 0x00);   /* null          */
    set_gate(1, 0, 0x000FFFFF, 0x9A, 0xCF);   /* ring 0 code   */
    set_gate(2, 0, 0x000FFFFF, 0x92, 0xCF);   /* ring 0 data   */
    set_gate(3, 0, 0x000FFFFF, 0xFA, 0xCF);   /* ring 3 code   */
    set_gate(4, 0, 0x000FFFFF, 0xF2, 0xCF);   /* ring 3 data   */

    gdt_flush(&gdt_pointer);
}
