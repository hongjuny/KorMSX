// Zilog Z80 CPU core: all documented and undocumented opcodes, the X/Y flag
// bits and MEMPTR (WZ), with an optional extra wait state on every M1 cycle
// (an MSX inserts one).
#ifndef Z80_H
#define Z80_H

#include <stdbool.h>
#include <stdint.h>

typedef uint8_t (*z80_read_fn)(void *ctx, uint16_t addr);
typedef void (*z80_write_fn)(void *ctx, uint16_t addr, uint8_t value);

typedef union {
    uint16_t w;
    struct { uint8_t l, h; } b;   // little-endian hosts only (arm64, x86-64)
} z80_pair;

typedef struct z80 {
    z80_pair af, bc, de, hl, ix, iy, sp, pc, wz;
    z80_pair af2, bc2, de2, hl2;
    uint8_t i, r;            // r holds all 8 bits; bit 7 is kept by refresh
    bool iff1, iff2;
    uint8_t im;
    bool halted;
    bool ei_delay;           // no interrupt right after EI
    bool irq;                // level of the /INT line
    bool nmi;                // pending NMI (edge)
    uint8_t q;               // flags changed by the last instruction (for SCF/CCF)
    bool fw;                 // the current instruction wrote F
    int m1_wait;             // extra cycles per M1 (1 on an MSX)
    uint64_t cycles;         // total T-states executed

    void *ctx;
    z80_read_fn read, in;
    z80_write_fn write, out;
} z80;

void z80_init(z80 *cpu, void *ctx, z80_read_fn read, z80_write_fn write, z80_read_fn in, z80_write_fn out);
void z80_reset(z80 *cpu);
/// Executes one instruction (or accepts an interrupt) and returns its T-states.
int z80_step(z80 *cpu);

#endif
