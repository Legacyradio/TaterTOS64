// GOP framebuffer driver

#include <stdint.h>
#include "gop_fb.h"
#include "../../boot/efi_handoff.h"
#include "../../kernel/mm/vmm.h"

extern struct fry_handoff *g_handoff;

static struct gop_fb_info g_info;

static inline uint32_t rgb_to_pixel(uint32_t rgb) {
    // if pixel_format==0 BGRX, swap
    if (g_info.pixel_format == 0) {
        uint32_t r = (rgb >> 16) & 0xFF;
        uint32_t g = (rgb >> 8) & 0xFF;
        uint32_t b = rgb & 0xFF;
        return (b << 16) | (g << 8) | r;
    }
    return rgb;
}

void gop_fb_init(void) {
    if (!g_handoff || !g_handoff->fb_base) {
        g_info.base = 0;
        return;
    }

    g_info.base = VMM_FB_BASE; // mapped in vmm_init
    g_info.width = (uint32_t)g_handoff->fb_width;
    g_info.height = (uint32_t)g_handoff->fb_height;
    g_info.stride = (uint32_t)g_handoff->fb_stride;
    g_info.pixel_format = g_handoff->fb_pixel_format;
}

const struct gop_fb_info *gop_fb_get_info(void) {
    return g_info.base ? &g_info : 0;
}

void gop_fb_putpixel(uint32_t x, uint32_t y, uint32_t rgb) {
    if (!g_info.base) return;
    if (x >= g_info.width || y >= g_info.height) return;

    uint32_t *fb = (uint32_t *)(uintptr_t)g_info.base;
    uint32_t val = rgb_to_pixel(rgb);
    fb[y * g_info.stride + x] = val;
}

void gop_fb_clear(uint32_t rgb) {
    if (!g_info.base) return;
    uint32_t *fb = (uint32_t *)(uintptr_t)g_info.base;
    uint32_t val = rgb_to_pixel(rgb);
    for (uint32_t y = 0; y < g_info.height; y++) {
        for (uint32_t x = 0; x < g_info.width; x++) {
            fb[y * g_info.stride + x] = val;
        }
    }
}
