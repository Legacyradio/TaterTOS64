// Physical memory manager (buddy allocator)

#include <stdint.h>
#include "pmm.h"
#include "vmm.h"
#include "../../drivers/smp/spinlock.h"
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
#define EARLY_POOL_SIZE (16ULL * 1024 * 1024)
#define SMP_TRAMPOLINE_PHYS 0x00008000ULL

#define PMM_META_ORDER_MASK 0x3Fu
#define PMM_META_KIND_MASK 0xC0u
#define PMM_META_FREE_BASE 0x00u
#define PMM_META_USED_BASE 0x40u
#define PMM_META_RESERVED_BASE 0x80u
#define PMM_META_TAIL 0xFFu
#define PMM_INVALID_PAGE UINT64_MAX

struct phys_range {
    uint64_t start;
    uint64_t end;
};

static uint8_t *pmm_page_meta;
static uint64_t *pmm_free_next;
static uint64_t *pmm_free_heads;
static uint64_t pmm_metadata_bytes;
static uint64_t pmm_metadata_phys;
static uint64_t pmm_total_pages;
static uint64_t pmm_used_pages;
static uint64_t early_alloc_limit;
static uint64_t early_pool_start;
static uint64_t early_pool_end;
static uint64_t early_pool_next;
static uint8_t pmm_max_order;
static spinlock_t g_pmm_lock = {0};

static uint64_t align_up(uint64_t v, uint64_t a) {
    return (v + a - 1) & ~(a - 1);
}

static uint64_t align_down(uint64_t v, uint64_t a) {
    return v & ~(a - 1);
}

static uint64_t clamp_early_alloc_limit(uint64_t limit) {
    if (limit == 0 || limit > EARLY_PHYS_LIMIT) {
        limit = EARLY_PHYS_LIMIT;
    }
    return align_down(limit, PAGE_SIZE);
}

static uint8_t meta_make_free(uint8_t order) {
    return (uint8_t)(PMM_META_FREE_BASE | order);
}

static uint8_t meta_make_used(uint8_t order) {
    return (uint8_t)(PMM_META_USED_BASE | order);
}

static uint8_t meta_make_reserved(uint8_t order) {
    return (uint8_t)(PMM_META_RESERVED_BASE | order);
}

static int meta_is_tail(uint8_t meta) {
    return meta == PMM_META_TAIL;
}

static int meta_is_free(uint8_t meta) {
    return meta != PMM_META_TAIL && (meta & PMM_META_KIND_MASK) == PMM_META_FREE_BASE;
}

static int meta_is_used(uint8_t meta) {
    return meta != PMM_META_TAIL && (meta & PMM_META_KIND_MASK) == PMM_META_USED_BASE;
}

static uint8_t meta_order(uint8_t meta) {
    return (uint8_t)(meta & PMM_META_ORDER_MASK);
}

static uint64_t order_pages(uint8_t order) {
    return 1ULL << order;
}

static uint8_t compute_max_order(uint64_t pages) {
    uint8_t order = 0;
    while (order < 63) {
        uint64_t next_pages = 1ULL << (order + 1);
        if (next_pages == 0 || next_pages > pages) {
            break;
        }
        order++;
    }
    return order;
}

static uint8_t order_for_pages(uint64_t count) {
    uint8_t order = 0;
    uint64_t pages = 1;
    while (pages < count && order < pmm_max_order) {
        pages <<= 1;
        order++;
    }
    if (pages < count) {
        return 0xFFu;
    }
    return order;
}

static uint64_t metadata_size_bytes(uint64_t pages, uint8_t max_order) {
    uint64_t size = 0;
    size += pages * sizeof(uint8_t);
    size = align_up(size, sizeof(uint64_t));
    size += pages * sizeof(uint64_t);
    size = align_up(size, sizeof(uint64_t));
    size += ((uint64_t)max_order + 1ULL) * sizeof(uint64_t);
    return align_up(size, PAGE_SIZE);
}

static void bind_metadata(void *base_ptr) {
    uint8_t *base = (uint8_t *)base_ptr;
    uint64_t off = 0;

    pmm_page_meta = base + off;
    off += pmm_total_pages * sizeof(uint8_t);

    off = align_up(off, sizeof(uint64_t));
    pmm_free_next = (uint64_t *)(void *)(base + off);
    off += pmm_total_pages * sizeof(uint64_t);

    off = align_up(off, sizeof(uint64_t));
    pmm_free_heads = (uint64_t *)(void *)(base + off);
}

static int is_usable_type(uint32_t type) {
    return type == EFI_CONVENTIONAL_MEMORY;
}

extern char __kernel_lma_start;
extern char __kernel_lma_end;
extern char __boot_lma_start;
extern char __boot_lma_end;

static void sort_ranges(struct phys_range *ranges, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        for (uint32_t j = i + 1; j < count; j++) {
            if (ranges[j].start < ranges[i].start) {
                struct phys_range tmp = ranges[i];
                ranges[i] = ranges[j];
                ranges[j] = tmp;
            }
        }
    }
}

static uint32_t normalize_ranges(struct phys_range *ranges, uint32_t count) {
    sort_ranges(ranges, count);

    uint32_t out = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (ranges[i].end <= ranges[i].start) {
            continue;
        }
        if (out == 0 || ranges[i].start > ranges[out - 1].end) {
            ranges[out++] = ranges[i];
            continue;
        }
        if (ranges[i].end > ranges[out - 1].end) {
            ranges[out - 1].end = ranges[i].end;
        }
    }
    return out;
}

static int find_region_base(uint64_t start, uint64_t end,
                            const struct phys_range *guards, uint32_t guard_count,
                            uint64_t size, uint64_t *out_base) {
    if (!out_base || size == 0) {
        return 0;
    }

    uint64_t cursor = align_up(start, PAGE_SIZE);
    end = align_down(end, PAGE_SIZE);
    if (cursor >= end || size > (end - cursor)) {
        return 0;
    }

    for (uint32_t i = 0; i < guard_count; i++) {
        if (guards[i].end <= cursor) {
            continue;
        }
        if (guards[i].start >= end) {
            break;
        }
        if (cursor + size <= guards[i].start) {
            *out_base = cursor;
            return 1;
        }
        if (guards[i].end > cursor) {
            cursor = align_up(guards[i].end, PAGE_SIZE);
            if (cursor >= end || size > (end - cursor)) {
                return 0;
            }
        }
    }

    if (cursor + size <= end) {
        *out_base = cursor;
        return 1;
    }
    return 0;
}

static void set_block_meta(uint64_t start_page, uint8_t order, uint8_t head_meta) {
    uint64_t pages = order_pages(order);
    pmm_page_meta[start_page] = head_meta;
    for (uint64_t i = 1; i < pages; i++) {
        pmm_page_meta[start_page + i] = PMM_META_TAIL;
    }
}

static void insert_free_block(uint64_t start_page, uint8_t order) {
    set_block_meta(start_page, order, meta_make_free(order));
    pmm_free_next[start_page] = pmm_free_heads[order];
    pmm_free_heads[order] = start_page;
}

static int remove_free_block(uint64_t start_page, uint8_t order) {
    uint64_t prev = PMM_INVALID_PAGE;
    uint64_t cur = pmm_free_heads[order];
    while (cur != PMM_INVALID_PAGE) {
        if (cur == start_page) {
            uint64_t next = pmm_free_next[cur];
            if (prev == PMM_INVALID_PAGE) {
                pmm_free_heads[order] = next;
            } else {
                pmm_free_next[prev] = next;
            }
            pmm_free_next[cur] = PMM_INVALID_PAGE;
            return 1;
        }
        prev = cur;
        cur = pmm_free_next[cur];
    }
    return 0;
}

static void free_block(uint64_t start_page, uint8_t order) {
    uint64_t block_pages = order_pages(order);
    pmm_used_pages -= block_pages;

    while (order < pmm_max_order) {
        uint64_t buddy = start_page ^ block_pages;
        if (buddy >= pmm_total_pages) {
            break;
        }

        uint8_t buddy_meta = pmm_page_meta[buddy];
        if (!meta_is_free(buddy_meta) || meta_order(buddy_meta) != order) {
            break;
        }
        if (!remove_free_block(buddy, order)) {
            break;
        }

        if (buddy < start_page) {
            start_page = buddy;
        }
        order++;
        block_pages = order_pages(order);
    }

    insert_free_block(start_page, order);
}

static void release_block(uint64_t start_page, uint8_t order) {
    set_block_meta(start_page, order, meta_make_used(order));
    free_block(start_page, order);
}

static void release_range_pages(uint64_t start_page, uint64_t end_page) {
    while (start_page < end_page) {
        uint8_t order = pmm_max_order;
        while (order > 0) {
            uint64_t pages = order_pages(order);
            if ((start_page & (pages - 1ULL)) == 0 && start_page + pages <= end_page) {
                break;
            }
            order--;
        }
        release_block(start_page, order);
        start_page += order_pages(order);
    }
}

static void release_descriptor_usable(uint64_t start, uint64_t end,
                                      const struct phys_range *guards, uint32_t guard_count,
                                      int *freed_any) {
    uint64_t cursor = align_up(start, PAGE_SIZE);
    end = align_down(end, PAGE_SIZE);
    if (cursor >= end) {
        return;
    }

    for (uint32_t i = 0; i < guard_count && cursor < end; i++) {
        if (guards[i].end <= cursor) {
            continue;
        }
        if (guards[i].start >= end) {
            break;
        }
        if (guards[i].start > cursor) {
            release_range_pages(cursor / PAGE_SIZE, guards[i].start / PAGE_SIZE);
            if (freed_any) {
                *freed_any = 1;
            }
        }
        if (guards[i].end > cursor) {
            cursor = guards[i].end;
        }
    }

    if (cursor < end) {
        release_range_pages(cursor / PAGE_SIZE, end / PAGE_SIZE);
        if (freed_any) {
            *freed_any = 1;
        }
    }
}

static int block_can_satisfy(uint64_t start_page, uint8_t block_order, uint8_t target_order,
                             uint64_t min_page, uint64_t max_page) {
    uint64_t block_pages = order_pages(block_order);
    uint64_t target_pages = order_pages(target_order);
    uint64_t block_end = start_page + block_pages;
    uint64_t range_start = start_page;
    uint64_t range_end = block_end;

    if (range_start < min_page) {
        range_start = min_page;
    }
    if (range_end > max_page) {
        range_end = max_page;
    }
    if (range_end <= range_start || range_end - range_start < target_pages) {
        return 0;
    }

    uint64_t candidate = align_up(range_start, target_pages);
    return candidate + target_pages <= range_end;
}

static uint64_t alloc_pages_range_locked(uint64_t count, uint64_t min_phys, uint64_t max_phys) {
    if (count == 0 || pmm_total_pages == 0) {
        return 0;
    }

    uint64_t min_page = align_up(min_phys, PAGE_SIZE) / PAGE_SIZE;
    uint64_t max_page = max_phys / PAGE_SIZE;
    if (min_page < 1) {
        min_page = 1;
    }
    if (max_page > pmm_total_pages) {
        max_page = pmm_total_pages;
    }
    if (min_page >= max_page) {
        return 0;
    }

    uint8_t target_order = order_for_pages(count);
    if (target_order == 0xFFu) {
        return 0;
    }

    for (uint8_t order = target_order; order <= pmm_max_order; order++) {
        uint64_t prev = PMM_INVALID_PAGE;
        uint64_t cur = pmm_free_heads[order];
        while (cur != PMM_INVALID_PAGE) {
            uint64_t next = pmm_free_next[cur];
            if (!block_can_satisfy(cur, order, target_order, min_page, max_page)) {
                prev = cur;
                cur = next;
                continue;
            }

            if (prev == PMM_INVALID_PAGE) {
                pmm_free_heads[order] = next;
            } else {
                pmm_free_next[prev] = next;
            }
            pmm_free_next[cur] = PMM_INVALID_PAGE;

            uint64_t block = cur;
            uint8_t split_order = order;
            while (split_order > target_order) {
                split_order--;
                uint64_t half_pages = order_pages(split_order);
                uint64_t left = block;
                uint64_t right = block + half_pages;
                int left_ok = block_can_satisfy(left, split_order, target_order, min_page, max_page);
                int right_ok = block_can_satisfy(right, split_order, target_order, min_page, max_page);

                if (!left_ok && !right_ok) {
                    insert_free_block(block, order);
                    return 0;
                }

                if (left_ok) {
                    set_block_meta(left, split_order, meta_make_free(split_order));
                    insert_free_block(right, split_order);
                    block = left;
                } else {
                    set_block_meta(right, split_order, meta_make_free(split_order));
                    insert_free_block(left, split_order);
                    block = right;
                }
            }

            set_block_meta(block, target_order, meta_make_used(target_order));
            pmm_used_pages += order_pages(target_order);
            return block * PAGE_SIZE;
        }
    }

    return 0;
}

static void reserve_early_pool(void) {
    early_pool_start = 0;
    early_pool_end = 0;
    early_pool_next = 0;

    uint64_t pool_limit = clamp_early_alloc_limit(early_alloc_limit);
    if (pool_limit <= EARLY_POOL_MIN) {
        return;
    }

    for (uint64_t pool_size = EARLY_POOL_SIZE; ; pool_size >>= 1) {
        uint64_t pool_pages = pool_size / PAGE_SIZE;
        uint64_t phys = alloc_pages_range_locked(pool_pages, EARLY_POOL_MIN, pool_limit);
        if (phys) {
            early_pool_start = phys;
            early_pool_end = phys + pool_size;
            early_pool_next = phys;
            return;
        }
        if (pool_size == PAGE_SIZE) {
            return;
        }
    }
}

static uint64_t count_free_pages_below(uint64_t max_phys) {
    uint64_t max_page = max_phys / PAGE_SIZE;
    if (max_page > pmm_total_pages) {
        max_page = pmm_total_pages;
    }

    uint64_t free_pages = 0;
    uint64_t page = 0;
    while (page < max_page) {
        uint8_t meta = pmm_page_meta[page];
        if (meta_is_tail(meta)) {
            page++;
            continue;
        }

        uint64_t block_pages = order_pages(meta_order(meta));
        uint64_t visible_pages = block_pages;
        if (page + visible_pages > max_page) {
            visible_pages = max_page - page;
        }
        if (meta_is_free(meta)) {
            free_pages += visible_pages;
        }
        page += block_pages;
    }
    return free_pages;
}

void pmm_init(struct fry_handoff *handoff) {
    pmm_page_meta = 0;
    pmm_free_next = 0;
    pmm_free_heads = 0;
    pmm_metadata_bytes = 0;
    pmm_metadata_phys = 0;
    pmm_total_pages = 0;
    pmm_used_pages = 0;
    early_alloc_limit = EARLY_PHYS_LIMIT;
    early_pool_start = 0;
    early_pool_end = 0;
    early_pool_next = 0;
    pmm_max_order = 0;

    if (!handoff || handoff->mmap_base == 0 || handoff->mmap_size == 0 || handoff->mmap_desc_size == 0) {
        early_debug_putc('0');
        return;
    }
    if (handoff->boot_identity_limit) {
        early_alloc_limit = clamp_early_alloc_limit(handoff->boot_identity_limit);
    }

    uint64_t max_phys = 0;
    uint8_t *mmap = (uint8_t *)(uintptr_t)handoff->mmap_base;
    for (uint64_t off = 0; off < handoff->mmap_size; off += handoff->mmap_desc_size) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(mmap + off);
        if (!is_usable_type(d->Type)) {
            continue;
        }
        if (d->NumberOfPages == 0 || d->NumberOfPages > (UINT64_MAX / PAGE_SIZE)) {
            continue;
        }
        uint64_t end = d->PhysicalStart + d->NumberOfPages * PAGE_SIZE;
        if (end > max_phys) {
            max_phys = end;
        }
    }

    pmm_total_pages = (max_phys + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pmm_total_pages == 0) {
        early_debug_putc('0');
        return;
    }

    pmm_max_order = compute_max_order(pmm_total_pages);
    pmm_metadata_bytes = metadata_size_bytes(pmm_total_pages, pmm_max_order);

    uint64_t kernel_start = align_down((uint64_t)(uintptr_t)&__kernel_lma_start, PAGE_SIZE);
    uint64_t kernel_end = align_up((uint64_t)(uintptr_t)&__kernel_lma_end, PAGE_SIZE);
    uint64_t boot_start = align_down((uint64_t)(uintptr_t)&__boot_lma_start, PAGE_SIZE);
    uint64_t boot_end = align_up((uint64_t)(uintptr_t)&__boot_lma_end, PAGE_SIZE);

    struct phys_range metadata_guards[4];
    uint32_t metadata_guard_count = 0;
    metadata_guards[metadata_guard_count++] = (struct phys_range){0, PAGE_SIZE};
    metadata_guards[metadata_guard_count++] = (struct phys_range){SMP_TRAMPOLINE_PHYS, SMP_TRAMPOLINE_PHYS + PAGE_SIZE};
    metadata_guards[metadata_guard_count++] = (struct phys_range){boot_start, boot_end};
    metadata_guards[metadata_guard_count++] = (struct phys_range){kernel_start, kernel_end};
    metadata_guard_count = normalize_ranges(metadata_guards, metadata_guard_count);

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
        if (end <= start) {
            continue;
        }

        uint64_t candidate = 0;
        if (!find_region_base(start, end, metadata_guards, metadata_guard_count,
                              pmm_metadata_bytes, &candidate)) {
            continue;
        }

        uint64_t size = end - start;
        if (size > best_size) {
            best_size = size;
            best_base = candidate;
        }
    }

    if (best_base == 0) {
        early_debug_putc('B');
        return;
    }

    pmm_metadata_phys = best_base;
    bind_metadata((void *)(uintptr_t)pmm_metadata_phys);

    for (uint64_t i = 0; i < pmm_total_pages; i++) {
        pmm_page_meta[i] = meta_make_reserved(0);
        pmm_free_next[i] = PMM_INVALID_PAGE;
    }
    for (uint64_t order = 0; order <= pmm_max_order; order++) {
        pmm_free_heads[order] = PMM_INVALID_PAGE;
    }
    pmm_used_pages = pmm_total_pages;

    struct phys_range guards[5];
    uint32_t guard_count = 0;
    guards[guard_count++] = (struct phys_range){0, PAGE_SIZE};
    guards[guard_count++] = (struct phys_range){SMP_TRAMPOLINE_PHYS, SMP_TRAMPOLINE_PHYS + PAGE_SIZE};
    guards[guard_count++] = (struct phys_range){boot_start, boot_end};
    guards[guard_count++] = (struct phys_range){kernel_start, kernel_end};
    guards[guard_count++] = (struct phys_range){pmm_metadata_phys, pmm_metadata_phys + pmm_metadata_bytes};
    guard_count = normalize_ranges(guards, guard_count);

    int freed_any = 0;
    for (uint64_t off = 0; off < handoff->mmap_size; off += handoff->mmap_desc_size) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(mmap + off);
        if (!is_usable_type(d->Type) || d->NumberOfPages == 0) {
            continue;
        }
        uint64_t start = d->PhysicalStart;
        uint64_t end = d->PhysicalStart + d->NumberOfPages * PAGE_SIZE;
        release_descriptor_usable(start, end, guards, guard_count, &freed_any);
    }

    reserve_early_pool();

    uint64_t free_total = pmm_total_pages - pmm_used_pages;
    uint64_t free_low = count_free_pages_below(0x100000000ULL);
    kprint("PMM: free_pages=%llu free_low4g=%llu\n",
           (unsigned long long)free_total, (unsigned long long)free_low);
    early_debug_putc(freed_any ? 'u' : 'U');
    early_debug_putc('b');
}

void pmm_relocate_bitmap(void) {
    if (!pmm_metadata_phys) {
        return;
    }
    bind_metadata((void *)(uintptr_t)vmm_phys_to_virt(pmm_metadata_phys));
}

void pmm_debug_dump_state(const char *tag, uint64_t order) {
    early_serial_puts(tag ? tag : "PMM_DEBUG");
    early_serial_puts(" meta=");
    early_serial_puthex64(pmm_metadata_phys);
    early_serial_puts(" page_meta=");
    early_serial_puthex64((uint64_t)(uintptr_t)pmm_page_meta);
    early_serial_puts(" free_next=");
    early_serial_puthex64((uint64_t)(uintptr_t)pmm_free_next);
    early_serial_puts(" free_heads=");
    early_serial_puthex64((uint64_t)(uintptr_t)pmm_free_heads);
    if (pmm_free_heads && order <= pmm_max_order) {
        early_serial_puts(" head=");
        early_serial_puthex64(pmm_free_heads[order]);
    }
    early_serial_puts("\n");
}

uint64_t pmm_alloc_page(void) {
    uint64_t irqf = spin_lock_irqsave(&g_pmm_lock);
    uint64_t phys = alloc_pages_range_locked(1, PAGE_SIZE, UINT64_MAX);
    spin_unlock_irqrestore(&g_pmm_lock, irqf);
    return phys;
}

uint64_t pmm_alloc_early_low(void) {
    if (early_pool_start && early_pool_next < early_pool_end) {
        uint64_t phys = early_pool_next;
        early_pool_next += PAGE_SIZE;
        return phys;
    }

    uint64_t pool_limit = clamp_early_alloc_limit(early_alloc_limit);
    if (pool_limit <= EARLY_POOL_MIN) {
        return 0;
    }

    uint64_t irqf = spin_lock_irqsave(&g_pmm_lock);
    uint64_t phys = alloc_pages_range_locked(1, EARLY_POOL_MIN, pool_limit);
    spin_unlock_irqrestore(&g_pmm_lock, irqf);
    return phys;
}

uint64_t pmm_alloc_page_below(uint64_t max_phys) {
    uint64_t irqf = spin_lock_irqsave(&g_pmm_lock);
    uint64_t phys = alloc_pages_range_locked(1, PAGE_SIZE, max_phys);
    spin_unlock_irqrestore(&g_pmm_lock, irqf);
    return phys;
}

uint64_t pmm_alloc_pages(uint64_t count) {
    if (count == 0) {
        return 0;
    }
    uint64_t irqf = spin_lock_irqsave(&g_pmm_lock);
    uint64_t phys = alloc_pages_range_locked(count, PAGE_SIZE, UINT64_MAX);
    spin_unlock_irqrestore(&g_pmm_lock, irqf);
    return phys;
}

uint64_t pmm_alloc_pages_below(uint64_t count, uint64_t max_phys) {
    if (count == 0) {
        return 0;
    }
    uint64_t irqf = spin_lock_irqsave(&g_pmm_lock);
    uint64_t phys = alloc_pages_range_locked(count, PAGE_SIZE, max_phys);
    spin_unlock_irqrestore(&g_pmm_lock, irqf);
    return phys;
}

void pmm_free_pages(uint64_t phys, uint64_t count) {
    (void)count;
    if (!phys) {
        return;
    }

    uint64_t irqf = spin_lock_irqsave(&g_pmm_lock);
    uint64_t start_page = phys / PAGE_SIZE;
    if (start_page >= pmm_total_pages) {
        spin_unlock_irqrestore(&g_pmm_lock, irqf);
        return;
    }

    uint8_t meta = pmm_page_meta[start_page];
    if (!meta_is_used(meta)) {
        spin_unlock_irqrestore(&g_pmm_lock, irqf);
        return;
    }

    free_block(start_page, meta_order(meta));
    spin_unlock_irqrestore(&g_pmm_lock, irqf);
}

void pmm_free_page(uint64_t phys) {
    pmm_free_pages(phys, 1);
}

uint64_t pmm_get_total_pages(void) {
    return pmm_total_pages;
}

uint64_t pmm_get_used_pages(void) {
    uint64_t irqf = spin_lock_irqsave(&g_pmm_lock);
    uint64_t used = pmm_used_pages;
    spin_unlock_irqrestore(&g_pmm_lock, irqf);
    return used;
}
