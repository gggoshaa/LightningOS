#ifndef _LOS_MEM_H
#define _LOS_MEM_H

#include "types.h"

void   mem_init(void);
void  *kmalloc(size_t size);
void   kfree(void *ptr);
void  *kcalloc(size_t count, size_t size);

uint64_t mem_total_bytes(void);      /* usable RAM reported by the BIOS */
size_t   mem_heap_size(void);
size_t   mem_heap_used(void);
size_t   mem_heap_free(void);
int      mem_region_count(void);
void     mem_region_info(int index, uint64_t *base, uint64_t *len, uint32_t *type);

#endif
