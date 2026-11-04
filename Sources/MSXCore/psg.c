// AY-3-8910: three square-wave tones, one noise generator (17-bit LFSR at half
// rate) and a 16-step envelope, mixed as (tone | tone_off) & (noise | noise_off).
#include "psg.h"

#include <string.h>

// Logarithmic DAC levels (about 3 dB per step), normalised to 1.0.
static const float volume[16] = {
    0.0f,     0.00999f, 0.01445f, 0.02106f, 0.03070f, 0.04555f, 0.06450f, 0.10736f,
    0.12659f, 0.20499f, 0.29221f, 0.37284f, 0.49253f, 0.63532f, 0.80558f, 1.0f,
};

static const uint8_t reg_mask[16] = {
    0xff, 0x0f, 0xff, 0x0f, 0xff, 0x0f, 0x1f, 0xff, 0x1f, 0x1f, 0x1f, 0xff, 0xff, 0x0f, 0xff, 0xff,
};

static void set_shape(psg *p, uint8_t shape) {
    p->env_attack = (shape & 0x04) ? 0x0f : 0x00;
    if (!(shape & 0x08)) {
        p->env_hold = true;
        p->env_alternate = p->env_attack != 0;
    } else {
        p->env_hold = shape & 0x01;
        p->env_alternate = shape & 0x02;
    }
    p->env_step = 0x0f;
    p->env_holding = false;
    p->env_count = 0;
}

void psg_reset(psg *p) {
    memset(p->regs, 0, sizeof p->regs);
    p->latch = 0;
    memset(p->tone_count, 0, sizeof p->tone_count);
    memset(p->tone_out, 0, sizeof p->tone_out);
    p->noise_count = 0;
    p->noise_prescale = 0;
    p->rng = 1;
    set_shape(p, 0);
    p->env_holding = true;
    p->env_step = 0;
}

void psg_address(psg *p, uint8_t reg) {
    p->latch = reg & 0x0f;
}

void psg_write(psg *p, uint8_t value) {
    int r = p->latch;
    p->regs[r] = value & reg_mask[r];
    if (r == 13) set_shape(p, p->regs[13]);
    if (r == 15 && p->port_b_write) p->port_b_write(p->ctx, value);
}

uint8_t psg_read(psg *p) {
    int r = p->latch;
    if (r == 14) return p->port_a_read ? p->port_a_read(p->ctx) : 0xff;
    return p->regs[r];
}

float psg_tick(psg *p) {
    const uint8_t *r = p->regs;
    for (int ch = 0; ch < 3; ch++) {
        int period = r[ch * 2] | ((r[ch * 2 + 1] & 0x0f) << 8);
        if (period < 1) period = 1;
        if (++p->tone_count[ch] >= period) {
            p->tone_count[ch] = 0;
            p->tone_out[ch] ^= 1;
        }
    }

    int np = r[6] & 0x1f;
    if (np < 1) np = 1;
    if (++p->noise_count >= np) {
        p->noise_count = 0;
        p->noise_prescale ^= 1;
        if (!p->noise_prescale) {
            p->rng ^= (((p->rng & 1) ^ ((p->rng >> 3) & 1)) << 17);
            p->rng >>= 1;
        }
    }
    uint8_t noise = p->rng & 1;

    if (!p->env_holding) {
        int ep = r[11] | (r[12] << 8);
        if (ep < 1) ep = 1;
        if (++p->env_count >= ep * 2) {
            p->env_count = 0;
            if (--p->env_step < 0) {
                if (p->env_hold) {
                    if (p->env_alternate) p->env_attack ^= 0x0f;
                    p->env_holding = true;
                    p->env_step = 0;
                } else {
                    if (p->env_alternate) p->env_attack ^= 0x0f;
                    p->env_step &= 0x0f;
                }
            }
        }
    }
    int env = (p->env_step ^ p->env_attack) & 0x0f;

    float out = 0;
    uint8_t mixer = r[7];
    for (int ch = 0; ch < 3; ch++) {
        bool on = (p->tone_out[ch] | ((mixer >> ch) & 1)) & (noise | ((mixer >> (ch + 3)) & 1));
        if (!on) continue;
        uint8_t amp = r[8 + ch];
        out += volume[(amp & 0x10) ? env : (amp & 0x0f)];
    }
    return out;
}
