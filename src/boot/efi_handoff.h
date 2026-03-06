#ifndef FRY_EFI_HANDOFF_H
#define FRY_EFI_HANDOFF_H

#include <stdint.h>

struct fry_handoff {
    uint64_t fb_base;
    uint64_t fb_width;
    uint64_t fb_height;
    uint64_t fb_stride;
    uint32_t fb_pixel_format; // 0 = BGRX, 1 = RGBX (UEFI GOP enums)
    uint64_t rsdp_phys;
    uint64_t mmap_base;
    uint64_t mmap_size;
    uint64_t mmap_desc_size;
    uint64_t boot_identity_limit;
    // Ramdisk: .fry files read from EFI partition by loader, passed to kernel.
    // ramdisk_base is a physical address (below 4GB); kernel accesses via physmap.
    uint64_t ramdisk_base;
    uint64_t ramdisk_size;
};

// Ramdisk format: ramdisk_header at ramdisk_base, followed by packed file data.
// File data begins at byte offset sizeof(ramdisk_header) from ramdisk_base.
#define RAMDISK_MAGIC    0x4B534452UL  /* 'RDSK' little-endian */
#define RAMDISK_MAXFILES 16

struct ramdisk_entry {
    char     name[32];   /* case-insensitive relative path, e.g. "system/INIT.FRY" */
    uint64_t offset;     /* byte offset of file data from start of ramdisk buffer */
    uint64_t size;       /* file size in bytes */
};

struct ramdisk_header {
    uint32_t             magic;
    uint32_t             count;
    struct ramdisk_entry entries[RAMDISK_MAXFILES];
};

// UEFI memory descriptor layout (matches firmware-provided map entries)
typedef struct {
    uint32_t Type;
    uint64_t PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} EFI_MEMORY_DESCRIPTOR;

#endif
