// PS/2 controller

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static void wait_input_clear(void) {
    for (uint32_t i = 0; i < 100000; i++) {
        if ((inb(0x64) & 0x02) == 0) return;
    }
}

static void wait_output_full(void) {
    for (uint32_t i = 0; i < 100000; i++) {
        if (inb(0x64) & 0x01) return;
    }
}

void ps2_ctrl_init(void) {
    // Disable devices
    wait_input_clear();
    outb(0x64, 0xAD);
    wait_input_clear();
    outb(0x64, 0xA7);

    // Flush output buffer — drain all pending bytes left by BIOS/firmware.
    // A single read is not enough; the BIOS may leave multiple scan codes or
    // mouse bytes in the controller FIFO.  Loop until OBF is clear.
    for (int i = 0; i < 32; i++) {
        if (!(inb(0x64) & 0x01)) break;
        (void)inb(0x60);
    }

    // Read config
    wait_input_clear();
    outb(0x64, 0x20);
    wait_output_full();
    uint8_t cfg = inb(0x60);

    // Enable IRQ1 and enable set-1 translation for keyboard scancodes.
    cfg |= 0x01;
    cfg |= (1 << 4);

    // Write config
    wait_input_clear();
    outb(0x64, 0x60);
    wait_input_clear();
    outb(0x60, cfg);

    // Enable first port
    wait_input_clear();
    outb(0x64, 0xAE);
}
