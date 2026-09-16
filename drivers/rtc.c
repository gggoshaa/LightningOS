#include "rtc.h"
#include "io.h"

#define CMOS_ADDRESS 0x70
#define CMOS_DATA    0x71

static uint8_t cmos_read(uint8_t reg)
{
    outb(CMOS_ADDRESS, reg);
    return inb(CMOS_DATA);
}

static int update_in_progress(void)
{
    return cmos_read(0x0A) & 0x80;
}

static uint8_t from_bcd(uint8_t value)
{
    return (uint8_t)((value & 0x0F) + ((value >> 4) * 10));
}

void rtc_read(rtc_time_t *out)
{
    uint8_t second, minute, hour, day, month, year, century, registerB;

    while (update_in_progress())
        ;

    second = cmos_read(0x00);
    minute = cmos_read(0x02);
    hour   = cmos_read(0x04);
    day    = cmos_read(0x07);
    month  = cmos_read(0x08);
    year   = cmos_read(0x09);
    century = cmos_read(0x32);
    registerB = cmos_read(0x0B);

    if (!(registerB & 0x04)) {          /* values are stored in BCD */
        second = from_bcd(second);
        minute = from_bcd(minute);
        hour   = (uint8_t)(from_bcd((uint8_t)(hour & 0x7F)) | (hour & 0x80));
        day    = from_bcd(day);
        month  = from_bcd(month);
        year   = from_bcd(year);
        century = from_bcd(century);
    }

    if (!(registerB & 0x02) && (hour & 0x80))   /* 12 hour clock, PM flag set */
        hour = (uint8_t)(((hour & 0x7F) + 12) % 24);

    out->second = second;
    out->minute = minute;
    out->hour   = hour;
    out->day    = day;
    out->month  = month;
    out->year   = (century >= 19 && century <= 21)
                      ? (uint16_t)(century * 100 + year)
                      : (uint16_t)(2000 + year);
}
