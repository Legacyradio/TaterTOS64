// PS/2 mouse driver for TaterTOS64v3
// Phase 7: Intellimouse wheel support (4-byte packets when available)

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

// Packet accumulation (3 or 4 bytes depending on wheel support).
static uint8_t  mouse_buf[4];
static uint8_t  mouse_cycle;
static uint8_t  mouse_has_wheel;   /* 1 if Intellimouse protocol active */
static uint8_t  mouse_pkt_size;    /* 3 or 4 */

// Per-read delta accumulators: sum all IRQ deltas between calls to
// ps2_mouse_get().  Cleared by ps2_mouse_get() so the caller receives
// the exact displacement since the last read.
static int32_t  mouse_dx_acc;
static int32_t  mouse_dy_acc;
static int32_t  mouse_wheel_acc;   /* scroll wheel accumulator */

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
    if (mouse_cycle == 0 && !(data & 0x08)) {
        return;   // not a valid start byte; keep waiting
    }

    mouse_buf[mouse_cycle++] = data;

    if (mouse_cycle < mouse_pkt_size) return;

    /* Full packet received */
    mouse_cycle = 0;

    mouse_btns = mouse_buf[0] & 0x07;

    // PS/2 uses 9-bit two's complement for X and Y deltas.
    int32_t dx = (int32_t)mouse_buf[1];
    if (mouse_buf[0] & 0x10) dx -= 256;   /* XS=1: value is negative */
    int32_t dy = (int32_t)mouse_buf[2];
    if (mouse_buf[0] & 0x20) dy -= 256;   /* YS=1: value is negative */

    // Overflow bits (ignore the entire axis if set).
    if (mouse_buf[0] & 0x40) dx = 0;
    if (mouse_buf[0] & 0x80) dy = 0;

    // Clamp to prevent runaway deltas from a misbehaving device.
    dx = clamp_i32(dx, -96, 96);
    dy = clamp_i32(dy, -96, 96);

    // UNIVERSAL Y-AXIS CONVENTION: positive dY = UP on screen.
    // Screen coordinates have Y increasing DOWNWARD, so we negate.
    dy = -dy;

    mouse_x += dx;
    mouse_y += dy;

    // Accumulate deltas for the next ps2_mouse_get() call.
    mouse_dx_acc += dx;
    mouse_dy_acc += dy;

    // Wheel data: byte 3 is a signed 8-bit value.
    // Positive = scroll up (away from user), negative = scroll down.
    if (mouse_has_wheel) {
        int8_t wz = (int8_t)mouse_buf[3];
        mouse_wheel_acc += (int32_t)wz;
    }

    // Clamp to a 4096x4096 virtual canvas; GUI clips to screen size.
    if (mouse_x < 0)    mouse_x = 0;
    if (mouse_y < 0)    mouse_y = 0;
    if (mouse_x > 4095) mouse_x = 4095;
    if (mouse_y > 4095) mouse_y = 4095;
}

void ps2_mouse_get(int32_t *x, int32_t *y, uint8_t *btns,
                   int32_t *dx, int32_t *dy) {
    if (x)    *x    = mouse_x;
    if (y)    *y    = mouse_y;
    if (btns) *btns = mouse_btns;
    if (dx) { *dx = mouse_dx_acc; mouse_dx_acc = 0; }
    if (dy) { *dy = mouse_dy_acc; mouse_dy_acc = 0; }
}

/* Extended: also returns wheel delta since last call */
void ps2_mouse_get_ext(int32_t *x, int32_t *y, uint8_t *btns,
                       int32_t *dx, int32_t *dy, int32_t *wheel) {
    if (x)     *x     = mouse_x;
    if (y)     *y     = mouse_y;
    if (btns)  *btns  = mouse_btns;
    if (dx)    { *dx = mouse_dx_acc; mouse_dx_acc = 0; }
    if (dy)    { *dy = mouse_dy_acc; mouse_dy_acc = 0; }
    if (wheel) { *wheel = mouse_wheel_acc; mouse_wheel_acc = 0; }
}

uint8_t ps2_mouse_has_wheel(void) {
    return mouse_has_wheel;
}

/* Intellimouse magic sequence: set sample rate 200, 100, 80, then read ID.
 * If the device reports ID=3 instead of ID=0, it supports the wheel and
 * sends 4-byte packets with scroll data in byte 3. */
static uint8_t try_intellimouse(void) {
    /* Set sample rate sequence: 200, 100, 80 */
    mouse_write(0xF3); (void)mouse_read_byte(); /* ack */
    mouse_write(200);  (void)mouse_read_byte(); /* ack */
    mouse_write(0xF3); (void)mouse_read_byte(); /* ack */
    mouse_write(100);  (void)mouse_read_byte(); /* ack */
    mouse_write(0xF3); (void)mouse_read_byte(); /* ack */
    mouse_write(80);   (void)mouse_read_byte(); /* ack */

    /* Read device ID */
    mouse_write(0xF2); (void)mouse_read_byte(); /* ack */
    uint8_t id = mouse_read_byte();

    return (id == 3) ? 1 : 0;
}

void ps2_mouse_init(void) {
    mouse_x      = 2048;   // start cursor at centre of 0-4095 virtual canvas
    mouse_y      = 2048;
    mouse_btns   = 0;
    mouse_cycle  = 0;
    mouse_dx_acc = 0;
    mouse_dy_acc = 0;
    mouse_wheel_acc = 0;
    mouse_has_wheel = 0;
    mouse_pkt_size  = 3;

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

    // Try to enable Intellimouse wheel protocol
    if (try_intellimouse()) {
        mouse_has_wheel = 1;
        mouse_pkt_size  = 4;
        kprint("PS2_MOUSE: Intellimouse wheel detected (4-byte packets)\n");
    } else {
        kprint("PS2_MOUSE: standard 3-byte mode (no wheel)\n");
    }

    // Set resolution to maximum: 0xE8 cmd, then 0x03 = 8 counts/mm.
    mouse_write(0xE8);
    (void)mouse_read_byte();   // 0xFA  (ack)
    mouse_write(0x03);         // 8 counts/mm
    (void)mouse_read_byte();   // 0xFA  (ack)

    // Enable data reporting (streaming mode).
    mouse_write(0xF4);
    (void)mouse_read_byte();   // 0xFA  (ack)

    // Register the IRQ12 handler.  IRQ12 GSI=12 → vector 44 via IOAPIC
    if (request_irq(44, ps2_mouse_irq, 0, "ps2_mouse", 0) != 0) {
        kprint("PS2_MOUSE: irq register failed\n");
        return;
    }

    kprint("PS2_MOUSE: init ok x=2048 y=2048\n");
}
