// PS/2 keyboard

#include <stdint.h>
#include "../../kernel/irq/manage.h"
#include "../../kernel/irq/chip.h"

void kprint(const char *fmt, ...);

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static char scancode_map[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0, 'a','s','d','f','g','h','j','k','l',';','\'','`',
    0, '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0, ' ', 0
};

static char scancode_map_shift[128] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0, 'A','S','D','F','G','H','J','K','L',':','"','~',
    0, '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0, ' ', 0
};

static char keybuf[256];
static uint32_t key_head;
static uint32_t key_tail;
static uint32_t key_seen;
static uint8_t shift_down;
static uint8_t extended;

static void keybuf_put(char c) {
    uint32_t next = (key_head + 1) & 0xFF;
    if (next == key_tail) return;
    keybuf[key_head] = c;
    key_head = next;
}

int ps2_kbd_read(char *buf, uint32_t len) {
    uint32_t n = 0;
    while (n < len && key_tail != key_head) {
        buf[n++] = keybuf[key_tail];
        key_tail = (key_tail + 1) & 0xFF;
    }
    if (n == 0 && len > 0) {
        // Fallback poll if IRQs are not delivering key events.
        // MUST check bit 5 (AUXB) of the status register before reading 0x60.
        // Bit 5 = 1 means the byte is from the aux (mouse) port.  Reading a
        // mouse byte here would steal it from the ps2_mouse IRQ handler,
        // corrupting 3-byte packet accumulation and killing mouse input.
        for (int i = 0; i < 8 && n == 0; i++) {
            uint8_t st = inb(0x64);
            if (!(st & 0x01)) continue;   // output buffer empty — no data
            if (st & 0x20) continue;      // AUXB=1: mouse byte — do NOT read
            uint8_t sc = inb(0x60);
            if (sc & 0x80) continue;      // key release
            char c = scancode_map[sc & 0x7F];
            if (c) {
                buf[n++] = c;
                if (key_seen == 0) {
                    key_seen = 1;
                    kprint("PS2: key input detected\n");
                }
            }
        }
    }
    return (int)n;
}

static void ps2_kbd_irq(uint32_t vector, void *ctx, void *dev_id, uint64_t error) {
    (void)ctx; (void)dev_id; (void)error; (void)vector;
    uint8_t sc = inb(0x60);
    if (sc == 0xE0) {
        extended = 1;
        return;
    }
    if (extended) {
        extended = 0;
        return;
    }
    if (sc == 0x2A || sc == 0x36) { shift_down = 1; return; }
    if (sc == 0xAA || sc == 0xB6) { shift_down = 0; return; }
    if (sc & 0x80) return; // key release
    char c = shift_down ? scancode_map_shift[sc & 0x7F] : scancode_map[sc & 0x7F];
    if (c) {
        keybuf_put(c);
        if (key_seen == 0) {
            key_seen = 1;
            kprint("PS2: key input detected\n");
        }
    }
}

void ps2_kbd_init(void) {
    key_head = key_tail = 0;
    key_seen = 0;
    // IRQ1 is vector 33
    if (request_irq(33, ps2_kbd_irq, 0, "ps2_kbd", 0) != 0) {
        kprint("PS2: irq register failed\n");
    }
}
