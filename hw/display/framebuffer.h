#ifndef QEMU_FRAMEBUFFER_H
#define QEMU_FRAMEBUFFER_H

#include "exec/memory.h"

/* Framebuffer device helper routines.  */

typedef void (*drawfn)(void *, uint8_t *, const uint8_t *, int, int);

void framebuffer_update_display(
    DisplaySurface *ds,
    MemoryRegion *address_space,
    hwaddr base,
    int cols,
    int rows,
    int src_width,
    int dest_row_pitch,
    int dest_col_pitch,
    int invalidate,
    drawfn fn,
    void *opaque,
    int *first_row,
    int *last_row);

enum framebuffer_swapmode {
    FB_SWAP_NONE = 0,
    FB_SWAP_16_BYTES,
    FB_SWAP_32_BYTES,
    FB_SWAP_32_WORDS,
};

void framebuffer_update_display_swap_pixman(
    DisplaySurface *ds,
    MemoryRegion *address_space,
    hwaddr base,
    enum framebuffer_swapmode,
    pixman_format_code_t format,
    int invalidate, /* nonzero to redraw the whole image.  */
    int *first_row, /* Input and output.  */
    int *last_row /* Output only */);

#endif
