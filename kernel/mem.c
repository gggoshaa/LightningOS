#include "mem.h"
#include "string.h"
#include "kprintf.h"
#include "panic.h"
#include "lock.h"

/* The bootloader stashed the BIOS memory map here before leaving real mode. */
#define E820_COUNT_ADDR 0x8000
#define E820_LIST_ADDR  0x8004
#define E820_MAX        32

#define E820_TYPE_USABLE 1

/* The kernel image lives at 0x10000 and is at most 128 KiB, so 2 MiB is a safe
   place to start the heap without a page allocator in the way. */
#define HEAP_BASE      0x00200000
#define HEAP_MAX_SIZE  (32 * 1024 * 1024)
#define HEAP_MIN_SIZE  (1  * 1024 * 1024)

#define BLOCK_MAGIC 0x4C4F5342u    /* "LOSB" */

typedef struct e820_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t acpi;
} __attribute__((packed)) e820_entry_t;

typedef struct block {
    uint32_t      magic;
    size_t        size;        /* payload bytes, not counting this header */
    bool          free;
    struct block *next;
    struct block *prev;
} block_t;

static e820_entry_t regions[E820_MAX];
static int          region_count;
static uint64_t     total_usable;

static block_t *heap_head;
static size_t   heap_size;
static size_t   heap_used;

static void read_memory_map(void)
{
    uint16_t count = *(volatile uint16_t *)E820_COUNT_ADDR;
    const e820_entry_t *src = (const e820_entry_t *)E820_LIST_ADDR;

    if (count > E820_MAX)
        count = E820_MAX;

    for (int i = 0; i < count; i++) {
        regions[i] = src[i];
        if (regions[i].type == E820_TYPE_USABLE)
            total_usable += regions[i].length;
    }
    region_count = count;
}

/* Finds how far the usable region containing HEAP_BASE extends. */
static size_t usable_heap_size(void)
{
    for (int i = 0; i < region_count; i++) {
        if (regions[i].type != E820_TYPE_USABLE)
            continue;

        uint64_t start = regions[i].base;
        uint64_t end   = regions[i].base + regions[i].length;

        if (start <= HEAP_BASE && end > HEAP_BASE) {
            uint64_t available = end - HEAP_BASE;
            if (available > HEAP_MAX_SIZE)
                available = HEAP_MAX_SIZE;
            return (size_t)available;
        }
    }

    /* No memory map (very old BIOS) - assume the classic 16 MiB machine. */
    return HEAP_MIN_SIZE;
}

void mem_init(void)
{
    read_memory_map();

    heap_size = usable_heap_size();
    if (heap_size < HEAP_MIN_SIZE)
        panic("not enough RAM to create the kernel heap");

    heap_head = (block_t *)HEAP_BASE;
    heap_head->magic = BLOCK_MAGIC;
    heap_head->size  = heap_size - sizeof(block_t);
    heap_head->free  = true;
    heap_head->next  = NULL;
    heap_head->prev  = NULL;
    heap_used = 0;
}

static void split_block(block_t *block, size_t size)
{
    /* Only split when the leftover can hold a header plus something useful. */
    if (block->size < size + sizeof(block_t) + 16)
        return;

    block_t *rest = (block_t *)((uint8_t *)block + sizeof(block_t) + size);
    rest->magic = BLOCK_MAGIC;
    rest->size  = block->size - size - sizeof(block_t);
    rest->free  = true;
    rest->next  = block->next;
    rest->prev  = block;

    if (block->next)
        block->next->prev = rest;
    block->next = rest;
    block->size = size;
}

/* The free list is shared by every task, so allocation runs with interrupts
   masked. The critical section is a short walk over a handful of blocks. */
void *kmalloc(size_t size)
{
    if (size == 0)
        return NULL;

    size = (size + 7) & ~((size_t)7);       /* 8-byte alignment */

    uint32_t flags = irq_save();

    for (block_t *block = heap_head; block; block = block->next) {
        if (!block->free || block->size < size)
            continue;

        split_block(block, size);
        block->free = false;
        heap_used += block->size + sizeof(block_t);
        irq_restore(flags);
        return (uint8_t *)block + sizeof(block_t);
    }

    irq_restore(flags);
    return NULL;                            /* out of heap */
}

void *kcalloc(size_t count, size_t size)
{
    void *ptr = kmalloc(count * size);

    if (ptr)
        memset(ptr, 0, count * size);
    return ptr;
}

static void coalesce(block_t *block)
{
    if (block->next && block->next->free) {
        block->size += block->next->size + sizeof(block_t);
        block->next = block->next->next;
        if (block->next)
            block->next->prev = block;
    }
    if (block->prev && block->prev->free) {
        block->prev->size += block->size + sizeof(block_t);
        block->prev->next = block->next;
        if (block->next)
            block->next->prev = block->prev;
    }
}

void kfree(void *ptr)
{
    if (!ptr)
        return;

    block_t *block = (block_t *)((uint8_t *)ptr - sizeof(block_t));

    if (block->magic != BLOCK_MAGIC)
        panic("kfree() on a pointer that is not a heap block");

    uint32_t flags = irq_save();
    if (!block->free) {
        block->free = true;
        heap_used -= block->size + sizeof(block_t);
        coalesce(block);
    }
    irq_restore(flags);
}

uint64_t mem_total_bytes(void) { return total_usable; }
size_t   mem_heap_size(void)   { return heap_size; }
size_t   mem_heap_used(void)   { return heap_used; }
size_t   mem_heap_free(void)   { return heap_size - heap_used; }
int      mem_region_count(void) { return region_count; }

void mem_region_info(int index, uint64_t *base, uint64_t *len, uint32_t *type)
{
    if (index < 0 || index >= region_count)
        return;
    if (base) *base = regions[index].base;
    if (len)  *len  = regions[index].length;
    if (type) *type = regions[index].type;
}
