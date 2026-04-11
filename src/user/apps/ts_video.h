/*
 * ts_video.h -- YUV-to-RGB conversion and framebuffer blit for TaterSurf
 *
 * Header-only.  BT.709 color matrix, SSE2 fast path, nearest-neighbor
 * downscale, 32-bit BGRX output into a SHM pixel buffer.
 *
 * Usage:
 *   ts_video_yuv_to_rgb(y, u, v, stride_y, stride_uv,
 *                        src_w, src_h, dst, dst_w, dst_h, dst_stride);
 *
 * All buffers are caller-owned.  No malloc.
 */

#ifndef TS_VIDEO_H
#define TS_VIDEO_H

#include <stdint.h>

#ifdef __SSE2__
#include <emmintrin.h>
#endif

/* ================================================================== */
/* Scalar helpers                                                      */
/* ================================================================== */

static inline int ts_clamp(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

/*
 * BT.709 YUV→RGB (studio-swing input, 16-235 Y, 16-240 UV):
 *   R = 1.164*(Y-16) + 1.793*(V-128)
 *   G = 1.164*(Y-16) - 0.213*(U-128) - 0.533*(V-128)
 *   B = 1.164*(Y-16) + 2.112*(U-128)
 *
 * Fixed-point: multiply by 256 to stay in int32.
 */
#define YUV_FP_SHIFT  8
#define YUV_C_Y    298  /* 1.164 * 256 */
#define YUV_C_RV   459  /* 1.793 * 256 */
#define YUV_C_GU   (-55)  /* -0.213 * 256 */
#define YUV_C_GV  (-136)  /* -0.533 * 256 */
#define YUV_C_BU   541  /* 2.112 * 256 */

static inline uint32_t ts_yuv_pixel(int y, int u, int v) {
    int c = YUV_C_Y * (y - 16);
    int r = ts_clamp((c + YUV_C_RV * (v - 128) + 128) >> YUV_FP_SHIFT);
    int g = ts_clamp((c + YUV_C_GU * (u - 128) + YUV_C_GV * (v - 128) + 128) >> YUV_FP_SHIFT);
    int b = ts_clamp((c + YUV_C_BU * (u - 128) + 128) >> YUV_FP_SHIFT);
    return (uint32_t)b | ((uint32_t)g << 8) | ((uint32_t)r << 16) | 0xFF000000u;
}

/* ================================================================== */
/* SSE2 fast path — 8 pixels at a time                                 */
/* ================================================================== */

#ifdef __SSE2__

/*
 * Convert 8 Y pixels (with corresponding 4 U/V pairs, doubled for 4:2:0)
 * and write 8 BGRX pixels to dst.
 */
static inline void ts_yuv_row_sse2(const uint8_t *yp, const uint8_t *up,
                                    const uint8_t *vp, uint32_t *dst,
                                    int width) {
    int x;
    __m128i zero = _mm_setzero_si128();
    __m128i c16  = _mm_set1_epi16(16);
    __m128i c128 = _mm_set1_epi16(128);
    __m128i cY   = _mm_set1_epi16(298);
    __m128i cRV  = _mm_set1_epi16(459);
    __m128i cGU  = _mm_set1_epi16(-55);
    __m128i cGV  = _mm_set1_epi16(-136);
    __m128i cBU  = _mm_set1_epi16(541);
    __m128i bias = _mm_set1_epi16(128);

    for (x = 0; x + 7 < width; x += 8) {
        /* Load 8 Y values → 16-bit */
        __m128i y8 = _mm_loadl_epi64((const __m128i *)(yp + x));
        __m128i y16 = _mm_unpacklo_epi8(y8, zero);
        y16 = _mm_sub_epi16(y16, c16);

        /* Load 4 U,V values → double to 8 wide */
        __m128i u4 = _mm_cvtsi32_si128(*(const int32_t *)(up + (x >> 1)));
        __m128i v4 = _mm_cvtsi32_si128(*(const int32_t *)(vp + (x >> 1)));
        __m128i u8 = _mm_unpacklo_epi8(u4, zero);
        __m128i v8 = _mm_unpacklo_epi8(v4, zero);
        /* Double: [u0 u1 u2 u3] → [u0 u0 u1 u1 u2 u2 u3 u3] */
        u8 = _mm_unpacklo_epi16(u8, u8);
        v8 = _mm_unpacklo_epi16(v8, v8);
        u8 = _mm_sub_epi16(u8, c128);
        v8 = _mm_sub_epi16(v8, c128);

        /* C = 298 * (Y - 16) */
        __m128i c_val = _mm_mullo_epi16(cY, y16);

        /* R = (C + 459*(V-128) + 128) >> 8 */
        __m128i r16 = _mm_add_epi16(c_val, _mm_mullo_epi16(cRV, v8));
        r16 = _mm_add_epi16(r16, bias);
        r16 = _mm_srai_epi16(r16, YUV_FP_SHIFT);

        /* G = (C - 55*(U-128) - 136*(V-128) + 128) >> 8 */
        __m128i g16 = _mm_add_epi16(c_val, _mm_mullo_epi16(cGU, u8));
        g16 = _mm_add_epi16(g16, _mm_mullo_epi16(cGV, v8));
        g16 = _mm_add_epi16(g16, bias);
        g16 = _mm_srai_epi16(g16, YUV_FP_SHIFT);

        /* B = (C + 541*(U-128) + 128) >> 8 */
        __m128i b16 = _mm_add_epi16(c_val, _mm_mullo_epi16(cBU, u8));
        b16 = _mm_add_epi16(b16, bias);
        b16 = _mm_srai_epi16(b16, YUV_FP_SHIFT);

        /* Clamp to [0,255] and pack to bytes */
        __m128i r8out = _mm_packus_epi16(r16, zero);
        __m128i g8out = _mm_packus_epi16(g16, zero);
        __m128i b8out = _mm_packus_epi16(b16, zero);

        /* Interleave to BGRX: B in byte 0, G in byte 1, R in byte 2, 0xFF in byte 3 */
        __m128i bg_lo = _mm_unpacklo_epi8(b8out, g8out);  /* b0 g0 b1 g1 ... */
        __m128i ff = _mm_set1_epi8((char)0xFF);
        __m128i ra_lo = _mm_unpacklo_epi8(r8out, ff);      /* r0 ff r1 ff ... */
        __m128i px_lo = _mm_unpacklo_epi16(bg_lo, ra_lo);  /* b0 g0 r0 ff ... (4 pixels) */
        __m128i px_hi = _mm_unpackhi_epi16(bg_lo, ra_lo);  /* b4 g4 r4 ff ... (4 pixels) */

        _mm_storeu_si128((__m128i *)(dst + x), px_lo);
        _mm_storeu_si128((__m128i *)(dst + x + 4), px_hi);
    }

    /* Handle remaining pixels with scalar */
    for (; x < width; x++) {
        int yval = yp[x];
        int uval = up[x >> 1];
        int vval = vp[x >> 1];
        dst[x] = ts_yuv_pixel(yval, uval, vval);
    }
}

#endif /* __SSE2__ */

/* ================================================================== */
/* Main conversion: YUV 4:2:0 → BGRX with nearest-neighbor scale      */
/* ================================================================== */

/*
 * Convert a YUV 4:2:0 frame to 32-bit BGRX and scale to destination
 * dimensions using nearest-neighbor interpolation.
 *
 *   y_plane, u_plane, v_plane  — source YUV planes
 *   stride_y, stride_uv       — row stride for Y and U/V planes
 *   src_w, src_h               — source frame dimensions
 *   dst                        — destination BGRX pixel buffer
 *   dst_w, dst_h               — destination dimensions
 *   dst_stride                 — destination stride in pixels (not bytes)
 */
static void ts_video_yuv_to_rgb(const uint8_t *y_plane,
                                 const uint8_t *u_plane,
                                 const uint8_t *v_plane,
                                 int stride_y, int stride_uv,
                                 int src_w, int src_h,
                                 uint32_t *dst,
                                 int dst_w, int dst_h,
                                 int dst_stride) {
    int dy;

    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return;

    /* No scaling needed — direct 1:1 conversion */
    if (src_w == dst_w && src_h == dst_h) {
        for (dy = 0; dy < dst_h; dy++) {
            const uint8_t *yrow = y_plane + dy * stride_y;
            const uint8_t *urow = u_plane + (dy >> 1) * stride_uv;
            const uint8_t *vrow = v_plane + (dy >> 1) * stride_uv;
            uint32_t *drow = dst + dy * dst_stride;

#ifdef __SSE2__
            ts_yuv_row_sse2(yrow, urow, vrow, drow, src_w);
#else
            int dx;
            for (dx = 0; dx < src_w; dx++) {
                drow[dx] = ts_yuv_pixel(yrow[dx], urow[dx >> 1], vrow[dx >> 1]);
            }
#endif
        }
        return;
    }

    /* Nearest-neighbor downscale/upscale */
    for (dy = 0; dy < dst_h; dy++) {
        int sy = (dy * src_h) / dst_h;
        if (sy >= src_h) sy = src_h - 1;

        const uint8_t *yrow = y_plane + sy * stride_y;
        const uint8_t *urow = u_plane + (sy >> 1) * stride_uv;
        const uint8_t *vrow = v_plane + (sy >> 1) * stride_uv;
        uint32_t *drow = dst + dy * dst_stride;

        int dx;
        for (dx = 0; dx < dst_w; dx++) {
            int sx = (dx * src_w) / dst_w;
            if (sx >= src_w) sx = src_w - 1;
            drow[dx] = ts_yuv_pixel(yrow[sx], urow[sx >> 1], vrow[sx >> 1]);
        }
    }
}

/* ================================================================== */
/* Blit helper: copy a scaled video frame into a sub-region of the     */
/* window's pixel buffer.                                              */
/* ================================================================== */

static void ts_video_blit(const uint32_t *frame, int frame_w, int frame_h,
                           uint32_t *win_buf, int win_stride,
                           int x, int y, int w, int h) {
    int row;
    int copy_w = (w < frame_w) ? w : frame_w;
    int copy_h = (h < frame_h) ? h : frame_h;

    if (x < 0 || y < 0) return;

    for (row = 0; row < copy_h; row++) {
        uint32_t *dst_row = win_buf + (y + row) * win_stride + x;
        const uint32_t *src_row = frame + row * frame_w;
        memcpy(dst_row, src_row, (size_t)copy_w * 4);
    }
}

#endif /* TS_VIDEO_H */
