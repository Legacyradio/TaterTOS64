// FADT parser

#include <stdint.h>
#include "fadt.h"
#include "tables.h"
#include "../mm/vmm.h"

void kprint(const char *fmt, ...);

struct acpi_gas {
    uint8_t address_space_id;
    uint8_t register_bit_width;
    uint8_t register_bit_offset;
    uint8_t access_size;
    uint64_t address;
} __attribute__((packed));

struct acpi_fadt {
    struct acpi_sdt_header hdr;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t reserved1;
    uint8_t preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t acpi_enable;
    uint8_t acpi_disable;
    uint8_t s4bios_req;
    uint8_t pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;
    uint32_t pm2_cnt_blk;
    uint32_t pm_tmr_blk;
    uint32_t gpe0_blk;
    uint32_t gpe1_blk;
    uint8_t pm1_evt_len;
    uint8_t pm1_cnt_len;
    uint8_t pm2_cnt_len;
    uint8_t pm_tmr_len;
    uint8_t gpe0_blk_len;
    uint8_t gpe1_blk_len;
    uint8_t gpe1_base;
    uint8_t cst_cnt;
    uint16_t p_lvl2_lat;
    uint16_t p_lvl3_lat;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t duty_offset;
    uint8_t duty_width;
    uint8_t day_alrm;
    uint8_t mon_alrm;
    uint8_t century;
    uint16_t iapc_boot_arch;
    uint8_t reserved2;
    uint32_t flags;
    struct acpi_gas reset_reg;
    uint8_t reset_value;
    uint16_t arm_boot_arch;
    uint8_t fadt_minor_version;
    uint64_t x_firmware_ctrl;
    uint64_t x_dsdt;
    struct acpi_gas x_pm1a_evt_blk;
    struct acpi_gas x_pm1b_evt_blk;
    struct acpi_gas x_pm1a_cnt_blk;
    struct acpi_gas x_pm1b_cnt_blk;
    struct acpi_gas x_pm2_cnt_blk;
    struct acpi_gas x_pm_tmr_blk;
    struct acpi_gas x_gpe0_blk;
    struct acpi_gas x_gpe1_blk;
} __attribute__((packed));

static struct fadt_info g_fadt;

static uint64_t pick_gas_u64(uint32_t legacy, const struct acpi_gas *gas) {
    if (gas && gas->address) {
        return gas->address;
    }
    return legacy;
}

void fadt_init(void) {
    struct acpi_sdt_header *h = acpi_find_table("FACP");
    if (!h) {
        kprint("FADT: not found\n");
        return;
    }

    struct acpi_fadt *f = (struct acpi_fadt *)h;

    g_fadt.smi_cmd = f->smi_cmd;
    g_fadt.acpi_enable = f->acpi_enable;
    g_fadt.acpi_disable = f->acpi_disable;
    g_fadt.pm1a_evt_blk = (uint32_t)pick_gas_u64(f->pm1a_evt_blk, &f->x_pm1a_evt_blk);
    g_fadt.pm1b_evt_blk = (uint32_t)pick_gas_u64(f->pm1b_evt_blk, &f->x_pm1b_evt_blk);
    g_fadt.pm1a_cnt_blk = (uint32_t)pick_gas_u64(f->pm1a_cnt_blk, &f->x_pm1a_cnt_blk);
    g_fadt.pm1b_cnt_blk = (uint32_t)pick_gas_u64(f->pm1b_cnt_blk, &f->x_pm1b_cnt_blk);
    g_fadt.pm_tmr_blk = (uint32_t)pick_gas_u64(f->pm_tmr_blk, &f->x_pm_tmr_blk);
    g_fadt.gpe0_blk = (uint32_t)pick_gas_u64(f->gpe0_blk, &f->x_gpe0_blk);
    g_fadt.gpe1_blk = (uint32_t)pick_gas_u64(f->gpe1_blk, &f->x_gpe1_blk);
    g_fadt.gpe0_blk_len = f->gpe0_blk_len;
    g_fadt.gpe1_blk_len = f->gpe1_blk_len;
    g_fadt.gpe1_base = f->gpe1_base;
    g_fadt.reset_reg = (uint32_t)f->reset_reg.address;
    g_fadt.reset_reg_space = f->reset_reg.address_space_id;
    g_fadt.reset_reg_width = f->reset_reg.register_bit_width;
    g_fadt.reset_reg_offset = f->reset_reg.register_bit_offset;
    g_fadt.reset_reg_access = f->reset_reg.access_size;
    g_fadt.reset_val = f->reset_value;
    g_fadt.sci_int = (uint8_t)f->sci_int;

    if (f->x_dsdt) {
        g_fadt.dsdt_phys = f->x_dsdt;
    } else {
        g_fadt.dsdt_phys = f->dsdt;
    }

    g_fadt.flags = f->flags;

    kprint("FADT: SCI=%u PM1a_CNT=0x%x DSDT=0x%llx\n",
           g_fadt.sci_int, g_fadt.pm1a_cnt_blk, g_fadt.dsdt_phys);
}

const struct fadt_info *fadt_get_info(void) {
    return &g_fadt;
}

uint8_t fadt_get_sci_irq(void) {
    return g_fadt.sci_int;
}

uint32_t fadt_get_pm1a_cnt(void) {
    return g_fadt.pm1a_cnt_blk;
}

uint32_t fadt_get_gpe0_blk(void) {
    return g_fadt.gpe0_blk;
}

uint32_t fadt_get_gpe1_blk(void) {
    return g_fadt.gpe1_blk;
}

uint8_t fadt_get_gpe0_len(void) {
    return g_fadt.gpe0_blk_len;
}

uint8_t fadt_get_gpe1_len(void) {
    return g_fadt.gpe1_blk_len;
}

uint8_t fadt_get_gpe1_base(void) {
    return g_fadt.gpe1_base;
}

uint64_t fadt_get_dsdt_phys(void) {
    return g_fadt.dsdt_phys;
}
