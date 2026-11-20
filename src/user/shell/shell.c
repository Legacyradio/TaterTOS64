/*
 * shell.c — TaterTOS System Shell
 */

#include "libc.h"
#include "fry.h"
#include "gfx.h"
#include "taterwin.h"
/* fry1827: FRY_PATH_MAX for resolve_program()'s path buffers. shell.c reached
 * neither fry_limits.h nor it transitively through the headers above. */
#include <fry_limits.h>

/* Standard headers for POSIX types */
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE 1024
#define MAX_ARGS 64

static char cwd[256] = "/";

#define TERM_W 960
#define TERM_H 576
#define TERM_COLS 120
#define TERM_ROWS 36
#define TERM_CHAR_W 8
#define TERM_CHAR_H 16

static uint32_t *term_pixels;
static gfx_ctx_t term_gfx;
static char term_rows[TERM_ROWS][TERM_COLS + 1];
static int term_row;
static int term_col;
static int term_ready;
static int term_surf_w = TERM_W;   /* live window surface size; changes on resize */
static int term_surf_h = TERM_H;
static char input_line[MAX_LINE];
static int input_pos;
#define INPUT_STREAM_MAX 512
static uint8_t input_stream[INPUT_STREAM_MAX];
static int input_stream_len;

/* ---- VT/ANSI terminal parser state (C1) ---- */
enum { TS_NORMAL, TS_ESC, TS_CSI, TS_OSC, TS_OSC_ST, TS_CHARSET };
static int term_state = TS_NORMAL;
#define TERM_MAXPARAM 16
static int term_params[TERM_MAXPARAM];
static int term_nparam;
static int term_priv;                                /* '?' private-mode prefix seen */
static int term_cursor_visible = 1;
static int term_saved_row, term_saved_col;
static char term_savebuf[TERM_ROWS][TERM_COLS + 1];  /* main screen saved while in alt */
static uint32_t term_savefg[TERM_ROWS][TERM_COLS];
static uint32_t term_savebg[TERM_ROWS][TERM_COLS];
static int term_in_alt;
static int term_alt_save_row, term_alt_save_col;
static uint32_t term_child_pid;                      /* target pid for DSR cursor reports */
static uint32_t term_u8_cp;                          /* fry1644: UTF-8 multibyte accumulator */
static int term_u8_need, term_u8_got;

/* fry1644: per-cell color attributes (ANSI SGR). uint32_t RGB so 16-color,
 * 256-color and truecolor all resolve to real pixel colors. */
#define TERM_FG_DEFAULT 0xD0D0D0u
#define TERM_BG_DEFAULT 0x101010u
static uint32_t term_fg[TERM_ROWS][TERM_COLS];
static uint32_t term_bg[TERM_ROWS][TERM_COLS];
static uint32_t term_cur_fg = TERM_FG_DEFAULT;
static uint32_t term_cur_bg = TERM_BG_DEFAULT;
static int term_bold;
static int term_reverse;
static const uint32_t term_ansi[16] = {
    0x303030, 0xE05050, 0x50C050, 0xD0B040, 0x5088E0, 0xC060C0, 0x40C0C0, 0xC8C8C8,  /* normal */
    0x808080, 0xFF7060, 0x70E070, 0xF0D860, 0x70A8FF, 0xE080E0, 0x60E0E0, 0xFFFFFF   /* bright */
};
static uint32_t term_xterm256(int n) {
    if (n < 16) return term_ansi[n & 15];
    if (n >= 232) { uint32_t g = 8 + (uint32_t)(n - 232) * 10; return (g << 16) | (g << 8) | g; }
    static const int cv[6] = {0, 95, 135, 175, 215, 255};
    n -= 16;
    return ((uint32_t)cv[(n / 36) % 6] << 16) | ((uint32_t)cv[(n / 6) % 6] << 8) | (uint32_t)cv[n % 6];
}

static uint32_t term_eff_fg(void) {
    return term_reverse ? term_cur_bg : term_cur_fg;
}

static uint32_t term_eff_bg(void) {
    return term_reverse ? term_cur_fg : term_cur_bg;
}

static void term_blank_cell(int r, int c) {
    term_rows[r][c] = ' ';
    term_fg[r][c] = term_eff_fg();
    term_bg[r][c] = term_eff_bg();
}

static void term_copy_row(int dst, int src) {
    memcpy(term_rows[dst], term_rows[src], TERM_COLS + 1);
    memcpy(term_fg[dst], term_fg[src], sizeof(term_fg[0]));
    memcpy(term_bg[dst], term_bg[src], sizeof(term_bg[0]));
}

static void term_blank_row(int r) {
    memset(term_rows[r], ' ', TERM_COLS);
    term_rows[r][TERM_COLS] = 0;
    for (int c = 0; c < TERM_COLS; c++) term_blank_cell(r, c);
}

static void execute_command(int argc, char **argv);

static void term_clear(void) {
    for (int r = 0; r < TERM_ROWS; r++) {
        memset(term_rows[r], ' ', TERM_COLS);
        term_rows[r][TERM_COLS] = 0;
        for (int c = 0; c < TERM_COLS; c++) {
            term_fg[r][c] = TERM_FG_DEFAULT;
            term_bg[r][c] = TERM_BG_DEFAULT;
        }
    }
    term_row = 0;
    term_col = 0;
}

static void term_scroll(void) {
    for (int r = 0; r + 1 < TERM_ROWS; r++) {
        term_copy_row(r, r + 1);
    }
    term_blank_row(TERM_ROWS - 1);
    term_row = TERM_ROWS - 1;
}

static void term_putchar_raw(int c) {
    if (c == '\r') {
        term_col = 0;
        return;
    }
    if (c == '\n') {
        term_col = 0;
        term_row++;
        if (term_row >= TERM_ROWS) term_scroll();
        return;
    }
    if (c == '\b') {
        if (term_col > 0) {
            term_col--;
            term_blank_cell(term_row, term_col);
        }
        return;
    }
    if (c == '\t') {
        do {
            term_putchar_raw(' ');
        } while ((term_col & 3) != 0);
        return;
    }
    if ((unsigned char)c < 0x20) return;
    term_rows[term_row][term_col] = (char)c;
    term_fg[term_row][term_col] = term_eff_fg();
    term_bg[term_row][term_col] = term_eff_bg();
    term_col++;
    if (term_col >= TERM_COLS) {
        term_col = 0;
        term_row++;
        if (term_row >= TERM_ROWS) term_scroll();
    }
}

static void term_puts_raw(const char *s) {
    if (!s) s = "(null)";
    while (*s) term_putchar_raw((unsigned char)*s++);
}

static int term_puts_line(const char *s) {
    term_puts_raw(s);
    term_putchar_raw('\n');
    return s ? (int)strlen(s) : 6;
}

static int term_putchar(int c) {
    term_putchar_raw(c);
    return c;
}

static int term_printf(const char *fmt, ...) {
    va_list ap;
    char buf[1024];
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return n;
    term_puts_raw(buf);
    return n;
}

static void term_render(void) {
    if (!term_ready || !term_pixels) return;
    gfx_fill(&term_gfx, 0, 0, (uint32_t)term_surf_w, (uint32_t)term_surf_h, TERM_BG_DEFAULT);
    for (int r = 0; r < TERM_ROWS; r++) {
        for (int c = 0; c < TERM_COLS; c++) {
            unsigned char ch = (unsigned char)term_rows[r][c];
            uint32_t bg = term_bg[r][c];
            if (ch == ' ' || ch == 0) {
                if (bg != TERM_BG_DEFAULT) {
                    gfx_fill(&term_gfx, 8 + (uint32_t)(c * TERM_CHAR_W),
                             (uint32_t)(8 + r * TERM_CHAR_H),
                             TERM_CHAR_W, TERM_CHAR_H, bg);
                }
                continue;
            }
            gfx_draw_char(&term_gfx, 8 + (uint32_t)(c * TERM_CHAR_W),
                          (uint32_t)(8 + r * TERM_CHAR_H),
                          (char)ch, term_fg[r][c], bg);
        }
    }
    if (term_cursor_visible) {
        gfx_fill(&term_gfx,
                 8 + (uint32_t)(term_col * TERM_CHAR_W),
                 8 + (uint32_t)(term_row * TERM_CHAR_H),
                 TERM_CHAR_W, TERM_CHAR_H, 0xD0D0D0);
    }
    tw_msg_header_t upd;
    upd.type = TW_MSG_UPDATE;
    upd.magic = TW_MAGIC;
    fry_write(1, &upd, sizeof(upd));
}

/* ---- VT/ANSI parser (C1): cursor, erase, alt-screen, DSR, mode swallow ---- */

static int term_utoa(int v, char *out) {
    char d[8];
    int dl = 0;
    if (v <= 0) { out[0] = '0'; return 1; }
    while (v && dl < 8) { d[dl++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < dl; i++) out[i] = d[dl - 1 - i];
    return dl;
}

static void term_clamp(void) {
    if (term_row < 0) term_row = 0;
    if (term_row >= TERM_ROWS) term_row = TERM_ROWS - 1;
    if (term_col < 0) term_col = 0;
    if (term_col >= TERM_COLS) term_col = TERM_COLS - 1;
}

static void term_clear_region(int r0, int c0, int r1, int c1) {
    for (int r = r0; r <= r1 && r < TERM_ROWS; r++) {
        if (r < 0) continue;
        int cs = (r == r0) ? c0 : 0;
        int ce = (r == r1) ? c1 : TERM_COLS - 1;
        if (cs < 0) cs = 0;
        for (int c = cs; c <= ce && c < TERM_COLS; c++) term_blank_cell(r, c);
    }
}

static void term_csi_dispatch(int f) {
    int p0 = term_params[0];
    int p1 = (term_nparam > 1) ? term_params[1] : 0;
    int n  = (p0 > 0) ? p0 : 1;

    if (term_priv) {                       /* DEC private modes: 'h' set / 'l' reset */
        if (f == 'h' || f == 'l') {
            int set = (f == 'h');
            if (p0 == 25) {
                term_cursor_visible = set;
            } else if (p0 == 47 || p0 == 1047 || p0 == 1049) {
                if (set && !term_in_alt) {
                    for (int r = 0; r < TERM_ROWS; r++) {
                        memcpy(term_savebuf[r], term_rows[r], TERM_COLS + 1);
                        memcpy(term_savefg[r], term_fg[r], sizeof(term_fg[0]));
                        memcpy(term_savebg[r], term_bg[r], sizeof(term_bg[0]));
                    }
                    term_alt_save_row = term_row;
                    term_alt_save_col = term_col;
                    term_clear();
                    term_in_alt = 1;
                } else if (!set && term_in_alt) {
                    for (int r = 0; r < TERM_ROWS; r++) {
                        memcpy(term_rows[r], term_savebuf[r], TERM_COLS + 1);
                        memcpy(term_fg[r], term_savefg[r], sizeof(term_fg[0]));
                        memcpy(term_bg[r], term_savebg[r], sizeof(term_bg[0]));
                    }
                    term_row = term_alt_save_row;
                    term_col = term_alt_save_col;
                    term_clamp();
                    term_in_alt = 0;
                }
            }
            /* other private modes (2004 paste, 1000/1006 mouse, 2026 sync, ...) ignored */
        }
        return;
    }

    switch (f) {
    case 'A': term_row -= n; term_clamp(); break;
    case 'B': term_row += n; term_clamp(); break;
    case 'C': case 'a': term_col += n; term_clamp(); break;
    case 'D': term_col -= n; term_clamp(); break;
    case 'E': case 'e': term_row += n; term_col = 0; term_clamp(); break;
    case 'F': term_row -= n; term_col = 0; term_clamp(); break;
    case 'G': term_col = (p0 > 0 ? p0 : 1) - 1; term_clamp(); break;
    case 'd': term_row = (p0 > 0 ? p0 : 1) - 1; term_clamp(); break;
    case 'H': case 'f':
        term_row = (p0 > 0 ? p0 : 1) - 1;
        term_col = (p1 > 0 ? p1 : 1) - 1;
        term_clamp();
        break;
    case 'J':
        if (p0 == 0)      term_clear_region(term_row, term_col, TERM_ROWS - 1, TERM_COLS - 1);
        else if (p0 == 1) term_clear_region(0, 0, term_row, term_col);
        else              term_clear_region(0, 0, TERM_ROWS - 1, TERM_COLS - 1);
        break;
    case 'K':
        if (p0 == 0)      term_clear_region(term_row, term_col, term_row, TERM_COLS - 1);
        else if (p0 == 1) term_clear_region(term_row, 0, term_row, term_col);
        else              term_clear_region(term_row, 0, term_row, TERM_COLS - 1);
        break;
    case 'X':
        for (int i = 0; i < n && term_col + i < TERM_COLS; i++)
            term_blank_cell(term_row, term_col + i);
        break;
    case 'P':                              /* DCH: delete n chars, shift left */
        for (int c = term_col; c < TERM_COLS; c++) {
            int src = c + n;
            if (src < TERM_COLS) {
                term_rows[term_row][c] = term_rows[term_row][src];
                term_fg[term_row][c] = term_fg[term_row][src];
                term_bg[term_row][c] = term_bg[term_row][src];
            } else {
                term_blank_cell(term_row, c);
            }
        }
        break;
    case '@':                              /* ICH: insert n blanks, shift right */
        for (int c = TERM_COLS - 1; c >= term_col; c--) {
            int src = c - n;
            if (src >= term_col) {
                term_rows[term_row][c] = term_rows[term_row][src];
                term_fg[term_row][c] = term_fg[term_row][src];
                term_bg[term_row][c] = term_bg[term_row][src];
            } else {
                term_blank_cell(term_row, c);
            }
        }
        break;
    case 'L':                              /* IL: insert n blank lines at cursor row */
        if (n > TERM_ROWS - term_row) n = TERM_ROWS - term_row;
        for (int r = TERM_ROWS - 1; r >= term_row + n; r--) term_copy_row(r, r - n);
        for (int r = term_row; r < term_row + n; r++) term_blank_row(r);
        break;
    case 'M':                              /* DL: delete n lines at cursor row */
        if (n > TERM_ROWS - term_row) n = TERM_ROWS - term_row;
        for (int r = term_row; r + n < TERM_ROWS; r++) term_copy_row(r, r + n);
        for (int r = TERM_ROWS - n; r < TERM_ROWS; r++) term_blank_row(r);
        break;
    case 'S':                              /* SU: scroll up n full-screen lines */
        if (n > TERM_ROWS) n = TERM_ROWS;
        for (int r = 0; r + n < TERM_ROWS; r++) term_copy_row(r, r + n);
        for (int r = TERM_ROWS - n; r < TERM_ROWS; r++) term_blank_row(r);
        break;
    case 'T':                              /* SD: scroll down n full-screen lines */
        if (n > TERM_ROWS) n = TERM_ROWS;
        for (int r = TERM_ROWS - 1; r >= n; r--) term_copy_row(r, r - n);
        for (int r = 0; r < n; r++) term_blank_row(r);
        break;
    case 's': term_saved_row = term_row; term_saved_col = term_col; break;
    case 'u': term_row = term_saved_row; term_col = term_saved_col; term_clamp(); break;
    case 'n':                              /* DSR: 6 = report cursor position */
        if (p0 == 6 && term_child_pid) {
            char resp[24];
            int len = 0;
            resp[len++] = 0x1b;
            resp[len++] = '[';
            len += term_utoa(term_row + 1, resp + len);
            resp[len++] = ';';
            len += term_utoa(term_col + 1, resp + len);
            resp[len++] = 'R';
            fry_proc_input(term_child_pid, resp, (size_t)len);
        }
        break;
    case 'm': {                            /* SGR — foreground/background color + attrs */
        if (term_nparam == 0) {
            term_cur_fg = TERM_FG_DEFAULT;
            term_cur_bg = TERM_BG_DEFAULT;
            term_bold = 0;
            term_reverse = 0;
            break;
        }
        for (int i = 0; i < term_nparam; i++) {
            int v = term_params[i];
            if (v == 0)                  { term_cur_fg = TERM_FG_DEFAULT; term_cur_bg = TERM_BG_DEFAULT; term_bold = 0; term_reverse = 0; }
            else if (v == 1)             { term_bold = 1; }
            else if (v == 22)            { term_bold = 0; }
            else if (v == 7)             { term_reverse = 1; }
            else if (v == 27)            { term_reverse = 0; }
            else if (v == 39)            { term_cur_fg = TERM_FG_DEFAULT; }
            else if (v == 49)            { term_cur_bg = TERM_BG_DEFAULT; }
            else if (v >= 30 && v <= 37) { term_cur_fg = term_ansi[(v - 30) + (term_bold ? 8 : 0)]; }
            else if (v >= 40 && v <= 47) { term_cur_bg = term_ansi[v - 40]; }
            else if (v >= 90 && v <= 97) { term_cur_fg = term_ansi[(v - 90) + 8]; }
            else if (v >= 100 && v <= 107) { term_cur_bg = term_ansi[(v - 100) + 8]; }
            else if ((v == 38 || v == 48) && i + 1 < term_nparam) {
                uint32_t *dst = (v == 38) ? &term_cur_fg : &term_cur_bg;
                if (term_params[i + 1] == 5 && i + 2 < term_nparam) {
                    *dst = term_xterm256(term_params[i + 2]); i += 2;
                } else if (term_params[i + 1] == 2 && i + 4 < term_nparam) {
                    *dst = ((uint32_t)(term_params[i + 2] & 0xFF) << 16) |
                           ((uint32_t)(term_params[i + 3] & 0xFF) << 8) |
                           (uint32_t)(term_params[i + 4] & 0xFF);
                    i += 4;
                }
            }
        }
        break;
    }
    case 'r':                              /* DECSTBM scroll region — ignored in C1 */
    default:
        break;
    }
}

/* fry1644: map a Unicode codepoint to a renderable glyph index. ASCII passes
 * through; the box-drawing/block/symbol ranges Claude's TUI uses are folded onto
 * the 0x80+ glyphs added to the 8x16 font; everything else falls back to '?'. */
static unsigned char cp_to_glyph(uint32_t cp) {
    if (cp < 0x80) return (unsigned char)cp;
    switch (cp) {
    case 0x2500: case 0x2501: case 0x2550: return 0x80;             /* ─ ━ ═ horizontal */
    case 0x2502: case 0x2503: case 0x2551: return 0x81;             /* │ ┃ ║ vertical */
    case 0x250C: case 0x250F: case 0x2554: case 0x256D: return 0x82;/* ┌ ┏ ╔ ╭ */
    case 0x2510: case 0x2513: case 0x2557: case 0x256E: return 0x83;/* ┐ ┓ ╗ ╮ */
    case 0x2514: case 0x2517: case 0x255A: case 0x2570: return 0x84;/* └ ┗ ╚ ╰ */
    case 0x2518: case 0x251B: case 0x255D: case 0x256F: return 0x85;/* ┘ ┛ ╝ ╯ */
    case 0x251C: case 0x2523: case 0x2560: return 0x86;             /* ├ */
    case 0x2524: case 0x252B: case 0x2563: return 0x87;             /* ┤ */
    case 0x252C: case 0x2533: case 0x2566: return 0x88;             /* ┬ */
    case 0x2534: case 0x253B: case 0x2569: return 0x89;             /* ┴ */
    case 0x253C: case 0x254B: case 0x256C: return 0x8A;             /* ┼ */
    case 0x2588: case 0x2580: case 0x2584: case 0x258C: case 0x2590: return 0x8B; /* blocks */
    case 0x2591: case 0x2592: case 0x2593: return 0x8C;             /* ░▒▓ shades */
    case 0x2022: case 0x00B7: case 0x25CF: case 0x25AA: case 0x2219: return 0x8D; /* • · bullet */
    case 0x2026: return 0x8E;                                       /* … ellipsis */
    case 0x2713: case 0x2714: case 0x221A: return 0x8F;             /* ✓ check */
    case 0x2192: case 0x279C: case 0x2794: case 0x27A4: return 0x90;/* → arrow */
    case 0x2717: case 0x2718: case 0x2715: case 0x2716: case 0x2573: return 'x'; /* ✗ cross */
    case 0x2190: return '<';
    case 0x2191: return '^';
    case 0x2193: return 'v';
    case 0x2014: case 0x2013: return '-';                           /* dashes */
    case 0x2018: case 0x2019: return '\'';                          /* curly quotes */
    case 0x201C: case 0x201D: return '"';
    case 0x00A0: return ' ';                                        /* nbsp */
    default:
        if (cp >= 0x2800 && cp <= 0x28FF) return 0x91;              /* braille spinner */
        return '?';
    }
}

static void term_feed(int c) {
    switch (term_state) {
    case TS_NORMAL:
        /* fry1644: UTF-8 multibyte assembly. Escape/control bytes are all <0x80,
         * so they pass through untouched; only 0x80+ runs are folded to codepoints. */
        if (term_u8_need) {
            if ((c & 0xC0) == 0x80) {
                term_u8_cp = (term_u8_cp << 6) | (uint32_t)(c & 0x3F);
                if (++term_u8_got >= term_u8_need) {
                    term_putchar_raw(cp_to_glyph(term_u8_cp));
                    term_u8_need = 0;
                }
                return;
            }
            term_u8_need = 0;            /* malformed: drop partial, reprocess c */
        }
        if (c == 0x1b) { term_state = TS_ESC; return; }
        if ((unsigned)c < 0x80) { term_putchar_raw(c); return; }
        if ((c & 0xE0) == 0xC0) { term_u8_cp = c & 0x1F; term_u8_need = 2; term_u8_got = 1; return; }
        if ((c & 0xF0) == 0xE0) { term_u8_cp = c & 0x0F; term_u8_need = 3; term_u8_got = 1; return; }
        if ((c & 0xF8) == 0xF0) { term_u8_cp = c & 0x07; term_u8_need = 4; term_u8_got = 1; return; }
        term_putchar_raw('?');           /* stray continuation / invalid lead */
        return;
    case TS_ESC:
        switch (c) {
        case '[':
            term_state = TS_CSI;
            term_nparam = 0;
            term_priv = 0;
            for (int i = 0; i < TERM_MAXPARAM; i++) term_params[i] = 0;
            return;
        case ']': term_state = TS_OSC; return;
        case '(': case ')': case '*': case '+':
            term_state = TS_CHARSET; return;
        case '7': term_saved_row = term_row; term_saved_col = term_col; term_state = TS_NORMAL; return;
        case '8': term_row = term_saved_row; term_col = term_saved_col; term_clamp(); term_state = TS_NORMAL; return;
        case 'M': if (term_row > 0) term_row--; term_state = TS_NORMAL; return;
        default:  term_state = TS_NORMAL; return;   /* '=', '>', etc. ignored */
        }
    case TS_CHARSET:
        term_state = TS_NORMAL;
        return;
    case TS_OSC:
        if (c == 0x07) { term_state = TS_NORMAL; return; }   /* BEL ends OSC */
        if (c == 0x1b) { term_state = TS_OSC_ST; return; }   /* ESC \ (ST) */
        return;                                              /* swallow body */
    case TS_OSC_ST:
        term_state = TS_NORMAL;
        return;
    case TS_CSI:
        if (c == '?') { term_priv = 1; return; }
        if (c >= '0' && c <= '9') {
            if (term_nparam == 0) term_nparam = 1;
            term_params[term_nparam - 1] = term_params[term_nparam - 1] * 10 + (c - '0');
            return;
        }
        if (c == ';') {
            if (term_nparam == 0) term_nparam = 1;
            if (term_nparam < TERM_MAXPARAM) term_nparam++;
            return;
        }
        if (c >= 0x20 && c <= 0x2f) return;   /* intermediate bytes */
        term_csi_dispatch(c);                 /* final byte */
        term_state = TS_NORMAL;
        return;
    default:
        term_state = TS_NORMAL;
        return;
    }
}

static void print_prompt(void) {
    term_printf("%s> ", cwd);
}

static void handle_input_char(char c) {
    if (c == '\r') c = '\n';
    if (c == '\b' || c == 127) {
        if (input_pos > 0) {
            input_pos--;
            term_putchar_raw('\b');
        }
        return;
    }
    if (c == '\n') {
        char *argv[MAX_ARGS];
        char *p;
        int argc = 0;
        input_line[input_pos] = 0;
        term_putchar_raw('\n');
        p = input_line;
        while (*p && argc < MAX_ARGS) {
            while (*p == ' ') p++;
            if (!*p) break;
            argv[argc++] = p;
            while (*p && *p != ' ') p++;
            if (*p) *p++ = '\0';
        }
        execute_command(argc, argv);
        input_pos = 0;
        input_line[0] = 0;
        print_prompt();
        return;
    }
    if ((unsigned char)c >= 0x20 && input_pos + 1 < MAX_LINE) {
        input_line[input_pos++] = c;
        term_putchar_raw(c);
    }
}

static int tw_msg_size(uint32_t type) {
    switch (type) {
        case TW_MSG_CREATE_WINDOW:   return (int)sizeof(tw_msg_create_win_t);
        case TW_MSG_WINDOW_CREATED:  return (int)sizeof(tw_msg_win_created_t);
        case TW_MSG_MOUSE_EVENT:     return (int)sizeof(tw_msg_mouse_t);
        case TW_MSG_KEY_EVENT:       return (int)sizeof(tw_msg_key_t);
        case TW_MSG_UPDATE:          return (int)sizeof(tw_msg_header_t);
        case TW_MSG_RESIZE:          return (int)sizeof(tw_msg_resize_t);
        case TW_MSG_RESIZED:         return (int)sizeof(tw_msg_resized_t);
        case TW_MSG_WHEEL_EVENT:     return (int)sizeof(tw_msg_wheel_t);
        case TW_MSG_FOCUS_EVENT:     return (int)sizeof(tw_msg_focus_t);
        case TW_MSG_ENTER_LEAVE:     return (int)sizeof(tw_msg_enter_leave_t);
        case TW_MSG_CLOSE_REQUEST:   return (int)sizeof(tw_msg_close_request_t);
        case TW_MSG_CLIPBOARD_DATA:  return (int)sizeof(tw_msg_clipboard_data_t);
        case TW_MSG_MOUSE_MOVE:      return (int)sizeof(tw_msg_mouse_move_t);
        default:                     return (int)sizeof(tw_msg_header_t);
    }
}

static void input_consume(int n) {
    if (n <= 0 || n > input_stream_len) return;
    if (n < input_stream_len) {
        memmove(input_stream, input_stream + n, (size_t)(input_stream_len - n));
    }
    input_stream_len -= n;
}

static void input_append(const uint8_t *src, int n) {
    if (!src || n <= 0) return;
    if (n > INPUT_STREAM_MAX) {
        src += n - INPUT_STREAM_MAX;
        n = INPUT_STREAM_MAX;
    }
    if (input_stream_len + n > INPUT_STREAM_MAX) {
        input_consume(input_stream_len + n - INPUT_STREAM_MAX);
    }
    memcpy(input_stream + input_stream_len, src, (size_t)n);
    input_stream_len += n;
}

static int maybe_tw_prefix(void) {
    static const uint8_t magic_le[4] = {0x4E, 0x49, 0x57, 0x54};
    if (input_stream_len <= 0) return 0;
    if (input_stream[0] < 1 || input_stream[0] > TW_MSG_MOUSE_MOVE) return 0;
    if (input_stream_len >= 2 && input_stream[1] != 0) return 0;
    if (input_stream_len >= 3 && input_stream[2] != 0) return 0;
    if (input_stream_len >= 4 && input_stream[3] != 0) return 0;
    for (int i = 4; i < input_stream_len && i < 8; i++) {
        if (input_stream[i] != magic_le[i - 4]) return 0;
    }
    return 1;
}

/* Compositor resized our window: it allocated a NEW shared surface and sent us its
 * id/dimensions (TW_MSG_RESIZED). Map the new surface, repoint the gfx context to
 * it, and re-render. Without this the compositor shows the new (empty) buffer while
 * we keep drawing the old one -> black + garbage. */
static void term_handle_resize(int shm_id, int new_w, int new_h) {
    if (new_w <= 0 || new_h <= 0) return;
    long ptr = fry_shm_map(shm_id);
    if (ptr <= 0) return;
    term_pixels = (uint32_t *)(uintptr_t)ptr;
    term_surf_w = new_w;
    term_surf_h = new_h;
    gfx_init(&term_gfx, term_pixels, (uint32_t)new_w, (uint32_t)new_h, (uint32_t)new_w);
    if (term_row >= TERM_ROWS) term_row = TERM_ROWS - 1;
    if (term_col >= TERM_COLS) term_col = TERM_COLS - 1;
    term_render();
}

static int process_input_stream(uint32_t forward_pid) {
    int changed = 0;
    while (input_stream_len > 0) {
        if (input_stream_len >= (int)sizeof(tw_msg_header_t)) {
            tw_msg_header_t hdr;
            memcpy(&hdr, input_stream, sizeof(hdr));
            if (hdr.magic == TW_MAGIC) {
                int need = tw_msg_size(hdr.type);
                if (input_stream_len < need) break;
                if (hdr.type == TW_MSG_KEY_EVENT && need == (int)sizeof(tw_msg_key_t)) {
                    tw_msg_key_t key;
                    memcpy(&key, input_stream, sizeof(key));
                    if (key.flags == FRY_KEY_PRESSED) {
                        /* Forward on PRESSED even if ascii==0 for children.
                         * Force to term_child_pid if set (main UI takeover) to ensure pass-through.
                         * WHY (fry1543): previous changes got some bytes to child, but Shell was still
                         * consuming controls (beacons to pid=13) because some input paths called
                         * process_input_stream(0) even with child active, or the gate was limiting.
                         * When term_child_pid (alt-screen child), Shell must be pure pass-through;
                         * all pressed keys (including ascii=0) go to the child. Map Enter to \r for
                         * standard terminal submit. This stops Shell consumption and gives the TUI
                         * all keys so it can accept typing and Enter. */
                        uint32_t target = forward_pid ? forward_pid : term_child_pid;
                        char c = (char)key.ascii;
                        if (target) {
                            if (c == 0 || c == '\n' || c == '\r') c = '\r';  /* standardize Enter for TUI */
                            fry_proc_input(target, &c, 1);
                        } else if (key.ascii != 0) {
                            handle_input_char(c);
                            changed = 1;
                        }
                    }
                } else if (hdr.type == TW_MSG_RESIZED &&
                           need == (int)sizeof(tw_msg_resized_t)) {
                    tw_msg_resized_t rz;
                    memcpy(&rz, input_stream, sizeof(rz));
                    term_handle_resize(rz.shm_id, rz.new_w, rz.new_h);
                    changed = 1;
                }
                input_consume(need);
                continue;
            }
        }
        if (input_stream_len < (int)sizeof(tw_msg_header_t) && maybe_tw_prefix()) {
            break;
        }
        uint32_t target = forward_pid ? forward_pid : term_child_pid;
        if (target) {
            char c = (char)input_stream[0];
            if (c == 0 || c == '\n' || c == '\r') c = '\r';  /* standardize Enter for TUI */
            fry_proc_input(target, &c, 1);
        } else {
            handle_input_char((char)input_stream[0]);
            changed = 1;
        }
        input_consume(1);
    }
    return changed;
}

static void pump_child_io(uint32_t pid) {
    int redraw = 1;
    term_child_pid = pid;
    for (;;) {
        uint8_t in[128];
        long ni = fry_read(0, in, sizeof(in));
        if (ni > 0) {
            input_append(in, (int)ni);
            (void)process_input_stream(pid);
        }

        char out[512];
        long no;
        int dead = 0;
        while ((no = fry_proc_output(pid, out, sizeof(out))) > 0) {
            for (long i = 0; i < no; i++) term_feed((int)(unsigned char)out[i]);
            redraw = 1;
        }
        if (no < 0) dead = 1;

        if (redraw) {
            term_render();
            redraw = 0;
        }
        if (dead) break;
        fry_sleep(16);
    }
    /* child gone: leave the alternate screen and restore normal terminal state */
    if (term_in_alt) {
        for (int r = 0; r < TERM_ROWS; r++) {
            memcpy(term_rows[r], term_savebuf[r], TERM_COLS + 1);
            memcpy(term_fg[r], term_savefg[r], sizeof(term_fg[0]));
            memcpy(term_bg[r], term_savebg[r], sizeof(term_bg[0]));
        }
        term_row = term_alt_save_row;
        term_col = term_alt_save_col;
        term_in_alt = 0;
    }
    term_state = TS_NORMAL;
    term_cursor_visible = 1;
    term_child_pid = 0;
    term_clamp();
    term_putchar_raw('\n');
    term_render();
}

static int shell_tw_setup(void) {
    tw_msg_create_win_t req;
    tw_msg_win_created_t resp;
    uint8_t inbuf[sizeof(resp)];
    int inlen = 0;

    term_clear();
    term_pixels = malloc((size_t)TERM_W * (size_t)TERM_H * 4);
    if (!term_pixels) return -1;
    gfx_init(&term_gfx, term_pixels, TERM_W, TERM_H, TERM_W);

    memset(&req, 0, sizeof(req));
    req.hdr.type = TW_MSG_CREATE_WINDOW;
    req.hdr.magic = TW_MAGIC;
    req.w = TERM_W;
    req.h = TERM_H;
    strncpy(req.title, "Shell", sizeof(req.title) - 1);
    fry_write(1, &req, sizeof(req));

    memset(&resp, 0, sizeof(resp));
    for (int tries = 0; tries < 300; tries++) {
        uint8_t buf[32];
        long n = fry_read(0, buf, sizeof(buf));
        if (n > 0) {
            for (long i = 0; i < n; i++) {
                if (inlen < (int)sizeof(inbuf)) {
                    inbuf[inlen++] = buf[i];
                } else {
                    memmove(inbuf, inbuf + 1, sizeof(inbuf) - 1);
                    inbuf[sizeof(inbuf) - 1] = buf[i];
                }
                if (inlen == (int)sizeof(inbuf)) {
                    memcpy(&resp, inbuf, sizeof(resp));
                    if (resp.hdr.magic == TW_MAGIC &&
                        resp.hdr.type == TW_MSG_WINDOW_CREATED &&
                        resp.shm_id >= 0) {
                        long ptr = fry_shm_map(resp.shm_id);
                        if (ptr > 0) {
                            term_pixels = (uint32_t *)(uintptr_t)ptr;
                            gfx_init(&term_gfx, term_pixels, TERM_W, TERM_H, TERM_W);
                            term_ready = 1;
                            return 0;
                        }
                    }
                }
            }
        } else {
            fry_sleep(10);
        }
    }
    term_ready = 1;
    return 0;
}

#define printf term_printf
#define puts term_puts_line
#define putchar term_putchar

/* -----------------------------------------------------------------------
 * Shell Command Implementations
 * ----------------------------------------------------------------------- */

/* fry1824: this is now THE shell environment, handed to every program the shell
 * spawns — not just to native Python. It was called native_python_env because
 * cmd_python3/cmd_hermes were the only things that passed an environment at
 * all; the generic spawn path passed none. PATH gains the directories where
 * programs actually live in the guest, so `hermes` and `python3` resolve as
 * ordinary files instead of needing a compiled-in builtin each. */
static const char *shell_env[] = {
    "PATH=/nvme/apps:/nvme/bin:/nvme/usr/bin:/nvme/usr/local/bin:"
        "/nvme/opt/hermes-agent:/bin:/usr/bin",
    "HOME=/nvme",
    "TMPDIR=/tmp",
    "TATERTOS=1",
    "PYTHONHOME=/nvme/usr/local",
    "PYTHONPATH=/nvme/usr/local/lib/python3.14:/nvme/usr/local/lib/python3.14/site-packages",
    "PYTHONUNBUFFERED=1",
    /* fry1811: dump a real Python traceback (file + line) on fatal signals and
     * SIGABRT, to stderr -> serial. hermes blocks silently in startup with no
     * exception and no output; every blocker solved this session was solved by
     * evidence, and the two solved by theory were both wrong. */
    "PYTHONFAULTHANDLER=1",
    "SSL_CERT_FILE=/nvme/usr/local/lib/python3.14/site-packages/certifi/cacert.pem",
    /* No configure-generated _sysconfigdata_<abiflags>_<plat>_<multiarch> module in
     * this freestanding build; point sysconfig at the shipped stub so sysconfig/
     * zoneinfo/etc. import cleanly (fry1722). */
    "_PYTHON_SYSCONFIGDATA_NAME=_sysconfigdata",
    "TERM=xterm-256color",
    "COLORTERM=truecolor"
};

#define SHELL_ENVC ((uint32_t)(sizeof(shell_env) / sizeof(shell_env[0])))

static long spawn_native_python(const char **argv, uint32_t argc) {
    return fry_spawn_args("/nvme/apps/python3.fry", argv, argc,
                          shell_env, SHELL_ENVC);
}

/* fry1824: PATH resolution. A name containing '/' is taken as a path (relative
 * names are resolved against cwd, as a shell must). A bare name is searched
 * along PATH, trying the plain name first and then the two executable suffixes
 * this OS actually uses — .fry for native binaries and .tot for the shell's
 * own format — so `python3` finds /nvme/apps/python3.fry without the user
 * having to know the extension.
 * Returns 1 and fills out[] when something exists at the resolved path, 0 when
 * nothing matches. Existence is checked with fry_stat(); a directory is
 * rejected so `cd`-like names do not resolve to something unspawnable. */
static int resolve_program(const char *name, char *out, uint32_t outsz) {
    static const char *suffixes[] = { "", ".fry", ".tot", ".FRY", ".TOT" };
    struct fry_stat st;
    const char *path_env = 0;

    if (!name || !*name || !out || outsz == 0) return 0;

    /* Explicit path: absolute as given, relative against cwd. */
    if (strchr(name, '/')) {
        if (name[0] == '/') {
            snprintf(out, outsz, "%s", name);
        } else if (strcmp(cwd, "/") == 0) {
            snprintf(out, outsz, "/%s", name);
        } else {
            snprintf(out, outsz, "%s/%s", cwd, name);
        }
        if (fry_stat(out, &st) < 0) return 0;
        return 1;
    }

    for (uint32_t i = 0; i < SHELL_ENVC; i++) {
        if (strncmp(shell_env[i], "PATH=", 5) == 0) {
            path_env = shell_env[i] + 5;
            break;
        }
    }
    if (!path_env) return 0;

    while (*path_env) {
        char dir[FRY_PATH_MAX];
        uint32_t d = 0;

        while (*path_env == ':') path_env++;
        while (*path_env && *path_env != ':' && d + 1 < sizeof(dir)) {
            dir[d++] = *path_env++;
        }
        while (*path_env && *path_env != ':') path_env++;  /* overlong entry */
        dir[d] = '\0';
        if (d == 0) continue;

        for (uint32_t s = 0; s < sizeof(suffixes) / sizeof(suffixes[0]); s++) {
            snprintf(out, outsz, "%s/%s%s", dir, name, suffixes[s]);
            if (fry_stat(out, &st) == 0) return 1;
        }
    }

    out[0] = '\0';
    return 0;
}

static void cmd_python3(int argc, char **argv) {
    const char *pyargv[MAX_ARGS];
    if (argc <= 0) return;
    if (argc > MAX_ARGS) argc = MAX_ARGS;
    pyargv[0] = "python3";
    for (int i = 1; i < argc; i++) pyargv[i] = argv[i];

    long pid = spawn_native_python(pyargv, (uint32_t)argc);
    if (pid < 0) {
        printf("python3: launch failed (%ld). Is /nvme/apps/python3.fry staged?\n", -pid);
        return;
    }
    pump_child_io((uint32_t)pid);
}

static void cmd_hermes(int argc, char **argv) {
    const char *pyargv[MAX_ARGS];
    int pyargc = 0;

    /* `hermes` launches the full agent CLI (hermes_cli.main:main), same as a
     * production install. Any args pass straight through. The old bare-`hermes`
     * tatertos_bootstrap.py diagnostic was a dev scaffold and is retired. */
    pyargv[pyargc++] = "python3";
    pyargv[pyargc++] = "/nvme/opt/hermes-agent/hermes";
    for (int i = 1; i < argc && pyargc < MAX_ARGS; i++) {
        pyargv[pyargc++] = argv[i];
    }

    long pid = spawn_native_python(pyargv, (uint32_t)pyargc);
    if (pid < 0) {
        printf("hermes: launch failed (%ld). Is native Python/Hermes staged?\n", -pid);
        return;
    }
    pump_child_io((uint32_t)pid);
}

static void cmd_help(int argc, char **argv) {
    (void)argc; (void)argv;
    puts("TaterTOS Shell Commands:");
    puts("  ls [path]      List directory");
    puts("  cd <path>      Change directory");
    puts("  cat <file>     Display file content");
    puts("  pwd            Show current directory");
    puts("  echo [str]     Echo text");
    puts("  ps             List processes");
    puts("  kill <pid>     Terminate process");
    puts("  mkdir <path>   Create directory");
    puts("  rm <path>      Delete file");
    puts("  free           Show memory info");
    puts("  df             Show disk info");
    puts("  ifconfig       Show network info");
    puts("  ping <ip>      Ping host");
    puts("  python3 [...]  Run native TaterTOS CPython");
    puts("  hermes [...]   Launch the Hermes agent CLI");
    puts("  claude [...]   Run Claude Code from /nvme");
    puts("  reboot         Restart system");
    puts("  shutdown       Power off");
    puts("  help           Show this help");
    puts("  exit           Exit shell");
}

static void cmd_ls(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : cwd;
    char buf[4096];
    long n = fry_readdir(path, buf, sizeof(buf));
    if (n < 0) {
        printf("ls: %s: error %ld\n", path, -n);
        return;
    }

    struct fry_dirent *de = (struct fry_dirent *)buf;
    while ((char *)de < buf + n) {
        printf("%c %10lu  %s\n", (de->attr & 0x1) ? 'd' : '-', (unsigned long)de->size, de->name);
        de = (struct fry_dirent *)((char *)de + de->rec_len);
    }
}

static void cmd_cd(int argc, char **argv) {
    if (argc < 2) return;
    if (fry_chdir(argv[1]) < 0) {
        printf("cd: %s: no such directory\n", argv[1]);
        return;
    }
    /* Refresh local CWD from kernel — far more reliable than local path join */
    char gcwd[256];
    if (fry_getcwd(gcwd, sizeof(gcwd)) >= 0) {
        strncpy(cwd, gcwd, sizeof(cwd) - 1);
        cwd[sizeof(cwd) - 1] = 0;
    }
}

static void cmd_cat(int argc, char **argv) {
    if (argc < 2) return;
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        printf("cat: %s: cannot open\n", argv[1]);
        return;
    }

    char buf[1024];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            term_putchar_raw(buf[i]);
        }
    }
    close(fd);
    putchar('\n');
}

static void cmd_ps(int argc, char **argv) {
    (void)argc; (void)argv;
    long count = fry_proc_count();
    printf("Active processes: %ld\n", count);
}

static void cmd_kill(int argc, char **argv) {
    if (argc < 2) return;
    int pid = atoi(argv[1]);
    if (kill(pid, 15) < 0) {
        printf("kill: %d: failed\n", pid);
    }
}

static void cmd_mkdir(int argc, char **argv) {
    if (argc < 2) return;
    if (mkdir(argv[1], 0755) < 0) {
        printf("mkdir: %s: failed\n", argv[1]);
    }
}

static void cmd_rm(int argc, char **argv) {
    if (argc < 2) return;
    if (unlink(argv[1]) < 0) {
        printf("rm: %s: failed\n", argv[1]);
    }
}

static void cmd_df(int argc, char **argv) {
    (void)argc; (void)argv;
    struct fry_storage_info info;
    if (fry_storage_info(&info) >= 0) {
        printf("Storage: %s (%lu sectors)\n", info.nvme_detected ? "NVMe" : "RAM", (unsigned long)info.total_sectors);
        printf("Root: %s\n", info.root_mount);
    }
}

static void cmd_ifconfig(int argc, char **argv) {
    (void)argc; (void)argv;
    struct fry_wifi_status ws;
    if (fry_wifi_status(&ws) >= 0) {
        printf("WiFi: %s (IP: %u.%u.%u.%u)\n", ws.connected ? "Connected" : "Disconnected",
               (ws.ip >> 24) & 0xFF, (ws.ip >> 16) & 0xFF, (ws.ip >> 8) & 0xFF, ws.ip & 0xFF);
    }
}

static void cmd_ping(int argc, char **argv) {
    if (argc < 2) return;
    printf("PING %s...\n", argv[1]);
    /* simplified ping logic */
}

static void cmd_acpi(int argc, char **argv) {
    (void)argc; (void)argv;
    struct fry_acpi_diag d;
    if (fry_acpi_diag(&d) >= 0) {
        printf("ACPI: %u nodes, EC: %s, Batt: %u, BL: %u\n",
               d.ns_nodes, d.ec_ok ? "OK" : "FAIL", d.batt_count, d.bl_found);
    }
}

static void cmd_claude(int argc, char **argv) {
    static const char *envp[] = {
        "PATH=/nvme/bin:/home/legacyindieradio/mcp-servers/.venv/bin:/usr/bin:/apps",
        "LD_LIBRARY_PATH=/usr/lib:/lib64",
        "HOME=/nvme",
        "TMPDIR=/nvme",
        "TATERTOS=1",
        "PYTHONHOME=/usr",
        "PYTHONPATH=/home/legacyindieradio/mcp-servers/.venv/lib/python3.14/site-packages:/home/legacyindieradio/mcp-servers",
        "PYTHONUNBUFFERED=1",
        "SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt",
        "TERM=xterm-256color",
        "COLORTERM=truecolor",
        /* fry1604: RE-ADD SHELL. The A/B (fry1595/1596) PROVED the shell env was
         * NOT the freeze regression (the fry1592 demand-batch was, now reverted in
         * fry1600). On screen Claude's Bash tool was probing macOS/Homebrew paths
         * (/opt/homebrew/bin/bash, /usr/local/bin/bash) that don't exist here ->
         * "bash is broke". Point SHELL at the staged busybox 'sh' applet (the 'sh'
         * applet is guaranteed; 'bash' may not be). PATH above now leads with
         * /nvme/bin so the shell + staged tools resolve. */
        "SHELL=/nvme/bin/sh"
    };
    if (argc <= 0) return;
    argv[0] = "claude";
    puts("claude: loading /nvme/RCLAUDE.LXE...");
    term_render();
    long pid = fry_spawn_linux_args("/nvme/RCLAUDE.LXE",
                                    (const char **)argv, (uint32_t)argc,
                                    envp, (uint32_t)(sizeof(envp) / sizeof(envp[0])));
    if (pid < 0) {
        printf("claude: launch failed (%ld). Is STAGE_CLAUDE=1 in the image?\n", -pid);
        return;
    }
    pump_child_io((uint32_t)pid);
}

/* -----------------------------------------------------------------------
 * Main loop
 * ----------------------------------------------------------------------- */

static void execute_command(int argc, char **argv) {
    if (argc == 0) return;
    if (strcmp(argv[0], "help") == 0) cmd_help(argc, argv);
    else if (strcmp(argv[0], "ls") == 0) cmd_ls(argc, argv);
    else if (strcmp(argv[0], "cd") == 0) cmd_cd(argc, argv);
    else if (strcmp(argv[0], "cat") == 0) cmd_cat(argc, argv);
    else if (strcmp(argv[0], "pwd") == 0) puts(cwd);
    else if (strcmp(argv[0], "ps") == 0) cmd_ps(argc, argv);
    else if (strcmp(argv[0], "kill") == 0) cmd_kill(argc, argv);
    else if (strcmp(argv[0], "mkdir") == 0) cmd_mkdir(argc, argv);
    else if (strcmp(argv[0], "rm") == 0) cmd_rm(argc, argv);
    else if (strcmp(argv[0], "df") == 0) cmd_df(argc, argv);
    else if (strcmp(argv[0], "ifconfig") == 0) cmd_ifconfig(argc, argv);
    else if (strcmp(argv[0], "ping") == 0) cmd_ping(argc, argv);
    else if (strcmp(argv[0], "acpi") == 0) cmd_acpi(argc, argv);
    else if (strcmp(argv[0], "python3") == 0) cmd_python3(argc, argv);
    else if (strcmp(argv[0], "hermes") == 0) cmd_hermes(argc, argv);
    else if (strcmp(argv[0], "claude") == 0) cmd_claude(argc, argv);
    else if (strcmp(argv[0], "reboot") == 0) fry_reboot();
    else if (strcmp(argv[0], "shutdown") == 0) fry_shutdown();
    else if (strcmp(argv[0], "exit") == 0) fry_exit(0);
    else {
        /* fry1824: real external command execution.
         * This used to be `fry_spawn(argv[0])`, which took ONLY a path — no
         * arguments, no environment, no PATH search. That is why python3,
         * hermes and claude each needed a compiled-in builtin above: the
         * generic path could not pass them so much as an argv.
         * Now: resolve argv[0] through PATH, then spawn it with the real argv
         * and the shell environment, and wait for it like any other job. With
         * the kernel's "#!" support (fry1822) this runs scripts too, so
         * `hermes` is an ordinary file on PATH rather than a special case. */
        char prog[FRY_PATH_MAX];
        if (!resolve_program(argv[0], prog, sizeof(prog))) {
            printf("shell: %s: command not found\n", argv[0]);
            return;
        }
        long pid = fry_spawn_args(prog, (const char **)argv, (uint32_t)argc,
                                  shell_env, SHELL_ENVC);
        if (pid < 0) {
            printf("shell: %s: cannot execute (%ld)\n", argv[0], -pid);
            return;
        }
        pump_child_io((uint32_t)pid);
    }
}

int main(void) {
    if (shell_tw_setup() != 0) return 1;

    puts("TaterTOS64v3 Shell");
    puts("Type 'help' for commands.");
    putchar('\n');
    print_prompt();
    term_render();

    for (;;) {
        uint8_t buf[128];
        long n = fry_read(0, buf, sizeof(buf));
        if (n > 0) {
            input_append(buf, (int)n);
            if (process_input_stream(0)) term_render();
        } else {
            fry_sleep(16);
        }
    }
}
