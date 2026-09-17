#ifndef _LOS_PERSIST_H
#define _LOS_PERSIST_H

#include "types.h"

typedef enum {
    PERSIST_NO_DISK = 0,    /* no data drive was found            */
    PERSIST_EMPTY,          /* drive present but never formatted  */
    PERSIST_LOADED,         /* a snapshot was restored from disk  */
    PERSIST_ERROR,          /* drive present but the image is bad */
} persist_state_t;

/* Looks for a data disk and restores the filesystem and accounts from it.
   Returns the resulting state. */
persist_state_t persist_mount(void);

/* Writes the whole filesystem and account table back to disk. Returns 0 on
   success. Does nothing (and succeeds) when there is no data disk. */
int persist_save(void);

/* Queues a save to happen when the current shell command finishes. */
void persist_mark_dirty(void);
bool persist_is_dirty(void);
int  persist_flush(void);          /* saves only when marked dirty */

persist_state_t persist_state(void);
bool     persist_available(void);
uint32_t persist_bytes_used(void);
uint32_t persist_bytes_capacity(void);
uint32_t persist_saves(void);

/* Wipes the on-disk snapshot so the next boot starts from a fresh system.
   Afterwards the layer is sealed: persist_save() refuses to write until the
   machine reboots, because the tree still in RAM would otherwise be written
   straight back and undo the wipe. */
int persist_format(void);
bool persist_sealed(void);

#endif
