/*
 * ts_layout.h — TaterSurf layout and rendering engine
 *
 * Header-only. Takes HTML tokens + CSS rules and produces a document
 * with positioned render nodes that can be painted to a gfx_ctx_t.
 *
 * Pipeline: HTML tokens → document tree → style resolution → layout → render
 *
 * Layout model:
 *   Block elements flow vertically (div, p, h1-h6, etc.)
 *   Inline elements flow horizontally with word-wrap (span, a, b, etc.)
 *   Text nodes word-wrap at viewport width minus margins/padding.
 *   Headings rendered at scaled font sizes.
 *   Links rendered in accent color with underline.
 */

#ifndef TS_LAYOUT_H
#define TS_LAYOUT_H

#include "ts_html.h"

/* Forward declaration — defined in ts_webcomp.h (included after ts_layout.h) */
static int ts_shadow_get_root(struct ts_dom_ctx *dom, int host_id);
#include "ts_css.h"
#include "ts_dom.h"
#include "../libc/libc.h"
#include "../libc/gfx.h"
#include <stdint.h>
#include <stddef.h>

/* ================================================================== */
/* Constants                                                           */
/* ================================================================== */

#define TS_MAX_NODES         8192
#define TS_MAX_LINKS          512
#define TS_MAX_TEXT_BUF    131072  /* 128KB text storage */
#define TS_FONT_W               8  /* 8x16 bitmap font */
#define TS_FONT_H              16
#define TS_DEFAULT_MARGIN      8   /* default page margin */
#define TS_LINE_SPACING        4   /* extra pixels between lines */
#define TS_PARAGRAPH_SPACING  12   /* extra pixels between paragraphs */

/* Style stack depth for nested tags */
#define TS_STYLE_STACK_MAX   256

/* Position auto value (unset) */
#define TS_POS_AUTO          0x7FFFFFFF

/* ================================================================== */
/* Color theme (dark, matching TaterTOS64v3 desktop)                   */
/* ================================================================== */

#define TS_COL_BG          0x0E131B
#define TS_COL_TEXT         0xE6ECF8
#define TS_COL_TEXT_DIM     0x95A4BC
#define TS_COL_HEADING      0xFFFFFF
#define TS_COL_LINK         0x29B6F6
#define TS_COL_LINK_VISITED 0xB388FF
#define TS_COL_HRULE        0x2A364A
#define TS_COL_PRE_BG       0x161D27
#define TS_COL_CODE_BG      0x1A2233
#define TS_COL_BLOCKQUOTE   0x3A7CA5
#define TS_COL_TRANSPARENT  0xFF000000

/* ================================================================== */
/* Render node types                                                   */
/* ================================================================== */

enum ts_node_type {
    TS_NODE_TEXT = 0,       /* text run with style */
    TS_NODE_BREAK,          /* line break / paragraph break */
    TS_NODE_HRULE,          /* horizontal rule */
    TS_NODE_IMAGE,          /* image placeholder (Level 3 fills in) */
    TS_NODE_LIST_MARKER,    /* bullet or number for list items */
    TS_NODE_INPUT,          /* form input field */
    TS_NODE_BUTTON,         /* form button */
    TS_NODE_CANVAS,         /* canvas 2D — pixel buffer blit */
    TS_NODE_SVG,            /* SVG — rendered to pixel buffer */
};

/* ================================================================== */
/* Box model                                                           */
/* ================================================================== */

#define TS_MAX_BOXES 512

struct ts_block_box {
    /* CSS box model properties (pixels) */
    int margin[4];           /* top, right, bottom, left */
    int padding[4];          /* top, right, bottom, left */
    int border_w[4];         /* top, right, bottom, left */
    uint32_t border_color;
    uint32_t bg_color;       /* TS_COL_TRANSPARENT = no background */
    int explicit_width;      /* 0 = auto (fill parent) */
    int explicit_height;     /* 0 = auto (fit content) */

    /* Computed during layout */
    int x, y;                /* position of border edge */
    int content_x, content_y; /* position of content area */
    int content_w;           /* width available for children */
    int total_w, total_h;    /* total including margin+border+padding */

    /* Node range this box covers */
    int node_start;          /* first child render node index */
    int node_end;            /* last child render node index */

    /* Nesting */
    int parent_box;          /* index of parent box (-1 = root) */
    int depth;               /* nesting depth */

    /* Flexbox container properties */
    int display_flex;        /* 1 = flex container */
    int flex_direction;      /* 0=row (default), 1=column */
    int justify_content;     /* 0=flex-start, 1=center, 2=flex-end, 3=space-between, 4=space-around */
    int align_items;         /* 0=stretch, 1=flex-start, 2=center, 3=flex-end */
    int gap;                 /* gap between flex items (px) */

    /* Flex item properties (when parent is display:flex) */
    int flex_grow;           /* default 0 */
    int flex_shrink;         /* default 1 */
    int flex_basis;          /* px, 0=auto */

    /* Set by flex layout — position pre-computed, skip normal positioning */
    int flex_positioned;

    /* CSS Grid container properties */
    int display_grid;
    int grid_col_count;            /* number of column tracks (max 8) */
    int grid_row_count;            /* number of row tracks (max 8) */
    int grid_col_track[8];         /* >0: px, <0: -(fr*100) e.g. 1fr=-100 */
    int grid_row_track[8];
    /* Grid item placement */
    int grid_col_start, grid_col_end;   /* 1-based line, 0=auto, negative=span */
    int grid_row_start, grid_row_end;
    int grid_positioned;

    /* Position */
    int position;                  /* 0=static, 1=relative, 2=absolute, 3=fixed */
    int pos_top, pos_right, pos_bottom, pos_left; /* TS_POS_AUTO = unset */
    int z_index;

    /* Float */
    int css_float;                 /* 0=none, 1=left, 2=right */
    int css_clear;                 /* 0=none, 1=left, 2=right, 3=both */

    /* Overflow */
    int overflow;                  /* 0=visible, 1=hidden, 2=scroll, 3=auto */

    /* Inline-block: block sizing with inline flow positioning */
    int display_inline_block;
    int inline_block_positioned;   /* set during layout when box is placed inline */

    /* Transforms (Step 13) */
    int transform_x;               /* translateX in px */
    int transform_y;               /* translateY in px */
    int transform_scale_pct;       /* scale * 100 (100 = 1.0, 0 = unset) */
    int opacity_pct;               /* 0-100 (100 = fully visible, 0 = unset/default) */
    int visibility_hidden;         /* 1 = visibility:hidden (takes space, invisible) */
};

/* ================================================================== */
/* Text style                                                          */
/* ================================================================== */

struct ts_text_style {
    uint32_t fg_color;
    uint32_t bg_color;      /* TS_COL_TRANSPARENT = no background */
    int bold;
    int italic;
    int underline;
    int strikethrough;
    int font_scale;         /* 1 = normal, 2 = large, 3 = h1 */
    int link_index;         /* -1 = not a link */
    int preformatted;       /* 1 = inside <pre>, preserve whitespace */
    int monospace;          /* 1 = inside <code>/<pre> */
    int list_depth;         /* nesting depth for indentation */
    int text_align;         /* 0=left, 1=center, 2=right */
    int box_index;          /* index into boxes[] or -1 */
};

/* ================================================================== */
/* Render node                                                         */
/* ================================================================== */

struct ts_render_node {
    enum ts_node_type type;
    /* Text content (for TEXT and LIST_MARKER nodes) */
    uint32_t text_offset;   /* offset into text_buf */
    uint16_t text_len;      /* byte length */
    /* Style */
    struct ts_text_style style;
    /* Computed layout */
    int x, y;               /* pixel position in document space */
    int w, h;               /* pixel size */
    /* For BREAK nodes: amount of extra vertical spacing */
    int break_height;
    /* For IMAGE nodes */
    int img_cache_idx;      /* index into image cache (-1 = no image) */
    int img_requested_w;    /* width="" attribute value */
    int img_requested_h;    /* height="" attribute value */
    /* For INPUT/BUTTON nodes */
    int form_idx;           /* index into form_inputs array (-1 = none) */
    /* DOM association (set during ts_doc_build_from_dom) */
    int dom_node_id;        /* DOM node pool index (-1 = none) */
};

/* ================================================================== */
/* Link                                                                */
/* ================================================================== */

struct ts_link {
    char href[512];
    int x1, y1, x2, y2;    /* bounding box (union of all nodes in link) */
    int node_start;         /* first node index */
    int node_end;           /* last node index (inclusive) */
};

/* ================================================================== */
/* Document                                                            */
/* ================================================================== */

struct ts_document {
    /* Render nodes (flat list, in document order) */
    struct ts_render_node nodes[TS_MAX_NODES];
    int node_count;

    /* Text storage (all text runs point into here) */
    char text_buf[TS_MAX_TEXT_BUF];
    uint32_t text_used;

    /* Links */
    struct ts_link links[TS_MAX_LINKS];
    int link_count;

    /* Form inputs */
    #define TS_MAX_FORM_INPUTS 64
    struct ts_form_input {
        char name[64];           /* input name="" */
        char value[256];         /* current value */
        int value_len;
        char type[16];           /* "text", "password", "submit", "hidden" */
        char placeholder[128];   /* placeholder text */
        int node_idx;            /* render node index */
        int cursor;              /* cursor position */
        int focused;             /* 1 if this input is focused */
        char action[512];        /* form action URL */
        char method[8];          /* GET or POST */
    } form_inputs[TS_MAX_FORM_INPUTS];
    int form_input_count;
    int focused_input;           /* index of focused input, -1 = none */

    /* Block boxes (for box model layout) */
    struct ts_block_box boxes[TS_MAX_BOXES];
    int box_count;

    /* Document dimensions (computed after layout) */
    int content_width;
    int content_height;

    /* Page metadata */
    char title[256];
    char base_url[512];     /* base URL for resolving relative links */

    /* Page-level background (from <body>/<html> CSS) */
    uint32_t page_bg;

    /* Stylesheet */
    struct ts_stylesheet stylesheet;

    /* External resource URLs discovered during parse */
    char external_css[16][512];
    int external_css_count;
    char external_js[16][512];
    int external_js_count;
    char external_img[64][512];
    int external_img_count;

    /* Iframe URLs discovered during parse */
    char iframe_src[8][512];
    int iframe_x[8], iframe_y[8], iframe_w[8], iframe_h[8];
    int iframe_count;

    /* Image cache (decoded pixel data) */
    struct {
        char url[512];
        uint32_t *pixels;
        int w, h;
        int used;
    } img_cache[64];
    int img_cache_count;

    /* Canvas pixel buffers (Step 14) */
    struct {
        uint32_t *pixels;
        int w, h;
        int used;
    } canvas_cache[16];
    int canvas_cache_count;
};

/* ================================================================== */
/* Style stack (used during document building)                         */
/* ================================================================== */

struct ts_style_entry {
    struct ts_text_style style;
    char tag[16];           /* tag name that pushed this style */
};

struct ts_style_stack {
    struct ts_style_entry entries[TS_STYLE_STACK_MAX];
    int count;
};

/* ================================================================== */
/* Internal helpers                                                    */
/* ================================================================== */

static void ts_doc__zero(void *p, size_t n) {
    char *d = (char *)p; while (n--) *d++ = 0;
}

/* Append text to document text_buf, return offset */
static uint32_t ts_doc__store_text(struct ts_document *doc,
                                    const char *text, size_t len) {
    uint32_t offset = doc->text_used;
    if (doc->text_used + len + 1 > TS_MAX_TEXT_BUF) {
        /* Truncate if text buffer full */
        len = TS_MAX_TEXT_BUF - doc->text_used - 1;
    }
    if (len > 0) {
        memcpy(doc->text_buf + doc->text_used, text, len);
        doc->text_used += (uint32_t)len;
        doc->text_buf[doc->text_used] = '\0';
    }
    return offset;
}

/* Emit a render node */
static struct ts_render_node *ts_doc__emit(struct ts_document *doc,
                                            enum ts_node_type type) {
    struct ts_render_node *n;
    if (doc->node_count >= TS_MAX_NODES) return NULL;
    n = &doc->nodes[doc->node_count++];
    ts_doc__zero(n, sizeof(*n));
    n->type = type;
    n->style.fg_color = TS_COL_TEXT;
    n->style.bg_color = TS_COL_TRANSPARENT;
    n->style.font_scale = 1;
    n->style.link_index = -1;
    n->img_cache_idx = -1;
    n->dom_node_id = -1;
    return n;
}

/* Push style onto stack */
static void ts_style_push(struct ts_style_stack *stack,
                           const struct ts_text_style *style,
                           const char *tag, size_t tag_len) {
    if (stack->count >= TS_STYLE_STACK_MAX) return;
    {
        struct ts_style_entry *e = &stack->entries[stack->count++];
        e->style = *style;
        if (tag_len >= sizeof(e->tag)) tag_len = sizeof(e->tag) - 1;
        memcpy(e->tag, tag, tag_len);
        e->tag[tag_len] = '\0';
    }
}

/* Pop style from stack, matching tag name */
static struct ts_text_style ts_style_pop(struct ts_style_stack *stack,
                                          const char *tag, size_t tag_len) {
    int i;
    /* Search from top for matching tag */
    for (i = stack->count - 1; i >= 0; i--) {
        size_t j;
        int match = 1;
        size_t elen = 0;
        { const char *p = stack->entries[i].tag; while (*p) { elen++; p++; } }
        if (elen != tag_len) continue;
        for (j = 0; j < tag_len; j++) {
            char a = stack->entries[i].tag[j];
            char b = tag[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) { match = 0; break; }
        }
        if (match) {
            struct ts_text_style result = stack->entries[i].style;
            /* Remove this entry and shift down */
            {
                int k;
                for (k = i; k < stack->count - 1; k++)
                    stack->entries[k] = stack->entries[k + 1];
            }
            stack->count--;
            return result;
        }
    }
    /* No match — return current top or default */
    if (stack->count > 0)
        return stack->entries[stack->count - 1].style;
    {
        struct ts_text_style def;
        ts_doc__zero(&def, sizeof(def));
        def.fg_color = TS_COL_TEXT;
        def.bg_color = TS_COL_TRANSPARENT;
        def.font_scale = 1;
        def.link_index = -1;
        return def;
    }
}

/* Get current style (top of stack) */
static struct ts_text_style ts_style_current(const struct ts_style_stack *stack) {
    if (stack->count > 0)
        return stack->entries[stack->count - 1].style;
    {
        struct ts_text_style def;
        ts_doc__zero(&def, sizeof(def));
        def.fg_color = TS_COL_TEXT;
        def.bg_color = TS_COL_TRANSPARENT;
        def.font_scale = 1;
        def.link_index = -1;
        return def;
    }
}

/* Collapse whitespace in text (non-pre mode) */
static size_t ts_doc__collapse_ws(const char *src, size_t len,
                                   char *dst, size_t max) {
    size_t si = 0, di = 0;
    int last_was_space = 0;

    while (si < len && di < max - 1) {
        char c = src[si++];
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c == ' ') {
            if (!last_was_space) {
                dst[di++] = ' ';
                last_was_space = 1;
            }
        } else {
            dst[di++] = c;
            last_was_space = 0;
        }
    }
    dst[di] = '\0';
    return di;
}

/* ================================================================== */
/* Document building — HTML tokens → render nodes                      */
/* ================================================================== */

/*
 * ts_doc_init — zero-initialize a document.
 */
static void ts_doc_init(struct ts_document *doc) {
    ts_doc__zero(doc, sizeof(*doc));
    doc->focused_input = -1;
    doc->page_bg = TS_COL_BG;
}

/*
 * ts_doc_reinit — reset render tree but preserve stylesheet and image cache.
 * Avoids the ~5 MB stack copy that caused stack overflow on rebuild.
 */
static void ts_doc_reinit(struct ts_document *doc) {
    doc->node_count = 0;
    doc->text_used = 0;
    doc->link_count = 0;
    doc->form_input_count = 0;
    doc->focused_input = -1;
    doc->box_count = 0;
    doc->content_width = 0;
    doc->content_height = 0;
    doc->title[0] = '\0';
    doc->base_url[0] = '\0';
    doc->page_bg = TS_COL_BG;
    doc->external_css_count = 0;
    doc->external_js_count = 0;
    doc->external_img_count = 0;
    doc->canvas_cache_count = 0;
    /* stylesheet and img_cache are intentionally preserved */
}

/* ================================================================== */
/* CSS cascade — apply stylesheet rules to element styles              */
/* ================================================================== */

/* Parse CSS shorthand for 1-4 value properties (margin, padding) */
static void ts_css__parse_sides(const char *value, int base_px, int out[4]) {
    int vals[4] = {0, 0, 0, 0};
    int count = 0;
    const char *p = value;

    while (*p && count < 4) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (*p == 'a' && p[1] == 'u' && p[2] == 't' && p[3] == 'o') {
            vals[count++] = 0;
            p += 4;
        } else {
            vals[count++] = ts_css_to_px(p, base_px);
            /* Skip past number+unit */
            if (*p == '-') p++;
            while ((*p >= '0' && *p <= '9') || *p == '.') p++;
            while (*p && *p != ' ' && *p != ',') p++;
        }
    }

    if (count == 1) {
        out[0] = out[1] = out[2] = out[3] = vals[0];
    } else if (count == 2) {
        out[0] = out[2] = vals[0]; /* top/bottom */
        out[1] = out[3] = vals[1]; /* right/left */
    } else if (count == 3) {
        out[0] = vals[0]; out[1] = out[3] = vals[1]; out[2] = vals[2];
    } else if (count == 4) {
        out[0] = vals[0]; out[1] = vals[1]; out[2] = vals[2]; out[3] = vals[3];
    }
}

/* Parse CSS grid track list: "100px 1fr 2fr" or "repeat(3, 1fr)" */
static void ts_doc__parse_grid_tracks(const char *value,
                                       int *tracks, int *count, int max_tracks) {
    const char *p = value;
    *count = 0;

    while (*p && *count < max_tracks) {
        while (*p == ' ') p++;
        if (!*p) break;

        /* repeat(N, size) */
        if (*p == 'r' && p[1] == 'e' && p[2] == 'p') {
            int n = 0, ri, rep_val = -100;
            while (*p && *p != '(') p++;
            if (*p == '(') p++;
            while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); p++; }
            while (*p == ' ' || *p == ',') p++;
            if (*p >= '0' && *p <= '9') {
                int num = 0;
                while (*p >= '0' && *p <= '9') { num = num * 10 + (*p - '0'); p++; }
                if (*p == 'f' && p[1] == 'r') { rep_val = -(num > 0 ? num : 1) * 100; p += 2; }
                else { while (*p && *p != ')' && *p != ' ') p++; rep_val = num > 0 ? num : 100; }
            }
            while (*p && *p != ')') p++;
            if (*p == ')') p++;
            for (ri = 0; ri < n && *count < max_tracks; ri++)
                tracks[(*count)++] = rep_val;
            continue;
        }

        /* minmax(min, max) — use max value */
        if (*p == 'm' && p[1] == 'i' && p[2] == 'n' && p[3] == 'm') {
            while (*p && *p != ',') p++;
            if (*p == ',') p++;
            while (*p == ' ') p++;
            if (*p >= '0' && *p <= '9') {
                int num = 0;
                while (*p >= '0' && *p <= '9') { num = num * 10 + (*p - '0'); p++; }
                if (*p == 'f' && p[1] == 'r') { tracks[(*count)++] = -(num > 0 ? num : 1) * 100; p += 2; }
                else { tracks[(*count)++] = num; while (*p && *p != ')') p++; }
            } else { tracks[(*count)++] = -100; }
            while (*p && *p != ')') p++;
            if (*p == ')') p++;
            continue;
        }

        /* auto */
        if (*p == 'a' && p[1] == 'u' && p[2] == 't' && p[3] == 'o') {
            tracks[(*count)++] = 0;
            p += 4;
            continue;
        }

        /* Number: could be px or fr */
        if (*p >= '0' && *p <= '9') {
            int num = 0;
            while (*p >= '0' && *p <= '9') { num = num * 10 + (*p - '0'); p++; }
            if (*p == '.') { p++; while (*p >= '0' && *p <= '9') p++; }
            if (*p == 'f' && p[1] == 'r') {
                tracks[(*count)++] = -(num > 0 ? num : 1) * 100;
                p += 2;
            } else {
                while (*p && *p != ' ' && *p != ')') p++;
                tracks[(*count)++] = num > 0 ? num : 0;
            }
            continue;
        }

        /* skip unknown */
        while (*p && *p != ' ') p++;
    }
}

/* Parse grid placement: "1", "1 / 3", "span 2" */
static void ts_doc__parse_grid_placement(const char *value,
                                          int *start, int *end) {
    const char *p = value;
    *start = 0;
    *end = 0;
    while (*p == ' ') p++;

    /* span N */
    if (*p == 's' && p[1] == 'p' && p[2] == 'a' && p[3] == 'n') {
        int n = 0;
        p += 4;
        while (*p == ' ') p++;
        while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); p++; }
        *start = 0;
        *end = -(n > 0 ? n : 1);
        return;
    }

    /* Start line number */
    {
        int num = 0, neg = 0;
        if (*p == '-') { neg = 1; p++; }
        while (*p >= '0' && *p <= '9') { num = num * 10 + (*p - '0'); p++; }
        *start = neg ? -num : num;
    }

    /* " / end" */
    while (*p == ' ') p++;
    if (*p == '/') {
        p++;
        while (*p == ' ') p++;
        if (*p == 's' && p[1] == 'p' && p[2] == 'a' && p[3] == 'n') {
            int n = 0;
            p += 4; while (*p == ' ') p++;
            while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); p++; }
            *end = *start + (n > 0 ? n : 1);
        } else {
            int num = 0, neg = 0;
            if (*p == '-') { neg = 1; p++; }
            while (*p >= '0' && *p <= '9') { num = num * 10 + (*p - '0'); p++; }
            *end = neg ? -num : num;
        }
    }
}

/* Apply CSS properties to a block box */
static void ts_doc__apply_box_props(struct ts_block_box *box,
                                     const struct ts_css_property *props,
                                     int count) {
    int i;
    for (i = 0; i < count; i++) {
        const char *nm = props[i].name;
        const char *vl = props[i].value;

        /* margin shorthand */
        if (strcmp(nm, "margin") == 0)
            ts_css__parse_sides(vl, 0, box->margin);
        else if (strcmp(nm, "margin-top") == 0)
            box->margin[0] = ts_css_to_px(vl, 0);
        else if (strcmp(nm, "margin-right") == 0)
            box->margin[1] = ts_css_to_px(vl, 0);
        else if (strcmp(nm, "margin-bottom") == 0)
            box->margin[2] = ts_css_to_px(vl, 0);
        else if (strcmp(nm, "margin-left") == 0)
            box->margin[3] = ts_css_to_px(vl, 0);

        /* padding shorthand */
        else if (strcmp(nm, "padding") == 0)
            ts_css__parse_sides(vl, 0, box->padding);
        else if (strcmp(nm, "padding-top") == 0)
            box->padding[0] = ts_css_to_px(vl, 0);
        else if (strcmp(nm, "padding-right") == 0)
            box->padding[1] = ts_css_to_px(vl, 0);
        else if (strcmp(nm, "padding-bottom") == 0)
            box->padding[2] = ts_css_to_px(vl, 0);
        else if (strcmp(nm, "padding-left") == 0)
            box->padding[3] = ts_css_to_px(vl, 0);

        /* border shorthand: "1px solid #color" */
        else if (strcmp(nm, "border") == 0) {
            int bw = ts_css_to_px(vl, 0);
            if (bw < 1) bw = 1;
            box->border_w[0] = box->border_w[1] = box->border_w[2] = box->border_w[3] = bw;
            /* Try to extract color from the value */
            {
                const char *c = vl;
                /* Skip past number+unit */
                while (*c && *c != ' ') c++;
                while (*c == ' ') c++;
                /* Skip past "solid"/"dashed"/etc */
                while (*c && *c != ' ') c++;
                while (*c == ' ') c++;
                if (*c) {
                    uint32_t col = ts_css_color(c);
                    if (col != 0xFF000000) box->border_color = col;
                }
            }
        }
        else if (strcmp(nm, "border-width") == 0) {
            int bw = ts_css_to_px(vl, 0);
            box->border_w[0] = box->border_w[1] = box->border_w[2] = box->border_w[3] = bw;
        }
        else if (strcmp(nm, "border-color") == 0) {
            uint32_t c = ts_css_color(vl);
            if (c != 0xFF000000) box->border_color = c;
        }
        else if (strcmp(nm, "border-top") == 0)
            box->border_w[0] = ts_css_to_px(vl, 0);
        else if (strcmp(nm, "border-bottom") == 0)
            box->border_w[2] = ts_css_to_px(vl, 0);
        else if (strcmp(nm, "border-left") == 0)
            box->border_w[3] = ts_css_to_px(vl, 0);
        else if (strcmp(nm, "border-right") == 0)
            box->border_w[1] = ts_css_to_px(vl, 0);

        /* width / height */
        else if (strcmp(nm, "width") == 0)
            box->explicit_width = ts_css_to_px(vl, 900);
        else if (strcmp(nm, "height") == 0)
            box->explicit_height = ts_css_to_px(vl, 600);
        else if (strcmp(nm, "max-width") == 0) {
            int mw = ts_css_to_px(vl, 900);
            if (box->explicit_width == 0 || box->explicit_width > mw)
                box->explicit_width = mw;
        }

        /* background */
        else if (strcmp(nm, "background-color") == 0 ||
                 strcmp(nm, "background") == 0) {
            uint32_t c = ts_css_color(vl);
            if (c != 0xFF000000) box->bg_color = c;
        }

        /* display */
        else if (strcmp(nm, "display") == 0) {
            if (strstr(vl, "inline-block"))
                box->display_inline_block = 1;
            else if (strstr(vl, "flex"))
                box->display_flex = 1;
            else if (strstr(vl, "grid"))
                box->display_grid = 1;
        }

        /* flex-direction */
        else if (strcmp(nm, "flex-direction") == 0) {
            if (strstr(vl, "column")) box->flex_direction = 1;
            else box->flex_direction = 0;
        }

        /* justify-content */
        else if (strcmp(nm, "justify-content") == 0) {
            if (strstr(vl, "space-between")) box->justify_content = 3;
            else if (strstr(vl, "space-around")) box->justify_content = 4;
            else if (strstr(vl, "center")) box->justify_content = 1;
            else if (strstr(vl, "flex-end") || strstr(vl, "end"))
                box->justify_content = 2;
            else box->justify_content = 0;
        }

        /* align-items */
        else if (strcmp(nm, "align-items") == 0) {
            if (strstr(vl, "center")) box->align_items = 2;
            else if (strstr(vl, "flex-end") || strstr(vl, "end"))
                box->align_items = 3;
            else if (strstr(vl, "flex-start") || strstr(vl, "start"))
                box->align_items = 1;
            else box->align_items = 0; /* stretch */
        }

        /* gap / column-gap / row-gap */
        else if (strcmp(nm, "gap") == 0 ||
                 strcmp(nm, "column-gap") == 0 ||
                 strcmp(nm, "row-gap") == 0) {
            box->gap = ts_css_to_px(vl, 0);
        }

        /* flex-grow */
        else if (strcmp(nm, "flex-grow") == 0) {
            box->flex_grow = ts_css_to_px(vl, 0);
        }

        /* flex-shrink */
        else if (strcmp(nm, "flex-shrink") == 0) {
            box->flex_shrink = ts_css_to_px(vl, 0);
        }

        /* flex-basis */
        else if (strcmp(nm, "flex-basis") == 0) {
            if (strstr(vl, "auto")) box->flex_basis = 0;
            else box->flex_basis = ts_css_to_px(vl, 0);
        }

        /* flex shorthand: <grow> [<shrink>] [<basis>] */
        else if (strcmp(nm, "flex") == 0) {
            if (strstr(vl, "none")) {
                box->flex_grow = 0;
                box->flex_shrink = 0;
                box->flex_basis = 0;
            } else {
                const char *p = vl;
                int g = 0;
                while (*p == ' ') p++;
                while (*p >= '0' && *p <= '9') {
                    g = g * 10 + (*p - '0'); p++;
                }
                box->flex_grow = g;
                while (*p == ' ') p++;
                if (*p >= '0' && *p <= '9') {
                    int s = 0;
                    while (*p >= '0' && *p <= '9') {
                        s = s * 10 + (*p - '0'); p++;
                    }
                    box->flex_shrink = s;
                }
                while (*p == ' ') p++;
                if (*p && !strstr(p, "auto")) {
                    box->flex_basis = ts_css_to_px(p, 0);
                }
            }
        }

        /* grid-template-columns */
        else if (strcmp(nm, "grid-template-columns") == 0) {
            ts_doc__parse_grid_tracks(vl, box->grid_col_track,
                                       &box->grid_col_count, 8);
        }

        /* grid-template-rows */
        else if (strcmp(nm, "grid-template-rows") == 0) {
            ts_doc__parse_grid_tracks(vl, box->grid_row_track,
                                       &box->grid_row_count, 8);
        }

        /* grid-column / grid-column-start */
        else if (strcmp(nm, "grid-column") == 0 ||
                 strcmp(nm, "grid-column-start") == 0) {
            ts_doc__parse_grid_placement(vl, &box->grid_col_start,
                                          &box->grid_col_end);
        }
        else if (strcmp(nm, "grid-column-end") == 0) {
            box->grid_col_end = ts_css_to_px(vl, 0);
        }

        /* grid-row / grid-row-start */
        else if (strcmp(nm, "grid-row") == 0 ||
                 strcmp(nm, "grid-row-start") == 0) {
            ts_doc__parse_grid_placement(vl, &box->grid_row_start,
                                          &box->grid_row_end);
        }
        else if (strcmp(nm, "grid-row-end") == 0) {
            box->grid_row_end = ts_css_to_px(vl, 0);
        }

        /* position */
        else if (strcmp(nm, "position") == 0) {
            if (strstr(vl, "relative")) box->position = 1;
            else if (strstr(vl, "absolute")) box->position = 2;
            else if (strstr(vl, "fixed")) box->position = 3;
            else if (strstr(vl, "sticky")) box->position = 3; /* treat as fixed */
            else box->position = 0;
        }

        /* top / right / bottom / left */
        else if (strcmp(nm, "top") == 0) {
            if (strstr(vl, "auto")) box->pos_top = TS_POS_AUTO;
            else box->pos_top = ts_css_to_px(vl, 0);
        }
        else if (strcmp(nm, "right") == 0) {
            if (strstr(vl, "auto")) box->pos_right = TS_POS_AUTO;
            else box->pos_right = ts_css_to_px(vl, 0);
        }
        else if (strcmp(nm, "bottom") == 0) {
            if (strstr(vl, "auto")) box->pos_bottom = TS_POS_AUTO;
            else box->pos_bottom = ts_css_to_px(vl, 0);
        }
        else if (strcmp(nm, "left") == 0) {
            if (strstr(vl, "auto")) box->pos_left = TS_POS_AUTO;
            else box->pos_left = ts_css_to_px(vl, 0);
        }

        /* z-index */
        else if (strcmp(nm, "z-index") == 0) {
            if (strstr(vl, "auto")) box->z_index = 0;
            else box->z_index = ts_css_to_px(vl, 0);
        }

        /* float */
        else if (strcmp(nm, "float") == 0) {
            if (strstr(vl, "left")) box->css_float = 1;
            else if (strstr(vl, "right")) box->css_float = 2;
            else box->css_float = 0;
        }

        /* clear */
        else if (strcmp(nm, "clear") == 0) {
            if (strstr(vl, "both")) box->css_clear = 3;
            else if (strstr(vl, "left")) box->css_clear = 1;
            else if (strstr(vl, "right")) box->css_clear = 2;
            else box->css_clear = 0;
        }

        /* overflow */
        else if (strcmp(nm, "overflow") == 0 ||
                 strcmp(nm, "overflow-x") == 0 ||
                 strcmp(nm, "overflow-y") == 0) {
            if (strstr(vl, "hidden")) box->overflow = 1;
            else if (strstr(vl, "scroll")) box->overflow = 2;
            else if (strstr(vl, "auto")) box->overflow = 3;
            else box->overflow = 0;
        }

        /* opacity */
        else if (strcmp(nm, "opacity") == 0) {
            double op = 1.0;
            { const char *p = vl; int neg = 0; double val = 0, frac = 0, div = 1;
              if (*p == '-') { neg = 1; p++; }
              while (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); p++; }
              if (*p == '.') { p++; while (*p >= '0' && *p <= '9') { frac = frac * 10 + (*p - '0'); div *= 10; p++; } }
              op = val + frac / div;
              if (neg) op = -op;
            }
            if (op < 0) op = 0;
            if (op > 1) op = 1;
            box->opacity_pct = (int)(op * 100);
        }

        /* visibility */
        else if (strcmp(nm, "visibility") == 0) {
            if (strstr(vl, "hidden") || strstr(vl, "collapse"))
                box->visibility_hidden = 1;
            else
                box->visibility_hidden = 0;
        }

        /* transform */
        else if (strcmp(nm, "transform") == 0) {
            const char *p = vl;
            while (*p) {
                /* translateX(Npx) */
                if (strncmp(p, "translateX(", 11) == 0) {
                    box->transform_x = ts_css_to_px(p + 11, 0);
                    while (*p && *p != ')') p++; if (*p) p++;
                }
                /* translateY(Npx) */
                else if (strncmp(p, "translateY(", 11) == 0) {
                    box->transform_y = ts_css_to_px(p + 11, 0);
                    while (*p && *p != ')') p++; if (*p) p++;
                }
                /* translate(X, Y) */
                else if (strncmp(p, "translate(", 10) == 0) {
                    box->transform_x = ts_css_to_px(p + 10, 0);
                    { const char *comma = strchr(p + 10, ',');
                      if (comma) box->transform_y = ts_css_to_px(comma + 1, 0);
                    }
                    while (*p && *p != ')') p++; if (*p) p++;
                }
                /* scale(N) */
                else if (strncmp(p, "scale(", 6) == 0) {
                    double sv = 1.0;
                    { const char *sp = p + 6; double val = 0, frac = 0, div = 1;
                      while (*sp >= '0' && *sp <= '9') { val = val * 10 + (*sp - '0'); sp++; }
                      if (*sp == '.') { sp++; while (*sp >= '0' && *sp <= '9') { frac = frac * 10 + (*sp - '0'); div *= 10; sp++; } }
                      sv = val + frac / div;
                    }
                    box->transform_scale_pct = (int)(sv * 100);
                    while (*p && *p != ')') p++; if (*p) p++;
                }
                /* none */
                else if (strncmp(p, "none", 4) == 0) {
                    box->transform_x = 0; box->transform_y = 0;
                    box->transform_scale_pct = 0;
                    p += 4;
                }
                else p++;
            }
        }

        /* transition / animation — parse to prevent JS errors, snap to final state */
        else if (strcmp(nm, "transition") == 0 ||
                 strcmp(nm, "transition-property") == 0 ||
                 strcmp(nm, "transition-duration") == 0 ||
                 strcmp(nm, "transition-timing-function") == 0 ||
                 strcmp(nm, "transition-delay") == 0 ||
                 strcmp(nm, "animation") == 0 ||
                 strcmp(nm, "animation-name") == 0 ||
                 strcmp(nm, "animation-duration") == 0 ||
                 strcmp(nm, "animation-timing-function") == 0 ||
                 strcmp(nm, "animation-delay") == 0 ||
                 strcmp(nm, "animation-iteration-count") == 0 ||
                 strcmp(nm, "animation-direction") == 0 ||
                 strcmp(nm, "animation-fill-mode") == 0 ||
                 strcmp(nm, "animation-play-state") == 0) {
            /* Acknowledged but not animated — values snap immediately */
            (void)vl;
        }
    }
}

/* Emit a block box for an element — returns box index or -1 */
static int ts_doc__open_box(struct ts_document *doc,
                             const struct ts_token *tag,
                             int parent_box) {
    struct ts_block_box *box;
    char cls_buf[128], id_buf[128], style_attr[512];
    char tag_lc[32];
    size_t ti;
    int i, j;

    if (doc->box_count >= TS_MAX_BOXES) return -1;

    box = &doc->boxes[doc->box_count];
    memset(box, 0, sizeof(*box));
    box->bg_color = TS_COL_TRANSPARENT;
    box->flex_shrink = 1;    /* CSS default */
    box->pos_top = TS_POS_AUTO;
    box->pos_right = TS_POS_AUTO;
    box->pos_bottom = TS_POS_AUTO;
    box->pos_left = TS_POS_AUTO;
    box->border_color = 0x2A364A; /* default border color */
    box->parent_box = parent_box;
    box->depth = (parent_box >= 0) ? doc->boxes[parent_box].depth + 1 : 0;
    box->node_start = doc->node_count;

    /* Extract tag info for CSS matching */
    for (ti = 0; ti < tag->tag_name_len && ti < 31; ti++)
        tag_lc[ti] = ts_html__lower(tag->tag_name[ti]);
    tag_lc[ti] = '\0';
    cls_buf[0] = '\0';
    id_buf[0] = '\0';
    ts_tok_attr_get(tag, "class", cls_buf, sizeof(cls_buf));
    ts_tok_attr_get(tag, "id", id_buf, sizeof(id_buf));

    /* Match stylesheet rules for box properties */
    { int __rc = doc->stylesheet.rule_count;
      if (__rc < 0 || __rc > TS_CSS_MAX_RULES) __rc = 0;
    for (i = 0; i < __rc; i++) {
        struct ts_css_rule *rule = &doc->stylesheet.rules[i];
        int __sc = rule->selector_count;
        if (__sc < 0 || __sc > TS_CSS_MAX_SELECTORS) __sc = 0;
        for (j = 0; j < __sc; j++) {
            struct ts_css_selector *sel = &rule->selectors[j];
            if (sel->part_count > 0 && sel->part_count <= TS_CSS_MAX_SELECTOR_PARTS) {
                struct ts_css_selector_part *part =
                    &sel->parts[sel->part_count - 1];
                if (ts_css_match_part(part, tag_lc, cls_buf, id_buf)) {
                    ts_doc__apply_box_props(box, rule->props, rule->prop_count);
                    break;
                }
            }
        }
    }
    }

    /* Inline style="" has highest priority */
    if (ts_tok_attr_get(tag, "style", style_attr, sizeof(style_attr)) > 0) {
        struct ts_css_property inline_props[16];
        int inline_count = 0;
        ts_css_parse_inline(style_attr, inline_props, &inline_count, 16);
        ts_doc__apply_box_props(box, inline_props, inline_count);
    }

    return doc->box_count++;
}

static void ts_doc__close_box(struct ts_document *doc, int box_idx) {
    if (box_idx < 0 || box_idx >= doc->box_count) return;
    doc->boxes[box_idx].node_end = doc->node_count - 1;
}

/* Apply an array of CSS properties to a text style */
static void ts_doc__apply_css_props(struct ts_text_style *style,
                                     const struct ts_css_property *props,
                                     int count) {
    int i;
    for (i = 0; i < count; i++) {
        const char *nm = props[i].name;
        const char *vl = props[i].value;

        if (nm[0] == 'c' && nm[1] == 'o' && nm[2] == 'l' &&
            nm[3] == 'o' && nm[4] == 'r' && nm[5] == '\0') {
            uint32_t c = ts_css_color(vl);
            if (c != 0xFF000000) style->fg_color = c;
        }
        else if (strcmp(nm, "background-color") == 0 ||
                 strcmp(nm, "background") == 0) {
            uint32_t c = ts_css_color(vl);
            if (c != 0xFF000000) style->bg_color = c;
        }
        else if (strcmp(nm, "font-weight") == 0) {
            if (strstr(vl, "bold") || (vl[0] >= '6' && vl[0] <= '9'))
                style->bold = 1;
            else if (strstr(vl, "normal") || vl[0] == '4' || vl[0] == '3')
                style->bold = 0;
        }
        else if (strcmp(nm, "font-style") == 0) {
            style->italic = (strstr(vl, "italic") != NULL ||
                             strstr(vl, "oblique") != NULL);
        }
        else if (strcmp(nm, "text-decoration") == 0) {
            if (strstr(vl, "none")) {
                style->underline = 0;
                style->strikethrough = 0;
            }
            if (strstr(vl, "underline")) style->underline = 1;
            if (strstr(vl, "line-through")) style->strikethrough = 1;
        }
        else if (strcmp(nm, "text-align") == 0) {
            if (strstr(vl, "center")) style->text_align = 1;
            else if (strstr(vl, "right")) style->text_align = 2;
            else style->text_align = 0;
        }
        else if (strcmp(nm, "font-size") == 0) {
            int px = ts_css_to_px(vl, 16);
            /* Named sizes */
            if (strstr(vl, "xx-large") || strstr(vl, "xxx-large")) px = 32;
            else if (strstr(vl, "x-large")) px = 24;
            else if (strstr(vl, "large")) px = 20;
            else if (strstr(vl, "medium")) px = 16;
            else if (strstr(vl, "small")) px = 13;
            else if (strstr(vl, "x-small")) px = 10;
            else if (strstr(vl, "xx-small")) px = 9;
            /* Map to scale: 1=16px base, 2=20-27px, 3=28px+ */
            if (px >= 28) style->font_scale = 3;
            else if (px >= 20) style->font_scale = 2;
            else style->font_scale = 1;
        }
        /* font-family — acknowledged, always uses bitmap fallback */
        else if (strcmp(nm, "font-family") == 0) {
            /* Store for getComputedStyle but rendering always uses 8x16 bitmap */
            (void)vl;
        }
        /* font shorthand: font: [style] [weight] size[/line-height] family */
        else if (strcmp(nm, "font") == 0) {
            /* Extract size from the shorthand */
            const char *fp = vl;
            /* Skip style/variant/weight keywords */
            while (*fp) {
                if (*fp >= '0' && *fp <= '9') {
                    int px = ts_css_to_px(fp, 16);
                    if (px >= 28) style->font_scale = 3;
                    else if (px >= 20) style->font_scale = 2;
                    else style->font_scale = 1;
                    break;
                }
                /* Skip word */
                while (*fp && *fp != ' ') fp++;
                while (*fp == ' ') fp++;
            }
            /* Check for bold in the shorthand */
            if (strstr(vl, "bold") || strstr(vl, "700") ||
                strstr(vl, "800") || strstr(vl, "900"))
                style->bold = 1;
            if (strstr(vl, "italic"))
                style->italic = 1;
        }
        /* letter-spacing, word-spacing, line-height — parse to prevent layout errors */
        else if (strcmp(nm, "letter-spacing") == 0 ||
                 strcmp(nm, "word-spacing") == 0 ||
                 strcmp(nm, "line-height") == 0) {
            (void)vl; /* acknowledged, bitmap font is fixed-width */
        }
    }
}

/* Check if CSS properties include display:none or visibility:hidden */
static int ts_doc__css_is_hidden(const struct ts_css_property *props, int count) {
    int i;
    for (i = 0; i < count; i++) {
        if (strcmp(props[i].name, "display") == 0 &&
            strcmp(props[i].value, "none") == 0)
            return 1;
        if (strcmp(props[i].name, "visibility") == 0 &&
            strcmp(props[i].value, "hidden") == 0)
            return 1;
    }
    return 0;
}

/* Ancestor chain for CSS selector matching.
 * Each entry represents a parent element with its tag, class, and id. */
#define TS_CSS_MAX_ANCESTORS 64
struct ts_css_ancestor {
    char tag[32];
    char cls[128];
    char id[128];
};

struct ts_css_ancestor_chain {
    struct ts_css_ancestor entries[TS_CSS_MAX_ANCESTORS];
    int count;  /* number of ancestors (0 = at root) */
};

static void ts_css_ancestor_push(struct ts_css_ancestor_chain *chain,
                                  const char *tag, const char *cls,
                                  const char *id) {
    if (chain->count >= TS_CSS_MAX_ANCESTORS) return;
    {
        struct ts_css_ancestor *a = &chain->entries[chain->count++];
        strncpy(a->tag, tag, 31); a->tag[31] = '\0';
        if (cls) { strncpy(a->cls, cls, 127); a->cls[127] = '\0'; }
        else a->cls[0] = '\0';
        if (id) { strncpy(a->id, id, 127); a->id[127] = '\0'; }
        else a->id[0] = '\0';
    }
}

static void ts_css_ancestor_pop(struct ts_css_ancestor_chain *chain) {
    if (chain->count > 0) chain->count--;
}

/* Push ancestor from a token (for CSS matching in ts_doc_build) */
static void ts_doc__push_ancestor_from_tok(struct ts_css_ancestor_chain *chain,
                                            const struct ts_token *tag) {
    char tag_lc[32], cls_buf[128], id_buf[128];
    size_t i;
    for (i = 0; i < tag->tag_name_len && i < 31; i++)
        tag_lc[i] = ts_html__lower(tag->tag_name[i]);
    tag_lc[i] = '\0';
    cls_buf[0] = '\0';
    id_buf[0] = '\0';
    ts_tok_attr_get(tag, "class", cls_buf, sizeof(cls_buf));
    ts_tok_attr_get(tag, "id", id_buf, sizeof(id_buf));
    ts_css_ancestor_push(chain, tag_lc, cls_buf, id_buf);
}

/* Match a full compound selector against element + ancestor chain.
 * Walks selector parts right-to-left, checking combinators:
 *   ' ' = descendant (search up chain for any matching ancestor)
 *   '>' = child (check immediate parent only)
 */
static int ts_css_match_full(struct ts_css_selector *sel,
                              const char *tag, const char *cls,
                              const char *id,
                              const struct ts_css_ancestor_chain *ancestors) {
    int pi; /* selector part index, walk right-to-left */
    int ai; /* ancestor chain index (ancestors->count-1 = immediate parent) */

    if (sel->part_count <= 0) return 0;

    /* Last part must match the current element */
    if (!ts_css_match_part(&sel->parts[sel->part_count - 1], tag, cls, id))
        return 0;

    /* Single-part selector: matched */
    if (sel->part_count == 1) return 1;

    /* Walk remaining parts right-to-left against ancestor chain */
    ai = ancestors->count - 1; /* start at immediate parent */
    for (pi = sel->part_count - 2; pi >= 0; pi--) {
        struct ts_css_selector_part *part = &sel->parts[pi];
        /* The combinator on sel->parts[pi+1] tells us the relationship */
        char comb = sel->parts[pi + 1].combinator;

        if (comb == '>') {
            /* Child combinator: must match immediate parent (ai) */
            if (ai < 0) return 0;
            if (!ts_css_match_part(part, ancestors->entries[ai].tag,
                                    ancestors->entries[ai].cls,
                                    ancestors->entries[ai].id))
                return 0;
            ai--;
        } else {
            /* Descendant combinator (space): search up the chain */
            int found = 0;
            while (ai >= 0) {
                if (ts_css_match_part(part, ancestors->entries[ai].tag,
                                       ancestors->entries[ai].cls,
                                       ancestors->entries[ai].id)) {
                    found = 1;
                    ai--;
                    break;
                }
                ai--;
            }
            if (!found) return 0;
        }
    }

    return 1;
}

/* Match an element against all stylesheet rules and apply CSS to style.
 * Sets *hidden to 1 if display:none or visibility:hidden.
 * Uses ancestor chain for proper compound selector matching. */
static void ts_doc__match_apply_css(struct ts_document *doc,
                                     const char *tag_name,
                                     const char *cls,
                                     const char *id_attr,
                                     struct ts_text_style *style,
                                     int *hidden,
                                     const struct ts_css_ancestor_chain *ancestors) {
    int i, j;
    { int __rc = doc->stylesheet.rule_count;
      if (__rc < 0 || __rc > TS_CSS_MAX_RULES) __rc = 0;
    for (i = 0; i < __rc; i++) {
        struct ts_css_rule *rule = &doc->stylesheet.rules[i];
        int __sc = rule->selector_count;
        if (__sc < 0 || __sc > TS_CSS_MAX_SELECTORS) __sc = 0;
        for (j = 0; j < __sc; j++) {
            struct ts_css_selector *sel = &rule->selectors[j];
            if (sel->part_count > 0 && sel->part_count <= TS_CSS_MAX_SELECTOR_PARTS) {
                if (ts_css_match_full(sel, tag_name, cls, id_attr, ancestors)) {
                    ts_doc__apply_css_props(style, rule->props,
                                             rule->prop_count);
                    if (ts_doc__css_is_hidden(rule->props, rule->prop_count))
                        *hidden = 1;
                    break;
                }
            }
        }
    }
    }
}

/* Apply CSS rules + inline style="" to current style for an element token.
 * Returns 1 if element should be hidden (display:none).
 * ancestors may be NULL for backward compatibility (matches last part only). */
static int ts_doc__apply_element_css(struct ts_document *doc,
                                      const struct ts_token *tag,
                                      struct ts_text_style *style,
                                      const struct ts_css_ancestor_chain *ancestors) {
    char tag_lc[32], cls_buf[128], id_buf[128], style_attr[512];
    size_t i;
    int hidden = 0;
    struct ts_css_ancestor_chain empty_chain;

    /* Extract lowercase tag name */
    for (i = 0; i < tag->tag_name_len && i < 31; i++)
        tag_lc[i] = ts_html__lower(tag->tag_name[i]);
    tag_lc[i] = '\0';

    cls_buf[0] = '\0';
    id_buf[0] = '\0';
    ts_tok_attr_get(tag, "class", cls_buf, sizeof(cls_buf));
    ts_tok_attr_get(tag, "id", id_buf, sizeof(id_buf));

    /* Use empty chain if NULL */
    if (!ancestors) {
        empty_chain.count = 0;
        ancestors = &empty_chain;
    }

    /* Match stylesheet rules */
    ts_doc__match_apply_css(doc, tag_lc, cls_buf, id_buf, style, &hidden,
                             ancestors);

    /* Apply inline style="" attribute (highest priority) */
    if (ts_tok_attr_get(tag, "style", style_attr, sizeof(style_attr)) > 0) {
        struct ts_css_property inline_props[16];
        int inline_count = 0;
        ts_css_parse_inline(style_attr, inline_props, &inline_count, 16);
        ts_doc__apply_css_props(style, inline_props, inline_count);
        if (ts_doc__css_is_hidden(inline_props, inline_count))
            hidden = 1;
    }

    return hidden;
}

/* ================================================================== */
/* Document building — HTML tokens → render nodes                      */
/* ================================================================== */

/*
 * ts_doc_build — main pipeline: tokenize HTML → build render nodes.
 *
 * Walks HTML tokens, maintains a style stack, emits render nodes
 * for text, breaks, rules, images, and list markers.
 */
static void ts_doc_build(struct ts_document *doc,
                          const char *html, size_t html_len) {
    struct ts_tokenizer tok;
    struct ts_token t;
    struct ts_style_stack style_stack;
    struct ts_text_style cur_style;
    struct ts_css_ancestor_chain ancestors;
    int in_head = 0;
    int in_title = 0;
    int in_script = 0;
    int title_pos = 0;
    int ordered_counter = 0;
    int in_style = 0;
    int display_none_depth = 0;
    int box_stack[64];         /* stack of open box indices */
    int box_stack_depth = 0;
    int cur_box = -1;          /* current parent box */

    ts_doc__zero(&style_stack, sizeof(style_stack));
    ts_doc__zero(&cur_style, sizeof(cur_style));
    ancestors.count = 0;
    cur_style.fg_color = TS_COL_TEXT;
    cur_style.bg_color = TS_COL_TRANSPARENT;
    cur_style.font_scale = 1;
    cur_style.link_index = -1;
    cur_style.box_index = -1;

    ts_tok_init(&tok, html, html_len);

    while (ts_tok_next(&tok, &t)) {
        if (t.type == TS_TOK_TAG_OPEN || t.type == TS_TOK_TAG_SELF_CLOSE) {
            const char *tn = t.tag_name;
            size_t tnl = t.tag_name_len;

            /* ---- <head> ---- */
            if (ts_html__tag_name_eq(tn, tnl, "head")) {
                in_head = 1;
                continue;
            }

            /* ---- <title> ---- */
            if (ts_html__tag_name_eq(tn, tnl, "title")) {
                in_title = 1;
                title_pos = 0;
                continue;
            }

            /* Skip rendering inside <head> (except <title>, <style>, <link>, <meta>) */
            if (in_head) {
                /* Still collect <link> CSS and <script> external URLs */
                if (ts_html__tag_name_eq(tn, tnl, "link")) {
                    char rel[64], href[512];
                    if (ts_tok_attr_get(&t, "rel", rel, sizeof(rel)) >= 0 &&
                        strstr(rel, "stylesheet")) {
                        if (ts_tok_attr_get(&t, "href", href, sizeof(href)) > 0) {
                            if (doc->external_css_count < 16) {
                                strncpy(doc->external_css[doc->external_css_count],
                                        href, 511);
                                doc->external_css[doc->external_css_count][511] = '\0';
                                doc->external_css_count++;
                            }
                        }
                    }
                    continue;
                }
                if (ts_html__tag_name_eq(tn, tnl, "script")) {
                    char src_url[512];
                    if (ts_tok_attr_get(&t, "src", src_url, sizeof(src_url)) > 0) {
                        if (doc->external_js_count < 16) {
                            strncpy(doc->external_js[doc->external_js_count],
                                    src_url, 511);
                            doc->external_js[doc->external_js_count][511] = '\0';
                            doc->external_js_count++;
                        }
                    }
                    continue;
                }
                if (!ts_html__tag_name_eq(tn, tnl, "style"))
                    continue;
            }

            /* ---- <style> ---- */
            if (ts_html__tag_name_eq(tn, tnl, "style")) {
                /* Next TEXT token contains CSS — in_style tells text handler */
                in_style = 1;
                continue;
            }

            /* ---- <link rel="stylesheet"> ---- */
            if (ts_html__tag_name_eq(tn, tnl, "link")) {
                char rel[64], href[512];
                if (ts_tok_attr_get(&t, "rel", rel, sizeof(rel)) >= 0 &&
                    strstr(rel, "stylesheet")) {
                    if (ts_tok_attr_get(&t, "href", href, sizeof(href)) > 0) {
                        if (doc->external_css_count < 16) {
                            strncpy(doc->external_css[doc->external_css_count],
                                    href, 511);
                            doc->external_css[doc->external_css_count][511] = '\0';
                            doc->external_css_count++;
                        }
                    }
                }
                continue;
            }

            /* ---- <script> — invisible, skip content ---- */
            if (ts_html__tag_name_eq(tn, tnl, "script")) {
                /* Collect external script URLs */
                if (ts_html__tag_name_eq(tn, tnl, "script")) {
                    char src_url[512];
                    if (ts_tok_attr_get(&t, "src", src_url, sizeof(src_url)) > 0) {
                        if (doc->external_js_count < 16) {
                            strncpy(doc->external_js[doc->external_js_count],
                                    src_url, 511);
                            doc->external_js[doc->external_js_count][511] = '\0';
                            doc->external_js_count++;
                        }
                    }
                }
                in_script = 1;
                continue;
            }

            /* ---- <html>, <body> — capture page background ---- */
            if (ts_html__tag_name_eq(tn, tnl, "body") ||
                ts_html__tag_name_eq(tn, tnl, "html")) {
                fprintf(stderr, "PRE_BG: tag=%.*s tnl=%d rc=%d nodes=%d boxes=%d\n",
                        (int)tnl, tn, (int)tnl, doc->stylesheet.rule_count,
                        doc->node_count, doc->box_count);
                ts_style_push(&style_stack, &cur_style, tn, tnl); ts_doc__push_ancestor_from_tok(&ancestors, &t);
                ts_doc__apply_element_css(doc, &t, &cur_style, &ancestors);
                fprintf(stderr, "PAGE_BG: matched <%.*s> bg=0x%06X rules=%d\n",
                        (int)tnl, tn, (unsigned)cur_style.bg_color,
                        doc->stylesheet.rule_count);
                /* If CSS set a background, use it as page background */
                if (cur_style.bg_color != TS_COL_TRANSPARENT)
                    doc->page_bg = cur_style.bg_color;
                /* Keep dark theme when no CSS background is specified */
                continue;
            }

            /* ---- Heading tags: h1-h6 ---- */
            if (tnl == 2 && tn[0] == 'h' && tn[1] >= '1' && tn[1] <= '6') {
                struct ts_render_node *br = ts_doc__emit(doc, TS_NODE_BREAK);
                if (br) br->break_height = TS_PARAGRAPH_SPACING;
                {
                    int level = tn[1] - '0';
                    struct ts_text_style heading = cur_style;
                    heading.fg_color = TS_COL_HEADING;
                    heading.bold = 1;
                    heading.font_scale = (level <= 1) ? 3 : (level <= 2) ? 2 : 1;
                    ts_style_push(&style_stack, &cur_style, tn, tnl); ts_doc__push_ancestor_from_tok(&ancestors, &t);
                    cur_style = heading;
                    if (ts_doc__apply_element_css(doc, &t, &cur_style, &ancestors))
                        display_none_depth++;
                }
                continue;
            }

            /* ---- <b>, <strong> ---- */
            if (ts_html__tag_name_eq(tn, tnl, "b") ||
                ts_html__tag_name_eq(tn, tnl, "strong")) {
                ts_style_push(&style_stack, &cur_style, tn, tnl); ts_doc__push_ancestor_from_tok(&ancestors, &t);
                cur_style.bold = 1;
                ts_doc__apply_element_css(doc, &t, &cur_style, &ancestors);
                continue;
            }

            /* ---- <i>, <em> ---- */
            if (ts_html__tag_name_eq(tn, tnl, "i") ||
                ts_html__tag_name_eq(tn, tnl, "em")) {
                ts_style_push(&style_stack, &cur_style, tn, tnl); ts_doc__push_ancestor_from_tok(&ancestors, &t);
                cur_style.italic = 1;
                ts_doc__apply_element_css(doc, &t, &cur_style, &ancestors);
                continue;
            }

            /* ---- <u> ---- */
            if (ts_html__tag_name_eq(tn, tnl, "u")) {
                ts_style_push(&style_stack, &cur_style, tn, tnl); ts_doc__push_ancestor_from_tok(&ancestors, &t);
                cur_style.underline = 1;
                ts_doc__apply_element_css(doc, &t, &cur_style, &ancestors);
                continue;
            }

            /* ---- <s>, <strike>, <del> ---- */
            if (ts_html__tag_name_eq(tn, tnl, "s") ||
                ts_html__tag_name_eq(tn, tnl, "strike") ||
                ts_html__tag_name_eq(tn, tnl, "del")) {
                ts_style_push(&style_stack, &cur_style, tn, tnl); ts_doc__push_ancestor_from_tok(&ancestors, &t);
                cur_style.strikethrough = 1;
                ts_doc__apply_element_css(doc, &t, &cur_style, &ancestors);
                continue;
            }

            /* ---- <a href="..."> ---- */
            if (ts_html__tag_name_eq(tn, tnl, "a")) {
                char href[512];
                int href_len = ts_tok_attr_get(&t, "href", href, sizeof(href));
                ts_style_push(&style_stack, &cur_style, tn, tnl); ts_doc__push_ancestor_from_tok(&ancestors, &t);
                cur_style.fg_color = TS_COL_LINK;
                cur_style.underline = 1;
                if (href_len >= 0 && doc->link_count < TS_MAX_LINKS) {
                    struct ts_link *link = &doc->links[doc->link_count];
                    ts_doc__zero(link, sizeof(*link));
                    memcpy(link->href, href,
                           (size_t)href_len < sizeof(link->href) - 1
                            ? (size_t)href_len : sizeof(link->href) - 1);
                    link->node_start = doc->node_count;
                    cur_style.link_index = doc->link_count;
                    doc->link_count++;
                }
                ts_doc__apply_element_css(doc, &t, &cur_style, &ancestors);
                continue;
            }

            /* ---- <pre>, <code> ---- */
            if (ts_html__tag_name_eq(tn, tnl, "pre")) {
                struct ts_render_node *br = ts_doc__emit(doc, TS_NODE_BREAK);
                if (br) br->break_height = TS_LINE_SPACING;
                ts_style_push(&style_stack, &cur_style, tn, tnl); ts_doc__push_ancestor_from_tok(&ancestors, &t);
                cur_style.preformatted = 1;
                cur_style.monospace = 1;
                cur_style.bg_color = TS_COL_PRE_BG;
                ts_doc__apply_element_css(doc, &t, &cur_style, &ancestors);
                continue;
            }
            if (ts_html__tag_name_eq(tn, tnl, "code")) {
                ts_style_push(&style_stack, &cur_style, tn, tnl); ts_doc__push_ancestor_from_tok(&ancestors, &t);
                cur_style.monospace = 1;
                cur_style.bg_color = TS_COL_CODE_BG;
                ts_doc__apply_element_css(doc, &t, &cur_style, &ancestors);
                continue;
            }

            /* ---- <blockquote> ---- */
            if (ts_html__tag_name_eq(tn, tnl, "blockquote")) {
                struct ts_render_node *br = ts_doc__emit(doc, TS_NODE_BREAK);
                if (br) br->break_height = TS_PARAGRAPH_SPACING;
                ts_style_push(&style_stack, &cur_style, tn, tnl); ts_doc__push_ancestor_from_tok(&ancestors, &t);
                cur_style.fg_color = TS_COL_BLOCKQUOTE;
                cur_style.italic = 1;
                cur_style.list_depth++;
                ts_doc__apply_element_css(doc, &t, &cur_style, &ancestors);
                continue;
            }

            /* ---- <span> ---- */
            if (ts_html__tag_name_eq(tn, tnl, "span")) {
                ts_style_push(&style_stack, &cur_style, tn, tnl); ts_doc__push_ancestor_from_tok(&ancestors, &t);
                if (ts_doc__apply_element_css(doc, &t, &cur_style, &ancestors))
                    display_none_depth++;
                continue;
            }

            /* ---- <p>, <div>, <section>, <article>, <header>, <footer>, <main>, <nav> ---- */
            if (ts_html__tag_name_eq(tn, tnl, "p") ||
                ts_html__tag_name_eq(tn, tnl, "div") ||
                ts_html__tag_name_eq(tn, tnl, "section") ||
                ts_html__tag_name_eq(tn, tnl, "article") ||
                ts_html__tag_name_eq(tn, tnl, "header") ||
                ts_html__tag_name_eq(tn, tnl, "footer") ||
                ts_html__tag_name_eq(tn, tnl, "main") ||
                ts_html__tag_name_eq(tn, tnl, "nav") ||
                ts_html__tag_name_eq(tn, tnl, "aside")) {
                /* Open box first to check for display:inline-block */
                {
                    int bi = ts_doc__open_box(doc, &t, cur_box);
                    int is_ib = (bi >= 0 && doc->boxes[bi].display_inline_block);
                    /* Inline-block: emit zero-height break; block: normal break */
                    if (!is_ib) {
                        struct ts_render_node *br = ts_doc__emit(doc, TS_NODE_BREAK);
                        if (br) {
                            br->break_height = ts_html__tag_name_eq(tn, tnl, "p")
                                                ? TS_PARAGRAPH_SPACING : TS_LINE_SPACING;
                        }
                    } else {
                        struct ts_render_node *br = ts_doc__emit(doc, TS_NODE_BREAK);
                        if (br) br->break_height = 0;
                    }
                    if (bi >= 0) {
                        if (box_stack_depth < 64) box_stack[box_stack_depth++] = cur_box;
                        cur_box = bi;
                        cur_style.box_index = bi;
                    }
                }
                /* Push style so CSS can override, pop on close tag */
                ts_style_push(&style_stack, &cur_style, tn, tnl); ts_doc__push_ancestor_from_tok(&ancestors, &t);
                if (ts_doc__apply_element_css(doc, &t, &cur_style, &ancestors))
                    display_none_depth++;
                continue;
            }

            /* ---- <br> ---- */
            if (ts_html__tag_name_eq(tn, tnl, "br")) {
                struct ts_render_node *br = ts_doc__emit(doc, TS_NODE_BREAK);
                if (br) br->break_height = 0;
                continue;
            }

            /* ---- <hr> ---- */
            if (ts_html__tag_name_eq(tn, tnl, "hr")) {
                struct ts_render_node *br = ts_doc__emit(doc, TS_NODE_BREAK);
                if (br) br->break_height = TS_LINE_SPACING;
                ts_doc__emit(doc, TS_NODE_HRULE);
                {
                    struct ts_render_node *br2 = ts_doc__emit(doc, TS_NODE_BREAK);
                    if (br2) br2->break_height = TS_LINE_SPACING;
                }
                continue;
            }

            /* ---- <ul>, <ol> ---- */
            if (ts_html__tag_name_eq(tn, tnl, "ul") ||
                ts_html__tag_name_eq(tn, tnl, "ol")) {
                struct ts_render_node *br = ts_doc__emit(doc, TS_NODE_BREAK);
                if (br) br->break_height = TS_LINE_SPACING;
                ts_style_push(&style_stack, &cur_style, tn, tnl); ts_doc__push_ancestor_from_tok(&ancestors, &t);
                cur_style.list_depth++;
                if (ts_html__tag_name_eq(tn, tnl, "ol")) ordered_counter = 0;
                continue;
            }

            /* ---- <li> ---- */
            if (ts_html__tag_name_eq(tn, tnl, "li")) {
                struct ts_render_node *br = ts_doc__emit(doc, TS_NODE_BREAK);
                if (br) br->break_height = 2;
                /* Emit list marker */
                {
                    struct ts_render_node *marker = ts_doc__emit(doc, TS_NODE_LIST_MARKER);
                    if (marker) {
                        marker->style = cur_style;
                        marker->style.fg_color = TS_COL_TEXT_DIM;
                        /* For ordered lists, show number */
                        if (ordered_counter >= 0) {
                            ordered_counter++;
                            char numbuf[12];
                            int ni = 0;
                            int val = ordered_counter;
                            if (val == 0) { numbuf[ni++] = '0'; }
                            else {
                                char tmp[12]; int ti = 0;
                                while (val > 0) { tmp[ti++] = '0' + (char)(val % 10); val /= 10; }
                                while (ti > 0) numbuf[ni++] = tmp[--ti];
                            }
                            numbuf[ni++] = '.';
                            numbuf[ni++] = ' ';
                            numbuf[ni] = '\0';
                            marker->text_offset = ts_doc__store_text(doc, numbuf, (size_t)ni);
                            marker->text_len = (uint16_t)ni;
                        } else {
                            /* Unordered: bullet */
                            marker->text_offset = ts_doc__store_text(doc, "* ", 2);
                            marker->text_len = 2;
                        }
                    }
                }
                continue;
            }

            /* ---- <img> ---- */
            if (ts_html__tag_name_eq(tn, tnl, "img")) {
                struct ts_render_node *img = ts_doc__emit(doc, TS_NODE_IMAGE);
                if (img) {
                    char val[32];
                    char src_url[512];
                    img->style = cur_style;
                    img->img_cache_idx = -1;
                    if (ts_tok_attr_get(&t, "width", val, sizeof(val)) >= 0)
                        img->img_requested_w = ts_css_to_px(val, 0);
                    if (ts_tok_attr_get(&t, "height", val, sizeof(val)) >= 0)
                        img->img_requested_h = ts_css_to_px(val, 0);
                    /* Collect image URL for resource loader */
                    if (ts_tok_attr_get(&t, "src", src_url, sizeof(src_url)) > 0) {
                        if (doc->external_img_count < 64) {
                            strncpy(doc->external_img[doc->external_img_count],
                                    src_url, 511);
                            doc->external_img[doc->external_img_count][511] = '\0';
                            /* Link image node to cache slot */
                            img->img_cache_idx = doc->external_img_count;
                            doc->external_img_count++;
                        }
                    }
                    /* Alt text stored for display when image not loaded */
                    {
                        char alt[128];
                        if (ts_tok_attr_get(&t, "alt", alt, sizeof(alt)) > 0) {
                            size_t alen = 0;
                            while (alt[alen]) alen++;
                            img->text_offset = ts_doc__store_text(doc, alt, alen);
                            img->text_len = (uint16_t)alen;
                        }
                    }
                }
                continue;
            }

            /* ---- <table>, <tr>, <td>, <th> ---- */
            if (ts_html__tag_name_eq(tn, tnl, "table") ||
                ts_html__tag_name_eq(tn, tnl, "tr")) {
                struct ts_render_node *br = ts_doc__emit(doc, TS_NODE_BREAK);
                if (br) br->break_height = TS_LINE_SPACING;
                continue;
            }
            if (ts_html__tag_name_eq(tn, tnl, "td") ||
                ts_html__tag_name_eq(tn, tnl, "th")) {
                /* Separate cells with tab-like spacing */
                struct ts_render_node *n = ts_doc__emit(doc, TS_NODE_TEXT);
                if (n) {
                    n->style = cur_style;
                    n->text_offset = ts_doc__store_text(doc, "  |  ", 5);
                    n->text_len = 5;
                }
                continue;
            }

            /* ---- <form> ---- */
            if (ts_html__tag_name_eq(tn, tnl, "form")) {
                /* Store action URL for form inputs */
                continue;
            }

            /* ---- <input> ---- */
            if (ts_html__tag_name_eq(tn, tnl, "input")) {
                char inp_type[16] = "text";
                char inp_name[64] = {0};
                char inp_value[256] = {0};
                char inp_placeholder[128] = {0};
                ts_tok_attr_get(&t, "type", inp_type, sizeof(inp_type));
                ts_tok_attr_get(&t, "name", inp_name, sizeof(inp_name));
                ts_tok_attr_get(&t, "value", inp_value, sizeof(inp_value));
                ts_tok_attr_get(&t, "placeholder", inp_placeholder,
                                sizeof(inp_placeholder));

                /* Hidden inputs — no rendering */
                if (strcmp(inp_type, "hidden") == 0) {
                    if (doc->form_input_count < TS_MAX_FORM_INPUTS) {
                        struct ts_form_input *fi =
                            &doc->form_inputs[doc->form_input_count];
                        memset(fi, 0, sizeof(*fi));
                        strncpy(fi->name, inp_name, 63);
                        strncpy(fi->value, inp_value, 255);
                        fi->value_len = (int)strlen(inp_value);
                        strncpy(fi->type, "hidden", 15);
                        fi->node_idx = -1;
                        doc->form_input_count++;
                    }
                    continue;
                }

                /* Submit button */
                if (strcmp(inp_type, "submit") == 0) {
                    struct ts_render_node *btn = ts_doc__emit(doc, TS_NODE_BUTTON);
                    if (btn) {
                        btn->style = cur_style;
                        btn->form_idx = -1;
                        const char *label = inp_value[0] ? inp_value : "Submit";
                        size_t ll = strlen(label);
                        btn->text_offset = ts_doc__store_text(doc, label, ll);
                        btn->text_len = (uint16_t)ll;
                    }
                    continue;
                }

                /* Text/password/search/email/url input */
                {
                    struct ts_render_node *inp = ts_doc__emit(doc, TS_NODE_INPUT);
                    if (inp) {
                        inp->style = cur_style;
                        inp->form_idx = doc->form_input_count;
                        /* Store form input data */
                        if (doc->form_input_count < TS_MAX_FORM_INPUTS) {
                            struct ts_form_input *fi =
                                &doc->form_inputs[doc->form_input_count];
                            memset(fi, 0, sizeof(*fi));
                            strncpy(fi->name, inp_name, 63);
                            strncpy(fi->value, inp_value, 255);
                            fi->value_len = (int)strlen(inp_value);
                            strncpy(fi->type, inp_type, 15);
                            strncpy(fi->placeholder, inp_placeholder, 127);
                            fi->node_idx = doc->node_count - 1;
                            doc->form_input_count++;
                        }
                    }
                }
                continue;
            }

            /* ---- <button> ---- */
            if (ts_html__tag_name_eq(tn, tnl, "button")) {
                ts_style_push(&style_stack, &cur_style, tn, tnl); ts_doc__push_ancestor_from_tok(&ancestors, &t);
                /* Button content is text between <button>...</button> */
                continue;
            }

            /* ---- <textarea> ---- */
            if (ts_html__tag_name_eq(tn, tnl, "textarea")) {
                struct ts_render_node *inp = ts_doc__emit(doc, TS_NODE_INPUT);
                if (inp) {
                    char ta_name[64] = {0};
                    inp->style = cur_style;
                    ts_tok_attr_get(&t, "name", ta_name, sizeof(ta_name));
                    inp->form_idx = doc->form_input_count;
                    if (doc->form_input_count < TS_MAX_FORM_INPUTS) {
                        struct ts_form_input *fi =
                            &doc->form_inputs[doc->form_input_count];
                        memset(fi, 0, sizeof(*fi));
                        strncpy(fi->name, ta_name, 63);
                        strncpy(fi->type, "textarea", 15);
                        fi->node_idx = doc->node_count - 1;
                        doc->form_input_count++;
                    }
                }
                in_script = 1; /* skip textarea content for now */
                continue;
            }

            /* ---- <select> ---- */
            if (ts_html__tag_name_eq(tn, tnl, "select")) {
                /* Render as text placeholder */
                struct ts_render_node *n = ts_doc__emit(doc, TS_NODE_TEXT);
                if (n) {
                    const char *lbl = "[select]";
                    n->style = cur_style;
                    n->text_offset = ts_doc__store_text(doc, lbl, 8);
                    n->text_len = 8;
                }
                in_script = 1; /* skip option content */
                continue;
            }

            /* ---- DEFAULT: unknown/unhandled tags ---- */
            /* Treat unknown tags as generic containers so their children
             * still render. Block-level unknowns get a line break before;
             * inline unknowns just push style and continue. This covers
             * web components (yt-*, my-*), HTML5 tags we don't explicitly
             * handle (figure, figcaption, details, summary, mark, time,
             * abbr, dl, dt, dd, noscript, address, dialog, etc.), and
             * any future/framework-specific elements. */
            {
                /* Custom elements (contain a hyphen) default to block */
                int is_block = ts_html_is_block(tn, tnl);
                if (!is_block) {
                    size_t hi;
                    for (hi = 0; hi < tnl; hi++) {
                        if (tn[hi] == '-') { is_block = 1; break; }
                    }
                }
                if (is_block) {
                    struct ts_render_node *br = ts_doc__emit(doc, TS_NODE_BREAK);
                    if (br) br->break_height = TS_LINE_SPACING;
                }
                ts_style_push(&style_stack, &cur_style, tn, tnl); ts_doc__push_ancestor_from_tok(&ancestors, &t);
                if (ts_doc__apply_element_css(doc, &t, &cur_style, &ancestors))
                    display_none_depth++;
                continue;
            }

        } else if (t.type == TS_TOK_TAG_CLOSE) {
            const char *tn = t.tag_name;
            size_t tnl = t.tag_name_len;

            if (ts_html__tag_name_eq(tn, tnl, "head")) {
                in_head = 0;
                continue;
            }
            if (ts_html__tag_name_eq(tn, tnl, "title")) {
                in_title = 0;
                continue;
            }
            if (ts_html__tag_name_eq(tn, tnl, "script")) {
                in_script = 0;
                continue;
            }
            if (ts_html__tag_name_eq(tn, tnl, "style")) {
                in_style = 0;
                continue;
            }

            /* Heading close: break + pop style */
            if (tnl == 2 && tn[0] == 'h' && tn[1] >= '1' && tn[1] <= '6') {
                struct ts_render_node *br = ts_doc__emit(doc, TS_NODE_BREAK);
                if (br) br->break_height = TS_LINE_SPACING;
                cur_style = ts_style_pop(&style_stack, tn, tnl); ts_css_ancestor_pop(&ancestors);
                continue;
            }

            /* </a> — close link */
            if (ts_html__tag_name_eq(tn, tnl, "a")) {
                if (cur_style.link_index >= 0 &&
                    cur_style.link_index < doc->link_count) {
                    doc->links[cur_style.link_index].node_end = doc->node_count - 1;
                }
                cur_style = ts_style_pop(&style_stack, tn, tnl); ts_css_ancestor_pop(&ancestors);
                continue;
            }

            /* </ul>, </ol> */
            if (ts_html__tag_name_eq(tn, tnl, "ul") ||
                ts_html__tag_name_eq(tn, tnl, "ol")) {
                struct ts_render_node *br = ts_doc__emit(doc, TS_NODE_BREAK);
                if (br) br->break_height = TS_LINE_SPACING;
                cur_style = ts_style_pop(&style_stack, tn, tnl); ts_css_ancestor_pop(&ancestors);
                if (ts_html__tag_name_eq(tn, tnl, "ol")) ordered_counter = -1;
                continue;
            }

            /* Block-level close tags: break + pop style + close box */
            if (ts_html__tag_name_eq(tn, tnl, "p") ||
                ts_html__tag_name_eq(tn, tnl, "div") ||
                ts_html__tag_name_eq(tn, tnl, "section") ||
                ts_html__tag_name_eq(tn, tnl, "article") ||
                ts_html__tag_name_eq(tn, tnl, "header") ||
                ts_html__tag_name_eq(tn, tnl, "footer") ||
                ts_html__tag_name_eq(tn, tnl, "main") ||
                ts_html__tag_name_eq(tn, tnl, "nav") ||
                ts_html__tag_name_eq(tn, tnl, "aside") ||
                ts_html__tag_name_eq(tn, tnl, "pre") ||
                ts_html__tag_name_eq(tn, tnl, "blockquote")) {
                /* Close current box */
                {
                    int was_ib = (cur_box >= 0 && cur_box < doc->box_count &&
                                  doc->boxes[cur_box].display_inline_block);
                    if (cur_box >= 0) {
                        ts_doc__close_box(doc, cur_box);
                        cur_box = (box_stack_depth > 0) ? box_stack[--box_stack_depth] : -1;
                    }
                    { struct ts_render_node *br = ts_doc__emit(doc, TS_NODE_BREAK);
                      if (br) br->break_height = was_ib ? 0 : TS_LINE_SPACING;
                    }
                }
                cur_style = ts_style_pop(&style_stack, tn, tnl); ts_css_ancestor_pop(&ancestors);
                if (display_none_depth > 0) display_none_depth--;
                continue;
            }

            /* Inline close: pop style */
            if (ts_html__tag_name_eq(tn, tnl, "b") ||
                ts_html__tag_name_eq(tn, tnl, "strong") ||
                ts_html__tag_name_eq(tn, tnl, "i") ||
                ts_html__tag_name_eq(tn, tnl, "em") ||
                ts_html__tag_name_eq(tn, tnl, "u") ||
                ts_html__tag_name_eq(tn, tnl, "s") ||
                ts_html__tag_name_eq(tn, tnl, "strike") ||
                ts_html__tag_name_eq(tn, tnl, "del") ||
                ts_html__tag_name_eq(tn, tnl, "code") ||
                ts_html__tag_name_eq(tn, tnl, "span")) {
                cur_style = ts_style_pop(&style_stack, tn, tnl); ts_css_ancestor_pop(&ancestors);
                if (display_none_depth > 0) display_none_depth--;
                continue;
            }

            /* Table close */
            if (ts_html__tag_name_eq(tn, tnl, "table") ||
                ts_html__tag_name_eq(tn, tnl, "tr")) {
                struct ts_render_node *br = ts_doc__emit(doc, TS_NODE_BREAK);
                if (br) br->break_height = TS_LINE_SPACING;
                continue;
            }

            /* DEFAULT: unknown/unhandled close tags — pop style, break if block */
            {
                int is_block = ts_html_is_block(tn, tnl);
                if (!is_block) {
                    size_t hi;
                    for (hi = 0; hi < tnl; hi++) {
                        if (tn[hi] == '-') { is_block = 1; break; }
                    }
                }
                if (is_block) {
                    struct ts_render_node *br = ts_doc__emit(doc, TS_NODE_BREAK);
                    if (br) br->break_height = TS_LINE_SPACING;
                }
                cur_style = ts_style_pop(&style_stack, tn, tnl); ts_css_ancestor_pop(&ancestors);
                if (display_none_depth > 0) display_none_depth--;
                continue;
            }

        } else if (t.type == TS_TOK_TEXT) {
            /* ---- Text content ---- */

            /* Title capture */
            if (in_title) {
                size_t clen = t.len;
                if (title_pos + (int)clen >= (int)sizeof(doc->title) - 1)
                    clen = sizeof(doc->title) - 1 - (size_t)title_pos;
                if (clen > 0) {
                    memcpy(doc->title + title_pos, t.start, clen);
                    title_pos += (int)clen;
                    doc->title[title_pos] = '\0';
                }
                continue;
            }

            /* Skip text inside <head> that isn't <title> */
            if (in_head) continue;

            /* Skip text inside <script> / <noscript> */
            if (in_script) continue;

            /* CSS content from <style> blocks — parse into stylesheet */
            if (in_style) {
                ts_css_parse(&doc->stylesheet, t.start, t.len);
                in_style = 0;
                continue;
            }

            /* Skip text inside display:none elements */
            if (display_none_depth > 0) continue;

            /* Decode entities and collapse whitespace */
            {
                char decoded[4096];
                size_t decoded_len = ts_decode_entities(t.start, t.len,
                                                         decoded, sizeof(decoded));
                if (decoded_len == 0) continue;

                if (!cur_style.preformatted) {
                    char collapsed[4096];
                    size_t clen = ts_doc__collapse_ws(decoded, decoded_len,
                                                       collapsed, sizeof(collapsed));
                    if (clen == 0) continue;
                    /* Skip text that is only whitespace */
                    if (clen == 1 && collapsed[0] == ' ') {
                        /* Still emit the space — needed for inline spacing */
                    }
                    /* Emit text node */
                    {
                        struct ts_render_node *n = ts_doc__emit(doc, TS_NODE_TEXT);
                        if (n) {
                            n->style = cur_style;
                            n->text_offset = ts_doc__store_text(doc, collapsed, clen);
                            n->text_len = (uint16_t)clen;
                        }
                    }
                } else {
                    /* Preformatted: preserve whitespace, emit as-is */
                    struct ts_render_node *n = ts_doc__emit(doc, TS_NODE_TEXT);
                    if (n) {
                        n->style = cur_style;
                        n->text_offset = ts_doc__store_text(doc, decoded, decoded_len);
                        n->text_len = (uint16_t)decoded_len;
                    }
                }
            }
        }
        /* Skip COMMENT, DOCTYPE, CDATA tokens */
    }
}

/* ================================================================== */
/* Build render tree from DOM (for JS-modified content)                */
/* ================================================================== */

/*
 * Helper: build a fake ts_token from a DOM node so we can reuse
 * ts_doc__apply_element_css(). Constructs a minimal tag string like
 * <div class="foo" id="bar" style="color:red"> in a static buffer.
 */
static void ts_doc__dom_node_to_fake_token(struct ts_dom_node *dn,
                                            char *buf, size_t buf_max,
                                            struct ts_token *out) {
    size_t pos = 0;
    int i;

    buf[pos++] = '<';
    /* Tag name */
    {
        const char *t = dn->tag;
        while (*t && pos < buf_max - 2) buf[pos++] = *t++;
    }
    /* Attributes */
    for (i = 0; i < dn->attr_count && pos < buf_max - 10; i++) {
        buf[pos++] = ' ';
        {
            const char *n = dn->attrs[i].name;
            while (*n && pos < buf_max - 5) buf[pos++] = *n++;
        }
        buf[pos++] = '=';
        buf[pos++] = '"';
        {
            const char *v = dn->attrs[i].value;
            while (*v && pos < buf_max - 3) {
                if (*v == '"') { buf[pos++] = '&'; buf[pos++] = 'q'; buf[pos++] = 'u'; buf[pos++] = 'o'; buf[pos++] = 't'; buf[pos++] = ';'; v++; }
                else buf[pos++] = *v++;
            }
        }
        buf[pos++] = '"';
    }
    buf[pos++] = '>';
    buf[pos] = '\0';

    out->type = TS_TOK_TAG_OPEN;
    out->start = buf;
    out->len = pos;
    out->tag_name = buf + 1; /* skip '<' */
    out->tag_name_len = strlen(dn->tag);
}

/* ================================================================== */
/* SVG <path> d-string helpers (file scope)                            */
/* ================================================================== */

#ifndef TS_SVG_PATH_MAX_SEGS
#define TS_SVG_PATH_MAX_SEGS 2048

struct ts_svg_seg { int x1, y1, x2, y2; };

static const char *ts_svg_skip(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',') p++;
    return p;
}

static double ts_svg_parse_num(const char **pp) {
    const char *p = *pp;
    double sign = 1.0, val = 0, frac = 0, div2 = 1;
    int has_dot = 0;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',') p++;
    if (*p == '-') { sign = -1.0; p++; } else if (*p == '+') { p++; }
    while ((*p >= '0' && *p <= '9') || (*p == '.' && !has_dot)) {
        if (*p == '.') { has_dot = 1; p++; continue; }
        if (has_dot) { frac = frac * 10 + (*p - '0'); div2 *= 10; }
        else { val = val * 10 + (*p - '0'); }
        p++;
    }
    *pp = p;
    return sign * (val + frac / div2);
}

static int ts_svg_add_seg(struct ts_svg_seg *segs, int count,
                           int x1, int y1, int x2, int y2) {
    if (count >= TS_SVG_PATH_MAX_SEGS) return count;
    segs[count].x1 = x1; segs[count].y1 = y1;
    segs[count].x2 = x2; segs[count].y2 = y2;
    return count + 1;
}

static int ts_svg_cubic(struct ts_svg_seg *segs, int count,
                         double x0, double y0, double cx1, double cy1,
                         double cx2, double cy2, double x3, double y3) {
    int steps = 16, si;
    double prev_x = x0, prev_y = y0;
    for (si = 1; si <= steps; si++) {
        double t = (double)si / steps, u = 1.0 - t;
        double bx = u*u*u*x0 + 3*u*u*t*cx1 + 3*u*t*t*cx2 + t*t*t*x3;
        double by = u*u*u*y0 + 3*u*u*t*cy1 + 3*u*t*t*cy2 + t*t*t*y3;
        count = ts_svg_add_seg(segs, count, (int)prev_x, (int)prev_y, (int)bx, (int)by);
        prev_x = bx; prev_y = by;
    }
    return count;
}

static int ts_svg_quad(struct ts_svg_seg *segs, int count,
                        double x0, double y0, double cx, double cy,
                        double x2, double y2) {
    int steps = 12, si;
    double prev_x = x0, prev_y = y0;
    for (si = 1; si <= steps; si++) {
        double t = (double)si / steps, u = 1.0 - t;
        double bx = u*u*x0 + 2*u*t*cx + t*t*x2;
        double by = u*u*y0 + 2*u*t*cy + t*t*y2;
        count = ts_svg_add_seg(segs, count, (int)prev_x, (int)prev_y, (int)bx, (int)by);
        prev_x = bx; prev_y = by;
    }
    return count;
}

static int ts_svg_parse_path(const char *d, struct ts_svg_seg *segs) {
    int count = 0;
    double cx = 0, cy = 0, sx = 0, sy = 0, lcx = 0, lcy = 0;
    char last_cmd = 0;
    const char *p = d;
    while (*p) {
        char cmd, uc;
        int is_cmd, rel;
        p = ts_svg_skip(p);
        if (!*p) break;
        cmd = *p;
        is_cmd = ((cmd >= 'A' && cmd <= 'Z') || (cmd >= 'a' && cmd <= 'z'));
        if (is_cmd) { p++; last_cmd = cmd; } else { cmd = last_cmd; }
        if (!cmd) break;
        rel = (cmd >= 'a' && cmd <= 'z');
        uc = rel ? (char)(cmd - 32) : cmd;
        switch (uc) {
        case 'M': { double mx = ts_svg_parse_num(&p), my = ts_svg_parse_num(&p);
            if (rel) { mx += cx; my += cy; } cx = mx; cy = my; sx = cx; sy = cy;
            last_cmd = rel ? 'l' : 'L'; break; }
        case 'L': { double lx = ts_svg_parse_num(&p), ly = ts_svg_parse_num(&p);
            if (rel) { lx += cx; ly += cy; }
            count = ts_svg_add_seg(segs, count, (int)cx, (int)cy, (int)lx, (int)ly);
            cx = lx; cy = ly; break; }
        case 'H': { double hx = ts_svg_parse_num(&p);
            if (rel) hx += cx;
            count = ts_svg_add_seg(segs, count, (int)cx, (int)cy, (int)hx, (int)cy);
            cx = hx; break; }
        case 'V': { double vy = ts_svg_parse_num(&p);
            if (rel) vy += cy;
            count = ts_svg_add_seg(segs, count, (int)cx, (int)cy, (int)cx, (int)vy);
            cy = vy; break; }
        case 'C': { double c1x = ts_svg_parse_num(&p), c1y = ts_svg_parse_num(&p);
            double c2x = ts_svg_parse_num(&p), c2y = ts_svg_parse_num(&p);
            double ex = ts_svg_parse_num(&p), ey = ts_svg_parse_num(&p);
            if (rel) { c1x+=cx; c1y+=cy; c2x+=cx; c2y+=cy; ex+=cx; ey+=cy; }
            count = ts_svg_cubic(segs, count, cx, cy, c1x, c1y, c2x, c2y, ex, ey);
            lcx = c2x; lcy = c2y; cx = ex; cy = ey; break; }
        case 'S': { double c2x = ts_svg_parse_num(&p), c2y = ts_svg_parse_num(&p);
            double ex = ts_svg_parse_num(&p), ey = ts_svg_parse_num(&p);
            if (rel) { c2x+=cx; c2y+=cy; ex+=cx; ey+=cy; }
            { double c1x = 2*cx - lcx, c1y = 2*cy - lcy;
              count = ts_svg_cubic(segs, count, cx, cy, c1x, c1y, c2x, c2y, ex, ey); }
            lcx = c2x; lcy = c2y; cx = ex; cy = ey; break; }
        case 'Q': { double qcx = ts_svg_parse_num(&p), qcy = ts_svg_parse_num(&p);
            double ex = ts_svg_parse_num(&p), ey = ts_svg_parse_num(&p);
            if (rel) { qcx+=cx; qcy+=cy; ex+=cx; ey+=cy; }
            count = ts_svg_quad(segs, count, cx, cy, qcx, qcy, ex, ey);
            lcx = qcx; lcy = qcy; cx = ex; cy = ey; break; }
        case 'T': { double ex = ts_svg_parse_num(&p), ey = ts_svg_parse_num(&p);
            if (rel) { ex+=cx; ey+=cy; }
            { double qcx = 2*cx - lcx, qcy = 2*cy - lcy;
              count = ts_svg_quad(segs, count, cx, cy, qcx, qcy, ex, ey);
              lcx = qcx; lcy = qcy; } cx = ex; cy = ey; break; }
        case 'A': { double arx = ts_svg_parse_num(&p), ary = ts_svg_parse_num(&p);
            double rot = ts_svg_parse_num(&p); (void)rot;
            double laf = ts_svg_parse_num(&p), sf = ts_svg_parse_num(&p);
            double ex = ts_svg_parse_num(&p), ey = ts_svg_parse_num(&p);
            if (rel) { ex+=cx; ey+=cy; }
            if (arx > 0 && ary > 0) {
                double dx2 = (cx-ex)/2, dy2 = (cy-ey)/2;
                double midx = (cx+ex)/2, midy = (cy+ey)/2;
                double sa = atan2(dy2/ary, dx2/arx), ea = atan2(-(dy2/ary), -(dx2/arx));
                if ((int)sf==0 && ea>sa) ea -= 6.28318;
                if ((int)sf==1 && ea<sa) ea += 6.28318;
                if ((int)laf) {
                    if ((int)sf==0 && ea > sa-3.14159) ea -= 6.28318;
                    if ((int)sf==1 && ea < sa+3.14159) ea += 6.28318;
                }
                { int ai; double astep = (ea-sa)/12.0, px2 = cx, py2 = cy;
                  for (ai = 1; ai <= 12; ai++) {
                      double a = sa + ai * astep;
                      double ax = midx + arx*cos(a), ay = midy + ary*sin(a);
                      count = ts_svg_add_seg(segs, count, (int)px2, (int)py2, (int)ax, (int)ay);
                      px2 = ax; py2 = ay;
                  }
                }
            } else { count = ts_svg_add_seg(segs, count, (int)cx, (int)cy, (int)ex, (int)ey); }
            cx = ex; cy = ey; break; }
        case 'Z':
            if ((int)cx != (int)sx || (int)cy != (int)sy)
                count = ts_svg_add_seg(segs, count, (int)cx, (int)cy, (int)sx, (int)sy);
            cx = sx; cy = sy; break;
        default: p++; break;
        }
    }
    return count;
}

static void ts_svg_stroke_path(uint32_t *pixels, int pw, int ph,
                                struct ts_svg_seg *segs, int seg_count, uint32_t color) {
    int si;
    for (si = 0; si < seg_count; si++) {
        int x1 = segs[si].x1, y1 = segs[si].y1, x2 = segs[si].x2, y2 = segs[si].y2;
        int ldx = x2-x1, ldy = y2-y1, lsx = ldx>0?1:-1, lsy = ldy>0?1:-1;
        int lerr, lx, ly;
        if (ldx<0) ldx=-ldx; if (ldy<0) ldy=-ldy;
        lerr = ldx-ldy; lx = x1; ly = y1;
        while (1) {
            if (lx>=0 && lx<pw && ly>=0 && ly<ph) pixels[ly*pw+lx] = color;
            if (lx==x2 && ly==y2) break;
            { int le2 = 2*lerr;
              if (le2 > -ldy) { lerr -= ldy; lx += lsx; }
              if (le2 < ldx) { lerr += ldx; ly += lsy; }
            }
        }
    }
}

static void ts_svg_fill_path(uint32_t *pixels, int pw, int ph,
                              struct ts_svg_seg *segs, int seg_count, uint32_t color) {
    int min_y = ph, max_y = 0, si, scanline;
    int x_crossings[256];
    for (si = 0; si < seg_count; si++) {
        if (segs[si].y1 < min_y) min_y = segs[si].y1;
        if (segs[si].y2 < min_y) min_y = segs[si].y2;
        if (segs[si].y1 > max_y) max_y = segs[si].y1;
        if (segs[si].y2 > max_y) max_y = segs[si].y2;
    }
    if (min_y < 0) min_y = 0;
    if (max_y >= ph) max_y = ph - 1;
    for (scanline = min_y; scanline <= max_y; scanline++) {
        int xc = 0, ci;
        for (si = 0; si < seg_count && xc < 255; si++) {
            int y1 = segs[si].y1, y2 = segs[si].y2;
            if ((y1 <= scanline && y2 > scanline) || (y2 <= scanline && y1 > scanline))
                x_crossings[xc++] = segs[si].x1 + (scanline - y1) * (segs[si].x2 - segs[si].x1) / (y2 - y1);
        }
        for (ci = 1; ci < xc; ci++) {
            int tmp = x_crossings[ci], cj = ci - 1;
            while (cj >= 0 && x_crossings[cj] > tmp) { x_crossings[cj+1] = x_crossings[cj]; cj--; }
            x_crossings[cj+1] = tmp;
        }
        for (ci = 0; ci + 1 < xc; ci += 2) {
            int fx1 = x_crossings[ci], fx2 = x_crossings[ci+1], fx;
            if (fx1 < 0) fx1 = 0; if (fx2 >= pw) fx2 = pw - 1;
            for (fx = fx1; fx <= fx2; fx++) pixels[scanline * pw + fx] = color;
        }
    }
}
#endif /* TS_SVG_PATH_MAX_SEGS */

/*
 * ts_doc__build_dom_recursive — recursively walk DOM tree emitting nodes.
 */
static void ts_doc__build_dom_recursive(struct ts_document *doc,
                                         struct ts_dom_ctx *dom,
                                         int node_id,
                                         struct ts_text_style *cur_style,
                                         struct ts_style_stack *style_stack,
                                         int *display_none_depth) {
    struct ts_dom_node *dn = ts_dom_get_node(dom, node_id);
    if (!dn) return;

    if (dn->type == TS_DOM_TEXT_NODE) {
        /* Emit text node */
        if (*display_none_depth > 0) return;
        if (dn->text_len <= 0) return;

        /* Static buffers — NOT reentrant, but DOM walk is single-threaded.
         * Saves 8KB per recursion frame (was causing stack overflow on deep pages). */
        static char decoded[4096];
        static char collapsed[4096];
        size_t decoded_len = (size_t)dn->text_len;
        if (decoded_len >= sizeof(decoded)) decoded_len = sizeof(decoded) - 1;
        memcpy(decoded, dn->text, decoded_len);
        decoded[decoded_len] = '\0';

        if (!cur_style->preformatted) {
            size_t clen = ts_doc__collapse_ws(decoded, decoded_len,
                                               collapsed, sizeof(collapsed));
            if (clen == 0) return;
            {
                struct ts_render_node *n = ts_doc__emit(doc, TS_NODE_TEXT);
                if (n) {
                    n->style = *cur_style;
                    n->text_offset = ts_doc__store_text(doc, collapsed, clen);
                    n->text_len = (uint16_t)clen;
                    n->dom_node_id = dn->parent; /* text node's parent element */
                }
            }
        } else {
            struct ts_render_node *n = ts_doc__emit(doc, TS_NODE_TEXT);
            if (n) {
                n->style = *cur_style;
                n->text_offset = ts_doc__store_text(doc, decoded, decoded_len);
                n->text_len = (uint16_t)decoded_len;
                n->dom_node_id = dn->parent;
            }
        }
        return;
    }

    if (dn->type == TS_DOM_COMMENT_NODE) return;

    if (dn->type != TS_DOM_ELEMENT_NODE &&
        dn->type != TS_DOM_DOCUMENT_NODE &&
        dn->type != TS_DOM_FRAGMENT_NODE) return;

    /* For document/fragment nodes, just recurse children */
    if (dn->type == TS_DOM_DOCUMENT_NODE || dn->type == TS_DOM_FRAGMENT_NODE) {
        int child = dn->first_child;
        while (child >= 0) {
            ts_doc__build_dom_recursive(doc, dom, child, cur_style,
                                         style_stack, display_none_depth);
            child = dom->nodes[child].next_sibling;
        }
        return;
    }

    /* Element node — apply tag-specific style + CSS, then recurse */
    {
        const char *tag = dn->tag;
        size_t tag_len = strlen(tag);
        static char fake_buf[2048]; /* static to save stack in deep recursion */
        struct ts_token fake_tok;
        int hidden = 0;

        /* Skip script/style elements */
        if (strcmp(tag, "script") == 0 || strcmp(tag, "style") == 0) return;
        /* Skip head element */
        if (strcmp(tag, "head") == 0) return;

        /* Build fake token for CSS matching */
        ts_doc__dom_node_to_fake_token(dn, fake_buf, sizeof(fake_buf), &fake_tok);

        /* ---- <html>, <body> — capture background ---- */
        if (strcmp(tag, "body") == 0 || strcmp(tag, "html") == 0) {
            ts_style_push(style_stack, cur_style, tag, tag_len);
            hidden = ts_doc__apply_element_css(doc, &fake_tok, cur_style, NULL);
            if (cur_style->bg_color != TS_COL_TRANSPARENT)
                doc->page_bg = cur_style->bg_color;
            /* Keep dark theme when no CSS background is specified */
            goto recurse_children;
        }

        /* ---- Headings ---- */
        if (tag_len == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6') {
            struct ts_render_node *br = ts_doc__emit(doc, TS_NODE_BREAK);
            if (br) br->break_height = TS_PARAGRAPH_SPACING;
            {
                int level = tag[1] - '0';
                struct ts_text_style heading = *cur_style;
                heading.fg_color = TS_COL_HEADING;
                heading.bold = 1;
                heading.font_scale = (level <= 1) ? 3 : (level <= 2) ? 2 : 1;
                ts_style_push(style_stack, cur_style, tag, tag_len);
                *cur_style = heading;
                hidden = ts_doc__apply_element_css(doc, &fake_tok, cur_style, NULL);
                if (hidden) (*display_none_depth)++;
            }
            goto recurse_children;
        }

        /* ---- Bold ---- */
        if (strcmp(tag, "b") == 0 || strcmp(tag, "strong") == 0) {
            ts_style_push(style_stack, cur_style, tag, tag_len);
            cur_style->bold = 1;
            ts_doc__apply_element_css(doc, &fake_tok, cur_style, NULL);
            goto recurse_children;
        }

        /* ---- Italic ---- */
        if (strcmp(tag, "i") == 0 || strcmp(tag, "em") == 0) {
            ts_style_push(style_stack, cur_style, tag, tag_len);
            cur_style->italic = 1;
            ts_doc__apply_element_css(doc, &fake_tok, cur_style, NULL);
            goto recurse_children;
        }

        /* ---- Underline ---- */
        if (strcmp(tag, "u") == 0) {
            ts_style_push(style_stack, cur_style, tag, tag_len);
            cur_style->underline = 1;
            ts_doc__apply_element_css(doc, &fake_tok, cur_style, NULL);
            goto recurse_children;
        }

        /* ---- Links ---- */
        if (strcmp(tag, "a") == 0) {
            const char *href = ts_dom_get_attr(dn, "href");
            ts_style_push(style_stack, cur_style, tag, tag_len);
            cur_style->fg_color = TS_COL_LINK;
            cur_style->underline = 1;
            if (href && doc->link_count < TS_MAX_LINKS) {
                struct ts_link *link = &doc->links[doc->link_count];
                memset(link, 0, sizeof(*link));
                strncpy(link->href, href, sizeof(link->href) - 1);
                link->node_start = doc->node_count;
                cur_style->link_index = doc->link_count;
                doc->link_count++;
            }
            ts_doc__apply_element_css(doc, &fake_tok, cur_style, NULL);
            goto recurse_children;
        }

        /* ---- Pre/code ---- */
        if (strcmp(tag, "pre") == 0) {
            struct ts_render_node *br = ts_doc__emit(doc, TS_NODE_BREAK);
            if (br) br->break_height = TS_LINE_SPACING;
            ts_style_push(style_stack, cur_style, tag, tag_len);
            cur_style->preformatted = 1;
            cur_style->monospace = 1;
            cur_style->bg_color = TS_COL_PRE_BG;
            ts_doc__apply_element_css(doc, &fake_tok, cur_style, NULL);
            goto recurse_children;
        }
        if (strcmp(tag, "code") == 0) {
            ts_style_push(style_stack, cur_style, tag, tag_len);
            cur_style->monospace = 1;
            cur_style->bg_color = TS_COL_CODE_BG;
            ts_doc__apply_element_css(doc, &fake_tok, cur_style, NULL);
            goto recurse_children;
        }

        /* ---- Block elements ---- */
        if (strcmp(tag, "p") == 0 || strcmp(tag, "div") == 0 ||
            strcmp(tag, "section") == 0 || strcmp(tag, "article") == 0 ||
            strcmp(tag, "header") == 0 || strcmp(tag, "footer") == 0 ||
            strcmp(tag, "main") == 0 || strcmp(tag, "nav") == 0 ||
            strcmp(tag, "aside") == 0) {
            struct ts_render_node *br = ts_doc__emit(doc, TS_NODE_BREAK);
            if (br) {
                br->break_height = (strcmp(tag, "p") == 0)
                    ? TS_PARAGRAPH_SPACING : TS_LINE_SPACING;
            }
            ts_style_push(style_stack, cur_style, tag, tag_len);
            hidden = ts_doc__apply_element_css(doc, &fake_tok, cur_style, NULL);
            if (hidden) (*display_none_depth)++;
            goto recurse_children;
        }

        /* ---- <br> ---- */
        if (strcmp(tag, "br") == 0) {
            struct ts_render_node *br = ts_doc__emit(doc, TS_NODE_BREAK);
            if (br) br->break_height = 0;
            return; /* void element, no children */
        }

        /* ---- <hr> ---- */
        if (strcmp(tag, "hr") == 0) {
            ts_doc__emit(doc, TS_NODE_BREAK);
            ts_doc__emit(doc, TS_NODE_HRULE);
            {
                struct ts_render_node *br2 = ts_doc__emit(doc, TS_NODE_BREAK);
                if (br2) br2->break_height = TS_LINE_SPACING;
            }
            return;
        }

        /* ---- Lists ---- */
        if (strcmp(tag, "ul") == 0 || strcmp(tag, "ol") == 0) {
            struct ts_render_node *br = ts_doc__emit(doc, TS_NODE_BREAK);
            if (br) br->break_height = TS_LINE_SPACING;
            ts_style_push(style_stack, cur_style, tag, tag_len);
            cur_style->list_depth++;
            goto recurse_children;
        }
        if (strcmp(tag, "li") == 0) {
            struct ts_render_node *br = ts_doc__emit(doc, TS_NODE_BREAK);
            if (br) br->break_height = 2;
            {
                struct ts_render_node *marker = ts_doc__emit(doc, TS_NODE_LIST_MARKER);
                if (marker) {
                    marker->style = *cur_style;
                    marker->style.fg_color = TS_COL_TEXT_DIM;
                    marker->text_offset = ts_doc__store_text(doc, "* ", 2);
                    marker->text_len = 2;
                }
            }
            goto recurse_children;
        }

        /* ---- SVG ---- */
        if (strcmp(tag, "svg") == 0) {
            struct ts_render_node *sn = ts_doc__emit(doc, TS_NODE_SVG);
            if (sn) {
                const char *vb = ts_dom_get_attr(dn, "viewbox");
                if (!vb) vb = ts_dom_get_attr(dn, "viewBox");
                const char *w_attr = ts_dom_get_attr(dn, "width");
                const char *h_attr = ts_dom_get_attr(dn, "height");
                int sw = w_attr ? ts_css_to_px(w_attr, 0) : 0;
                int sh = h_attr ? ts_css_to_px(h_attr, 0) : 0;
                /* Parse viewBox for dimensions if width/height not set */
                if ((sw <= 0 || sh <= 0) && vb) {
                    int vx = 0, vy = 0, vw = 0, vh = 0;
                    sscanf(vb, "%d %d %d %d", &vx, &vy, &vw, &vh);
                    (void)vx; (void)vy;
                    if (sw <= 0 && vw > 0) sw = vw;
                    if (sh <= 0 && vh > 0) sh = vh;
                }
                if (sw <= 0) sw = 100;
                if (sh <= 0) sh = 100;
                if (sw > 800) sw = 800;
                if (sh > 600) sh = 600;
                sn->style = *cur_style;
                sn->img_cache_idx = -1;
                sn->img_requested_w = sw;
                sn->img_requested_h = sh;
                sn->dom_node_id = node_id;

                /* Rasterize SVG children into a pixel buffer */
                if (doc->canvas_cache_count < 16) {
                    int ci = doc->canvas_cache_count;
                    uint32_t *pixels = (uint32_t *)calloc((size_t)(sw * sh), sizeof(uint32_t));
                    if (pixels) {
                        gfx_ctx_t svg_ctx;
                        gfx_init(&svg_ctx, pixels, (uint32_t)sw, (uint32_t)sh, (uint32_t)sw);

                        /* Walk SVG child elements and render shapes */
                        { int child = dn->first_child;
                          while (child >= 0) {
                              struct ts_dom_node *cn = ts_dom_get_node(dom, child);
                              if (cn && cn->type == TS_DOM_ELEMENT_NODE) {
                                  uint32_t fill = 0x000000, stroke = 0x000000;
                                  int stroke_w = 1, has_fill = 1, has_stroke = 0;
                                  { const char *f = ts_dom_get_attr(cn, "fill");
                                    if (f) { if (strcmp(f, "none") == 0) has_fill = 0; else fill = ts_css_color(f); }
                                  }
                                  { const char *s = ts_dom_get_attr(cn, "stroke");
                                    if (s) { if (strcmp(s, "none") != 0) { stroke = ts_css_color(s); has_stroke = 1; } }
                                  }
                                  { const char *sw2 = ts_dom_get_attr(cn, "stroke-width");
                                    if (sw2) stroke_w = ts_css_to_px(sw2, 0);
                                    if (stroke_w < 1) stroke_w = 1;
                                  }

                                  if (strcmp(cn->tag, "rect") == 0) {
                                      int rx = 0, ry = 0, rw = 0, rh = 0;
                                      { const char *v; v = ts_dom_get_attr(cn, "x"); if (v) rx = ts_css_to_px(v, 0);
                                        v = ts_dom_get_attr(cn, "y"); if (v) ry = ts_css_to_px(v, 0);
                                        v = ts_dom_get_attr(cn, "width"); if (v) rw = ts_css_to_px(v, 0);
                                        v = ts_dom_get_attr(cn, "height"); if (v) rh = ts_css_to_px(v, 0);
                                      }
                                      if (has_fill && rw > 0 && rh > 0)
                                          gfx_fill(&svg_ctx, (uint32_t)rx, (uint32_t)ry, (uint32_t)rw, (uint32_t)rh, fill);
                                      if (has_stroke && rw > 0 && rh > 0)
                                          gfx_rect(&svg_ctx, (uint32_t)rx, (uint32_t)ry, (uint32_t)rw, (uint32_t)rh, stroke);
                                  }
                                  else if (strcmp(cn->tag, "circle") == 0) {
                                      int ccx = 0, ccy = 0, cr = 0;
                                      { const char *v; v = ts_dom_get_attr(cn, "cx"); if (v) ccx = ts_css_to_px(v, 0);
                                        v = ts_dom_get_attr(cn, "cy"); if (v) ccy = ts_css_to_px(v, 0);
                                        v = ts_dom_get_attr(cn, "r"); if (v) cr = ts_css_to_px(v, 0);
                                      }
                                      if (cr > 0) {
                                          /* Midpoint circle fill + outline */
                                          { int cx2 = cr, cy2 = 0, err2 = 1 - cr;
                                            while (cx2 >= cy2) {
                                                if (has_fill) {
                                                    int fx;
                                                    for (fx = ccx - cx2; fx <= ccx + cx2; fx++)
                                                        if (fx >= 0 && fx < sw) {
                                                            if (ccy+cy2 >= 0 && ccy+cy2 < sh) pixels[(ccy+cy2)*sw+fx] = fill;
                                                            if (ccy-cy2 >= 0 && ccy-cy2 < sh) pixels[(ccy-cy2)*sw+fx] = fill;
                                                        }
                                                    for (fx = ccx - cy2; fx <= ccx + cy2; fx++)
                                                        if (fx >= 0 && fx < sw) {
                                                            if (ccy+cx2 >= 0 && ccy+cx2 < sh) pixels[(ccy+cx2)*sw+fx] = fill;
                                                            if (ccy-cx2 >= 0 && ccy-cx2 < sh) pixels[(ccy-cx2)*sw+fx] = fill;
                                                        }
                                                }
                                                if (has_stroke) {
                                                    int pts2[][2] = {{ccx+cx2,ccy+cy2},{ccx-cx2,ccy+cy2},{ccx+cx2,ccy-cy2},{ccx-cx2,ccy-cy2},
                                                                     {ccx+cy2,ccy+cx2},{ccx-cy2,ccy+cx2},{ccx+cy2,ccy-cx2},{ccx-cy2,ccy-cx2}};
                                                    int pi;
                                                    for (pi = 0; pi < 8; pi++) {
                                                        int px = pts2[pi][0], py = pts2[pi][1];
                                                        if (px >= 0 && px < sw && py >= 0 && py < sh)
                                                            pixels[py * sw + px] = stroke;
                                                    }
                                                }
                                                cy2++;
                                                if (err2 < 0) err2 += 2 * cy2 + 1;
                                                else { cx2--; err2 += 2 * (cy2 - cx2) + 1; }
                                            }
                                          }
                                      }
                                  }
                                  else if (strcmp(cn->tag, "line") == 0) {
                                      int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
                                      { const char *v;
                                        v = ts_dom_get_attr(cn, "x1"); if (v) x1 = ts_css_to_px(v, 0);
                                        v = ts_dom_get_attr(cn, "y1"); if (v) y1 = ts_css_to_px(v, 0);
                                        v = ts_dom_get_attr(cn, "x2"); if (v) x2 = ts_css_to_px(v, 0);
                                        v = ts_dom_get_attr(cn, "y2"); if (v) y2 = ts_css_to_px(v, 0);
                                      }
                                      { uint32_t lcol = has_stroke ? stroke : fill;
                                        /* Bresenham */
                                        int ldx = x2-x1, ldy = y2-y1;
                                        int lsx = ldx>0?1:-1, lsy = ldy>0?1:-1;
                                        if (ldx<0) ldx=-ldx; if (ldy<0) ldy=-ldy;
                                        { int lerr = ldx-ldy, lx = x1, ly = y1;
                                          while (1) {
                                              if (lx>=0 && lx<sw && ly>=0 && ly<sh) pixels[ly*sw+lx] = lcol;
                                              if (lx==x2 && ly==y2) break;
                                              { int le2 = 2*lerr;
                                                if (le2 > -ldy) { lerr -= ldy; lx += lsx; }
                                                if (le2 < ldx) { lerr += ldx; ly += lsy; }
                                              }
                                          }
                                        }
                                      }
                                  }
                                  else if (strcmp(cn->tag, "text") == 0) {
                                      int tx = 0, ty = 0;
                                      { const char *v; v = ts_dom_get_attr(cn, "x"); if (v) tx = ts_css_to_px(v, 0);
                                        v = ts_dom_get_attr(cn, "y"); if (v) ty = ts_css_to_px(v, 0);
                                      }
                                      { char tbuf[256]; int tlen;
                                        tlen = ts_dom_get_text_content(dom, child, tbuf, sizeof(tbuf));
                                        if (tlen > 0)
                                            gfx_draw_text(&svg_ctx, (uint32_t)tx, (uint32_t)(ty > 8 ? ty - 8 : 0),
                                                          tbuf, has_fill ? fill : 0xFFFFFF, TS_COL_TRANSPARENT);
                                      }
                                  }
                                  else if (strcmp(cn->tag, "ellipse") == 0) {
                                      int ecx = 0, ecy = 0, erx = 0, ery = 0;
                                      { const char *v;
                                        v = ts_dom_get_attr(cn, "cx"); if (v) ecx = ts_css_to_px(v, 0);
                                        v = ts_dom_get_attr(cn, "cy"); if (v) ecy = ts_css_to_px(v, 0);
                                        v = ts_dom_get_attr(cn, "rx"); if (v) erx = ts_css_to_px(v, 0);
                                        v = ts_dom_get_attr(cn, "ry"); if (v) ery = ts_css_to_px(v, 0);
                                      }
                                      if (erx > 0 && ery > 0 && has_fill) {
                                          int ey;
                                          for (ey = -ery; ey <= ery; ey++) {
                                              int ex_span = erx * (int)sqrt((double)(ery*ery - ey*ey)) / ery;
                                              int ex;
                                              for (ex = -ex_span; ex <= ex_span; ex++) {
                                                  int px = ecx + ex, py = ecy + ey;
                                                  if (px >= 0 && px < sw && py >= 0 && py < sh)
                                                      pixels[py * sw + px] = fill;
                                              }
                                          }
                                      }
                                  }
                                  else if (strcmp(cn->tag, "path") == 0) {
                                      const char *d_attr = ts_dom_get_attr(cn, "d");
                                      if (d_attr) {
                                          struct ts_svg_seg *path_segs = (struct ts_svg_seg *)malloc(
                                              TS_SVG_PATH_MAX_SEGS * sizeof(struct ts_svg_seg));
                                          if (path_segs) {
                                              int nseg = ts_svg_parse_path(d_attr, path_segs);
                                              if (nseg > 0) {
                                                  if (has_fill)
                                                      ts_svg_fill_path(pixels, sw, sh,
                                                          path_segs, nseg, fill);
                                                  if (has_stroke)
                                                      ts_svg_stroke_path(pixels, sw, sh,
                                                          path_segs, nseg, stroke);
                                                  else if (has_fill) /* default: stroke with fill color */
                                                      ts_svg_stroke_path(pixels, sw, sh,
                                                          path_segs, nseg, fill);
                                              }
                                              free(path_segs);
                                          }
                                      }
                                  }
                                  else if (strcmp(cn->tag, "polygon") == 0 ||
                                           strcmp(cn->tag, "polyline") == 0) {
                                      const char *pts = ts_dom_get_attr(cn, "points");
                                      if (pts) {
                                          /* Build path d-string from points */
                                          char dbuf[2048];
                                          int dpos = 0;
                                          int is_poly = (strcmp(cn->tag, "polygon") == 0);
                                          int first = 1;
                                          const char *pp = pts;
                                          while (*pp && dpos < 2040) {
                                              double px = ts_svg_parse_num(&pp);
                                              double py = ts_svg_parse_num(&pp);
                                              if (first) {
                                                  dpos += snprintf(dbuf + dpos, sizeof(dbuf) - (size_t)dpos,
                                                      "M%.0f %.0f", px, py);
                                                  first = 0;
                                              } else {
                                                  dpos += snprintf(dbuf + dpos, sizeof(dbuf) - (size_t)dpos,
                                                      "L%.0f %.0f", px, py);
                                              }
                                              pp = ts_svg_skip(pp);
                                          }
                                          if (is_poly && dpos < 2046) {
                                              dbuf[dpos++] = 'Z';
                                              dbuf[dpos] = '\0';
                                          }
                                          { struct ts_svg_seg *path_segs = (struct ts_svg_seg *)malloc(
                                                TS_SVG_PATH_MAX_SEGS * sizeof(struct ts_svg_seg));
                                            if (path_segs) {
                                                int nseg = ts_svg_parse_path(dbuf, path_segs);
                                                if (nseg > 0) {
                                                    if (has_fill && is_poly)
                                                        ts_svg_fill_path(pixels, sw, sh,
                                                            path_segs, nseg, fill);
                                                    if (has_stroke)
                                                        ts_svg_stroke_path(pixels, sw, sh,
                                                            path_segs, nseg, stroke);
                                                    else
                                                        ts_svg_stroke_path(pixels, sw, sh,
                                                            path_segs, nseg, has_fill ? fill : 0x000000);
                                                }
                                                free(path_segs);
                                            }
                                          }
                                      }
                                  }
                                  /* g (group) — recurse would need more infrastructure, skip for now */
                              }
                              child = cn ? cn->next_sibling : -1;
                          }
                        }

                        doc->canvas_cache[ci].pixels = pixels;
                        doc->canvas_cache[ci].w = sw;
                        doc->canvas_cache[ci].h = sh;
                        doc->canvas_cache[ci].used = 1;
                        sn->img_cache_idx = ci;
                        doc->canvas_cache_count++;
                    }
                }
            }
            return; /* don't recurse children — already processed */
        }

        /* ---- Canvas ---- */
        if (strcmp(tag, "canvas") == 0) {
            struct ts_render_node *cn = ts_doc__emit(doc, TS_NODE_CANVAS);
            if (cn) {
                const char *w_attr = ts_dom_get_attr(dn, "width");
                const char *h_attr = ts_dom_get_attr(dn, "height");
                cn->style = *cur_style;
                cn->img_cache_idx = -1; /* set by JS getContext() */
                cn->img_requested_w = w_attr ? ts_css_to_px(w_attr, 0) : 300;
                cn->img_requested_h = h_attr ? ts_css_to_px(h_attr, 0) : 150;
                cn->dom_node_id = node_id;
            }
            return;
        }

        /* ---- Iframes ---- */
        if (strcmp(tag, "iframe") == 0) {
            const char *src = ts_dom_get_attr(dn, "src");
            const char *w_attr = ts_dom_get_attr(dn, "width");
            const char *h_attr = ts_dom_get_attr(dn, "height");
            if (src && src[0] && doc->iframe_count < 8) {
                int idx = doc->iframe_count;
                strncpy(doc->iframe_src[idx], src, 511);
                doc->iframe_src[idx][511] = '\0';
                doc->iframe_w[idx] = w_attr ? ts_css_to_px(w_attr, 0) : 600;
                doc->iframe_h[idx] = h_attr ? ts_css_to_px(h_attr, 0) : 400;
                doc->iframe_count++;
            }
            /* Emit a placeholder box for the iframe */
            {
                struct ts_render_node *br = ts_doc__emit(doc, TS_NODE_BREAK);
                if (br) br->break_height = 4;
            }
            return; /* don't recurse children */
        }

        /* ---- Images ---- */
        if (strcmp(tag, "img") == 0) {
            struct ts_render_node *img = ts_doc__emit(doc, TS_NODE_IMAGE);
            if (img) {
                const char *w_attr = ts_dom_get_attr(dn, "width");
                const char *h_attr = ts_dom_get_attr(dn, "height");
                const char *alt = ts_dom_get_attr(dn, "alt");
                const char *src = ts_dom_get_attr(dn, "src");
                img->style = *cur_style;
                img->img_cache_idx = -1;
                if (w_attr) img->img_requested_w = ts_css_to_px(w_attr, 0);
                if (h_attr) img->img_requested_h = ts_css_to_px(h_attr, 0);
                if (src && doc->external_img_count < 64) {
                    strncpy(doc->external_img[doc->external_img_count], src, 511);
                    doc->external_img[doc->external_img_count][511] = '\0';
                    img->img_cache_idx = doc->external_img_count;
                    doc->external_img_count++;
                }
                if (alt) {
                    size_t alen = strlen(alt);
                    img->text_offset = ts_doc__store_text(doc, alt, alen);
                    img->text_len = (uint16_t)alen;
                }
            }
            return;
        }

        /* ---- <input> ---- */
        if (strcmp(tag, "input") == 0) {
            const char *inp_type_attr = ts_dom_get_attr(dn, "type");
            const char *inp_name_attr = ts_dom_get_attr(dn, "name");
            const char *inp_value_attr = ts_dom_get_attr(dn, "value");
            const char *inp_ph_attr = ts_dom_get_attr(dn, "placeholder");
            char inp_type[16] = "text";
            if (inp_type_attr) { strncpy(inp_type, inp_type_attr, 15); inp_type[15] = '\0'; }

            /* Hidden inputs — no rendering */
            if (strcmp(inp_type, "hidden") == 0) {
                if (doc->form_input_count < TS_MAX_FORM_INPUTS) {
                    struct ts_form_input *fi = &doc->form_inputs[doc->form_input_count];
                    memset(fi, 0, sizeof(*fi));
                    if (inp_name_attr) strncpy(fi->name, inp_name_attr, 63);
                    if (inp_value_attr) { strncpy(fi->value, inp_value_attr, 255); fi->value_len = (int)strlen(fi->value); }
                    strncpy(fi->type, "hidden", 15);
                    fi->node_idx = -1;
                    doc->form_input_count++;
                }
                return;
            }

            /* Submit button */
            if (strcmp(inp_type, "submit") == 0) {
                struct ts_render_node *btn = ts_doc__emit(doc, TS_NODE_BUTTON);
                if (btn) {
                    btn->style = *cur_style;
                    btn->form_idx = -1;
                    const char *label = (inp_value_attr && inp_value_attr[0]) ? inp_value_attr : "Submit";
                    size_t ll = strlen(label);
                    btn->text_offset = ts_doc__store_text(doc, label, ll);
                    btn->text_len = (uint16_t)ll;
                    btn->dom_node_id = node_id;
                }
                return;
            }

            /* Text/password/search/email/url input */
            {
                struct ts_render_node *inp = ts_doc__emit(doc, TS_NODE_INPUT);
                if (inp) {
                    inp->style = *cur_style;
                    inp->form_idx = doc->form_input_count;
                    inp->dom_node_id = node_id;
                    if (doc->form_input_count < TS_MAX_FORM_INPUTS) {
                        struct ts_form_input *fi = &doc->form_inputs[doc->form_input_count];
                        memset(fi, 0, sizeof(*fi));
                        if (inp_name_attr) strncpy(fi->name, inp_name_attr, 63);
                        if (inp_value_attr) { strncpy(fi->value, inp_value_attr, 255); fi->value_len = (int)strlen(fi->value); }
                        strncpy(fi->type, inp_type, 15);
                        if (inp_ph_attr) strncpy(fi->placeholder, inp_ph_attr, 127);
                        fi->node_idx = doc->node_count - 1;
                        doc->form_input_count++;
                    }
                }
            }
            return;
        }

        /* ---- <button> ---- */
        if (strcmp(tag, "button") == 0) {
            ts_style_push(style_stack, cur_style, tag, tag_len);
            ts_doc__apply_element_css(doc, &fake_tok, cur_style, NULL);
            goto recurse_children;
        }

        /* ---- <textarea> ---- */
        if (strcmp(tag, "textarea") == 0) {
            struct ts_render_node *inp = ts_doc__emit(doc, TS_NODE_INPUT);
            if (inp) {
                const char *ta_name = ts_dom_get_attr(dn, "name");
                inp->style = *cur_style;
                inp->form_idx = doc->form_input_count;
                inp->dom_node_id = node_id;
                if (doc->form_input_count < TS_MAX_FORM_INPUTS) {
                    struct ts_form_input *fi = &doc->form_inputs[doc->form_input_count];
                    memset(fi, 0, sizeof(*fi));
                    if (ta_name) strncpy(fi->name, ta_name, 63);
                    strncpy(fi->type, "textarea", 15);
                    fi->node_idx = doc->node_count - 1;
                    doc->form_input_count++;
                }
            }
            return;
        }

        /* ---- DEFAULT: unknown/unhandled element ---- */
        {
            int is_block = ts_html_is_block(tag, tag_len);
            if (!is_block) {
                size_t hi;
                for (hi = 0; hi < tag_len; hi++) {
                    if (tag[hi] == '-') { is_block = 1; break; }
                }
            }
            if (is_block) {
                struct ts_render_node *br = ts_doc__emit(doc, TS_NODE_BREAK);
                if (br) br->break_height = TS_LINE_SPACING;
            }
            ts_style_push(style_stack, cur_style, tag, tag_len);
            hidden = ts_doc__apply_element_css(doc, &fake_tok, cur_style, NULL);
            if (hidden) (*display_none_depth)++;
            goto recurse_children;
        }

    recurse_children:
        /* Mark render nodes emitted for this element with dom_node_id */
        {
            int node_start_idx = doc->node_count;
            /* If element has a shadow root, render shadow tree instead of
             * light DOM children. This is what makes web components visible:
             * YouTube's <ytd-*> elements store their content in shadow DOM. */
            {
                int shadow_root = ts_shadow_get_root(dom, node_id);
                if (shadow_root >= 0) {
                    /* Walk shadow root's children */
                    int child = dom->nodes[shadow_root].first_child;
                    while (child >= 0) {
                        ts_doc__build_dom_recursive(doc, dom, child, cur_style,
                                                     style_stack, display_none_depth);
                        child = dom->nodes[child].next_sibling;
                    }
                }
            }
            /* Always walk light DOM children too (they render if no shadow,
             * and may be slotted into shadow DOM via <slot> elements) */
            {
                int child = dn->first_child;
                while (child >= 0) {
                    ts_doc__build_dom_recursive(doc, dom, child, cur_style,
                                                 style_stack, display_none_depth);
                    child = dom->nodes[child].next_sibling;
                }
            }
            /* Tag all nodes emitted during this element (that don't already
               have a more specific dom_node_id from a child element) */
            {
                int ni;
                for (ni = node_start_idx; ni < doc->node_count; ni++) {
                    if (doc->nodes[ni].dom_node_id < 0)
                        doc->nodes[ni].dom_node_id = node_id;
                }
            }
        }

        /* Pop style on close */
        {
            /* Close link */
            if (strcmp(tag, "a") == 0 && cur_style->link_index >= 0 &&
                cur_style->link_index < doc->link_count) {
                doc->links[cur_style->link_index].node_end = doc->node_count - 1;
            }

            int was_block = ts_html_is_block(tag, tag_len);
            if (!was_block) {
                size_t hi;
                for (hi = 0; hi < tag_len; hi++) {
                    if (tag[hi] == '-') { was_block = 1; break; }
                }
            }

            /* Heading close: emit trailing break */
            if (tag_len == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6') {
                struct ts_render_node *br = ts_doc__emit(doc, TS_NODE_BREAK);
                if (br) br->break_height = TS_LINE_SPACING;
            }
            /* List close */
            else if (strcmp(tag, "ul") == 0 || strcmp(tag, "ol") == 0) {
                struct ts_render_node *br = ts_doc__emit(doc, TS_NODE_BREAK);
                if (br) br->break_height = TS_LINE_SPACING;
            }
            /* Generic block close */
            else if (was_block) {
                struct ts_render_node *br = ts_doc__emit(doc, TS_NODE_BREAK);
                if (br) br->break_height = TS_LINE_SPACING;
            }

            *cur_style = ts_style_pop(style_stack, tag, tag_len);
            if (*display_none_depth > 0) (*display_none_depth)--;
        }
    }
}

/* SVG path helpers are defined earlier in this file (before ts_doc__build_dom_recursive) */
#if 0 /* DUPLICATE REMOVED — start */
static const char *ts_svg_skip_REMOVED(const char *p) {
    (void)p; return p;
}

static double ts_svg_parse_num(const char **pp) {
    const char *p = *pp;
    double sign = 1.0, val = 0, frac = 0, div2 = 1;
    int has_dot = 0;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',') p++;
    if (*p == '-') { sign = -1.0; p++; } else if (*p == '+') { p++; }
    while ((*p >= '0' && *p <= '9') || (*p == '.' && !has_dot)) {
        if (*p == '.') { has_dot = 1; p++; continue; }
        if (has_dot) { frac = frac * 10 + (*p - '0'); div2 *= 10; }
        else { val = val * 10 + (*p - '0'); }
        p++;
    }
    *pp = p;
    return sign * (val + frac / div2);
}

static int ts_svg_add_seg(struct ts_svg_seg *segs, int count,
                           int x1, int y1, int x2, int y2) {
    if (count >= TS_SVG_PATH_MAX_SEGS) return count;
    segs[count].x1 = x1; segs[count].y1 = y1;
    segs[count].x2 = x2; segs[count].y2 = y2;
    return count + 1;
}

static int ts_svg_cubic(struct ts_svg_seg *segs, int count,
                         double x0, double y0, double cx1, double cy1,
                         double cx2, double cy2, double x3, double y3) {
    int steps = 16, si;
    double prev_x = x0, prev_y = y0;
    for (si = 1; si <= steps; si++) {
        double t = (double)si / steps, u = 1.0 - t;
        double bx = u*u*u*x0 + 3*u*u*t*cx1 + 3*u*t*t*cx2 + t*t*t*x3;
        double by = u*u*u*y0 + 3*u*u*t*cy1 + 3*u*t*t*cy2 + t*t*t*y3;
        count = ts_svg_add_seg(segs, count, (int)prev_x, (int)prev_y, (int)bx, (int)by);
        prev_x = bx; prev_y = by;
    }
    return count;
}

static int ts_svg_quad(struct ts_svg_seg *segs, int count,
                        double x0, double y0, double cx, double cy,
                        double x2, double y2) {
    int steps = 12, si;
    double prev_x = x0, prev_y = y0;
    for (si = 1; si <= steps; si++) {
        double t = (double)si / steps, u = 1.0 - t;
        double bx = u*u*x0 + 2*u*t*cx + t*t*x2;
        double by = u*u*y0 + 2*u*t*cy + t*t*y2;
        count = ts_svg_add_seg(segs, count, (int)prev_x, (int)prev_y, (int)bx, (int)by);
        prev_x = bx; prev_y = by;
    }
    return count;
}

static int ts_svg_parse_path(const char *d, struct ts_svg_seg *segs) {
    int count = 0;
    double cx = 0, cy = 0, sx = 0, sy = 0, lcx = 0, lcy = 0;
    char last_cmd = 0;
    const char *p = d;
    while (*p) {
        char cmd, uc;
        int is_cmd, rel;
        p = ts_svg_skip(p);
        if (!*p) break;
        cmd = *p;
        is_cmd = ((cmd >= 'A' && cmd <= 'Z') || (cmd >= 'a' && cmd <= 'z'));
        if (is_cmd) { p++; last_cmd = cmd; } else { cmd = last_cmd; }
        if (!cmd) break;
        rel = (cmd >= 'a' && cmd <= 'z');
        uc = rel ? (char)(cmd - 32) : cmd;
        switch (uc) {
        case 'M': { double mx = ts_svg_parse_num(&p), my = ts_svg_parse_num(&p);
            if (rel) { mx += cx; my += cy; } cx = mx; cy = my; sx = cx; sy = cy;
            last_cmd = rel ? 'l' : 'L'; break; }
        case 'L': { double lx = ts_svg_parse_num(&p), ly = ts_svg_parse_num(&p);
            if (rel) { lx += cx; ly += cy; }
            count = ts_svg_add_seg(segs, count, (int)cx, (int)cy, (int)lx, (int)ly);
            cx = lx; cy = ly; break; }
        case 'H': { double hx = ts_svg_parse_num(&p);
            if (rel) hx += cx;
            count = ts_svg_add_seg(segs, count, (int)cx, (int)cy, (int)hx, (int)cy);
            cx = hx; break; }
        case 'V': { double vy = ts_svg_parse_num(&p);
            if (rel) vy += cy;
            count = ts_svg_add_seg(segs, count, (int)cx, (int)cy, (int)cx, (int)vy);
            cy = vy; break; }
        case 'C': { double c1x = ts_svg_parse_num(&p), c1y = ts_svg_parse_num(&p);
            double c2x = ts_svg_parse_num(&p), c2y = ts_svg_parse_num(&p);
            double ex = ts_svg_parse_num(&p), ey = ts_svg_parse_num(&p);
            if (rel) { c1x+=cx; c1y+=cy; c2x+=cx; c2y+=cy; ex+=cx; ey+=cy; }
            count = ts_svg_cubic(segs, count, cx, cy, c1x, c1y, c2x, c2y, ex, ey);
            lcx = c2x; lcy = c2y; cx = ex; cy = ey; break; }
        case 'S': { double c2x = ts_svg_parse_num(&p), c2y = ts_svg_parse_num(&p);
            double ex = ts_svg_parse_num(&p), ey = ts_svg_parse_num(&p);
            if (rel) { c2x+=cx; c2y+=cy; ex+=cx; ey+=cy; }
            { double c1x = 2*cx - lcx, c1y = 2*cy - lcy;
              count = ts_svg_cubic(segs, count, cx, cy, c1x, c1y, c2x, c2y, ex, ey); }
            lcx = c2x; lcy = c2y; cx = ex; cy = ey; break; }
        case 'Q': { double qcx = ts_svg_parse_num(&p), qcy = ts_svg_parse_num(&p);
            double ex = ts_svg_parse_num(&p), ey = ts_svg_parse_num(&p);
            if (rel) { qcx+=cx; qcy+=cy; ex+=cx; ey+=cy; }
            count = ts_svg_quad(segs, count, cx, cy, qcx, qcy, ex, ey);
            lcx = qcx; lcy = qcy; cx = ex; cy = ey; break; }
        case 'T': { double ex = ts_svg_parse_num(&p), ey = ts_svg_parse_num(&p);
            if (rel) { ex+=cx; ey+=cy; }
            { double qcx = 2*cx - lcx, qcy = 2*cy - lcy;
              count = ts_svg_quad(segs, count, cx, cy, qcx, qcy, ex, ey);
              lcx = qcx; lcy = qcy; } cx = ex; cy = ey; break; }
        case 'A': { double arx = ts_svg_parse_num(&p), ary = ts_svg_parse_num(&p);
            double rot = ts_svg_parse_num(&p); (void)rot;
            double laf = ts_svg_parse_num(&p), sf = ts_svg_parse_num(&p);
            double ex = ts_svg_parse_num(&p), ey = ts_svg_parse_num(&p);
            if (rel) { ex+=cx; ey+=cy; }
            if (arx > 0 && ary > 0) {
                double dx2 = (cx-ex)/2, dy2 = (cy-ey)/2;
                double midx = (cx+ex)/2, midy = (cy+ey)/2;
                double sa = atan2(dy2/ary, dx2/arx), ea = atan2(-(dy2/ary), -(dx2/arx));
                if ((int)sf==0 && ea>sa) ea -= 6.28318;
                if ((int)sf==1 && ea<sa) ea += 6.28318;
                if ((int)laf) {
                    if ((int)sf==0 && ea > sa-3.14159) ea -= 6.28318;
                    if ((int)sf==1 && ea < sa+3.14159) ea += 6.28318;
                }
                { int ai; double astep = (ea-sa)/12.0, px2 = cx, py2 = cy;
                  for (ai = 1; ai <= 12; ai++) {
                      double a = sa + ai * astep;
                      double ax = midx + arx*cos(a), ay = midy + ary*sin(a);
                      count = ts_svg_add_seg(segs, count, (int)px2, (int)py2, (int)ax, (int)ay);
                      px2 = ax; py2 = ay;
                  }
                }
            } else { count = ts_svg_add_seg(segs, count, (int)cx, (int)cy, (int)ex, (int)ey); }
            cx = ex; cy = ey; break; }
        case 'Z':
            if ((int)cx != (int)sx || (int)cy != (int)sy)
                count = ts_svg_add_seg(segs, count, (int)cx, (int)cy, (int)sx, (int)sy);
            cx = sx; cy = sy; break;
        default: p++; break;
        }
    }
    return count;
}

static void ts_svg_stroke_path(uint32_t *pixels, int pw, int ph,
                                struct ts_svg_seg *segs, int seg_count, uint32_t color) {
    int si;
    for (si = 0; si < seg_count; si++) {
        int x1 = segs[si].x1, y1 = segs[si].y1, x2 = segs[si].x2, y2 = segs[si].y2;
        int ldx = x2-x1, ldy = y2-y1, lsx = ldx>0?1:-1, lsy = ldy>0?1:-1;
        int lerr, lx, ly;
        if (ldx<0) ldx=-ldx; if (ldy<0) ldy=-ldy;
        lerr = ldx-ldy; lx = x1; ly = y1;
        while (1) {
            if (lx>=0 && lx<pw && ly>=0 && ly<ph) pixels[ly*pw+lx] = color;
            if (lx==x2 && ly==y2) break;
            { int le2 = 2*lerr;
              if (le2 > -ldy) { lerr -= ldy; lx += lsx; }
              if (le2 < ldx) { lerr += ldx; ly += lsy; }
            }
        }
    }
}

static void ts_svg_fill_path(uint32_t *pixels, int pw, int ph,
                              struct ts_svg_seg *segs, int seg_count, uint32_t color) {
    int min_y = ph, max_y = 0, si, scanline;
    int x_crossings[256];
    for (si = 0; si < seg_count; si++) {
        if (segs[si].y1 < min_y) min_y = segs[si].y1;
        if (segs[si].y2 < min_y) min_y = segs[si].y2;
        if (segs[si].y1 > max_y) max_y = segs[si].y1;
        if (segs[si].y2 > max_y) max_y = segs[si].y2;
    }
    if (min_y < 0) min_y = 0;
    if (max_y >= ph) max_y = ph - 1;
    for (scanline = min_y; scanline <= max_y; scanline++) {
        int xc = 0, ci;
        for (si = 0; si < seg_count && xc < 255; si++) {
            int y1 = segs[si].y1, y2 = segs[si].y2;
            if ((y1 <= scanline && y2 > scanline) || (y2 <= scanline && y1 > scanline))
                x_crossings[xc++] = segs[si].x1 + (scanline - y1) * (segs[si].x2 - segs[si].x1) / (y2 - y1);
        }
        for (ci = 1; ci < xc; ci++) {
            int tmp = x_crossings[ci], cj = ci - 1;
            while (cj >= 0 && x_crossings[cj] > tmp) { x_crossings[cj+1] = x_crossings[cj]; cj--; }
            x_crossings[cj+1] = tmp;
        }
        for (ci = 0; ci + 1 < xc; ci += 2) {
            int fx1 = x_crossings[ci], fx2 = x_crossings[ci+1], fx;
            if (fx1 < 0) fx1 = 0; if (fx2 >= pw) fx2 = pw - 1;
            for (fx = fx1; fx <= fx2; fx++) pixels[scanline * pw + fx] = color;
        }
    }
}
#endif /* end DUPLICATE REMOVED */

/*
 * ts_doc_build_from_dom — rebuild the render tree from a (possibly
 * JS-modified) DOM tree. Called when g_dom->dirty is set.
 *
 * This is the bridge that makes JavaScript-created content visible.
 */
static void ts_doc_build_from_dom(struct ts_document *doc,
                                   struct ts_dom_ctx *dom) {
    struct ts_style_stack style_stack;
    struct ts_text_style cur_style;
    int display_none_depth = 0;

    memset(&style_stack, 0, sizeof(style_stack));
    memset(&cur_style, 0, sizeof(cur_style));
    cur_style.fg_color = TS_COL_TEXT;
    cur_style.bg_color = TS_COL_TRANSPARENT;
    cur_style.font_scale = 1;
    cur_style.link_index = -1;

    if (dom->document_node < 0) return;

    ts_doc__build_dom_recursive(doc, dom, dom->document_node,
                                 &cur_style, &style_stack,
                                 &display_none_depth);
}

/* ================================================================== */
/* Flexbox layout                                                      */
/* ================================================================== */

/*
 * ts_doc__layout_flex_children — compute positions of direct child boxes
 * within a flex container. Called during ts_doc_layout when a flex
 * container's position has been established.
 *
 * For flex-direction:row, computes x-positions and widths.
 * For flex-direction:column, sets proper widths (y handled by flow).
 */
static void ts_doc__layout_flex_children(struct ts_document *doc,
                                          int container_idx) {
    struct ts_block_box *container = &doc->boxes[container_idx];
    int children[64];
    int child_count = 0;
    int j;

    /* Collect direct child boxes */
    for (j = 0; j < doc->box_count; j++) {
        if (doc->boxes[j].parent_box == container_idx) {
            if (child_count < 64) children[child_count++] = j;
        }
    }
    if (child_count == 0) return;

    if (container->flex_direction == 0) {
        /* === flex-direction: row === */
        int avail = container->content_w;
        int main_sizes[64];
        int total_basis = 0;
        int total_grow = 0;
        int remaining, total_after;
        int offset = 0, gap_extra = 0, pos;

        /* Step 1: compute initial main axis sizes (outer box size) */
        for (j = 0; j < child_count; j++) {
            struct ts_block_box *ch = &doc->boxes[children[j]];
            int outer;
            if (ch->flex_basis > 0) {
                outer = ch->flex_basis;
            } else if (ch->explicit_width > 0) {
                outer = ch->explicit_width
                    + ch->margin[1] + ch->margin[3]
                    + ch->border_w[1] + ch->border_w[3]
                    + ch->padding[1] + ch->padding[3];
            } else {
                /* Auto: equal share minus gaps */
                int total_gap = container->gap * (child_count - 1);
                outer = (avail - total_gap) / child_count;
                if (outer < 20) outer = 20;
            }
            main_sizes[j] = outer;
            total_basis += outer;
            total_grow += ch->flex_grow;
        }
        total_basis += container->gap * (child_count - 1);

        /* Step 2: distribute remaining space (grow/shrink) */
        remaining = avail - total_basis;
        if (remaining > 0 && total_grow > 0) {
            for (j = 0; j < child_count; j++) {
                struct ts_block_box *ch = &doc->boxes[children[j]];
                main_sizes[j] += remaining * ch->flex_grow / total_grow;
            }
        } else if (remaining < 0) {
            int total_shrink = 0;
            for (j = 0; j < child_count; j++) {
                total_shrink += doc->boxes[children[j]].flex_shrink
                                * main_sizes[j];
            }
            if (total_shrink > 0) {
                int deficit = -remaining;
                for (j = 0; j < child_count; j++) {
                    struct ts_block_box *ch = &doc->boxes[children[j]];
                    int shrink = deficit * (ch->flex_shrink * main_sizes[j])
                                 / total_shrink;
                    main_sizes[j] -= shrink;
                    if (main_sizes[j] < 20) main_sizes[j] = 20;
                }
            }
        }

        /* Recompute remaining after flex adjustments */
        total_after = 0;
        for (j = 0; j < child_count; j++) total_after += main_sizes[j];
        total_after += container->gap * (child_count - 1);
        remaining = avail - total_after;
        if (remaining < 0) remaining = 0;

        /* Step 3: justify-content — start offset and extra gap */
        switch (container->justify_content) {
        case 0: /* flex-start */
            break;
        case 1: /* center */
            offset = remaining / 2;
            break;
        case 2: /* flex-end */
            offset = remaining;
            break;
        case 3: /* space-between */
            if (child_count > 1)
                gap_extra = remaining / (child_count - 1);
            break;
        case 4: /* space-around */
            if (child_count > 0) {
                int s = remaining / child_count;
                offset = s / 2;
                gap_extra = s;
            }
            break;
        }

        /* Step 4: position children along main axis */
        pos = offset;
        for (j = 0; j < child_count; j++) {
            struct ts_block_box *ch = &doc->boxes[children[j]];
            int outer_w = main_sizes[j];

            ch->x = container->content_x + pos + ch->margin[3];
            ch->y = container->content_y + ch->margin[0];
            ch->content_x = ch->x + ch->border_w[3] + ch->padding[3];
            ch->content_y = ch->y + ch->border_w[0] + ch->padding[0];
            ch->content_w = outer_w
                - ch->margin[1] - ch->margin[3]
                - ch->border_w[1] - ch->border_w[3]
                - ch->padding[1] - ch->padding[3];
            if (ch->content_w < 10) ch->content_w = 10;

            ch->flex_positioned = 1;
            pos += outer_w + container->gap + gap_extra;
        }
    } else {
        /* === flex-direction: column === */
        /* Children stack vertically (close to default flow) with proper widths */
        for (j = 0; j < child_count; j++) {
            struct ts_block_box *ch = &doc->boxes[children[j]];
            ch->content_w = container->content_w
                - ch->margin[1] - ch->margin[3]
                - ch->border_w[1] - ch->border_w[3]
                - ch->padding[1] - ch->padding[3];
            if (ch->content_w < 10) ch->content_w = 10;
            /* y-positioning handled by normal flow */
        }
    }
}

/* ================================================================== */
/* CSS Grid layout                                                     */
/* ================================================================== */

/*
 * ts_doc__layout_grid_children — compute positions of direct child boxes
 * within a CSS Grid container. Resolves track sizes (px and fr units),
 * places items explicitly or via auto-placement, then sets child positions.
 */
static void ts_doc__layout_grid_children(struct ts_document *doc,
                                          int container_idx) {
    struct ts_block_box *container = &doc->boxes[container_idx];
    int children[64];
    int child_count = 0;
    int col_sizes[8], row_heights[8];
    int col_pos[9], row_pos[9];
    int grid[8][8]; /* grid[row][col] = child index, -1 = empty */
    int j, r, c;
    int avail_w = container->content_w;
    int total_fixed = 0, total_fr = 0;
    int total_gap_w, remaining;

    /* Collect direct child boxes */
    for (j = 0; j < doc->box_count; j++) {
        if (doc->boxes[j].parent_box == container_idx) {
            if (child_count < 64) children[child_count++] = j;
        }
    }
    if (child_count == 0) return;

    /* Default columns: if none defined, create equal fr columns for children */
    if (container->grid_col_count == 0) {
        container->grid_col_count = child_count < 8 ? child_count : 8;
        for (j = 0; j < container->grid_col_count; j++)
            container->grid_col_track[j] = -100; /* 1fr each */
    }
    /* Default rows: enough to fit all children */
    if (container->grid_row_count == 0) {
        int needed = (child_count + container->grid_col_count - 1)
                     / container->grid_col_count;
        container->grid_row_count = needed < 8 ? needed : 8;
        for (j = 0; j < container->grid_row_count; j++)
            container->grid_row_track[j] = 0; /* auto */
    }

    /* Resolve column widths: sum fixed px and fr */
    for (j = 0; j < container->grid_col_count && j < 8; j++) {
        int t = container->grid_col_track[j];
        if (t > 0) { col_sizes[j] = t; total_fixed += t; }
        else if (t < 0) { col_sizes[j] = 0; total_fr += (-t); }
        else { col_sizes[j] = 0; total_fr += 100; } /* auto = 1fr */
    }
    total_gap_w = container->gap * (container->grid_col_count - 1);
    remaining = avail_w - total_fixed - total_gap_w;
    if (remaining < 0) remaining = 0;
    if (total_fr > 0) {
        for (j = 0; j < container->grid_col_count && j < 8; j++) {
            int t = container->grid_col_track[j];
            if (t <= 0) {
                int fr = (t < 0) ? (-t) : 100;
                col_sizes[j] = remaining * fr / total_fr;
            }
        }
    }

    /* Column positions (cumulative) */
    col_pos[0] = 0;
    for (j = 0; j < container->grid_col_count; j++)
        col_pos[j + 1] = col_pos[j] + col_sizes[j] + container->gap;

    /* Row heights: fixed if specified, else auto */
    for (j = 0; j < container->grid_row_count && j < 8; j++) {
        int t = container->grid_row_track[j];
        if (t > 0) row_heights[j] = t;
        else row_heights[j] = TS_FONT_H + 8; /* auto: minimum height */
    }
    /* Row positions */
    row_pos[0] = 0;
    for (j = 0; j < container->grid_row_count; j++)
        row_pos[j + 1] = row_pos[j] + row_heights[j] + container->gap;

    /* Initialize grid cells to empty */
    for (r = 0; r < 8; r++)
        for (c = 0; c < 8; c++)
            grid[r][c] = -1;

    /* Place explicitly positioned items first */
    for (j = 0; j < child_count; j++) {
        struct ts_block_box *ch = &doc->boxes[children[j]];
        if (ch->grid_col_start > 0 && ch->grid_row_start > 0) {
            int gc = ch->grid_col_start - 1;
            int gr = ch->grid_row_start - 1;
            if (gc < 8 && gr < 8) grid[gr][gc] = j;
        }
    }

    /* Auto-place remaining items into first available cell */
    {
        int ar = 0, ac = 0;
        for (j = 0; j < child_count; j++) {
            struct ts_block_box *ch = &doc->boxes[children[j]];
            if (ch->grid_col_start > 0 && ch->grid_row_start > 0) continue;
            while (ar < container->grid_row_count && ar < 8) {
                while (ac < container->grid_col_count && ac < 8) {
                    if (grid[ar][ac] < 0) {
                        grid[ar][ac] = j;
                        ac++;
                        goto grid_placed;
                    }
                    ac++;
                }
                ac = 0;
                ar++;
            }
            break; /* no room */
        grid_placed:;
        }
    }

    /* Set child positions from grid cells */
    for (r = 0; r < container->grid_row_count && r < 8; r++) {
        for (c = 0; c < container->grid_col_count && c < 8; c++) {
            int idx = grid[r][c];
            struct ts_block_box *ch;
            if (idx < 0 || idx >= child_count) continue;
            ch = &doc->boxes[children[idx]];
            ch->x = container->content_x + col_pos[c] + ch->margin[3];
            ch->y = container->content_y + row_pos[r] + ch->margin[0];
            ch->content_x = ch->x + ch->border_w[3] + ch->padding[3];
            ch->content_y = ch->y + ch->border_w[0] + ch->padding[0];
            ch->content_w = col_sizes[c]
                - ch->margin[1] - ch->margin[3]
                - ch->border_w[1] - ch->border_w[3]
                - ch->padding[1] - ch->padding[3];
            if (ch->content_w < 10) ch->content_w = 10;
            ch->grid_positioned = 1;
        }
    }
}

/* ================================================================== */
/* Layout — compute x, y, w, h for every node                         */
/* ================================================================== */

/*
 * ts_doc_layout — position all render nodes for a given viewport width.
 */
static void ts_doc_layout(struct ts_document *doc, int viewport_width) {
    int x = TS_DEFAULT_MARGIN;
    int y = TS_DEFAULT_MARGIN;
    int max_w = viewport_width - TS_DEFAULT_MARGIN * 2;
    int line_height = 0;
    int flex_bottom_y = 0;
    int i;

    if (max_w < 100) max_w = 100;

    /* First pass: compute box positions and content widths */
    for (i = 0; i < doc->box_count; i++) {
        struct ts_block_box *box = &doc->boxes[i];
        int parent_content_w = max_w;
        if (box->parent_box >= 0 && box->parent_box < doc->box_count)
            parent_content_w = doc->boxes[box->parent_box].content_w;
        if (parent_content_w <= 0) parent_content_w = max_w;

        /* Content width = explicit or parent minus margins/padding/borders */
        if (box->explicit_width > 0) {
            box->content_w = box->explicit_width;
        } else {
            box->content_w = parent_content_w
                - box->margin[1] - box->margin[3]
                - box->border_w[1] - box->border_w[3]
                - box->padding[1] - box->padding[3];
        }
        if (box->content_w < 20) box->content_w = 20;
    }

    int line_start_idx = 0; /* first node index on current line */

    for (i = 0; i < doc->node_count; i++) {
        struct ts_render_node *n = &doc->nodes[i];
        int indent = n->style.list_depth * 24; /* 24px per list level */
        int scale = n->style.font_scale;
        if (scale < 1) scale = 1;
        int char_w = TS_FONT_W * scale;
        int char_h = TS_FONT_H * scale;

        /* Compute effective layout width from box model */
        int layout_left = TS_DEFAULT_MARGIN;
        int layout_max = max_w;
        int bi = n->style.box_index;
        if (bi >= 0 && bi < doc->box_count) {
            struct ts_block_box *box = &doc->boxes[bi];
            if (box->flex_positioned || box->grid_positioned) {
                layout_left = box->content_x;
            } else {
                layout_left = TS_DEFAULT_MARGIN + box->margin[3] +
                              box->border_w[3] + box->padding[3];
            }
            layout_max = box->content_w;
        }

        switch (n->type) {
        case TS_NODE_TEXT:
        case TS_NODE_LIST_MARKER: {
            int tlen = n->text_len;
            int effective_max = layout_max - indent;
            if (effective_max < char_w) effective_max = char_w;

            /* Position text with word-wrap */
            if (x <= layout_left + indent)
                x = layout_left + indent;

            n->x = x;
            n->y = y;
            n->h = char_h;

            /* Compute text width */
            {
                int text_w = tlen * char_w;
                if (x - layout_left - indent + text_w <= effective_max) {
                    /* Fits on current line */
                    n->w = text_w;
                    x += text_w;
                    if (char_h > line_height) line_height = char_h;
                } else {
                    /* Word-wrap: find last space that fits */
                    int fit_chars = (effective_max - (x - layout_left - indent)) / char_w;
                    if (fit_chars < 1 && x == layout_left + indent) {
                        fit_chars = 1;
                    }
                    if (fit_chars < 1) {
                        y += line_height + TS_LINE_SPACING;
                        x = layout_left + indent;
                        n->x = x;
                        n->y = y;
                        line_height = 0;
                        fit_chars = effective_max / char_w;
                        if (fit_chars < 1) fit_chars = 1;
                    }
                    n->w = tlen * char_w;
                    x += n->w;
                    if (char_h > line_height) line_height = char_h;
                    if (x - layout_left > layout_max) {
                        y += line_height + TS_LINE_SPACING;
                        x = layout_left + indent;
                        line_height = 0;
                    }
                }
            }
            break;
        }

        case TS_NODE_BREAK: {
            /* ---- text-align centering for the line that just ended ---- */
            if (line_start_idx < i) {
                int need_center = 0, ll_left = 999999, ll_right = 0;
                int jj;
                for (jj = line_start_idx; jj < i; jj++) {
                    struct ts_render_node *ln = &doc->nodes[jj];
                    if (ln->w == 0 && ln->h == 0) continue;
                    if (ln->style.text_align == 1) need_center = 1;
                    if (ln->x < ll_left) ll_left = ln->x;
                    if (ln->x + ln->w > ll_right) ll_right = ln->x + ln->w;
                }
                if (need_center && ll_right > ll_left) {
                    int ll_w = ll_right - ll_left;
                    int ctr_max = layout_max;
                    int shift = (ctr_max - ll_w) / 2;
                    if (shift > 0) {
                        for (jj = line_start_idx; jj < i; jj++) {
                            if (doc->nodes[jj].w > 0 || doc->nodes[jj].h > 0)
                                doc->nodes[jj].x += shift;
                        }
                    }
                }
            }
            line_start_idx = i + 1;

            /* Check if this break starts a box — apply top margin+border+padding */
            int next_bi = -1;
            int entering_ib = 0;
            if (i + 1 < doc->node_count)
                next_bi = doc->nodes[i + 1].style.box_index;

            /* Detect inline-block entry: skip y-advance, position inline */
            if (next_bi >= 0 && next_bi < doc->box_count) {
                struct ts_block_box *nbox = &doc->boxes[next_bi];
                if ((nbox->node_start == i + 1 || nbox->node_start == i) &&
                    nbox->display_inline_block) {
                    entering_ib = 1;
                }
            }

            /* Detect inline-block exit: advance x, restore inline flow */
            {
                int cur_bi = n->style.box_index;
                if (cur_bi >= 0 && cur_bi < doc->box_count &&
                    doc->boxes[cur_bi].display_inline_block &&
                    i >= doc->boxes[cur_bi].node_end) {
                    /* Exiting inline-block: advance x by box width */
                    struct ts_block_box *ib = &doc->boxes[cur_bi];
                    x = ib->x + ib->total_w + 4; /* 4px gap */
                    y = ib->y; /* restore to box's y origin */
                    if (ib->total_h > line_height)
                        line_height = ib->total_h;
                    n->x = x; n->y = y; n->w = 0; n->h = 0;
                    break;
                }
            }

            if (!entering_ib)
                y += line_height + n->break_height + TS_LINE_SPACING;

            /* If exiting a flex/grid container, ensure y is past all content */
            {
                int cur_bi = n->style.box_index;
                if (cur_bi >= 0 && cur_bi < doc->box_count &&
                    (doc->boxes[cur_bi].display_flex ||
                     doc->boxes[cur_bi].display_grid) &&
                    i > doc->boxes[cur_bi].node_end) {
                    if (flex_bottom_y > y) y = flex_bottom_y;
                }
            }

            /* If entering a new box, apply its top spacing and record position */
            if (next_bi >= 0 && next_bi < doc->box_count) {
                struct ts_block_box *nbox = &doc->boxes[next_bi];
                /* Check if this is the start of this box */
                if (nbox->node_start == i + 1 || nbox->node_start == i) {
                    if (nbox->flex_positioned || nbox->grid_positioned) {
                        /* Flex/grid item: jump to pre-computed position */
                        x = nbox->content_x;
                        y = nbox->content_y;
                        line_height = 0;
                        n->x = x; n->y = y; n->w = 0; n->h = 0;
                        break;
                    }

                    /* Inline-block: position at current inline flow */
                    if (nbox->display_inline_block) {
                        nbox->x = x + nbox->margin[3];
                        nbox->y = y;
                        nbox->content_x = nbox->x + nbox->border_w[3] + nbox->padding[3];
                        nbox->content_y = nbox->y + nbox->border_w[0] + nbox->padding[0];
                        nbox->inline_block_positioned = 1;
                        /* Jump into inline-block content area */
                        x = nbox->content_x;
                        y = nbox->content_y;
                        line_height = 0;
                        n->x = x; n->y = y; n->w = 0; n->h = 0;
                        break;
                    }

                    y += nbox->margin[0] + nbox->border_w[0] + nbox->padding[0];
                    nbox->x = TS_DEFAULT_MARGIN + nbox->margin[3];
                    nbox->y = y - nbox->border_w[0] - nbox->padding[0];
                    nbox->content_x = nbox->x + nbox->border_w[3] + nbox->padding[3];
                    nbox->content_y = y;

                    /* If this box is a flex/grid container, layout its children */
                    if (nbox->display_flex) {
                        ts_doc__layout_flex_children(doc, next_bi);
                    }
                    if (nbox->display_grid) {
                        ts_doc__layout_grid_children(doc, next_bi);
                    }
                }
            }

            x = layout_left;
            line_height = 0;
            n->x = x;
            n->y = y;
            n->w = 0;
            n->h = 0;
            break;
        }

        case TS_NODE_HRULE:
            n->x = TS_DEFAULT_MARGIN;
            n->y = y;
            n->w = max_w;
            n->h = 1;
            y += 1;
            x = TS_DEFAULT_MARGIN;
            line_height = 0;
            break;

        case TS_NODE_IMAGE: {
            int iw = n->img_requested_w > 0 ? n->img_requested_w : 200;
            int ih = n->img_requested_h > 0 ? n->img_requested_h : 100;
            /* If image is decoded, use actual dimensions if no explicit size */
            if (n->img_cache_idx >= 0 && n->img_cache_idx < 64 &&
                doc->img_cache[n->img_cache_idx].used) {
                if (n->img_requested_w <= 0)
                    iw = doc->img_cache[n->img_cache_idx].w;
                if (n->img_requested_h <= 0)
                    ih = doc->img_cache[n->img_cache_idx].h;
                /* Scale down if wider than viewport */
                if (iw > max_w) {
                    ih = ih * max_w / iw;
                    iw = max_w;
                }
            }
            if (iw > max_w) iw = max_w;
            /* Start image on new line */
            if (x > TS_DEFAULT_MARGIN) {
                y += line_height + TS_LINE_SPACING;
                line_height = 0;
            }
            n->x = TS_DEFAULT_MARGIN;
            n->y = y;
            n->w = iw;
            n->h = ih;
            y += ih + TS_LINE_SPACING;
            x = TS_DEFAULT_MARGIN;
            line_height = 0;
            break;
        }

        case TS_NODE_SVG:
        case TS_NODE_CANVAS: {
            /* Canvas/SVG element — uses img_requested_w/h for dimensions */
            int cw = n->img_requested_w > 0 ? n->img_requested_w : 300;
            int ch = n->img_requested_h > 0 ? n->img_requested_h : 150;
            if (cw > max_w) cw = max_w;
            if (x > TS_DEFAULT_MARGIN) {
                y += line_height + TS_LINE_SPACING;
                line_height = 0;
            }
            n->x = TS_DEFAULT_MARGIN;
            n->y = y;
            n->w = cw;
            n->h = ch;
            y += ch + TS_LINE_SPACING;
            x = TS_DEFAULT_MARGIN;
            line_height = 0;
            break;
        }

        case TS_NODE_INPUT: {
            /* Text input field: 200px wide, 24px tall */
            int iw = 200;
            int ih = 24;
            if (iw > max_w - indent) iw = max_w - indent;
            n->x = x;
            n->y = y;
            n->w = iw;
            n->h = ih;
            x += iw + 8;
            if (ih > line_height) line_height = ih;
            break;
        }

        case TS_NODE_BUTTON: {
            /* Button: sized to text + padding */
            int tlen = n->text_len;
            int bw = tlen * TS_FONT_W + 16;
            int bh = 24;
            if (bw > max_w) bw = max_w;
            n->x = x;
            n->y = y;
            n->w = bw;
            n->h = bh;
            x += bw + 8;
            if (bh > line_height) line_height = bh;
            break;
        }
        }

        /* Track max bottom for flex container exit */
        if (n->h > 0 && n->y + n->h > flex_bottom_y)
            flex_bottom_y = n->y + n->h;
    }

    /* text-align centering for final line */
    if (line_start_idx < doc->node_count) {
        int need_center = 0, ll_left = 999999, ll_right = 0;
        int jj;
        for (jj = line_start_idx; jj < doc->node_count; jj++) {
            struct ts_render_node *ln = &doc->nodes[jj];
            if (ln->w == 0 && ln->h == 0) continue;
            if (ln->style.text_align == 1) need_center = 1;
            if (ln->x < ll_left) ll_left = ln->x;
            if (ln->x + ln->w > ll_right) ll_right = ln->x + ln->w;
        }
        if (need_center && ll_right > ll_left) {
            int ll_w = ll_right - ll_left;
            int shift = (max_w - ll_w) / 2;
            if (shift > 0) {
                for (jj = line_start_idx; jj < doc->node_count; jj++) {
                    if (doc->nodes[jj].w > 0 || doc->nodes[jj].h > 0)
                        doc->nodes[jj].x += shift;
                }
            }
        }
    }

    /* Final height */
    doc->content_height = y + line_height + TS_DEFAULT_MARGIN;
    doc->content_width = viewport_width;

    /* Finalize box dimensions from their child node positions */
    for (i = 0; i < doc->box_count; i++) {
        struct ts_block_box *box = &doc->boxes[i];
        int min_y = 999999, max_y = 0;
        int j;
        for (j = box->node_start; j <= box->node_end && j < doc->node_count; j++) {
            struct ts_render_node *cn = &doc->nodes[j];
            if (cn->h == 0 && cn->w == 0) continue; /* skip empty breaks */
            if (cn->y < min_y) min_y = cn->y;
            if (cn->y + cn->h > max_y) max_y = cn->y + cn->h;
        }
        if (min_y > max_y) { min_y = box->content_y; max_y = min_y; }

        /* Total box dimensions */
        box->total_w = box->content_w + box->padding[1] + box->padding[3] +
                       box->border_w[1] + box->border_w[3];
        int content_h = (box->explicit_height > 0) ? box->explicit_height
                        : (max_y - min_y);
        box->total_h = content_h + box->padding[0] + box->padding[2] +
                       box->border_w[0] + box->border_w[2];
    }

    /* Flex align-items: adjust child positions after heights are known */
    for (i = 0; i < doc->box_count; i++) {
        struct ts_block_box *fcontainer = &doc->boxes[i];
        int fj;
        if (!fcontainer->display_flex || fcontainer->flex_direction != 0)
            continue;
        /* stretch(0) and flex-start(1) need no adjustment */
        if (fcontainer->align_items <= 1) continue;

        /* Container content height */
        {
            int container_h = fcontainer->total_h
                - fcontainer->padding[0] - fcontainer->padding[2]
                - fcontainer->border_w[0] - fcontainer->border_w[2];
            if (container_h <= 0) continue;

            for (fj = 0; fj < doc->box_count; fj++) {
                struct ts_block_box *fch = &doc->boxes[fj];
                int shift = 0, fk;
                if (fch->parent_box != i || !fch->flex_positioned) continue;

                if (fcontainer->align_items == 2) /* center */
                    shift = (container_h - fch->total_h) / 2;
                else if (fcontainer->align_items == 3) /* flex-end */
                    shift = container_h - fch->total_h;

                if (shift > 0) {
                    for (fk = fch->node_start;
                         fk <= fch->node_end && fk < doc->node_count; fk++) {
                        doc->nodes[fk].y += shift;
                    }
                    fch->y += shift;
                    fch->content_y += shift;
                }
            }
        }
    }

    /* ---- Position: relative — shift nodes by offset ---- */
    for (i = 0; i < doc->box_count; i++) {
        struct ts_block_box *pbox = &doc->boxes[i];
        if (pbox->position == 1) { /* relative */
            int dx = 0, dy = 0, pj;
            if (pbox->pos_left != TS_POS_AUTO) dx = pbox->pos_left;
            else if (pbox->pos_right != TS_POS_AUTO) dx = -pbox->pos_right;
            if (pbox->pos_top != TS_POS_AUTO) dy = pbox->pos_top;
            else if (pbox->pos_bottom != TS_POS_AUTO) dy = -pbox->pos_bottom;
            if (dx != 0 || dy != 0) {
                for (pj = pbox->node_start;
                     pj <= pbox->node_end && pj < doc->node_count; pj++) {
                    doc->nodes[pj].x += dx;
                    doc->nodes[pj].y += dy;
                }
                pbox->x += dx; pbox->y += dy;
                pbox->content_x += dx; pbox->content_y += dy;
            }
        }
    }

    /* ---- Position: absolute/fixed — reposition relative to container ---- */
    for (i = 0; i < doc->box_count; i++) {
        struct ts_block_box *pbox = &doc->boxes[i];
        if (pbox->position == 2 || pbox->position == 3) {
            int ref_x, ref_y, ref_w, ref_h;
            int new_x, new_y, dx, dy, pj;

            if (pbox->position == 2) {
                /* absolute: find nearest positioned ancestor */
                int cb = pbox->parent_box;
                while (cb >= 0 && cb < doc->box_count &&
                       doc->boxes[cb].position == 0)
                    cb = doc->boxes[cb].parent_box;
                ref_x = (cb >= 0) ? doc->boxes[cb].content_x : TS_DEFAULT_MARGIN;
                ref_y = (cb >= 0) ? doc->boxes[cb].content_y : TS_DEFAULT_MARGIN;
                ref_w = (cb >= 0) ? doc->boxes[cb].content_w : max_w;
                ref_h = (cb >= 0) ? doc->boxes[cb].total_h : doc->content_height;
            } else {
                /* fixed: relative to viewport */
                ref_x = 0; ref_y = 0;
                ref_w = viewport_width;
                ref_h = doc->content_height;
            }

            new_x = pbox->x; new_y = pbox->y;
            if (pbox->pos_left != TS_POS_AUTO)
                new_x = ref_x + pbox->pos_left;
            else if (pbox->pos_right != TS_POS_AUTO)
                new_x = ref_x + ref_w - pbox->total_w - pbox->pos_right;
            if (pbox->pos_top != TS_POS_AUTO)
                new_y = ref_y + pbox->pos_top;
            else if (pbox->pos_bottom != TS_POS_AUTO)
                new_y = ref_y + ref_h - pbox->total_h - pbox->pos_bottom;

            dx = new_x - pbox->x;
            dy = new_y - pbox->y;
            if (dx != 0 || dy != 0) {
                for (pj = pbox->node_start;
                     pj <= pbox->node_end && pj < doc->node_count; pj++) {
                    doc->nodes[pj].x += dx;
                    doc->nodes[pj].y += dy;
                }
                pbox->x = new_x; pbox->y = new_y;
                pbox->content_x += dx; pbox->content_y += dy;
            }
        }
    }

    /* ---- Float: position floated boxes at left/right of container ---- */
    for (i = 0; i < doc->box_count; i++) {
        struct ts_block_box *fbox = &doc->boxes[i];
        int dx, pj;
        if (fbox->css_float == 0) continue;
        if (fbox->parent_box < 0 || fbox->parent_box >= doc->box_count) continue;
        {
            struct ts_block_box *parent = &doc->boxes[fbox->parent_box];
            if (fbox->css_float == 2) {
                /* float: right — align to right edge of parent */
                int right_x = parent->content_x + parent->content_w
                              - fbox->total_w;
                dx = right_x - fbox->x;
            } else {
                /* float: left — align to left edge of parent */
                dx = parent->content_x - fbox->x;
            }
            if (dx != 0) {
                for (pj = fbox->node_start;
                     pj <= fbox->node_end && pj < doc->node_count; pj++)
                    doc->nodes[pj].x += dx;
                fbox->x += dx;
                fbox->content_x += dx;
            }
        }
    }

    /* Update link bounding boxes */
    for (i = 0; i < doc->link_count; i++) {
        struct ts_link *link = &doc->links[i];
        int j;
        link->x1 = 999999; link->y1 = 999999;
        link->x2 = 0; link->y2 = 0;
        for (j = link->node_start; j <= link->node_end && j < doc->node_count; j++) {
            struct ts_render_node *n = &doc->nodes[j];
            if (n->x < link->x1) link->x1 = n->x;
            if (n->y < link->y1) link->y1 = n->y;
            if (n->x + n->w > link->x2) link->x2 = n->x + n->w;
            if (n->y + n->h > link->y2) link->y2 = n->y + n->h;
        }
    }
}

/* ================================================================== */
/* Rendering — paint visible nodes to a gfx context                    */
/* ================================================================== */

/* Draw scaled text (for headings with font_scale > 1) */
static void ts_draw_scaled_text(gfx_ctx_t *ctx, int x, int y,
                                 const char *text, int len, int scale,
                                 uint32_t fg, uint32_t bg) {
    /* TrueType path: render using stb_truetype baked atlas */
    if (ts_font_is_loaded()) {
        int size_idx = (scale <= 1) ? 0 : (scale >= 3) ? 2 : scale - 1;
        /* Draw background if specified */
        if (bg != TS_COL_TRANSPARENT) {
            int tw = ts_font_text_width(text, len, size_idx);
            int th = ts_font_line_height(size_idx);
            gfx_fill(ctx, (uint32_t)x, (uint32_t)y, (uint32_t)tw, (uint32_t)th, bg);
        }
        ts_font_draw_text(ctx->buffer, (int)ctx->width, (int)ctx->height,
                           x, y, text, len, size_idx, fg);
        return;
    }

    /* Bitmap fallback */
    if (scale <= 1) {
        /* Normal size — use gfx_draw_char directly */
        int i;
        for (i = 0; i < len; i++) {
            if (bg != TS_COL_TRANSPARENT)
                gfx_fill(ctx, (uint32_t)(x + i * TS_FONT_W), (uint32_t)y,
                         TS_FONT_W, TS_FONT_H, bg);
            gfx_draw_char(ctx, (uint32_t)(x + i * TS_FONT_W), (uint32_t)y,
                          text[i], fg, bg);
        }
        return;
    }

    /* Scaled bitmap fallback: bold effect */
    {
        int ci;
        for (ci = 0; ci < len; ci++) {
            int cx = x + ci * TS_FONT_W * scale;
            if (bg != TS_COL_TRANSPARENT)
                gfx_fill(ctx, (uint32_t)cx, (uint32_t)y,
                         (uint32_t)(TS_FONT_W * scale),
                         (uint32_t)(TS_FONT_H * scale), bg);
            gfx_draw_char(ctx, (uint32_t)cx, (uint32_t)y,
                          text[ci], fg, bg);
            gfx_draw_char(ctx, (uint32_t)(cx + 1), (uint32_t)y,
                          text[ci], fg, TS_COL_TRANSPARENT);
            if (scale >= 2) {
                gfx_draw_char(ctx, (uint32_t)cx, (uint32_t)(y + 1),
                              text[ci], fg, TS_COL_TRANSPARENT);
            }
        }
    }
}

/*
 * ts_doc_render — render visible portion of document.
 */
static void ts_doc_render(const struct ts_document *doc,
                           gfx_ctx_t *ctx,
                           int scroll_x, int scroll_y,
                           int viewport_width, int viewport_height) {
    int i;

    /* Clear viewport with page background (from <body>/<html> CSS, or default) */
    gfx_fill(ctx, 0, 0, (uint32_t)viewport_width,
             (uint32_t)viewport_height, doc->page_bg);

    /* Render block boxes (backgrounds + borders) */
    for (i = 0; i < doc->box_count; i++) {
        const struct ts_block_box *box = &doc->boxes[i];
        int bx = box->x - scroll_x + box->transform_x;
        int by = box->y - scroll_y + box->transform_y;
        int bw = box->total_w;
        int bh = box->total_h;

        /* Apply scale transform to dimensions */
        if (box->transform_scale_pct > 0 && box->transform_scale_pct != 100) {
            int cx = bx + bw / 2, cy = by + bh / 2;
            bw = bw * box->transform_scale_pct / 100;
            bh = bh * box->transform_scale_pct / 100;
            bx = cx - bw / 2;
            by = cy - bh / 2;
        }

        /* Skip invisible boxes */
        if (box->visibility_hidden) continue;
        if (box->opacity_pct == 0) continue; /* fully transparent */

        /* Cull off-screen boxes */
        if (by + bh < 0 || by > viewport_height) continue;
        if (bx + bw < 0 || bx > viewport_width) continue;
        if (bw <= 0 || bh <= 0) continue;

        /* Background */
        if (box->bg_color != TS_COL_TRANSPARENT) {
            int fx = bx + box->border_w[3];
            int fy = by + box->border_w[0];
            int fw = bw - box->border_w[1] - box->border_w[3];
            int fh = bh - box->border_w[0] - box->border_w[2];
            if (fw > 0 && fh > 0)
                gfx_fill(ctx, (uint32_t)(fx > 0 ? fx : 0),
                         (uint32_t)(fy > 0 ? fy : 0),
                         (uint32_t)fw, (uint32_t)fh, box->bg_color);
        }

        /* Borders */
        if (box->border_w[0] > 0) /* top */
            gfx_fill(ctx, (uint32_t)(bx > 0 ? bx : 0), (uint32_t)(by > 0 ? by : 0),
                     (uint32_t)bw, (uint32_t)box->border_w[0], box->border_color);
        if (box->border_w[2] > 0) /* bottom */
            gfx_fill(ctx, (uint32_t)(bx > 0 ? bx : 0),
                     (uint32_t)(by + bh - box->border_w[2]),
                     (uint32_t)bw, (uint32_t)box->border_w[2], box->border_color);
        if (box->border_w[3] > 0) /* left */
            gfx_fill(ctx, (uint32_t)(bx > 0 ? bx : 0), (uint32_t)(by > 0 ? by : 0),
                     (uint32_t)box->border_w[3], (uint32_t)bh, box->border_color);
        if (box->border_w[1] > 0) /* right */
            gfx_fill(ctx, (uint32_t)(bx + bw - box->border_w[1]),
                     (uint32_t)(by > 0 ? by : 0),
                     (uint32_t)box->border_w[1], (uint32_t)bh, box->border_color);
    }

    for (i = 0; i < doc->node_count; i++) {
        const struct ts_render_node *n = &doc->nodes[i];
        int sx = n->x - scroll_x;
        int sy = n->y - scroll_y;

        /* Apply transform offsets from parent box */
        {
            int bi = n->style.box_index;
            if (bi >= 0 && bi < doc->box_count) {
                const struct ts_block_box *nbox = &doc->boxes[bi];
                sx += nbox->transform_x;
                sy += nbox->transform_y;
                /* Skip nodes in invisible/transparent boxes */
                if (nbox->visibility_hidden) continue;
                if (nbox->opacity_pct == 0) continue;
            }
        }

        /* Cull nodes outside viewport */
        if (sy + n->h < 0) continue;
        if (sy > viewport_height) continue;
        if (sx + n->w < 0) continue;
        if (sx > viewport_width) continue;

        /* Overflow: hidden — skip nodes outside their container */
        {
            int obi = n->style.box_index;
            if (obi >= 0 && obi < doc->box_count) {
                int opb = doc->boxes[obi].parent_box;
                if (opb >= 0 && opb < doc->box_count &&
                    doc->boxes[opb].overflow == 1) {
                    const struct ts_block_box *ob = &doc->boxes[opb];
                    int ox1 = ob->x - scroll_x;
                    int oy1 = ob->y - scroll_y;
                    int ox2 = ox1 + ob->total_w;
                    int oy2 = oy1 + ob->total_h;
                    if (sx + n->w < ox1 || sx > ox2 ||
                        sy + n->h < oy1 || sy > oy2)
                        continue;
                }
            }
        }

        switch (n->type) {
        case TS_NODE_TEXT:
        case TS_NODE_LIST_MARKER: {
            const char *text = doc->text_buf + n->text_offset;
            int tlen = n->text_len;

            /* Background */
            if (n->style.bg_color != TS_COL_TRANSPARENT) {
                gfx_fill(ctx, (uint32_t)(sx > 0 ? sx : 0), (uint32_t)(sy > 0 ? sy : 0),
                         (uint32_t)n->w, (uint32_t)n->h, n->style.bg_color);
            }

            /* Text */
            ts_draw_scaled_text(ctx, sx, sy, text, tlen,
                                n->style.font_scale,
                                n->style.fg_color, TS_COL_TRANSPARENT);

            /* Bold: second pass at +1px offset */
            if (n->style.bold && n->style.font_scale <= 1) {
                ts_draw_scaled_text(ctx, sx + 1, sy, text, tlen,
                                    1, n->style.fg_color, TS_COL_TRANSPARENT);
            }

            /* Underline */
            if (n->style.underline && n->h > 0) {
                gfx_fill(ctx, (uint32_t)(sx > 0 ? sx : 0),
                         (uint32_t)(sy + n->h - 1),
                         (uint32_t)n->w, 1, n->style.fg_color);
            }

            /* Strikethrough */
            if (n->style.strikethrough && n->h > 0) {
                gfx_fill(ctx, (uint32_t)(sx > 0 ? sx : 0),
                         (uint32_t)(sy + n->h / 2),
                         (uint32_t)n->w, 1, n->style.fg_color);
            }
            break;
        }

        case TS_NODE_BREAK:
            /* Nothing to render */
            break;

        case TS_NODE_HRULE:
            gfx_fill(ctx, (uint32_t)(sx > 0 ? sx : 0), (uint32_t)(sy > 0 ? sy : 0),
                     (uint32_t)n->w, 1, TS_COL_HRULE);
            break;

        case TS_NODE_INPUT: {
            /* Render text input field */
            int fx = sx > 0 ? sx : 0;
            int fy = sy > 0 ? sy : 0;
            /* Field background */
            gfx_fill(ctx, (uint32_t)fx, (uint32_t)fy,
                     (uint32_t)n->w, (uint32_t)n->h, 0x1C2333);
            /* Border */
            gfx_rect(ctx, (uint32_t)fx, (uint32_t)fy,
                     (uint32_t)n->w, (uint32_t)n->h, 0x4A5A70);
            /* Text content or placeholder */
            if (n->form_idx >= 0 && n->form_idx < doc->form_input_count) {
                const struct ts_form_input *fi = &doc->form_inputs[n->form_idx];
                const char *display = fi->value_len > 0 ? fi->value
                                                         : fi->placeholder;
                uint32_t text_col = fi->value_len > 0 ? 0xE6ECF8 : 0x5A6A80;
                int ci;
                int max_chars = (n->w - 8) / TS_FONT_W;
                for (ci = 0; display[ci] && ci < max_chars; ci++) {
                    gfx_draw_char(ctx, (uint32_t)(fx + 4 + ci * TS_FONT_W),
                                  (uint32_t)(fy + 4),
                                  display[ci], text_col, TS_COL_TRANSPARENT);
                }
                /* Cursor if focused */
                if (fi->focused) {
                    int cx = fx + 4 + fi->cursor * TS_FONT_W;
                    if (cx < fx + n->w - 4)
                        gfx_fill(ctx, (uint32_t)cx, (uint32_t)(fy + 3),
                                 1, (uint32_t)(n->h - 6), 0x29B6F6);
                    /* Focused border highlight */
                    gfx_rect(ctx, (uint32_t)fx, (uint32_t)fy,
                             (uint32_t)n->w, (uint32_t)n->h, 0x29B6F6);
                }
            }
            break;
        }

        case TS_NODE_BUTTON: {
            /* Render button */
            int bx = sx > 0 ? sx : 0;
            int by = sy > 0 ? sy : 0;
            gfx_fill_rounded(ctx, (uint32_t)bx, (uint32_t)by,
                              (uint32_t)n->w, (uint32_t)n->h, 0x2A364A, 3);
            /* Button text */
            if (n->text_len > 0) {
                const char *label = doc->text_buf + n->text_offset;
                int lx = bx + (n->w - n->text_len * TS_FONT_W) / 2;
                int ly = by + (n->h - TS_FONT_H) / 2;
                int ci;
                for (ci = 0; ci < n->text_len; ci++) {
                    gfx_draw_char(ctx, (uint32_t)(lx + ci * TS_FONT_W),
                                  (uint32_t)ly, label[ci],
                                  0xE6ECF8, TS_COL_TRANSPARENT);
                }
            }
            break;
        }

        case TS_NODE_IMAGE:
            /* Check if decoded image is available in cache */
            if (n->img_cache_idx >= 0 && n->img_cache_idx < 64 &&
                doc->img_cache[n->img_cache_idx].used &&
                doc->img_cache[n->img_cache_idx].pixels) {
                /* Blit decoded image, scaled to fit render node */
                uint32_t *src = doc->img_cache[n->img_cache_idx].pixels;
                int src_w = doc->img_cache[n->img_cache_idx].w;
                int src_h = doc->img_cache[n->img_cache_idx].h;
                int dst_w = n->w > 0 ? n->w : src_w;
                int dst_h = n->h > 0 ? n->h : src_h;
                int dy, dx;
                for (dy = 0; dy < dst_h; dy++) {
                    int dst_y = sy + dy;
                    if (dst_y < 0 || dst_y >= viewport_height) continue;
                    int src_y = dy * src_h / dst_h;
                    if (src_y >= src_h) src_y = src_h - 1;
                    for (dx = 0; dx < dst_w; dx++) {
                        int dst_x = sx + dx;
                        if (dst_x < 0 || dst_x >= viewport_width) continue;
                        int src_x = dx * src_w / dst_w;
                        if (src_x >= src_w) src_x = src_w - 1;
                        uint32_t pixel = src[src_y * src_w + src_x];
                        uint8_t alpha = (uint8_t)(pixel >> 24);
                        if (alpha > 128) {
                            /* Write pixel (strip alpha for framebuffer) */
                            ctx->buffer[(uint32_t)dst_y * ctx->stride +
                                        (uint32_t)dst_x] = pixel & 0x00FFFFFF;
                        }
                    }
                }
            } else {
                /* Placeholder rectangle */
                gfx_fill(ctx, (uint32_t)(sx > 0 ? sx : 0),
                         (uint32_t)(sy > 0 ? sy : 0),
                         (uint32_t)n->w, (uint32_t)n->h, 0x1A2233);
                gfx_rect(ctx, (uint32_t)(sx > 0 ? sx : 0),
                         (uint32_t)(sy > 0 ? sy : 0),
                         (uint32_t)n->w, (uint32_t)n->h, TS_COL_HRULE);
                /* Alt text */
                if (n->text_len > 0) {
                    const char *alt = doc->text_buf + n->text_offset;
                    gfx_draw_text(ctx, (uint32_t)(sx + 4),
                                  (uint32_t)(sy + 4),
                                  "[IMG]", TS_COL_TEXT_DIM,
                                  TS_COL_TRANSPARENT);
                    if (sy + 20 < viewport_height) {
                        int alen = n->text_len;
                        int ax = sx + 4;
                        int ay = sy + 20;
                        int j;
                        for (j = 0; j < alen && j < 30; j++) {
                            gfx_draw_char(ctx,
                                          (uint32_t)(ax + j * TS_FONT_W),
                                          (uint32_t)ay, alt[j],
                                          TS_COL_TEXT_DIM,
                                          TS_COL_TRANSPARENT);
                        }
                    }
                } else {
                    gfx_draw_text(ctx, (uint32_t)(sx + 4),
                                  (uint32_t)(sy + 4),
                                  "[IMG]", TS_COL_TEXT_DIM,
                                  TS_COL_TRANSPARENT);
                }
            }
            break;

        case TS_NODE_SVG:
        case TS_NODE_CANVAS:
            /* Blit canvas/SVG pixel buffer */
            { int ci2 = n->img_cache_idx;
              if (ci2 >= 0 && ci2 < 16 &&
                  doc->canvas_cache[ci2].used &&
                  doc->canvas_cache[ci2].pixels) {
                  uint32_t *csrc = doc->canvas_cache[ci2].pixels;
                  int cw2 = doc->canvas_cache[ci2].w;
                  int ch2 = doc->canvas_cache[ci2].h;
                  int dy2, dx2;
                  for (dy2 = 0; dy2 < ch2 && dy2 < n->h; dy2++) {
                      int dst_y = sy + dy2;
                      if (dst_y < 0 || dst_y >= viewport_height) continue;
                      for (dx2 = 0; dx2 < cw2 && dx2 < n->w; dx2++) {
                          int dst_x = sx + dx2;
                          if (dst_x < 0 || dst_x >= viewport_width) continue;
                          { uint32_t pixel = csrc[dy2 * cw2 + dx2];
                            if (pixel != 0)
                                ctx->buffer[(uint32_t)dst_y * ctx->stride +
                                            (uint32_t)dst_x] = pixel & 0x00FFFFFF;
                          }
                      }
                  }
              } else {
                  gfx_rect(ctx, (uint32_t)(sx > 0 ? sx : 0),
                           (uint32_t)(sy > 0 ? sy : 0),
                           (uint32_t)n->w, (uint32_t)n->h, 0x444444);
              }
            }
            break;
        } /* end switch */
    }
}

/* ================================================================== */
/* Hit testing — find link at viewport coordinates                     */
/* ================================================================== */

/*
 * ts_doc_hit_test — find the link index at a viewport click position.
 * Returns link index or -1 if no link at that position.
 */
static int ts_doc_hit_test(const struct ts_document *doc,
                            int vx, int vy,
                            int scroll_x, int scroll_y) {
    int dx = vx + scroll_x;
    int dy = vy + scroll_y;
    int i;

    for (i = 0; i < doc->link_count; i++) {
        const struct ts_link *link = &doc->links[i];
        if (dx >= link->x1 && dx <= link->x2 &&
            dy >= link->y1 && dy <= link->y2)
            return i;
    }
    return -1;
}

/*
 * ts_doc_hit_test_input — find form input at viewport click position.
 * Returns form_input index or -1.
 */
static int ts_doc_hit_test_input(const struct ts_document *doc,
                                  int vx, int vy,
                                  int scroll_x, int scroll_y) {
    int dx = vx + scroll_x;
    int dy = vy + scroll_y;
    int i;
    for (i = 0; i < doc->node_count; i++) {
        const struct ts_render_node *n = &doc->nodes[i];
        if (n->type == TS_NODE_INPUT && n->form_idx >= 0) {
            if (dx >= n->x && dx <= n->x + n->w &&
                dy >= n->y && dy <= n->y + n->h)
                return n->form_idx;
        }
    }
    return -1;
}

#endif /* TS_LAYOUT_H */
