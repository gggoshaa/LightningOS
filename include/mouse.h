#ifndef _LOS_MOUSE_H
#define _LOS_MOUSE_H

#include "types.h"

void mouse_init(void);

/* 0 if no wheel was detected, 3 for a standard IntelliMouse. */
int  mouse_device_id(void);
bool mouse_present(void);

#endif
