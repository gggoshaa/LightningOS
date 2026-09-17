#ifndef _LOS_INSTALL_H
#define _LOS_INSTALL_H

#include "types.h"
#include "ata.h"

/* Where the hard disk layout puts things. The kernel is read by the boot
   sector as one contiguous run starting at sector 1; the data area begins far
   enough past it that the kernel can grow without colliding. */
#define INSTALL_KERNEL_LBA   1
#define INSTALL_KERNEL_MAX   256        /* must match the bootloader */
#define INSTALL_DATA_LBA     2048

/* True when this boot came off the install medium rather than a disk. */
bool install_booted_from_medium(void);

/* Takes the pristine copy of the kernel image. Must be called before anything
   has had a chance to modify .data - in practice, straight after mem_init. */
void install_capture_image(void);

uint32_t install_image_bytes(void);
uint32_t install_image_sectors(void);

/* Picks the disk to install onto: the first ATA drive that is not the one we
   are running from. Returns false when there is nothing suitable. */
bool install_find_target(ata_drive_t *drive);

/* Writes boot sector, kernel and a blank data area onto `drive`.
   Returns 0 on success. */
int install_to_disk(ata_drive_t drive);

/* The full screen installer. Only meaningful when booted from the medium. */
void install_run_menu(void);

#endif
