#ifndef _LOS_ATA_H
#define _LOS_ATA_H

#include "types.h"

#define ATA_SECTOR_SIZE 512

typedef enum {
    ATA_MASTER = 0,
    ATA_SLAVE  = 1,
} ata_drive_t;

/* Probes the primary channel. Safe to call before interrupts are enabled -
   the driver polls and never uses IRQ14. */
void ata_init(void);

bool     ata_present(ata_drive_t drive);
uint32_t ata_sector_count(ata_drive_t drive);
const char *ata_model(ata_drive_t drive);

/* Both return 0 on success. `count` is limited to 255 sectors per call;
   larger transfers are split internally. */
int ata_read(ata_drive_t drive, uint32_t lba, uint32_t count, void *buffer);
int ata_write(ata_drive_t drive, uint32_t lba, uint32_t count, const void *buffer);

#endif
