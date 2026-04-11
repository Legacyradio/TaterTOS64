/*
 * ts_css.h — TaterSurf CSS parser
 *
 * Header-only. Parses CSS from <style> blocks and style="" attributes.
 * Supports selectors: tag, .class, #id, tag.class, descendant (space),
 * child (>), comma groups. Properties limited to the set that matters
 * for web page rendering on an 8x16 bitmap font.
 *
 * Does NOT support: media queries, @import, @keyframes, pseudo-elements
 * (::before/::after), complex pseudo-classes beyond :first-child/:last-child.
 */

#ifndef TS_CSS_H
#define TS_CSS_H

#include <stddef.h>
#include <stdint.h>

/* ================================================================== */
/* Constants                                                           */
/* ================================================================== */

#define TS_CSS_MAX_RULES         512
#define TS_CSS_MAX_PROPERTIES     24
#define TS_CSS_MAX_SELECTORS       8  /* comma-separated selectors per rule */

/* ================================================================== */
/* Data structures                                                     */
/* ================================================================== */

struct ts_css_property {
    char name[32];         /* e.g. "color", "margin-left", "display" */
    char value[128];       /* e.g. "red", "10px", "block", "#ff0000" */
};

/* Selector component (one part of a compound selector) */
struct ts_css_selector_part {
    char tag[32];          /* tag name or "" for any */
    char cls[64];          /* class name or "" */
    char id[64];           /* id or "" */
    char pseudo[32];       /* pseudo-class or "" (e.g. "hover", "first-child") */
    char combinator;       /* ' '=descendant, '>'=child, 0=none (first part) */
};

/* Full selector: up to 4 parts (e.g. "div .content > p.intro") */
#define TS_CSS_MAX_SELECTOR_PARTS 4

struct ts_css_selector {
    struct ts_css_selector_part parts[TS_CSS_MAX_SELECTOR_PARTS];
    int part_count;
    int specificity;       /* computed: (id*100) + (class*10) + tag */
};

struct ts_css_rule {
    struct ts_css_selector selectors[TS_CSS_MAX_SELECTORS];
    int selector_count;    /* number of comma-separated selectors */
    struct ts_css_property props[TS_CSS_MAX_PROPERTIES];
    int prop_count;
};

struct ts_stylesheet {
    struct ts_css_rule rules[TS_CSS_MAX_RULES];
    int rule_count;
};

/* ================================================================== */
/* Internal helpers                                                    */
/* ================================================================== */

static int ts_css__is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int ts_css__is_ident(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_';
}

static char ts_css__lower(char c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

static void ts_css__skip_ws(const char **p, const char *end) {
    while (*p < end && ts_css__is_space(**p)) (*p)++;
}

static void ts_css__skip_comment(const char **p, const char *end) {
    if (*p + 1 < end && (*p)[0] == '/' && (*p)[1] == '*') {
        *p += 2;
        while (*p + 1 < end) {
            if ((*p)[0] == '*' && (*p)[1] == '/') {
                *p += 2;
                return;
            }
            (*p)++;
        }
        *p = end;
    }
}

static void ts_css__skip_ws_comments(const char **p, const char *end) {
    while (*p < end) {
        ts_css__skip_ws(p, end);
        if (*p + 1 < end && (*p)[0] == '/' && (*p)[1] == '*')
            ts_css__skip_comment(p, end);
        else
            break;
    }
}

/* Read an identifier */
static size_t ts_css__read_ident(const char *p, const char *end,
                                  char *buf, size_t max) {
    size_t i = 0;
    while (p < end && ts_css__is_ident(*p) && i < max - 1) {
        buf[i++] = ts_css__lower(*p++);
    }
    buf[i] = '\0';
    return i;
}

/* ================================================================== */
/* Color parsing                                                       */
/* ================================================================== */

/*
 * ts_css_color — parse a CSS color value.
 *
 * Supports: #RGB, #RRGGBB, rgb(r,g,b), rgba(r,g,b,a), named colors.
 * Returns 0xRRGGBB (alpha channel ignored for now).
 * Returns 0x000000 on parse failure.
 */
static uint32_t ts_css_color(const char *value) {
    const char *p = value;

    /* Skip leading whitespace */
    while (*p == ' ') p++;

    /* #hex */
    if (*p == '#') {
        p++;
        uint32_t hex = 0;
        int digits = 0;
        while (*p) {
            char c = *p;
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            hex = (hex << 4) | (uint32_t)d;
            digits++;
            p++;
        }
        if (digits == 3) {
            /* #RGB → #RRGGBB */
            uint32_t r = (hex >> 8) & 0xF;
            uint32_t g = (hex >> 4) & 0xF;
            uint32_t b = hex & 0xF;
            return (r | (r << 4)) << 16 | (g | (g << 4)) << 8 | (b | (b << 4));
        }
        if (digits == 6) return hex;
        if (digits == 8) return hex >> 8; /* #RRGGBBAA → drop alpha */
        return 0;
    }

    /* rgb(r, g, b) or rgba(r, g, b, a) */
    if ((p[0] == 'r' && p[1] == 'g' && p[2] == 'b') &&
        (p[3] == '(' || (p[3] == 'a' && p[4] == '('))) {
        const char *s = p + 3;
        if (*s == 'a') s++;
        s++; /* skip '(' */
        int vals[4] = {0, 0, 0, 255};
        int vi = 0;
        while (*s && *s != ')' && vi < 4) {
            while (*s == ' ' || *s == ',') s++;
            int v = 0;
            int neg = 0;
            if (*s == '-') { neg = 1; s++; }
            while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
            if (*s == '.') { s++; while (*s >= '0' && *s <= '9') s++; } /* skip decimal */
            if (*s == '%') { v = v * 255 / 100; s++; }
            if (neg) v = -v;
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            vals[vi++] = v;
        }
        return ((uint32_t)vals[0] << 16) | ((uint32_t)vals[1] << 8) | (uint32_t)vals[2];
    }

    /* Named colors — comprehensive list */
    struct { const char *name; uint32_t color; } named[] = {
        {"black",       0x000000}, {"white",       0xFFFFFF},
        {"red",         0xFF0000}, {"green",       0x008000},
        {"blue",        0x0000FF}, {"yellow",      0xFFFF00},
        {"cyan",        0x00FFFF}, {"magenta",     0xFF00FF},
        {"orange",      0xFFA500}, {"purple",      0x800080},
        {"pink",        0xFFC0CB}, {"brown",       0xA52A2A},
        {"gray",        0x808080}, {"grey",        0x808080},
        {"silver",      0xC0C0C0}, {"gold",        0xFFD700},
        {"navy",        0x000080}, {"teal",        0x008080},
        {"maroon",      0x800000}, {"olive",       0x808000},
        {"lime",        0x00FF00}, {"aqua",        0x00FFFF},
        {"fuchsia",     0xFF00FF}, {"coral",       0xFF7F50},
        {"salmon",      0xFA8072}, {"tomato",      0xFF6347},
        {"crimson",     0xDC143C}, {"darkred",     0x8B0000},
        {"darkgreen",   0x006400}, {"darkblue",    0x00008B},
        {"darkgray",    0xA9A9A9}, {"darkgrey",    0xA9A9A9},
        {"lightgray",   0xD3D3D3}, {"lightgrey",   0xD3D3D3},
        {"lightblue",   0xADD8E6}, {"lightgreen",  0x90EE90},
        {"lightyellow", 0xFFFFE0}, {"lightcoral",  0xF08080},
        {"darkslategray", 0x2F4F4F}, {"slategray",   0x708090},
        {"dimgray",     0x696969}, {"gainsboro",   0xDCDCDC},
        {"whitesmoke",  0xF5F5F5}, {"ghostwhite",  0xF8F8FF},
        {"aliceblue",   0xF0F8FF}, {"lavender",    0xE6E6FA},
        {"beige",       0xF5F5DC}, {"ivory",       0xFFFFF0},
        {"honeydew",    0xF0FFF0}, {"mintcream",   0xF5FFFA},
        {"azure",       0xF0FFFF}, {"snow",        0xFFFAFA},
        {"seashell",    0xFFF5EE}, {"linen",       0xFAF0E6},
        {"cornsilk",    0xFFF8DC}, {"wheat",       0xF5DEB3},
        {"tan",         0xD2B48C}, {"chocolate",   0xD2691E},
        {"firebrick",   0xB22222}, {"indianred",   0xCD5C5C},
        {"royalblue",   0x4169E1}, {"steelblue",   0x4682B4},
        {"dodgerblue",  0x1E90FF}, {"deepskyblue", 0x00BFFF},
        {"skyblue",     0x87CEEB}, {"turquoise",   0x40E0D0},
        {"mediumaquamarine", 0x66CDAA},
        {"mediumseagreen", 0x3CB371},
        {"seagreen",    0x2E8B57}, {"forestgreen", 0x228B22},
        {"limegreen",   0x32CD32}, {"springgreen", 0x00FF7F},
        {"greenyellow", 0xADFF2F}, {"chartreuse",  0x7FFF00},
        {"yellowgreen", 0x9ACD32}, {"olivedrab",   0x6B8E23},
        {"darkkhaki",   0xBDB76B}, {"khaki",       0xF0E68C},
        {"palegoldenrod", 0xEEE8AA},
        {"moccasin",    0xFFE4B5}, {"peachpuff",   0xFFFDAD5},
        {"papayawhip",  0xFFEFD5}, {"bisque",      0xFFE4C4},
        {"blanchedalmond", 0xFFEBCD},
        {"navajowhite", 0xFFDEAD}, {"sandybrown",  0xF4A460},
        {"rosybrown",   0xBC8F8F}, {"sienna",      0xA0522D},
        {"saddlebrown", 0x8B4513}, {"peru",        0xCD853F},
        {"burlywood",   0xDEB887},
        {"darkorange",  0xFF8C00}, {"orangered",   0xFF4500},
        {"deeppink",    0xFF1493}, {"hotpink",     0xFF69B4},
        {"palevioletred", 0xDB7093},
        {"mediumvioletred", 0xC71585},
        {"orchid",      0xDA70D6}, {"plum",        0xDDA0DD},
        {"violet",      0xEE82EE}, {"darkorchid",  0x9932CC},
        {"darkviolet",  0x9400D3}, {"blueviolet",  0x8A2BE2},
        {"mediumpurple", 0x9370DB},
        {"slateblue",   0x6A5ACD}, {"darkslateblue", 0x483D8B},
        {"mediumslateblue", 0x7B68EE},
        {"indigo",      0x4B0082}, {"rebeccapurple", 0x663399},
        {"midnightblue", 0x191970},
        {"cornflowerblue", 0x6495ED},
        {"mediumblue",  0x0000CD},
        {"darkcyan",    0x008B8B}, {"cadetblue",   0x5F9EA0},
        {"powderblue",  0xB0E0E6}, {"paleturquoise", 0xAFEEEE},
        {"mediumturquoise", 0x48D1CC},
        {"darkturquoise", 0x00CED1},
        {"aquamarine",  0x7FFFD4},
        {"transparent", 0xFF000000}, /* special: use as flag */
        {NULL, 0}
    };

    {
        int i;
        for (i = 0; named[i].name; i++) {
            const char *n = named[i].name;
            const char *v = p;
            int match = 1;
            while (*n && *v) {
                if (ts_css__lower(*v) != *n) { match = 0; break; }
                n++; v++;
            }
            if (match && *n == '\0' && (*v == '\0' || *v == ' ' || *v == ';' || *v == '!'))
                return named[i].color;
        }
    }

    return 0xFF000000; /* invalid — same as transparent sentinel */
}

/* ================================================================== */
/* Parse a single CSS value to pixels (px, em, rem, %)                 */
/* ================================================================== */

/*
 * ts_css_to_px — convert a CSS length value to pixels.
 * base_px is the parent/reference size for em/rem/%.
 * Returns pixel value. Defaults to 0 for unrecognized values.
 */
static int ts_css_to_px(const char *value, int base_px) {
    const char *p = value;
    int neg = 0;
    int result = 0;
    int frac = 0;
    int frac_div = 1;

    while (*p == ' ') p++;
    if (*p == '-') { neg = 1; p++; }

    /* "auto" → 0 (caller handles auto semantics) */
    if (p[0] == 'a' && p[1] == 'u' && p[2] == 't' && p[3] == 'o') return 0;

    /* Integer part */
    while (*p >= '0' && *p <= '9') {
        result = result * 10 + (*p - '0');
        p++;
    }

    /* Fractional part */
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') {
            frac = frac * 10 + (*p - '0');
            frac_div *= 10;
            p++;
        }
    }

    /* Unit */
    while (*p == ' ') p++;
    if (p[0] == 'p' && p[1] == 'x') {
        /* px — already in pixels */
    } else if (p[0] == 'e' && p[1] == 'm') {
        result = result * base_px + (frac * base_px / frac_div);
        frac = 0;
    } else if (p[0] == 'r' && p[1] == 'e' && p[2] == 'm') {
        result = result * 16 + (frac * 16 / frac_div); /* rem = 16px base */
        frac = 0;
    } else if (*p == '%') {
        result = result * base_px / 100 + (frac * base_px / (frac_div * 100));
        frac = 0;
    } else if (p[0] == 'p' && p[1] == 't') {
        result = result * 4 / 3; /* 1pt ≈ 1.333px */
    } else if (p[0] == 'v' && (p[1] == 'w' || p[1] == 'h')) {
        /* viewport units — approximate: assume 900px wide, 600px tall */
        int vp = (p[1] == 'w') ? 900 : 600;
        result = result * vp / 100;
    }
    /* else: unitless number — treat as px */

    if (neg) result = -result;
    return result;
}

/* ================================================================== */
/* Parse a selector string into structured parts                       */
/* ================================================================== */

static void ts_css__parse_selector(const char *sel_str, size_t sel_len,
                                    struct ts_css_selector *out) {
    const char *p = sel_str;
    const char *end = sel_str + sel_len;
    int pi = 0;

    out->part_count = 0;
    out->specificity = 0;

    ts_css__skip_ws(&p, end);

    while (p < end && pi < TS_CSS_MAX_SELECTOR_PARTS) {
        struct ts_css_selector_part *part = &out->parts[pi];
        part->tag[0] = '\0';
        part->cls[0] = '\0';
        part->id[0] = '\0';
        part->pseudo[0] = '\0';
        part->combinator = (pi == 0) ? 0 : ' '; /* default: descendant */

        ts_css__skip_ws(&p, end);
        if (p >= end) break;

        /* Check for combinator */
        if (pi > 0 && *p == '>') {
            part->combinator = '>';
            p++;
            ts_css__skip_ws(&p, end);
        }

        /* Parse selector components: tag#id.class:pseudo */
        while (p < end && !ts_css__is_space(*p) && *p != '>' && *p != ',' && *p != '{') {
            if (*p == '#') {
                p++;
                ts_css__read_ident(p, end, part->id, sizeof(part->id));
                while (p < end && ts_css__is_ident(*p)) p++;
                out->specificity += 100;
            } else if (*p == '.') {
                p++;
                ts_css__read_ident(p, end, part->cls, sizeof(part->cls));
                while (p < end && ts_css__is_ident(*p)) p++;
                out->specificity += 10;
            } else if (*p == ':') {
                p++;
                if (p < end && *p == ':') p++; /* skip :: pseudo-elements */
                ts_css__read_ident(p, end, part->pseudo, sizeof(part->pseudo));
                while (p < end && ts_css__is_ident(*p)) p++;
                /* Skip pseudo-class arguments like :nth-child(2n) */
                if (p < end && *p == '(') {
                    int depth = 1;
                    p++;
                    while (p < end && depth > 0) {
                        if (*p == '(') depth++;
                        else if (*p == ')') depth--;
                        p++;
                    }
                }
            } else if (*p == '[') {
                /* Attribute selector — skip for now */
                while (p < end && *p != ']') p++;
                if (p < end) p++; /* skip ']' */
                out->specificity += 10;
            } else if (*p == '*') {
                /* Universal selector */
                p++;
            } else {
                /* Tag name */
                ts_css__read_ident(p, end, part->tag, sizeof(part->tag));
                while (p < end && ts_css__is_ident(*p)) p++;
                out->specificity += 1;
            }
        }

        pi++;
        out->part_count = pi;
    }
}

/* ================================================================== */
/* CSS stylesheet parsing                                              */
/* ================================================================== */

/*
 * ts_css_parse — parse a CSS stylesheet string into rules.
 *
 * Handles: rule blocks (selector { properties }), @media (skipped),
 * comments, comma-separated selectors.
 */
static void ts_css_parse(struct ts_stylesheet *ss,
                          const char *css, size_t len) {
    const char *p = css;
    const char *end = css + len;

    if (!ss) return;
    /* Don't zero — may be called multiple times to accumulate rules */
    fprintf(stderr, "CSS_PARSE: entry rc=%d len=%zu\n", ss->rule_count, len);

    while (p < end) {
        ts_css__skip_ws_comments(&p, end);
        if (p >= end) break;

        /* Skip @-rules (media, import, keyframes, etc.) */
        if (*p == '@') {
            /* Find matching '{' and skip the whole block */
            while (p < end && *p != '{' && *p != ';') p++;
            if (p < end && *p == '{') {
                int depth = 1;
                p++;
                while (p < end && depth > 0) {
                    if (*p == '{') depth++;
                    else if (*p == '}') depth--;
                    p++;
                }
            } else if (p < end) {
                p++; /* skip ';' */
            }
            continue;
        }

        /* Parse selector(s) — everything up to '{' */
        {
            const char *sel_start = p;
            while (p < end && *p != '{') p++;
            if (p >= end) break;
            {
                size_t sel_total_len = (size_t)(p - sel_start);
                p++; /* skip '{' */

                /* Parse property block — everything up to '}' */
                const char *props_start = p;
                while (p < end && *p != '}') p++;
                {
                    size_t props_len = (size_t)(p - props_start);
                    if (p < end) p++; /* skip '}' */

                    if (ss->rule_count >= TS_CSS_MAX_RULES) continue;

                    {
                        struct ts_css_rule *rule = &ss->rules[ss->rule_count];
                        rule->selector_count = 0;
                        rule->prop_count = 0;

                        /* Parse comma-separated selectors */
                        {
                            const char *s = sel_start;
                            const char *s_end = sel_start + sel_total_len;
                            while (s < s_end && rule->selector_count < TS_CSS_MAX_SELECTORS) {
                                const char *comma = s;
                                while (comma < s_end && *comma != ',') comma++;
                                /* Trim whitespace */
                                const char *ss_start = s;
                                while (ss_start < comma && ts_css__is_space(*ss_start)) ss_start++;
                                const char *ss_end_p = comma;
                                while (ss_end_p > ss_start && ts_css__is_space(ss_end_p[-1])) ss_end_p--;
                                if (ss_end_p > ss_start) {
                                    ts_css__parse_selector(ss_start, (size_t)(ss_end_p - ss_start),
                                                            &rule->selectors[rule->selector_count]);
                                    rule->selector_count++;
                                }
                                s = (comma < s_end) ? comma + 1 : s_end;
                            }
                        }

                        /* Parse properties */
                        {
                            const char *pp = props_start;
                            const char *pp_end = props_start + props_len;
                            while (pp < pp_end && rule->prop_count < TS_CSS_MAX_PROPERTIES) {
                                ts_css__skip_ws_comments(&pp, pp_end);
                                if (pp >= pp_end) break;

                                /* Property name */
                                const char *pname = pp;
                                while (pp < pp_end && *pp != ':' && *pp != ';' && *pp != '}')
                                    pp++;
                                if (pp >= pp_end || *pp != ':') {
                                    if (pp < pp_end) pp++;
                                    continue;
                                }
                                {
                                    const char *pname_end = pp;
                                    while (pname_end > pname && ts_css__is_space(pname_end[-1]))
                                        pname_end--;
                                    pp++; /* skip ':' */

                                    /* Property value */
                                    ts_css__skip_ws(&pp, pp_end);
                                    const char *pval = pp;
                                    while (pp < pp_end && *pp != ';' && *pp != '}') pp++;
                                    {
                                        const char *pval_end = pp;
                                        while (pval_end > pval && ts_css__is_space(pval_end[-1]))
                                            pval_end--;
                                        /* Strip !important */
                                        if (pval_end - pval >= 10) {
                                            const char *imp = pval_end - 10;
                                            if (imp[0] == '!' &&
                                                ts_css__lower(imp[1]) == 'i' &&
                                                ts_css__lower(imp[2]) == 'm') {
                                                pval_end = imp;
                                                while (pval_end > pval && ts_css__is_space(pval_end[-1]))
                                                    pval_end--;
                                            }
                                        }

                                        /* Store property */
                                        {
                                            struct ts_css_property *prop =
                                                &rule->props[rule->prop_count];
                                            size_t nl = (size_t)(pname_end - pname);
                                            size_t vl = (size_t)(pval_end - pval);
                                            if (nl >= sizeof(prop->name)) nl = sizeof(prop->name) - 1;
                                            if (vl >= sizeof(prop->value)) vl = sizeof(prop->value) - 1;
                                            {
                                                size_t i;
                                                for (i = 0; i < nl; i++)
                                                    prop->name[i] = ts_css__lower(pname[i]);
                                                prop->name[nl] = '\0';
                                            }
                                            {
                                                size_t i;
                                                for (i = 0; i < vl; i++)
                                                    prop->value[i] = pval[i];
                                                prop->value[vl] = '\0';
                                            }
                                            rule->prop_count++;
                                        }
                                    }
                                }
                            }
                        }

                        if (rule->selector_count > 0 && rule->prop_count > 0)
                            ss->rule_count++;
                        if (ss->rule_count > TS_CSS_MAX_RULES) {
                            fprintf(stderr, "CSS_CORRUPT: rc=%d after rule parse!\n", ss->rule_count);
                        }
                    }
                }
            }
        }
    }
    fprintf(stderr, "CSS_PARSE: exit rc=%d\n", ss->rule_count);
}

/* ================================================================== */
/* Inline style parsing                                                */
/* ================================================================== */

/*
 * ts_css_parse_inline — parse a style="" attribute into properties.
 */
static void ts_css_parse_inline(const char *style_attr,
                                 struct ts_css_property *props,
                                 int *prop_count, int max_props) {
    const char *p = style_attr;
    *prop_count = 0;

    while (*p && *prop_count < max_props) {
        while (*p == ' ' || *p == ';') p++;
        if (!*p) break;

        /* Name */
        const char *name = p;
        while (*p && *p != ':' && *p != ';') p++;
        if (*p != ':') { if (*p) p++; continue; }
        const char *name_end = p;
        while (name_end > name && name_end[-1] == ' ') name_end--;
        p++; /* skip ':' */

        /* Value */
        while (*p == ' ') p++;
        const char *val = p;
        while (*p && *p != ';') p++;
        const char *val_end = p;
        while (val_end > val && val_end[-1] == ' ') val_end--;

        /* Store */
        {
            struct ts_css_property *prop = &props[*prop_count];
            size_t nl = (size_t)(name_end - name);
            size_t vl = (size_t)(val_end - val);
            if (nl >= sizeof(prop->name)) nl = sizeof(prop->name) - 1;
            if (vl >= sizeof(prop->value)) vl = sizeof(prop->value) - 1;
            { size_t i; for (i = 0; i < nl; i++) prop->name[i] = ts_css__lower(name[i]); prop->name[nl] = '\0'; }
            { size_t i; for (i = 0; i < vl; i++) prop->value[i] = val[i]; prop->value[vl] = '\0'; }
            (*prop_count)++;
        }
    }
}

/* ================================================================== */
/* Selector matching                                                   */
/* ================================================================== */

/*
 * ts_css_match_part — check if a single selector part matches an element.
 *
 * tag/cls/id are the element's tag name, class, and id attributes.
 * All comparisons are case-insensitive.
 */
static int ts_css_match_part(const struct ts_css_selector_part *part,
                              const char *tag, const char *class_attr,
                              const char *id_attr) {
    /* Tag match */
    if (part->tag[0]) {
        const char *a = part->tag;
        const char *b = tag;
        while (*a && *b) {
            if (ts_css__lower(*a) != ts_css__lower(*b)) return 0;
            a++; b++;
        }
        if (*a || *b) return 0;
    }

    /* ID match */
    if (part->id[0]) {
        if (!id_attr || !id_attr[0]) return 0;
        const char *a = part->id;
        const char *b = id_attr;
        while (*a && *b) {
            if (ts_css__lower(*a) != ts_css__lower(*b)) return 0;
            a++; b++;
        }
        if (*a || *b) return 0;
    }

    /* Class match (space-separated class list) */
    if (part->cls[0]) {
        if (!class_attr || !class_attr[0]) return 0;
        const char *p = class_attr;
        size_t cls_len = 0;
        { const char *c = part->cls; while (*c) { cls_len++; c++; } }
        int found = 0;
        while (*p) {
            while (*p == ' ') p++;
            const char *word = p;
            while (*p && *p != ' ') p++;
            size_t wlen = (size_t)(p - word);
            if (wlen == cls_len) {
                int match = 1;
                size_t i;
                for (i = 0; i < wlen; i++) {
                    if (ts_css__lower(word[i]) != part->cls[i]) { match = 0; break; }
                }
                if (match) { found = 1; break; }
            }
        }
        if (!found) return 0;
    }

    return 1; /* all specified parts match */
}

/*
 * ts_css_find_property — find a property by name in a property array.
 * Returns pointer to value or NULL.
 */
static const char *ts_css_find_property(const struct ts_css_property *props,
                                         int count, const char *name) {
    int i;
    for (i = 0; i < count; i++) {
        const char *a = props[i].name;
        const char *b = name;
        int match = 1;
        while (*a && *b) {
            if (*a != *b) { match = 0; break; }
            a++; b++;
        }
        if (match && *a == '\0' && *b == '\0')
            return props[i].value;
    }
    return NULL;
}

/* ================================================================== */
/* @import extraction                                                  */
/* ================================================================== */

#define TS_CSS_MAX_IMPORTS 8

struct ts_css_import_list {
    char urls[TS_CSS_MAX_IMPORTS][512];
    int count;
};

/*
 * ts_css_extract_imports — scan CSS for @import rules and extract URLs.
 *
 * Handles:
 *   @import url("style.css");
 *   @import url('style.css');
 *   @import url(style.css);
 *   @import "style.css";
 *   @import 'style.css';
 *
 * @import must appear before any rules (after optional @charset).
 * Stops scanning at the first non-@import, non-whitespace, non-comment token.
 */
static void ts_css_extract_imports(const char *css, size_t len,
                                    struct ts_css_import_list *imports) {
    const char *p = css;
    const char *end = css + len;

    imports->count = 0;

    while (p < end && imports->count < TS_CSS_MAX_IMPORTS) {
        /* Skip whitespace */
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            p++;
        if (p >= end) break;

        /* Skip CSS comments */
        if (p + 1 < end && p[0] == '/' && p[1] == '*') {
            p += 2;
            while (p + 1 < end && !(p[0] == '*' && p[1] == '/'))
                p++;
            if (p + 1 < end) p += 2;
            continue;
        }

        /* Skip @charset (it's valid before @import) */
        if (p + 8 < end && memcmp(p, "@charset", 8) == 0) {
            p += 8;
            while (p < end && *p != ';') p++;
            if (p < end) p++; /* skip ';' */
            continue;
        }

        /* Check for @import */
        if (p + 7 >= end || memcmp(p, "@import", 7) != 0)
            break; /* not an @import — stop scanning */

        p += 7;
        /* Skip whitespace after @import */
        while (p < end && (*p == ' ' || *p == '\t'))
            p++;
        if (p >= end) break;

        /* Extract URL */
        {
            const char *url_start = NULL;
            const char *url_end = NULL;

            if (p + 4 < end && memcmp(p, "url(", 4) == 0) {
                /* @import url(...) */
                p += 4;
                while (p < end && (*p == ' ' || *p == '\t')) p++;
                if (p < end && (*p == '"' || *p == '\'')) {
                    char q = *p++;
                    url_start = p;
                    while (p < end && *p != q) p++;
                    url_end = p;
                    if (p < end) p++; /* skip closing quote */
                } else {
                    /* url(bare-url) */
                    url_start = p;
                    while (p < end && *p != ')' && *p != ' ' && *p != '\t') p++;
                    url_end = p;
                }
                /* Skip to closing ')' */
                while (p < end && *p != ')') p++;
                if (p < end) p++;
            } else if (*p == '"' || *p == '\'') {
                /* @import "url" or @import 'url' */
                char q = *p++;
                url_start = p;
                while (p < end && *p != q) p++;
                url_end = p;
                if (p < end) p++; /* skip closing quote */
            }

            /* Store URL if found */
            if (url_start && url_end && url_end > url_start) {
                size_t ulen = (size_t)(url_end - url_start);
                if (ulen > 511) ulen = 511;
                memcpy(imports->urls[imports->count], url_start, ulen);
                imports->urls[imports->count][ulen] = '\0';
                imports->count++;
            }
        }

        /* Skip to ';' (end of @import statement) */
        while (p < end && *p != ';') p++;
        if (p < end) p++;
    }
}

#endif /* TS_CSS_H */
