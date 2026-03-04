// Virtual memory manager (4-level paging)

#include <stdint.h>
#include "vmm.h"
#include "pmm.h"
#include "../../boot/early_serial.h"

#define PAGE_SIZE 4096ULL

static uint64_t *kernel_pml4;
static uint64_t kernel_pml4_phys;
static int vmm_ready;
static uint64_t early_identity_limit = 0x40000000ULL;
static uint64_t physmap_max = 0;
static uint64_t *mmio_pd = 0;

extern char __kernel_start;
extern char __kernel_end;
extern char __kernel_lma_start;
extern char __kernel_stack_base;
extern char __kernel_stack_top;
extern char __kernel_stack_lma_start;
extern char __kernel_stack_lma_end;

static inline void write_cr3(uint64_t phys) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(phys));
}

uint64_t vmm_phys_to_virt(uint64_t phys) {
    return VMM_PHYSMAP_BASE + phys;
}

static uint64_t *phys_to_virt(uint64_t phys) {
    if (!vmm_ready && phys < 0x100000000ULL) {
        return (uint64_t *)(uintptr_t)phys; // identity for early build
    }
    return (uint64_t *)(uintptr_t)vmm_phys_to_virt(phys);
}

static uint64_t alloc_table(void) {
    // While building new page tables, we only have the boot identity map.
    // Keep allocations low so tables are addressable before CR3 switch.
    uint64_t phys = 0;
    if (vmm_ready) {
        // Keep page tables in low 4GB so physmap already covers them.
        phys = pmm_alloc_page_below(0x100000000ULL);
    } else {
        phys = pmm_alloc_early_low();
    }
    if (!phys) {
        early_debug_putc('!');
        for (;;) {
            __asm__ volatile("hlt");
        }
    }
    uint64_t *tbl = phys_to_virt(phys);
    for (uint64_t i = 0; i < 512; i++) {
        tbl[i] = 0;
    }
    return phys;
}

static uint64_t *get_or_alloc_table(uint64_t *parent, uint64_t idx, uint64_t flags) {
    if (!(parent[idx] & VMM_FLAG_PRESENT)) {
        uint64_t phys = alloc_table();
        if (!phys) {
            return 0;
        }
        parent[idx] = phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITE | (flags & VMM_FLAG_USER);
    }
    return phys_to_virt(parent[idx] & 0x000FFFFFFFFFF000ULL);
}

void vmm_map(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t pml4_i = (virt >> 39) & 0x1FF;
    uint64_t pdpt_i = (virt >> 30) & 0x1FF;
    uint64_t pd_i   = (virt >> 21) & 0x1FF;
    uint64_t pt_i   = (virt >> 12) & 0x1FF;

    uint64_t *pml4 = kernel_pml4;
    uint64_t *pdpt = get_or_alloc_table(pml4, pml4_i, flags);
    if (!pdpt) return;
    uint64_t *pd = get_or_alloc_table(pdpt, pdpt_i, flags);
    if (!pd) return;
    uint64_t *pt = get_or_alloc_table(pd, pd_i, flags);
    if (!pt) return;

    pt[pt_i] = (phys & 0x000FFFFFFFFFF000ULL) | (flags & 0xFFFULL) | VMM_FLAG_PRESENT;
}

void vmm_map_range(uint64_t virt, uint64_t phys, uint64_t pages, uint64_t flags) {
    for (uint64_t i = 0; i < pages; i++) {
        vmm_map(virt + i * PAGE_SIZE, phys + i * PAGE_SIZE, flags);
    }
}

static void vmm_map_2m(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t pml4_i = (virt >> 39) & 0x1FF;
    uint64_t pdpt_i = (virt >> 30) & 0x1FF;
    uint64_t pd_i   = (virt >> 21) & 0x1FF;

    uint64_t *pml4 = kernel_pml4;
    uint64_t *pdpt = get_or_alloc_table(pml4, pml4_i, flags);
    if (!pdpt) return;
    uint64_t *pd = get_or_alloc_table(pdpt, pdpt_i, flags);
    if (!pd) return;

    pd[pd_i] = (phys & 0x000FFFFFFFFFF000ULL) | (flags & 0xFFFULL) | VMM_FLAG_PRESENT | VMM_FLAG_LARGE;
}

static void map_range_2m(uint64_t virt, uint64_t phys, uint64_t bytes, uint64_t flags) {
    uint64_t pages_2m = (bytes + (2 * 1024 * 1024 - 1)) / (2 * 1024 * 1024);
    for (uint64_t i = 0; i < pages_2m; i++) {
        vmm_map_2m(virt, phys, flags);
        virt += 2 * 1024 * 1024;
        phys += 2 * 1024 * 1024;
    }
}

uint64_t vmm_create_address_space(void) {
    uint64_t new_pml4_phys = alloc_table();
    if (!new_pml4_phys) {
        return 0;
    }
    uint64_t *new_pml4 = phys_to_virt(new_pml4_phys);

    // Copy kernel higher-half mappings (entries 256-511)
    for (uint64_t i = 256; i < 512; i++) {
        new_pml4[i] = kernel_pml4[i];
    }

    return new_pml4_phys;
}

void vmm_map_user(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *pml4 = phys_to_virt(pml4_phys);
    uint64_t pml4_i = (virt >> 39) & 0x1FF;
    uint64_t pdpt_i = (virt >> 30) & 0x1FF;
    uint64_t pd_i   = (virt >> 21) & 0x1FF;
    uint64_t pt_i   = (virt >> 12) & 0x1FF;

    uint64_t *pdpt = get_or_alloc_table(pml4, pml4_i, flags | VMM_FLAG_USER);
    if (!pdpt) return;
    uint64_t *pd = get_or_alloc_table(pdpt, pdpt_i, flags | VMM_FLAG_USER);
    if (!pd) return;
    uint64_t *pt = get_or_alloc_table(pd, pd_i, flags | VMM_FLAG_USER);
    if (!pt) return;

    pt[pt_i] = (phys & 0x000FFFFFFFFFF000ULL) | (flags & 0xFFFULL) | VMM_FLAG_PRESENT | VMM_FLAG_USER;
}

void vmm_unmap(uint64_t virt) {
    uint64_t pml4_i = (virt >> 39) & 0x1FF;
    uint64_t pdpt_i = (virt >> 30) & 0x1FF;
    uint64_t pd_i   = (virt >> 21) & 0x1FF;
    uint64_t pt_i   = (virt >> 12) & 0x1FF;

    uint64_t *pml4 = kernel_pml4;
    if (!(pml4[pml4_i] & VMM_FLAG_PRESENT)) return;
    uint64_t *pdpt = phys_to_virt(pml4[pml4_i] & 0x000FFFFFFFFFF000ULL);
    if (!(pdpt[pdpt_i] & VMM_FLAG_PRESENT)) return;
    uint64_t *pd = phys_to_virt(pdpt[pdpt_i] & 0x000FFFFFFFFFF000ULL);
    if (!(pd[pd_i] & VMM_FLAG_PRESENT)) return;
    uint64_t *pt = phys_to_virt(pd[pd_i] & 0x000FFFFFFFFFF000ULL);
    if (!(pt[pt_i] & VMM_FLAG_PRESENT)) return;
    pt[pt_i] = 0;
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

uint64_t vmm_virt_to_phys(uint64_t virt) {
    // physmap direct: only the dedicated physmap window, not all higher-half VA.
    if (virt >= VMM_PHYSMAP_BASE && virt < KERNEL_VMA_BASE) {
        return virt - VMM_PHYSMAP_BASE;
    }

    uint64_t pml4_i = (virt >> 39) & 0x1FF;
    uint64_t pdpt_i = (virt >> 30) & 0x1FF;
    uint64_t pd_i   = (virt >> 21) & 0x1FF;
    uint64_t pt_i   = (virt >> 12) & 0x1FF;

    uint64_t *pml4 = kernel_pml4;
    if (!(pml4[pml4_i] & VMM_FLAG_PRESENT)) return 0;
    uint64_t *pdpt = phys_to_virt(pml4[pml4_i] & 0x000FFFFFFFFFF000ULL);
    if (!(pdpt[pdpt_i] & VMM_FLAG_PRESENT)) return 0;

    uint64_t pdpe = pdpt[pdpt_i];
    if (pdpe & VMM_FLAG_LARGE) {
        return (pdpe & 0x000FFFFFFFFFF000ULL) + (virt & 0x3FFFFFFFULL);
    }

    uint64_t *pd = phys_to_virt(pdpe & 0x000FFFFFFFFFF000ULL);
    if (!(pd[pd_i] & VMM_FLAG_PRESENT)) return 0;

    uint64_t pde = pd[pd_i];
    if (pde & VMM_FLAG_LARGE) {
        return (pde & 0x000FFFFFFFFFF000ULL) + (virt & 0x1FFFFFULL);
    }

    uint64_t *pt = phys_to_virt(pde & 0x000FFFFFFFFFF000ULL);
    if (!(pt[pt_i] & VMM_FLAG_PRESENT)) return 0;
    return (pt[pt_i] & 0x000FFFFFFFFFF000ULL) + (virt & 0xFFFULL);
}

void vmm_init(struct fry_handoff *handoff) {
    vmm_ready = 0;
    if (handoff && handoff->boot_identity_limit) {
        early_identity_limit = handoff->boot_identity_limit;
    }
    kernel_pml4_phys = alloc_table();
    kernel_pml4 = phys_to_virt(kernel_pml4_phys);

    // Identity map low region using 4KB pages (allows MMIO cache flags)
    // Keep this small to avoid exhausting early page-table pool.
    map_range_2m(0x0, 0x0, early_identity_limit, VMM_FLAG_WRITE);

    // Map physmap for all RAM (use max physical from memory map)
    uint64_t max_phys = 0;
    if (handoff && handoff->mmap_base && handoff->mmap_size && handoff->mmap_desc_size) {
        uint8_t *mmap = (uint8_t *)(uintptr_t)handoff->mmap_base;
        for (uint64_t off = 0; off < handoff->mmap_size; off += handoff->mmap_desc_size) {
            EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(mmap + off);
            uint64_t end = d->PhysicalStart + d->NumberOfPages * PAGE_SIZE;
            if (end > max_phys) max_phys = end;
        }
    }
    if (max_phys == 0) max_phys = 4ULL * 1024 * 1024 * 1024;
    if (max_phys > early_identity_limit) {
        max_phys = early_identity_limit;
    }
    map_range_2m(VMM_PHYSMAP_BASE, 0x0, max_phys, VMM_FLAG_WRITE | VMM_FLAG_NO_EXECUTE);
    physmap_max = max_phys;

    // Map kernel higher-half (excluding separate stack section)
    uint64_t kstart = (uint64_t)(uintptr_t)&__kernel_start;
    uint64_t kend = (uint64_t)(uintptr_t)&__kernel_end;
    uint64_t kphys = (uint64_t)(uintptr_t)&__kernel_lma_start;
    uint64_t ksize = kend - kstart;
    map_range_2m(kstart, kphys, ksize, VMM_FLAG_WRITE);

    // Map kernel stack separately using 4KB pages, leave a guard page at the base.
    uint64_t s_virt_base = (uint64_t)(uintptr_t)&__kernel_stack_base;
    uint64_t s_virt_top = (uint64_t)(uintptr_t)&__kernel_stack_top;
    uint64_t s_phys_base = (uint64_t)(uintptr_t)&__kernel_stack_lma_start;
    uint64_t s_phys_end = (uint64_t)(uintptr_t)&__kernel_stack_lma_end;
    uint64_t s_size = s_virt_top - s_virt_base;
    if (s_size >= PAGE_SIZE && (s_phys_end - s_phys_base) >= s_size) {
        uint64_t guard = PAGE_SIZE;
        uint64_t map_size = s_size - guard;
        vmm_map_range(s_virt_base + guard, s_phys_base + guard,
                      (map_size + PAGE_SIZE - 1) / PAGE_SIZE, VMM_FLAG_WRITE);
    }


    // Map framebuffer at known virtual address
    if (handoff && handoff->fb_base && handoff->fb_width && handoff->fb_height) {
        uint64_t fb_size = handoff->fb_stride * handoff->fb_height * 4ULL;
        vmm_map_range(VMM_FB_BASE, handoff->fb_base, (fb_size + PAGE_SIZE - 1) / PAGE_SIZE, VMM_FLAG_WRITE);
    }

    // Pre-create page tables for MMIO window to avoid allocations at runtime.
    uint64_t mmio_pml4_i = (VMM_MMIO_BASE >> 39) & 0x1FF;
    uint64_t mmio_pdpt_i = (VMM_MMIO_BASE >> 30) & 0x1FF;
    uint64_t *mmio_pdpt = get_or_alloc_table(kernel_pml4, mmio_pml4_i, VMM_FLAG_WRITE);
    if (mmio_pdpt) {
        mmio_pd = get_or_alloc_table(mmio_pdpt, mmio_pdpt_i, VMM_FLAG_WRITE);
    }

    write_cr3(kernel_pml4_phys);
    vmm_ready = 1;
    kernel_pml4 = (uint64_t *)(uintptr_t)vmm_phys_to_virt(kernel_pml4_phys);
}

void vmm_ensure_physmap(uint64_t phys_end) {
    if (phys_end == 0) {
        return;
    }
    // Map only the 2MB region that contains phys_end-1 to avoid
    // mapping massive sparse ranges (e.g. high MMIO BARs).
    uint64_t last = phys_end - 1;
    uint64_t start = last & ~0x1FFFFFULL;
    uint64_t end = start + 0x200000ULL;
    map_range_2m(VMM_PHYSMAP_BASE + start, start, end - start,
                 VMM_FLAG_WRITE | VMM_FLAG_NO_EXECUTE);
    if (end > physmap_max) {
        physmap_max = end;
    }
}

// Like vmm_ensure_physmap but maps with cache-disable + write-through flags.
// Use this for MMIO regions (e.g. PCI config space) where WB caching is wrong.
// Does NOT update physmap_max (MMIO is not RAM).
void vmm_ensure_physmap_uc(uint64_t phys_end) {
    if (phys_end == 0) {
        return;
    }
    uint64_t last = phys_end - 1;
    uint64_t start = last & ~0x1FFFFFULL;
    uint64_t end = start + 0x200000ULL;
    map_range_2m(VMM_PHYSMAP_BASE + start, start, end - start,
                 VMM_FLAG_WRITE | VMM_FLAG_NO_EXECUTE |
                 VMM_FLAG_CACHE_DISABLE | VMM_FLAG_WRITE_THROUGH);
}

uint64_t vmm_map_mmio_2m(uint64_t phys) {
    uint64_t base = phys & ~0x1FFFFFULL;
    uint64_t off = phys - base;
    if (mmio_pd) {
        uint64_t pd_i = (VMM_MMIO_BASE >> 21) & 0x1FF;
        mmio_pd[pd_i] = (base & 0x000FFFFFFFFFF000ULL) |
                        (VMM_FLAG_WRITE | VMM_FLAG_NO_EXECUTE |
                         VMM_FLAG_CACHE_DISABLE | VMM_FLAG_WRITE_THROUGH |
                         VMM_FLAG_PRESENT | VMM_FLAG_LARGE);
        __asm__ volatile("invlpg (%0)" : : "r"(VMM_MMIO_BASE) : "memory");
    } else {
        vmm_map_2m(VMM_MMIO_BASE, base,
                   VMM_FLAG_WRITE | VMM_FLAG_NO_EXECUTE |
                   VMM_FLAG_CACHE_DISABLE | VMM_FLAG_WRITE_THROUGH);
    }
    return VMM_MMIO_BASE + off;
}

uint64_t vmm_get_kernel_pml4_phys(void) {
    return kernel_pml4_phys;
}

static void free_page_table(uint64_t phys, int level) {
    if (!phys) return;
    uint64_t *tbl = (uint64_t *)(uintptr_t)vmm_phys_to_virt(phys);
    for (uint64_t i = 0; i < 512; i++) {
        uint64_t e = tbl[i];
        if (!(e & VMM_FLAG_PRESENT)) continue;
        uint64_t child = e & 0x000FFFFFFFFFF000ULL;
        if (level == 1) {
            if (!(e & VMM_FLAG_NOFREE)) {
                pmm_free_page(child);
            }
            continue;
        }
        if (e & VMM_FLAG_LARGE) {
            pmm_free_page(child);
            continue;
        }
        free_page_table(child, level - 1);
    }
    pmm_free_page(phys);
}

void vmm_destroy_address_space(uint64_t pml4_phys) {
    if (!pml4_phys) return;
    uint64_t *pml4 = (uint64_t *)(uintptr_t)vmm_phys_to_virt(pml4_phys);
    for (uint64_t i = 0; i < 256; i++) {
        uint64_t e = pml4[i];
        if (!(e & VMM_FLAG_PRESENT)) continue;
        uint64_t child = e & 0x000FFFFFFFFFF000ULL;
        if (e & VMM_FLAG_LARGE) {
            pmm_free_page(child);
            continue;
        }
        free_page_table(child, 3);
    }
    pmm_free_page(pml4_phys);
}
