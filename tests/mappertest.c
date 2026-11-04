// Unit checks for slot selection, sub-slots and cartridge mappers. Includes
// msx.c directly to drive the memory and I/O handlers without a BIOS.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../Sources/MSXCore/msx.c"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// A ROM of `banks` 8 KB banks whose first byte of each bank is its number,
// with code that writes to `regs` so the mapper is detected.
static uint8_t rom[0x80000];
static size_t make_rom(int banks, const uint16_t *regs, int nregs) {
    memset(rom, 0xff, sizeof rom);
    for (int b = 0; b < banks; b++) rom[b * 0x2000] = (uint8_t)b;
    rom[1] = 'A'; rom[2] = 'B';   // header bytes don't matter here
    for (int i = 0; i < nregs; i++) {
        rom[0x100 + i * 3] = 0x32;
        rom[0x101 + i * 3] = (uint8_t)regs[i];
        rom[0x102 + i * 3] = (uint8_t)(regs[i] >> 8);
    }
    return (size_t)banks * 0x2000;
}

int main(void) {
    static const uint8_t bios[0x8000] = { 0xf3 };
    msx *m = msx_create();
    msx_add_rom(m, 0, 0, 0, 2, bios, sizeof bios);
    msx_expand_slot(m, 3);
    msx_add_ram(m, 3, 2, 0, 4);
    msx_add_cartridge_slot(m, 0, 1, 0);
    msx_reset(m);

    // Sub-slot register: page 3 in slot 3 (expanded), sub-slot 2 everywhere.
    io_write(m, 0xa8, 0xc0);
    mem_write(m, 0xffff, 0xaa);
    CHECK(mem_read(m, 0xffff) == 0x55);
    io_write(m, 0xa8, 0xff);            // all pages in slot 3-2 = RAM
    mem_write(m, 0x1234, 0x42);
    CHECK(mem_read(m, 0x1234) == 0x42);
    io_write(m, 0xa8, 0xfc);            // page 0 back to the BIOS
    CHECK(mem_read(m, 0x0000) == 0xf3);

    // Konami SCC: banks at 4000/6000/8000/A000 selected by 5000/7000/9000/B000.
    static const uint16_t scc_regs[] = { 0x5000, 0x7000, 0x9000, 0xb000, 0x9000 };
    size_t n = make_rom(16, scc_regs, 5);
    CHECK(msx_insert_cartridge(m, 0, rom, n, MSX_MAPPER_AUTO) == MSX_MAPPER_KONAMI_SCC);
    io_write(m, 0xa8, 0xd4);            // pages 1-2 in slot 1, page 3 in slot 3
    CHECK(mem_read(m, 0x4000) == 0 && mem_read(m, 0x6000) == 1 && mem_read(m, 0x8000) == 2 && mem_read(m, 0xa000) == 3);
    mem_write(m, 0x5000, 7);
    mem_write(m, 0xb000, 9);
    CHECK(mem_read(m, 0x4000) == 7 && mem_read(m, 0xa000) == 9);
    mem_write(m, 0x9000, 0x3f);         // SCC on
    mem_write(m, 0x9800, 0x12);         // wave ch 1
    mem_write(m, 0x9860, 0x34);         // wave ch 4 (shared with 5)
    CHECK(mem_read(m, 0x9800) == 0x12 && mem_read(m, 0x9860) == 0x34);
    mem_write(m, 0x9880, 0x20); mem_write(m, 0x988a, 15); mem_write(m, 0x988f, 1);
    float level = 0;
    for (int i = 0; i < 100; i++) level += scc_tick(&m->cart[0].dev->scc, 16);
    CHECK(level != 0);
    mem_write(m, 0x9000, 2);            // SCC off: bank 2 visible again
    CHECK(mem_read(m, 0x8000) == 2);

    // Konami (no SCC).
    static const uint16_t k_regs[] = { 0x6000, 0x8000, 0xa000, 0x8000 };
    n = make_rom(16, k_regs, 4);
    CHECK(msx_insert_cartridge(m, 0, rom, n, MSX_MAPPER_AUTO) == MSX_MAPPER_KONAMI);
    mem_write(m, 0x4000, 5);            // page 1 bank is fixed
    mem_write(m, 0x8000, 6);
    CHECK(mem_read(m, 0x4000) == 0 && mem_read(m, 0x8000) == 6);

    // ASCII 8K.
    static const uint16_t a8_regs[] = { 0x6000, 0x6800, 0x7000, 0x7800 };
    n = make_rom(32, a8_regs, 4);
    CHECK(msx_insert_cartridge(m, 0, rom, n, MSX_MAPPER_AUTO) == MSX_MAPPER_ASCII8);
    mem_write(m, 0x6800, 21);
    mem_write(m, 0x7800, 31);
    CHECK(mem_read(m, 0x6000) == 21 && mem_read(m, 0xa000) == 31 && mem_read(m, 0x4000) == 0);

    // ASCII 16K.
    static const uint16_t a16_regs[] = { 0x6000, 0x7000, 0x77ff, 0x77ff };
    n = make_rom(32, a16_regs, 4);
    CHECK(msx_insert_cartridge(m, 0, rom, n, MSX_MAPPER_AUTO) == MSX_MAPPER_ASCII16);
    mem_write(m, 0x7000, 3);
    CHECK(mem_read(m, 0x8000) == 6 && mem_read(m, 0xa000) == 7);

    // Plain 16 KB with INIT in page 1, and an 8 KB ROM mirrored in its page.
    memset(rom, 0, 0x4000);
    rom[0] = 'A'; rom[1] = 'B'; rom[2] = 0x10; rom[3] = 0x40; rom[0x3fff] = 0x99;
    CHECK(msx_insert_cartridge(m, 0, rom, 0x4000, MSX_MAPPER_AUTO) == MSX_MAPPER_PLAIN);
    CHECK(mem_read(m, 0x4000) == 'A' && mem_read(m, 0x7fff) == 0x99 && mem_read(m, 0x8000) == 0xff);
    rom[0x1fff] = 0x77;
    CHECK(msx_insert_cartridge(m, 0, rom, 0x2000, MSX_MAPPER_AUTO) == MSX_MAPPER_PLAIN);
    CHECK(mem_read(m, 0x6000) == 'A' && mem_read(m, 0x7fff) == 0x77);

    // CRC-32 of "123456789".
    CHECK(msx_crc32("123456789", 9) == 0xcbf43926);

    msx_destroy(m);
    printf(failures ? "%d failure(s)\n" : "all mapper tests passed\n", failures);
    return failures != 0;
}
