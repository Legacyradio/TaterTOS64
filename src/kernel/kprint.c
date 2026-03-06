// Kernel printing: serial + framebuffer

#include <stdint.h>
#include <stdarg.h>
#include "../boot/efi_handoff.h"
#include "mm/vmm.h"
#include "../drivers/smp/spinlock.h"
#include "../boot/early_serial.h"

// Minimal 5x7 font for ASCII (expanded to 8x16 at draw time).
// Lowercase letters are mapped to uppercase.
static const uint8_t font5x7[128][7] = {
    [' '] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['!'] = {0x04,0x04,0x04,0x04,0x04,0x00,0x04},
    ['"'] = {0x0A,0x0A,0x0A,0x00,0x00,0x00,0x00},
    ['#'] = {0x0A,0x1F,0x0A,0x0A,0x1F,0x0A,0x00},
    ['$'] = {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04},
    ['%'] = {0x19,0x19,0x02,0x04,0x08,0x13,0x13},
    ['&'] = {0x0C,0x12,0x14,0x08,0x15,0x12,0x0D},
    ['\'']= {0x06,0x04,0x08,0x00,0x00,0x00,0x00},
    ['('] = {0x02,0x04,0x08,0x08,0x08,0x04,0x02},
    [')'] = {0x08,0x04,0x02,0x02,0x02,0x04,0x08},
    ['*'] = {0x00,0x0A,0x04,0x1F,0x04,0x0A,0x00},
    ['+'] = {0x00,0x04,0x04,0x1F,0x04,0x04,0x00},
    [','] = {0x00,0x00,0x00,0x00,0x06,0x04,0x08},
    ['-'] = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},
    ['.'] = {0x00,0x00,0x00,0x00,0x00,0x06,0x06},
    ['/'] = {0x01,0x02,0x04,0x08,0x10,0x00,0x00},
    ['0'] = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
    ['1'] = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    ['2'] = {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
    ['3'] = {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
    ['4'] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
    ['5'] = {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    ['6'] = {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},
    ['7'] = {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    ['8'] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    ['9'] = {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    [':'] = {0x00,0x06,0x06,0x00,0x06,0x06,0x00},
    [';'] = {0x00,0x06,0x06,0x00,0x06,0x04,0x08},
    ['<'] = {0x02,0x04,0x08,0x10,0x08,0x04,0x02},
    ['='] = {0x00,0x1F,0x00,0x1F,0x00,0x00,0x00},
    ['>'] = {0x08,0x04,0x02,0x01,0x02,0x04,0x08},
    ['?'] = {0x0E,0x11,0x01,0x02,0x04,0x00,0x04},
    ['@'] = {0x0E,0x11,0x01,0x0D,0x15,0x15,0x0E},
    ['A'] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},
    ['B'] = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    ['C'] = {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
    ['D'] = {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C},
    ['E'] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
    ['F'] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    ['G'] = {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},
    ['H'] = {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    ['I'] = {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
    ['J'] = {0x07,0x02,0x02,0x02,0x12,0x12,0x0C},
    ['K'] = {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
    ['L'] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    ['M'] = {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},
    ['N'] = {0x11,0x19,0x15,0x13,0x11,0x11,0x11},
    ['O'] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
    ['P'] = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    ['Q'] = {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},
    ['R'] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    ['S'] = {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},
    ['T'] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    ['U'] = {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
    ['V'] = {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
    ['W'] = {0x11,0x11,0x11,0x15,0x15,0x15,0x0A},
    ['X'] = {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    ['Y'] = {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},
    ['Z'] = {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
    ['['] = {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E},
    ['\\']= {0x10,0x08,0x04,0x02,0x01,0x00,0x00},
    [']'] = {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E},
    ['^'] = {0x04,0x0A,0x11,0x00,0x00,0x00,0x00},
    ['_'] = {0x00,0x00,0x00,0x00,0x00,0x00,0x1F},
    ['`'] = {0x08,0x04,0x02,0x00,0x00,0x00,0x00},
    ['{'] = {0x02,0x04,0x04,0x08,0x04,0x04,0x02},
    ['|'] = {0x04,0x04,0x04,0x04,0x04,0x04,0x04},
    ['}'] = {0x08,0x04,0x04,0x02,0x04,0x04,0x08},
    ['~'] = {0x00,0x09,0x16,0x00,0x00,0x00,0x00},
};

static struct {
    uint64_t fb_base;
    uint64_t fb_width;
    uint64_t fb_height;
    uint64_t fb_stride;
    uint32_t fb_pixel_format;
    uint64_t term_x;
    uint64_t term_y;
    uint64_t term_w;
    uint64_t term_h;
    uint32_t term_fg;
    uint32_t term_bg;
    uint32_t chrome_bg;
    uint32_t chrome_bar;
    uint32_t chrome_border;
    uint64_t cursor_x;
    uint64_t cursor_y;
    int serial_inited;
} kcon;

static spinlock_t kprint_lock;
static uint64_t saved_cursor_x;
static uint64_t saved_cursor_y;
static int esc_active;
static char esc_buf[16];
static uint32_t esc_len;
static int g_kprint_errors_only = 1;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static void serial_init(void) {
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x80);
    outb(0x3F8 + 0, 0x03);
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x03);
    outb(0x3F8 + 2, 0xC7);
    // Loopback test: verify a real UART is present before committing to serial.
    // On hardware without COM1 (e.g. Dell Precision 7530), reading LSR may
    // return 0x00 (TX not empty) forever, causing every kprint to hang.
    outb(0x3F8 + 4, 0x1E);  // MCR: enable loopback
    outb(0x3F8 + 0, 0xAE);  // send test byte
    int uart_ok = 0;
    for (int i = 0; i < 10000; i++) {
        if (inb(0x3F8 + 5) & 0x01) {           // LSR: data ready
            uart_ok = (inb(0x3F8 + 0) == 0xAE);
            break;
        }
    }
    if (!uart_ok) {
        kcon.serial_inited = 2;  // 2 = no UART, suppress all serial I/O
        return;
    }
    outb(0x3F8 + 4, 0x0B);  // MCR: restore normal mode (OUT2|RTS|DTR)
    kcon.serial_inited = 1;
}

static int serial_is_transmit_empty(void) {
    if (!kcon.serial_inited) {
        serial_init();
    }
    if (kcon.serial_inited != 1) return 1;
    return inb(0x3F8 + 5) & 0x20;
}

static int serial_received(void) {
    if (!kcon.serial_inited) {
        serial_init();
    }
    if (kcon.serial_inited != 1) return 0;
    return inb(0x3F8 + 5) & 0x01;
}

static void serial_write_char(char c) {
    if (!kcon.serial_inited) {
        serial_init();
    }
    if (kcon.serial_inited != 1) return;  // no UART detected — skip spin and write
    while (!serial_is_transmit_empty()) {}
    outb(0x3F8, (uint8_t)c);
}

static char serial_read_char(void) {
    if (!kcon.serial_inited) {
        serial_init();
    }
    if (kcon.serial_inited != 1) {
        return 0;
    }
    while (!serial_received()) {}
    return (char)inb(0x3F8);
}

static void fb_putpixel(uint64_t x, uint64_t y, uint32_t rgb) {
    if (x >= kcon.fb_width || y >= kcon.fb_height) {
        return;
    }
    uint32_t *fb = (uint32_t *)(uintptr_t)kcon.fb_base;
    uint64_t index = y * kcon.fb_stride + x;
    uint32_t val = rgb;
    // Convert RGB to BGRX if needed
    if (kcon.fb_pixel_format == 0) {
        uint32_t r = (rgb >> 16) & 0xFF;
        uint32_t g = (rgb >> 8) & 0xFF;
        uint32_t b = rgb & 0xFF;
        val = (b << 16) | (g << 8) | r;
    }
    fb[index] = val;
}

static void fb_fill_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint32_t rgb) {
    if (!kcon.fb_base || !kcon.fb_stride) return;
    // Convert colour once, then write uint32_t directly — avoids per-pixel
    // bounds checks and function call overhead of fb_putpixel.
    uint32_t val = rgb;
    if (kcon.fb_pixel_format == 0) {
        uint32_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
        val = (b << 16) | (g << 8) | r;
    }
    uint32_t *fb = (uint32_t *)(uintptr_t)kcon.fb_base;
    for (uint64_t yy = 0; yy < h; yy++) {
        uint64_t row_y = y + yy;
        if (row_y >= kcon.fb_height) break;
        uint64_t col_w = w;
        if (x >= kcon.fb_width) break;
        if (x + col_w > kcon.fb_width) col_w = kcon.fb_width - x;
        uint32_t *row = fb + row_y * kcon.fb_stride + x;
        for (uint64_t xx = 0; xx < col_w; xx++) row[xx] = val;
    }
}

static uint64_t fb_cols(void) {
    return kcon.term_w / 8;
}

static uint64_t fb_rows(void) {
    return kcon.term_h / 16;
}

static void fb_set_cursor_cell(uint64_t row, uint64_t col) {
    uint64_t rows = fb_rows();
    uint64_t cols = fb_cols();
    if (rows == 0 || cols == 0) return;
    if (row < 1) row = 1;
    if (col < 1) col = 1;
    if (row > rows) row = rows;
    if (col > cols) col = cols;
    kcon.cursor_x = (col - 1) * 8;
    kcon.cursor_y = (row - 1) * 16;
}

static void fb_draw_char_abs(uint64_t x, uint64_t y, char c, uint32_t fg, uint32_t bg);
static void fb_draw_text_abs(uint64_t x, uint64_t y, const char *s, uint32_t fg, uint32_t bg);

static void fb_draw_chrome(void) {
    if (!kcon.fb_base) return;
    fb_fill_rect(0, 0, kcon.fb_width, kcon.fb_height, kcon.chrome_bg);
    fb_fill_rect(0, 0, kcon.fb_width, kcon.term_y, kcon.chrome_bar);
    // Simple window border
    fb_fill_rect(kcon.term_x - 2, kcon.term_y - 2, kcon.term_w + 4, 2, kcon.chrome_border);
    fb_fill_rect(kcon.term_x - 2, kcon.term_y + kcon.term_h, kcon.term_w + 4, 2, kcon.chrome_border);
    fb_fill_rect(kcon.term_x - 2, kcon.term_y - 2, 2, kcon.term_h + 4, kcon.chrome_border);
    fb_fill_rect(kcon.term_x + kcon.term_w, kcon.term_y - 2, 2, kcon.term_h + 4, kcon.chrome_border);
    fb_draw_text_abs(16, 8, "TaterTOS64v3 Debug Terminal", 0xF0F0F0, kcon.chrome_bar);
    // Absolute debug banner to verify chrome path is visible
    fb_draw_text_abs(16, 24, "DEBUG CHROME", 0xFFB000, kcon.chrome_bar);
}

static void fb_clear_screen(void) {
    if (kcon.fb_base) {
        fb_draw_chrome();
        fb_fill_rect(kcon.term_x, kcon.term_y, kcon.term_w, kcon.term_h, kcon.term_bg);
        kcon.cursor_x = 0;
        kcon.cursor_y = 0;
    }
}

static void fb_clear_line_from_cursor(void) {
    if (!kcon.fb_base) return;
    uint64_t y = kcon.cursor_y;
    if (y >= kcon.term_h) return;
    fb_fill_rect(kcon.term_x + kcon.cursor_x, kcon.term_y + y,
                 kcon.term_w - kcon.cursor_x, 16, kcon.term_bg);
}

static const uint8_t *glyph5x7_for(char c) {
    unsigned char uc = (unsigned char)c;
    if (uc > 127) return font5x7[(int)'?'];
    if (uc >= 'a' && uc <= 'z') uc = (unsigned char)(uc - 32);
    if (uc == ' ') return font5x7[(int)' '];
    const uint8_t *g = font5x7[uc];
    // If glyph is all zeros, fall back to '?'
    for (int i = 0; i < 7; i++) {
        if (g[i] != 0) return g;
    }
    return font5x7[(int)'?'];
}

static void fb_draw_char(uint64_t cx, uint64_t cy, char c, uint32_t fg, uint32_t bg) {
    const uint8_t *glyph = glyph5x7_for(c);
    // Expand 5x7 into 8x16: double rows, center horizontally
    for (uint64_t row = 0; row < 7; row++) {
        uint8_t bits = glyph[row] & 0x1F;
        for (uint64_t dy = 0; dy < 2; dy++) {
            uint64_t y = kcon.term_y + cy + 1 + row * 2 + dy;
            for (uint64_t col = 0; col < 5; col++) {
                uint8_t mask = 1u << (4 - col);
                uint32_t color = (bits & mask) ? fg : bg;
                fb_putpixel(kcon.term_x + cx + 1 + col, y, color);
            }
            // Fill remaining columns with background
            fb_putpixel(kcon.term_x + cx + 0, y, bg);
            fb_putpixel(kcon.term_x + cx + 6, y, bg);
            fb_putpixel(kcon.term_x + cx + 7, y, bg);
        }
    }
    // Bottom two rows as background padding
    for (uint64_t row = 14; row < 16; row++) {
        for (uint64_t col = 0; col < 8; col++) {
            fb_putpixel(kcon.term_x + cx + col, kcon.term_y + cy + row, bg);
        }
    }
}

static void fb_draw_char_abs(uint64_t x, uint64_t y, char c, uint32_t fg, uint32_t bg) {
    const uint8_t *glyph = glyph5x7_for(c);
    for (uint64_t row = 0; row < 7; row++) {
        uint8_t bits = glyph[row] & 0x1F;
        for (uint64_t dy = 0; dy < 2; dy++) {
            uint64_t yy = y + 1 + row * 2 + dy;
            for (uint64_t col = 0; col < 5; col++) {
                uint8_t mask = 1u << (4 - col);
                uint32_t color = (bits & mask) ? fg : bg;
                fb_putpixel(x + 1 + col, yy, color);
            }
            fb_putpixel(x + 0, yy, bg);
            fb_putpixel(x + 6, yy, bg);
            fb_putpixel(x + 7, yy, bg);
        }
    }
    for (uint64_t row = 14; row < 16; row++) {
        for (uint64_t col = 0; col < 8; col++) {
            fb_putpixel(x + col, y + row, bg);
        }
    }
}

static void fb_draw_text_abs(uint64_t x, uint64_t y, const char *s, uint32_t fg, uint32_t bg) {
    if (!s) return;
    uint64_t cx = x;
    while (*s) {
        fb_draw_char_abs(cx, y, *s++, fg, bg);
        cx += 8;
    }
}

static void fb_scroll_up(uint64_t lines) {
    if (lines == 0 || kcon.fb_base == 0) {
        return;
    }
    if (lines >= kcon.term_h) {
        fb_fill_rect(kcon.term_x, kcon.term_y, kcon.term_w, kcon.term_h, kcon.term_bg);
        return;
    }
    uint64_t row_bytes = kcon.fb_stride * 4;
    uint8_t *fb = (uint8_t *)(uintptr_t)kcon.fb_base;
    uint64_t move_rows = kcon.term_h - lines;
    // Move terminal area up — uint32_t pixel copies (4× faster than byte loop)
    for (uint64_t y = 0; y < move_rows; y++) {
        uint32_t *dst = (uint32_t *)(fb + (kcon.term_y + y) * row_bytes + kcon.term_x * 4);
        uint32_t *src = (uint32_t *)(fb + (kcon.term_y + y + lines) * row_bytes + kcon.term_x * 4);
        for (uint64_t i = 0; i < kcon.term_w; i++) {
            dst[i] = src[i];
        }
    }
    // Clear bottom area of terminal
    fb_fill_rect(kcon.term_x, kcon.term_y + move_rows, kcon.term_w, lines, kcon.term_bg);
}

static void fb_write_char(char c) {
    if (kcon.fb_base == 0) {
        return;
    }
    const uint64_t char_w = 8;
    const uint64_t char_h = 16;

    if (c == '\n') {
        kcon.cursor_x = 0;
        kcon.cursor_y += char_h;
    } else if (c == '\r') {
        kcon.cursor_x = 0;
    } else {
        fb_draw_char(kcon.cursor_x, kcon.cursor_y, c, kcon.term_fg, kcon.term_bg);
        kcon.cursor_x += char_w;
        if (kcon.cursor_x + char_w > kcon.term_w) {
            kcon.cursor_x = 0;
            kcon.cursor_y += char_h;
        }
    }

    if (kcon.cursor_y + char_h > kcon.term_h) {
        fb_scroll_up(char_h);
        kcon.cursor_y -= char_h;
    }
}

void kprint_init(struct fry_handoff *handoff) {
    early_debug_putc('0');
    spinlock_init(&kprint_lock);
    early_debug_putc('1');
    /* fry453: disable framebuffer output from kprint — screen stays black
     * until the GUI process takes over. All kernel debug goes to serial;
     * user-facing diagnostics go through the shell/sysinfo TaterWin apps. */
    kcon.fb_base = 0;
    kcon.fb_width = handoff->fb_width;
    kcon.fb_height = handoff->fb_height;
    kcon.fb_stride = handoff->fb_stride;
    kcon.fb_pixel_format = handoff->fb_pixel_format;
    kcon.term_fg = 0xF0F0F0;
    kcon.term_bg = 0x080808;
    kcon.chrome_bg = 0x141414;
    kcon.chrome_bar = 0x383838;
    kcon.chrome_border = 0x5A5A5A;
    if (kcon.fb_base) {
        // Force visible offsets regardless of framebuffer size.
        uint64_t margin = 64;
        uint64_t bar_h = 40;
        kcon.term_x = (kcon.fb_width > margin * 2 + 64) ? margin : 0;
        kcon.term_w = (kcon.fb_width > margin * 2 + 64) ? (kcon.fb_width - margin * 2) : kcon.fb_width;
        kcon.term_y = (kcon.fb_height > bar_h + margin + 64) ? (bar_h + margin) : bar_h;
        if (kcon.term_y >= kcon.fb_height) kcon.term_y = 0;
        kcon.term_h = (kcon.fb_height > (kcon.term_y + margin + 64))
            ? (kcon.fb_height - kcon.term_y - margin)
            : (kcon.fb_height - kcon.term_y);
        fb_clear_screen();
    } else {
        kcon.term_x = 0;
        kcon.term_y = 0;
        kcon.term_w = 0;
        kcon.term_h = 0;
    }
    kcon.cursor_x = 0;
    kcon.cursor_y = 0;
    kcon.serial_inited = 0;
    early_debug_putc('2');
    saved_cursor_x = 0;
    saved_cursor_y = 0;
    esc_active = 0;
    esc_len = 0;
    early_debug_putc('3');

    if (kcon.fb_base) {
        fb_fill_rect(0, 0, kcon.fb_width, kcon.fb_height, 0x000000);
    }
}

static void kprint_char(char c) {
    serial_write_char(c);
    fb_write_char(c);
}

static void kprint_string(const char *s) {
    while (*s) {
        kprint_char(*s++);
    }
}

static char ascii_lower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c + ('a' - 'A'));
    return c;
}

static int str_contains_ci(const char *haystack, const char *needle) {
    if (!haystack || !needle || !*needle) return 0;
    for (uint32_t i = 0; haystack[i]; i++) {
        uint32_t j = 0;
        while (needle[j] &&
               haystack[i + j] &&
               ascii_lower(haystack[i + j]) == ascii_lower(needle[j])) {
            j++;
        }
        if (!needle[j]) return 1;
    }
    return 0;
}

static int kprint_is_error_line(const char *line) {
    static const char *error_keys[] = {
        "error",
        "panic",
        "failed",
        "fail",
        "fault",
        "invalid",
        "bad ",
        "not found",
        "timeout",
        "denied",
        "corrupt",
        "too small",
        "assert",
        "unhandled",
        "overflow",
        "stack alloc failed",
        "irq register failed",
        "header read failed",
        "no block device",
        "no rsdp",
        "no xsdt/rsdt",
        "no lapic",
        "shutdown failed",
        "rsp=",
        "cr2=",
        "cr3="
    };

    if (!line || !*line) return 0;
    for (uint32_t i = 0; i < (uint32_t)(sizeof(error_keys) / sizeof(error_keys[0])); i++) {
        if (str_contains_ci(line, error_keys[i])) return 1;
    }
    return 0;
}

static int kprint_should_emit(const char *line) {
    (void)g_kprint_errors_only;  // output is hard-suppressed
    if (!line || !*line) return 0;
    if (str_contains_ci(line, "panic")) return 1;
    if (str_contains_ci(line, "fault")) return 1;
    return 0;  // drop everything else
}

void kprint_set_errors_only(int enabled) {
    (void)enabled;
    // kprint is locked to panic/fault-only output
    g_kprint_errors_only = 1;
}

static uint32_t parse_uint(const char *s, uint32_t *idx, uint32_t max) {
    uint32_t val = 0;
    while (*idx < max && s[*idx] >= '0' && s[*idx] <= '9') {
        val = val * 10 + (uint32_t)(s[*idx] - '0');
        (*idx)++;
    }
    return val;
}

static void handle_csi(const char *seq, uint32_t len) {
    if (len == 0 || seq[0] != '[') return;
    char term = seq[len - 1];
    if (term == 'J') {
        fb_clear_screen();
        return;
    }
    if (term == 'K') {
        fb_clear_line_from_cursor();
        return;
    }
    if (term == 's') {
        saved_cursor_x = kcon.cursor_x;
        saved_cursor_y = kcon.cursor_y;
        return;
    }
    if (term == 'u') {
        kcon.cursor_x = saved_cursor_x;
        kcon.cursor_y = saved_cursor_y;
        return;
    }
    if (term == 'H') {
        uint32_t i = 1;
        uint32_t row = 1;
        uint32_t col = 1;
        if (i < len - 1 && seq[i] >= '0' && seq[i] <= '9') {
            row = parse_uint(seq, &i, len - 1);
        }
        if (i < len - 1 && seq[i] == ';') {
            i++;
            if (i < len - 1) {
                col = parse_uint(seq, &i, len - 1);
            }
        }
        fb_set_cursor_cell(row, col);
        return;
    }
}

static void u64_to_str(uint64_t val, char *buf, int base, int uppercase) {
    char tmp[32];
    int i = 0;
    if (val == 0) {
        tmp[i++] = '0';
    } else {
        while (val > 0) {
            uint64_t digit = val % (uint64_t)base;
            if (digit < 10) tmp[i++] = (char)('0' + digit);
            else tmp[i++] = (char)((uppercase ? 'A' : 'a') + (digit - 10));
            val /= (uint64_t)base;
        }
    }
    int j = 0;
    while (i > 0) {
        buf[j++] = tmp[--i];
    }
    buf[j] = 0;
}

// Emit a number string into buf with optional width and padding.
// sign: 0 = no sign, '-' = prepend minus. flag_zero: pad with '0' vs ' '.
static void emit_num(char *buf, int *bi, int bufsz,
                     const char *num, int width, int flag_zero, int sign) {
    int len = 0;
    const char *n = num;
    while (*n) { len++; n++; }
    int total = len + (sign ? 1 : 0);
    int padlen = (width > total) ? width - total : 0;
    char padchar = flag_zero ? '0' : ' ';
    // zero-pad: sign comes before zeros; space-pad: sign comes after spaces
    if (sign && flag_zero && *bi < bufsz - 1) buf[(*bi)++] = (char)sign;
    for (int k = 0; k < padlen && *bi < bufsz - 1; k++) buf[(*bi)++] = padchar;
    if (sign && !flag_zero && *bi < bufsz - 1) buf[(*bi)++] = (char)sign;
    for (n = num; *n && *bi < bufsz - 1; n++) buf[(*bi)++] = *n;
}

static int vkformat_buf(char *buf, int bufsz, const char *fmt, va_list ap) {
    int bi = 0;
    for (const char *p = fmt; *p && bi < bufsz - 1; p++) {
        if (*p != '%') {
            buf[bi++] = *p;
            continue;
        }
        p++;
        if (!*p) break;

        // Flags
        int flag_zero = 0;
        int flag_left = 0;
        while (*p == '0' || *p == '-' || *p == ' ' || *p == '+' || *p == '#') {
            if (*p == '0') flag_zero = 1;
            if (*p == '-') flag_left = 1;
            p++;
        }
        // If left-align, zero-pad is ignored
        if (flag_left) flag_zero = 0;

        // Width
        int width = 0;
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }

        // Length modifier
        int len = 0;
        if (*p == 'l') {
            len = 1;
            if (*(p + 1) == 'l') { len = 2; p++; }
            p++;
            if (!*p) break;
        }

        if (*p == 's') {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int slen = 0;
            const char *t = s;
            while (*t) { slen++; t++; }
            int padlen = (!flag_left && width > slen) ? width - slen : 0;
            for (int k = 0; k < padlen && bi < bufsz - 1; k++) buf[bi++] = ' ';
            while (*s && bi < bufsz - 1) buf[bi++] = *s++;
            padlen = (flag_left && width > slen) ? width - slen : 0;
            for (int k = 0; k < padlen && bi < bufsz - 1; k++) buf[bi++] = ' ';
        } else if (*p == 'c') {
            char c = (char)va_arg(ap, int);
            if (!flag_left && width > 1) {
                for (int k = 0; k < width - 1 && bi < bufsz - 1; k++) buf[bi++] = ' ';
            }
            if (bi < bufsz - 1) buf[bi++] = c;
            if (flag_left && width > 1) {
                for (int k = 0; k < width - 1 && bi < bufsz - 1; k++) buf[bi++] = ' ';
            }
        } else if (*p == 'd' || *p == 'i') {
            long long v = (len ? va_arg(ap, long long) : (long long)va_arg(ap, int));
            char num[32];
            int sign = 0;
            if (v < 0) { sign = '-'; v = -v; }
            u64_to_str((uint64_t)v, num, 10, 0);
            emit_num(buf, &bi, bufsz, num, width, flag_zero, sign);
        } else if (*p == 'u') {
            unsigned long long v = (len ? va_arg(ap, unsigned long long)
                                        : (unsigned long long)va_arg(ap, unsigned int));
            char num[32];
            u64_to_str((uint64_t)v, num, 10, 0);
            emit_num(buf, &bi, bufsz, num, width, flag_zero, 0);
        } else if (*p == 'x' || *p == 'X') {
            unsigned long long v = (len ? va_arg(ap, unsigned long long)
                                        : (unsigned long long)va_arg(ap, unsigned int));
            char num[32];
            u64_to_str((uint64_t)v, num, 16, (*p == 'X'));
            emit_num(buf, &bi, bufsz, num, width, flag_zero, 0);
        } else if (*p == 'p') {
            uint64_t v = (uint64_t)va_arg(ap, void *);
            char num[32];
            u64_to_str(v, num, 16, 0);
            if (bi < bufsz - 1) buf[bi++] = '0';
            if (bi < bufsz - 1) buf[bi++] = 'x';
            // pointer: no extra width applied beyond 0x prefix
            for (char *n = num; *n && bi < bufsz - 1; n++) buf[bi++] = *n;
        } else if (*p == '%') {
            buf[bi++] = '%';
        }
        // unknown specifier: silently skip
    }
    buf[bi] = 0;
    return bi;
}

void kprint_serial_only(const char *fmt, ...) {
    uint64_t flags = spin_lock_irqsave(&kprint_lock);
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vkformat_buf(buf, (int)sizeof(buf), fmt, ap);
    va_end(ap);
    kprint_string(buf);
    spin_unlock_irqrestore(&kprint_lock, flags);
}

void kprint_serial_write(const char *buf, uint64_t len) {
    uint64_t flags = spin_lock_irqsave(&kprint_lock);
    if (!buf || len == 0) {
        spin_unlock_irqrestore(&kprint_lock, flags);
        return;
    }
    for (uint64_t i = 0; i < len; i++) {
        serial_write_char(buf[i]);
    }
    spin_unlock_irqrestore(&kprint_lock, flags);
}

void kprint(const char *fmt, ...) {
    uint64_t flags = spin_lock_irqsave(&kprint_lock);
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vkformat_buf(buf, (int)sizeof(buf), fmt, ap);
    va_end(ap);
    if (!kprint_should_emit(buf)) {
        spin_unlock_irqrestore(&kprint_lock, flags);
        return;
    }
    kprint_string(buf);
    spin_unlock_irqrestore(&kprint_lock, flags);
}

void kprint_write(const char *buf, uint64_t len) {
    uint64_t flags = spin_lock_irqsave(&kprint_lock);
    if (!buf || len == 0) {
        spin_unlock_irqrestore(&kprint_lock, flags);
        return;
    }
    for (uint64_t i = 0; i < len; i++) {
        char c = buf[i];
        serial_write_char(c);
        if (!kcon.fb_base) {
            continue;
        }
        if (esc_active) {
            if (esc_len + 1 < sizeof(esc_buf)) {
                esc_buf[esc_len++] = c;
                if (c == 'J' || c == 'H' || c == 'K' || c == 's' || c == 'u') {
                    handle_csi(esc_buf, esc_len);
                    esc_active = 0;
                    esc_len = 0;
                }
            } else {
                esc_active = 0;
                esc_len = 0;
            }
            continue;
        }
        if ((uint8_t)c == 0x1B) {
            esc_active = 1;
            esc_len = 0;
            continue;
        }
        fb_write_char(c);
    }
    spin_unlock_irqrestore(&kprint_lock, flags);
}

uint64_t kread_serial(char *buf, uint64_t len) {
    if (!buf || len == 0) return 0;
    uint64_t i = 0;
    while (i < len) {
        // Non-blocking: return immediately if no serial data available.
        // serial_received() checks UART LSR bit0; when serial goes to a file
        // in QEMU, this is always 0, so we never block here.
        if (!serial_received()) break;
        char c = (char)inb(0x3F8);
        if (c == '\r') c = '\n';
        buf[i++] = c;
        kprint_char(c); // echo
        if (c == '\n') break;
    }
    return i;
}
