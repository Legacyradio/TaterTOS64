// Physical memory manager (bitmap allocator)

#include <stdint.h>
#include "pmm.h"
#include "vmm.h"
#include "../../boot/early_serial.h"

void kprint(const char *fmt, ...);

#define PAGE_SIZE 4096ULL

// UEFI memory types (subset)
#define EFI_RESERVED_MEMORY_TYPE 0
#define EFI_LOADER_CODE 1
#define EFI_LOADER_DATA 2
#define EFI_BOOT_SERVICES_CODE 3
#define EFI_BOOT_SERVICES_DATA 4
#define EFI_RUNTIME_SERVICES_CODE 5
#define EFI_RUNTIME_SERVICES_DATA 6
#define EFI_CONVENTIONAL_MEMORY 7
#define EFI_UNUSABLE_MEMORY 8
#define EFI_ACPI_RECLAIM_MEMORY 9
#define EFI_ACPI_MEMORY_NVS 10
#define EFI_MEMORY_MAPPED_IO 11
#define EFI_MEMORY_MAPPED_IO_PORT_SPACE 12
#define EFI_PAL_CODE 13
#define EFI_PERSISTENT_MEMORY 14
#define EARLY_PHYS_LIMIT 0x100000000ULL
#define EARLY_POOL_MIN 0x00200000ULL
#define EARLY_POOL_MAX 0x40000000ULL
#define EARLY_POOL_SIZE (16ULL * 1024 * 1024)
#define SMP_TRAMPOLINE_PHYS 0x00008000ULL

static uint8_t *pmm_bitmap;
static uint64_t pmm_bitmap_bytes;
static uint64_t pmm_bitmap_phys;
static uint64_t pmm_total_pages;
static uint64_t pmm_used_pages;
static uint64_t early_pool_start;
static uint64_t early_pool_end;
static uint64_t early_pool_next;

static inline void bit_set(uint64_t idx) {
    pmm_bitmap[idx / 8] |= (uint8_t)(1u << (idx % 8));
}

static inline void bit_clear(uint64_t idx) {
    pmm_bitmap[idx / 8] &= (uint8_t)~(1u << (idx % 8));
}

static inline int bit_test(uint64_t idx) {
    return (pmm_bitmap[idx / 8] >> (idx % 8)) & 1u;
}

static uint64_t align_up(uint64_t v, uint64_t a) {
    return (v + a - 1) & ~(a - 1);
}

static uint64_t align_down(uint64_t v, uint64_t a) {
    return v & ~(a - 1);
}

static int is_usable_type(uint32_t type) {
    return type == EFI_CONVENTIONAL_MEMORY;
}

static uint64_t count_free_pages_below(uint64_t max_phys) {
    uint64_t max_page = max_phys / PAGE_SIZE;
    if (max_page > pmm_total_pages) {
        max_page = pmm_total_pages;
    }
    uint64_t free_pages = 0;
    for (uint64_t i = 0; i < max_page; i++) {
        if (!bit_test(i)) {
            free_pages++;
        }
    }
    return free_pages;
}

extern char __kernel_lma_start;
extern char __kernel_lma_end;
extern char __boot_lma_start;
extern char __boot_lma_end;

static int ranges_overlap(uint64_t a_start, uint64_t a_end, uint64_t b_start, uint64_t b_end) {
    return a_start < b_end && b_start < a_end;
}

static int find_bitmap_base(uint64_t start, uint64_t end,
                            uint64_t boot_start, uint64_t boot_end,
                            uint64_t kernel_start, uint64_t kernel_end,
                            uint64_t size, uint64_t *out_base) {
    struct range { uint64_t s, e; };
    struct range forbid[2];
    int count = 0;
    if (ranges_overlap(start, end, boot_start, boot_end)) {
        forbid[count++] = (struct range){boot_start, boot_end};
    }
    if (ranges_overlap(start, end, kernel_start, kernel_end)) {
        forbid[count++] = (struct range){kernel_start, kernel_end};
    }
    if (count == 2 && forbid[1].s < forbid[0].s) {
        struct range tmp = forbid[0];
        forbid[0] = forbid[1];
        forbid[1] = tmp;
    }

    uint64_t cursor = start;
    for (int i = 0; i < count; i++) {
        if (cursor + size <= forbid[i].s) {
            *out_base = cursor;
            return 1;
        }
        if (cursor < forbid[i].e) {
            cursor = forbid[i].e;
        }
    }
    if (cursor + size <= end) {
        *out_base = cursor;
        return 1;
    }
    return 0;
}

static void reserve_early_pool(void) {
    early_pool_start = 0;
    early_pool_end = 0;
    early_pool_next = 0;

    uint64_t pool_pages = EARLY_POOL_SIZE / PAGE_SIZE;
    uint64_t min_page = EARLY_POOL_MIN / PAGE_SIZE;
    uint64_t max_page = EARLY_POOL_MAX / PAGE_SIZE;
    if (max_page > pmm_total_pages) {
        max_page = pmm_total_pages;
    }
    if (min_page >= max_page) {
        return;
    }

    uint64_t run = 0;
    uint64_t start = 0;
    for (uint64_t i = min_page; i < max_page; i++) {
        if (!bit_test(i)) {
            if (run == 0) {
                start = i;
            }
            run++;
            if (run == pool_pages) {
                early_pool_start = start * PAGE_SIZE;
                early_pool_end = early_pool_start + EARLY_POOL_SIZE;
                early_pool_next = early_pool_start;
                for (uint64_t j = 0; j < pool_pages; j++) {
                    bit_set(start + j);
                }
                pmm_used_pages += pool_pages;
                return;
            }
        } else {
            run = 0;
        }
    }
    (void)pool_pages;
}

static void reserve_smp_trampoline(void) {
    uint64_t idx = SMP_TRAMPOLINE_PHYS / PAGE_SIZE;
    if (idx < pmm_total_pages && !bit_test(idx)) {
        bit_set(idx);
        pmm_used_pages++;
    }
}

void pmm_init(struct fry_handoff *handoff) {
    pmm_bitmap = 0;
    pmm_bitmap_bytes = 0;
    pmm_total_pages = 0;
    pmm_used_pages = 0;
    pmm_bitmap_phys = 0;

    if (!handoff || handoff->mmap_base == 0 || handoff->mmap_size == 0 || handoff->mmap_desc_size == 0) {
        early_debug_putc('0');
        return;
    }

    // Find max physical address to size bitmap
    uint64_t max_phys = 0;
    uint8_t *mmap = (uint8_t *)(uintptr_t)handoff->mmap_base;
    for (uint64_t off = 0; off < handoff->mmap_size; off += handoff->mmap_desc_size) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(mmap + off);
        uint64_t end = d->PhysicalStart + d->NumberOfPages * PAGE_SIZE;
        if (end > max_phys) {
            max_phys = end;
        }
    }

    pmm_total_pages = (max_phys + PAGE_SIZE - 1) / PAGE_SIZE;
    pmm_bitmap_bytes = (pmm_total_pages + 7) / 8;

    uint64_t kernel_start = (uint64_t)(uintptr_t)&__kernel_lma_start;
    uint64_t kernel_end   = (uint64_t)(uintptr_t)&__kernel_lma_end;
    uint64_t boot_start   = (uint64_t)(uintptr_t)&__boot_lma_start;
    uint64_t boot_end     = (uint64_t)(uintptr_t)&__boot_lma_end;

    // Find a place for the bitmap inside usable memory (avoid kernel/boot ranges)
    uint64_t best_base = 0;
    uint64_t best_size = 0;
    for (uint64_t off = 0; off < handoff->mmap_size; off += handoff->mmap_desc_size) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(mmap + off);
        if (!is_usable_type(d->Type)) {
            continue;
        }
        uint64_t start = d->PhysicalStart;
        uint64_t end = d->PhysicalStart + d->NumberOfPages * PAGE_SIZE;
        if (start >= EARLY_PHYS_LIMIT) {
            continue;
        }
        if (end > EARLY_PHYS_LIMIT) {
            end = EARLY_PHYS_LIMIT;
        }
        uint64_t size = end - start;
        if (size == 0) {
            continue;
        }
        uint64_t candidate = 0;
        if (!find_bitmap_base(start, end, boot_start, boot_end, kernel_start, kernel_end,
                              pmm_bitmap_bytes, &candidate)) {
            continue;
        }
        if (size > best_size) {
            best_size = size;
            best_base = candidate;
        }
    }

    if (best_base == 0) {
        early_debug_putc('B');
        return;
    }

    uint64_t bitmap_phys = align_up(best_base, PAGE_SIZE);
    pmm_bitmap_phys = bitmap_phys;
    pmm_bitmap = (uint8_t *)(uintptr_t)bitmap_phys; // assumes identity map for low memory

    // Mark all pages as used
    for (uint64_t i = 0; i < pmm_total_pages; i++) {
        bit_set(i);
    }
    pmm_used_pages = pmm_total_pages;

    // Free usable pages
    int freed_any = 0;
    for (uint64_t off = 0; off < handoff->mmap_size; off += handoff->mmap_desc_size) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(mmap + off);
        if (!is_usable_type(d->Type)) {
            continue;
        }
        uint64_t start = align_up(d->PhysicalStart, PAGE_SIZE);
        uint64_t end = align_down(d->PhysicalStart + d->NumberOfPages * PAGE_SIZE, PAGE_SIZE);
        for (uint64_t p = start; p < end; p += PAGE_SIZE) {
            uint64_t idx = p / PAGE_SIZE;
            if (idx < pmm_total_pages) {
                bit_clear(idx);
                pmm_used_pages--;
                freed_any = 1;
            }
        }
    }

    // Reserve kernel + boot image pages so nothing allocates over them
    uint64_t k_start = align_down(kernel_start, PAGE_SIZE);
    uint64_t k_end = align_up(kernel_end, PAGE_SIZE);
    for (uint64_t p = k_start; p < k_end; p += PAGE_SIZE) {
        uint64_t idx = p / PAGE_SIZE;
        if (idx < pmm_total_pages && !bit_test(idx)) {
            bit_set(idx);
            pmm_used_pages++;
        }
    }
    uint64_t b_start = align_down(boot_start, PAGE_SIZE);
    uint64_t b_end = align_up(boot_end, PAGE_SIZE);
    for (uint64_t p = b_start; p < b_end; p += PAGE_SIZE) {
        uint64_t idx = p / PAGE_SIZE;
        if (idx < pmm_total_pages && !bit_test(idx)) {
            bit_set(idx);
            pmm_used_pages++;
        }
    }

    // Never allocate physical page 0 (avoid 0 as valid address).
    if (!bit_test(0)) {
        bit_set(0);
        pmm_used_pages++;
    }

    // Reserve bitmap pages
    uint64_t bitmap_pages = (pmm_bitmap_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t p = 0; p < bitmap_pages; p++) {
        uint64_t idx = (bitmap_phys / PAGE_SIZE) + p;
        if (!bit_test(idx)) {
            bit_set(idx);
            pmm_used_pages++;
        }
    }
    reserve_early_pool();
    reserve_smp_trampoline();
    uint64_t free_total = pmm_total_pages - pmm_used_pages;
    uint64_t free_low = count_free_pages_below(0x100000000ULL);
    kprint("PMM: free_pages=%llu free_low4g=%llu\n",
           (unsigned long long)free_total, (unsigned long long)free_low);
    early_debug_putc(freed_any ? 'u' : 'U');
    early_debug_putc('b');
}

void pmm_relocate_bitmap(void) {
    if (!pmm_bitmap_phys) {
        return;
    }
    // After VMM init, use higher-half physmap for kernel access under user CR3.
    pmm_bitmap = (uint8_t *)(uintptr_t)vmm_phys_to_virt(pmm_bitmap_phys);
}

uint64_t pmm_alloc_page(void) {
    for (uint64_t i = 1; i < pmm_total_pages; i++) {
        if (!bit_test(i)) {
            bit_set(i);
            pmm_used_pages++;
            return i * PAGE_SIZE;
        }
    }
    return 0;
}

uint64_t pmm_alloc_early_low(void) {
    if (!early_pool_start || early_pool_next >= early_pool_end) {
        return 0;
    }
    uint64_t phys = early_pool_next;
    early_pool_next += PAGE_SIZE;
    return phys;
}

uint64_t pmm_alloc_page_below(uint64_t max_phys) {
    uint64_t max_page = max_phys / PAGE_SIZE;
    if (max_page > pmm_total_pages) {
        max_page = pmm_total_pages;
    }
    for (uint64_t i = 1; i < max_page; i++) {
        if (!bit_test(i)) {
            bit_set(i);
            pmm_used_pages++;
            return i * PAGE_SIZE;
        }
    }
    return 0;
}

uint64_t pmm_alloc_pages(uint64_t count) {
    if (count == 0) {
        return 0;
    }

    uint64_t run = 0;
    uint64_t start = 0;
    // Keep page 0 reserved-by-convention so physical 0 never aliases
    // the allocator's failure sentinel.
    for (uint64_t i = 1; i < pmm_total_pages; i++) {
        if (!bit_test(i)) {
            if (run == 0) {
                start = i;
            }
            run++;
            if (run == count) {
                for (uint64_t j = 0; j < count; j++) {
                    bit_set(start + j);
                }
                pmm_used_pages += count;
                return start * PAGE_SIZE;
            }
        } else {
            run = 0;
        }
    }
    return 0;
}

void pmm_free_page(uint64_t phys) {
    uint64_t idx = phys / PAGE_SIZE;
    if (idx < pmm_total_pages && bit_test(idx)) {
        bit_clear(idx);
        pmm_used_pages--;
    }
}

uint64_t pmm_get_total_pages(void) {
    return pmm_total_pages;
}

uint64_t pmm_get_used_pages(void) {
    return pmm_used_pages;
}
