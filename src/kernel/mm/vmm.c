// Virtual memory manager (4-level paging)

#include <stdint.h>
#include "vmm.h"
#include "pmm.h"
#include "../../boot/early_serial.h"
#include "../../include/tater_trace.h"

#define PAGE_SIZE 4096ULL
#define PAGE_SIZE_2M (2ULL * 1024ULL * 1024ULL)
#define PAGE_SIZE_1G (1024ULL * 1024ULL * 1024ULL)
#define EARLY_TABLE_POOL_PAGES 128ULL

// UEFI memory types used by physmap bootstrap selection.
#define EFI_LOADER_CODE 1
#define EFI_LOADER_DATA 2
#define EFI_BOOT_SERVICES_CODE 3
#define EFI_BOOT_SERVICES_DATA 4
#define EFI_RUNTIME_SERVICES_CODE 5
#define EFI_RUNTIME_SERVICES_DATA 6
#define EFI_CONVENTIONAL_MEMORY 7
#define EFI_ACPI_RECLAIM_MEMORY 9
#define EFI_ACPI_MEMORY_NVS 10
#define EFI_PERSISTENT_MEMORY 14

static uint64_t *kernel_pml4;
static uint64_t kernel_pml4_phys;
static int vmm_ready;
static uint64_t early_identity_limit = 0x100000000ULL;
static uint64_t physmap_max = 0;
static uint64_t *mmio_pd = 0;
static uint64_t early_table_pool_used = 0;
static uint8_t early_table_pool[EARLY_TABLE_POOL_PAGES * PAGE_SIZE]
    __attribute__((aligned(PAGE_SIZE)));

static void vmm_stage(const char *tag, char marker) {
    if (TATER_BOOT_SERIAL_TRACE) {
        early_serial_puts(tag);
        early_serial_puts("\n");
    }
    early_debug_putc(marker);
}

extern char __kernel_start;
extern char __kernel_end;
extern char __kernel_lma_start;
extern char __text_start;
extern char __text_end;
extern char __text_lma_start;
extern char __rodata_start;
extern char __rodata_end;
extern char __rodata_lma_start;
extern char __data_start;
extern char __bss_end;
extern char __data_lma_start;
extern char __kernel_stack_base;
extern char __kernel_stack_top;
extern char __kernel_stack_lma_start;
extern char __kernel_stack_lma_end;
extern char __ist_stacks_start;
extern char __ist_stacks_end;
extern char __ist_stacks_lma_start;

static inline void write_cr3(uint64_t phys) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(phys));
}

static inline uint64_t read_cr3(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline uint64_t read_cr0(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr0, %0" : "=r"(v));
    return v;
}

static inline void write_cr0(uint64_t v) {
    __asm__ volatile("mov %0, %%cr0" : : "r"(v) : "memory");
}

static inline uint64_t read_msr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void write_msr(uint32_t msr, uint64_t v) {
    uint32_t lo = (uint32_t)v;
    uint32_t hi = (uint32_t)(v >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static void enable_cpu_page_protections(void) {
    uint64_t efer = read_msr(0xC0000080u);
    efer |= (1ULL << 11); /* EFER.NXE */
    write_msr(0xC0000080u, efer);

    uint64_t cr0 = read_cr0();
    cr0 |= (1ULL << 16); /* CR0.WP */
    write_cr0(cr0);
}

static void map_kernel_region(uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags) {
    if (!size) {
        return;
    }
    vmm_map_range(virt, phys, (size + PAGE_SIZE - 1) / PAGE_SIZE, flags);
}

uint64_t vmm_phys_to_virt(uint64_t phys) {
    return VMM_PHYSMAP_BASE + phys;
}

static uint64_t kernel_virt_to_phys(uint64_t virt) {
    uint64_t kernel_virt_base = (uint64_t)(uintptr_t)&__kernel_start;
    uint64_t kernel_phys_base = (uint64_t)(uintptr_t)&__kernel_lma_start;

    if (virt >= kernel_virt_base) {
        return kernel_phys_base + (virt - kernel_virt_base);
    }
    return virt;
}

static uint64_t *phys_to_virt(uint64_t phys) {
    if (!vmm_ready && phys < early_identity_limit) {
        return (uint64_t *)(uintptr_t)phys; // identity for early build
    }
    return (uint64_t *)(uintptr_t)vmm_phys_to_virt(phys);
}

static uint64_t alloc_early_table_pool(uint64_t **out_tbl) {
    if (!out_tbl || early_table_pool_used >= EARLY_TABLE_POOL_PAGES) {
        return 0;
    }

    uint64_t *tbl = (uint64_t *)(void *)(early_table_pool + early_table_pool_used * PAGE_SIZE);
    early_table_pool_used++;

    for (uint64_t i = 0; i < 512; i++) {
        tbl[i] = 0;
    }

    *out_tbl = tbl;
    return kernel_virt_to_phys((uint64_t)(uintptr_t)tbl);
}

static uint64_t alloc_table(void) {
    // While building new page tables, we only have the boot identity map.
    // Keep allocations low so tables are addressable before CR3 switch.
    uint64_t phys = 0;
    uint64_t *tbl = 0;
    if (vmm_ready) {
        // Keep page tables in low 4GB so physmap already covers them.
        phys = pmm_alloc_page_below(0x100000000ULL);
    } else {
        phys = alloc_early_table_pool(&tbl);
        if (!phys) {
            phys = pmm_alloc_early_low();
        }
    }
    if (!phys) {
        early_debug_putc('!');
        for (;;) {
            __asm__ volatile("hlt");
        }
    }
    if (!tbl) {
        tbl = phys_to_virt(phys);
        for (uint64_t i = 0; i < 512; i++) {
            tbl[i] = 0;
        }
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

static uint64_t *vmm_lookup_user_pte(uint64_t pml4_phys, uint64_t virt) {
    if (!pml4_phys || virt >= USER_VA_TOP) return 0;

    uint64_t *pml4 = phys_to_virt(pml4_phys);
    uint64_t pml4_i = (virt >> 39) & 0x1FF;
    uint64_t pdpt_i = (virt >> 30) & 0x1FF;
    uint64_t pd_i   = (virt >> 21) & 0x1FF;
    uint64_t pt_i   = (virt >> 12) & 0x1FF;

    if (!(pml4[pml4_i] & VMM_FLAG_PRESENT)) return 0;
    uint64_t *pdpt = phys_to_virt(pml4[pml4_i] & 0x000FFFFFFFFFF000ULL);
    if (!(pdpt[pdpt_i] & VMM_FLAG_PRESENT)) return 0;

    uint64_t pdpe = pdpt[pdpt_i];
    if (pdpe & VMM_FLAG_LARGE) return 0;

    uint64_t *pd = phys_to_virt(pdpe & 0x000FFFFFFFFFF000ULL);
    if (!(pd[pd_i] & VMM_FLAG_PRESENT)) return 0;

    uint64_t pde = pd[pd_i];
    if (pde & VMM_FLAG_LARGE) return 0;

    uint64_t *pt = phys_to_virt(pde & 0x000FFFFFFFFFF000ULL);
    if (!(pt[pt_i] & VMM_FLAG_PRESENT)) return 0;
    return &pt[pt_i];
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

    pt[pt_i] = (phys & 0x000FFFFFFFFFF000ULL) | (flags & VMM_PTE_FLAG_MASK) | VMM_FLAG_PRESENT;
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

    pd[pd_i] = (phys & 0x000FFFFFFFFFF000ULL) | (flags & VMM_PTE_FLAG_MASK) | VMM_FLAG_PRESENT | VMM_FLAG_LARGE;
}

static void map_range_2m(uint64_t virt, uint64_t phys, uint64_t bytes, uint64_t flags) {
    uint64_t pages_2m = (bytes + (PAGE_SIZE_2M - 1)) / PAGE_SIZE_2M;
    for (uint64_t i = 0; i < pages_2m; i++) {
        vmm_map_2m(virt, phys, flags);
        virt += PAGE_SIZE_2M;
        phys += PAGE_SIZE_2M;
    }
}

static int is_physmap_bootstrap_type(uint32_t type) {
    return type == EFI_LOADER_CODE ||
           type == EFI_LOADER_DATA ||
           type == EFI_BOOT_SERVICES_CODE ||
           type == EFI_BOOT_SERVICES_DATA ||
           type == EFI_RUNTIME_SERVICES_CODE ||
           type == EFI_RUNTIME_SERVICES_DATA ||
           type == EFI_CONVENTIONAL_MEMORY ||
           type == EFI_ACPI_RECLAIM_MEMORY ||
           type == EFI_ACPI_MEMORY_NVS ||
           type == EFI_PERSISTENT_MEMORY;
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
    if (virt >= USER_VA_TOP) return;

    uint64_t *pml4 = phys_to_virt(pml4_phys);
    uint64_t pml4_i = (virt >> 39) & 0x1FF;
    uint64_t pdpt_i = (virt >> 30) & 0x1FF;
    uint64_t pd_i   = (virt >> 21) & 0x1FF;
    uint64_t pt_i   = (virt >> 12) & 0x1FF;

    uint64_t *pdpt = get_or_alloc_table(pml4, pml4_i, flags);
    if (!pdpt) return;
    uint64_t *pd = get_or_alloc_table(pdpt, pdpt_i, flags);
    if (!pd) return;
    uint64_t *pt = get_or_alloc_table(pd, pd_i, flags);
    if (!pt) return;

    pt[pt_i] = (phys & 0x000FFFFFFFFFF000ULL) | (flags & VMM_PTE_FLAG_MASK) | VMM_FLAG_PRESENT;
}

uint64_t vmm_virt_to_phys_user(uint64_t pml4_phys, uint64_t virt) {
    if (!pml4_phys) return 0;
    if (virt >= USER_VA_TOP) return 0;

    uint64_t pml4_i = (virt >> 39) & 0x1FF;
    uint64_t pdpt_i = (virt >> 30) & 0x1FF;
    uint64_t pd_i   = (virt >> 21) & 0x1FF;
    uint64_t pt_i   = (virt >> 12) & 0x1FF;

    uint64_t *pml4 = phys_to_virt(pml4_phys);
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

uint64_t vmm_query_user_flags(uint64_t pml4_phys, uint64_t virt) {
    uint64_t *pte = vmm_lookup_user_pte(pml4_phys, virt);
    if (!pte) return 0;
    return *pte & VMM_PTE_FLAG_MASK;
}

void vmm_unmap_user(uint64_t pml4_phys, uint64_t virt) {
    if (!pml4_phys) return;
    if (virt >= USER_VA_TOP) return;

    uint64_t *pml4 = phys_to_virt(pml4_phys);
    uint64_t pml4_i = (virt >> 39) & 0x1FF;
    uint64_t pdpt_i = (virt >> 30) & 0x1FF;
    uint64_t pd_i   = (virt >> 21) & 0x1FF;
    uint64_t pt_i   = (virt >> 12) & 0x1FF;

    if (!(pml4[pml4_i] & VMM_FLAG_PRESENT)) return;
    uint64_t *pdpt = phys_to_virt(pml4[pml4_i] & 0x000FFFFFFFFFF000ULL);
    if (!(pdpt[pdpt_i] & VMM_FLAG_PRESENT)) return;
    uint64_t *pd = phys_to_virt(pdpt[pdpt_i] & 0x000FFFFFFFFFF000ULL);
    if (!(pd[pd_i] & VMM_FLAG_PRESENT)) return;
    if (pd[pd_i] & VMM_FLAG_LARGE) return;
    uint64_t *pt = phys_to_virt(pd[pd_i] & 0x000FFFFFFFFFF000ULL);
    if (!(pt[pt_i] & VMM_FLAG_PRESENT)) return;

    pt[pt_i] = 0;
    if ((read_cr3() & 0x000FFFFFFFFFF000ULL) == (pml4_phys & 0x000FFFFFFFFFF000ULL)) {
        __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
    }
}

int vmm_protect_user(uint64_t pml4_phys, uint64_t virt, uint64_t flags) {
    uint64_t *pte = vmm_lookup_user_pte(pml4_phys, virt);
    if (!pte) return -1;

    uint64_t entry = *pte;
    entry &= ~(VMM_FLAG_USER | VMM_FLAG_WRITE | VMM_FLAG_NO_EXECUTE);
    entry |= VMM_FLAG_PRESENT;
    entry |= (flags & (VMM_FLAG_USER | VMM_FLAG_WRITE | VMM_FLAG_NO_EXECUTE));
    *pte = entry;

    if ((read_cr3() & 0x000FFFFFFFFFF000ULL) == (pml4_phys & 0x000FFFFFFFFFF000ULL)) {
        __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
    }
    return 0;
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
    early_table_pool_used = 0;
    vmm_stage("VMM_00_ENTER", '0');
    if (handoff && handoff->boot_identity_limit) {
        early_identity_limit = handoff->boot_identity_limit;
    }
    kernel_pml4_phys = alloc_table();
    kernel_pml4 = phys_to_virt(kernel_pml4_phys);
    vmm_stage("VMM_01_PML4", '1');

    // Identity map low region using 4KB pages (allows MMIO cache flags)
    // Keep this small to avoid exhausting early page-table pool.
    map_range_2m(0x0, 0x0, early_identity_limit, VMM_FLAG_WRITE);
    vmm_stage("VMM_02_IDENT", '2');

    // Map physmap bootstrap coverage from UEFI map ranges (not hard-capped to boot identity).
    uint64_t mapped_max = 0;
    if (handoff && handoff->mmap_base && handoff->mmap_size && handoff->mmap_desc_size) {
        uint8_t *mmap = (uint8_t *)(uintptr_t)handoff->mmap_base;
        for (uint64_t off = 0; off < handoff->mmap_size; off += handoff->mmap_desc_size) {
            EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(mmap + off);
            if (!is_physmap_bootstrap_type(d->Type)) {
                continue;
            }
            if (d->NumberOfPages == 0 || d->NumberOfPages > (UINT64_MAX / PAGE_SIZE)) {
                continue;
            }

            uint64_t start = d->PhysicalStart;
            uint64_t end = start + d->NumberOfPages * PAGE_SIZE;
            if (end <= start) {
                continue;
            }

            uint64_t map_start = start & ~(PAGE_SIZE_2M - 1ULL);
            uint64_t map_end = (end + (PAGE_SIZE_2M - 1ULL)) & ~(PAGE_SIZE_2M - 1ULL);
            if (map_end <= map_start) {
                continue;
            }

            map_range_2m(VMM_PHYSMAP_BASE + map_start, map_start,
                         map_end - map_start, VMM_FLAG_WRITE | VMM_FLAG_NO_EXECUTE);
            if (map_end > mapped_max) {
                mapped_max = map_end;
            }
        }
    }

    if (mapped_max == 0) {
        // Fallback when no usable handoff map is present.
        mapped_max = early_identity_limit;
        map_range_2m(VMM_PHYSMAP_BASE, 0x0, mapped_max, VMM_FLAG_WRITE | VMM_FLAG_NO_EXECUTE);
    }
    physmap_max = mapped_max;
    vmm_stage("VMM_03_PHYSMAP", '3');

    // Map kernel sections with explicit permissions instead of one RWX blob.
    uint64_t text_start = (uint64_t)(uintptr_t)&__text_start;
    uint64_t text_end = (uint64_t)(uintptr_t)&__text_end;
    uint64_t text_phys = (uint64_t)(uintptr_t)&__text_lma_start;
    uint64_t ro_start = (uint64_t)(uintptr_t)&__rodata_start;
    uint64_t ro_end = (uint64_t)(uintptr_t)&__rodata_end;
    uint64_t ro_phys = (uint64_t)(uintptr_t)&__rodata_lma_start;
    uint64_t data_start = (uint64_t)(uintptr_t)&__data_start;
    uint64_t data_end = (uint64_t)(uintptr_t)&__bss_end;
    uint64_t data_phys = (uint64_t)(uintptr_t)&__data_lma_start;
    map_kernel_region(text_start, text_phys, text_end - text_start, 0);
    map_kernel_region(ro_start, ro_phys, ro_end - ro_start, VMM_FLAG_NO_EXECUTE);
    map_kernel_region(data_start, data_phys, data_end - data_start,
                      VMM_FLAG_WRITE | VMM_FLAG_NO_EXECUTE);
    vmm_stage("VMM_04_KERNEL", '4');

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
                      (map_size + PAGE_SIZE - 1) / PAGE_SIZE,
                      VMM_FLAG_WRITE | VMM_FLAG_NO_EXECUTE);
    }
    vmm_stage("VMM_05_STACK", '5');

    uint64_t ist_virt_start = (uint64_t)(uintptr_t)&__ist_stacks_start;
    uint64_t ist_virt_end = (uint64_t)(uintptr_t)&__ist_stacks_end;
    uint64_t ist_phys_start = (uint64_t)(uintptr_t)&__ist_stacks_lma_start;
    if (ist_virt_end > ist_virt_start) {
        map_kernel_region(ist_virt_start, ist_phys_start,
                          ist_virt_end - ist_virt_start,
                          VMM_FLAG_WRITE | VMM_FLAG_NO_EXECUTE);
    }


    // Map framebuffer at known virtual address
    if (handoff && handoff->fb_base && handoff->fb_width && handoff->fb_height) {
        uint64_t fb_size = handoff->fb_stride * handoff->fb_height * 4ULL;
        vmm_map_range(VMM_FB_BASE, handoff->fb_base,
                      (fb_size + PAGE_SIZE - 1) / PAGE_SIZE,
                      VMM_FLAG_WRITE | VMM_FLAG_NO_EXECUTE);
    }
    vmm_stage("VMM_06_FB", '6');

    // Pre-create page tables for MMIO window to avoid allocations at runtime.
    uint64_t mmio_pml4_i = (VMM_MMIO_BASE >> 39) & 0x1FF;
    uint64_t mmio_pdpt_i = (VMM_MMIO_BASE >> 30) & 0x1FF;
    uint64_t *mmio_pdpt = get_or_alloc_table(kernel_pml4, mmio_pml4_i, VMM_FLAG_WRITE);
    if (mmio_pdpt) {
        mmio_pd = get_or_alloc_table(mmio_pdpt, mmio_pdpt_i, VMM_FLAG_WRITE);
    }
    vmm_stage("VMM_07_MMIO", '7');

    vmm_stage("VMM_08_CR3_WRITE", '8');
    write_cr3(kernel_pml4_phys);
    enable_cpu_page_protections();
    vmm_stage("VMM_09_CR3_DONE", '9');
    vmm_ready = 1;
    kernel_pml4 = (uint64_t *)(uintptr_t)vmm_phys_to_virt(kernel_pml4_phys);
    vmm_stage("VMM_10_READY", 'A');
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

static inline void vmm_free_leaf_entry(uint64_t entry, uint64_t pages) {
    if (!(entry & VMM_FLAG_PRESENT) || (entry & VMM_FLAG_NOFREE)) {
        return;
    }
    uint64_t phys = entry & 0x000FFFFFFFFFF000ULL;
    if (!phys) {
        return;
    }
    pmm_free_pages(phys, pages);
}

static void vmm_destroy_pt(uint64_t pt_phys) {
    if (!pt_phys) {
        return;
    }
    uint64_t *pt = (uint64_t *)(uintptr_t)vmm_phys_to_virt(pt_phys);
    for (uint64_t i = 0; i < 512; i++) {
        vmm_free_leaf_entry(pt[i], 1);
    }
    pmm_free_page(pt_phys);
}

static void vmm_destroy_pd(uint64_t pd_phys) {
    if (!pd_phys) {
        return;
    }
    uint64_t *pd = (uint64_t *)(uintptr_t)vmm_phys_to_virt(pd_phys);
    for (uint64_t i = 0; i < 512; i++) {
        uint64_t entry = pd[i];
        if (!(entry & VMM_FLAG_PRESENT)) {
            continue;
        }
        if (entry & VMM_FLAG_LARGE) {
            vmm_free_leaf_entry(entry, PAGE_SIZE_2M / PAGE_SIZE);
            continue;
        }
        vmm_destroy_pt(entry & 0x000FFFFFFFFFF000ULL);
    }
    pmm_free_page(pd_phys);
}

static void vmm_destroy_pdpt(uint64_t pdpt_phys) {
    if (!pdpt_phys) {
        return;
    }
    uint64_t *pdpt = (uint64_t *)(uintptr_t)vmm_phys_to_virt(pdpt_phys);
    for (uint64_t i = 0; i < 512; i++) {
        uint64_t entry = pdpt[i];
        if (!(entry & VMM_FLAG_PRESENT)) {
            continue;
        }
        if (entry & VMM_FLAG_LARGE) {
            vmm_free_leaf_entry(entry, PAGE_SIZE_1G / PAGE_SIZE);
            continue;
        }
        vmm_destroy_pd(entry & 0x000FFFFFFFFFF000ULL);
    }
    pmm_free_page(pdpt_phys);
}

void vmm_destroy_address_space(uint64_t pml4_phys) {
    if (!pml4_phys) return;
    uint64_t *pml4 = (uint64_t *)(uintptr_t)vmm_phys_to_virt(pml4_phys);
    for (uint64_t i = 0; i < 256; i++) {
        uint64_t e = pml4[i];
        if (!(e & VMM_FLAG_PRESENT)) continue;
        if (e & VMM_FLAG_LARGE) {
            continue;
        }
        vmm_destroy_pdpt(e & 0x000FFFFFFFFFF000ULL);
    }
    pmm_free_page(pml4_phys);
}
