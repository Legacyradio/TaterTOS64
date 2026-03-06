// Early serial output (COM1) for boot diagnostics
#ifndef TATER_EARLY_SERIAL_H
#define TATER_EARLY_SERIAL_H

#include <stdint.h>

static inline void early_outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t early_inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* 0 = unknown, 1 = UART present, -1 = no usable UART */
static inline int *early_serial_state_ptr(void) {
    static int state = 0;
    return &state;
}

static inline int early_serial_ensure_ready(void) {
    int *statep = early_serial_state_ptr();
    int state = *statep;
    if (state != 0) return (state > 0);

    /* Program COM1 for 38400 8N1 then loopback-test for a real UART. */
    early_outb(0x3F8 + 1, 0x00);
    early_outb(0x3F8 + 3, 0x80);
    early_outb(0x3F8 + 0, 0x03);
    early_outb(0x3F8 + 1, 0x00);
    early_outb(0x3F8 + 3, 0x03);
    early_outb(0x3F8 + 2, 0xC7);
    early_outb(0x3F8 + 4, 0x1E);  /* loopback mode */
    early_outb(0x3F8 + 0, 0xAE);  /* test byte */

    int ok = 0;
    for (int i = 0; i < 10000; i++) {
        if (early_inb(0x3F8 + 5) & 0x01) {
            ok = (early_inb(0x3F8 + 0) == 0xAE);
            break;
        }
    }

    if (!ok) {
        *statep = -1;
        return 0;
    }

    early_outb(0x3F8 + 4, 0x0B);  /* normal mode */
    *statep = 1;
    return 1;
}

static inline void early_serial_init(void) {
    (void)early_serial_ensure_ready();
}

static inline void early_serial_putc(char c) {
    if (!early_serial_ensure_ready()) return;
    for (int i = 0; i < 200000; i++) {
        if (early_inb(0x3F8 + 5) & 0x20) {
            early_outb(0x3F8, (uint8_t)c);
            return;
        }
        __asm__ volatile("pause");
    }
}

static inline void early_serial_puts(const char *s) {
    if (!s) return;
    while (*s) {
        early_serial_putc(*s++);
    }
}

static inline void early_debug_putc(char c) {
    early_outb(0xE9, (uint8_t)c);
}

static inline void early_debug_puts(const char *s) {
    if (!s) return;
    while (*s) {
        early_debug_putc(*s++);
    }
}

static inline void early_serial_puthex64(uint64_t v) {
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 15; i >= 0; i--) {
        early_serial_putc(hex[(v >> (i * 4)) & 0xF]);
    }
}

#endif
