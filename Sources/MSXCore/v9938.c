// V9938/V9958, ported from MAME's v9938.cpp (BSD-3-Clause, Aaron Giles,
// Nathan Woods; command engine by Alex Wulms). Rendering, sprites, the
// G6/G7 VRAM interleave, status registers and the command engine follow
// that code; the frame is driven one scanline at a time by v9938_line().
#include "v9938.h"

#include <string.h>

enum {
    MODE_TEXT1, MODE_MULTI, MODE_GRAPHIC1, MODE_GRAPHIC2, MODE_GRAPHIC3, MODE_GRAPHIC4,
    MODE_GRAPHIC5, MODE_GRAPHIC6, MODE_GRAPHIC7, MODE_TEXT2, MODE_UNKNOWN,
};

#define EXPMEM_OFFSET 0x20000
#define VTOTAL 262
#define VBLANK_START 251

static uint32_t yjk[0x20000];
static bool yjk_ready;

static inline uint32_t rgb(int r, int g, int b) {
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | 0xff000000u;
}
static inline int pal3bit(int x) { x &= 7; return (x << 5) | (x << 2) | (x >> 1); }
static inline int pal5bit(int x) { x &= 31; return (x << 3) | (x >> 2); }

static inline uint32_t pen16(v9938 *v, int i) { return v->pal16[i & 15]; }
static inline uint32_t pen256(v9938 *v, int i) { return v->pal256[i & 255]; }
static inline uint8_t vr(v9938 *v, int a) { return v->vram[a & 0x1ffff]; }

static inline int position_offset(uint8_t value) {
    value &= 0x0f;
    return value < 8 ? -value : 16 - value;
}

static void build_yjk(void) {
    for (int y = 0; y < 32; y++)
        for (int k = 0; k < 64; k++)
            for (int j = 0; j < 64; j++) {
                int k0 = k >= 32 ? k - 64 : k, j0 = j >= 32 ? j - 64 : j;
                int r = y + j0, b = (y * 5 - 2 * j0 - k0) / 4, g = y + k0;
                r = r < 0 ? 0 : r > 31 ? 31 : r;
                g = g < 0 ? 0 : g > 31 ? 31 : g;
                b = b < 0 ? 0 : b > 31 ? 31 : b;
                yjk[y | j << 5 | k << 11] = rgb(pal5bit(r), pal5bit(g), pal5bit(b));
            }
    yjk_ready = true;
}

// ---- memory ------------------------------------------------------------------

// In G6/G7 the two 64 KB banks are interleaved byte by byte.
static void vram_write(v9938 *v, int offset, uint8_t data) {
    if (v->mode == MODE_GRAPHIC6 || v->mode == MODE_GRAPHIC7)
        offset = ((offset & 1) << 16) | (offset >> 1);
    v->vram[offset & 0x1ffff] = data;
}

static uint8_t vram_read(v9938 *v, int offset) {
    if (v->mode == MODE_GRAPHIC6 || v->mode == MODE_GRAPHIC7)
        offset = ((offset & 1) << 16) | (offset >> 1);
    return v->vram[offset & 0x1ffff];
}

static void check_int(v9938 *v) {
    v->irq = ((v->reg[1] & 0x20) && (v->stat[0] & 0x80)) || ((v->stat[1] & 0x01) && (v->reg[0] & 0x10));
}

// ---- palette -------------------------------------------------------------------

static void reset_palette(v9938 *v) {
    // V9938 Technical Data Book p.148, in G-R-B order
    static const uint8_t pal[16 * 3] = {
        0, 0, 0, 0, 0, 0, 6, 1, 1, 7, 3, 3, 1, 1, 7, 3, 2, 7, 1, 5, 1, 6, 2, 7,
        1, 7, 1, 3, 7, 3, 6, 6, 1, 6, 6, 4, 4, 1, 1, 2, 6, 5, 5, 5, 5, 7, 7, 7,
    };
    for (int i = 0; i < 16; i++) {
        v->pal_reg[i * 2] = (uint8_t)(pal[i * 3 + 1] << 4 | pal[i * 3 + 2]);
        v->pal_reg[i * 2 + 1] = pal[i * 3];
        v->pal16[i] = rgb(pal3bit(pal[i * 3 + 1]), pal3bit(pal[i * 3]), pal3bit(pal[i * 3 + 2]));
    }
    for (int i = 0; i < 256; i++) {
        int blue = (i << 1) & 6;
        if (blue == 6) blue++;
        v->pal256[i] = rgb(pal3bit((i & 0x1c) >> 2), pal3bit((i & 0xe0) >> 5), pal3bit(blue));
    }
}

static void palette_w(v9938 *v, uint8_t data) {
    if (!v->pal_write_first) {
        v->pal_write = data;
        v->pal_write_first = true;
        return;
    }
    int i = v->reg[16] & 15;
    v->pal_reg[i * 2] = v->pal_write & 0x77;
    v->pal_reg[i * 2 + 1] = data & 0x07;
    v->pal16[i] = rgb(pal3bit((v->pal_write & 0x70) >> 4), pal3bit(data & 7), pal3bit(v->pal_write & 7));
    v->reg[16] = (v->reg[16] + 1) & 15;
    v->pal_write_first = false;
}

// ---- mode selection ------------------------------------------------------------

static void set_mode(v9938 *v) {
    static const uint8_t codes[10] = { 0x02, 0x01, 0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x1c, 0x0a };
    int n = ((v->reg[0] & 0x0e) << 1) | ((v->reg[1] & 0x18) >> 3);
    int mode = MODE_UNKNOWN;
    for (int i = 0; i < 10; i++)
        if (codes[i] == n) { mode = i; break; }
    if (v->engine && v->mode != mode) {   // a mode change aborts a running command
        v->engine = 0;
        v->stat[2] &= 0xfe;
    }
    v->mode = mode;
}

// ---- command engine ------------------------------------------------------------

static const uint8_t Mask[4] = { 0x0f, 0x03, 0x0f, 0xff };
static const int PPB[4] = { 2, 4, 2, 1 };
static const int PPL[4] = { 256, 512, 512, 256 };

static const int srch_timing[8] = { 818, 1025, 818, 830, 696, 854, 696, 684 };
static const int line_timing[8] = { 1063, 1259, 1063, 1161, 904, 1026, 904, 953 };
static const int hmmv_timing[8] = { 439, 549, 439, 531, 366, 439, 366, 427 };
static const int lmmv_timing[8] = { 873, 1135, 873, 1056, 732, 909, 732, 854 };
static const int ymmm_timing[8] = { 586, 952, 586, 610, 488, 720, 488, 500 };
static const int hmmm_timing[8] = { 818, 1111, 818, 854, 684, 879, 684, 708 };
static const int lmmm_timing[8] = { 1160, 1599, 1160, 1172, 964, 1257, 964, 977 };

#define VDP_VRMP5(MX, X, Y) ((!MX) ? ((((Y)&1023)<<7) + (((X)&255)>>1)) : (EXPMEM_OFFSET + (((Y)&511)<<7) + (((X)&255)>>1)))
#define VDP_VRMP6(MX, X, Y) ((!MX) ? ((((Y)&1023)<<7) + (((X)&511)>>2)) : (EXPMEM_OFFSET + (((Y)&511)<<7) + (((X)&511)>>2)))
#define VDP_VRMP7(MX, X, Y) ((!MX) ? ((((X)&2)<<15) + (((Y)&511)<<7) + (((X)&511)>>2)) : (EXPMEM_OFFSET + (((Y)&511)<<7) + (((X)&511)>>2)))
#define VDP_VRMP8(MX, X, Y) ((!MX) ? ((((X)&1)<<16) + (((Y)&511)<<7) + (((X)>>1)&127)) : (EXPMEM_OFFSET + (((Y)&511)<<7) + (((X)>>1)&127)))

static inline int vrmp(int M, int MX, int X, int Y) {
    switch (M) {
    case 0: return VDP_VRMP5(MX, X, Y);
    case 1: return VDP_VRMP6(MX, X, Y);
    case 2: return VDP_VRMP7(MX, X, Y);
    default: return VDP_VRMP8(MX, X, Y);
    }
}

static inline uint8_t point5(v9938 *v, int MXS, int SX, int SY) { return (v->vram[VDP_VRMP5(MXS, SX, SY)] >> (((~SX) & 1) << 2)) & 15; }
static inline uint8_t point6(v9938 *v, int MXS, int SX, int SY) { return (v->vram[VDP_VRMP6(MXS, SX, SY)] >> (((~SX) & 3) << 1)) & 3; }
static inline uint8_t point7(v9938 *v, int MXS, int SX, int SY) { return (v->vram[VDP_VRMP7(MXS, SX, SY)] >> (((~SX) & 1) << 2)) & 15; }
static inline uint8_t point8(v9938 *v, int MXS, int SX, int SY) { return v->vram[VDP_VRMP8(MXS, SX, SY)]; }

static inline uint8_t point(v9938 *v, int SM, int MXS, int SX, int SY) {
    switch (SM) {
    case 0: return point5(v, MXS, SX, SY);
    case 1: return point6(v, MXS, SX, SY);
    case 2: return point7(v, MXS, SX, SY);
    default: return point8(v, MXS, SX, SY);
    }
}

static inline void pset_low(v9938 *v, int addr, uint8_t CL, uint8_t M, uint8_t OP) {
    uint8_t val = v->vram[addr];
    switch (OP) {
    case 0: val = (val & M) | CL; break;
    case 1: val = val & (CL | M); break;
    case 2: val |= CL; break;
    case 3: val ^= CL; break;
    case 4: val = (val & M) | ~(CL | M); break;
    case 8: if (CL) val = (val & M) | CL; break;
    case 9: if (CL) val = val & (CL | M); break;
    case 10: if (CL) val |= CL; break;
    case 11: if (CL) val ^= CL; break;
    case 12: if (CL) val = (val & M) | ~(CL | M); break;
    default: break;
    }
    v->vram[addr] = val;
}

static inline void pset5(v9938 *v, int MXD, int DX, int DY, uint8_t CL, uint8_t OP) {
    uint8_t SH = ((~DX) & 1) << 2;
    pset_low(v, VDP_VRMP5(MXD, DX, DY), (uint8_t)(CL << SH), (uint8_t)~(15 << SH), OP);
}
static inline void pset6(v9938 *v, int MXD, int DX, int DY, uint8_t CL, uint8_t OP) {
    uint8_t SH = ((~DX) & 3) << 1;
    pset_low(v, VDP_VRMP6(MXD, DX, DY), (uint8_t)(CL << SH), (uint8_t)~(3 << SH), OP);
}
static inline void pset7(v9938 *v, int MXD, int DX, int DY, uint8_t CL, uint8_t OP) {
    uint8_t SH = ((~DX) & 1) << 2;
    pset_low(v, VDP_VRMP7(MXD, DX, DY), (uint8_t)(CL << SH), (uint8_t)~(15 << SH), OP);
}
static inline void pset8(v9938 *v, int MXD, int DX, int DY, uint8_t CL, uint8_t OP) {
    pset_low(v, VDP_VRMP8(MXD, DX, DY), CL, 0, OP);
}
static inline void pset(v9938 *v, int SM, int MXD, int DX, int DY, uint8_t CL, uint8_t OP) {
    switch (SM) {
    case 0: pset5(v, MXD, DX, DY, CL, OP); break;
    case 1: pset6(v, MXD, DX, DY, CL, OP); break;
    case 2: pset7(v, MXD, DX, DY, CL, OP); break;
    default: pset8(v, MXD, DX, DY, CL, OP); break;
    }
}

static int timing(v9938 *v, const int *t) {
    return t[((v->reg[1] >> 6) & 1) | (v->reg[8] & 2) | ((v->reg[9] << 1) & 4)];
}

static inline int screen_mode_index(v9938 *v) {
    return (v->mode >= MODE_GRAPHIC4 && v->mode <= MODE_GRAPHIC7) ? v->mode - MODE_GRAPHIC4 : 0;
}

#define pre_loop while ((cnt -= delta) > 0) {
#define post_loop }

#define post__x_y(MX) \
    if (!--ANX || ((ADX += TX) & MX)) { \
        if (!(--NY & 1023) || (DY += TY) == -1) break; \
        else { ADX = DX; ANX = NX; } \
    } \
    post_loop

#define post__xyy(MX) \
    if ((ADX += TX) & MX) { \
        if (!(--NY & 1023) || (SY += TY) == -1 || (DY += TY) == -1) break; \
        else ADX = DX; \
    } \
    post_loop

#define post_xxyy(MX) \
    if (!--ANX || ((ASX += TX) & MX) || ((ADX += TX) & MX)) { \
        if (!(--NY & 1023) || (SY += TY) == -1 || (DY += TY) == -1) break; \
        else { ASX = SX; ADX = DX; ANX = NX; } \
    } \
    post_loop

static void finish(v9938 *v) {
    v->stat[2] &= 0xfe;
    v->engine = 0;
}

static void srch_engine(v9938 *v) {
    int SX = v->mmc.SX, SY = v->mmc.SY, TX = v->mmc.TX, ANX = v->mmc.ANX;
    uint8_t CL = v->mmc.CL;
    int MXD = v->mmc.MXD;
    int delta = timing(v, srch_timing), cnt = v->ops_count;
#define post_srch(MX) \
    { v->stat[2] |= 0x10; break; } \
    if ((SX += TX) & MX) { v->stat[2] &= 0xef; break; }
    switch (v->mode) {
    default:
    case MODE_GRAPHIC4: pre_loop if ((point5(v, MXD, SX, SY) == CL) ^ ANX) post_srch(256) post_loop break;
    case MODE_GRAPHIC5: pre_loop if ((point6(v, MXD, SX, SY) == CL) ^ ANX) post_srch(512) post_loop break;
    case MODE_GRAPHIC6: pre_loop if ((point7(v, MXD, SX, SY) == CL) ^ ANX) post_srch(512) post_loop break;
    case MODE_GRAPHIC7: pre_loop if ((point8(v, MXD, SX, SY) == CL) ^ ANX) post_srch(256) post_loop break;
    }
#undef post_srch
    if ((v->ops_count = cnt) > 0) {
        finish(v);
        v->stat[8] = SX & 0xff;
        v->stat[9] = (uint8_t)((SX >> 8) | 0xfe);
    } else {
        v->mmc.SX = SX;
    }
}

static void line_engine(v9938 *v) {
    int DX = v->mmc.DX, DY = v->mmc.DY, TX = v->mmc.TX, TY = v->mmc.TY, NX = v->mmc.NX, NY = v->mmc.NY;
    int ASX = v->mmc.ASX, ADX = v->mmc.ADX;
    uint8_t CL = v->mmc.CL, LO = v->mmc.LO;
    int MXD = v->mmc.MXD;
    int delta = timing(v, line_timing), cnt = v->ops_count;
#define post_linexmaj(MX) \
    DX += TX; \
    if ((ASX -= NY) < 0) { ASX += NX; DY += TY; } \
    ASX &= 1023; \
    if (ADX++ == NX || (DX & MX)) break; \
    post_loop
#define post_lineymaj(MX) \
    DY += TY; \
    if ((ASX -= NY) < 0) { ASX += NX; DX += TX; } \
    ASX &= 1023; \
    if (ADX++ == NX || (DX & MX)) break; \
    post_loop
    if ((v->reg[45] & 0x01) == 0) {
        switch (v->mode) {
        default:
        case MODE_GRAPHIC4: pre_loop pset5(v, MXD, DX, DY, CL, LO); post_linexmaj(256) break;
        case MODE_GRAPHIC5: pre_loop pset6(v, MXD, DX, DY, CL, LO); post_linexmaj(512) break;
        case MODE_GRAPHIC6: pre_loop pset7(v, MXD, DX, DY, CL, LO); post_linexmaj(512) break;
        case MODE_GRAPHIC7: pre_loop pset8(v, MXD, DX, DY, CL, LO); post_linexmaj(256) break;
        }
    } else {
        switch (v->mode) {
        default:
        case MODE_GRAPHIC4: pre_loop pset5(v, MXD, DX, DY, CL, LO); post_lineymaj(256) break;
        case MODE_GRAPHIC5: pre_loop pset6(v, MXD, DX, DY, CL, LO); post_lineymaj(512) break;
        case MODE_GRAPHIC6: pre_loop pset7(v, MXD, DX, DY, CL, LO); post_lineymaj(512) break;
        case MODE_GRAPHIC7: pre_loop pset8(v, MXD, DX, DY, CL, LO); post_lineymaj(256) break;
        }
    }
#undef post_linexmaj
#undef post_lineymaj
    if ((v->ops_count = cnt) > 0) {
        finish(v);
        v->reg[38] = DY & 0xff;
        v->reg[39] = (DY >> 8) & 0x03;
    } else {
        v->mmc.DX = DX;
        v->mmc.DY = DY;
        v->mmc.ASX = ASX;
        v->mmc.ADX = ADX;
    }
}

static void lmmv_engine(v9938 *v) {
    int DX = v->mmc.DX, DY = v->mmc.DY, TX = v->mmc.TX, TY = v->mmc.TY, NX = v->mmc.NX, NY = v->mmc.NY;
    int ADX = v->mmc.ADX, ANX = v->mmc.ANX;
    uint8_t CL = v->mmc.CL, LO = v->mmc.LO;
    int MXD = v->mmc.MXD;
    int delta = timing(v, lmmv_timing), cnt = v->ops_count;
    switch (v->mode) {
    default:
    case MODE_GRAPHIC4: pre_loop pset5(v, MXD, ADX, DY, CL, LO); post__x_y(256) break;
    case MODE_GRAPHIC5: pre_loop pset6(v, MXD, ADX, DY, CL, LO); post__x_y(512) break;
    case MODE_GRAPHIC6: pre_loop pset7(v, MXD, ADX, DY, CL, LO); post__x_y(512) break;
    case MODE_GRAPHIC7: pre_loop pset8(v, MXD, ADX, DY, CL, LO); post__x_y(256) break;
    }
    if ((v->ops_count = cnt) > 0) {
        finish(v);
        if (!NY) DY += TY;
        v->reg[38] = DY & 0xff;
        v->reg[39] = (DY >> 8) & 0x03;
        v->reg[42] = NY & 0xff;
        v->reg[43] = (NY >> 8) & 0x03;
    } else {
        v->mmc.DY = DY;
        v->mmc.NY = NY;
        v->mmc.ANX = ANX;
        v->mmc.ADX = ADX;
    }
}

static void lmmm_engine(v9938 *v) {
    int SX = v->mmc.SX, SY = v->mmc.SY, DX = v->mmc.DX, DY = v->mmc.DY, TX = v->mmc.TX, TY = v->mmc.TY;
    int NX = v->mmc.NX, NY = v->mmc.NY, ASX = v->mmc.ASX, ADX = v->mmc.ADX, ANX = v->mmc.ANX;
    uint8_t LO = v->mmc.LO;
    int MXS = v->mmc.MXS, MXD = v->mmc.MXD;
    int delta = timing(v, lmmm_timing), cnt = v->ops_count;
    switch (v->mode) {
    default:
    case MODE_GRAPHIC4: pre_loop pset5(v, MXD, ADX, DY, point5(v, MXS, ASX, SY), LO); post_xxyy(256) break;
    case MODE_GRAPHIC5: pre_loop pset6(v, MXD, ADX, DY, point6(v, MXS, ASX, SY), LO); post_xxyy(512) break;
    case MODE_GRAPHIC6: pre_loop pset7(v, MXD, ADX, DY, point7(v, MXS, ASX, SY), LO); post_xxyy(512) break;
    case MODE_GRAPHIC7: pre_loop pset8(v, MXD, ADX, DY, point8(v, MXS, ASX, SY), LO); post_xxyy(256) break;
    }
    if ((v->ops_count = cnt) > 0) {
        finish(v);
        if (!NY) { SY += TY; DY += TY; }
        else if (SY == -1) DY += TY;
        v->reg[42] = NY & 0xff; v->reg[43] = (NY >> 8) & 0x03;
        v->reg[34] = SY & 0xff; v->reg[35] = (SY >> 8) & 0x03;
        v->reg[38] = DY & 0xff; v->reg[39] = (DY >> 8) & 0x03;
    } else {
        v->mmc.SY = SY; v->mmc.DY = DY; v->mmc.NY = NY;
        v->mmc.ANX = ANX; v->mmc.ASX = ASX; v->mmc.ADX = ADX;
    }
}

static void lmcm_engine(v9938 *v) {
    if ((v->stat[2] & 0x80) == 0x80) return;
    v9938_cmd *c = &v->mmc;
    v->stat[7] = v->reg[44] = point(v, screen_mode_index(v), c->MXS, c->ASX, c->SY);
    v->ops_count -= timing(v, lmmv_timing);
    v->stat[2] |= 0x80;
    if (!--c->ANX || ((c->ASX += c->TX) & c->MX)) {
        if (!(--c->NY & 1023) || (c->SY += c->TY) == -1) {
            finish(v);
            if (!c->NY) c->DY += c->TY;
            v->reg[42] = c->NY & 0xff; v->reg[43] = (c->NY >> 8) & 0x03;
            v->reg[34] = c->SY & 0xff; v->reg[35] = (c->SY >> 8) & 0x03;
        } else {
            c->ASX = c->SX;
            c->ANX = c->NX;
        }
    }
}

static void lmmc_engine(v9938 *v) {
    if ((v->stat[2] & 0x80) == 0x80) return;
    v9938_cmd *c = &v->mmc;
    int SM = screen_mode_index(v);
    v->stat[7] = v->reg[44] &= Mask[SM];
    pset(v, SM, c->MXD, c->ADX, c->DY, v->reg[44], c->LO);
    v->ops_count -= timing(v, lmmv_timing);
    v->stat[2] |= 0x80;
    if (!--c->ANX || ((c->ADX += c->TX) & c->MX)) {
        if (!(--c->NY & 1023) || (c->DY += c->TY) == -1) {
            finish(v);
            if (!c->NY) c->DY += c->TY;
            v->reg[42] = c->NY & 0xff; v->reg[43] = (c->NY >> 8) & 0x03;
            v->reg[38] = c->DY & 0xff; v->reg[39] = (c->DY >> 8) & 0x03;
        } else {
            c->ADX = c->DX;
            c->ANX = c->NX;
        }
    }
}

static void hmmv_engine(v9938 *v) {
    int DX = v->mmc.DX, DY = v->mmc.DY, TX = v->mmc.TX, TY = v->mmc.TY, NX = v->mmc.NX, NY = v->mmc.NY;
    int ADX = v->mmc.ADX, ANX = v->mmc.ANX;
    uint8_t CL = v->mmc.CL;
    int MXD = v->mmc.MXD;
    int delta = timing(v, hmmv_timing), cnt = v->ops_count;
    switch (v->mode) {
    default:
    case MODE_GRAPHIC4: pre_loop v->vram[VDP_VRMP5(MXD, ADX, DY)] = CL; post__x_y(256) break;
    case MODE_GRAPHIC5: pre_loop v->vram[VDP_VRMP6(MXD, ADX, DY)] = CL; post__x_y(512) break;
    case MODE_GRAPHIC6: pre_loop v->vram[VDP_VRMP7(MXD, ADX, DY)] = CL; post__x_y(512) break;
    case MODE_GRAPHIC7: pre_loop v->vram[VDP_VRMP8(MXD, ADX, DY)] = CL; post__x_y(256) break;
    }
    if ((v->ops_count = cnt) > 0) {
        finish(v);
        if (!NY) DY += TY;
        v->reg[42] = NY & 0xff; v->reg[43] = (NY >> 8) & 0x03;
        v->reg[38] = DY & 0xff; v->reg[39] = (DY >> 8) & 0x03;
    } else {
        v->mmc.DY = DY; v->mmc.NY = NY; v->mmc.ANX = ANX; v->mmc.ADX = ADX;
    }
}

static void hmmm_engine(v9938 *v) {
    int SX = v->mmc.SX, SY = v->mmc.SY, DX = v->mmc.DX, DY = v->mmc.DY, TX = v->mmc.TX, TY = v->mmc.TY;
    int NX = v->mmc.NX, NY = v->mmc.NY, ASX = v->mmc.ASX, ADX = v->mmc.ADX, ANX = v->mmc.ANX;
    int MXS = v->mmc.MXS, MXD = v->mmc.MXD;
    int delta = timing(v, hmmm_timing), cnt = v->ops_count;
    switch (v->mode) {
    default:
    case MODE_GRAPHIC4: pre_loop v->vram[VDP_VRMP5(MXD, ADX, DY)] = v->vram[VDP_VRMP5(MXS, ASX, SY)]; post_xxyy(256) break;
    case MODE_GRAPHIC5: pre_loop v->vram[VDP_VRMP6(MXD, ADX, DY)] = v->vram[VDP_VRMP6(MXS, ASX, SY)]; post_xxyy(512) break;
    case MODE_GRAPHIC6: pre_loop v->vram[VDP_VRMP7(MXD, ADX, DY)] = v->vram[VDP_VRMP7(MXS, ASX, SY)]; post_xxyy(512) break;
    case MODE_GRAPHIC7: pre_loop v->vram[VDP_VRMP8(MXD, ADX, DY)] = v->vram[VDP_VRMP8(MXS, ASX, SY)]; post_xxyy(256) break;
    }
    if ((v->ops_count = cnt) > 0) {
        finish(v);
        if (!NY) { SY += TY; DY += TY; }
        else if (SY == -1) DY += TY;
        v->reg[42] = NY & 0xff; v->reg[43] = (NY >> 8) & 0x03;
        v->reg[34] = SY & 0xff; v->reg[35] = (SY >> 8) & 0x03;
        v->reg[38] = DY & 0xff; v->reg[39] = (DY >> 8) & 0x03;
    } else {
        v->mmc.SY = SY; v->mmc.DY = DY; v->mmc.NY = NY;
        v->mmc.ANX = ANX; v->mmc.ASX = ASX; v->mmc.ADX = ADX;
    }
}

static void ymmm_engine(v9938 *v) {
    int SY = v->mmc.SY, DX = v->mmc.DX, DY = v->mmc.DY, TX = v->mmc.TX, TY = v->mmc.TY, NY = v->mmc.NY;
    int ADX = v->mmc.ADX;
    int MXD = v->mmc.MXD;
    int delta = timing(v, ymmm_timing), cnt = v->ops_count;
    switch (v->mode) {
    default:
    case MODE_GRAPHIC4: pre_loop v->vram[VDP_VRMP5(MXD, ADX, DY)] = v->vram[VDP_VRMP5(MXD, ADX, SY)]; post__xyy(256) break;
    case MODE_GRAPHIC5: pre_loop v->vram[VDP_VRMP6(MXD, ADX, DY)] = v->vram[VDP_VRMP6(MXD, ADX, SY)]; post__xyy(512) break;
    case MODE_GRAPHIC6: pre_loop v->vram[VDP_VRMP7(MXD, ADX, DY)] = v->vram[VDP_VRMP7(MXD, ADX, SY)]; post__xyy(512) break;
    case MODE_GRAPHIC7: pre_loop v->vram[VDP_VRMP8(MXD, ADX, DY)] = v->vram[VDP_VRMP8(MXD, ADX, SY)]; post__xyy(256) break;
    }
    if ((v->ops_count = cnt) > 0) {
        finish(v);
        if (!NY) { SY += TY; DY += TY; }
        else if (SY == -1) DY += TY;
        v->reg[42] = NY & 0xff; v->reg[43] = (NY >> 8) & 0x03;
        v->reg[34] = SY & 0xff; v->reg[35] = (SY >> 8) & 0x03;
        v->reg[38] = DY & 0xff; v->reg[39] = (DY >> 8) & 0x03;
    } else {
        v->mmc.SY = SY; v->mmc.DY = DY; v->mmc.NY = NY; v->mmc.ADX = ADX;
    }
}

static void hmmc_engine(v9938 *v) {
    if ((v->stat[2] & 0x80) == 0x80) return;
    v9938_cmd *c = &v->mmc;
    v->vram[vrmp(screen_mode_index(v), c->MXD, c->ADX, c->DY)] = v->reg[44];
    v->ops_count -= timing(v, hmmv_timing);
    v->stat[2] |= 0x80;
    if (!--c->ANX || ((c->ADX += c->TX) & c->MX)) {
        if (!(--c->NY & 1023) || (c->DY += c->TY) == -1) {
            finish(v);
            if (!c->NY) c->DY += c->TY;
            v->reg[42] = c->NY & 0xff; v->reg[43] = (c->NY >> 8) & 0x03;
            v->reg[38] = c->DY & 0xff; v->reg[39] = (c->DY >> 8) & 0x03;
        } else {
            c->ADX = c->DX;
            c->ANX = c->NX;
        }
    }
}

static void run_engine(v9938 *v) {
    switch (v->engine) {
    case 0x6: srch_engine(v); break;
    case 0x7: line_engine(v); break;
    case 0x8: lmmv_engine(v); break;
    case 0x9: lmmm_engine(v); break;
    case 0xa: lmcm_engine(v); break;
    case 0xb: lmmc_engine(v); break;
    case 0xc: hmmv_engine(v); break;
    case 0xd: hmmm_engine(v); break;
    case 0xe: ymmm_engine(v); break;
    case 0xf: hmmc_engine(v); break;
    default: break;
    }
}

static void cpu_to_vdp(v9938 *v, uint8_t value) {
    v->stat[2] &= 0x7f;
    v->stat[7] = v->reg[44] = value;
    if (v->engine && v->ops_count > 0) run_engine(v);
}

static uint8_t vdp_to_cpu(v9938 *v) {
    v->stat[2] &= 0x7f;
    if (v->engine && v->ops_count > 0) run_engine(v);
    return v->reg[44];
}

static void command_unit_w(v9938 *v, uint8_t op) {
    if (v->mode < MODE_GRAPHIC4 || v->mode > MODE_GRAPHIC7) return;   // commands work in SCREEN 5-8 only
    int SM = v->mode - MODE_GRAPHIC4;
    v9938_cmd *c = &v->mmc;
    c->CM = op >> 4;
    if ((c->CM & 0x0c) != 0x0c && c->CM != 0) v->stat[7] = (v->reg[44] &= Mask[SM]);

    switch (op >> 4) {
    case 0x0:   // ABRT
        finish(v);
        return;
    case 0x4:   // POINT
        finish(v);
        v->stat[7] = v->reg[44] = point(v, SM, (v->reg[45] & 0x10) != 0,
                                        v->reg[32] + (v->reg[33] << 8), v->reg[34] + (v->reg[35] << 8));
        return;
    case 0x5:   // PSET
        finish(v);
        pset(v, SM, (v->reg[45] & 0x20) != 0, v->reg[36] + (v->reg[37] << 8), v->reg[38] + (v->reg[39] << 8),
             v->reg[44], op & 0x0f);
        return;
    case 0x6: case 0x7: case 0x8: case 0x9: case 0xa: case 0xb: case 0xc: case 0xd: case 0xe: case 0xf:
        v->engine = op >> 4;
        break;
    default:
        return;
    }

    c->SX = (v->reg[32] + (v->reg[33] << 8)) & 511;
    c->SY = (v->reg[34] + (v->reg[35] << 8)) & 1023;
    c->DX = (v->reg[36] + (v->reg[37] << 8)) & 511;
    c->DY = (v->reg[38] + (v->reg[39] << 8)) & 1023;
    c->NY = (v->reg[42] + (v->reg[43] << 8)) & 1023;
    c->TY = (v->reg[45] & 0x08) ? -1 : 1;
    c->MX = PPL[SM];
    c->CL = v->reg[44];
    c->LO = op & 0x0f;
    c->MXS = (v->reg[45] & 0x10) != 0;
    c->MXD = (v->reg[45] & 0x20) != 0;
    if ((c->CM & 0x0c) == 0x0c) {   // byte commands move PPB pixels at a time
        c->TX = (v->reg[45] & 0x04) ? -PPB[SM] : PPB[SM];
        c->NX = ((v->reg[40] + (v->reg[41] << 8)) & 1023) / PPB[SM];
    } else {
        c->TX = (v->reg[45] & 0x04) ? -1 : 1;
        c->NX = (v->reg[40] + (v->reg[41] << 8)) & 1023;
    }
    if (c->CM == 0x7) {
        c->ASX = (c->NX - 1) >> 1;
        c->ADX = 0;
    } else {
        c->ASX = c->SX;
        c->ADX = c->DX;
    }
    c->ANX = c->CM == 0x6 ? (v->reg[45] & 0x02) != 0 : c->NX;
    v->stat[2] |= 0x01;
    if (v->engine && v->ops_count > 0) run_engine(v);
}

static void update_command(v9938 *v) {
    if (v->ops_count <= 0) {
        v->ops_count += 13662;
        if (v->engine && v->ops_count > 0) run_engine(v);
    } else {
        v->ops_count = 13662;
        if (v->engine) run_engine(v);
    }
}

// ---- registers and ports -----------------------------------------------------

static void register_write(v9938 *v, int reg, uint8_t data) {
    static const uint8_t reg_mask[28] = {
        0x7e, 0x7b, 0x7f, 0xff, 0x3f, 0xff, 0x3f, 0xff, 0xfb, 0xbf, 0x07, 0x03, 0xff, 0xff, 0x07, 0x0f,
        0x0f, 0xbf, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x7f, 0x3f, 0x07,
    };
    if (reg <= 27) {
        data &= reg_mask[reg];
        if (v->reg[reg] == data) return;
    }
    if (reg > 46) return;
    switch (reg) {
    case 0: case 1:
        v->reg[reg] = data;
        set_mode(v);
        check_int(v);
        break;
    case 9: case 18:
        v->reg[reg] = data;
        v->offset_x = 8 + position_offset(v->reg[18] & 0x0f);
        break;
    case 15:
        v->pal_write_first = false;
        break;
    case 25: case 26: case 27:
        if (!v->v9958) data = 0;
        break;
    case 44:
        cpu_to_vdp(v, data);
        break;
    case 46:
        command_unit_w(v, data);
        break;
    default: break;
    }
    v->reg[reg] = data;
}

static uint8_t status_r(v9938 *v) {
    v->cmd_write_first = false;
    int reg = v->reg[15] & 0x0f;
    if (reg > 9) return 0xff;
    uint8_t ret;
    switch (reg) {
    case 0:
        ret = v->stat[0];
        v->stat[0] &= 0x1f;
        break;
    case 1:
        ret = v->stat[1];
        v->stat[1] &= 0xfe;
        break;
    case 2:
        // HR: horizontal blanking, the last part of each 228-cycle line
        if (v->hpos >= 180) v->stat[2] |= 0x20; else v->stat[2] &= ~0x20;
        ret = v->stat[2];
        break;
    case 7:
        ret = v->stat[7];
        v->stat[7] = v->reg[44] = vdp_to_cpu(v);
        break;
    default:
        ret = v->stat[reg];
        break;
    }
    check_int(v);
    return ret;
}

static uint8_t vram_r(v9938 *v) {
    int address = (v->reg[14] << 14) | v->address_latch;
    v->cmd_write_first = false;
    uint8_t ret = v->read_ahead;
    if (v->reg[45] & 0x40) v->read_ahead = 0xff;   // no expansion RAM
    else v->read_ahead = vram_read(v, address);
    v->address_latch = (v->address_latch + 1) & 0x3fff;
    if (!v->address_latch && (v->reg[0] & 0x0c)) v->reg[14] = (v->reg[14] + 1) & 7;
    return ret;
}

static void vram_w(v9938 *v, uint8_t data) {
    int address = (v->reg[14] << 14) | v->address_latch;
    v->cmd_write_first = false;
    if (!(v->reg[45] & 0x40)) vram_write(v, address, data);
    v->address_latch = (v->address_latch + 1) & 0x3fff;
    if (!v->address_latch && (v->reg[0] & 0x0c)) v->reg[14] = (v->reg[14] + 1) & 7;
}

static void command_w(v9938 *v, uint8_t data) {
    if (!v->cmd_write_first) {
        v->cmd_write = data;
        v->cmd_write_first = true;
        return;
    }
    if (data & 0x80) {
        if (!(data & 0x40)) register_write(v, data & 0x3f, v->cmd_write);
    } else {
        v->address_latch = (uint16_t)(((data << 8) | v->cmd_write) & 0x3fff);
        if (!(data & 0x40)) vram_r(v);   // read ahead
    }
    v->cmd_write_first = false;
}

uint8_t v9938_read(v9938 *v, int port) {
    switch (port & 3) {
    case 0: return vram_r(v);
    case 1: return status_r(v);
    default: return 0xff;
    }
}

void v9938_write(v9938 *v, int port, uint8_t value) {
    switch (port & 3) {
    case 0: vram_w(v, value); break;
    case 1: command_w(v, value); break;
    case 2: palette_w(v, value); break;
    default: {
        int reg = v->reg[17] & 0x3f;
        if (reg != 17) register_write(v, reg, value);
        if (!(v->reg[17] & 0x80)) v->reg[17] = (v->reg[17] + 1) & 0x3f;
        break;
    }
    }
}

// ---- rendering -------------------------------------------------------------------

static bool second_field(v9938 *v) {
    return !(((v->reg[9] & 0x04) && !(v->stat[2] & 2)) || v->blink);
}

static void fill(uint32_t **ln, int n, uint32_t pen) {
    uint32_t *p = *ln;
    while (n-- > 0) *p++ = pen;
    *ln = p;
}

static void default_border(v9938 *v, uint32_t *ln) { fill(&ln, V9938_WIDTH, pen16(v, v->reg[7] & 0x0f)); }
static void graphic7_border(v9938 *v, uint32_t *ln) { fill(&ln, V9938_WIDTH, pen256(v, v->reg[7])); }
static void graphic5_border(v9938 *v, uint32_t *ln) {
    uint32_t p1 = pen16(v, v->reg[7] & 0x03), p0 = pen16(v, (v->reg[7] >> 2) & 0x03);
    for (int i = 0; i < V9938_WIDTH / 2; i++) { *ln++ = p0; *ln++ = p1; }
}

static void mode_text1(v9938 *v, uint32_t *ln, int line) {
    int pat = v->reg[4] << 11, nam = v->reg[2] << 10;
    uint32_t fg = pen16(v, v->reg[7] >> 4), bg = pen16(v, v->reg[7] & 15), pen = pen16(v, v->reg[7] & 15);
    int name = (line / 8) * 40;
    fill(&ln, (v->offset_x + 8) * 2, pen);
    for (int x = 0; x < 40; x++) {
        uint8_t pattern = vr(v, pat + vr(v, nam + name) * 8 + ((line + v->reg[23]) & 7));
        for (int i = 0; i < 6; i++, pattern <<= 1) {
            uint32_t c = (pattern & 0x80) ? fg : bg;
            *ln++ = c; *ln++ = c;
        }
        name = (name + 1) & 0x3ff;
    }
    fill(&ln, ((16 - v->offset_x) + 8) * 2, pen);
}

static void mode_text2(v9938 *v, uint32_t *ln, int line) {
    int pat = v->reg[4] << 11;
    int col = ((v->reg[3] & 0xf8) << 6) + (v->reg[10] << 14);
    int colourmask = ((v->reg[3] & 7) << 6) | 0x3f;
    int nam = (v->reg[2] & 0xfc) << 10;
    int patternmask = ((v->reg[2] & 3) << 10) | 0x3ff;
    uint32_t fg = pen16(v, v->reg[7] >> 4), bg = pen16(v, v->reg[7] & 15);
    uint32_t fg0 = pen16(v, v->reg[12] >> 4), bg0 = pen16(v, v->reg[12] & 15);
    int name = (line / 8) * 80;
    fill(&ln, (v->offset_x + 8) * 2, pen16(v, v->reg[7] & 15));
    for (int x = 0; x < 80; x++, name++) {
        int charcode = vr(v, nam + (name & patternmask));
        uint32_t f = fg, b = bg;
        if (v->blink && (vr(v, col + ((name / 8) & colourmask)) & (0x80 >> (name & 7)))) {
            f = fg0;
            b = bg0;
        }
        uint8_t pattern = vr(v, pat + charcode * 8 + ((line + v->reg[23]) & 7));
        for (int i = 0; i < 6; i++, pattern <<= 1) *ln++ = (pattern & 0x80) ? f : b;
    }
    fill(&ln, (16 - v->offset_x + 8) * 2, pen16(v, v->reg[7] & 15));
}

static void mode_multi(v9938 *v, uint32_t *ln, int line) {
    int nam = v->reg[2] << 10, pat = v->reg[4] << 11;
    int line2 = (line - v->reg[23]) & 255;
    int name = (line2 / 8) * 32;
    uint32_t bgp = pen16(v, v->reg[7] & 0x0f);
    fill(&ln, v->offset_x * 2, bgp);
    for (int x = 0; x < 32; x++, name++) {
        uint8_t colour = vr(v, pat + vr(v, nam + name) * 8 + ((line2 / 4) & 7));
        fill(&ln, 8, pen16(v, colour >> 4));
        fill(&ln, 8, pen16(v, colour & 15));
    }
    fill(&ln, (16 - v->offset_x) * 2, bgp);
}

static void mode_graphic1(v9938 *v, uint32_t *ln, int line) {
    int nam = v->reg[2] << 10, col = (v->reg[3] << 6) + (v->reg[10] << 14), pat = v->reg[4] << 11;
    int line2 = (line - v->reg[23]) & 255;
    int name = (line2 / 8) * 32;
    uint32_t pen = pen16(v, v->reg[7] & 0x0f);
    fill(&ln, v->offset_x * 2, pen);
    for (int x = 0; x < 32; x++, name++) {
        int charcode = vr(v, nam + name);
        uint8_t colour = vr(v, col + charcode / 8);
        uint32_t fg = pen16(v, colour >> 4), bg = pen16(v, colour & 15);
        uint8_t pattern = vr(v, pat + charcode * 8 + (line2 & 7));
        for (int i = 0; i < 8; i++, pattern <<= 1) {
            uint32_t c = (pattern & 0x80) ? fg : bg;
            *ln++ = c; *ln++ = c;
        }
    }
    fill(&ln, (16 - v->offset_x) * 2, pen);
}

static void mode_graphic23(v9938 *v, uint32_t *ln, int line) {
    const int colourmask = ((v->reg[3] & 0x7f) << 3) | 7;
    const int patternmask = ((v->reg[4] & 0x03) << 8) | 0xff;
    const int sy = (line + v->reg[23]) & 0xff;
    const int col = ((v->reg[3] & 0x80) << 6) + (v->reg[10] << 14);
    const int pat = (v->reg[4] & 0x3c) << 11;
    const uint32_t border = pen16(v, v->reg[7] & 0x0f);
    int nbase = (v->reg[2] << 10) + ((sy / 8) * 32);
    int noff = v->reg[26] & 0x1f;
    if ((v->reg[25] & 1) && (v->reg[26] & 0x20)) nbase ^= 0x8000;
    fill(&ln, v->offset_x * 2, border);
    int dot_scroll = v->reg[27] & 0x07;
    int to_mask = (v->reg[25] & 2) ? 8 - dot_scroll : 0;
    int to_draw = 256 - dot_scroll;
    fill(&ln, dot_scroll * 2, border);
    do {
        const int charcode = vr(v, nbase + noff) + ((sy & 0xc0) << 2);
        const uint8_t colour = vr(v, col + ((charcode & colourmask) << 3) + (sy & 7));
        uint8_t pattern = vr(v, pat + ((charcode & patternmask) << 3) + (sy & 7));
        const uint32_t fg = pen16(v, colour >> 4), bg = pen16(v, colour & 0x0f);
        for (int x = 0; x < 8 && to_draw > 0; x++, pattern <<= 1, to_draw--) {
            uint32_t c = border;
            if (!to_mask) c = (pattern & 0x80) ? fg : bg;
            else to_mask--;
            *ln++ = c; *ln++ = c;
        }
        noff = (noff + 1) & 0x1f;
        if ((v->reg[25] & 1) && !noff) nbase ^= 0x8000;
    } while (to_draw > 0);
    fill(&ln, (16 - v->offset_x) * 2, border);
}

static void mode_graphic4(v9938 *v, uint32_t *ln, int line) {
    int linemask = ((v->reg[2] & 0x1f) << 3) | 7;
    const int sy = ((line + v->reg[23]) & linemask) & 0xff;
    const uint32_t border = pen16(v, v->reg[7] & 0x0f);
    int nbase = ((v->reg[2] & 0x40) << 10) + (sy << 7);
    int noff = (v->reg[26] & 0x1f) << 2;
    if (!(v->reg[25] & 1) && (v->reg[2] & 0x20) && second_field(v)) nbase += 0x8000;
    if ((v->reg[25] & 1) && (v->reg[26] & 0x20)) nbase ^= 0x8000;
    fill(&ln, v->offset_x * 2, border);
    int dot_scroll = v->reg[27] & 0x07;
    int to_mask = (v->reg[25] & 2) ? 8 - dot_scroll : 0;
    int to_draw = 256 - dot_scroll;
    fill(&ln, dot_scroll * 2, border);
    do {
        bool m1 = false, m2 = false;
        if (to_mask) {
            m1 = true;
            if (--to_mask) { m2 = true; to_mask--; }
        }
        const uint8_t colour = vr(v, nbase + noff);
        const uint32_t p1 = m1 ? border : pen16(v, colour >> 4);
        const uint32_t p2 = m2 ? border : pen16(v, colour & 0x0f);
        *ln++ = p1; *ln++ = p1;
        to_draw--;
        if (to_draw) {
            *ln++ = p2; *ln++ = p2;
            to_draw--;
        }
        noff = (noff + 1) & 0x7f;
        if ((v->reg[25] & 1) && !noff) nbase ^= 0x8000;
    } while (to_draw > 0);
    fill(&ln, (16 - v->offset_x) * 2, border);
}

static void mode_graphic5(v9938 *v, uint32_t *ln, int line) {
    int linemask = ((v->reg[2] & 0x1f) << 3) | 7;
    int line2 = ((line + v->reg[23]) & linemask) & 255;
    int nam = ((v->reg[2] & 0x40) << 10) + line2 * 128;
    if ((v->reg[2] & 0x20) && second_field(v)) nam += 0x8000;
    uint32_t bg0[4], bg1[4];
    bg1[0] = pen16(v, v->reg[7] & 0x03);
    bg0[0] = pen16(v, (v->reg[7] >> 2) & 0x03);
    for (int i = 0; i < v->offset_x; i++) { *ln++ = bg0[0]; *ln++ = bg1[0]; }
    for (int x = (v->reg[8] & 0x20) ? 0 : 1; x < 4; x++) bg0[x] = bg1[x] = pen16(v, x);
    for (int x = 0; x < 128; x++) {
        uint8_t c = vr(v, nam++);
        *ln++ = bg0[c >> 6];
        *ln++ = bg1[(c >> 4) & 3];
        *ln++ = bg0[(c >> 2) & 3];
        *ln++ = bg1[c & 3];
    }
    bg1[0] = pen16(v, v->reg[7] & 0x03);
    bg0[0] = pen16(v, (v->reg[7] >> 2) & 0x03);
    for (int i = 0; i < 16 - v->offset_x; i++) { *ln++ = bg0[0]; *ln++ = bg1[0]; }
}

static inline uint8_t ivr(v9938 *v, int a) {   // interleaved G6/G7 read
    return v->vram[(((a & 1) << 16) | (a >> 1)) & 0x1ffff];
}

static void mode_graphic6(v9938 *v, uint32_t *ln, int line) {
    int linemask = ((v->reg[2] & 0x1f) << 3) | 7;
    int line2 = ((line + v->reg[23]) & linemask) & 255;
    int nam = line2 << 8;
    if ((v->reg[2] & 0x20) && second_field(v)) nam += 0x10000;
    uint32_t bgp = pen16(v, v->reg[7] & 0x0f);
    fill(&ln, v->offset_x * 2, bgp);
    if (v->reg[2] & 0x40) {
        for (int x = 0; x < 32; x++) {
            nam++;
            uint8_t c = ivr(v, nam);
            uint32_t f0 = pen16(v, c >> 4), f1 = pen16(v, c & 15);
            for (int i = 0; i < 8; i++) { *ln++ = f0; *ln++ = f1; }
            nam += 7;
        }
    } else {
        for (int x = 0; x < 256; x++) {
            uint8_t c = ivr(v, nam++);
            *ln++ = pen16(v, c >> 4);
            *ln++ = pen16(v, c & 15);
        }
    }
    fill(&ln, (16 - v->offset_x) * 2, bgp);
}

static void mode_graphic7(v9938 *v, uint32_t *ln, int line) {
    int linemask = ((v->reg[2] & 0x1f) << 3) | 7;
    int line2 = ((line + v->reg[23]) & linemask) & 255;
    int nam = line2 << 8;
    if ((v->reg[2] & 0x20) && second_field(v)) nam += 0x10000;
    uint32_t bgp = pen256(v, v->reg[7]);
    fill(&ln, v->offset_x * 2, bgp);
    int yjk_mode = v->reg[25] & 0x18;
    if (v->v9958 && (yjk_mode == 0x08 || yjk_mode == 0x18)) {
        // SCREEN 12 (YJK) and SCREEN 10/11 (YJK with YAE palette pixels)
        for (int x = 0; x < 64; x++) {
            int c[4];
            for (int i = 0; i < 4; i++) c[i] = ivr(v, nam++);
            int ind = (c[0] & 7) << 11 | (c[1] & 7) << 14 | (c[2] & 7) << 5 | (c[3] & 7) << 8;
            for (int i = 0; i < 4; i++) {
                uint32_t p;
                if (yjk_mode == 0x08) p = yjk[ind | ((c[i] >> 3) & 31)];
                else p = (c[i] & 8) ? pen16(v, c[i] >> 4) : yjk[ind | ((c[i] >> 3) & 30)];
                *ln++ = p; *ln++ = p;
            }
        }
    } else if (v->reg[2] & 0x40) {
        for (int x = 0; x < 32; x++) {
            nam++;
            fill(&ln, 16, pen256(v, ivr(v, nam)));
            nam++;
        }
    } else {
        for (int x = 0; x < 256; x++) {
            uint32_t p = pen256(v, ivr(v, nam++));
            *ln++ = p; *ln++ = p;
        }
    }
    fill(&ln, (16 - v->offset_x) * 2, bgp);
}

static void mode_unknown(v9938 *v, uint32_t *ln, int line) {
    (void)line;
    uint32_t fg = pen16(v, v->reg[7] >> 4), bg = pen16(v, v->reg[7] & 0x0f);
    fill(&ln, v->offset_x * 2, bg);
    fill(&ln, 512, fg);
    fill(&ln, (16 - v->offset_x) * 2, bg);
}

// ---- sprites -------------------------------------------------------------------

static void sprite_mode1(v9938 *v, int line, uint8_t *col) {
    memset(col, 0, 256);
    if (v->reg[8] & 0x02) return;
    int attr = (v->reg[5] << 7) + (v->reg[11] << 15);
    int pgen = v->reg[6] << 11;
    int height = (v->reg[1] & 2) ? 16 : 8;
    if (v->reg[1] & 1) height *= 2;
    int p = 0, p2 = 0;
    for (;;) {
        int y = vr(v, attr);
        if (y == 208) break;
        y = (y - v->reg[23]) & 255;
        y = y > 208 ? -(~y & 255) : y + 1;
        if (line >= y && line < y + height) {
            if (p2 == 4) {
                if (!(v->stat[0] & 0x40)) v->stat[0] = (uint8_t)((v->stat[0] & 0xa0) | 0x40 | p);
                break;
            }
            int x = vr(v, attr + 1);
            if (vr(v, attr + 3) & 0x80) x -= 32;
            int pattern = vr(v, attr + 2);
            if (v->reg[1] & 2) pattern &= 0xfc;
            int n = line - y;
            int paddr = pgen + pattern * 8 + ((v->reg[1] & 1) ? n / 2 : n);
            int c = vr(v, attr + 3) & 0x0f;
            for (n = 0;; n++) {
                if (n == 0) pattern = vr(v, paddr);
                else if (n == 1 && (v->reg[1] & 2)) pattern = vr(v, paddr + 16);
                else break;
                for (int i = 0; i < 8; i++, pattern <<= 1) {
                    if ((pattern & 0x80) && x >= 0 && x < 256) {
                        if ((col[x] & 0x40) && p2 < 4) v->stat[0] |= 0x20;
                        if (!(col[x] & 0x80)) {
                            if (c || (v->reg[8] & 0x20)) col[x] |= 0xc0 | c;
                            else col[x] |= 0x40;
                        }
                        if ((v->reg[1] & 1) && x + 1 < 256) {
                            if ((col[x + 1] & 0x40) && p2 < 4) v->stat[0] |= 0x20;
                            if (!(col[x + 1] & 0x80)) {
                                if (c || (v->reg[8] & 0x20)) col[x + 1] |= 0xc0 | c;
                                else col[x + 1] |= 0x40;   // MAME sets 0x80 here, drawing a colour-0 pixel
                            }
                        }
                    }
                    x += (v->reg[1] & 1) ? 2 : 1;
                }
            }
            p2++;
        }
        if (p >= 31) break;
        p++;
        attr += 4;
    }
    if (!(v->stat[0] & 0x40)) v->stat[0] = (uint8_t)((v->stat[0] & 0xa0) | p);
}

static void sprite_mode2(v9938 *v, int line, uint8_t *col) {
    memset(col, 0, 256);
    if (v->reg[8] & 0x02) return;
    int attr = ((v->reg[5] & 0xfc) << 7) + (v->reg[11] << 15);
    int ctab = ((v->reg[5] & 0xf8) << 7) + (v->reg[11] << 15);
    int pgen = v->reg[6] << 11;
    int colourmask = ((v->reg[5] & 3) << 3) | 0x7;
    int height = (v->reg[1] & 2) ? 16 : 8;
    if (v->reg[1] & 1) height *= 2;
    int p = 0, p2 = 0;
    bool first_cc_seen = false;
    for (;;) {
        int y = vram_read(v, attr);
        if (y == 216) break;
        y = (y - v->reg[23]) & 255;
        y = y > 216 ? -(~y & 255) : y + 1;
        if (line >= y && line < y + height) {
            if (p2 == 8) {
                if (!(v->stat[0] & 0x40)) v->stat[0] = (uint8_t)((v->stat[0] & 0xa0) | 0x40 | p);
                break;
            }
            int n = line - y;
            if (v->reg[1] & 1) n /= 2;
            int c = vram_read(v, ctab + ((p & colourmask) * 16) + n);
            // sprites with CC set are not drawn before a sprite with CC = 0 on this line
            if ((c & 0x40) && !first_cc_seen) goto next;
            if (!(c & 0x40)) first_cc_seen = true;
            int pattern = vram_read(v, attr + 2);
            if (v->reg[1] & 2) pattern &= 0xfc;
            int paddr = pgen + pattern * 8 + n;
            pattern = (vram_read(v, paddr) << 8) | vram_read(v, paddr + 16);
            int x = vram_read(v, attr + 1);
            if (c & 0x80) x -= 32;
            for (n = (v->reg[1] & 2) ? 16 : 8; n--; pattern <<= 1) {
                for (int i = 0; i <= (v->reg[1] & 1); i++) {
                    if (x >= 0 && x < 256) {
                        if ((pattern & 0x8000) && !(col[x] & 0x10)) {
                            if ((c & 15) || (v->reg[8] & 0x20)) {
                                if (!(c & 0x40)) {
                                    if (col[x] & 0x20) col[x] |= 0x10;
                                    else col[x] |= 0x20 | (c & 15);
                                } else {
                                    col[x] |= c & 15;
                                }
                                col[x] |= 0x80;
                            }
                        } else if (!(c & 0x40) && (col[x] & 0x20)) {
                            col[x] |= 0x10;
                        }
                        if (!(c & 0x60) && (pattern & 0x8000)) {
                            if (col[x] & 0x40) {
                                if (p2 < 8) v->stat[0] |= 0x20;
                            } else {
                                col[x] |= 0x40;
                            }
                        }
                    }
                    x++;
                }
            }
        next:
            p2++;
        }
        if (p >= 31) break;
        p++;
        attr += 4;
    }
    if (!(v->stat[0] & 0x40)) v->stat[0] = (uint8_t)((v->stat[0] & 0xa0) | p);
}

static void draw_sprites(v9938 *v, uint32_t *ln, const uint8_t *col) {
    static const uint16_t g7_ind16[16] = { 0, 2, 192, 194, 48, 50, 240, 242, 482, 7, 448, 455, 56, 63, 504, 511 };
    ln += v->offset_x * 2;
    for (int i = 0; i < 256; i++, ln += 2) {
        if (!(col[i] & 0x80)) continue;
        uint32_t a, b;
        if (v->mode == MODE_GRAPHIC5) {
            a = pen16(v, (col[i] >> 2) & 3);
            b = pen16(v, col[i] & 3);
        } else if (v->mode == MODE_GRAPHIC7) {
            int g = g7_ind16[col[i] & 15];
            a = b = rgb(pal3bit(g >> 6), pal3bit(g >> 3), pal3bit(g));
        } else {
            a = b = pen16(v, col[i] & 15);
        }
        ln[0] = a;
        ln[1] = b;
    }
}

static void refresh_line(v9938 *v, int line, uint32_t *ln) {
    // Colour 0 shows the backdrop unless TP (R8 bit 5) is set.
    uint32_t keep16 = v->pal16[0], keep256 = v->pal256[0];
    bool tp = !(v->reg[8] & 0x20) && v->mode != MODE_GRAPHIC5;
    if (tp) {
        v->pal16[0] = v->pal16[v->reg[7] & 0x0f];
        v->pal256[0] = v->pal256[v->reg[7]];
    }
    if (!(v->reg[1] & 0x40) || (v->stat[2] & 0x40)) {
        if (v->mode == MODE_GRAPHIC5) graphic5_border(v, ln);
        else if (v->mode == MODE_GRAPHIC7) graphic7_border(v, ln);
        else default_border(v, ln);
    } else {
        bool sprites = true;
        switch (v->mode) {
        case MODE_TEXT1: mode_text1(v, ln, line); sprites = false; break;
        case MODE_TEXT2: mode_text2(v, ln, line); sprites = false; break;
        case MODE_MULTI: mode_multi(v, ln, line); break;
        case MODE_GRAPHIC1: mode_graphic1(v, ln, line); break;
        case MODE_GRAPHIC2: case MODE_GRAPHIC3: mode_graphic23(v, ln, line); break;
        case MODE_GRAPHIC4: mode_graphic4(v, ln, line); break;
        case MODE_GRAPHIC5: mode_graphic5(v, ln, line); break;
        case MODE_GRAPHIC6: mode_graphic6(v, ln, line); break;
        case MODE_GRAPHIC7: mode_graphic7(v, ln, line); break;
        default: mode_unknown(v, ln, line); sprites = false; break;
        }
        if (sprites) {
            uint8_t col[256];
            if (v->mode <= MODE_GRAPHIC2) sprite_mode1(v, line, col);
            else sprite_mode2(v, line, col);
            draw_sprites(v, ln, col);
        }
    }
    if (tp) {
        v->pal16[0] = keep16;
        v->pal256[0] = keep256;
    }
}

static void start_vblank(v9938 *v) {
    v->stat[2] = (v->stat[2] & 0xfd) | (~v->stat[2] & 2);   // field toggles every frame
    if (!(v->reg[13] & 0xf0)) {
        v->blink = false;
    } else if (!(v->reg[13] & 0x0f)) {
        v->blink = true;
    } else {
        if (v->blink_count) v->blink_count--;
        if (!v->blink_count) {
            v->blink = !v->blink;
            v->blink_count = v->blink ? (v->reg[13] >> 4) * 10 : (v->reg[13] & 0x0f) * 10;
        }
    }
}

static void set_screen_parameters(v9938 *v) {
    v->scanline_start = (v->reg[9] & 0x80) ? 16 : 26;
    v->scanline_max = (v->reg[9] & 0x80) ? 234 : 244;
    v->visible_y = (v->reg[9] & 0x80) ? 212 : 192;
}

void v9938_line(v9938 *v) {
    int s = v->scanline;
    int line = s - (v->scanline_start + v->offset_y);

    update_command(v);
    if (s == v->scanline_start + v->offset_y) {
        v->stat[2] &= ~0x40;
    } else if (s == v->scanline_start + v->offset_y + v->visible_y) {
        v->stat[2] |= 0x40;
        v->stat[0] |= 0x80;
    }
    if (line >= 0 && line <= v->scanline_max && ((line + v->reg[23]) & 255) == v->reg[19])
        v->stat[1] |= 1;
    else if (!(v->reg[0] & 0x10))
        v->stat[1] &= 0xfe;
    check_int(v);

    if (s == VBLANK_START) start_vblank(v);
    int row = s - V9938_FIRST_LINE;
    if (row >= 0 && row < V9938_LINES) {
        uint32_t *ln = v->fb + row * 2 * V9938_WIDTH;
        refresh_line(v, line, ln);
        memcpy(ln + V9938_WIDTH, ln, V9938_WIDTH * sizeof *ln);
    }
    if (++v->scanline >= VTOTAL) {
        v->scanline = 0;
        v->offset_y = position_offset(v->reg[18] >> 4);
        set_screen_parameters(v);
    }
}

void v9938_init(v9938 *v, bool v9958) {
    memset(v, 0, sizeof *v);
    v->v9958 = v9958;
    if (v9958 && !yjk_ready) build_yjk();
    memset(v->vram + 0x20000, 0xff, 0x10000);
    v9938_reset(v);
}

void v9938_reset(v9938 *v) {
    v->offset_x = 8;
    v->offset_y = 0;
    reset_palette(v);
    memset(v->stat, 0, sizeof v->stat);
    v->stat[2] = 0x0c;
    if (v->v9958) v->stat[1] |= 4;
    v->stat[4] = 0xfe;
    v->stat[6] = 0xfc;
    memset(v->reg, 0, sizeof v->reg);
    v->cmd_write_first = v->pal_write_first = false;
    v->irq = false;
    v->read_ahead = 0;
    v->address_latch = 0;
    v->scanline = 0;
    v->engine = 0;
    v->ops_count = 1;
    v->blink = false;
    v->blink_count = 0;
    set_mode(v);
    set_screen_parameters(v);
}
