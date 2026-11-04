// KorMSX core: an MSX1/MSX2/MSX2+ machine (Z80, TMS9918A or V9938/V9958,
// AY-3-8910, 8255 PPI, slots, memory mapper, RP5C01) assembled at run time
// from a slot layout, the way MAME's machine definitions place ROM and RAM
// with add_internal_slot().
#ifndef MSXCORE_H
#define MSXCORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MSX_AUDIO_RATE 44100
#define MSX_CPU_CLOCK 3579545
#define MSX_CYCLES_PER_LINE 228
#define MSX_LINES_PER_FRAME 262

typedef struct msx msx;

typedef enum {
    MSX_VDP_TMS9918,         // MSX1: 288x224 framebuffer
    MSX_VDP_V9938,           // MSX2: 544x480 (512 + border, scanlines doubled)
    MSX_VDP_V9958,           // MSX2+
} msx_vdp_type;

typedef enum {
    MSX_MAPPER_AUTO = -1,
    MSX_MAPPER_PLAIN = 0,
    MSX_MAPPER_KONAMI,       // Konami without SCC (8 KB banks at 6000/8000/A000)
    MSX_MAPPER_KONAMI_SCC,   // Konami with SCC sound (5000/7000/9000/B000)
    MSX_MAPPER_ASCII8,
    MSX_MAPPER_ASCII16,
} msx_mapper;

msx *msx_create(void);
void msx_destroy(msx *m);

// ---- machine layout (before reset) -------------------------------------
/// Marks primary slot `prim` as expanded (sub-slot register at FFFFh).
void msx_expand_slot(msx *m, int prim);
/// Places a ROM in slot prim-sec over `pages` 16 KB pages starting at `page`.
/// Smaller images are mirrored to fill the pages. Returns false on bad arguments.
bool msx_add_rom(msx *m, int prim, int sec, int page, int pages, const void *data, size_t size);
/// Places RAM in slot prim-sec (64 KB backing store, `pages` pages from `page`).
bool msx_add_ram(msx *m, int prim, int sec, int page, int pages);
/// Chooses the video chip (default TMS9918A). Call before reset.
bool msx_set_vdp(msx *m, msx_vdp_type type);
/// Places memory mapper RAM (`kb`, a power of two from 64 to 4096) in slot prim-sec, all pages.
bool msx_add_mapper_ram(msx *m, int prim, int sec, int kb);
/// Kanji font ROM on ports D8h-DBh (128 KB level 1, 256 KB with level 2).
bool msx_add_kanji_rom(msx *m, const void *data, size_t size);
/// Declares primary slot `prim` (sub-slot `sec`) as cartridge slot `index` (0 or 1).
bool msx_add_cartridge_slot(msx *m, int index, int prim, int sec);

// ---- cartridges ---------------------------------------------------------
/// Inserts a ROM cartridge image. Returns the mapper used, or -1 on failure.
int msx_insert_cartridge(msx *m, int index, const void *data, size_t size, msx_mapper mapper);
void msx_eject_cartridge(msx *m, int index);

// ---- running -----------------------------------------------------------
void msx_reset(msx *m);
/// Runs one 262-line NTSC frame (59736 CPU cycles).
void msx_run_frame(msx *m);
/// RGBX pixels (R in the lowest byte), msx_screen_width x msx_screen_height.
const uint32_t *msx_framebuffer(const msx *m);
int msx_screen_width(const msx *m);
int msx_screen_height(const msx *m);
/// Moves up to `max` mono samples (44.1 kHz) produced so far into `out`.
size_t msx_read_audio(msx *m, int16_t *out, size_t max);

// ---- input -------------------------------------------------------------
/// Presses or releases key `bit` (0-7) in keyboard matrix row `row` (0-10).
void msx_key(msx *m, int row, int bit, bool down);
void msx_release_all_keys(msx *m);
/// Joystick `port` (0 or 1): bit 0 up, 1 down, 2 left, 3 right, 4 trigger A, 5 trigger B.
void msx_joystick(msx *m, int port, uint8_t pressed);

// ---- status ------------------------------------------------------------
bool msx_caps_led(const msx *m);
/// The CODE/KANA LED, labelled 한글 on Korean machines.
bool msx_code_led(const msx *m);
uint16_t msx_pc(const msx *m);
/// Debug access to the CPU's current view of memory.
uint8_t msx_peek(msx *m, uint16_t addr);
const uint8_t *msx_vram(const msx *m);

/// CRC-32 (as in zip files and MAME's ROM definitions), for identifying ROM dumps.
uint32_t msx_crc32(const void *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif
