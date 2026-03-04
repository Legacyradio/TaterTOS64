// TaterTOS64v3 UEFI entry point
// Phase 1 - Foundation

#include <stdint.h>
#include "efi_handoff.h"

// Minimal UEFI type system (no external headers)
typedef void *EFI_HANDLE;
typedef uint64_t EFI_STATUS;
typedef uint64_t EFI_PHYSICAL_ADDRESS;
typedef uint64_t EFI_VIRTUAL_ADDRESS;
typedef uint64_t UINTN;
typedef uint64_t UINT64;
typedef uint32_t UINT32;
typedef uint16_t UINT16;
typedef uint8_t UINT8;

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8  Data4[8];
} EFI_GUID;

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

// Configuration table
typedef struct {
    EFI_GUID VendorGuid;
    void *VendorTable;
} EFI_CONFIGURATION_TABLE;

// Graphics Output Protocol (GOP)
typedef enum {
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBltOnly,
    PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    UINT32 RedMask;
    UINT32 GreenMask;
    UINT32 BlueMask;
    UINT32 ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    UINT32 Version;
    UINT32 HorizontalResolution;
    UINT32 VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    EFI_PIXEL_BITMASK PixelInformation;
    UINT32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    UINT32 MaxMode;
    UINT32 Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct {
    EFI_STATUS (*QueryMode)(void *, UINT32, UINTN *, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **);
    EFI_STATUS (*SetMode)(void *, UINT32);
    EFI_STATUS (*Blt)(void *, void *, UINT32, UINTN, UINTN, UINTN, UINTN, UINTN, UINTN, UINTN);
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

// Memory map descriptor is defined in efi_handoff.h

// Boot Services
typedef struct {
    EFI_TABLE_HEADER Hdr;

    void *RaiseTPL;
    void *RestoreTPL;

    EFI_STATUS (*AllocatePages)(UINT32, UINT32, UINTN, EFI_PHYSICAL_ADDRESS *);
    EFI_STATUS (*FreePages)(EFI_PHYSICAL_ADDRESS, UINTN);
    EFI_STATUS (*GetMemoryMap)(UINTN *, EFI_MEMORY_DESCRIPTOR *, UINTN *, UINTN *, UINT32 *);
    EFI_STATUS (*AllocatePool)(UINT32, UINTN, void **);
    EFI_STATUS (*FreePool)(void *);

    void *CreateEvent;
    void *SetTimer;
    void *WaitForEvent;
    void *SignalEvent;
    void *CloseEvent;
    void *CheckEvent;

    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    EFI_STATUS (*HandleProtocol)(EFI_HANDLE, EFI_GUID *, void **);
    void *Reserved;
    void *RegisterProtocolNotify;
    EFI_STATUS (*LocateHandle)(UINT32, EFI_GUID *, void *, UINTN *, EFI_HANDLE *);
    EFI_STATUS (*LocateDevicePath)(EFI_GUID *, void **, EFI_HANDLE *);
    EFI_STATUS (*InstallConfigurationTable)(EFI_GUID *, void *);

    void *LoadImage;
    void *StartImage;
    void *Exit;
    void *UnloadImage;
    EFI_STATUS (*ExitBootServices)(EFI_HANDLE, UINTN);

    void *GetNextMonotonicCount;
    void *Stall;
    void *SetWatchdogTimer;

    EFI_STATUS (*ConnectController)(EFI_HANDLE, void *, void *, UINT8);
    EFI_STATUS (*DisconnectController)(EFI_HANDLE, void *, void *, UINT8);

    void *OpenProtocol;
    void *CloseProtocol;
    void *OpenProtocolInformation;

    void *ProtocolsPerHandle;
    void *LocateHandleBuffer;
    EFI_STATUS (*LocateProtocol)(EFI_GUID *, void *, void **);

    void *InstallMultipleProtocolInterfaces;
    void *UninstallMultipleProtocolInterfaces;

    void *CalculateCrc32;
    void *CopyMem;
    void *SetMem;

    void *CreateEventEx;
} EFI_BOOT_SERVICES;

// System Table
typedef struct {
    EFI_TABLE_HEADER Hdr;
    UINT16 *FirmwareVendor;
    UINT32 FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    void *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    void *ConOut;
    EFI_HANDLE StandardErrorHandle;
    void *StdErr;
    void *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

// GUIDs
static const EFI_GUID EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID =
    {0x9042a9de, 0x23dc, 0x4a38, {0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a}};

static const EFI_GUID EFI_ACPI_20_TABLE_GUID =
    {0x8868e871, 0xe4f1, 0x11d3, {0xbc,0x22,0x00,0x80,0xc7,0x3c,0x88,0x81}};

static const EFI_GUID EFI_ACPI_TABLE_GUID =
    {0xeb9d2d30, 0x2d88, 0x11d3, {0x9a,0x16,0x00,0x90,0x27,0x3f,0xc1,0x4d}};

// Kernel entry (stack switch for UEFI path)
extern void efi_start(struct fry_handoff *handoff);

static int guid_equal(const EFI_GUID *a, const EFI_GUID *b) {
    if (a->Data1 != b->Data1 || a->Data2 != b->Data2 || a->Data3 != b->Data3) {
        return 0;
    }
    for (int i = 0; i < 8; i++) {
        if (a->Data4[i] != b->Data4[i]) {
            return 0;
        }
    }
    return 1;
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS status;
    EFI_BOOT_SERVICES *BS = SystemTable->BootServices;

    // Locate GOP before ExitBootServices
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
    status = BS->LocateProtocol((EFI_GUID *)&EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID, 0, (void **)&gop);
    if (status != 0 || gop == 0 || gop->Mode == 0 || gop->Mode->Info == 0) {
        // Continue without GOP if unavailable
        gop = 0;
    }

    // Locate RSDP from configuration table (prefer ACPI 2.0)
    void *rsdp = 0;
    for (UINTN i = 0; i < SystemTable->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *ct = &SystemTable->ConfigurationTable[i];
        if (guid_equal(&ct->VendorGuid, &EFI_ACPI_20_TABLE_GUID)) {
            rsdp = ct->VendorTable;
            break;
        }
    }
    if (rsdp == 0) {
        for (UINTN i = 0; i < SystemTable->NumberOfTableEntries; i++) {
            EFI_CONFIGURATION_TABLE *ct = &SystemTable->ConfigurationTable[i];
            if (guid_equal(&ct->VendorGuid, &EFI_ACPI_TABLE_GUID)) {
                rsdp = ct->VendorTable;
                break;
            }
        }
    }

    // Get memory map size
    UINTN mmap_size = 0;
    UINTN mmap_key = 0;
    UINTN desc_size = 0;
    UINT32 desc_version = 0;

    status = BS->GetMemoryMap(&mmap_size, 0, &mmap_key, &desc_size, &desc_version);
    if (status == 0) {
        // Should not happen with NULL buffer; continue anyway
    }

    // Allocate buffer for memory map (+ slop)
    mmap_size += desc_size * 8;
    EFI_MEMORY_DESCRIPTOR *mmap = 0;
    status = BS->AllocatePool(2, mmap_size, (void **)&mmap); // 2 = EfiLoaderData
    if (status != 0 || mmap == 0) {
        // Unable to continue safely
        return status;
    }

    // Get memory map
    status = BS->GetMemoryMap(&mmap_size, mmap, &mmap_key, &desc_size, &desc_version);
    if (status != 0) {
        return status;
    }

    // Exit boot services (retry once if needed)
    status = BS->ExitBootServices(ImageHandle, mmap_key);
    if (status != 0) {
        // Memory map may have changed; refresh once
        mmap_size = 0;
        status = BS->GetMemoryMap(&mmap_size, 0, &mmap_key, &desc_size, &desc_version);
        mmap_size += desc_size * 8;
        status = BS->GetMemoryMap(&mmap_size, mmap, &mmap_key, &desc_size, &desc_version);
        if (status != 0) {
            return status;
        }
        status = BS->ExitBootServices(ImageHandle, mmap_key);
        if (status != 0) {
            return status;
        }
    }

    // Prepare handoff
    struct fry_handoff handoff;
    if (gop) {
        handoff.fb_base = (uint64_t)gop->Mode->FrameBufferBase;
        handoff.fb_width = (uint64_t)gop->Mode->Info->HorizontalResolution;
        handoff.fb_height = (uint64_t)gop->Mode->Info->VerticalResolution;
        handoff.fb_stride = (uint64_t)gop->Mode->Info->PixelsPerScanLine;
        handoff.fb_pixel_format = (uint32_t)gop->Mode->Info->PixelFormat;
    } else {
        handoff.fb_base = 0;
        handoff.fb_width = 0;
        handoff.fb_height = 0;
        handoff.fb_stride = 0;
        handoff.fb_pixel_format = 0;
    }

    handoff.rsdp_phys = (uint64_t)rsdp;
    handoff.mmap_base = (uint64_t)mmap;
    handoff.mmap_size = (uint64_t)mmap_size;
    handoff.mmap_desc_size = (uint64_t)desc_size;
    handoff.boot_identity_limit = 0x100000000ULL;

    // Jump to kernel entry
    efi_start(&handoff);

    // If kernel returns, halt
    for (;;) { }

    return 0;
}
