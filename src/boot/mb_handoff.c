// Multiboot2 handoff shim: build fry_handoff and jump to kernel

#include <stdint.h>
#include "efi_handoff.h"
#include "early_serial.h"

void _fry_start(struct fry_handoff *handoff);

#define MB2_MAGIC 0x36d76289u

struct mb2_tag {
    uint32_t type;
    uint32_t size;
};

struct mb2_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
};

struct mb2_mmap_entry {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t zero;
};

struct mb2_tag_fb {
    uint32_t type;
    uint32_t size;
    uint64_t addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t  bpp;
    uint8_t  fb_type;
    uint16_t reserved;
    uint8_t  red_pos;
    uint8_t  red_size;
    uint8_t  green_pos;
    uint8_t  green_size;
    uint8_t  blue_pos;
    uint8_t  blue_size;
};

struct mb2_tag_acpi {
    uint32_t type;
    uint32_t size;
    uint8_t  rsdp[0];
};

extern char __kernel_start;
extern char __kernel_lma_start;

static uint64_t virt_to_phys(const void *p) {
    uint64_t v = (uint64_t)(uintptr_t)p;
    uint64_t vbase = (uint64_t)(uintptr_t)&__kernel_start;
    uint64_t lbase = (uint64_t)(uintptr_t)&__kernel_lma_start;
    if (v >= vbase) {
        return (v - vbase) + lbase;
    }
    return v;
}

static struct fry_handoff g_handoff;
static EFI_MEMORY_DESCRIPTOR g_mmap[256];

void _mb_start(void *mb_info, uint32_t magic) {
    early_serial_init();
    early_serial_puts("MB_START\n");
    early_debug_putc('m');
    if (magic != MB2_MAGIC || !mb_info) {
        early_serial_puts("MB_BAD_MAGIC\n");
        early_debug_putc('!');
        _fry_start(0);
        for (;;) { __asm__ volatile("hlt"); }
    }

    uint8_t *base = (uint8_t *)mb_info;
    uint32_t total = *(uint32_t *)(base + 0);
    uint32_t off = 8;

    for (int i = 0; i < (int)sizeof(g_handoff); i++) {
        ((uint8_t *)&g_handoff)[i] = 0;
    }

    uint32_t mmap_count = 0;
    uint32_t mmap_entry_size = 0;

    while (off + 8 <= total) {
        struct mb2_tag *tag = (struct mb2_tag *)(base + off);
        if (tag->type == 0) break;

        if (tag->type == 6) { // memory map
            struct mb2_tag_mmap *m = (struct mb2_tag_mmap *)tag;
            mmap_entry_size = m->entry_size;
            uint8_t *p = (uint8_t *)m + sizeof(*m);
            uint8_t *end = (uint8_t *)m + m->size;
            while (p + m->entry_size <= end && mmap_count < 256) {
                struct mb2_mmap_entry *e = (struct mb2_mmap_entry *)p;
                EFI_MEMORY_DESCRIPTOR *d = &g_mmap[mmap_count++];
                d->Type = (e->type == 1) ? 7 : 0; // EFI_CONVENTIONAL_MEMORY or RESERVED
                d->PhysicalStart = e->addr;
                d->VirtualStart = 0;
                d->NumberOfPages = e->len / 4096;
                d->Attribute = 0;
                p += m->entry_size;
            }
        } else if (tag->type == 8) { // framebuffer
            struct mb2_tag_fb *fb = (struct mb2_tag_fb *)tag;
            g_handoff.fb_base = fb->addr;
            g_handoff.fb_width = fb->width;
            g_handoff.fb_height = fb->height;
            g_handoff.fb_stride = fb->pitch;
            if (fb->fb_type == 1) {
                g_handoff.fb_pixel_format = (fb->red_pos == 0) ? 1 : 0;
            } else {
                g_handoff.fb_pixel_format = 0;
            }
        } else if (tag->type == 14 || tag->type == 15) { // ACPI RSDP
            struct mb2_tag_acpi *a = (struct mb2_tag_acpi *)tag;
            g_handoff.rsdp_phys = (uint64_t)(uintptr_t)a->rsdp;
        }

        uint32_t sz = (tag->size + 7) & ~7u;
        off += sz;
    }

    if (mmap_count > 0) {
        g_handoff.mmap_base = virt_to_phys(g_mmap);
        g_handoff.mmap_size = (uint64_t)mmap_count * sizeof(EFI_MEMORY_DESCRIPTOR);
        g_handoff.mmap_desc_size = sizeof(EFI_MEMORY_DESCRIPTOR);
    }
    g_handoff.boot_identity_limit = 0x100000000ULL;

    early_serial_puts("MB_HANDOFF\n");
    early_debug_putc('h');
    _fry_start(&g_handoff);
    for (;;) { __asm__ volatile("hlt"); }
}
