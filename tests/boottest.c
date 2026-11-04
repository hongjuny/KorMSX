// Headless boot: builds a machine from ROM files, runs N frames, writes a PPM.
// Usage: boottest <frames> <out.ppm> <layout> [roms...]
//   layout "cbios": cbios_main.rom cbios_logo.rom [cart.rom]
//   layout "kr":    bios.rom hangul.rom [cart.rom]   (Daewoo DPC-200 / MAME dpc200)
//   layout "cbios2" / "cbios2+": main.rom logo.rom [cart.rom], sub ROM in $KORMSX_SUB
//   layout "msx2":  main.rom sub.rom [cart.rom]       (Philips NMS 8250 / MAME nms8250)
// Keys to type after boot can be given in KORMSX_TYPE (letters, digits, space, '\n').
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "msxcore.h"

static void *load(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    *size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc(*size);
    if (fread(buf, 1, *size, f) != *size) { perror(path); exit(2); }
    fclose(f);
    return buf;
}

// MSX matrix position of an ASCII character (international layout, unshifted).
static int matrix(char c, int *row, int *bit) {
    if (c >= '0' && c <= '7') { *row = 0; *bit = c - '0'; return 1; }
    if (c == '8' || c == '9') { *row = 1; *bit = c - '8'; return 1; }
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') {
        int i = c - 'A' + 6;           // A is row 2 bit 6
        *row = 2 + i / 8; *bit = i % 8; return 1;
    }
    if (c == ' ') { *row = 8; *bit = 0; return 1; }
    if (c == '\n') { *row = 7; *bit = 7; return 1; }
    if (c == '"') { *row = 0; *bit = 2; return 2; }   // shift+2 on a JP/KR layout
    if (c == '*') { *row = 1; *bit = 0; return 2; }   // shift+8 (international)
    if (c == '+') { *row = 1; *bit = 3; return 2; }   // shift+= (international)
    if (c == '(') { *row = 1; *bit = 1; return 2; }   // shift+9
    if (c == ')') { *row = 0; *bit = 0; return 2; }   // shift+0
    if (c == ':') { *row = 1; *bit = 7; return 2; }   // shift+;
    if (c == ',') { *row = 2; *bit = 2; return 1; }
    if (c == '-') { *row = 1; *bit = 2; return 1; }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: boottest frames out.ppm cbios|kr roms...\n"); return 2; }
    int frames = atoi(argv[1]);
    msx *m = msx_create();
    size_t n0, n1;
    void *r0 = load(argv[4], &n0);
    void *r1 = argc > 5 ? load(argv[5], &n1) : NULL;
    if (!strncmp(argv[3], "cbios2", 6)) {
        // C-BIOS MSX2/MSX2+: main + logo in slot 0, sub ROM in 3-0, 512 KB mapper in 3-2
        msx_set_vdp(m, strcmp(argv[3], "cbios2+") ? MSX_VDP_V9938 : MSX_VDP_V9958);
        msx_add_rom(m, 0, 0, 0, 2, r0, n0);
        msx_add_rom(m, 0, 0, 2, 1, r1, n1);
        msx_expand_slot(m, 3);
        size_t ns;
        void *sub = load(getenv("KORMSX_SUB"), &ns);
        msx_add_rom(m, 3, 0, 0, 1, sub, ns);
        msx_add_mapper_ram(m, 3, 2, 512);
        msx_add_cartridge_slot(m, 0, 1, 0);
        msx_add_cartridge_slot(m, 1, 2, 0);
    } else if (!strcmp(argv[3], "msx2")) {
        // MSX2 (Philips NMS 8250 layout): main in slot 0, sub ROM (2nd file) in 3-0, 128 KB mapper in 3-2
        msx_set_vdp(m, MSX_VDP_V9938);
        msx_add_rom(m, 0, 0, 0, 2, r0, n0);
        msx_expand_slot(m, 3);
        msx_add_rom(m, 3, 0, 0, 4, r1, n1);
        msx_add_mapper_ram(m, 3, 2, 128);
        msx_add_cartridge_slot(m, 0, 1, 0);
        msx_add_cartridge_slot(m, 1, 2, 0);
    } else if (!strcmp(argv[3], "cbios")) {
        msx_add_rom(m, 0, 0, 0, 2, r0, n0);
        if (r1) msx_add_rom(m, 0, 0, 2, 1, r1, n1);
        msx_add_ram(m, 3, 0, 0, 4);
        msx_add_cartridge_slot(m, 0, 1, 0);
        msx_add_cartridge_slot(m, 1, 2, 0);
    } else {
        msx_add_rom(m, 0, 0, 0, 2, r0, n0);
        msx_add_rom(m, 0, 0, 2, 1, r1, n1);
        msx_add_cartridge_slot(m, 0, 1, 0);
        msx_add_ram(m, 2, 0, 0, 4);
    }
    if (argc > 6) {
        size_t nc;
        void *c = load(argv[6], &nc);
        printf("mapper %d\n", msx_insert_cartridge(m, 0, c, nc, MSX_MAPPER_AUTO));
    }
    msx_reset(m);
    const char *type = getenv("KORMSX_TYPE");
    int typed = 0, hold = 0;
    size_t audio = 0;
    int16_t buf[4096];
    long crossings = 0, last_sign = 0;
    double energy = 0;
    size_t measured = 0;
    for (int f = 0; f < frames; f++) {
        if (type && f > frames / 2 && type[typed]) {
            int row, bit, k = matrix(type[typed], &row, &bit);
            if (hold == 0 && k) {
                if (k == 2) msx_key(m, 6, 0, true);
                msx_key(m, row, bit, true);
            }
            if (++hold == 3) {
                msx_release_all_keys(m);
            } else if (hold == 6) {
                hold = 0;
                typed++;
            }
        }
        msx_run_frame(m);
        size_t got = msx_read_audio(m, buf, 4096);
        audio += got;
        if (f >= frames - (getenv("KORMSX_TYPE") ? frames / 3 : 60)) {           // the last second
            for (size_t i = 0; i < got; i++) {
                long sign = buf[i] > 0 ? 1 : (buf[i] < 0 ? -1 : 0);
                if (sign && last_sign && sign != last_sign) crossings++;
                if (sign) last_sign = sign;
                energy += (double)buf[i] * buf[i];
            }
            measured += got;
        }
    }
    if (measured) printf("last second: rms=%.0f, tone ~%.1f Hz\n", sqrt(energy / measured),
                         crossings / 2.0 * MSX_AUDIO_RATE / measured);
    printf("pc=%04x audio samples=%zu (%.1f/frame) code_led=%d\n", msx_pc(m), audio, (double)audio / frames, msx_code_led(m));
    FILE *o = fopen(argv[2], "wb");
    int w = msx_screen_width(m), h = msx_screen_height(m);
    fprintf(o, "P6 %d %d 255\n", w, h);
    const uint32_t *fb = msx_framebuffer(m);
    for (int i = 0; i < w * h; i++) {
        uint8_t px[3] = { (uint8_t)fb[i], (uint8_t)(fb[i] >> 8), (uint8_t)(fb[i] >> 16) };
        fwrite(px, 1, 3, o);
    }
    fclose(o);
    msx_destroy(m);
    return 0;
}
