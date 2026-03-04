#ifndef TATER_GFX_H
#define TATER_GFX_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t *buffer;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
} gfx_ctx_t;

void gfx_init(gfx_ctx_t *ctx, uint32_t *buffer, uint32_t w, uint32_t h, uint32_t stride);
void gfx_putpixel(gfx_ctx_t *ctx, uint32_t x, uint32_t y, uint32_t color);
void gfx_fill(gfx_ctx_t *ctx, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void gfx_rect(gfx_ctx_t *ctx, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void gfx_draw_char(gfx_ctx_t *ctx, uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg);
void gfx_draw_text(gfx_ctx_t *ctx, uint32_t x, uint32_t y, const char *text, uint32_t fg, uint32_t bg);
void gfx_blit(gfx_ctx_t *dst, gfx_ctx_t *src, uint32_t dx, uint32_t dy);
uint32_t gfx_lerp_color(uint32_t c1, uint32_t c2, uint8_t t);
void gfx_gradient_v(gfx_ctx_t *ctx, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t c1, uint32_t c2);
void gfx_gradient_h(gfx_ctx_t *ctx, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t c1, uint32_t c2);
void gfx_fill_rounded(gfx_ctx_t *ctx, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color, int r);

#endif
