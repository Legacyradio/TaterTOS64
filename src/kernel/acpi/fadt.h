#ifndef TATER_ACPI_FADT_H
#define TATER_ACPI_FADT_H

#include <stdint.h>

struct fadt_info {
    uint32_t smi_cmd;
    uint8_t acpi_enable;
    uint8_t acpi_disable;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;
    uint32_t pm_tmr_blk;
    uint32_t gpe0_blk;
    uint32_t gpe1_blk;
    uint8_t gpe0_blk_len;
    uint8_t gpe1_blk_len;
    uint8_t gpe1_base;
    uint32_t reset_reg;
    uint8_t reset_reg_space;
    uint8_t reset_reg_width;
    uint8_t reset_reg_offset;
    uint8_t reset_reg_access;
    uint8_t reset_val;
    uint8_t sci_int;
    uint64_t dsdt_phys;
    uint32_t flags;
};

void fadt_init(void);
const struct fadt_info *fadt_get_info(void);
uint8_t fadt_get_sci_irq(void);
uint32_t fadt_get_pm1a_cnt(void);
uint32_t fadt_get_gpe0_blk(void);
uint32_t fadt_get_gpe1_blk(void);
uint8_t fadt_get_gpe0_len(void);
uint8_t fadt_get_gpe1_len(void);
uint8_t fadt_get_gpe1_base(void);
uint64_t fadt_get_dsdt_phys(void);

#endif
