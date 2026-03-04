// PS/2 mouse driver for TaterTOS64v3

#include <stdint.h>
#include "../../kernel/irq/manage.h"
#include "../../kernel/irq/chip.h"

void kprint(const char *fmt, ...);

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

// Wait until the PS/2 controller input buffer is empty (ready to accept data).
static void ps2_wait_write(void) {
    for (uint32_t i = 0; i < 100000; i++) {
        if ((inb(0x64) & 0x02) == 0) return;
    }
}

// Wait until the PS/2 controller output buffer has data to read.
static void ps2_wait_read(void) {
    for (uint32_t i = 0; i < 100000; i++) {
        if (inb(0x64) & 0x01) return;
    }
}

// Send a byte to the PS/2 mouse (via controller byte-redirect 0xD4).
static void mouse_write(uint8_t data) {
    ps2_wait_write();
    outb(0x64, 0xD4);   // next byte goes to the auxiliary (mouse) port
    ps2_wait_write();
    outb(0x60, data);
}

// Receive one byte from the PS/2 data port (used during init handshake).
static uint8_t mouse_read_byte(void) {
    ps2_wait_read();
    return inb(0x60);
}

// Mouse state (accumulated relative movements clamped to a virtual surface).
static int32_t  mouse_x;
static int32_t  mouse_y;
static uint8_t  mouse_btns;

// 3-byte packet accumulation.
static uint8_t  mouse_buf[3];
static uint8_t  mouse_cycle;

// Per-read delta accumulators: sum all IRQ deltas between calls to
// ps2_mouse_get().  Cleared by ps2_mouse_get() so the caller receives
// the exact displacement since the last read — no precision lost to
// integer truncation in the absolute-to-screen mapping.
static int32_t  mouse_dx_acc;
static int32_t  mouse_dy_acc;

static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void ps2_mouse_irq(uint32_t vector, void *ctx, void *dev_id, uint64_t error) {
    (void)vector; (void)ctx; (void)dev_id; (void)error;

    uint8_t data = inb(0x60);

    // PACKET SYNCHRONIZATION — universal PS/2 rule:
    // Byte 0 of every standard PS/2 packet always has bit 3 set.
    // If we are waiting for byte 0 but the incoming byte lacks bit 3,
    // it is a data byte from a shifted/4-byte Synaptics packet or a
    // spurious byte.  Discard it and stay at position 0.
    // This re-syncs automatically on any hardware without any device-
    // specific knowledge, matching the approach used by Linux, GRUB, and
    // OVMF for all PS/2 mice and Synaptics/ALPS touchpads in compat mode.
    if (mouse_cycle == 0 && !(data & 0x08)) {
        return;   // not a valid start byte; keep waiting
    }

    mouse_buf[mouse_cycle++] = data;

    if (mouse_cycle == 3) {
        mouse_cycle = 0;

        mouse_btns = mouse_buf[0] & 0x07;

        // PS/2 uses 9-bit two's complement for X and Y deltas.
        // Bit 4 of byte 0 (XS) is the 9th (sign) bit for X.
        // Bit 5 of byte 0 (YS) is the 9th (sign) bit for Y.
        // Correct decode: treat byte 1/2 as unsigned, then subtract 256 when
        // the sign bit is set.  Using (int8_t) instead is WRONG for deltas
        // > 127 because byte1's MSB may disagree with XS, flipping the sign
        // and trapping the cursor at one screen edge.
        int32_t dx = (int32_t)mouse_buf[1];
        if (mouse_buf[0] & 0x10) dx -= 256;   /* XS=1: value is negative */
        int32_t dy = (int32_t)mouse_buf[2];
        if (mouse_buf[0] & 0x20) dy -= 256;   /* YS=1: value is negative */

        // Overflow bits (ignore the entire axis if set).
        if (mouse_buf[0] & 0x40) dx = 0;
        if (mouse_buf[0] & 0x80) dy = 0;

        // Clamp to prevent runaway deltas from a misbehaving device.
        // No deadzone: even ±1 deltas are real movement.
        dx = clamp_i32(dx, -96, 96);
        dy = clamp_i32(dy, -96, 96);

        // UNIVERSAL Y-AXIS CONVENTION (per Synaptics PS/2 Interfacing Guide
        // Rev B §2.1 and the IBM PS/2 mouse spec):
        //   positive ∆Y = finger/mouse moved AWAY from user = UP on screen.
        // Screen coordinates have Y increasing DOWNWARD, so we negate.
        // This is correct for all standard PS/2 mice, Synaptics, ALPS, and
        // any other touchpad in PS/2 compat mode — it is NOT Dell-specific.
        dy = -dy;

        mouse_x += dx;
        mouse_y += dy;

        // Accumulate deltas for the next ps2_mouse_get() call.
        mouse_dx_acc += dx;
        mouse_dy_acc += dy;

        // Clamp to a 4096×4096 virtual canvas; GUI clips to screen size.
        if (mouse_x < 0)    mouse_x = 0;
        if (mouse_y < 0)    mouse_y = 0;
        if (mouse_x > 4095) mouse_x = 4095;
        if (mouse_y > 4095) mouse_y = 4095;
    }
}

void ps2_mouse_get(int32_t *x, int32_t *y, uint8_t *btns,
                   int32_t *dx, int32_t *dy) {
    if (x)    *x    = mouse_x;
    if (y)    *y    = mouse_y;
    if (btns) *btns = mouse_btns;
    // Return accumulated deltas since last call, then clear them so the
    // caller sees only new movement (not cumulative absolute drift).
    if (dx) { *dx = mouse_dx_acc; mouse_dx_acc = 0; }
    if (dy) { *dy = mouse_dy_acc; mouse_dy_acc = 0; }
}

void ps2_mouse_init(void) {
    mouse_x      = 2048;   // start cursor at centre of 0-4095 virtual canvas
    mouse_y      = 2048;
    mouse_btns   = 0;
    mouse_cycle  = 0;
    mouse_dx_acc = 0;
    mouse_dy_acc = 0;

    // Enable the PS/2 auxiliary (mouse) port.
    ps2_wait_write();
    outb(0x64, 0xA8);

    // Read the current controller configuration byte.
    ps2_wait_write();
    outb(0x64, 0x20);
    uint8_t cfg = mouse_read_byte();

    // Enable aux interrupt (bit 1) and ensure aux clock is enabled (bit 5 = 0).
    cfg |=  0x02;   // enable IRQ12 from mouse
    cfg &= ~0x20;   // clear "disable aux clock" bit

    ps2_wait_write();
    outb(0x64, 0x60);
    ps2_wait_write();
    outb(0x60, cfg);

    // Reset the mouse and wait for the self-test result.
    mouse_write(0xFF);
    (void)mouse_read_byte();   // 0xFA  (ack)
    (void)mouse_read_byte();   // 0xAA  (BAT passed)
    (void)mouse_read_byte();   // 0x00  (mouse ID)

    // Set resolution to maximum: 0xE8 cmd, then 0x03 = 8 counts/mm.
    // Default is 4 counts/mm (0x02); doubling gives larger raw deltas on
    // physical touchpads where slow movements only produce ±1-2 per packet.
    mouse_write(0xE8);
    (void)mouse_read_byte();   // 0xFA  (ack)
    mouse_write(0x03);         // 8 counts/mm
    (void)mouse_read_byte();   // 0xFA  (ack)

    // Enable data reporting (streaming mode).
    mouse_write(0xF4);
    (void)mouse_read_byte();   // 0xFA  (ack)

    // Register the IRQ12 handler.  IRQ12 GSI=12 → vector 44 via IOAPIC
    // (IOAPIC routes legacy IRQ N to vector 32+N by default).
    if (request_irq(44, ps2_mouse_irq, 0, "ps2_mouse", 0) != 0) {
        kprint("PS2_MOUSE: irq register failed\n");
        return;
    }

    kprint("PS2_MOUSE: init ok x=2048 y=2048\n");
}
