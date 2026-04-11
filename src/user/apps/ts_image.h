/*
 * ts_image.h — TaterSurf image decoder
 *
 * Header-only. Decodes PNG and BMP images to RGBA pixel buffers.
 * Includes a minimal inflate (RFC 1951) implementation for PNG.
 *
 * Supports:
 *   PNG:  8-bit RGB, RGBA, grayscale, grayscale+alpha, palette
 *   BMP:  24-bit and 32-bit uncompressed
 *
 * Usage:
 *   uint32_t *pixels;
 *   int w, h;
 *   if (ts_image_decode(data, len, &pixels, &w, &h) == 0) {
 *       // pixels is malloc'd RGBA (0xAARRGGBB native order)
 *       free(pixels);
 *   }
 */

#ifndef TS_IMAGE_H
#define TS_IMAGE_H

#include <stdint.h>
#include <stddef.h>

/* Forward declarations from libc */
extern void *malloc(size_t);
extern void *realloc(void *, size_t);
extern void free(void *);
extern void *memcpy(void *, const void *, size_t);
extern void *memset(void *, int, size_t);

/* ================================================================== */
/* Inflate (RFC 1951) — minimal decompressor                           */
/* ================================================================== */

/* Bit reader */
struct ts_inflate_bits {
    const uint8_t *data;
    size_t len;
    size_t pos;         /* byte position */
    uint32_t buf;       /* bit buffer */
    int bits;           /* bits in buffer */
};

static void ts_inf_init(struct ts_inflate_bits *b,
                         const uint8_t *data, size_t len) {
    b->data = data;
    b->len = len;
    b->pos = 0;
    b->buf = 0;
    b->bits = 0;
}

static uint32_t ts_inf_bits(struct ts_inflate_bits *b, int n) {
    uint32_t val;
    while (b->bits < n) {
        if (b->pos >= b->len) return 0;
        b->buf |= (uint32_t)b->data[b->pos++] << b->bits;
        b->bits += 8;
    }
    val = b->buf & ((1u << n) - 1);
    b->buf >>= n;
    b->bits -= n;
    return val;
}

static void ts_inf_align(struct ts_inflate_bits *b) {
    b->buf = 0;
    b->bits = 0;
}

/* Huffman tree (max 288 symbols, max 15-bit codes) */
#define TS_INF_MAX_SYMBOLS 320
#define TS_INF_MAX_BITS     16

struct ts_inf_tree {
    uint16_t counts[TS_INF_MAX_BITS];
    uint16_t symbols[TS_INF_MAX_SYMBOLS];
};

static void ts_inf_build_tree(struct ts_inf_tree *t,
                               const uint8_t *lengths, int num) {
    uint16_t offsets[TS_INF_MAX_BITS];
    int i;

    memset(t->counts, 0, sizeof(t->counts));
    memset(t->symbols, 0, sizeof(t->symbols));

    /* Count code lengths */
    for (i = 0; i < num; i++) {
        if (lengths[i] < TS_INF_MAX_BITS)
            t->counts[lengths[i]]++;
    }
    t->counts[0] = 0;

    /* Compute offsets for each length */
    offsets[0] = 0;
    for (i = 1; i < TS_INF_MAX_BITS; i++)
        offsets[i] = offsets[i - 1] + t->counts[i - 1];

    /* Assign symbols */
    for (i = 0; i < num; i++) {
        if (lengths[i] > 0 && lengths[i] < TS_INF_MAX_BITS)
            t->symbols[offsets[lengths[i]]++] = (uint16_t)i;
    }
}

static int ts_inf_decode(struct ts_inflate_bits *b, struct ts_inf_tree *t) {
    int code = 0, first = 0, idx = 0;
    int i;
    for (i = 1; i < TS_INF_MAX_BITS; i++) {
        code |= (int)ts_inf_bits(b, 1);
        int count = t->counts[i];
        if (code - count < first)
            return t->symbols[idx + (code - first)];
        idx += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1; /* invalid */
}

/* Fixed Huffman trees (precomputed per RFC 1951) */
static void ts_inf_fixed_trees(struct ts_inf_tree *lit_len,
                                struct ts_inf_tree *dist) {
    uint8_t lengths[320];
    int i;
    /* Literal/length tree */
    for (i = 0; i <= 143; i++) lengths[i] = 8;
    for (; i <= 255; i++) lengths[i] = 9;
    for (; i <= 279; i++) lengths[i] = 7;
    for (; i <= 287; i++) lengths[i] = 8;
    ts_inf_build_tree(lit_len, lengths, 288);
    /* Distance tree */
    for (i = 0; i < 32; i++) lengths[i] = 5;
    ts_inf_build_tree(dist, lengths, 32);
}

/* Length base + extra bits tables */
static const uint16_t ts_inf_len_base[] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
    35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const uint8_t ts_inf_len_extra[] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,
    3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const uint16_t ts_inf_dist_base[] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
    257,385,513,769,1025,1537,2049,3073,4097,6145,
    8193,12289,16385,24577
};
static const uint8_t ts_inf_dist_extra[] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,
    7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

/*
 * ts_inflate — decompress deflate stream.
 * Returns malloc'd output buffer and sets *out_len.
 * Returns NULL on error.
 */
static uint8_t *ts_inflate(const uint8_t *data, size_t data_len,
                            size_t *out_len) {
    struct ts_inflate_bits bits;
    uint8_t *out = NULL;
    size_t out_cap = 0, out_pos = 0;
    int final_block;

    ts_inf_init(&bits, data, data_len);

    /* Grow output buffer */
    #define TS_INF_GROW(need) do { \
        if (out_pos + (need) > out_cap) { \
            size_t nc = out_cap ? out_cap * 2 : 16384; \
            while (nc < out_pos + (need)) nc *= 2; \
            uint8_t *nb = (uint8_t *)realloc(out, nc); \
            if (!nb) { free(out); *out_len = 0; return NULL; } \
            out = nb; out_cap = nc; \
        } \
    } while(0)

    do {
        int btype;
        final_block = (int)ts_inf_bits(&bits, 1);
        btype = (int)ts_inf_bits(&bits, 2);

        if (btype == 0) {
            /* Stored (uncompressed) block */
            uint16_t len, nlen;
            ts_inf_align(&bits);
            if (bits.pos + 4 > bits.len) break;
            len = bits.data[bits.pos] | ((uint16_t)bits.data[bits.pos + 1] << 8);
            nlen = bits.data[bits.pos + 2] | ((uint16_t)bits.data[bits.pos + 3] << 8);
            bits.pos += 4;
            (void)nlen;
            if (bits.pos + len > bits.len) break;
            TS_INF_GROW((size_t)len);
            memcpy(out + out_pos, bits.data + bits.pos, len);
            bits.pos += len;
            out_pos += len;
        } else if (btype == 1 || btype == 2) {
            /* Huffman-coded block */
            struct ts_inf_tree lit_tree, dist_tree;

            if (btype == 1) {
                ts_inf_fixed_trees(&lit_tree, &dist_tree);
            } else {
                /* Dynamic Huffman trees */
                int hlit = (int)ts_inf_bits(&bits, 5) + 257;
                int hdist = (int)ts_inf_bits(&bits, 5) + 1;
                int hclen = (int)ts_inf_bits(&bits, 4) + 4;
                uint8_t code_lengths[320];
                struct ts_inf_tree code_tree;
                static const uint8_t code_order[] = {
                    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
                };
                uint8_t cl[19];
                int ci;

                memset(cl, 0, sizeof(cl));
                for (ci = 0; ci < hclen; ci++)
                    cl[code_order[ci]] = (uint8_t)ts_inf_bits(&bits, 3);
                ts_inf_build_tree(&code_tree, cl, 19);

                memset(code_lengths, 0, sizeof(code_lengths));
                ci = 0;
                while (ci < hlit + hdist) {
                    int sym = ts_inf_decode(&bits, &code_tree);
                    if (sym < 0) break;
                    if (sym < 16) {
                        code_lengths[ci++] = (uint8_t)sym;
                    } else if (sym == 16) {
                        int rep = (int)ts_inf_bits(&bits, 2) + 3;
                        uint8_t prev = ci > 0 ? code_lengths[ci - 1] : 0;
                        while (rep-- > 0 && ci < hlit + hdist)
                            code_lengths[ci++] = prev;
                    } else if (sym == 17) {
                        int rep = (int)ts_inf_bits(&bits, 3) + 3;
                        while (rep-- > 0 && ci < hlit + hdist)
                            code_lengths[ci++] = 0;
                    } else if (sym == 18) {
                        int rep = (int)ts_inf_bits(&bits, 7) + 11;
                        while (rep-- > 0 && ci < hlit + hdist)
                            code_lengths[ci++] = 0;
                    }
                }

                ts_inf_build_tree(&lit_tree, code_lengths, hlit);
                ts_inf_build_tree(&dist_tree, code_lengths + hlit, hdist);
            }

            /* Decode symbols */
            for (;;) {
                int sym = ts_inf_decode(&bits, &lit_tree);
                if (sym < 0 || sym == 256) break; /* end of block or error */

                if (sym < 256) {
                    /* Literal byte */
                    TS_INF_GROW(1);
                    out[out_pos++] = (uint8_t)sym;
                } else {
                    /* Length/distance pair */
                    int li = sym - 257;
                    int length, dist_sym, distance;
                    size_t j;

                    if (li < 0 || li >= 29) break;
                    length = ts_inf_len_base[li] +
                             (int)ts_inf_bits(&bits, ts_inf_len_extra[li]);

                    dist_sym = ts_inf_decode(&bits, &dist_tree);
                    if (dist_sym < 0 || dist_sym >= 30) break;
                    distance = ts_inf_dist_base[dist_sym] +
                               (int)ts_inf_bits(&bits, ts_inf_dist_extra[dist_sym]);

                    TS_INF_GROW((size_t)length);
                    /* Copy from back-reference (byte-by-byte for overlapping) */
                    for (j = 0; j < (size_t)length; j++) {
                        if (out_pos >= (size_t)distance)
                            out[out_pos] = out[out_pos - (size_t)distance];
                        else
                            out[out_pos] = 0;
                        out_pos++;
                    }
                }
            }
        } else {
            /* Invalid block type */
            break;
        }
    } while (!final_block);

    #undef TS_INF_GROW

    *out_len = out_pos;
    return out;
}

/* ================================================================== */
/* PNG decoder                                                         */
/* ================================================================== */

static uint32_t ts_png_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Paeth predictor */
static uint8_t ts_png_paeth(uint8_t a, uint8_t b, uint8_t c) {
    int p = (int)a + (int)b - (int)c;
    int pa = p - (int)a; if (pa < 0) pa = -pa;
    int pb = p - (int)b; if (pb < 0) pb = -pb;
    int pc = p - (int)c; if (pc < 0) pc = -pc;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

/*
 * ts_png_decode — decode a PNG file to RGBA pixels.
 * Returns 0 on success, -1 on error.
 * *out_pixels is malloc'd, caller must free.
 */
static int ts_png_decode(const uint8_t *data, size_t len,
                          uint32_t **out_pixels, int *out_w, int *out_h) {
    uint32_t width, height;
    uint8_t bit_depth, color_type;
    int bpp; /* bytes per pixel in raw data */
    size_t idat_total = 0;
    uint8_t *idat_buf = NULL;
    uint8_t *decompressed = NULL;
    size_t decomp_len;
    uint32_t *pixels = NULL;
    uint8_t palette[256][3];
    int palette_count = 0;
    uint8_t trns_alpha[256];
    int has_trns = 0;
    size_t pos;

    *out_pixels = NULL;
    *out_w = 0;
    *out_h = 0;

    /* Check PNG signature */
    if (len < 8) return -1;
    if (data[0] != 137 || data[1] != 80 || data[2] != 78 || data[3] != 71 ||
        data[4] != 13 || data[5] != 10 || data[6] != 26 || data[7] != 10)
        return -1;

    /* Parse chunks */
    pos = 8;

    /* IHDR must be first */
    if (pos + 8 > len) return -1;
    {
        uint32_t chunk_len = ts_png_u32(data + pos);
        if (data[pos + 4] != 'I' || data[pos + 5] != 'H' ||
            data[pos + 6] != 'D' || data[pos + 7] != 'R')
            return -1;
        if (chunk_len < 13 || pos + 12 + chunk_len > len) return -1;

        width = ts_png_u32(data + pos + 8);
        height = ts_png_u32(data + pos + 12);
        bit_depth = data[pos + 16];
        color_type = data[pos + 17];
        /* compression, filter, interlace at +18,+19,+20 */

        if (bit_depth != 8) return -1; /* only 8-bit supported */
        if (width == 0 || height == 0 || width > 8192 || height > 8192)
            return -1;

        pos += 12 + chunk_len; /* skip length(4) + type(4) + data + CRC(4) */
    }

    /* Determine bytes per pixel */
    switch (color_type) {
    case 0: bpp = 1; break; /* grayscale */
    case 2: bpp = 3; break; /* RGB */
    case 3: bpp = 1; break; /* palette */
    case 4: bpp = 2; break; /* gray+alpha */
    case 6: bpp = 4; break; /* RGBA */
    default: return -1;
    }

    /* Collect IDAT chunks + parse PLTE */
    memset(trns_alpha, 255, sizeof(trns_alpha));
    while (pos + 8 <= len) {
        uint32_t chunk_len = ts_png_u32(data + pos);
        const uint8_t *ctype = data + pos + 4;

        if (pos + 12 + chunk_len > len) break;

        if (ctype[0] == 'I' && ctype[1] == 'D' &&
            ctype[2] == 'A' && ctype[3] == 'T') {
            /* Append IDAT data */
            uint8_t *new_buf = (uint8_t *)realloc(idat_buf,
                                                    idat_total + chunk_len);
            if (!new_buf) { free(idat_buf); return -1; }
            idat_buf = new_buf;
            memcpy(idat_buf + idat_total, data + pos + 8, chunk_len);
            idat_total += chunk_len;
        }
        else if (ctype[0] == 'P' && ctype[1] == 'L' &&
                 ctype[2] == 'T' && ctype[3] == 'E') {
            palette_count = (int)(chunk_len / 3);
            if (palette_count > 256) palette_count = 256;
            {
                int i;
                for (i = 0; i < palette_count; i++) {
                    palette[i][0] = data[pos + 8 + i * 3];
                    palette[i][1] = data[pos + 8 + i * 3 + 1];
                    palette[i][2] = data[pos + 8 + i * 3 + 2];
                }
            }
        }
        else if (ctype[0] == 't' && ctype[1] == 'R' &&
                 ctype[2] == 'N' && ctype[3] == 'S') {
            has_trns = 1;
            {
                uint32_t ti;
                for (ti = 0; ti < chunk_len && ti < 256; ti++)
                    trns_alpha[ti] = data[pos + 8 + ti];
            }
        }
        else if (ctype[0] == 'I' && ctype[1] == 'E' &&
                 ctype[2] == 'N' && ctype[3] == 'D') {
            break;
        }

        pos += 12 + chunk_len;
    }

    if (!idat_buf || idat_total < 2) { free(idat_buf); return -1; }

    /* IDAT is zlib-wrapped: skip 2-byte header, inflate the rest */
    {
        size_t zlib_skip = 2; /* CMF + FLG */
        if (idat_total <= zlib_skip) { free(idat_buf); return -1; }
        decompressed = ts_inflate(idat_buf + zlib_skip,
                                   idat_total - zlib_skip, &decomp_len);
        free(idat_buf);
        if (!decompressed) return -1;
    }

    /* Verify decompressed size: height * (1 + width * bpp) */
    {
        size_t expected = (size_t)height * (1 + (size_t)width * (size_t)bpp);
        if (decomp_len < expected) {
            free(decompressed);
            return -1;
        }
    }

    /* Allocate output pixels */
    pixels = (uint32_t *)malloc((size_t)width * (size_t)height * 4);
    if (!pixels) { free(decompressed); return -1; }

    /* De-filter and convert to RGBA */
    {
        size_t stride = 1 + (size_t)width * (size_t)bpp;
        uint32_t y;
        uint8_t *prev_row = NULL;
        uint8_t *cur_row;

        /* Allocate temporary row buffer for de-filtered data */
        uint8_t *row_buf = (uint8_t *)malloc((size_t)width * (size_t)bpp);
        uint8_t *prev_buf = (uint8_t *)malloc((size_t)width * (size_t)bpp);
        if (!row_buf || !prev_buf) {
            free(row_buf); free(prev_buf);
            free(decompressed); free(pixels);
            return -1;
        }
        memset(prev_buf, 0, (size_t)width * (size_t)bpp);

        for (y = 0; y < height; y++) {
            cur_row = decompressed + y * stride;
            uint8_t filter = cur_row[0];
            const uint8_t *raw = cur_row + 1;
            size_t row_bytes = (size_t)width * (size_t)bpp;
            size_t x;

            /* Apply PNG filter */
            for (x = 0; x < row_bytes; x++) {
                uint8_t a = (x >= (size_t)bpp) ? row_buf[x - bpp] : 0;
                uint8_t b = prev_buf[x];
                uint8_t c = (x >= (size_t)bpp) ? prev_buf[x - bpp] : 0;
                uint8_t val = raw[x];

                switch (filter) {
                case 0: row_buf[x] = val; break;
                case 1: row_buf[x] = val + a; break;
                case 2: row_buf[x] = val + b; break;
                case 3: row_buf[x] = val + (uint8_t)(((int)a + (int)b) / 2); break;
                case 4: row_buf[x] = val + ts_png_paeth(a, b, c); break;
                default: row_buf[x] = val; break;
                }
            }

            /* Convert de-filtered row to RGBA pixels */
            {
                uint32_t xi;
                for (xi = 0; xi < width; xi++) {
                    uint8_t r, g, bl, al;
                    size_t off = (size_t)xi * (size_t)bpp;

                    switch (color_type) {
                    case 0: /* Grayscale */
                        r = g = bl = row_buf[off];
                        al = has_trns ? trns_alpha[0] : 255;
                        break;
                    case 2: /* RGB */
                        r = row_buf[off];
                        g = row_buf[off + 1];
                        bl = row_buf[off + 2];
                        al = 255;
                        break;
                    case 3: /* Palette */
                        {
                            uint8_t idx = row_buf[off];
                            if (idx < palette_count) {
                                r = palette[idx][0];
                                g = palette[idx][1];
                                bl = palette[idx][2];
                            } else {
                                r = g = bl = 0;
                            }
                            al = trns_alpha[idx];
                        }
                        break;
                    case 4: /* Gray + alpha */
                        r = g = bl = row_buf[off];
                        al = row_buf[off + 1];
                        break;
                    case 6: /* RGBA */
                        r = row_buf[off];
                        g = row_buf[off + 1];
                        bl = row_buf[off + 2];
                        al = row_buf[off + 3];
                        break;
                    default:
                        r = g = bl = 0; al = 255;
                        break;
                    }

                    pixels[y * width + xi] =
                        ((uint32_t)al << 24) |
                        ((uint32_t)r << 16) |
                        ((uint32_t)g << 8) |
                        (uint32_t)bl;
                }
            }

            /* Swap row buffers */
            {
                uint8_t *tmp = prev_buf;
                prev_buf = row_buf;
                row_buf = tmp;
            }
        }

        free(row_buf);
        free(prev_buf);
    }

    free(decompressed);
    *out_pixels = pixels;
    *out_w = (int)width;
    *out_h = (int)height;
    return 0;
}

/* ================================================================== */
/* BMP decoder                                                         */
/* ================================================================== */

static int ts_bmp_decode(const uint8_t *data, size_t len,
                          uint32_t **out_pixels, int *out_w, int *out_h) {
    uint32_t pixel_offset, width, height, bpp;
    int bottom_up;
    uint32_t *pixels;

    *out_pixels = NULL;
    *out_w = 0;
    *out_h = 0;

    if (len < 54) return -1;
    if (data[0] != 'B' || data[1] != 'M') return -1;

    pixel_offset = data[10] | ((uint32_t)data[11] << 8) |
                   ((uint32_t)data[12] << 16) | ((uint32_t)data[13] << 24);
    width = data[18] | ((uint32_t)data[19] << 8) |
            ((uint32_t)data[20] << 16) | ((uint32_t)data[21] << 24);
    {
        int32_t h_signed = (int32_t)(data[22] | ((uint32_t)data[23] << 8) |
                           ((uint32_t)data[24] << 16) | ((uint32_t)data[25] << 24));
        bottom_up = (h_signed > 0);
        height = (uint32_t)(bottom_up ? h_signed : -h_signed);
    }
    bpp = data[28] | ((uint32_t)data[29] << 8);

    if (width == 0 || height == 0 || width > 8192 || height > 8192)
        return -1;
    if (bpp != 24 && bpp != 32) return -1;

    pixels = (uint32_t *)malloc((size_t)width * (size_t)height * 4);
    if (!pixels) return -1;

    {
        uint32_t row_bytes = (width * (bpp / 8) + 3) & ~3u; /* 4-byte aligned */
        uint32_t y;
        for (y = 0; y < height; y++) {
            uint32_t src_y = bottom_up ? (height - 1 - y) : y;
            const uint8_t *row = data + pixel_offset + src_y * row_bytes;
            uint32_t x;
            if (pixel_offset + src_y * row_bytes + width * (bpp / 8) > len)
                break;
            for (x = 0; x < width; x++) {
                uint8_t r, g, b, a;
                if (bpp == 24) {
                    b = row[x * 3];
                    g = row[x * 3 + 1];
                    r = row[x * 3 + 2];
                    a = 255;
                } else {
                    b = row[x * 4];
                    g = row[x * 4 + 1];
                    r = row[x * 4 + 2];
                    a = row[x * 4 + 3];
                }
                pixels[y * width + x] =
                    ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                    ((uint32_t)g << 8) | (uint32_t)b;
            }
        }
    }

    *out_pixels = pixels;
    *out_w = (int)width;
    *out_h = (int)height;
    return 0;
}

/* ================================================================== */
/* GIF decoder (GIF87a / GIF89a, single frame)                         */
/* ================================================================== */

/*
 * GIF LZW decompressor.  Decodes the LZW sub-block stream that follows
 * each GIF image descriptor into a flat index buffer.
 */
static int ts_gif_lzw_decode(const uint8_t *data, size_t data_len,
                              size_t *pos, int min_code_size,
                              uint8_t *out, int out_len) {
    int clear_code = 1 << min_code_size;
    int eoi_code   = clear_code + 1;
    int code_size  = min_code_size + 1;
    int next_code  = eoi_code + 1;
    int code_mask  = (1 << code_size) - 1;
    int old_code   = -1;
    int out_pos    = 0;

    /* LZW table: prefix + suffix chains, max 4096 entries */
    uint16_t prefix[4096];
    uint8_t  suffix[4096];
    uint16_t tlen[4096];      /* string length for each code */
    int i;

    for (i = 0; i < clear_code; i++) {
        prefix[i] = 0xFFFF;
        suffix[i] = (uint8_t)i;
        tlen[i]   = 1;
    }

    /* Bit reader state — reads from GIF sub-blocks */
    uint32_t bits = 0;
    int nbits = 0;
    size_t p = *pos;
    int block_rem = 0;  /* remaining bytes in current sub-block */

    #define GIF_READ_BYTE(dst) do {                       \
        if (block_rem == 0) {                             \
            if (p >= data_len) goto done;                 \
            block_rem = data[p++];                        \
            if (block_rem == 0) goto done;                \
        }                                                 \
        if (p >= data_len) goto done;                     \
        (dst) = data[p++]; block_rem--;                   \
    } while(0)

    for (;;) {
        /* Need code_size bits */
        while (nbits < code_size) {
            uint8_t byte;
            GIF_READ_BYTE(byte);
            bits |= (uint32_t)byte << nbits;
            nbits += 8;
        }
        int code = (int)(bits & (uint32_t)code_mask);
        bits >>= code_size;
        nbits -= code_size;

        if (code == eoi_code) break;
        if (code == clear_code) {
            code_size = min_code_size + 1;
            next_code = eoi_code + 1;
            code_mask = (1 << code_size) - 1;
            old_code = -1;
            continue;
        }

        int emit_code;
        uint8_t first_char;

        if (code < next_code) {
            emit_code = code;
        } else if (code == next_code && old_code >= 0) {
            /* Special KwKwK case */
            emit_code = old_code;
        } else {
            break; /* corrupt */
        }

        /* Walk the chain to find first_char and string length */
        { int c = emit_code, slen = 0;
          while (c >= clear_code && c < 4096 && slen < 4096) {
              c = prefix[c]; slen++;
          }
          first_char = suffix[c >= 0 && c < clear_code ? c : 0];
        }

        /* Output the string for this code */
        { int c = (code < next_code) ? code : old_code;
          int slen = (c >= 0 && c < 4096) ? (int)tlen[c] : 1;
          /* Write in reverse, then flip */
          int wp = out_pos + slen - 1;
          if (code == next_code) wp++; /* extra char for KwKwK */
          int total = (code < next_code) ? slen : slen + 1;
          if (out_pos + total > out_len) goto done;
          if (code == next_code) {
              out[out_pos + total - 1] = first_char; /* append first_char */
          }
          { int tc = (code < next_code) ? code : old_code;
            int ww = (code < next_code) ? wp : wp - 1;
            while (tc >= clear_code && tc < 4096 && ww >= out_pos) {
                out[ww--] = suffix[tc];
                tc = prefix[tc];
            }
            if (ww >= out_pos) out[ww] = suffix[tc >= 0 && tc < clear_code ? tc : 0];
          }
          out_pos += total;
        }

        /* Add new table entry */
        if (old_code >= 0 && next_code < 4096) {
            prefix[next_code] = (uint16_t)old_code;
            suffix[next_code] = first_char;
            tlen[next_code] = (old_code < 4096) ? tlen[old_code] + 1 : 2;
            next_code++;
            if (next_code > code_mask && code_size < 12) {
                code_size++;
                code_mask = (1 << code_size) - 1;
            }
        }
        old_code = code;
    }
    #undef GIF_READ_BYTE

done:
    /* Skip any remaining sub-blocks */
    while (p < data_len) {
        int bsz = data[p++];
        if (bsz == 0) break;
        p += bsz;
        if (p > data_len) p = data_len;
    }
    *pos = p;
    return out_pos;
}

/*
 * ts_gif_decode — decode first frame of a GIF87a/89a image.
 * Outputs malloc'd ARGB pixel buffer.
 */
static int ts_gif_decode(const uint8_t *data, size_t len,
                          uint32_t **out_pixels, int *out_w, int *out_h) {
    size_t pos;
    int width, height, gct_flag, gct_size, bg_idx;
    uint8_t gct[256][3];
    int transparent_idx = -1;
    uint8_t *indices = NULL;
    uint32_t *pixels = NULL;
    int i;

    if (len < 13) return -1;
    /* Check GIF signature */
    if (data[0] != 'G' || data[1] != 'I' || data[2] != 'F') return -1;

    /* Logical screen descriptor */
    width  = data[6] | (data[7] << 8);
    height = data[8] | (data[9] << 8);
    gct_flag = (data[10] >> 7) & 1;
    gct_size = gct_flag ? (1 << ((data[10] & 7) + 1)) : 0;
    bg_idx   = data[11];

    if (width <= 0 || height <= 0 || width > 4096 || height > 4096)
        return -1;

    pos = 13;

    /* Read Global Color Table */
    if (gct_flag) {
        if (pos + (size_t)gct_size * 3 > len) return -1;
        for (i = 0; i < gct_size && i < 256; i++) {
            gct[i][0] = data[pos++]; /* R */
            gct[i][1] = data[pos++]; /* G */
            gct[i][2] = data[pos++]; /* B */
        }
    }

    /* Scan blocks until we find an image descriptor */
    while (pos < len) {
        uint8_t block = data[pos++];

        if (block == 0x3B) break; /* trailer */

        if (block == 0x21) {
            /* Extension block */
            if (pos >= len) break;
            uint8_t ext_label = data[pos++];

            if (ext_label == 0xF9 && pos + 4 <= len) {
                /* Graphic Control Extension */
                /* int block_sz = data[pos]; */
                uint8_t flags = data[pos + 1];
                int disposal = (flags >> 2) & 7;
                (void)disposal;
                if (flags & 1) {
                    transparent_idx = data[pos + 4];
                }
                pos += data[pos] + 1; /* skip block data */
            }
            /* Skip sub-blocks */
            while (pos < len) {
                int bsz = data[pos++];
                if (bsz == 0) break;
                pos += bsz;
            }
            continue;
        }

        if (block == 0x2C) {
            /* Image Descriptor */
            if (pos + 9 > len) break;
            int img_left   = data[pos] | (data[pos+1] << 8);
            int img_top    = data[pos+2] | (data[pos+3] << 8);
            int img_width  = data[pos+4] | (data[pos+5] << 8);
            int img_height = data[pos+6] | (data[pos+7] << 8);
            uint8_t img_flags = data[pos+8];
            int lct_flag = (img_flags >> 7) & 1;
            int interlace = (img_flags >> 6) & 1;
            int lct_size = lct_flag ? (1 << ((img_flags & 7) + 1)) : 0;
            pos += 9;

            /* Local Color Table (overrides GCT for this frame) */
            uint8_t lct[256][3];
            uint8_t (*ct)[3] = gct;
            int ct_size = gct_size;
            if (lct_flag) {
                if (pos + (size_t)lct_size * 3 > len) break;
                for (i = 0; i < lct_size && i < 256; i++) {
                    lct[i][0] = data[pos++];
                    lct[i][1] = data[pos++];
                    lct[i][2] = data[pos++];
                }
                ct = lct;
                ct_size = lct_size;
            }

            /* LZW minimum code size */
            if (pos >= len) break;
            int min_code_size = data[pos++];
            if (min_code_size < 2 || min_code_size > 11) break;

            /* Decode LZW data into index buffer */
            indices = (uint8_t *)malloc((size_t)img_width * (size_t)img_height);
            if (!indices) break;
            memset(indices, bg_idx < ct_size ? bg_idx : 0,
                   (size_t)img_width * (size_t)img_height);

            int decoded = ts_gif_lzw_decode(data, len, &pos, min_code_size,
                                             indices, img_width * img_height);
            (void)decoded;

            /* Convert indices to ARGB pixels */
            pixels = (uint32_t *)malloc((size_t)width * (size_t)height * 4);
            if (!pixels) { free(indices); return -1; }

            /* Fill background */
            { uint32_t bg_col = 0;
              if (bg_idx < ct_size)
                bg_col = 0xFF000000 |
                          ((uint32_t)ct[bg_idx][0] << 16) |
                          ((uint32_t)ct[bg_idx][1] << 8) |
                          (uint32_t)ct[bg_idx][2];
              for (i = 0; i < width * height; i++)
                pixels[i] = bg_col;
            }

            /* Blit image frame into canvas */
            { int y, x;
              for (y = 0; y < img_height; y++) {
                int src_y = y;
                if (interlace) {
                    /* GIF interlace: passes 0(8,0), 1(8,4), 2(4,2), 3(2,1) */
                    static const int pass_start[] = {0, 4, 2, 1};
                    static const int pass_step[]  = {8, 8, 4, 2};
                    int pass, row = 0;
                    for (pass = 0; pass < 4; pass++) {
                        int ps = pass_start[pass], pp = pass_step[pass];
                        if (y < row + (img_height - ps + pp - 1) / pp) {
                            src_y = ps + (y - row) * pp;
                            break;
                        }
                        row += (img_height - ps + pp - 1) / pp;
                    }
                }
                int dy = img_top + src_y;
                if (dy < 0 || dy >= height) continue;
                for (x = 0; x < img_width; x++) {
                    int dx = img_left + x;
                    if (dx < 0 || dx >= width) continue;
                    int idx = indices[y * img_width + x];
                    if (idx == transparent_idx) continue;
                    if (idx < ct_size) {
                        pixels[dy * width + dx] =
                            0xFF000000 |
                            ((uint32_t)ct[idx][0] << 16) |
                            ((uint32_t)ct[idx][1] << 8) |
                            (uint32_t)ct[idx][2];
                    }
                }
              }
            }

            free(indices);
            *out_pixels = pixels;
            *out_w = width;
            *out_h = height;
            return 0;
        }
    }

    return -1; /* no image frame found */
}

/* ================================================================== */
/* JPEG decode via stb_image (existing PNG/GIF/BMP decoders preserved) */
/* ================================================================== */

#define STBI_NO_STDIO           /* no FILE operations (bare-metal OS) */
#define STBI_ONLY_JPEG          /* only compile JPEG decoder */
#define STBI_NO_LINEAR          /* no linear-light float conversion */
#define STBI_NO_HDR             /* no HDR format support */
#define STBI_ASSERT(x) ((void)0) /* no assert.h on bare-metal */
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

/*
 * ts_jpeg_decode — decode JPEG via stb_image, convert to 0xAARRGGBB.
 */
static int ts_jpeg_decode(const uint8_t *data, size_t len,
                           uint32_t **out_pixels, int *out_w, int *out_h) {
    int w, h, channels;
    unsigned char *rgb = stbi_load_from_memory(data, (int)len, &w, &h,
                                                &channels, 4); /* force RGBA */
    if (!rgb) return -1;

    {
        uint32_t *pixels = (uint32_t *)malloc((size_t)w * (size_t)h * 4);
        if (!pixels) { free(rgb); return -1; }

        /* Convert stb_image RGBA (R,G,B,A bytes) → 0xAARRGGBB uint32 */
        {
            int i, total = w * h;
            for (i = 0; i < total; i++) {
                uint8_t r = rgb[i * 4 + 0];
                uint8_t g = rgb[i * 4 + 1];
                uint8_t b = rgb[i * 4 + 2];
                uint8_t a = rgb[i * 4 + 3];
                pixels[i] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                             ((uint32_t)g << 8) | (uint32_t)b;
            }
        }

        free(rgb);
        *out_pixels = pixels;
        *out_w = w;
        *out_h = h;
        return 0;
    }
}

/* ================================================================== */
/* Auto-detect and decode                                              */
/* ================================================================== */

/*
 * ts_image_decode — detect format and decode image data.
 * Returns 0 on success. *out_pixels is malloc'd ARGB, caller frees.
 */
static int ts_image_decode(const uint8_t *data, size_t len,
                            uint32_t **out_pixels, int *out_w, int *out_h) {
    /* Try PNG first (most common on web) */
    if (len >= 8 && data[0] == 137 && data[1] == 80 &&
        data[2] == 78 && data[3] == 71)
        return ts_png_decode(data, len, out_pixels, out_w, out_h);

    /* Try GIF */
    if (len >= 6 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F')
        return ts_gif_decode(data, len, out_pixels, out_w, out_h);

    /* Try BMP */
    if (len >= 54 && data[0] == 'B' && data[1] == 'M')
        return ts_bmp_decode(data, len, out_pixels, out_w, out_h);

    /* Try JPEG (magic: FF D8 FF) */
    if (len >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
        return ts_jpeg_decode(data, len, out_pixels, out_w, out_h);

    return -1; /* unsupported format */
}

#endif /* TS_IMAGE_H */
