#ifndef TATER_VMM_H
#define TATER_VMM_H

#include <stdint.h>
#include "../../boot/efi_handoff.h"

#define VMM_FLAG_PRESENT      0x001ULL
#define VMM_FLAG_WRITE        0x002ULL
#define VMM_FLAG_USER         0x004ULL
#define VMM_FLAG_WRITE_THROUGH 0x008ULL
#define VMM_FLAG_CACHE_DISABLE 0x010ULL
#define VMM_FLAG_LARGE        0x080ULL
#define VMM_FLAG_NOFREE       0x200ULL
#define VMM_FLAG_NO_EXECUTE   (1ULL << 63)

#define USER_VA_TOP     0x0000800000000000ULL
#define KERNEL_VMA_BASE 0xFFFFFFFF80000000ULL
#define VMM_FB_BASE     0xFFFFFFFFB0000000ULL
#define VMM_PHYSMAP_BASE 0xFFFF800000000000ULL
#define VMM_MMIO_BASE   0xFFFFFFFFC0000000ULL

void vmm_init(struct fry_handoff *handoff);
void vmm_map(uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_map_range(uint64_t virt, uint64_t phys, uint64_t pages, uint64_t flags);
uint64_t vmm_create_address_space(void);
void vmm_map_user(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags);
uint64_t vmm_virt_to_phys_user(uint64_t pml4_phys, uint64_t virt);
void vmm_unmap_user(uint64_t pml4_phys, uint64_t virt);
void vmm_unmap(uint64_t virt);
uint64_t vmm_virt_to_phys(uint64_t virt);
uint64_t vmm_phys_to_virt(uint64_t phys);
uint64_t vmm_get_kernel_pml4_phys(void);
void vmm_destroy_address_space(uint64_t pml4_phys);
void vmm_ensure_physmap(uint64_t phys_end);
void vmm_ensure_physmap_uc(uint64_t phys_end);
uint64_t vmm_map_mmio_2m(uint64_t phys);

#endif
