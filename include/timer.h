#ifndef _LOS_TIMER_H
#define _LOS_TIMER_H

#include "types.h"

#define TIMER_HZ 100

void     timer_init(void);
uint64_t timer_ticks(void);
uint64_t timer_uptime_ms(void);
void     sleep_ms(uint32_t ms);

#endif
