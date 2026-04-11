/*
 * ts_html.h — TaterSurf zero-copy HTML tokenizer
 *
 * Header-only. Scans an HTML buffer in-place without allocating memory.
 * Each token is a pointer+length into the source buffer.
 *
 * Features:
 *   - Tag tokenization (open, close, self-closing)
 *   - Attribute extraction from tags
 *   - HTML entity decoding (&amp; &#NNN; &#xHH;)
 *   - Script/style content isolation
 *   - Comment and DOCTYPE recognition
 *   - CDATA section handling
 */

#ifndef TS_HTML_H
#define TS_HTML_H

#include <stddef.h>
#include <stdint.h>

/* ================================================================== */
/* Token types                                                         */
/* ================================================================== */

enum ts_token_type {
    TS_TOK_TEXT = 0,        /* raw text content between tags */
    TS_TOK_TAG_OPEN,        /* <tagname attr="val"> */
    TS_TOK_TAG_CLOSE,       /* </tagname> */
    TS_TOK_TAG_SELF_CLOSE,  /* <tagname ... /> */
    TS_TOK_COMMENT,         /* <!-- ... --> */
    TS_TOK_DOCTYPE,         /* <!DOCTYPE ...> */
    TS_TOK_CDATA,           /* <![CDATA[ ... ]]> */
    TS_TOK_EOF              /* end of input */
};

/* ================================================================== */
/* Token structure                                                     */
/* ================================================================== */

struct ts_token {
    enum ts_token_type type;
    const char *start;      /* pointer into source buffer */
    size_t len;             /* length of entire token text */
    /* For tag tokens: */
    const char *tag_name;   /* pointer to tag name start */
    size_t tag_name_len;    /* length of tag name */
};

/* ================================================================== */
/* Tokenizer state                                                     */
/* ================================================================== */

struct ts_tokenizer {
    const char *src;        /* source buffer (not owned) */
    size_t src_len;         /* source length */
    size_t pos;             /* current scan position */
    int inside_script;      /* 1 = inside <script>, skip until </script> */
    int inside_style;       /* 1 = inside <style>, skip until </style> */
};

/* ================================================================== */
/* Internal helpers                                                    */
/* ================================================================== */

static int ts_html__is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static int ts_html__is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int ts_html__is_alnum(char c) {
    return ts_html__is_alpha(c) || (c >= '0' && c <= '9');
}

static char ts_html__lower(char c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

static int ts_html__tag_name_eq(const char *name, size_t name_len,
                                 const char *match) {
    size_t i;
    for (i = 0; i < name_len && match[i]; i++) {
        if (ts_html__lower(name[i]) != match[i]) return 0;
    }
    return match[i] == '\0' && i == name_len;
}

/* Check if position starts with a string (case-insensitive) */
static int ts_html__starts_with_ci(const char *src, size_t remaining,
                                    const char *match) {
    size_t i;
    for (i = 0; match[i]; i++) {
        if (i >= remaining) return 0;
        if (ts_html__lower(src[i]) != ts_html__lower(match[i])) return 0;
    }
    return 1;
}

/* ================================================================== */
/* Public API                                                          */
/* ================================================================== */

/*
 * ts_tok_init — initialize tokenizer with HTML source buffer.
 * The tokenizer does NOT own the buffer. Caller must keep it alive.
 */
static void ts_tok_init(struct ts_tokenizer *tok,
                         const char *html, size_t len) {
    tok->src = html;
    tok->src_len = len;
    tok->pos = 0;
    tok->inside_script = 0;
    tok->inside_style = 0;
}

/*
 * ts_tok_next — advance to the next token.
 *
 * Returns 1 if a token was produced, 0 on EOF.
 * Token start/len point into the original source buffer.
 */
static int ts_tok_next(struct ts_tokenizer *tok, struct ts_token *out) {
    const char *src = tok->src;
    size_t len = tok->src_len;
    size_t pos = tok->pos;

    /* Zero output */
    out->type = TS_TOK_EOF;
    out->start = NULL;
    out->len = 0;
    out->tag_name = NULL;
    out->tag_name_len = 0;

    if (pos >= len) return 0;

    /* ---- Handle script/style content isolation ---- */
    if (tok->inside_script || tok->inside_style) {
        const char *end_tag = tok->inside_script ? "</script" : "</style";
        size_t end_tag_len = tok->inside_script ? 8 : 7;
        size_t start = pos;

        /* Scan for closing tag */
        while (pos < len) {
            if (src[pos] == '<' && pos + end_tag_len < len &&
                ts_html__starts_with_ci(src + pos, len - pos, end_tag)) {
                /* Found closing tag — emit content as TEXT */
                if (pos > start) {
                    out->type = TS_TOK_TEXT;
                    out->start = src + start;
                    out->len = pos - start;
                    tok->pos = pos;
                    return 1;
                }
                /* Now let the tag parser handle the closing tag */
                tok->inside_script = 0;
                tok->inside_style = 0;
                break;
            }
            pos++;
        }

        if (pos >= len && pos > start) {
            /* Unterminated script/style — emit rest as TEXT */
            out->type = TS_TOK_TEXT;
            out->start = src + start;
            out->len = pos - start;
            tok->pos = pos;
            tok->inside_script = 0;
            tok->inside_style = 0;
            return 1;
        }

        if (pos >= len) {
            tok->pos = pos;
            return 0;
        }
    }

    /* ---- Main tokenization ---- */
    if (src[pos] == '<') {
        size_t tag_start = pos;

        /* Check what follows '<' */
        if (pos + 1 >= len) {
            /* Lone '<' at end — emit as text */
            out->type = TS_TOK_TEXT;
            out->start = src + pos;
            out->len = 1;
            tok->pos = pos + 1;
            return 1;
        }

        /* ---- Comment: <!-- ... --> ---- */
        if (pos + 3 < len && src[pos+1] == '!' &&
            src[pos+2] == '-' && src[pos+3] == '-') {
            size_t end = pos + 4;
            while (end + 2 < len) {
                if (src[end] == '-' && src[end+1] == '-' && src[end+2] == '>') {
                    end += 3;
                    break;
                }
                end++;
            }
            if (end + 2 >= len && !(src[end-1] == '>' && src[end-2] == '-' && src[end-3] == '-'))
                end = len; /* unterminated comment */
            out->type = TS_TOK_COMMENT;
            out->start = src + tag_start;
            out->len = end - tag_start;
            tok->pos = end;
            return 1;
        }

        /* ---- CDATA: <![CDATA[ ... ]]> ---- */
        if (pos + 8 < len &&
            ts_html__starts_with_ci(src + pos, len - pos, "<![cdata[")) {
            size_t end = pos + 9;
            while (end + 2 < len) {
                if (src[end] == ']' && src[end+1] == ']' && src[end+2] == '>') {
                    end += 3;
                    break;
                }
                end++;
            }
            if (end >= len) end = len;
            out->type = TS_TOK_CDATA;
            out->start = src + pos + 9; /* content after <![CDATA[ */
            out->len = (end >= 3 && src[end-1] == '>' && src[end-2] == ']')
                        ? (end - 3 - (pos + 9)) : (end - pos - 9);
            tok->pos = end;
            return 1;
        }

        /* ---- DOCTYPE: <!DOCTYPE ...> ---- */
        if (pos + 9 < len && src[pos+1] == '!' &&
            ts_html__starts_with_ci(src + pos + 2, len - pos - 2, "doctype")) {
            size_t end = pos + 2;
            while (end < len && src[end] != '>') end++;
            if (end < len) end++; /* include '>' */
            out->type = TS_TOK_DOCTYPE;
            out->start = src + tag_start;
            out->len = end - tag_start;
            tok->pos = end;
            return 1;
        }

        /* ---- Processing instruction: <?...?> ---- */
        if (pos + 1 < len && src[pos+1] == '?') {
            size_t end = pos + 2;
            while (end + 1 < len) {
                if (src[end] == '?' && src[end+1] == '>') {
                    end += 2;
                    break;
                }
                end++;
            }
            if (end >= len) end = len;
            /* Skip PI — don't emit a token, recurse */
            tok->pos = end;
            return ts_tok_next(tok, out);
        }

        /* ---- Closing tag: </tagname> ---- */
        if (pos + 1 < len && src[pos+1] == '/') {
            size_t name_start = pos + 2;
            size_t name_end = name_start;
            size_t end;

            /* Skip whitespace after </ */
            while (name_start < len && ts_html__is_space(src[name_start]))
                name_start++;

            name_end = name_start;
            while (name_end < len && (ts_html__is_alnum(src[name_end]) ||
                                       src[name_end] == '-' || src[name_end] == '_'))
                name_end++;

            /* Find closing '>' */
            end = name_end;
            while (end < len && src[end] != '>') end++;
            if (end < len) end++; /* include '>' */

            out->type = TS_TOK_TAG_CLOSE;
            out->start = src + tag_start;
            out->len = end - tag_start;
            out->tag_name = src + name_start;
            out->tag_name_len = name_end - name_start;
            tok->pos = end;
            return 1;
        }

        /* ---- Opening tag: <tagname ...> or <tagname .../> ---- */
        {
            size_t name_start = pos + 1;
            size_t name_end;
            size_t end;
            int self_close = 0;

            /* Tag name */
            name_end = name_start;
            while (name_end < len && (ts_html__is_alnum(src[name_end]) ||
                                       src[name_end] == '-' || src[name_end] == '_' ||
                                       src[name_end] == ':'))
                name_end++;

            if (name_end == name_start) {
                /* '<' not followed by valid tag name — emit as text */
                out->type = TS_TOK_TEXT;
                out->start = src + pos;
                out->len = 1;
                tok->pos = pos + 1;
                return 1;
            }

            /* Find closing '>' — handle quoted attributes */
            end = name_end;
            {
                int in_sq = 0; /* single quote */
                int in_dq = 0; /* double quote */
                while (end < len) {
                    char c = src[end];
                    if (c == '\'' && !in_dq) in_sq = !in_sq;
                    else if (c == '"' && !in_sq) in_dq = !in_dq;
                    else if (c == '>' && !in_sq && !in_dq) {
                        /* Check for self-closing /> */
                        if (end > 0 && src[end - 1] == '/')
                            self_close = 1;
                        end++; /* include '>' */
                        break;
                    }
                    end++;
                }
            }
            if (end >= len && (end == 0 || src[end-1] != '>'))
                end = len; /* unterminated tag */

            out->type = self_close ? TS_TOK_TAG_SELF_CLOSE : TS_TOK_TAG_OPEN;
            out->start = src + tag_start;
            out->len = end - tag_start;
            out->tag_name = src + name_start;
            out->tag_name_len = name_end - name_start;
            tok->pos = end;

            /* Check for script/style — set isolation flag */
            if (out->type == TS_TOK_TAG_OPEN) {
                if (ts_html__tag_name_eq(out->tag_name, out->tag_name_len, "script"))
                    tok->inside_script = 1;
                else if (ts_html__tag_name_eq(out->tag_name, out->tag_name_len, "style"))
                    tok->inside_style = 1;
            }

            return 1;
        }
    }

    /* ---- Text content ---- */
    {
        size_t start = pos;
        while (pos < len && src[pos] != '<') pos++;

        out->type = TS_TOK_TEXT;
        out->start = src + start;
        out->len = pos - start;
        tok->pos = pos;
        return 1;
    }
}

/* ================================================================== */
/* Attribute extraction                                                */
/* ================================================================== */

/*
 * ts_tok_attr_get — extract an attribute value from a tag token.
 *
 * Scans the tag content for attr_name="value" or attr_name='value'
 * or attr_name=value (unquoted).
 *
 * Returns length of value written to value_buf, or -1 if not found.
 * Attribute names are matched case-insensitively.
 */
static int ts_tok_attr_get(const struct ts_token *tag,
                            const char *attr_name,
                            char *value_buf, size_t value_max) {
    const char *p;
    const char *end;
    size_t attr_len;

    if (!tag || !attr_name || !value_buf || value_max == 0) return -1;
    if (tag->type != TS_TOK_TAG_OPEN && tag->type != TS_TOK_TAG_SELF_CLOSE)
        return -1;

    value_buf[0] = '\0';
    attr_len = 0;
    { const char *a = attr_name; while (*a) { attr_len++; a++; } }

    /* Start scanning after tag name */
    p = tag->tag_name + tag->tag_name_len;
    end = tag->start + tag->len;

    while (p < end) {
        const char *attr_start;
        size_t this_len;

        /* Skip whitespace */
        while (p < end && ts_html__is_space(*p)) p++;
        if (p >= end || *p == '>' || *p == '/') break;

        /* Read attribute name */
        attr_start = p;
        while (p < end && !ts_html__is_space(*p) && *p != '=' && *p != '>' && *p != '/')
            p++;
        this_len = (size_t)(p - attr_start);

        /* Skip whitespace around '=' */
        while (p < end && ts_html__is_space(*p)) p++;

        if (p < end && *p == '=') {
            p++; /* skip '=' */
            while (p < end && ts_html__is_space(*p)) p++;

            /* Read attribute value */
            if (p < end && (*p == '"' || *p == '\'')) {
                /* Quoted value */
                char quote = *p++;
                const char *val_start = p;
                while (p < end && *p != quote) p++;
                {
                    size_t vlen = (size_t)(p - val_start);

                    /* Check if this is the attribute we're looking for */
                    if (this_len == attr_len) {
                        int match = 1;
                        size_t i;
                        for (i = 0; i < this_len; i++) {
                            if (ts_html__lower(attr_start[i]) != ts_html__lower(attr_name[i])) {
                                match = 0;
                                break;
                            }
                        }
                        if (match) {
                            if (vlen >= value_max) vlen = value_max - 1;
                            {
                                size_t j;
                                for (j = 0; j < vlen; j++) value_buf[j] = val_start[j];
                            }
                            value_buf[vlen] = '\0';
                            return (int)vlen;
                        }
                    }
                }
                if (p < end) p++; /* skip closing quote */
            } else {
                /* Unquoted value */
                const char *val_start = p;
                while (p < end && !ts_html__is_space(*p) && *p != '>' && *p != '/')
                    p++;
                {
                    size_t vlen = (size_t)(p - val_start);

                    if (this_len == attr_len) {
                        int match = 1;
                        size_t i;
                        for (i = 0; i < this_len; i++) {
                            if (ts_html__lower(attr_start[i]) != ts_html__lower(attr_name[i])) {
                                match = 0;
                                break;
                            }
                        }
                        if (match) {
                            if (vlen >= value_max) vlen = value_max - 1;
                            {
                                size_t j;
                                for (j = 0; j < vlen; j++) value_buf[j] = val_start[j];
                            }
                            value_buf[vlen] = '\0';
                            return (int)vlen;
                        }
                    }
                }
            }
        } else {
            /* Boolean attribute (no value) — check if it matches */
            if (this_len == attr_len) {
                int match = 1;
                size_t i;
                for (i = 0; i < this_len; i++) {
                    if (ts_html__lower(attr_start[i]) != ts_html__lower(attr_name[i])) {
                        match = 0;
                        break;
                    }
                }
                if (match) {
                    value_buf[0] = '\0';
                    return 0; /* found, empty value (boolean attr) */
                }
            }
        }
    }

    return -1; /* not found */
}

/*
 * ts_tok_has_attr — check if a tag has an attribute (any value).
 */
static int ts_tok_has_attr(const struct ts_token *tag, const char *attr_name) {
    char tmp[1];
    return ts_tok_attr_get(tag, attr_name, tmp, sizeof(tmp)) >= 0;
}

/* ================================================================== */
/* HTML entity decoding                                                */
/* ================================================================== */

/*
 * ts_decode_entities — decode HTML entities in-place.
 *
 * Handles:
 *   &amp; &lt; &gt; &quot; &apos; &nbsp;
 *   &#NNN; (decimal codepoint)
 *   &#xHH; (hex codepoint)
 *   Unknown entities: passed through as-is.
 *
 * Writes decoded text to dst buffer. Returns bytes written.
 */
static size_t ts_decode_entities(const char *src, size_t src_len,
                                  char *dst, size_t dst_max) {
    size_t si = 0;
    size_t di = 0;

    while (si < src_len && di < dst_max - 1) {
        if (src[si] != '&') {
            dst[di++] = src[si++];
            continue;
        }

        /* Found '&' — try to decode entity */
        {
            size_t ent_start = si;
            si++; /* skip '&' */

            if (si < src_len && src[si] == '#') {
                /* Numeric entity */
                si++; /* skip '#' */
                uint32_t codepoint = 0;

                if (si < src_len && (src[si] == 'x' || src[si] == 'X')) {
                    /* Hex: &#xHHHH; */
                    si++;
                    while (si < src_len && src[si] != ';') {
                        char c = src[si];
                        if (c >= '0' && c <= '9') codepoint = codepoint * 16 + (uint32_t)(c - '0');
                        else if (c >= 'a' && c <= 'f') codepoint = codepoint * 16 + (uint32_t)(c - 'a' + 10);
                        else if (c >= 'A' && c <= 'F') codepoint = codepoint * 16 + (uint32_t)(c - 'A' + 10);
                        else break;
                        si++;
                    }
                } else {
                    /* Decimal: &#NNNN; */
                    while (si < src_len && src[si] != ';') {
                        char c = src[si];
                        if (c >= '0' && c <= '9') codepoint = codepoint * 10 + (uint32_t)(c - '0');
                        else break;
                        si++;
                    }
                }
                if (si < src_len && src[si] == ';') si++; /* skip ';' */

                /* Emit as UTF-8 (or ASCII for low codepoints) */
                if (codepoint < 0x80) {
                    if (codepoint == 0) codepoint = 0xFFFD; /* null → replacement */
                    dst[di++] = (char)codepoint;
                } else if (codepoint < 0x800 && di + 1 < dst_max - 1) {
                    dst[di++] = (char)(0xC0 | (codepoint >> 6));
                    dst[di++] = (char)(0x80 | (codepoint & 0x3F));
                } else if (codepoint < 0x10000 && di + 2 < dst_max - 1) {
                    dst[di++] = (char)(0xE0 | (codepoint >> 12));
                    dst[di++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
                    dst[di++] = (char)(0x80 | (codepoint & 0x3F));
                } else if (di + 3 < dst_max - 1) {
                    dst[di++] = (char)(0xF0 | (codepoint >> 18));
                    dst[di++] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
                    dst[di++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
                    dst[di++] = (char)(0x80 | (codepoint & 0x3F));
                }
                continue;
            }

            /* Named entity */
            {
                const char *name = src + si;
                size_t nlen = 0;
                while (si + nlen < src_len && src[si + nlen] != ';' && nlen < 10)
                    nlen++;
                if (si + nlen < src_len && src[si + nlen] == ';') {
                    /* Match known entities */
                    char decoded = 0;
                    int matched = 0;

                    if (nlen == 3 && name[0] == 'a' && name[1] == 'm' && name[2] == 'p')
                        { decoded = '&'; matched = 1; }
                    else if (nlen == 2 && name[0] == 'l' && name[1] == 't')
                        { decoded = '<'; matched = 1; }
                    else if (nlen == 2 && name[0] == 'g' && name[1] == 't')
                        { decoded = '>'; matched = 1; }
                    else if (nlen == 4 && name[0] == 'q' && name[1] == 'u' &&
                             name[2] == 'o' && name[3] == 't')
                        { decoded = '"'; matched = 1; }
                    else if (nlen == 4 && name[0] == 'a' && name[1] == 'p' &&
                             name[2] == 'o' && name[3] == 's')
                        { decoded = '\''; matched = 1; }
                    else if (nlen == 4 && name[0] == 'n' && name[1] == 'b' &&
                             name[2] == 's' && name[3] == 'p')
                        { decoded = ' '; matched = 1; }
                    else if (nlen == 5 && name[0] == 'e' && name[1] == 'n' &&
                             name[2] == 's' && name[3] == 'p' && name[4] == ';')
                        { decoded = ' '; matched = 1; } /* thin space → space */
                    else if (nlen == 5 && name[0] == 'e' && name[1] == 'm' &&
                             name[2] == 's' && name[3] == 'p' && name[4] == ';')
                        { decoded = ' '; matched = 1; } /* em space → space */
                    else if (nlen == 4 && name[0] == 'c' && name[1] == 'o' &&
                             name[2] == 'p' && name[3] == 'y')
                        { decoded = '\xC2'; matched = 2; } /* © = C2 A9 UTF-8 */
                    else if (nlen == 5 && name[0] == 'm' && name[1] == 'd' &&
                             name[2] == 'a' && name[3] == 's' && name[4] == 'h')
                        { decoded = '-'; matched = 1; } /* mdash → dash */
                    else if (nlen == 5 && name[0] == 'n' && name[1] == 'd' &&
                             name[2] == 'a' && name[3] == 's' && name[4] == 'h')
                        { decoded = '-'; matched = 1; } /* ndash → dash */

                    if (matched == 1) {
                        dst[di++] = decoded;
                        si += nlen + 1; /* skip name + ';' */
                        continue;
                    } else if (matched == 2) {
                        /* Multi-byte UTF-8 entity (©) */
                        if (di + 1 < dst_max - 1) {
                            dst[di++] = (char)0xC2;
                            dst[di++] = (char)0xA9;
                        }
                        si += nlen + 1;
                        continue;
                    }
                }
            }

            /* Unknown entity — pass through as-is */
            si = ent_start;
            dst[di++] = src[si++];
        }
    }

    dst[di] = '\0';
    return di;
}

/* ================================================================== */
/* Utility: check if a tag is a void element (self-closing by spec)    */
/* ================================================================== */

static int ts_html_is_void_element(const char *name, size_t name_len) {
    /* HTML void elements that don't have closing tags */
    static const char *voids[] = {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr", NULL
    };
    int i;
    for (i = 0; voids[i]; i++) {
        if (ts_html__tag_name_eq(name, name_len, voids[i]))
            return 1;
    }
    return 0;
}

/* ================================================================== */
/* Utility: check if tag is a block-level element                      */
/* ================================================================== */

static int ts_html_is_block(const char *name, size_t name_len) {
    static const char *blocks[] = {
        "address", "article", "aside", "blockquote", "canvas", "dd",
        "details", "dialog", "div", "dl", "dt", "fieldset", "figcaption",
        "figure", "footer", "form", "h1", "h2", "h3", "h4", "h5", "h6",
        "header", "hgroup", "hr", "li", "main", "nav", "noscript", "ol",
        "output", "p", "pre", "section", "summary", "table", "tbody",
        "td", "tfoot", "th", "thead", "tr", "ul", "video", NULL
    };
    int i;
    for (i = 0; blocks[i]; i++) {
        if (ts_html__tag_name_eq(name, name_len, blocks[i]))
            return 1;
    }
    return 0;
}

/* ================================================================== */
/* Utility: check if tag is an inline element                          */
/* ================================================================== */

static int ts_html_is_inline(const char *name, size_t name_len) {
    static const char *inlines[] = {
        "a", "abbr", "acronym", "b", "bdo", "big", "br", "button",
        "cite", "code", "dfn", "em", "i", "img", "input", "kbd",
        "label", "map", "object", "output", "q", "samp", "select",
        "small", "span", "strong", "sub", "sup", "textarea", "time",
        "tt", "u", "var", NULL
    };
    int i;
    for (i = 0; inlines[i]; i++) {
        if (ts_html__tag_name_eq(name, name_len, inlines[i]))
            return 1;
    }
    return 0;
}

#endif /* TS_HTML_H */
