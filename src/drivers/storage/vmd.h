#ifndef TATER_VMD_H
#define TATER_VMD_H

#include <stdint.h>

/* Initialise the Intel VMD controller.
 * Handles two generations:
 *   - Newer Intel VMD (class=01:04): cfgbar = BAR0+BAR1 (large ECAM, >= 1 MB).
 *   - Older DID=0x1911 (class=08:80): cfgbar from scratch register at MEMBAR0+0x6C0.
 * After a successful call vmd_ready()==1 and vmd_cfg_read32/write32 may be
 * used by the NVMe driver to enumerate child devices without touching pci_info[]. */
void vmd_init(void);

int      vmd_ready(void);
uint8_t  vmd_controller_count(void);
int      vmd_select_controller(uint8_t index);
uint8_t  vmd_selected_controller(void);
uint64_t vmd_cfgbar_base(void);
uint64_t vmd_cfgbar_size(void);
uint8_t  vmd_bus_start(void);
uint16_t vmd_bus_count(void);

uint32_t vmd_cfg_read32 (uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     vmd_cfg_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);

#endif
