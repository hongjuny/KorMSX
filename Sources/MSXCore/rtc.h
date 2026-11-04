// Ricoh RP5C01 real-time clock with 26 nibbles of battery RAM (MSX2 ports B4h/B5h).
#ifndef RTC_H
#define RTC_H

#include <stdint.h>

typedef struct rtc {
    uint8_t latch;          // selected register (0-15)
    uint8_t mode;           // register 13: bank select, timer/alarm enable
    uint8_t regs[4][13];    // bank 0 time (unused, read from the host clock), 1 alarm/config, 2-3 RAM
} rtc;

void rtc_reset(rtc *r);
void rtc_address(rtc *r, uint8_t value);
void rtc_write(rtc *r, uint8_t value);
uint8_t rtc_read(rtc *r);

#endif
