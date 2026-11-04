// Yamaha V9938 (MSX2) and V9958 (MSX2+) video display processors.
#ifndef V9938_H
#define V9938_H

#include <stdbool.h>
#include <stdint.h>

#define V9938_WIDTH  544          // 512 + 2 * 16 border, every pixel of 256-wide modes doubled
#define V9938_LINES  240          // rendered scanlines (top border, 192/212 display, bottom border)
#define V9938_HEIGHT (V9938_LINES * 2)   // each scanline drawn twice
#define V9938_FIRST_LINE 3        // first scanline of the frame shown in the framebuffer

typedef struct v9938_cmd {
    int SX, SY, DX, DY, TX, TY, NX, NY, MX;
    int ASX, ADX, ANX;
    uint8_t CL, LO, CM;
    int MXS, MXD;
} v9938_cmd;

typedef struct v9938 {
    uint8_t vram[0x30000];        // 128 KB plus the (absent) expansion RAM area
    uint8_t reg[64];
    uint8_t stat[10];
    uint8_t pal_reg[32];
    uint32_t pal16[16];
    uint32_t pal256[256];
    bool v9958;

    uint8_t read_ahead;
    uint16_t address_latch;
    bool cmd_write_first, pal_write_first;
    uint8_t cmd_write, pal_write;

    int mode;
    int offset_x, offset_y, visible_y;
    int scanline;
    int scanline_start, scanline_max;
    bool blink;
    int blink_count;
    bool irq;
    int hpos;                     // CPU cycle within the current line, set by the machine

    int engine;                   // active command (CM), 0 when idle
    int ops_count;
    v9938_cmd mmc;

    uint32_t fb[V9938_WIDTH * V9938_HEIGHT];
} v9938;

void v9938_init(v9938 *v, bool v9958);
void v9938_reset(v9938 *v);
uint8_t v9938_read(v9938 *v, int port);            // port & 3: 0 VRAM, 1 status
void v9938_write(v9938 *v, int port, uint8_t value);  // 0 VRAM, 1 command, 2 palette, 3 indirect register
/// Starts the next scanline (0-261): flags, interrupts, command engine, rendering.
void v9938_line(v9938 *v);

#endif
