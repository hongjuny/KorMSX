// Z80 core. Opcodes are decoded by their x/y/z/p/q fields; DD/FD prefixes
// select IX/IY in place of HL. Flag behaviour (including X/Y, MEMPTR and the
// Q latch used by SCF/CCF) follows the documented Zilog NMOS behaviour that
// ZEXALL checks.
#include "z80.h"

#include <string.h>

enum { FC = 0x01, FN = 0x02, FP = 0x04, FX = 0x08, FH = 0x10, FY = 0x20, FZ = 0x40, FS = 0x80 };

static uint8_t sz53[256], sz53p[256];
static bool tables_ready;

static void init_tables(void) {
    for (int i = 0; i < 256; i++) {
        uint8_t f = (uint8_t)(i & (FS | FX | FY));
        if (i == 0) f |= FZ;
        int bits = 0;
        for (int b = 0; b < 8; b++) bits += (i >> b) & 1;
        sz53[i] = f;
        sz53p[i] = f | ((bits & 1) ? 0 : FP);
    }
    tables_ready = true;
}

#define A  cpu->af.b.h
#define F  cpu->af.b.l
#define B  cpu->bc.b.h
#define C  cpu->bc.b.l
#define D  cpu->de.b.h
#define E  cpu->de.b.l
#define H  cpu->hl.b.h
#define L  cpu->hl.b.l
#define BC cpu->bc.w
#define DE cpu->de.w
#define HL cpu->hl.w
#define SP cpu->sp.w
#define PC cpu->pc.w
#define WZ cpu->wz.w

// Flag writes go through SETF so the Q latch knows the instruction changed F.
#define SETF(v) do { F = (uint8_t)(v); cpu->fw = true; } while (0)

static inline uint8_t rd(z80 *cpu, uint16_t a) { return cpu->read(cpu->ctx, a); }
static inline void wr(z80 *cpu, uint16_t a, uint8_t v) { cpu->write(cpu->ctx, a, v); }
static inline uint8_t imm8(z80 *cpu) { return rd(cpu, PC++); }
static inline uint16_t imm16(z80 *cpu) {
    uint8_t lo = imm8(cpu);
    return (uint16_t)(lo | (imm8(cpu) << 8));
}
static inline uint8_t fetch_op(z80 *cpu) {
    cpu->r = (uint8_t)((cpu->r & 0x80) | ((cpu->r + 1) & 0x7f));
    return rd(cpu, PC++);
}
static inline void push(z80 *cpu, uint16_t v) {
    wr(cpu, --SP, (uint8_t)(v >> 8));
    wr(cpu, --SP, (uint8_t)v);
}
static inline uint16_t pop(z80 *cpu) {
    uint8_t lo = rd(cpu, SP++);
    return (uint16_t)(lo | (rd(cpu, SP++) << 8));
}

// ---- ALU -------------------------------------------------------------------

static void alu(z80 *cpu, int op, uint8_t v) {
    unsigned a = A, res, c;
    switch (op) {
    case 0: case 1:   // ADD, ADC
        c = (op == 1) ? (F & FC) : 0;
        res = a + v + c;
        A = (uint8_t)res;
        SETF(sz53[A] | ((res >> 8) & FC) | ((a ^ v ^ res) & FH) | ((((a ^ ~v) & (a ^ res)) & 0x80) >> 5));
        break;
    case 2: case 3: case 7:   // SUB, SBC, CP
        c = (op == 3) ? (F & FC) : 0;
        res = a - v - c;
        SETF((op == 7 ? (sz53[res & 0xff] & ~(FX | FY)) | (v & (FX | FY)) : sz53[res & 0xff]) |
             FN | ((res >> 8) & FC) | ((a ^ v ^ res) & FH) | ((((a ^ v) & (a ^ res)) & 0x80) >> 5));
        if (op != 7) A = (uint8_t)res;
        break;
    case 4: A &= v; SETF(sz53p[A] | FH); break;
    case 5: A ^= v; SETF(sz53p[A]); break;
    case 6: A |= v; SETF(sz53p[A]); break;
    }
}

static uint8_t inc8(z80 *cpu, uint8_t v) {
    uint8_t r = (uint8_t)(v + 1);
    SETF((F & FC) | sz53[r] | (v == 0x7f ? FP : 0) | ((r & 0x0f) == 0 ? FH : 0));
    return r;
}

static uint8_t dec8(z80 *cpu, uint8_t v) {
    uint8_t r = (uint8_t)(v - 1);
    SETF((F & FC) | FN | sz53[r] | (v == 0x80 ? FP : 0) | ((v & 0x0f) == 0 ? FH : 0));
    return r;
}

static uint16_t add16(z80 *cpu, uint16_t a, uint16_t b) {
    uint32_t res = (uint32_t)a + b;
    WZ = (uint16_t)(a + 1);
    SETF((F & (FS | FZ | FP)) | ((res >> 16) & FC) | ((res >> 8) & (FX | FY)) | (((a ^ b ^ res) >> 8) & FH));
    return (uint16_t)res;
}

static void adc16(z80 *cpu, uint16_t v) {
    uint16_t a = HL;
    uint32_t res = (uint32_t)a + v + (F & FC);
    WZ = (uint16_t)(a + 1);
    HL = (uint16_t)res;
    SETF(((res >> 16) & FC) | (((a ^ v ^ res) >> 8) & FH) | ((((a ^ ~v) & (a ^ res)) & 0x8000) >> 13) |
         ((res >> 8) & (FS | FX | FY)) | ((res & 0xffff) ? 0 : FZ));
}

static void sbc16(z80 *cpu, uint16_t v) {
    uint16_t a = HL;
    uint32_t res = (uint32_t)a - v - (F & FC);
    WZ = (uint16_t)(a + 1);
    HL = (uint16_t)res;
    SETF(FN | ((res >> 16) & FC) | (((a ^ v ^ res) >> 8) & FH) | ((((a ^ v) & (a ^ res)) & 0x8000) >> 13) |
         ((res >> 8) & (FS | FX | FY)) | ((res & 0xffff) ? 0 : FZ));
}

// CB-prefix rotate/shift group, op 0..7 = RLC RRC RL RR SLA SRA SLL SRL.
static uint8_t rot(z80 *cpu, int op, uint8_t v) {
    uint8_t r, c;
    switch (op) {
    case 0: c = v >> 7; r = (uint8_t)((v << 1) | c); break;
    case 1: c = v & 1; r = (uint8_t)((v >> 1) | (c << 7)); break;
    case 2: c = v >> 7; r = (uint8_t)((v << 1) | (F & FC)); break;
    case 3: c = v & 1; r = (uint8_t)((v >> 1) | ((F & FC) << 7)); break;
    case 4: c = v >> 7; r = (uint8_t)(v << 1); break;
    case 5: c = v & 1; r = (uint8_t)((v >> 1) | (v & 0x80)); break;
    case 6: c = v >> 7; r = (uint8_t)((v << 1) | 1); break;
    default: c = v & 1; r = v >> 1; break;
    }
    SETF(sz53p[r] | c);
    return r;
}

// BIT n: X/Y come from `xy` (the register, or MEMPTR/address high byte for memory forms).
static void bit(z80 *cpu, int n, uint8_t v, uint8_t xy) {
    uint8_t m = v & (uint8_t)(1 << n);
    uint8_t f = (F & FC) | FH | (xy & (FX | FY));
    if (!m) f |= FZ | FP;
    if (n == 7 && m) f |= FS;
    SETF(f);
}

static void daa(z80 *cpu) {
    uint8_t a = A, corr = 0, c = F & FC, h;
    if ((F & FH) || (a & 0x0f) > 9) corr |= 0x06;
    if (c || a > 0x99) { corr |= 0x60; c = FC; }
    if (F & FN) {
        h = ((F & FH) && (a & 0x0f) < 6) ? FH : 0;
        A = (uint8_t)(a - corr);
    } else {
        h = ((a & 0x0f) > 9) ? FH : 0;
        A = (uint8_t)(a + corr);
    }
    SETF(sz53p[A] | (F & FN) | c | h);
}

// ---- register access ---------------------------------------------------------

// idx: 0 = HL, 1 = IX, 2 = IY.
static inline z80_pair *hlreg(z80 *cpu, int idx) {
    return idx == 0 ? &cpu->hl : (idx == 1 ? &cpu->ix : &cpu->iy);
}

// 8-bit register by r field (not 6). With an index prefix, H/L become IXH/IXL.
static inline uint8_t *reg8(z80 *cpu, int r, int idx) {
    switch (r) {
    case 0: return &B;
    case 1: return &C;
    case 2: return &D;
    case 3: return &E;
    case 4: return &hlreg(cpu, idx)->b.h;
    case 5: return &hlreg(cpu, idx)->b.l;
    default: return &A;
    }
}

static inline uint16_t *rp(z80 *cpu, int p, int idx) {
    switch (p) {
    case 0: return &BC;
    case 1: return &DE;
    case 2: return &hlreg(cpu, idx)->w;
    default: return &SP;
    }
}

static inline uint16_t *rp2(z80 *cpu, int p, int idx) {
    return p == 3 ? &cpu->af.w : rp(cpu, p, idx);
}

static inline bool cond(z80 *cpu, int y) {
    switch (y) {
    case 0: return !(F & FZ);
    case 1: return F & FZ;
    case 2: return !(F & FC);
    case 3: return F & FC;
    case 4: return !(F & FP);
    case 5: return F & FP;
    case 6: return !(F & FS);
    default: return F & FS;
    }
}

// Address of the (HL) operand: HL, or IX/IY plus a fetched displacement.
static inline uint16_t mem_addr(z80 *cpu, int idx) {
    if (idx == 0) return HL;
    int8_t d = (int8_t)imm8(cpu);
    uint16_t a = (uint16_t)(hlreg(cpu, idx)->w + d);
    WZ = a;
    return a;
}

// ---- prefixed groups -------------------------------------------------------

static int exec_cb(z80 *cpu, int idx) {
    int t = 0;
    uint16_t addr = 0;
    uint8_t op;
    if (idx) {
        addr = mem_addr(cpu, idx);
        op = imm8(cpu);               // not an M1 cycle, R not incremented
    } else {
        op = fetch_op(cpu);
        t = cpu->m1_wait;
    }
    int x = op >> 6, y = (op >> 3) & 7, z = op & 7;

    if (idx || z == 6) {
        if (!idx) addr = HL;
        uint8_t v = rd(cpu, addr);
        if (x == 1) {
            bit(cpu, y, v, idx ? (uint8_t)(addr >> 8) : cpu->wz.b.h);
            return t + (idx ? 12 : 12 - 4);
        }
        uint8_t r = x == 0 ? rot(cpu, y, v) : x == 2 ? (uint8_t)(v & ~(1 << y)) : (uint8_t)(v | (1 << y));
        wr(cpu, addr, r);
        if (idx && z != 6) *reg8(cpu, z, 0) = r;   // undocumented copy to a register
        return t + (idx ? 15 : 15 - 4);
    }
    uint8_t *p = reg8(cpu, z, 0);
    switch (x) {
    case 0: *p = rot(cpu, y, *p); break;
    case 1: bit(cpu, y, *p, *p); break;
    case 2: *p &= (uint8_t)~(1 << y); break;
    default: *p |= (uint8_t)(1 << y); break;
    }
    return t + 8 - 4;
}

static int exec_ed(z80 *cpu) {
    uint8_t op = fetch_op(cpu);
    int t = cpu->m1_wait;
    int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;

    if (x == 1) {
        switch (z) {
        case 0: {   // IN r,(C)
            uint8_t v = cpu->in(cpu->ctx, BC);
            WZ = (uint16_t)(BC + 1);
            if (y != 6) *reg8(cpu, y, 0) = v;
            SETF((F & FC) | sz53p[v]);
            return t + 12 - 4;
        }
        case 1:     // OUT (C),r
            cpu->out(cpu->ctx, BC, y == 6 ? 0 : *reg8(cpu, y, 0));
            WZ = (uint16_t)(BC + 1);
            return t + 12 - 4;
        case 2:
            if (q) adc16(cpu, *rp(cpu, p, 0)); else sbc16(cpu, *rp(cpu, p, 0));
            return t + 15 - 4;
        case 3: {
            uint16_t nn = imm16(cpu);
            uint16_t *r = rp(cpu, p, 0);
            if (q) {
                *r = (uint16_t)(rd(cpu, nn) | (rd(cpu, (uint16_t)(nn + 1)) << 8));
            } else {
                wr(cpu, nn, (uint8_t)*r);
                wr(cpu, (uint16_t)(nn + 1), (uint8_t)(*r >> 8));
            }
            WZ = (uint16_t)(nn + 1);
            return t + 20 - 4;
        }
        case 4: {   // NEG
            uint8_t v = A;
            A = 0;
            alu(cpu, 2, v);
            return t + 8 - 4;
        }
        case 5:     // RETN / RETI
            cpu->iff1 = cpu->iff2;
            PC = WZ = pop(cpu);
            return t + 14 - 4;
        case 6: {
            static const uint8_t modes[8] = { 0, 0, 1, 2, 0, 0, 1, 2 };
            cpu->im = modes[y];
            return t + 8 - 4;
        }
        default:
            switch (y) {
            case 0: cpu->i = A; return t + 9 - 4;
            case 1: cpu->r = A; return t + 9 - 4;
            case 2: case 3:
                A = (y == 2) ? cpu->i : cpu->r;
                SETF((F & FC) | sz53[A] | (cpu->iff2 ? FP : 0));
                return t + 9 - 4;
            case 4: case 5: {   // RRD / RLD
                uint8_t v = rd(cpu, HL);
                if (y == 4) {
                    wr(cpu, HL, (uint8_t)((A << 4) | (v >> 4)));
                    A = (uint8_t)((A & 0xf0) | (v & 0x0f));
                } else {
                    wr(cpu, HL, (uint8_t)((v << 4) | (A & 0x0f)));
                    A = (uint8_t)((A & 0xf0) | (v >> 4));
                }
                WZ = (uint16_t)(HL + 1);
                SETF((F & FC) | sz53p[A]);
                return t + 18 - 4;
            }
            default:
                return t + 8 - 4;
            }
        }
    }

    if (x == 2 && z <= 3 && y >= 4) {
        bool dec = y & 1, repeat = y >= 6;
        int dir = dec ? -1 : 1;
        switch (z) {
        case 0: {   // LDI/LDD/LDIR/LDDR
            uint8_t v = rd(cpu, HL);
            wr(cpu, DE, v);
            HL = (uint16_t)(HL + dir);
            DE = (uint16_t)(DE + dir);
            BC--;
            uint8_t n = (uint8_t)(v + A);
            SETF((F & (FS | FZ | FC)) | (BC ? FP : 0) | (n & FX) | ((n << 4) & FY));
            if (repeat && BC) {
                PC -= 2;
                WZ = (uint16_t)(PC + 1);
                return t + 21 - 4;
            }
            return t + 16 - 4;
        }
        case 1: {   // CPI/CPD/CPIR/CPDR
            uint8_t v = rd(cpu, HL);
            uint8_t res = (uint8_t)(A - v);
            uint8_t h = (A ^ v ^ res) & FH;
            uint8_t n = (uint8_t)(res - (h ? 1 : 0));
            HL = (uint16_t)(HL + dir);
            WZ = (uint16_t)(WZ + dir);
            BC--;
            SETF((F & FC) | FN | (sz53[res] & (FS | FZ)) | h | (BC ? FP : 0) | (n & FX) | ((n << 4) & FY));
            if (repeat && BC && res) {
                PC -= 2;
                WZ = (uint16_t)(PC + 1);
                return t + 21 - 4;
            }
            return t + 16 - 4;
        }
        case 2: {   // INI/IND/INIR/INDR
            uint8_t v = cpu->in(cpu->ctx, BC);
            WZ = (uint16_t)(BC + dir);
            B--;
            wr(cpu, HL, v);
            HL = (uint16_t)(HL + dir);
            unsigned k = v + (uint8_t)(C + dir);
            SETF(sz53[B] | ((v & 0x80) ? FN : 0) | (k > 0xff ? (FH | FC) : 0) | (sz53p[(k & 7) ^ B] & FP));
            if (repeat && B) {
                PC -= 2;
                return t + 21 - 4;
            }
            return t + 16 - 4;
        }
        default: {  // OUTI/OUTD/OTIR/OTDR
            uint8_t v = rd(cpu, HL);
            B--;
            WZ = (uint16_t)(BC + dir);
            cpu->out(cpu->ctx, BC, v);
            HL = (uint16_t)(HL + dir);
            unsigned k = v + L;
            SETF(sz53[B] | ((v & 0x80) ? FN : 0) | (k > 0xff ? (FH | FC) : 0) | (sz53p[(k & 7) ^ B] & FP));
            if (repeat && B) {
                PC -= 2;
                return t + 21 - 4;
            }
            return t + 16 - 4;
        }
        }
    }
    return t + 8 - 4;   // undefined ED opcodes act as two NOPs
}

// ---- main decoder ------------------------------------------------------------

// Executes an unprefixed opcode `op` (already fetched) with index mode idx.
// Returns T-states, counting 4 for the opcode fetch itself (prefixes add theirs).
static int exec_main(z80 *cpu, uint8_t op, int idx) {
    int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
    // Memory-operand forms with IX/IY take extra cycles for the displacement.
    const int ix_mem = idx ? 8 : 0;

    switch (x) {
    case 0:
        switch (z) {
        case 0:
            switch (y) {
            case 0: return 4;
            case 1: {
                uint16_t tmp = cpu->af.w;
                cpu->af.w = cpu->af2.w;
                cpu->af2.w = tmp;
                return 4;
            }
            case 2: {
                int8_t d = (int8_t)imm8(cpu);
                if (--B) {
                    PC = WZ = (uint16_t)(PC + d);
                    return 13;
                }
                return 8;
            }
            case 3: {
                int8_t d = (int8_t)imm8(cpu);
                PC = WZ = (uint16_t)(PC + d);
                return 12;
            }
            default: {
                int8_t d = (int8_t)imm8(cpu);
                if (cond(cpu, y - 4)) {
                    PC = WZ = (uint16_t)(PC + d);
                    return 12;
                }
                return 7;
            }
            }
        case 1:
            if (!q) {
                *rp(cpu, p, idx) = imm16(cpu);
                return 10;
            } else {
                z80_pair *h = hlreg(cpu, idx);
                h->w = add16(cpu, h->w, *rp(cpu, p, idx));
                return 11;
            }
        case 2:
            switch (y) {
            case 0: wr(cpu, BC, A); WZ = (uint16_t)(((BC + 1) & 0xff) | (A << 8)); return 7;
            case 1: A = rd(cpu, BC); WZ = (uint16_t)(BC + 1); return 7;
            case 2: wr(cpu, DE, A); WZ = (uint16_t)(((DE + 1) & 0xff) | (A << 8)); return 7;
            case 3: A = rd(cpu, DE); WZ = (uint16_t)(DE + 1); return 7;
            case 4: {
                uint16_t nn = imm16(cpu);
                z80_pair *h = hlreg(cpu, idx);
                wr(cpu, nn, h->b.l);
                wr(cpu, (uint16_t)(nn + 1), h->b.h);
                WZ = (uint16_t)(nn + 1);
                return 16;
            }
            case 5: {
                uint16_t nn = imm16(cpu);
                z80_pair *h = hlreg(cpu, idx);
                h->b.l = rd(cpu, nn);
                h->b.h = rd(cpu, (uint16_t)(nn + 1));
                WZ = (uint16_t)(nn + 1);
                return 16;
            }
            case 6: {
                uint16_t nn = imm16(cpu);
                wr(cpu, nn, A);
                WZ = (uint16_t)(((nn + 1) & 0xff) | (A << 8));
                return 13;
            }
            default: {
                uint16_t nn = imm16(cpu);
                A = rd(cpu, nn);
                WZ = (uint16_t)(nn + 1);
                return 13;
            }
            }
        case 3:
            if (q) (*rp(cpu, p, idx))--; else (*rp(cpu, p, idx))++;
            return 6;
        case 4: case 5:
            if (y == 6) {
                uint16_t a = mem_addr(cpu, idx);
                uint8_t v = rd(cpu, a);
                wr(cpu, a, z == 4 ? inc8(cpu, v) : dec8(cpu, v));
                return 11 + ix_mem;
            } else {
                uint8_t *r = reg8(cpu, y, idx);
                *r = z == 4 ? inc8(cpu, *r) : dec8(cpu, *r);
                return 4;
            }
        case 6:
            if (y == 6) {
                uint16_t a = mem_addr(cpu, idx);
                wr(cpu, a, imm8(cpu));
                return idx ? 15 : 10;
            }
            *reg8(cpu, y, idx) = imm8(cpu);
            return 7;
        default:
            switch (y) {
            case 0: A = (uint8_t)((A << 1) | (A >> 7)); SETF((F & (FS | FZ | FP)) | (A & (FX | FY | FC))); break;
            case 1: {
                uint8_t c = A & 1;
                A = (uint8_t)((A >> 1) | (c << 7));
                SETF((F & (FS | FZ | FP)) | (A & (FX | FY)) | c);
                break;
            }
            case 2: {
                uint8_t c = A >> 7;
                A = (uint8_t)((A << 1) | (F & FC));
                SETF((F & (FS | FZ | FP)) | (A & (FX | FY)) | c);
                break;
            }
            case 3: {
                uint8_t c = A & 1;
                A = (uint8_t)((A >> 1) | ((F & FC) << 7));
                SETF((F & (FS | FZ | FP)) | (A & (FX | FY)) | c);
                break;
            }
            case 4: daa(cpu); break;
            case 5: A = (uint8_t)~A; SETF((F & (FS | FZ | FP | FC)) | FH | FN | (A & (FX | FY))); break;
            case 6: SETF((F & (FS | FZ | FP)) | FC | (((cpu->q ^ F) | A) & (FX | FY))); break;
            default:
                SETF(((F & (FS | FZ | FP | FC)) | ((F & FC) ? FH : 0) | (((cpu->q ^ F) | A) & (FX | FY))) ^ FC);
                break;
            }
            return 4;
        }

    case 1:
        if (y == 6 && z == 6) {
            cpu->halted = true;
            PC--;
            return 4;
        }
        if (y == 6) {   // LD (HL),r: r is the real H/L even with a prefix
            wr(cpu, mem_addr(cpu, idx), *reg8(cpu, z, 0));
            return 7 + ix_mem;
        }
        if (z == 6) {
            uint8_t v = rd(cpu, mem_addr(cpu, idx));
            *reg8(cpu, y, 0) = v;
            return 7 + ix_mem;
        }
        *reg8(cpu, y, idx) = *reg8(cpu, z, idx);
        return 4;

    case 2:
        if (z == 6) {
            alu(cpu, y, rd(cpu, mem_addr(cpu, idx)));
            return 7 + ix_mem;
        }
        alu(cpu, y, *reg8(cpu, z, idx));
        return 4;

    default:
        switch (z) {
        case 0:
            if (cond(cpu, y)) {
                PC = WZ = pop(cpu);
                return 11;
            }
            return 5;
        case 1:
            if (!q) {
                *rp2(cpu, p, idx) = pop(cpu);
                return 10;
            }
            switch (p) {
            case 0: PC = WZ = pop(cpu); return 10;
            case 1: {
                uint16_t t;
                t = BC; BC = cpu->bc2.w; cpu->bc2.w = t;
                t = DE; DE = cpu->de2.w; cpu->de2.w = t;
                t = HL; HL = cpu->hl2.w; cpu->hl2.w = t;
                return 4;
            }
            case 2: PC = hlreg(cpu, idx)->w; return 4;
            default: SP = hlreg(cpu, idx)->w; return 6;
            }
        case 2: {
            uint16_t nn = imm16(cpu);
            WZ = nn;
            if (cond(cpu, y)) PC = nn;
            return 10;
        }
        case 3:
            switch (y) {
            case 0: PC = WZ = imm16(cpu); return 10;
            case 1: return 4 + exec_cb(cpu, idx);
            case 2: {
                uint8_t n = imm8(cpu);
                cpu->out(cpu->ctx, (uint16_t)(n | (A << 8)), A);
                WZ = (uint16_t)(((n + 1) & 0xff) | (A << 8));
                return 11;
            }
            case 3: {
                uint16_t port = (uint16_t)(imm8(cpu) | (A << 8));
                A = cpu->in(cpu->ctx, port);
                WZ = (uint16_t)(port + 1);
                return 11;
            }
            case 4: {
                z80_pair *h = hlreg(cpu, idx);
                uint16_t v = (uint16_t)(rd(cpu, SP) | (rd(cpu, (uint16_t)(SP + 1)) << 8));
                wr(cpu, (uint16_t)(SP + 1), h->b.h);
                wr(cpu, SP, h->b.l);
                h->w = WZ = v;
                return 19;
            }
            case 5: {
                uint16_t t = DE;
                DE = HL;
                HL = t;
                return 4;
            }
            case 6: cpu->iff1 = cpu->iff2 = false; return 4;
            default:
                cpu->iff1 = cpu->iff2 = true;
                cpu->ei_delay = true;
                return 4;
            }
        case 4: {
            uint16_t nn = imm16(cpu);
            WZ = nn;
            if (cond(cpu, y)) {
                push(cpu, PC);
                PC = nn;
                return 17;
            }
            return 10;
        }
        case 5:
            if (!q) {
                push(cpu, *rp2(cpu, p, idx));
                return 11;
            }
            if (p == 0) {
                uint16_t nn = imm16(cpu);
                push(cpu, PC);
                PC = WZ = nn;
                return 17;
            }
            return 4;   // DD/ED/FD are handled by z80_step
        case 6:
            alu(cpu, y, imm8(cpu));
            return 7;
        default:
            push(cpu, PC);
            PC = WZ = (uint16_t)(y * 8);
            return 11;
        }
    }
}

// ---- public API --------------------------------------------------------------

void z80_init(z80 *cpu, void *ctx, z80_read_fn read, z80_write_fn write, z80_read_fn in, z80_write_fn out) {
    if (!tables_ready) init_tables();
    memset(cpu, 0, sizeof *cpu);
    cpu->ctx = ctx;
    cpu->read = read;
    cpu->write = write;
    cpu->in = in;
    cpu->out = out;
    z80_reset(cpu);
}

void z80_reset(z80 *cpu) {
    PC = 0;
    cpu->i = cpu->r = 0;
    cpu->iff1 = cpu->iff2 = false;
    cpu->im = 0;
    cpu->halted = false;
    cpu->ei_delay = false;
    cpu->nmi = false;
    cpu->af.w = SP = 0xffff;
    cpu->q = 0;
}

int z80_step(z80 *cpu) {
    int t;
    cpu->fw = false;

    if (cpu->nmi) {
        cpu->nmi = false;
        cpu->halted = false;
        cpu->iff1 = false;
        cpu->r = (uint8_t)((cpu->r & 0x80) | ((cpu->r + 1) & 0x7f));
        push(cpu, PC);
        PC = WZ = 0x66;
        t = 11 + cpu->m1_wait;
        cpu->q = 0;
        cpu->cycles += (uint64_t)t;
        return t;
    }
    if (cpu->irq && cpu->iff1 && !cpu->ei_delay) {
        cpu->iff1 = cpu->iff2 = false;
        if (cpu->halted) {
            cpu->halted = false;
            PC++;
        }
        cpu->r = (uint8_t)((cpu->r & 0x80) | ((cpu->r + 1) & 0x7f));
        push(cpu, PC);
        if (cpu->im == 2) {
            // An MSX bus floats high, so the vector byte is 0xFF.
            uint16_t v = (uint16_t)((cpu->i << 8) | 0xff);
            PC = (uint16_t)(rd(cpu, v) | (rd(cpu, (uint16_t)(v + 1)) << 8));
            t = 19;
        } else {
            PC = 0x38;   // IM 1, and IM 0 with RST 38h on the bus
            t = 13;
        }
        WZ = PC;
        t += cpu->m1_wait;
        cpu->q = 0;
        cpu->cycles += (uint64_t)t;
        return t;
    }
    cpu->ei_delay = false;

    int idx = 0;
    t = 0;
    uint8_t op = fetch_op(cpu);
    t += cpu->m1_wait;
    while (op == 0xdd || op == 0xfd) {
        idx = (op == 0xdd) ? 1 : 2;
        t += 4;
        op = fetch_op(cpu);
        t += cpu->m1_wait;
    }
    if (op == 0xed) {
        t += 4 + exec_ed(cpu);
    } else {
        t += exec_main(cpu, op, idx);
    }
    cpu->q = cpu->fw ? F : 0;
    cpu->cycles += (uint64_t)t;
    return t;
}
