/*
 * Framebuffer device helper routines
 *
 * Copyright (c) 2009 CodeSourcery
 * Written by Paul Brook <paul@codesourcery.com>
 *
 * This code is licensed under the GNU GPLv2.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

/* TODO:
   - Do something similar for framebuffers with local ram
   - Handle rotation here instead of hacking dest_pitch
   - Use common pixel conversion routines instead of per-device drawfn
   - Remove all DisplayState knowledge from devices.
 */

#include "hw/hw.h"
#include "ui/console.h"
#include "framebuffer.h"

/* Render an image from a shared memory framebuffer.  */
   
void framebuffer_update_display(
    DisplaySurface *ds,
    MemoryRegion *address_space,
    hwaddr base,
    int cols, /* Width in pixels.  */
    int rows, /* Height in pixels.  */
    int src_width, /* Length of source line, in bytes.  */
    int dest_row_pitch, /* Bytes between adjacent horizontal output pixels.  */
    int dest_col_pitch, /* Bytes between adjacent vertical output pixels.  */
    int invalidate, /* nonzero to redraw the whole image.  */
    drawfn fn,
    void *opaque,
    int *first_row, /* Input and output.  */
    int *last_row /* Output only */)
{
    hwaddr src_len;
    uint8_t *dest;
    uint8_t *src;
    uint8_t *src_base;
    int first, last = 0;
    int dirty;
    int i;
    ram_addr_t addr;
    MemoryRegionSection mem_section;
    MemoryRegion *mem;

    i = *first_row;
    *first_row = -1;
    src_len = src_width * rows;

    mem_section = memory_region_find(address_space, base, src_len);
    mem = mem_section.mr;
    if (int128_get64(mem_section.size) != src_len ||
            !memory_region_is_ram(mem_section.mr)) {
        goto out;
    }
    assert(mem);
    assert(mem_section.offset_within_address_space == base);

    memory_region_sync_dirty_bitmap(mem);
    if (!memory_region_is_logging(mem, DIRTY_MEMORY_VGA)) {
        invalidate = true;
    }

    src_base = cpu_physical_memory_map(base, &src_len, 0);
    /* If we can't map the framebuffer then bail.  We could try harder,
       but it's not really worth it as dirty flag tracking will probably
       already have failed above.  */
    if (!src_base)
        goto out;
    if (src_len != src_width * rows) {
        cpu_physical_memory_unmap(src_base, src_len, 0, 0);
        goto out;
    }
    src = src_base;
    dest = surface_data(ds);
    if (dest_col_pitch < 0)
        dest -= dest_col_pitch * (cols - 1);
    if (dest_row_pitch < 0) {
        dest -= dest_row_pitch * (rows - 1);
    }
    first = -1;
    addr = mem_section.offset_within_region;

    addr += i * src_width;
    src += i * src_width;
    dest += i * dest_row_pitch;

    for (; i < rows; i++) {
        dirty = memory_region_get_dirty(mem, addr, src_width,
                                             DIRTY_MEMORY_VGA);
        if (dirty || invalidate) {
            fn(opaque, dest, src, cols, dest_col_pitch);
            if (first == -1)
                first = i;
            last = i;
        }
        addr += src_width;
        src += src_width;
        dest += dest_row_pitch;
    }
    cpu_physical_memory_unmap(src_base, src_len, 0, 0);
    if (first < 0) {
        goto out;
    }
    memory_region_reset_dirty(mem, mem_section.offset_within_region, src_len,
                              DIRTY_MEMORY_VGA);
    *first_row = first;
    *last_row = last;
out:
    memory_region_unref(mem);
}

static void framebuffer_swap(enum framebuffer_swapmode swapmode,
                             pixman_image_t *dest, void *src)
{
    int swaps;
    uint32_t *s32, *d32;
    uint16_t *s16, *d16;

    switch (swapmode) {
    case FB_SWAP_NONE:
        memcpy(pixman_image_get_data(dest),
               src,
               pixman_image_get_stride(dest));
        break;
    case FB_SWAP_16_BYTES:
        swaps = pixman_image_get_stride(dest) / 2;
        s16 = src;
        d16 = (void*)pixman_image_get_data(dest);
        while (swaps) {
            *d16 = bswap16(*s16);
            s16++; d16++; swaps--;
        }
        break;
    case FB_SWAP_32_BYTES:
        swaps = pixman_image_get_stride(dest) / 4;
        s32 = src;
        d32 = pixman_image_get_data(dest);
        while (swaps) {
            *d32 = bswap32(*s32);
            s32++; d32++; swaps--;
        }
        break;
    case FB_SWAP_32_WORDS:
        swaps = pixman_image_get_stride(dest) / 4;
        s32 = src;
        d32 = pixman_image_get_data(dest);
        while (swaps) {
            *d32 = (((*s32 & 0x0000ffff) << 16) |
                    ((*s32 & 0xffff0000) >> 16));
            s32++; d32++; swaps--;
        }
        break;
    default:
        g_assert_not_reached();
    }
}

void framebuffer_update_display_swap_pixman(
    DisplaySurface *ds,
    MemoryRegion *address_space,
    hwaddr base,
    enum framebuffer_swapmode swapmode,
    pixman_format_code_t format,
    pixman_indexed_t *palette,
    int invalidate, /* nonzero to redraw the whole image.  */
    int *first_row, /* Input and output.  */
    int *last_row /* Output only */)
{
    int cols = surface_width(ds);
    int rows = surface_height(ds);
    int src_width = cols * PIXMAN_FORMAT_BPP(format) / 8;
    pixman_image_t *linebuf;
    hwaddr src_len;
    uint8_t *src;
    uint8_t *src_base;
    int first, last = 0;
    int dirty;
    int i;
    ram_addr_t addr;
    MemoryRegionSection mem_section;
    MemoryRegion *mem;

    i = *first_row;
    *first_row = -1;
    src_len = src_width * rows;

    mem_section = memory_region_find(address_space, base, src_len);
    mem = mem_section.mr;
    if (int128_get64(mem_section.size) != src_len ||
        !memory_region_is_ram(mem_section.mr)) {
        goto out;
    }
    assert(mem);
    assert(mem_section.offset_within_address_space == base);

    memory_region_sync_dirty_bitmap(mem);
    src_base = cpu_physical_memory_map(base, &src_len, 0);
    /* If we can't map the framebuffer then bail.  We could try harder,
       but it's not really worth it as dirty flag tracking will probably
       already have failed above.  */
    if (!src_base)
        goto out;
    if (src_len != src_width * rows) {
        cpu_physical_memory_unmap(src_base, src_len, 0, 0);
        goto out;
    }
    src = src_base;
    first = -1;
    addr = mem_section.offset_within_region;

    addr += i * src_width;
    src += i * src_width;

    linebuf = qemu_pixman_linebuf_create(format, cols);
    pixman_image_set_indexed(linebuf, palette);
    for (; i < rows; i++) {
        dirty = memory_region_get_dirty(mem, addr, src_width,
                                        DIRTY_MEMORY_VGA);
        if (dirty || invalidate) {
            framebuffer_swap(swapmode, linebuf, src);
            qemu_pixman_linebuf_copy(ds->image, cols, 0, i, linebuf);
            if (first == -1)
                first = i;
            last = i;
        }
        addr += src_width;
        src += src_width;
    }
    qemu_pixman_image_unref(linebuf);

    cpu_physical_memory_unmap(src_base, src_len, 0, 0);
    if (first < 0) {
        goto out;
    }
    memory_region_reset_dirty(mem, mem_section.offset_within_region, src_len,
                              DIRTY_MEMORY_VGA);
    *first_row = first;
    *last_row = last;
out:
    memory_region_unref(mem);
}
