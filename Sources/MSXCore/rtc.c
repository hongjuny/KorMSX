// RP5C01. The time registers report the host's local time; writes to them
// are ignored, so the MSX clock always follows the Mac. Battery RAM (banks 2
// and 3: screen width, colours, beep, title...) lives only in memory.
#include "rtc.h"

#include <stdbool.h>
#include <string.h>
#include <time.h>

void rtc_reset(rtc *r) {
    memset(r, 0, sizeof *r);
    r->regs[1][10] = 1;     // 24-hour mode
}

void rtc_address(rtc *r, uint8_t value) {
    r->latch = value & 0x0f;
}

void rtc_write(rtc *r, uint8_t value) {
    value &= 0x0f;
    switch (r->latch) {
    case 13: r->mode = value; break;
    case 14: case 15: break;     // test and reset
    default:
        if ((r->mode & 3) != 0) r->regs[r->mode & 3][r->latch] = value;
        break;
    }
}

static uint8_t time_nibble(rtc *r, int reg) {
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    int hour = t.tm_hour;
    bool pm = false;
    if (!(r->regs[1][10] & 1)) {    // 12-hour mode: bit 1 of hour10 is PM
        pm = hour >= 12;
        hour %= 12;
    }
    int year = (t.tm_year + 1900 - 1980) % 100;
    switch (reg) {
    case 0: return (uint8_t)(t.tm_sec % 10);
    case 1: return (uint8_t)(t.tm_sec / 10);
    case 2: return (uint8_t)(t.tm_min % 10);
    case 3: return (uint8_t)(t.tm_min / 10);
    case 4: return (uint8_t)(hour % 10);
    case 5: return (uint8_t)(hour / 10 | (pm ? 2 : 0));
    case 6: return (uint8_t)t.tm_wday;
    case 7: return (uint8_t)(t.tm_mday % 10);
    case 8: return (uint8_t)(t.tm_mday / 10);
    case 9: return (uint8_t)((t.tm_mon + 1) % 10);
    case 10: return (uint8_t)((t.tm_mon + 1) / 10);
    case 11: return (uint8_t)(year % 10);
    default: return (uint8_t)(year / 10);
    }
}

uint8_t rtc_read(rtc *r) {
    uint8_t v;
    switch (r->latch) {
    case 13: v = r->mode; break;
    case 14: case 15: v = 0; break;
    default:
        v = (r->mode & 3) == 0 ? time_nibble(r, r->latch) : r->regs[r->mode & 3][r->latch];
        break;
    }
    return (uint8_t)(0xf0 | (v & 0x0f));
}
