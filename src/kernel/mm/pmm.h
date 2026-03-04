#ifndef TATER_PMM_H
#define TATER_PMM_H

#include <stdint.h>
#include "../../boot/efi_handoff.h"

void pmm_init(struct fry_handoff *handoff);
void pmm_relocate_bitmap(void);
uint64_t pmm_alloc_page(void);
uint64_t pmm_alloc_early_low(void);
uint64_t pmm_alloc_page_below(uint64_t max_phys);
uint64_t pmm_alloc_pages(uint64_t count);
void pmm_free_page(uint64_t phys);
uint64_t pmm_get_total_pages(void);
uint64_t pmm_get_used_pages(void);

#endif
