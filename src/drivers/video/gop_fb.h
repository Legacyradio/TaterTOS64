#ifndef TATER_GOP_FB_H
#define TATER_GOP_FB_H

#include <stdint.h>

struct gop_fb_info {
    uint64_t base;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t pixel_format;
};

void gop_fb_init(void);
const struct gop_fb_info *gop_fb_get_info(void);
void gop_fb_clear(uint32_t rgb);
void gop_fb_putpixel(uint32_t x, uint32_t y, uint32_t rgb);

#endif
