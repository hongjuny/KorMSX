// TMS9918A/TMS9928A video display processor (MSX1).
#ifndef VDP_H
#define VDP_H

#include <stdbool.h>
#include <stdint.h>

#define VDP_BORDER_X 16
#define VDP_BORDER_Y 16
#define VDP_WIDTH  (256 + 2 * VDP_BORDER_X)
#define VDP_HEIGHT (192 + 2 * VDP_BORDER_Y)
#define VDP_LINES  262          // NTSC
#define VDP_VBLANK_LINE 192     // first line after the active display

typedef struct vdp {
    uint8_t vram[0x4000];
    uint8_t regs[8];
    uint8_t status;
    uint8_t read_ahead;
    uint16_t addr;
    bool latch;
    uint8_t fifth;              // last sprite number processed (status bits 0-4)
    uint32_t fb[VDP_WIDTH * VDP_HEIGHT];   // R | G << 8 | B << 16 | 0xff << 24
} vdp;

void vdp_reset(vdp *v);
uint8_t vdp_read(vdp *v, int port);         // port 0 = data, 1 = status
void vdp_write(vdp *v, int port, uint8_t value);
/// Renders display line `line` (0 = first active line; negative or >= 192 is border).
void vdp_render_line(vdp *v, int line);
/// Called at the start of vertical blanking.
void vdp_vblank(vdp *v);
/// Level of the interrupt output.
bool vdp_irq(const vdp *v);

#endif
