// Runs ZEXDOC/ZEXALL (CP/M .COM programs) on the Z80 core with a minimal
// BDOS: function 2 (print char) and 9 (print $-terminated string).
// Usage: zextest tests/data/zexdoc.com
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../Sources/MSXCore/z80.h"

static uint8_t mem[65536];

static uint8_t rd(void *c, uint16_t a) { (void)c; return mem[a]; }
static void wr(void *c, uint16_t a, uint8_t v) { (void)c; mem[a] = v; }
static uint8_t in(void *c, uint16_t p) { (void)c; (void)p; return 0xff; }
static void out(void *c, uint16_t p, uint8_t v) { (void)c; (void)p; (void)v; }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s file.com\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    size_t n = fread(mem + 0x100, 1, sizeof mem - 0x100, f);
    fclose(f);
    (void)n;
    mem[0] = 0x76;                 // warm boot: HALT
    mem[5] = 0xc9;                 // BDOS entry: RET (handled below)
    z80 cpu;
    z80_init(&cpu, NULL, rd, wr, in, out);
    cpu.pc.w = 0x100;
    cpu.sp.w = 0xf000;
    int errors = 0;
    for (;;) {
        if (cpu.pc.w == 5) {
            if (cpu.bc.b.l == 2) {
                putchar(cpu.de.b.l);
            } else if (cpu.bc.b.l == 9) {
                for (uint16_t a = cpu.de.w; mem[a] != '$'; a++) putchar(mem[a]);
            }
            fflush(stdout);
        }
        if (cpu.pc.w == 0) break;
        z80_step(&cpu);
    }
    (void)errors;
    printf("\ncycles: %llu\n", (unsigned long long)cpu.cycles);
    return 0;
}
