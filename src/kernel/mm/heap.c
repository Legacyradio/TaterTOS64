// Kernel heap (Phase 1: bump allocator)

#include <stdint.h>
#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include "../../boot/early_serial.h"

#define PAGE_SIZE 4096ULL
#define HEAP_MAGIC 0xC0FFEE42u

struct heap_hdr {
    uint32_t magic;
    uint32_t size;
};

static uint64_t heap_start;
static uint64_t heap_end;
static uint64_t heap_curr;

static void *memcpy8(void *dst, const void *src, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint64_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

static uint64_t align_up(uint64_t v, uint64_t a) {
    return (v + a - 1) & ~(a - 1);
}

void heap_init(void) {
    heap_start = 0;
    heap_end = 0;
    heap_curr = 0;

    early_serial_puts("K_HEAP_ENTER\n");
    early_debug_puts("K_HEAP_ENTER\n");

    // Try to allocate an initial contiguous heap region
    uint64_t sizes[] = {256, 128, 64, 32, 16, 8, 4, 2, 1};
    uint64_t pages = 0;
    for (uint64_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        uint64_t phys = pmm_alloc_pages(sizes[i]);
        if (phys) {
            pages = sizes[i];
            heap_start = vmm_phys_to_virt(phys);
            break;
        }
    }

    if (heap_start == 0 || pages == 0) {
        early_serial_puts("K_HEAP_FAIL\n");
        early_debug_puts("K_HEAP_FAIL\n");
        return;
    }

    heap_end = heap_start + pages * PAGE_SIZE;
    heap_curr = heap_start;
    early_serial_puts("K_HEAP_OK\n");
    early_debug_puts("K_HEAP_OK\n");
}

void *kmalloc(uint64_t size) {
    if (size == 0) {
        return 0;
    }

    uint64_t total = sizeof(struct heap_hdr) + size;
    uint64_t aligned = align_up(total, 16);
    if (heap_curr + aligned > heap_end) {
        return 0;
    }

    struct heap_hdr *hdr = (struct heap_hdr *)(uintptr_t)heap_curr;
    hdr->magic = HEAP_MAGIC;
    hdr->size = (uint32_t)size;

    void *user = (void *)((uintptr_t)heap_curr + sizeof(struct heap_hdr));
    heap_curr += aligned;
    return user;
}

void kfree(void *ptr) {
    (void)ptr; // bump allocator: no free in Phase 1
}

void *krealloc(void *ptr, uint64_t new_size) {
    if (!ptr) {
        return kmalloc(new_size);
    }
    if (new_size == 0) {
        return 0;
    }

    struct heap_hdr *hdr = (struct heap_hdr *)((uintptr_t)ptr - sizeof(struct heap_hdr));
    uint64_t old_size = 0;
    if (hdr->magic == HEAP_MAGIC) {
        old_size = hdr->size;
    }

    void *new_ptr = kmalloc(new_size);
    if (!new_ptr) {
        return 0;
    }

    uint64_t copy = old_size < new_size ? old_size : new_size;
    if (copy) {
        memcpy8(new_ptr, ptr, copy);
    }
    return new_ptr;
}
