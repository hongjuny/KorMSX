// MSX machine: slot/sub-slot memory map, 8255 PPI (slot select, keyboard
// matrix, click), PSG joystick ports, ROM cartridges with mappers and SCC,
// and the line-by-line frame loop. MSX2 adds the V9938/V9958, memory mapper
// RAM, the RP5C01 clock and the kanji font ROM ports. Port and slot behaviour
// follows MAME's msx.cpp (ppi_port_a_w, ppi_port_c_w, expanded_slot_r/w,
// psg_port_a_r, kanji_r/w).
#include "msxcore.h"

#include <stdlib.h>
#include <string.h>

#include "psg.h"
#include "rtc.h"
#include "v9938.h"
#include "vdp.h"
#include "z80.h"

enum { DEV_ROM, DEV_RAM, DEV_CART, DEV_MAPPER };

typedef struct scc {
    int8_t wave[5][32];
    uint16_t freq[5];
    uint8_t vol[5];
    uint8_t enable;
    uint32_t phase[5];
    uint8_t pos[5];
} scc;

typedef struct device {
    int type;
    uint8_t *data;          // owned copy
    size_t size;
    uint16_t base;          // CPU address of data[0] (ROM)
    // cartridges
    int mapper;
    uint8_t bank[4];        // 8 KB banks for 4000/6000/8000/A000 (ASCII16 uses pairs)
    uint32_t bank_mask;     // number of 8 KB banks - 1 (power of two)
    bool scc_active;
    scc scc;
} device;

#define MAX_DEVICES 16

struct msx {
    z80 cpu;
    vdp vdp;
    psg psg;
    v9938 *v2;              // MSX2/2+ VDP, NULL on an MSX1
    rtc rtc;
    uint8_t mapper[4];      // memory mapper segment per page (ports FC-FF)
    uint32_t mapper_segments;   // segments of the mapper RAM (power of two), 0 = none
    uint8_t *kanji;         // kanji font ROM (128 or 256 KB)
    size_t kanji_size;
    uint32_t kanji_latch[2];

    device devices[MAX_DEVICES];
    int ndevices;
    device *map[4][4][4];   // [primary][secondary][page]
    bool expanded[4];
    uint8_t primary;
    uint8_t secondary[4];
    struct { int prim, sec; bool used; device *dev; } cart[2];

    uint8_t keys[11];       // active low
    uint8_t ppi_c;
    uint8_t psg_b;
    uint8_t joy[2];         // active high in the API, inverted on read

    int line_budget;
    int psg_cycles;
    uint32_t sample_phase;
    float acc;
    int acc_n;
    int16_t audio[8192];
    size_t audio_len;
    float dc;               // DC blocker state
    float dc_prev;
};

// ---- cartridge mappers -----------------------------------------------------

static uint8_t scc_read(scc *s, uint16_t a) {
    a &= 0xff;
    if (a < 0x80) return (uint8_t)s->wave[a >> 5][a & 31];
    if (a >= 0xa0 && a < 0xe0) return (uint8_t)s->wave[4][a & 31];   // read-back of ch 4/5 wave
    return 0xff;
}

static void scc_write(scc *s, uint16_t a, uint8_t v) {
    a &= 0xff;
    if (a < 0x80) {
        s->wave[a >> 5][a & 31] = (int8_t)v;
        if (a >= 0x60) s->wave[4][a & 31] = (int8_t)v;   // channels 4 and 5 share a waveform
    } else if (a < 0x8a) {
        int ch = (a - 0x80) >> 1;
        if (a & 1) s->freq[ch] = (uint16_t)((s->freq[ch] & 0xff) | ((v & 0x0f) << 8));
        else s->freq[ch] = (uint16_t)((s->freq[ch] & 0xf00) | v);
    } else if (a < 0x8f) {
        s->vol[a - 0x8a] = v & 0x0f;
    } else if (a == 0x8f) {
        s->enable = v & 0x1f;
    }
}

/// Advances the SCC by `cycles` CPU clocks and returns its output, about -1...1.
static float scc_tick(scc *s, int cycles) {
    int sum = 0;
    for (int ch = 0; ch < 5; ch++) {
        uint32_t period = s->freq[ch] + 1u;
        if (s->freq[ch] > 8) {
            s->phase[ch] += (uint32_t)cycles;
            while (s->phase[ch] >= period) {
                s->phase[ch] -= period;
                s->pos[ch] = (s->pos[ch] + 1) & 31;
            }
        }
        if (s->enable & (1 << ch)) sum += s->wave[ch][s->pos[ch]] * s->vol[ch];
    }
    return (float)sum / (128.0f * 15.0f * 2.0f);
}

static int guess_mapper(const uint8_t *d, size_t size) {
    if (size <= 0x10000) {
        // A plain ROM unless it is bigger than 32 KB and has no header where one would be.
        return MSX_MAPPER_PLAIN;
    }
    int konami = 0, scc_hits = 0, ascii8 = 0, ascii16 = 0;
    for (size_t i = 0; i + 2 < size; i++) {
        if (d[i] != 0x32) continue;   // LD (nnnn),A
        uint16_t a = (uint16_t)(d[i + 1] | (d[i + 2] << 8));
        switch (a) {
        case 0x5000: case 0x9000: case 0xb000: scc_hits++; break;
        case 0x4000: case 0x8000: case 0xa000: konami++; break;
        case 0x6800: case 0x7800: ascii8++; break;
        case 0x6000: konami++; ascii8++; ascii16++; break;
        case 0x7000: scc_hits++; ascii8++; ascii16++; break;
        case 0x77ff: ascii16++; break;
        }
    }
    int best = MSX_MAPPER_KONAMI_SCC, score = scc_hits;
    if (konami > score) { best = MSX_MAPPER_KONAMI; score = konami; }
    if (ascii8 > score) { best = MSX_MAPPER_ASCII8; score = ascii8; }
    if (ascii16 > score) { best = MSX_MAPPER_ASCII16; score = ascii16; }
    return best;
}

static uint8_t cart_read(device *d, uint16_t a) {
    if (d->mapper == MSX_MAPPER_PLAIN) {
        if (a < d->base) return 0xff;
        uint32_t off = (uint32_t)(a - d->base);
        if (d->size < 0x4000) off %= (uint32_t)d->size;   // 8 KB mirrored in its page
        return off < d->size ? d->data[off] : 0xff;
    }
    if (a < 0x4000 || a >= 0xc000) return 0xff;
    if (d->mapper == MSX_MAPPER_KONAMI_SCC && d->scc_active && a >= 0x9800 && a < 0xa000)
        return scc_read(&d->scc, a);
    int slot = (a - 0x4000) >> 13;
    uint32_t off = ((uint32_t)(d->bank[slot] & d->bank_mask) << 13) | (a & 0x1fff);
    return off < d->size ? d->data[off] : 0xff;
}

static void cart_write(device *d, uint16_t a, uint8_t v) {
    switch (d->mapper) {
    case MSX_MAPPER_KONAMI:
        if (a >= 0x6000 && a < 0xc000) d->bank[(a - 0x4000) >> 13] = v;
        break;
    case MSX_MAPPER_KONAMI_SCC:
        if (d->scc_active && a >= 0x9800 && a < 0xa000) {
            scc_write(&d->scc, a, v);
            break;
        }
        if (a >= 0x5000 && a < 0xc000 && (a & 0x1800) == 0x1000) {
            int slot = (a - 0x4000) >> 13;
            d->bank[slot] = v;
            if (slot == 2) d->scc_active = (v & 0x3f) == 0x3f;
        }
        break;
    case MSX_MAPPER_ASCII8:
        if (a >= 0x6000 && a < 0x8000) d->bank[(a >> 11) & 3] = v;
        break;
    case MSX_MAPPER_ASCII16:
        if (a >= 0x6000 && a < 0x6800) {
            d->bank[0] = (uint8_t)(v * 2);
            d->bank[1] = (uint8_t)(v * 2 + 1);
        } else if (a >= 0x7000 && a < 0x7800) {
            d->bank[2] = (uint8_t)(v * 2);
            d->bank[3] = (uint8_t)(v * 2 + 1);
        }
        break;
    }
}

// ---- memory and I/O ---------------------------------------------------------

static inline device *page_device(msx *m, uint16_t a) {
    int page = a >> 14;
    int prim = (m->primary >> (page * 2)) & 3;
    int sec = m->expanded[prim] ? (m->secondary[prim] >> (page * 2)) & 3 : 0;
    return m->map[prim][sec][page];
}

static uint8_t mem_read(void *ctx, uint16_t a) {
    msx *m = ctx;
    if (a == 0xffff) {
        int prim = m->primary >> 6;
        if (m->expanded[prim]) return (uint8_t)~m->secondary[prim];
    }
    device *d = page_device(m, a);
    if (!d) return 0xff;
    switch (d->type) {
    case DEV_RAM: return d->data[a];
    case DEV_MAPPER:
        return d->data[((m->mapper[a >> 14] & (m->mapper_segments - 1)) << 14) | (a & 0x3fff)];
    case DEV_ROM: {
        uint32_t off = (uint16_t)(a - d->base) % (uint32_t)d->size;
        return d->data[off];
    }
    default: return cart_read(d, a);
    }
}

static void mem_write(void *ctx, uint16_t a, uint8_t v) {
    msx *m = ctx;
    if (a == 0xffff) {
        int prim = m->primary >> 6;
        if (m->expanded[prim]) {
            m->secondary[prim] = v;
            return;
        }
    }
    device *d = page_device(m, a);
    if (!d) return;
    if (d->type == DEV_RAM) d->data[a] = v;
    else if (d->type == DEV_MAPPER) d->data[((m->mapper[a >> 14] & (m->mapper_segments - 1)) << 14) | (a & 0x3fff)] = v;
    else if (d->type == DEV_CART) cart_write(d, a, v);
}

static uint8_t psg_port_a(void *ctx) {
    msx *m = ctx;
    // bit 7: cassette input (high with nothing connected); bits 0-5: selected joystick
    int port = (m->psg_b >> 6) & 1;
    return (uint8_t)(0x80 | (~m->joy[port] & 0x3f));
}

static void psg_port_b(void *ctx, uint8_t v) {
    msx *m = ctx;
    m->psg_b = v;
}

static uint8_t io_read(void *ctx, uint16_t port) {
    msx *m = ctx;
    uint8_t p = port & 0xff;
    if (p >= 0x98 && p <= 0x9b) {
        if (m->v2) return v9938_read(m->v2, p & 3);
        return p <= 0x99 ? vdp_read(&m->vdp, p & 1) : 0xff;
    }
    if (p >= 0xfc && m->mapper_segments)   // unused high bits read back as 1
        return (uint8_t)(m->mapper[p & 3] | ~(m->mapper_segments - 1));
    if ((p == 0xd9 || p == 0xdb) && m->kanji) {
        int bank = (p == 0xdb) ? 1 : 0;
        uint32_t addr = (uint32_t)bank * 0x20000 + m->kanji_latch[bank];
        uint8_t data = addr < m->kanji_size ? m->kanji[addr] : 0xff;
        m->kanji_latch[bank] = (m->kanji_latch[bank] & ~0x1fu) | ((m->kanji_latch[bank] + 1) & 0x1f);
        return data;
    }
    switch (p) {
    case 0xb5: return m->v2 ? rtc_read(&m->rtc) : 0xff;
    case 0xa2: return psg_read(&m->psg);
    case 0xa8: return m->primary;
    case 0xa9: {
        int row = m->ppi_c & 0x0f;
        return row <= 10 ? m->keys[row] : 0xff;
    }
    case 0xaa: return m->ppi_c;
    default: return 0xff;
    }
}

static void io_write(void *ctx, uint16_t port, uint8_t v) {
    msx *m = ctx;
    uint8_t p = port & 0xff;
    if (p >= 0x98 && p <= 0x9b) {
        if (m->v2) v9938_write(m->v2, p & 3, v);
        else if (p <= 0x99) vdp_write(&m->vdp, p & 1, v);
        return;
    }
    if (p >= 0xfc && m->mapper_segments) {
        m->mapper[p & 3] = v;
        return;
    }
    if (p >= 0xd8 && p <= 0xdb && m->kanji) {
        int bank = (p >= 0xda) ? 1 : 0;
        uint32_t *l = &m->kanji_latch[bank];
        if (p & 1) *l = (*l & 0x007e0) | ((uint32_t)(v & 0x3f) << 11);
        else *l = (*l & 0x1f800) | ((uint32_t)(v & 0x3f) << 5);
        return;
    }
    switch (p) {
    case 0xb4: if (m->v2) rtc_address(&m->rtc, v); break;
    case 0xb5: if (m->v2) rtc_write(&m->rtc, v); break;
    case 0xa0: psg_address(&m->psg, v); break;
    case 0xa1: psg_write(&m->psg, v); break;
    case 0xa8: m->primary = v; break;
    case 0xaa: m->ppi_c = v; break;
    case 0xab:
        if (v & 0x80) {     // mode set clears the output latches
            m->primary = 0;
            m->ppi_c = 0;
        } else {            // bit set/reset on port C
            int b = (v >> 1) & 7;
            if (v & 1) m->ppi_c |= (uint8_t)(1 << b);
            else m->ppi_c &= (uint8_t)~(1 << b);
        }
        break;
    default: break;
    }
}

// ---- audio -------------------------------------------------------------------

static void audio_advance(msx *m, int cycles) {
    m->psg_cycles += cycles;
    while (m->psg_cycles >= 16) {    // PSG steps at CPU clock / 16
        m->psg_cycles -= 16;
        float v = psg_tick(&m->psg) * 0.30f;
        for (int i = 0; i < 2; i++) {
            device *d = m->cart[i].dev;
            if (d && d->mapper == MSX_MAPPER_KONAMI_SCC) v += scc_tick(&d->scc, 16) * 0.9f;
        }
        if (m->ppi_c & 0x80) v += 0.25f;   // key click
        m->acc += v;
        m->acc_n++;
        m->sample_phase += 16u * MSX_AUDIO_RATE;
        if (m->sample_phase >= MSX_CPU_CLOCK) {
            m->sample_phase -= MSX_CPU_CLOCK;
            float s = m->acc / (float)m->acc_n;
            m->acc = 0;
            m->acc_n = 0;
            // one-pole DC blocker: the PSG and click only swing upwards
            float out = s - m->dc_prev + 0.995f * m->dc;
            m->dc_prev = s;
            m->dc = out;
            int iv = (int)(out * 26000.0f);
            if (iv > 32767) iv = 32767;
            if (iv < -32768) iv = -32768;
            if (m->audio_len < sizeof m->audio / sizeof m->audio[0]) m->audio[m->audio_len++] = (int16_t)iv;
        }
    }
}

// ---- public API ------------------------------------------------------------

msx *msx_create(void) {
    msx *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    z80_init(&m->cpu, m, mem_read, mem_write, io_read, io_write);
    m->cpu.m1_wait = 1;   // MSX: one wait state on every M1 cycle
    m->psg.ctx = m;
    m->psg.port_a_read = psg_port_a;
    m->psg.port_b_write = psg_port_b;
    msx_reset(m);
    return m;
}

void msx_destroy(msx *m) {
    if (!m) return;
    for (int i = 0; i < m->ndevices; i++) free(m->devices[i].data);
    free(m->v2);
    free(m->kanji);
    free(m);
}

void msx_expand_slot(msx *m, int prim) {
    if (prim >= 0 && prim < 4) m->expanded[prim] = true;
}

static bool valid_place(int prim, int sec, int page, int pages) {
    return prim >= 0 && prim < 4 && sec >= 0 && sec < 4 && page >= 0 && pages >= 1 && page + pages <= 4;
}

bool msx_add_rom(msx *m, int prim, int sec, int page, int pages, const void *data, size_t size) {
    if (!valid_place(prim, sec, page, pages) || !data || size == 0 || m->ndevices >= MAX_DEVICES) return false;
    device *d = &m->devices[m->ndevices++];
    memset(d, 0, sizeof *d);
    d->type = DEV_ROM;
    d->data = malloc(size);
    memcpy(d->data, data, size);
    d->size = size;
    d->base = (uint16_t)(page * 0x4000);
    for (int p = page; p < page + pages; p++) m->map[prim][sec][p] = d;
    return true;
}

bool msx_add_ram(msx *m, int prim, int sec, int page, int pages) {
    if (!valid_place(prim, sec, page, pages) || m->ndevices >= MAX_DEVICES) return false;
    device *d = &m->devices[m->ndevices++];
    memset(d, 0, sizeof *d);
    d->type = DEV_RAM;
    d->size = 0x10000;
    d->data = calloc(1, 0x10000);
    for (int p = page; p < page + pages; p++) m->map[prim][sec][p] = d;
    return true;
}

bool msx_set_vdp(msx *m, msx_vdp_type type) {
    free(m->v2);
    m->v2 = NULL;
    if (type != MSX_VDP_TMS9918) {
        m->v2 = malloc(sizeof *m->v2);
        if (!m->v2) return false;
        v9938_init(m->v2, type == MSX_VDP_V9958);
    }
    return true;
}

bool msx_add_mapper_ram(msx *m, int prim, int sec, int kb) {
    if (!valid_place(prim, sec, 0, 4) || kb < 64 || (kb & (kb - 1)) || kb > 4096 || m->mapper_segments ||
        m->ndevices >= MAX_DEVICES) return false;
    device *d = &m->devices[m->ndevices++];
    memset(d, 0, sizeof *d);
    d->type = DEV_MAPPER;
    d->size = (size_t)kb * 1024;
    d->data = calloc(1, d->size);
    m->mapper_segments = (uint32_t)(kb / 16);
    for (int p = 0; p < 4; p++) m->map[prim][sec][p] = d;
    return true;
}

bool msx_add_kanji_rom(msx *m, const void *data, size_t size) {
    if (!data || size < 0x20000) return false;
    free(m->kanji);
    m->kanji = malloc(size);
    memcpy(m->kanji, data, size);
    m->kanji_size = size;
    return true;
}

bool msx_add_cartridge_slot(msx *m, int index, int prim, int sec) {
    if (index < 0 || index > 1 || !valid_place(prim, sec, 0, 4)) return false;
    m->cart[index].prim = prim;
    m->cart[index].sec = sec;
    m->cart[index].used = true;
    return true;
}

void msx_eject_cartridge(msx *m, int index) {
    if (index < 0 || index > 1 || !m->cart[index].used) return;
    device *d = m->cart[index].dev;
    if (!d) return;
    for (int p = 0; p < 4; p++) m->map[m->cart[index].prim][m->cart[index].sec][p] = NULL;
    free(d->data);
    d->data = NULL;
    d->type = -1;          // the device entry stays allocated but unused
    m->cart[index].dev = NULL;
}

int msx_insert_cartridge(msx *m, int index, const void *data, size_t size, msx_mapper mapper) {
    if (index < 0 || index > 1 || !m->cart[index].used || !data || size == 0 || size > 0x400000) return -1;
    msx_eject_cartridge(m, index);
    device *d = NULL;
    for (int i = 0; i < m->ndevices; i++)
        if (m->devices[i].type == -1) d = &m->devices[i];
    if (!d) {
        if (m->ndevices >= MAX_DEVICES) return -1;
        d = &m->devices[m->ndevices++];
    }
    memset(d, 0, sizeof *d);
    d->type = DEV_CART;
    d->data = malloc(size);
    memcpy(d->data, data, size);
    d->size = size;
    const uint8_t *b = data;
    d->mapper = mapper == MSX_MAPPER_AUTO ? guess_mapper(b, size) : mapper;

    if (d->mapper == MSX_MAPPER_PLAIN) {
        // Place by the "AB" header and its INIT address, like openMSX's ROM loader.
        d->base = 0x4000;
        if (size <= 0x4000) {
            uint16_t init = (size >= 4 && b[0] == 'A' && b[1] == 'B') ? (uint16_t)(b[2] | (b[3] << 8)) : 0;
            uint16_t text = (size >= 10 && b[0] == 'A' && b[1] == 'B') ? (uint16_t)(b[8] | (b[9] << 8)) : 0;
            if ((init >= 0x8000 && init < 0xc000) || (!init && text >= 0x8000 && text < 0xc000)) d->base = 0x8000;
        } else {
            bool header0 = b[0] == 'A' && b[1] == 'B';
            bool header1 = b[0x4000] == 'A' && b[0x4001] == 'B';
            if (header1 && !header0) d->base = 0x0000;              // ROM starts in page 0
            else if (size > 0xc000 || (size > 0x8000 && !header0)) d->base = 0x0000;
        }
    } else {
        uint32_t banks = 1;
        while (banks * 0x2000 < size) banks <<= 1;
        d->bank_mask = banks - 1;
        for (int i = 0; i < 4; i++)
            d->bank[i] = (d->mapper == MSX_MAPPER_KONAMI || d->mapper == MSX_MAPPER_KONAMI_SCC) ? (uint8_t)i : 0;
        if (d->mapper == MSX_MAPPER_ASCII16) {
            d->bank[0] = 0;
            d->bank[1] = 1;
            d->bank[2] = 0;
            d->bank[3] = 1;
        }
    }
    for (int p = 0; p < 4; p++) m->map[m->cart[index].prim][m->cart[index].sec][p] = d;
    m->cart[index].dev = d;
    return d->mapper;
}

void msx_reset(msx *m) {
    z80_reset(&m->cpu);
    vdp_reset(&m->vdp);
    psg_reset(&m->psg);
    m->primary = 0;
    memset(m->secondary, 0, sizeof m->secondary);
    memset(m->keys, 0xff, sizeof m->keys);
    m->ppi_c = 0;
    m->psg_b = 0;
    m->line_budget = 0;
    for (int i = 0; i < 4; i++) m->mapper[i] = (uint8_t)(3 - i);   // as the BIOS leaves it
    m->kanji_latch[0] = m->kanji_latch[1] = 0;
    rtc_reset(&m->rtc);
    if (m->v2) v9938_reset(m->v2);
    for (int i = 0; i < 2; i++) {
        device *d = m->cart[i].dev;
        if (!d || d->mapper == MSX_MAPPER_PLAIN) continue;
        for (int b = 0; b < 4; b++)
            d->bank[b] = (d->mapper == MSX_MAPPER_KONAMI || d->mapper == MSX_MAPPER_KONAMI_SCC) ? (uint8_t)b : 0;
        if (d->mapper == MSX_MAPPER_ASCII16) { d->bank[1] = 1; d->bank[3] = 1; }
        d->scc_active = false;
        memset(&d->scc, 0, sizeof d->scc);
    }
}

static void run_frame_v9938(msx *m) {
    v9938 *v = m->v2;
    for (int line = 0; line < MSX_LINES_PER_FRAME; line++) {
        v9938_line(v);      // flags, interrupts and rendering at the start of the line
        m->cpu.irq = v->irq;
        m->line_budget += MSX_CYCLES_PER_LINE;
        while (m->line_budget > 0) {
            v->hpos = MSX_CYCLES_PER_LINE - m->line_budget;
            int t = z80_step(&m->cpu);
            m->line_budget -= t;
            audio_advance(m, t);
            m->cpu.irq = v->irq;
        }
    }
}

void msx_run_frame(msx *m) {
    if (m->v2) {
        run_frame_v9938(m);
        return;
    }
    for (int line = 0; line < MSX_LINES_PER_FRAME; line++) {
        m->line_budget += MSX_CYCLES_PER_LINE;
        while (m->line_budget > 0) {
            int t = z80_step(&m->cpu);
            m->line_budget -= t;
            audio_advance(m, t);
            m->cpu.irq = vdp_irq(&m->vdp);
        }
        // Lines 0-191 are the picture, then the bottom border; the top border
        // comes at the end of the frame (lines 246-261 shown as -16...-1).
        int display = line < 192 + VDP_BORDER_Y ? line : line - MSX_LINES_PER_FRAME;
        vdp_render_line(&m->vdp, display);
        if (line == VDP_VBLANK_LINE - 1) {
            vdp_vblank(&m->vdp);
            m->cpu.irq = vdp_irq(&m->vdp);
        }
    }
}

const uint32_t *msx_framebuffer(const msx *m) { return m->v2 ? m->v2->fb : m->vdp.fb; }
int msx_screen_width(const msx *m) { return m->v2 ? V9938_WIDTH : VDP_WIDTH; }
int msx_screen_height(const msx *m) { return m->v2 ? V9938_HEIGHT : VDP_HEIGHT; }

size_t msx_read_audio(msx *m, int16_t *out, size_t max) {
    size_t n = m->audio_len < max ? m->audio_len : max;
    memcpy(out, m->audio, n * sizeof *out);
    memmove(m->audio, m->audio + n, (m->audio_len - n) * sizeof *out);
    m->audio_len -= n;
    return n;
}

void msx_key(msx *m, int row, int bit, bool down) {
    if (row < 0 || row > 10 || bit < 0 || bit > 7) return;
    if (down) m->keys[row] &= (uint8_t)~(1 << bit);
    else m->keys[row] |= (uint8_t)(1 << bit);
}

void msx_release_all_keys(msx *m) { memset(m->keys, 0xff, sizeof m->keys); }

void msx_joystick(msx *m, int port, uint8_t pressed) {
    if (port == 0 || port == 1) m->joy[port] = pressed & 0x3f;
}

bool msx_caps_led(const msx *m) { return !(m->ppi_c & 0x40); }
bool msx_code_led(const msx *m) { return !(m->psg_b & 0x80); }
uint16_t msx_pc(const msx *m) { return m->cpu.pc.w; }
uint8_t msx_peek(msx *m, uint16_t addr) { return mem_read(m, addr); }
const uint8_t *msx_vram(const msx *m) { return m->v2 ? m->v2->vram : m->vdp.vram; }

uint32_t msx_crc32(const void *data, size_t size) {
    static uint32_t table[256];
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    }
    const uint8_t *p = data;
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; i++) crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return crc ^ 0xffffffffu;
}
