// General Instrument AY-3-8910 PSG. Steps at clock/8 like MAME's ay8910.cpp.
#ifndef PSG_H
#define PSG_H

#include <stdbool.h>
#include <stdint.h>

typedef struct psg {
    uint8_t regs[16];
    uint8_t latch;              // selected register
    int tone_count[3];
    uint8_t tone_out[3];
    int noise_count;
    uint8_t noise_prescale;
    uint32_t rng;
    int env_count;
    int env_step;
    uint8_t env_attack;
    bool env_hold, env_alternate, env_holding;

    // I/O ports: port A is an input, port B an output on an MSX.
    void *ctx;
    uint8_t (*port_a_read)(void *ctx);
    void (*port_b_write)(void *ctx, uint8_t value);
} psg;

void psg_reset(psg *p);
void psg_address(psg *p, uint8_t reg);
void psg_write(psg *p, uint8_t value);
uint8_t psg_read(psg *p);
/// Advances one step (clock/8) and returns the mixed output, 0...3.
float psg_tick(psg *p);

#endif
