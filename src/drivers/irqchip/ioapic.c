// I/O APIC support

#include <stdint.h>
#include "../../kernel/acpi/madt.h"
#include "../../kernel/irq/irqdesc.h"
#include "../../kernel/mm/vmm.h"

void kprint(const char *fmt, ...);

#define IOAPIC_REGSEL 0x00
#define IOAPIC_WIN    0x10

struct ioapic_state {
    volatile uint32_t *base;
    uint32_t gsi_base;
    uint32_t gsi_max;
};

static struct ioapic_state ioapics[16];
static uint32_t ioapic_count;

static inline uint32_t ioapic_read(struct ioapic_state *io, uint8_t reg) {
    io->base[IOAPIC_REGSEL / 4] = reg;
    return io->base[IOAPIC_WIN / 4];
}

static inline void ioapic_write(struct ioapic_state *io, uint8_t reg, uint32_t val) {
    io->base[IOAPIC_REGSEL / 4] = reg;
    io->base[IOAPIC_WIN / 4] = val;
}

static struct ioapic_state *ioapic_for_gsi(uint32_t gsi) {
    for (uint32_t i = 0; i < ioapic_count; i++) {
        uint32_t base = ioapics[i].gsi_base;
        uint32_t max = ioapics[i].gsi_max;
        if (gsi >= base && gsi <= max) {
            return &ioapics[i];
        }
    }
    return 0;
}

void ioapic_route_gsi(uint32_t gsi, uint8_t vector, uint16_t flags, uint8_t cpu_apic_id) {
    struct ioapic_state *io = ioapic_for_gsi(gsi);
    if (!io) {
        return;
    }

    uint32_t redir = gsi - io->gsi_base;
    uint32_t low = vector;
    uint32_t high = ((uint32_t)cpu_apic_id) << 24;

    // Polarity/trigger from flags (ACPI spec)
    // flags bits: 0-1 polarity, 2-3 trigger
    uint16_t pol = flags & 0x3;
    uint16_t trg = (flags >> 2) & 0x3;
    if (pol == 0x3) {
        low |= (1u << 13); // active low
    }
    if (trg == 0x3) {
        low |= (1u << 15); // level triggered
    }

    ioapic_write(io, (uint8_t)(0x10 + redir * 2), low);
    ioapic_write(io, (uint8_t)(0x10 + redir * 2 + 1), high);
}

void ioapic_mask_gsi(uint32_t gsi) {
    struct ioapic_state *io = ioapic_for_gsi(gsi);
    if (!io) return;
    uint32_t redir = gsi - io->gsi_base;
    uint32_t low = ioapic_read(io, (uint8_t)(0x10 + redir * 2));
    low |= (1u << 16);
    ioapic_write(io, (uint8_t)(0x10 + redir * 2), low);
}

void ioapic_unmask_gsi(uint32_t gsi) {
    struct ioapic_state *io = ioapic_for_gsi(gsi);
    if (!io) return;
    uint32_t redir = gsi - io->gsi_base;
    uint32_t low = ioapic_read(io, (uint8_t)(0x10 + redir * 2));
    low &= ~(1u << 16);
    ioapic_write(io, (uint8_t)(0x10 + redir * 2), low);
}

void ioapic_init(void) {
    ioapic_count = 0;
    uint32_t count = madt_get_ioapic_count();
    for (uint32_t i = 0; i < count && i < 16; i++) {
        const struct madt_ioapic *m = madt_get_ioapic(i);
        vmm_map(m->addr, m->addr, VMM_FLAG_WRITE | VMM_FLAG_CACHE_DISABLE | VMM_FLAG_WRITE_THROUGH);
        ioapics[ioapic_count].base = (volatile uint32_t *)(uintptr_t)m->addr;
        ioapics[ioapic_count].gsi_base = m->gsi_base;

        uint32_t id = (ioapic_read(&ioapics[ioapic_count], 0x00) >> 24) & 0xF;
        uint32_t ver = ioapic_read(&ioapics[ioapic_count], 0x01);
        uint32_t max_redir = ((ver >> 16) & 0xFF) + 1;
        ioapics[ioapic_count].gsi_max = m->gsi_base + max_redir - 1;
        if (id != m->id) {
            kprint("IOAPIC: MADT id=%u hw id=%u\n", m->id, id);
        }

        for (uint32_t r = 0; r < max_redir; r++) {
            ioapic_write(&ioapics[ioapic_count], (uint8_t)(0x10 + r * 2), 1u << 16);
            ioapic_write(&ioapics[ioapic_count], (uint8_t)(0x10 + r * 2 + 1), 0);
        }
        ioapic_count++;
    }

    // Route legacy IRQs 0-15 to vectors 32-47
    uint8_t cpu_apic = 0;
    if (madt_get_lapic_count() > 0) {
        const struct madt_lapic *lap = madt_get_lapic(0);
        cpu_apic = lap ? lap->apic_id : 0;
    }

    for (uint8_t irq = 0; irq < 16; irq++) {
        const struct madt_iso *iso = madt_get_iso(irq);
        uint32_t gsi = iso ? iso->gsi : irq;
        uint16_t flags = iso ? iso->flags : 0;
        ioapic_route_gsi(gsi, (uint8_t)(32 + irq), flags, cpu_apic);
    }
}
