// TMS9918A. Register handling, the per-line renderer and the sprite engine
// (4 per line, 5th-sprite flag, collisions, early clock) follow MAME's
// tms9928a.cpp; this version renders into a fixed 288x224 framebuffer.
#include "vdp.h"

#include <string.h>

#define RGB(r, g, b) ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | 0xff000000u)

// Palette from MAME (R. Nabet's derivation from the TI datasheet).
static const uint32_t palette[16] = {
    RGB(0, 0, 0),       RGB(0, 0, 0),       RGB(33, 200, 66),   RGB(94, 220, 120),
    RGB(84, 85, 237),   RGB(125, 118, 252), RGB(212, 82, 77),   RGB(66, 235, 245),
    RGB(252, 85, 84),   RGB(255, 121, 120), RGB(212, 193, 84),  RGB(230, 206, 128),
    RGB(33, 176, 59),   RGB(201, 91, 186),  RGB(204, 204, 204), RGB(255, 255, 255),
};

void vdp_reset(vdp *v) {
    memset(v->regs, 0, sizeof v->regs);
    v->status = 0;
    v->read_ahead = 0;
    v->addr = 0;
    v->latch = false;
    v->fifth = 0;
    for (int i = 0; i < VDP_WIDTH * VDP_HEIGHT; i++) v->fb[i] = palette[1];
}

bool vdp_irq(const vdp *v) {
    return (v->status & 0x80) && (v->regs[1] & 0x20);
}

uint8_t vdp_read(vdp *v, int port) {
    v->latch = false;
    if (port & 1) {
        uint8_t s = v->status;
        v->status = v->fifth & 0x1f;   // clears F, 5S and C
        return s;
    }
    uint8_t data = v->read_ahead;
    v->read_ahead = v->vram[v->addr];
    v->addr = (v->addr + 1) & 0x3fff;
    return data;
}

void vdp_write(vdp *v, int port, uint8_t value) {
    if (!(port & 1)) {
        v->vram[v->addr] = value;
        v->read_ahead = value;
        v->addr = (v->addr + 1) & 0x3fff;
        v->latch = false;
        return;
    }
    if (!v->latch) {
        v->addr = (v->addr & 0x3f00) | value;
        v->latch = true;
        return;
    }
    v->latch = false;
    v->addr = (uint16_t)(((value << 8) | (v->addr & 0xff)) & 0x3fff);
    if (value & 0x80) {
        static const uint8_t mask[8] = { 0x03, 0xfb, 0x0f, 0xff, 0x07, 0x7f, 0x07, 0xff };
        int reg = value & 7;
        v->regs[reg] = (uint8_t)(v->addr & 0xff) & mask[reg];
    } else if (!(value & 0x40)) {
        v->read_ahead = v->vram[v->addr];
        v->addr = (v->addr + 1) & 0x3fff;
    }
}

void vdp_vblank(vdp *v) {
    v->status |= 0x80;
}

static void draw_sprites(vdp *v, int y, uint32_t *p) {
    const uint8_t *r = v->regs;
    const uint8_t *vram = v->vram;
    uint16_t attr = (uint16_t)((r[5] & 0x7f) * 128);
    uint16_t pgen = (uint16_t)((r[6] & 0x07) * 2048);
    int size = (r[1] & 0x02) ? 16 : 8;
    int mag = r[1] & 0x01;
    int height = size << mag;
    uint8_t drawn[32 + 256 + 32];
    memset(drawn, 0, sizeof drawn);
    int count = 0;
    bool fifth = false;

    for (int n = 0; n < 32; n++) {
        const uint8_t *s = vram + attr + n * 4;
        int sy = s[0];
        v->fifth = (uint8_t)n;
        if (sy == 208) break;
        if (sy > 0xe0) sy -= 256;
        sy++;                            // Y = 255 is the first line
        if (y < sy || y >= sy + height) continue;
        if (++count == 5) {
            fifth = true;
            break;
        }
        int sx = s[1];
        uint8_t code = (uint8_t)(size == 16 ? (s[2] & 0xfc) : s[2]);
        uint8_t color = s[3];
        uint16_t pat = (uint16_t)(pgen + code * 8 + ((y - sy) >> mag));
        if (color & 0x80) sx -= 32;      // early clock
        color &= 0x0f;
        uint8_t bits = vram[pat & 0x3fff];
        for (int half = 0; half < size; half += 8) {
            for (int i = 0; i < 8; i++, bits <<= 1) {
                if (!(bits & 0x80)) continue;
                for (int z = 0; z <= mag; z++) {
                    int xi = sx + (i << mag) + z + 32;
                    if (xi < 32 || xi >= 32 + 256) continue;
                    if (drawn[xi]) v->status |= 0x20;   // collision
                    drawn[xi] |= 1;
                    if (color && !(drawn[xi] & 2)) {
                        drawn[xi] |= 2;
                        p[xi - 32] = palette[color];
                    }
                }
            }
            bits = vram[(pat + 16) & 0x3fff];
            sx += 8 << mag;
        }
    }
    if (!(v->status & 0x40)) {
        v->status = (uint8_t)((v->status & 0xe0) | v->fifth);
        if (fifth && !(v->status & 0x80)) v->status |= 0x40;
    }
}

void vdp_render_line(vdp *v, int line) {
    int fy = line + VDP_BORDER_Y;
    if (fy < 0 || fy >= VDP_HEIGHT) return;
    const uint8_t *r = v->regs;
    const uint8_t *vram = v->vram;
    uint32_t *row = v->fb + fy * VDP_WIDTH;
    uint8_t back = r[7] & 0x0f;
    uint32_t bgc = palette[back];

    if (line < 0 || line >= 192 || !(r[1] & 0x40)) {
        for (int i = 0; i < VDP_WIDTH; i++) row[i] = bgc;
        return;
    }
    for (int i = 0; i < VDP_BORDER_X; i++) row[i] = row[VDP_WIDTH - 1 - i] = bgc;
    uint32_t *p = row + VDP_BORDER_X;
    int y = line;
    // mode: bit0 = M1 (text), bit1 = M3 (graphic 2), bit2 = M2 (multicolour)
    int mode = (r[0] & 0x02) | ((r[1] & 0x10) >> 4) | ((r[1] & 0x08) >> 1);
    uint16_t name = (uint16_t)((r[2] & 0x0f) * 1024);
    uint16_t colour, pattern;
    uint16_t colourmask = (uint16_t)(((r[3] & 0x7f) << 3) | 7);
    uint16_t patternmask = (uint16_t)(((r[4] & 3) << 8) | (colourmask & 0xff));
    if (r[0] & 0x02) {
        colour = (uint16_t)((r[3] & 0x80) * 64);
        pattern = (uint16_t)((r[4] & 0x04) * 2048);
    } else {
        colour = (uint16_t)(r[3] * 64);
        pattern = (uint16_t)((r[4] & 0x07) * 2048);
    }
#define PEN(c) ((c) ? palette[c] : bgc)

    switch (mode) {
    case 0: {   // Graphic 1
        uint16_t a = (uint16_t)(name + ((y & 0xf8) << 2));
        for (int x = 0; x < 256; x += 8, a++) {
            uint8_t ch = vram[a];
            uint8_t bits = vram[(pattern + ch * 8 + (y & 7)) & 0x3fff];
            uint8_t col = vram[(colour + (ch >> 3)) & 0x3fff];
            uint32_t fg = PEN(col >> 4), bg = PEN(col & 15);
            for (int i = 0; i < 8; i++, bits <<= 1) p[x + i] = (bits & 0x80) ? fg : bg;
        }
        break;
    }
    case 1: case 3: {   // Text 1 (and Text 1 with the Graphic 2 tables): 6 + 240 + 10 pixels
        uint16_t a = (uint16_t)(name + (y >> 3) * 40);
        uint32_t fg = PEN(r[7] >> 4), bg = bgc;
        for (int x = 0; x < 6; x++) p[x] = bg;
        for (int x = 6; x < 246; x += 6, a++) {
            unsigned ch = vram[a & 0x3fff];
            if (mode == 3) ch = (ch + ((y >> 6) << 8)) & patternmask;
            uint8_t bits = vram[(pattern + ch * 8 + (y & 7)) & 0x3fff];
            for (int i = 0; i < 6; i++, bits <<= 1) p[x + i] = (bits & 0x80) ? fg : bg;
        }
        for (int x = 246; x < 256; x++) p[x] = bg;
        break;
    }
    case 2: {   // Graphic 2
        uint16_t a = (uint16_t)(name + (y >> 3) * 32);
        for (int x = 0; x < 256; x += 8, a++) {
            unsigned ch = vram[a] + ((y >> 6) << 8);
            uint8_t bits = vram[(pattern + ((ch & patternmask) << 3) + (y & 7)) & 0x3fff];
            uint8_t col = vram[(colour + ((ch & colourmask) << 3) + (y & 7)) & 0x3fff];
            uint32_t fg = PEN(col >> 4), bg = PEN(col & 15);
            for (int i = 0; i < 8; i++, bits <<= 1) p[x + i] = (bits & 0x80) ? fg : bg;
        }
        break;
    }
    case 4: case 6: {   // Multicolour (and its Graphic 2 table variant)
        uint16_t a = (uint16_t)(name + (y >> 3) * 32);
        for (int x = 0; x < 256; x += 8, a++) {
            uint8_t ch = vram[a];
            uint8_t col;
            if (mode == 4)
                col = vram[(pattern + ch * 8 + ((y >> 2) & 7)) & 0x3fff];
            else
                col = vram[(pattern + (((ch + ((y >> 2) & 7) + ((y >> 6) << 8)) & patternmask) << 3)) & 0x3fff];
            uint32_t fg = PEN(col >> 4), bg = PEN(col & 15);
            p[x] = p[x + 1] = p[x + 2] = p[x + 3] = fg;
            p[x + 4] = p[x + 5] = p[x + 6] = p[x + 7] = bg;
        }
        break;
    }
    default: {  // undefined combinations show vertical bars in text layout
        uint32_t fg = PEN(r[7] >> 4), bg = bgc;
        for (int x = 0; x < 6; x++) p[x] = bg;
        for (int x = 6; x < 246; x += 6) {
            p[x] = p[x + 1] = p[x + 2] = p[x + 3] = fg;
            p[x + 4] = p[x + 5] = bg;
        }
        for (int x = 246; x < 256; x++) p[x] = bg;
        break;
    }
    }
#undef PEN

    if ((r[1] & 0x50) != 0x40)
        v->fifth = 31;   // text mode or blank: no sprites
    else
        draw_sprites(v, y, p);
}
