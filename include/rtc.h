#ifndef _LOS_RTC_H
#define _LOS_RTC_H

#include "types.h"

typedef struct {
    uint8_t second, minute, hour;
    uint8_t day, month;
    uint16_t year;
} rtc_time_t;

void rtc_read(rtc_time_t *out);

#endif
