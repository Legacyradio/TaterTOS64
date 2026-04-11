/*
 * ts_font.h — TrueType font rendering for TaterSurf
 *
 * Header-only. Uses stb_truetype to load DejaVu Sans from
 * /fonts/DEJAVU.TTF on the boot media and render antialiased
 * glyphs to pixel buffers.
 *
 * Three baked font sizes for font_scale 1/2/3.
 * Falls back to bitmap 8x16 font if TTF load fails.
 *
 * Depends on: stb_truetype.h, gfx.h (for gfx_ctx_t)
 */

#ifndef TS_FONT_H
#define TS_FONT_H

#include <stdint.h>
#include <stddef.h>

/* Pre-define stb_truetype dependencies to avoid <stdlib.h> etc.
 * (bare-metal libc has the functions but not the standard headers) */
#define STBTT_ifloor(x)   ((int) floor(x))
#define STBTT_iceil(x)    ((int) ceil(x))
#define STBTT_sqrt(x)     sqrt(x)
#define STBTT_pow(x,y)    pow(x,y)
#define STBTT_fmod(x,y)   fmod(x,y)
#define STBTT_cos(x)      cos(x)
#define STBTT_acos(x)     acos(x)
#define STBTT_fabs(x)     fabs(x)
#define STBTT_malloc(x,u) ((void)(u),malloc(x))
#define STBTT_free(x,u)   ((void)(u),free(x))
#define STBTT_assert(x)   ((void)(x))
#define STBTT_strlen(x)   strlen(x)
#define STBTT_memcpy      memcpy
#define STBTT_memset      memset

/* stb_truetype implementation (single compilation unit) */
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

/* ================================================================== */
/* Font atlas configuration                                            */
/* ================================================================== */

#define TS_FONT_ATLAS_W    512
#define TS_FONT_ATLAS_H    512
#define TS_FONT_FIRST_CHAR  32   /* ASCII space */
#define TS_FONT_NUM_CHARS   95   /* 32-126 inclusive */
#define TS_FONT_NUM_SIZES    3   /* 16px, 22px, 30px */

/* Pixel sizes for each font_scale level */
static const float ts_font_px_sizes[TS_FONT_NUM_SIZES] = {
    16.0f,  /* font_scale 1: body text */
    22.0f,  /* font_scale 2: h3-h4 */
    30.0f   /* font_scale 3: h1-h2 */
};

/* ================================================================== */
/* Font state (global, one per process)                                */
/* ================================================================== */

struct ts_font_atlas {
    unsigned char *pixels;    /* heap-allocated TS_FONT_ATLAS_W * TS_FONT_ATLAS_H */
    stbtt_bakedchar chardata[TS_FONT_NUM_CHARS];
    float px_size;       /* pixel height */
    int ascent;          /* pixels above baseline */
    int descent;         /* pixels below baseline (negative) */
    int line_gap;        /* extra spacing between lines */
    int line_height;     /* total line height in pixels */
    int avg_advance;     /* average character advance (for layout compat) */
};

static struct {
    int loaded;                              /* 1 = TTF loaded OK */
    unsigned char *ttf_data;                 /* raw TTF file */
    size_t ttf_size;
    stbtt_fontinfo info;
    struct ts_font_atlas atlas[TS_FONT_NUM_SIZES];
} g_ts_font = {0};

/* ================================================================== */
/* Font loading                                                        */
/* ================================================================== */

/* Load TTF from filesystem. Call once at startup. */
static int ts_font_init(void) {
    FILE *f;
    long fsize;
    int si;

    if (g_ts_font.loaded) return 1;

    /* Try multiple paths — ISO filesystem may use different names */
    f = fopen("/fonts/DEJAVU.TTF", "rb");
    if (!f) f = fopen("/system/fonts/DEJAVU.TTF", "rb");
    if (!f) f = fopen("/FONTS/DEJAVU.TTF", "rb");
    if (!f) return 0; /* TTF not found, stay on bitmap font */

    fseek(f, 0, SEEK_END);
    fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0 || fsize > 2 * 1024 * 1024) {
        fclose(f);
        return 0;
    }

    g_ts_font.ttf_data = (unsigned char *)malloc((size_t)fsize);
    if (!g_ts_font.ttf_data) { fclose(f); return 0; }

    { size_t read = fread(g_ts_font.ttf_data, 1, (size_t)fsize, f);
      fclose(f);
      if ((long)read != fsize) {
          free(g_ts_font.ttf_data);
          g_ts_font.ttf_data = NULL;
          return 0;
      }
    }
    g_ts_font.ttf_size = (size_t)fsize;

    /* Initialize stb_truetype font info */
    if (!stbtt_InitFont(&g_ts_font.info, g_ts_font.ttf_data,
                         stbtt_GetFontOffsetForIndex(g_ts_font.ttf_data, 0))) {
        free(g_ts_font.ttf_data);
        g_ts_font.ttf_data = NULL;
        return 0;
    }

    /* Bake font atlases for each size (heap-allocated pixel buffers) */
    for (si = 0; si < TS_FONT_NUM_SIZES; si++) {
        struct ts_font_atlas *a = &g_ts_font.atlas[si];
        a->px_size = ts_font_px_sizes[si];
        a->pixels = (unsigned char *)calloc(
            (size_t)TS_FONT_ATLAS_W * TS_FONT_ATLAS_H, 1);
        if (!a->pixels) continue; /* skip this size on OOM */

        stbtt_BakeFontBitmap(g_ts_font.ttf_data, 0, a->px_size,
                              a->pixels, TS_FONT_ATLAS_W, TS_FONT_ATLAS_H,
                              TS_FONT_FIRST_CHAR, TS_FONT_NUM_CHARS,
                              a->chardata);

        /* Get font metrics for this size */
        { float scale = stbtt_ScaleForPixelHeight(&g_ts_font.info, a->px_size);
          int asc, desc, lgap;
          stbtt_GetFontVMetrics(&g_ts_font.info, &asc, &desc, &lgap);
          a->ascent = (int)(asc * scale + 0.5f);
          a->descent = (int)(desc * scale - 0.5f);
          a->line_gap = (int)(lgap * scale + 0.5f);
          a->line_height = a->ascent - a->descent + a->line_gap;
        }

        /* Compute average advance from 'e' and 'o' (common chars) */
        { int adv_e = (int)(a->chardata['e' - TS_FONT_FIRST_CHAR].xadvance + 0.5f);
          int adv_o = (int)(a->chardata['o' - TS_FONT_FIRST_CHAR].xadvance + 0.5f);
          a->avg_advance = (adv_e + adv_o) / 2;
          if (a->avg_advance < 4) a->avg_advance = (int)(a->px_size * 0.55f);
        }
    }

    g_ts_font.loaded = 1;
    return 1;
}

/* ================================================================== */
/* Font rendering                                                      */
/* ================================================================== */

/* Draw a single glyph from the baked atlas onto an ARGB pixel buffer.
 * Alpha-blends the glyph with fg color over bg/existing pixels.
 * Returns advance width in pixels. */
static int ts_font_draw_glyph(uint32_t *pixels, int pw, int ph,
                                int dx, int dy,
                                char ch, int size_idx,
                                uint32_t fg) {
    struct ts_font_atlas *a;
    stbtt_bakedchar *bc;
    int glyph_idx;
    int gx, gy;
    int x0, y0, x1, y1;

    if (!g_ts_font.loaded) return 8;
    if (size_idx < 0) size_idx = 0;
    if (size_idx >= TS_FONT_NUM_SIZES) size_idx = TS_FONT_NUM_SIZES - 1;

    a = &g_ts_font.atlas[size_idx];
    if (!a->pixels) return 8; /* atlas not baked */
    glyph_idx = (unsigned char)ch - TS_FONT_FIRST_CHAR;
    if (glyph_idx < 0 || glyph_idx >= TS_FONT_NUM_CHARS)
        return a->avg_advance; /* unprintable — advance but don't draw */

    bc = &a->chardata[glyph_idx];
    x0 = (int)bc->x0;
    y0 = (int)bc->y0;
    x1 = (int)bc->x1;
    y1 = (int)bc->y1;

    /* Destination offset from baseline */
    { int off_x = dx + (int)(bc->xoff + 0.5f);
      int off_y = dy + (int)(bc->yoff + 0.5f) + a->ascent;

      /* Blit glyph with alpha blending */
      { uint8_t fg_r = (uint8_t)((fg >> 16) & 0xFF);
        uint8_t fg_g = (uint8_t)((fg >> 8) & 0xFF);
        uint8_t fg_b = (uint8_t)(fg & 0xFF);

        for (gy = y0; gy < y1; gy++) {
            int py = off_y + (gy - y0);
            if (py < 0 || py >= ph) continue;
            for (gx = x0; gx < x1; gx++) {
                int px = off_x + (gx - x0);
                if (px < 0 || px >= pw) continue;
                { uint8_t alpha = a->pixels[gy * TS_FONT_ATLAS_W + gx];
                  if (alpha == 0) continue;
                  if (alpha == 255) {
                      pixels[py * pw + px] = fg;
                  } else {
                      /* Alpha blend */
                      uint32_t dst = pixels[py * pw + px];
                      uint8_t dr = (uint8_t)((dst >> 16) & 0xFF);
                      uint8_t dg = (uint8_t)((dst >> 8) & 0xFF);
                      uint8_t db = (uint8_t)(dst & 0xFF);
                      uint8_t a2 = alpha;
                      uint8_t ia = (uint8_t)(255 - a2);
                      uint8_t r = (uint8_t)((fg_r * a2 + dr * ia) / 255);
                      uint8_t g = (uint8_t)((fg_g * a2 + dg * ia) / 255);
                      uint8_t b = (uint8_t)((fg_b * a2 + db * ia) / 255);
                      pixels[py * pw + px] = ((uint32_t)r << 16) |
                                              ((uint32_t)g << 8) | b;
                  }
                }
            }
        }
      }
    }

    return (int)(bc->xadvance + 0.5f);
}

/* Draw a string using the TrueType font. Returns total advance width.
 * size_idx: 0=16px, 1=22px, 2=30px */
static int ts_font_draw_text(uint32_t *pixels, int pw, int ph,
                               int x, int y,
                               const char *text, int len,
                               int size_idx, uint32_t fg) {
    int i;
    int cx = x;
    if (!g_ts_font.loaded) return len * 8;

    for (i = 0; i < len; i++) {
        int adv = ts_font_draw_glyph(pixels, pw, ph, cx, y,
                                      text[i], size_idx, fg);
        cx += adv;
    }
    return cx - x;
}

/* Get the advance width for a string without drawing */
static int ts_font_text_width(const char *text, int len, int size_idx) {
    int i, w = 0;
    struct ts_font_atlas *a;
    if (!g_ts_font.loaded) return len * 8;
    if (size_idx < 0) size_idx = 0;
    if (size_idx >= TS_FONT_NUM_SIZES) size_idx = TS_FONT_NUM_SIZES - 1;
    a = &g_ts_font.atlas[size_idx];
    for (i = 0; i < len; i++) {
        int gi = (unsigned char)text[i] - TS_FONT_FIRST_CHAR;
        if (gi >= 0 && gi < TS_FONT_NUM_CHARS)
            w += (int)(a->chardata[gi].xadvance + 0.5f);
        else
            w += a->avg_advance;
    }
    return w;
}

/* Get font metrics */
static int ts_font_line_height(int size_idx) {
    if (!g_ts_font.loaded) return 16;
    if (size_idx < 0 || size_idx >= TS_FONT_NUM_SIZES) return 16;
    return g_ts_font.atlas[size_idx].line_height;
}

static int ts_font_is_loaded(void) {
    return g_ts_font.loaded;
}

#endif /* TS_FONT_H */
