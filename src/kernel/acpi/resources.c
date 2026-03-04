// ACPI resource buffer decoder

#include <stdint.h>
#include "resources.h"

void kprint(const char *fmt, ...);

static void add_io_port_candidate(uint16_t *ports, uint32_t *found,
                                  uint32_t max_ports, uint16_t port) {
    if (!ports || !found || *found >= max_ports) return;
    /*
     * Ignore clearly invalid placeholder/system-reserved low ports.
     * Real EC command/data ports are expected to be above this range.
     */
    if (port < 0x10u) return;
    for (uint32_t i = 0; i < *found; i++) {
        if (ports[i] == port) return;
    }
    ports[(*found)++] = port;
}

static void decode_irq(const uint8_t *p, uint8_t len) {
    if (len < 2) return;
    uint16_t mask = (uint16_t)(p[0] | (p[1] << 8));
    kprint("RES: IRQ mask=0x%04x\n", mask);
}

static void decode_io(const uint8_t *p, uint8_t len) {
    if (len < 7) return;
    uint16_t min = (uint16_t)(p[1] | (p[2] << 8));
    uint16_t max = (uint16_t)(p[3] | (p[4] << 8));
    uint8_t align = p[5];
    uint8_t size = p[6];
    kprint("RES: IO 0x%x-0x%x align=%u size=%u\n", min, max, align, size);
}

static void decode_mem32(const uint8_t *p, uint16_t len) {
    if (len < 17) return;
    uint32_t min = (uint32_t)(p[4] | (p[5] << 8) | (p[6] << 16) | (p[7] << 24));
    uint32_t max = (uint32_t)(p[8] | (p[9] << 8) | (p[10] << 16) | (p[11] << 24));
    uint32_t size = (uint32_t)(p[12] | (p[13] << 8) | (p[14] << 16) | (p[15] << 24));
    kprint("RES: MEM32 0x%x-0x%x size=0x%x\n", min, max, size);
}

void acpi_decode_resources(const uint8_t *buf, uint32_t len) {
    const uint8_t *p = buf;
    const uint8_t *end = buf + len;
    while (p < end) {
        uint8_t tag = *p;
        if (tag == 0x79) { // End Tag
            break;
        }
        if ((tag & 0x80) == 0) {
            // Small item
            uint8_t name = (tag >> 3) & 0x0F;
            uint8_t slen = tag & 0x07;
            p++;
            if (p + slen > end) break;
            if (name == 0x04) {
                decode_irq(p, slen);
            } else if (name == 0x08) {
                decode_io(p, slen);
            }
            p += slen;
        } else {
            // Large item
            if (p + 3 > end) break;
            uint8_t name = p[0] & 0x7F;
            uint16_t l = (uint16_t)(p[1] | (p[2] << 8));
            p += 3;
            if (p + l > end) break;
            if (name == 0x05) { // 32-bit fixed memory
                decode_mem32(p, l);
            }
            p += l;
        }
    }
}

int acpi_get_gsi_from_crs(const uint8_t *buf, uint32_t len, uint32_t *gsi_out) {
    if (!buf || len == 0 || !gsi_out) return -1;
    const uint8_t *p = buf;
    const uint8_t *end = buf + len;
    while (p < end) {
        uint8_t tag = *p;
        if (tag == 0x79) {
            break;
        }
        if ((tag & 0x80) == 0) {
            uint8_t name = (tag >> 3) & 0x0F;
            uint8_t slen = tag & 0x07;
            p++;
            if (p + slen > end) break;
            if (name == 0x04 && slen >= 2) {
                uint16_t mask = (uint16_t)(p[0] | (p[1] << 8));
                for (uint32_t i = 0; i < 16; i++) {
                    if (mask & (1u << i)) {
                        *gsi_out = i;
                        return 0;
                    }
                }
            }
            p += slen;
        } else {
            if (p + 3 > end) break;
            uint8_t name = p[0] & 0x7F;
            uint16_t l = (uint16_t)(p[1] | (p[2] << 8));
            p += 3;
            if (p + l > end) break;
            if (name == 0x09 && l >= 2) {
                uint8_t irq_count = p[1];
                const uint8_t *list = p + 2;
                if (irq_count > 0 && (uint32_t)(l - 2) >= (uint32_t)irq_count * 4) {
                    uint32_t gsi = (uint32_t)(list[0] | (list[1] << 8) | (list[2] << 16) | (list[3] << 24));
                    *gsi_out = gsi;
                    return 0;
                }
            }
            p += l;
        }
    }
    return -1;
}

int acpi_get_io_from_crs(const uint8_t *buf, uint32_t len,
                         uint16_t *ports, uint32_t max_ports) {
    if (!buf || len == 0 || !ports || max_ports == 0) return 0;
    uint32_t found = 0;
    const uint8_t *p = buf;
    const uint8_t *end = buf + len;
    while (p < end && found < max_ports) {
        uint8_t tag = *p;
        if (tag == 0x79) break; /* End Tag */
        if ((tag & 0x80) == 0) {
            /* Small resource item */
            uint8_t name = (tag >> 3) & 0x0F;
            uint8_t slen = tag & 0x07;
            p++;
            if (p + slen > end) break;
            if (name == 0x08 && slen >= 7) {
                /* IO port descriptor: p[1..2] = min base (little-endian) */
                uint16_t base = (uint16_t)(p[1] | (p[2] << 8));
                add_io_port_candidate(ports, &found, max_ports, base);
            } else if (name == 0x09 && slen >= 3) {
                /* Fixed I/O port descriptor: p[0..1] = base */
                uint16_t base = (uint16_t)(p[0] | (p[1] << 8));
                add_io_port_candidate(ports, &found, max_ports, base);
            }
            p += slen;
        } else {
            /* Large resource item */
            uint8_t name = tag & 0x7F;
            if (p + 3 > end) break;
            uint16_t l = (uint16_t)(p[1] | (p[2] << 8));
            p += 3;
            if (p + l > end) break;
            if (name == 0x02 && l >= 12 && found < max_ports) {
                /* Generic Register Descriptor (GAS) */
                uint8_t addr_space = p[0];
                uint64_t addr = 0;
                for (uint32_t i = 0; i < 8; i++) {
                    addr |= ((uint64_t)p[4 + i]) << (i * 8);
                }
                if (addr_space == 1 && addr <= 0xFFFFu) {
                    add_io_port_candidate(ports, &found, max_ports, (uint16_t)addr);
                }
            }
            /* G448/G453: DWord Address Space (large 0x07) — I/O type */
            if (name == 0x07 && l >= 23 && found < max_ports) {
                uint8_t res_type = p[0]; /* 0=mem 1=io 2=bus */
                if (res_type == 1) {
                    /* DWord: _MIN at offset 10, _MAX at offset 14 */
                    uint32_t dw_min = (uint32_t)(p[10] | (p[11] << 8) |
                                       (p[12] << 16) | (p[13] << 24));
                    if (dw_min > 0 && dw_min <= 0xFFFFu) {
                        add_io_port_candidate(ports, &found, max_ports, (uint16_t)dw_min);
                    }
                }
            }
            /* G453: Word Address Space (large 0x08) — I/O type */
            if (name == 0x08 && l >= 13 && found < max_ports) {
                uint8_t res_type = p[0];
                if (res_type == 1) {
                    /* Word: _MIN at offset 6, _MAX at offset 8 */
                    uint16_t w_min = (uint16_t)(p[6] | (p[7] << 8));
                    if (w_min > 0) {
                        add_io_port_candidate(ports, &found, max_ports, w_min);
                    }
                }
            }
            /* G453: Extended IRQ (large 0x09) — skip, no I/O ports here */
            /* G448: QWord Address Space (large 0x0A) — I/O type */
            if (name == 0x0A && l >= 43 && found < max_ports) {
                uint8_t res_type = p[0];
                if (res_type == 1) {
                    /* QWord: _MIN at offset 14 (8 bytes) */
                    uint64_t qw_min = 0;
                    for (uint32_t i = 0; i < 8; i++) {
                        qw_min |= ((uint64_t)p[14 + i]) << (i * 8);
                    }
                    if (qw_min > 0 && qw_min <= 0xFFFFu) {
                        add_io_port_candidate(ports, &found, max_ports, (uint16_t)qw_min);
                    }
                }
            }
            p += l;
        }
    }
    return (int)found;
}
