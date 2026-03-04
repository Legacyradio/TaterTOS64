#include <stdint.h>
#include <stddef.h>
#include "efi_handoff.h"

#define TATER_BUILD_ID "2026-02-26-vfsprobe2"

#define EFIAPI __attribute__((ms_abi))

typedef uint64_t EFI_STATUS;
typedef void *EFI_HANDLE;
typedef uint64_t EFI_PHYSICAL_ADDRESS;
typedef uint64_t EFI_VIRTUAL_ADDRESS;
typedef uint64_t UINT64;
typedef uint32_t UINT32;
typedef uint16_t UINT16;
typedef uint8_t UINT8;
typedef uint64_t UINTN;
typedef uint16_t CHAR16;
typedef uint8_t BOOLEAN;

#define EFI_SUCCESS 0
#define EFI_ERROR(x) (((x) & 0x8000000000000000ULL) != 0)
#define EFI_BUFFER_TOO_SMALL 0x8000000000000005ULL
#define EFI_NOT_FOUND 0x8000000000000014ULL

#define AllocateAnyPages 0
#define AllocateMaxAddress 1
#define AllocateAddress 2

#define EfiLoaderData 4

#define EFI_OPEN_MODE_READ 0x0000000000000001ULL

#define EFI_FILE_MODE_READ EFI_OPEN_MODE_READ
#define EFI_FILE_READ_ONLY 0x0000000000000001ULL

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8 Data4[8];
} EFI_GUID;

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void *Reset;
    EFI_STATUS (EFIAPI *OutputString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);
    void *TestString;
    void *QueryMode;
    void *SetMode;
    void *SetAttribute;
    void *ClearScreen;
    void *SetCursorPosition;
    void *EnableCursor;
    void *Mode;
};

typedef struct {
    EFI_GUID VendorGuid;
    void *VendorTable;
} EFI_CONFIGURATION_TABLE;

typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;

struct EFI_FILE_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *Open)(EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **NewHandle,
                              CHAR16 *FileName, UINT64 OpenMode, UINT64 Attributes);
    EFI_STATUS (EFIAPI *Close)(EFI_FILE_PROTOCOL *This);
    EFI_STATUS (EFIAPI *Delete)(EFI_FILE_PROTOCOL *This);
    EFI_STATUS (EFIAPI *Read)(EFI_FILE_PROTOCOL *This, UINTN *BufferSize, void *Buffer);
    EFI_STATUS (EFIAPI *Write)(EFI_FILE_PROTOCOL *This, UINTN *BufferSize, void *Buffer);
    EFI_STATUS (EFIAPI *GetPosition)(EFI_FILE_PROTOCOL *This, UINT64 *Position);
    EFI_STATUS (EFIAPI *SetPosition)(EFI_FILE_PROTOCOL *This, UINT64 Position);
    EFI_STATUS (EFIAPI *GetInfo)(EFI_FILE_PROTOCOL *This, EFI_GUID *InformationType,
                                 UINTN *BufferSize, void *Buffer);
    EFI_STATUS (EFIAPI *SetInfo)(EFI_FILE_PROTOCOL *This, EFI_GUID *InformationType,
                                 UINTN BufferSize, void *Buffer);
    EFI_STATUS (EFIAPI *Flush)(EFI_FILE_PROTOCOL *This);
    EFI_STATUS (EFIAPI *OpenEx)(EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **NewHandle,
                                CHAR16 *FileName, UINT64 OpenMode, UINT64 Attributes, void *Token);
    EFI_STATUS (EFIAPI *ReadEx)(EFI_FILE_PROTOCOL *This, void *Token);
    EFI_STATUS (EFIAPI *WriteEx)(EFI_FILE_PROTOCOL *This, void *Token);
    EFI_STATUS (EFIAPI *FlushEx)(EFI_FILE_PROTOCOL *This, void *Token);
};

typedef struct {
    UINT64 Size;
    UINT64 FileSize;
    UINT64 PhysicalSize;
    UINT64 CreateTime[2];
    UINT64 LastAccessTime[2];
    UINT64 ModificationTime[2];
    UINT64 Attribute;
    CHAR16 FileName[1];
} EFI_FILE_INFO;

typedef struct {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(void *This, EFI_FILE_PROTOCOL **Root);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef struct {
    UINT32 Version;
    UINT32 HorizontalResolution;
    UINT32 VerticalResolution;
    UINT32 PixelFormat;
    struct {
        UINT32 RedMask;
        UINT32 GreenMask;
        UINT32 BlueMask;
        UINT32 ReservedMask;
    } PixelInformation;
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
    EFI_STATUS (EFIAPI *QueryMode)(void *This, UINT32 ModeNumber, UINTN *SizeOfInfo,
                                   EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);
    EFI_STATUS (EFIAPI *SetMode)(void *This, UINT32 ModeNumber);
    void *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

typedef struct {
    UINT32 Revision;
    EFI_HANDLE ParentHandle;
    void *SystemTable;
    EFI_HANDLE DeviceHandle;
    void *FilePath;
    void *Reserved;
    UINT32 LoadOptionsSize;
    void *LoadOptions;
    void *ImageBase;
    UINT64 ImageSize;
    UINT32 ImageCodeType;
    UINT32 ImageDataType;
    EFI_STATUS (EFIAPI *Unload)(EFI_HANDLE ImageHandle);
} EFI_LOADED_IMAGE_PROTOCOL;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *RaiseTPL;
    void *RestoreTPL;
    EFI_STATUS (EFIAPI *AllocatePages)(UINTN Type, UINTN MemoryType, UINTN Pages, EFI_PHYSICAL_ADDRESS *Memory);
    EFI_STATUS (EFIAPI *FreePages)(EFI_PHYSICAL_ADDRESS Memory, UINTN Pages);
    EFI_STATUS (EFIAPI *GetMemoryMap)(UINTN *MemoryMapSize, EFI_MEMORY_DESCRIPTOR *MemoryMap,
                                      UINTN *MapKey, UINTN *DescriptorSize, UINT32 *DescriptorVersion);
    EFI_STATUS (EFIAPI *AllocatePool)(UINTN PoolType, UINTN Size, void **Buffer);
    EFI_STATUS (EFIAPI *FreePool)(void *Buffer);
    void *CreateEvent;
    void *SetTimer;
    void *WaitForEvent;
    void *SignalEvent;
    void *CloseEvent;
    void *CheckEvent;
    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol, void **Interface);
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstallConfigurationTable;
    void *LoadImage;
    void *StartImage;
    void *Exit;
    void *UnloadImage;
    EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE ImageHandle, UINTN MapKey);
    void *GetNextMonotonicCount;
    void *Stall;
    void *SetWatchdogTimer;
    void *ConnectController;
    void *DisconnectController;
    void *OpenProtocol;
    void *CloseProtocol;
    void *OpenProtocolInformation;
    void *ProtocolsPerHandle;
    void *LocateHandleBuffer;
    EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *Protocol, void *Registration, void **Interface);
    void *InstallMultipleProtocolInterfaces;
    void *UninstallMultipleProtocolInterfaces;
    void *CalculateCrc32;
    void *CopyMem;
    void *SetMem;
    void *CreateEventEx;
} EFI_BOOT_SERVICES;


typedef struct {
    EFI_TABLE_HEADER Hdr;
    EFI_STATUS (EFIAPI *GetTime)(void *Time, void *Capabilities);
    EFI_STATUS (EFIAPI *SetTime)(void *Time);
    EFI_STATUS (EFIAPI *GetWakeupTime)(BOOLEAN *Enabled, BOOLEAN *Pending, void *Time);
    EFI_STATUS (EFIAPI *SetWakeupTime)(BOOLEAN Enable, void *Time);
    EFI_STATUS (EFIAPI *SetVirtualAddressMap)(UINTN MemoryMapSize, UINTN DescriptorSize,
                                              UINT32 DescriptorVersion, EFI_MEMORY_DESCRIPTOR *VirtualMap);
    EFI_STATUS (EFIAPI *ConvertPointer)(UINTN DebugDisposition, void **Address);
    EFI_STATUS (EFIAPI *GetVariable)(CHAR16 *VariableName, EFI_GUID *VendorGuid,
                                     UINT32 *Attributes, UINTN *DataSize, void *Data);
    EFI_STATUS (EFIAPI *GetNextVariableName)(UINTN *VariableNameSize, CHAR16 *VariableName,
                                             EFI_GUID *VendorGuid);
    EFI_STATUS (EFIAPI *SetVariable)(CHAR16 *VariableName, EFI_GUID *VendorGuid,
                                     UINT32 Attributes, UINTN DataSize, void *Data);
    EFI_STATUS (EFIAPI *GetNextHighMonotonicCount)(UINT32 *HighCount);
    void *ResetSystem;
    void *UpdateCapsule;
    void *QueryCapsuleCapabilities;
    EFI_STATUS (EFIAPI *QueryVariableInfo)(UINT32 Attributes, UINT64 *MaximumVariableStorageSize,
                                           UINT64 *RemainingVariableStorageSize, UINT64 *MaximumVariableSize);
} EFI_RUNTIME_SERVICES;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    UINT32 FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    void *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle;
    void *StdErr;
    EFI_RUNTIME_SERVICES *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

static void *alloc_pages_below(EFI_BOOT_SERVICES *bs, EFI_PHYSICAL_ADDRESS max_addr, UINTN pages) {
    EFI_PHYSICAL_ADDRESS addr = max_addr;
    EFI_STATUS st = bs->AllocatePages(AllocateMaxAddress, EfiLoaderData, pages, &addr);
    if (EFI_ERROR(st)) return 0;
    return (void *)(uintptr_t)addr;
}

typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} Elf64_Shdr;

typedef struct {
    uint32_t st_name;
    unsigned char st_info;
    unsigned char st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} Elf64_Sym;

static const EFI_GUID gLoadedImageProtocolGuid =
    {0x5B1B31A1,0x9562,0x11d2,{0x8E,0x3F,0x00,0xA0,0xC9,0x69,0x72,0x3B}};
static const EFI_GUID gSimpleFileSystemProtocolGuid =
    {0x964e5b22,0x6459,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
static const EFI_GUID gFileInfoGuid =
    {0x09576e92,0x6d3f,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
static const EFI_GUID gGopGuid =
    {0x9042a9de,0x23dc,0x4a38,{0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a}};
static const EFI_GUID gAcpi20TableGuid =
    {0x8868e871,0xe4f1,0x11d3,{0xbc,0x22,0x00,0x80,0xc7,0x3c,0x88,0x81}};
static const EFI_GUID gAcpi10TableGuid =
    {0xeb9d2d30,0x2d88,0x11d3,{0x9a,0x16,0x00,0x90,0x27,0x3f,0xc1,0x4d}};

/* EFI_EDID_ACTIVE_PROTOCOL  — firmware-initialised EDID data (preferred) */
static const EFI_GUID gEdidActiveGuid =
    {0xbd8c1056,0x9f36,0x44ec,{0x92,0xa8,0xa6,0x33,0x7f,0x81,0x79,0x86}};
/* EFI_EDID_DISCOVERED_PROTOCOL — raw DDC data before any override */
static const EFI_GUID gEdidDiscoveredGuid =
    {0x1c0c34f6,0xd380,0x41fa,{0xa0,0x49,0x8a,0xd0,0x6c,0x1a,0x66,0xaa}};

typedef struct {
    UINT32 SizeOfEdid;
    UINT8  *Edid;
} EFI_EDID_PROTOCOL;

/*
 * Parse the preferred (native) resolution from an EDID 1.x base block.
 * The first Detailed Timing Descriptor lives at bytes 54-71.  A non-zero
 * pixel clock at bytes 54-55 confirms it is a timing descriptor (not a
 * monitor descriptor).  Horizontal and vertical active pixel counts are
 * encoded across two bytes each per the VESA EDID 1.4 specification.
 */
static void edid_preferred_res(UINT8 *edid, UINT32 sz, UINT32 *w, UINT32 *h) {
    if (!edid || sz < 72) return;
    if (edid[54] == 0 && edid[55] == 0) return; /* not a timing descriptor */
    UINT32 ha = ((UINT32)(edid[58] >> 4) << 8) | (UINT32)edid[56];
    UINT32 va = ((UINT32)(edid[61] >> 4) << 8) | (UINT32)edid[59];
    if (ha && va) { *w = ha; *h = va; }
}

/* Extract the LSB position and bit-width of a GOP pixel-format bitmask. */
static UINT8 gop_mask_pos(UINT32 mask) {
    if (!mask) return 0;
    UINT8 p = 0;
    while (!(mask & 1u)) { mask >>= 1; p++; }
    return p;
}
static UINT8 gop_mask_size(UINT32 mask) {
    UINT8 s = 0;
    while (mask & 1u) { mask >>= 1; s++; }
    return s;
}

/*
 * Enumerate all GOP modes and return the one that best matches the monitor's
 * native resolution.  Resolution is sourced from EDID when available (via the
 * two standard UEFI EDID protocols), falling back to the largest-area linear-
 * framebuffer mode when EDID is absent or unreadable.
 *
 * Modes with PixelFormat == 3 (PixelBltOnly) have no memory-mapped
 * framebuffer and are always skipped.
 */
static UINT32 gop_pick_native_mode(EFI_BOOT_SERVICES *bs,
                                   EFI_GRAPHICS_OUTPUT_PROTOCOL *gop) {
    UINT32 edid_w = 0, edid_h = 0;

    EFI_EDID_PROTOCOL *ep = 0;
    if (!EFI_ERROR(bs->LocateProtocol((EFI_GUID*)&gEdidActiveGuid, 0,
                                       (void**)&ep)) &&
        ep && ep->SizeOfEdid >= 72 && ep->Edid)
        edid_preferred_res(ep->Edid, ep->SizeOfEdid, &edid_w, &edid_h);

    if (!edid_w) {
        ep = 0;
        if (!EFI_ERROR(bs->LocateProtocol((EFI_GUID*)&gEdidDiscoveredGuid, 0,
                                           (void**)&ep)) &&
            ep && ep->SizeOfEdid >= 72 && ep->Edid)
            edid_preferred_res(ep->Edid, ep->SizeOfEdid, &edid_w, &edid_h);
    }

    UINT32 best = gop->Mode->Mode;
    UINT32 best_area = 0;

    for (UINT32 m = 0; m < gop->Mode->MaxMode; m++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = 0;
        UINTN info_sz = 0;
        if (EFI_ERROR(gop->QueryMode(gop, m, &info_sz, &info))) continue;
        if (info->PixelFormat == 3) continue; /* PixelBltOnly — no framebuffer */
        UINT32 w = info->HorizontalResolution;
        UINT32 h = info->VerticalResolution;
        if (!w || !h) continue;
        /* Exact EDID match — take it immediately */
        if (edid_w && edid_h && w == edid_w && h == edid_h) { best = m; break; }
        /* No EDID: prefer the largest available framebuffer mode */
        if (w * h > best_area) { best_area = w * h; best = m; }
    }
    return best;
}

static int memeq(const void *a, const void *b, size_t n) {
    const uint8_t *pa = (const uint8_t*)a;
    const uint8_t *pb = (const uint8_t*)b;
    for (size_t i = 0; i < n; i++) if (pa[i] != pb[i]) return 0;
    return 1;
}

static void memclr(void *p, size_t n) {
    uint8_t *b = (uint8_t*)p;
    for (size_t i = 0; i < n; i++) b[i] = 0;
}

static void memcopy(void *d, const void *s, size_t n) {
    uint8_t *dd = (uint8_t*)d;
    const uint8_t *ss = (const uint8_t*)s;
    for (size_t i = 0; i < n; i++) dd[i] = ss[i];
}

static int streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static void efi_puts(EFI_SYSTEM_TABLE *st, CHAR16 *s) {
    if (!st || !st->ConOut || !st->ConOut->OutputString || !s) return;
    st->ConOut->OutputString(st->ConOut, s);
}

static void efi_die(EFI_SYSTEM_TABLE *st, CHAR16 *msg) {
    efi_puts(st, msg);
    for (;;) { __asm__ volatile ("hlt"); }
}

static void efi_put_hex64(EFI_SYSTEM_TABLE *st, uint64_t v) {
    CHAR16 buf[19];
    static const CHAR16 hex[] = L"0123456789ABCDEF";
    buf[0] = L'0';
    buf[1] = L'x';
    for (int i = 0; i < 16; i++) {
        int shift = 60 - i * 4;
        buf[2 + i] = hex[(v >> shift) & 0xFULL];
    }
    buf[18] = 0;
    efi_puts(st, buf);
}

// Simple COM1 serial output usable from EFI context (before ExitBootServices).
// Uses direct x86 port I/O — no dependencies on any library.
static void efi_serial_init(void) {
    // 115200 baud, 8N1 on COM1 (0x3F8).
    __asm__ volatile("outb %0,%1"::"a"((uint8_t)0x00),"Nd"((uint16_t)0x3F9)); // IER off
    __asm__ volatile("outb %0,%1"::"a"((uint8_t)0x80),"Nd"((uint16_t)0x3FB)); // DLAB=1
    __asm__ volatile("outb %0,%1"::"a"((uint8_t)0x01),"Nd"((uint16_t)0x3F8)); // divisor lo
    __asm__ volatile("outb %0,%1"::"a"((uint8_t)0x00),"Nd"((uint16_t)0x3F9)); // divisor hi
    __asm__ volatile("outb %0,%1"::"a"((uint8_t)0x03),"Nd"((uint16_t)0x3FB)); // 8N1, DLAB=0
    __asm__ volatile("outb %0,%1"::"a"((uint8_t)0xC7),"Nd"((uint16_t)0x3FA)); // FIFO
    __asm__ volatile("outb %0,%1"::"a"((uint8_t)0x0B),"Nd"((uint16_t)0x3FC)); // RTS+DTR
}
static void efi_serial_putc(char c) {
    for (int i = 0; i < 200000; i++) {
        uint8_t lsr;
        __asm__ volatile("inb %1,%0":"=a"(lsr):"Nd"((uint16_t)0x3FD));
        if (lsr & 0x20) break;
    }
    __asm__ volatile("outb %0,%1"::"a"((uint8_t)c),"Nd"((uint16_t)0x3F8));
}
static void efi_serial_puts(const char *s) {
    for (; *s; s++) {
        if (*s == '\n') efi_serial_putc('\r');
        efi_serial_putc(*s);
    }
}

static EFI_SYSTEM_TABLE *g_st = 0;

static uint64_t g_fail_st = 0;
static uint64_t g_fail_pa = 0;
static uint64_t g_fail_pages = 0;
static uint64_t g_fail_seg = 0;

static void fb_mark(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, uint32_t color) {
    if (!gop || !gop->Mode || !gop->Mode->Info) return;
    uint32_t w = gop->Mode->Info->HorizontalResolution;
    uint32_t h = gop->Mode->Info->VerticalResolution;
    if (w == 0 || h == 0) return;
    uint32_t ppsl = gop->Mode->Info->PixelsPerScanLine;
    uint32_t *fb = (uint32_t*)(uintptr_t)gop->Mode->FrameBufferBase;
    uint32_t mw = (w < 160) ? w : 160;
    uint32_t mh = (h < 40) ? h : 40;
    for (uint32_t y = 0; y < mh; y++) {
        for (uint32_t x = 0; x < mw; x++) fb[y * ppsl + x] = color;
    }
}

/* Early kernel page tables identity-map low 4GiB during EFI handoff. */
#define TATER_BOOT_IDENTITY_LIMIT 0x100000000ULL
#define TATER_BOOT_IDENTITY_MAX_ADDR (TATER_BOOT_IDENTITY_LIMIT - 1ULL)

static EFI_STATUS read_file(EFI_BOOT_SERVICES *bs, EFI_FILE_PROTOCOL *root, CHAR16 *path,
                            EFI_PHYSICAL_ADDRESS max_addr,
                            void **out_buf, UINTN *out_size) {
    EFI_FILE_PROTOCOL *f = 0;
    EFI_STATUS st = root->Open(root, &f, path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st)) return st;

    UINTN info_sz = 0;
    st = f->GetInfo(f, (EFI_GUID*)&gFileInfoGuid, &info_sz, 0);
    if (st != EFI_BUFFER_TOO_SMALL) { f->Close(f); return st; }

    EFI_FILE_INFO *fi = 0;
    st = bs->AllocatePool(EfiLoaderData, info_sz, (void**)&fi);
    if (EFI_ERROR(st)) { f->Close(f); return st; }
    st = f->GetInfo(f, (EFI_GUID*)&gFileInfoGuid, &info_sz, fi);
    if (EFI_ERROR(st)) { bs->FreePool(fi); f->Close(f); return st; }

    UINTN sz = (UINTN)fi->FileSize;
    bs->FreePool(fi);

    UINTN pages = (sz + 4095) / 4096;
    EFI_PHYSICAL_ADDRESS addr = max_addr ? max_addr : (EFI_PHYSICAL_ADDRESS)TATER_BOOT_IDENTITY_MAX_ADDR;
    st = bs->AllocatePages(AllocateMaxAddress, EfiLoaderData, pages, &addr);
    if (EFI_ERROR(st)) { f->Close(f); return st; }

    UINTN rd = sz;
    st = f->Read(f, &rd, (void*)(uintptr_t)addr);
    f->Close(f);
    if (EFI_ERROR(st) || rd != sz) return EFI_NOT_FOUND;

    *out_buf = (void*)(uintptr_t)addr;
    *out_size = sz;
    return EFI_SUCCESS;
}

static uint64_t find_symbol_va(void *elf, const char *name) {
    Elf64_Ehdr *eh = (Elf64_Ehdr*)elf;
    if (!(eh->e_ident[0] == 0x7F && eh->e_ident[1] == 'E' && eh->e_ident[2] == 'L' && eh->e_ident[3] == 'F')) return 0;
    Elf64_Shdr *sh = (Elf64_Shdr*)((uint8_t*)elf + eh->e_shoff);
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != 2) continue; /* SHT_SYMTAB */
        Elf64_Sym *syms = (Elf64_Sym*)((uint8_t*)elf + sh[i].sh_offset);
        uint64_t nsyms = sh[i].sh_size / sh[i].sh_entsize;
        Elf64_Shdr *strsec = &sh[sh[i].sh_link];
        const char *strs = (const char*)elf + strsec->sh_offset;
        for (uint64_t s = 0; s < nsyms; s++) {
            const char *nm = strs + syms[s].st_name;
            if (streq(nm, name)) return syms[s].st_value;
        }
    }
    return 0;
}

static uint64_t va_to_pa(void *elf, uint64_t va) {
    Elf64_Ehdr *eh = (Elf64_Ehdr*)elf;
    Elf64_Phdr *ph = (Elf64_Phdr*)((uint8_t*)elf + eh->e_phoff);
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != 1) continue; /* PT_LOAD */
        uint64_t lo = ph[i].p_vaddr;
        uint64_t hi = ph[i].p_vaddr + ph[i].p_memsz;
        if (va >= lo && va < hi) return ph[i].p_paddr + (va - lo);
    }
    return 0;
}

static void dump_mmap_overlap(EFI_BOOT_SERVICES *bs, uint64_t want_lo, uint64_t want_hi) {
    if (!g_st || !bs) return;

    UINTN map_sz = 0, map_key = 0, desc_sz = 0;
    UINT32 desc_ver = 0;
    EFI_STATUS st = bs->GetMemoryMap(&map_sz, 0, &map_key, &desc_sz, &desc_ver);
    if (st != EFI_BUFFER_TOO_SMALL || desc_sz == 0) return;
    map_sz += desc_sz * 8; /* slack for firmware allocations during dump */

    void *map_buf = 0;
    st = bs->AllocatePool(EfiLoaderData, map_sz, &map_buf);
    if (EFI_ERROR(st) || !map_buf) return;

    st = bs->GetMemoryMap(&map_sz, (EFI_MEMORY_DESCRIPTOR*)map_buf, &map_key, &desc_sz, &desc_ver);
    if (EFI_ERROR(st)) { bs->FreePool(map_buf); return; }

    efi_puts(g_st, L"TATER EFI: mmap overlap dump\r\n");
    efi_puts(g_st, L"  want_lo="); efi_put_hex64(g_st, want_lo);
    efi_puts(g_st, L" want_hi="); efi_put_hex64(g_st, want_hi);
    efi_puts(g_st, L" desc_sz="); efi_put_hex64(g_st, (uint64_t)desc_sz);
    efi_puts(g_st, L"\r\n");

    /* Print only descriptors overlapping the wanted range (plus a small margin). */
    const uint64_t margin = 16ULL * 1024ULL * 1024ULL; /* 16 MiB */
    uint64_t dump_lo = (want_lo > margin) ? (want_lo - margin) : 0;
    uint64_t dump_hi = want_hi + margin;

    UINTN n = map_sz / desc_sz;
    for (UINTN i = 0; i < n; i++) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR*)((uint8_t*)map_buf + i * desc_sz);
        uint64_t d_lo = (uint64_t)d->PhysicalStart;
        uint64_t d_hi = d_lo + (uint64_t)d->NumberOfPages * 4096ULL;
        if (d_hi <= dump_lo || d_lo >= dump_hi) continue;
        efi_puts(g_st, L"  mm: t="); efi_put_hex64(g_st, (uint64_t)d->Type);
        efi_puts(g_st, L" ps="); efi_put_hex64(g_st, d_lo);
        efi_puts(g_st, L" np="); efi_put_hex64(g_st, (uint64_t)d->NumberOfPages);
        efi_puts(g_st, L" attr="); efi_put_hex64(g_st, (uint64_t)d->Attribute);
        efi_puts(g_st, L"\r\n");
    }

    bs->FreePool(map_buf);
}

static EFI_STATUS load_kernel_elf(EFI_BOOT_SERVICES *bs, void *elf) {
    Elf64_Ehdr *eh = (Elf64_Ehdr*)elf;
    if (!(eh->e_ident[0] == 0x7F && eh->e_ident[1] == 'E' && eh->e_ident[2] == 'L' && eh->e_ident[3] == 'F')) return EFI_NOT_FOUND;
    if (eh->e_phentsize != sizeof(Elf64_Phdr)) return EFI_NOT_FOUND;
    Elf64_Phdr *ph = (Elf64_Phdr*)((uint8_t*)elf + eh->e_phoff);

    uint64_t min_pa = ~0ULL;
    uint64_t max_pa = 0;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != 1) continue;
        if (ph[i].p_filesz > ph[i].p_memsz) return EFI_NOT_FOUND;
        uint64_t lo = ph[i].p_paddr & ~0xFFFULL;
        uint64_t hi = (ph[i].p_paddr + ph[i].p_memsz + 0xFFFULL) & ~0xFFFULL;
        if (lo < min_pa) min_pa = lo;
        if (hi > max_pa) max_pa = hi;
    }
    if (min_pa == ~0ULL || max_pa <= min_pa) return EFI_NOT_FOUND;

    EFI_PHYSICAL_ADDRESS img_base = min_pa;
    UINTN img_pages = (UINTN)((max_pa - min_pa) / 4096ULL);
    EFI_STATUS st = bs->AllocatePages(AllocateAddress, EfiLoaderData, img_pages, &img_base);
    if (EFI_ERROR(st)) {
        g_fail_st = st;
        g_fail_pa = min_pa;
        g_fail_pages = img_pages;
        g_fail_seg = 0xFFFFFFFFFFFFFFFFULL;
        dump_mmap_overlap(bs, min_pa, max_pa);
        return st;
    }
    memclr((void*)(uintptr_t)min_pa, (size_t)(max_pa - min_pa));

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != 1) continue;
        memcopy((void*)(uintptr_t)ph[i].p_paddr, (uint8_t*)elf + ph[i].p_offset, (size_t)ph[i].p_filesz);
    }
    return EFI_SUCCESS;
}

static uint64_t get_rsdp(EFI_SYSTEM_TABLE *st) {
    for (UINTN i = 0; i < st->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *ct = &st->ConfigurationTable[i];
        if (memeq(&ct->VendorGuid, &gAcpi20TableGuid, sizeof(EFI_GUID))) return (uint64_t)(uintptr_t)ct->VendorTable;
        if (memeq(&ct->VendorGuid, &gAcpi10TableGuid, sizeof(EFI_GUID))) return (uint64_t)(uintptr_t)ct->VendorTable;
    }
    return 0;
}

typedef void (*kernel_entry_t)(struct fry_handoff *handoff);

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    g_st = SystemTable;
    efi_serial_init();
    efi_serial_puts("EFI: start\n");
    efi_serial_puts("EFI: build " TATER_BUILD_ID "\n");
    efi_puts(SystemTable, L"\r\nTATER EFI: start\r\n");
    efi_puts(SystemTable, L"TATER EFI: build 2026-02-26-vfsprobe2\r\n");
    EFI_BOOT_SERVICES *bs = SystemTable->BootServices;
    if (!bs) efi_die(SystemTable, L"TATER EFI E00 no BS\r\n");
    EFI_STATUS st = EFI_SUCCESS;

    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
    st = bs->LocateProtocol((EFI_GUID*)&gGopGuid, 0, (void**)&gop);
    if (!EFI_ERROR(st) && gop && gop->Mode && gop->Mode->Info) {
        fb_mark(gop, 0x00202020u); // stage 0: loader entered
    } else {
        gop = 0;
    }

    EFI_LOADED_IMAGE_PROTOCOL *li = 0;
    st = bs->HandleProtocol(ImageHandle, (EFI_GUID*)&gLoadedImageProtocolGuid, (void**)&li);
    if (EFI_ERROR(st) || !li) efi_die(SystemTable, L"TATER EFI E01 HandleProtocol LI\r\n");
    efi_puts(SystemTable, L"TATER EFI: LI ok\r\n");
    if (gop) fb_mark(gop, 0x00004080u); // stage 1

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = 0;
    st = bs->HandleProtocol(li->DeviceHandle, (EFI_GUID*)&gSimpleFileSystemProtocolGuid, (void**)&sfs);
    if (EFI_ERROR(st) || !sfs) efi_die(SystemTable, L"TATER EFI E02 HandleProtocol SFS\r\n");
    efi_puts(SystemTable, L"TATER EFI: SFS ok\r\n");
    if (gop) fb_mark(gop, 0x00006060u); // stage 2

    EFI_FILE_PROTOCOL *root = 0;
    st = sfs->OpenVolume(sfs, &root);
    if (EFI_ERROR(st) || !root) efi_die(SystemTable, L"TATER EFI E03 OpenVolume\r\n");
    efi_puts(SystemTable, L"TATER EFI: volume ok\r\n");
    if (gop) fb_mark(gop, 0x00008040u); // stage 3

    static CHAR16 kpath1[] = L"\\boot\\kernel.elf";
    static CHAR16 kpath2[] = L"\\boot\\KERNEL.ELF";

    void *kbuf = 0;
    UINTN ksz = 0;
    st = read_file(bs, root, kpath1, (EFI_PHYSICAL_ADDRESS)TATER_BOOT_IDENTITY_MAX_ADDR, &kbuf, &ksz);
    if (EFI_ERROR(st)) st = read_file(bs, root, kpath2, (EFI_PHYSICAL_ADDRESS)TATER_BOOT_IDENTITY_MAX_ADDR, &kbuf, &ksz);
    if (EFI_ERROR(st)) efi_die(SystemTable, L"TATER EFI E04 read kernel\r\n");
    efi_puts(SystemTable, L"TATER EFI: kernel ok\r\n");
    efi_puts(SystemTable, L"TATER EFI: kbuf="); efi_put_hex64(SystemTable, (uint64_t)(uintptr_t)kbuf);
    efi_puts(SystemTable, L" ksz="); efi_put_hex64(SystemTable, (uint64_t)ksz);
    efi_puts(SystemTable, L"\r\n");
    if (gop) fb_mark(gop, 0x0000A020u); // stage 4

    if (gop) fb_mark(gop, 0x0000C000u); // stage 5

    st = load_kernel_elf(bs, kbuf);
    if (EFI_ERROR(st)) {
        efi_puts(SystemTable, L"TATER EFI E06 load elf\r\n");
        efi_puts(SystemTable, L"  st=");
        efi_put_hex64(SystemTable, g_fail_st ? g_fail_st : st);
        efi_puts(SystemTable, L" seg=");
        efi_put_hex64(SystemTable, g_fail_seg);
        efi_puts(SystemTable, L" pa=");
        efi_put_hex64(SystemTable, g_fail_pa);
        efi_puts(SystemTable, L" pages=");
        efi_put_hex64(SystemTable, g_fail_pages);
        efi_puts(SystemTable, L"\r\n");
        efi_die(SystemTable, L"TATER EFI halt\r\n");
    }
    efi_puts(SystemTable, L"TATER EFI: elf loaded\r\n");
    if (gop) fb_mark(gop, 0x0020A000u); // stage 6

    uint64_t sym_va = find_symbol_va(kbuf, "efi_boot_start");
    if (!sym_va) efi_die(SystemTable, L"TATER EFI E07 no efi_boot_start\r\n");
    uint64_t entry_pa = va_to_pa(kbuf, sym_va);
    if (!entry_pa) efi_die(SystemTable, L"TATER EFI E08 va->pa\r\n");
    efi_puts(SystemTable, L"TATER EFI: entry resolved\r\n");
    if (!gop) {
        st = bs->LocateProtocol((EFI_GUID*)&gGopGuid, 0, (void**)&gop);
        if (EFI_ERROR(st) || !gop || !gop->Mode || !gop->Mode->Info) efi_die(SystemTable, L"TATER EFI E09 GOP\r\n");
    }
    efi_puts(SystemTable, L"TATER EFI: GOP ok\r\n");
    if (gop) fb_mark(gop, 0x00408000u); // stage 7

    /* UEFI firmware already initialises GOP to the native panel resolution
     * during POST.  We read the current mode directly — no SetMode() call.
     * SetMode() blanks the framebuffer and can crash on certain firmware
     * implementations, giving a blank screen with no error output.
     * gop_pick_native_mode() is kept for future use once we add safe
     * fallback handling, but we do not act on its result yet. */
    (void)gop_pick_native_mode(bs, gop); /* future: mode verification */
    if (gop) fb_mark(gop, 0x00508000u); // stage 7b

    struct fry_handoff *handoff = (struct fry_handoff *)alloc_pages_below(bs, (EFI_PHYSICAL_ADDRESS)TATER_BOOT_IDENTITY_MAX_ADDR, 1);
    if (!handoff) efi_die(SystemTable, L"TATER EFI E10 handoff page\r\n");
    memclr(handoff, sizeof(*handoff));
    handoff->fb_base = (uint64_t)gop->Mode->FrameBufferBase;
    handoff->fb_width = (uint64_t)gop->Mode->Info->HorizontalResolution;
    handoff->fb_height = (uint64_t)gop->Mode->Info->VerticalResolution;
    handoff->fb_stride = (uint64_t)gop->Mode->Info->PixelsPerScanLine;
    handoff->fb_pixel_format = (uint32_t)gop->Mode->Info->PixelFormat;
    handoff->rsdp_phys = get_rsdp(SystemTable);
    handoff->ramdisk_base = 0;
    handoff->ramdisk_size = 0;

    // Load userspace payload binaries into a ramdisk for ISO/USB boot without NVMe.
    // Split policy: SHELL.TOT for shell, .FRY for regular apps.
    // Strategy:
    //   1. Try common locations on the primary filesystem (li->DeviceHandle):
    //      \fry\, \FRY\, root (\), plus common EFI subdirectories.
    //   2. If nothing is found there, scan all EFI_SIMPLE_FILE_SYSTEM_PROTOCOL
    //      handles (firmware sometimes maps the EFI partition differently).
    {
        enum { FRY_FILE_COUNT = 6, FRY_PATH_VARIANTS = 8 };
        static CHAR16 fry_paths[FRY_FILE_COUNT][FRY_PATH_VARIANTS][32] = {
            {
                {'\\','f','r','y','\\','G','U','I','.','F','R','Y',0},
                {'\\','F','R','Y','\\','G','U','I','.','F','R','Y',0},
                {'\\','G','U','I','.','F','R','Y',0},
                {'\\','E','F','I','\\','f','r','y','\\','G','U','I','.','F','R','Y',0},
                {'\\','E','F','I','\\','F','R','Y','\\','G','U','I','.','F','R','Y',0},
                {'\\','E','F','I','\\','B','O','O','T','\\','G','U','I','.','F','R','Y',0},
                {'\\','E','F','I','\\','B','O','O','T','\\','f','r','y','\\','G','U','I','.','F','R','Y',0},
                {'\\','E','F','I','\\','B','O','O','T','\\','F','R','Y','\\','G','U','I','.','F','R','Y',0},
            },
            {
                {'\\','f','r','y','\\','S','H','E','L','L','.','T','O','T',0},
                {'\\','F','R','Y','\\','S','H','E','L','L','.','T','O','T',0},
                {'\\','S','H','E','L','L','.','T','O','T',0},
                {'\\','E','F','I','\\','f','r','y','\\','S','H','E','L','L','.','T','O','T',0},
                {'\\','E','F','I','\\','F','R','Y','\\','S','H','E','L','L','.','T','O','T',0},
                {'\\','E','F','I','\\','B','O','O','T','\\','S','H','E','L','L','.','T','O','T',0},
                {'\\','E','F','I','\\','B','O','O','T','\\','f','r','y','\\','S','H','E','L','L','.','T','O','T',0},
                {'\\','E','F','I','\\','B','O','O','T','\\','F','R','Y','\\','S','H','E','L','L','.','T','O','T',0},
            },
            {
                {'\\','f','r','y','\\','S','Y','S','I','N','F','O','.','F','R','Y',0},
                {'\\','F','R','Y','\\','S','Y','S','I','N','F','O','.','F','R','Y',0},
                {'\\','S','Y','S','I','N','F','O','.','F','R','Y',0},
                {'\\','E','F','I','\\','f','r','y','\\','S','Y','S','I','N','F','O','.','F','R','Y',0},
                {'\\','E','F','I','\\','F','R','Y','\\','S','Y','S','I','N','F','O','.','F','R','Y',0},
                {'\\','E','F','I','\\','B','O','O','T','\\','S','Y','S','I','N','F','O','.','F','R','Y',0},
                {'\\','E','F','I','\\','B','O','O','T','\\','f','r','y','\\','S','Y','S','I','N','F','O','.','F','R','Y',0},
                {'\\','E','F','I','\\','B','O','O','T','\\','F','R','Y','\\','S','Y','S','I','N','F','O','.','F','R','Y',0},
            },
            {
                {'\\','f','r','y','\\','U','P','T','I','M','E','.','F','R','Y',0},
                {'\\','F','R','Y','\\','U','P','T','I','M','E','.','F','R','Y',0},
                {'\\','U','P','T','I','M','E','.','F','R','Y',0},
                {'\\','E','F','I','\\','f','r','y','\\','U','P','T','I','M','E','.','F','R','Y',0},
                {'\\','E','F','I','\\','F','R','Y','\\','U','P','T','I','M','E','.','F','R','Y',0},
                {'\\','E','F','I','\\','B','O','O','T','\\','U','P','T','I','M','E','.','F','R','Y',0},
                {'\\','E','F','I','\\','B','O','O','T','\\','f','r','y','\\','U','P','T','I','M','E','.','F','R','Y',0},
                {'\\','E','F','I','\\','B','O','O','T','\\','F','R','Y','\\','U','P','T','I','M','E','.','F','R','Y',0},
            },
            {
                {'\\','f','r','y','\\','P','S','.','F','R','Y',0},
                {'\\','F','R','Y','\\','P','S','.','F','R','Y',0},
                {'\\','P','S','.','F','R','Y',0},
                {'\\','E','F','I','\\','f','r','y','\\','P','S','.','F','R','Y',0},
                {'\\','E','F','I','\\','F','R','Y','\\','P','S','.','F','R','Y',0},
                {'\\','E','F','I','\\','B','O','O','T','\\','P','S','.','F','R','Y',0},
                {'\\','E','F','I','\\','B','O','O','T','\\','f','r','y','\\','P','S','.','F','R','Y',0},
                {'\\','E','F','I','\\','B','O','O','T','\\','F','R','Y','\\','P','S','.','F','R','Y',0},
            },
            {
                {'\\','f','r','y','\\','F','I','L','E','M','A','N','.','F','R','Y',0},
                {'\\','F','R','Y','\\','F','I','L','E','M','A','N','.','F','R','Y',0},
                {'\\','F','I','L','E','M','A','N','.','F','R','Y',0},
                {'\\','E','F','I','\\','f','r','y','\\','F','I','L','E','M','A','N','.','F','R','Y',0},
                {'\\','E','F','I','\\','F','R','Y','\\','F','I','L','E','M','A','N','.','F','R','Y',0},
                {'\\','E','F','I','\\','B','O','O','T','\\','F','I','L','E','M','A','N','.','F','R','Y',0},
                {'\\','E','F','I','\\','B','O','O','T','\\','f','r','y','\\','F','I','L','E','M','A','N','.','F','R','Y',0},
                {'\\','E','F','I','\\','B','O','O','T','\\','F','R','Y','\\','F','I','L','E','M','A','N','.','F','R','Y',0},
            },
        };
        static const char *fry_names[FRY_FILE_COUNT] = {
            "GUI.FRY", "SHELL.TOT", "SYSINFO.FRY", "UPTIME.FRY", "PS.FRY", "FILEMAN.FRY"
        };
        void  *fry_bufs[FRY_FILE_COUNT];
        UINTN  fry_sizes[FRY_FILE_COUNT];
        for (int fi = 0; fi < FRY_FILE_COUNT; fi++) { fry_bufs[fi] = 0; fry_sizes[fi] = 0; }

        UINTN    fry_total = 0;
        uint32_t fry_found = 0;

        // Helper: try loading all userspace payload files from a given filesystem root.
        // Returns number newly found (added to fry_bufs[]).
        // Defined as an inline block to avoid needing a nested function.
        #define TRY_LOAD_FRY(fs_root) do {                                    \
            for (int fi = 0; fi < FRY_FILE_COUNT; fi++) {                     \
                if (fry_bufs[fi]) continue;  /* already loaded */             \
                for (int pv = 0; pv < FRY_PATH_VARIANTS && !fry_bufs[fi]; pv++) { \
                    void *fbuf = 0; UINTN fsz = 0;                           \
                    if (!EFI_ERROR(read_file(bs, (fs_root), fry_paths[fi][pv],\
                                             0, &fbuf, &fsz)) && fbuf && fsz > 0) { \
                        fry_bufs[fi]  = fbuf;                                 \
                        fry_sizes[fi] = fsz;                                  \
                        fry_total    += fsz;                                  \
                        fry_found++;                                          \
                    }                                                         \
                }                                                             \
            }                                                                 \
        } while (0)

        // Pass 1: primary filesystem (device that loaded BOOTX64.EFI).
        TRY_LOAD_FRY(root);
        efi_serial_puts("EFI: fry pass1 found=");
        efi_serial_putc('0' + (char)(fry_found & 0xF));
        efi_serial_puts("\n");

        // Pass 2: if primary device had none, scan all SFS handles.
        if (fry_found == 0) {
            efi_serial_puts("EFI: fry scanning all SFS handles\n");
            typedef EFI_STATUS (EFIAPI *LocHB_t)(UINTN, EFI_GUID*, void*,
                                                  UINTN*, EFI_HANDLE**);
            LocHB_t lhb = (LocHB_t)bs->LocateHandleBuffer;
            EFI_HANDLE *handles = 0;
            UINTN num_handles = 0;
            EFI_STATUS hst = lhb(2 /*ByProtocol*/,
                                  (EFI_GUID*)&gSimpleFileSystemProtocolGuid,
                                  0, &num_handles, &handles);
            if (!EFI_ERROR(hst) && handles) {
                for (UINTN hi = 0; hi < num_handles && fry_found < FRY_FILE_COUNT; hi++) {
                    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs2 = 0;
                    if (EFI_ERROR(bs->HandleProtocol(handles[hi],
                            (EFI_GUID*)&gSimpleFileSystemProtocolGuid,
                            (void**)&sfs2)) || !sfs2) continue;
                    EFI_FILE_PROTOCOL *root2 = 0;
                    if (EFI_ERROR(sfs2->OpenVolume(sfs2, &root2)) || !root2) continue;
                    UINTN before = fry_found;
                    TRY_LOAD_FRY(root2);
                    root2->Close(root2);
                    if (fry_found > before) {
                        efi_serial_puts("EFI: fry found on alt SFS handle\n");
                    }
                }
                bs->FreePool(handles);
            }
        }
        #undef TRY_LOAD_FRY

        if (fry_found > 0) {
            UINTN rd_total  = sizeof(struct ramdisk_header) + fry_total;
            UINTN rd_pages  = (rd_total + 4095) / 4096;
            EFI_PHYSICAL_ADDRESS rd_addr = (EFI_PHYSICAL_ADDRESS)TATER_BOOT_IDENTITY_MAX_ADDR;
            if (!EFI_ERROR(bs->AllocatePages(AllocateMaxAddress, EfiLoaderData,
                                              rd_pages, &rd_addr))) {
                struct ramdisk_header *rdhdr =
                    (struct ramdisk_header*)(uintptr_t)rd_addr;
                memclr(rdhdr, rd_total);
                rdhdr->magic = RAMDISK_MAGIC;
                rdhdr->count = 0;
                UINTN cursor = sizeof(struct ramdisk_header);
                for (int fi = 0; fi < FRY_FILE_COUNT; fi++) {
                    if (!fry_bufs[fi]) continue;
                    struct ramdisk_entry *re = &rdhdr->entries[rdhdr->count];
                    const char *nm = fry_names[fi];
                    int ni = 0;
                    while (nm[ni] && ni < 31) { re->name[ni] = nm[ni]; ni++; }
                    re->name[ni] = 0;
                    re->offset = (uint64_t)cursor;
                    re->size   = (uint64_t)fry_sizes[fi];
                    memcopy((uint8_t*)(uintptr_t)rd_addr + cursor,
                            fry_bufs[fi], fry_sizes[fi]);
                    cursor += fry_sizes[fi];
                    rdhdr->count++;
                }
                handoff->ramdisk_base = (uint64_t)(uintptr_t)rd_addr;
                handoff->ramdisk_size = (uint64_t)rd_total;
                efi_serial_puts("EFI: ramdisk built\n");
                efi_puts(SystemTable, L"TATER EFI: ramdisk built\r\n");
            } else {
                efi_serial_puts("EFI: ramdisk alloc failed\n");
            }
        } else {
            efi_serial_puts("EFI: no fry files found (NVMe only)\n");
            efi_puts(SystemTable, L"TATER EFI: no ramdisk (NVMe only)\r\n");
        }
    }

    UINTN map_sz = 0, map_key = 0, desc_sz = 0;
    UINT32 desc_ver = 0;
    st = bs->GetMemoryMap(&map_sz, 0, &map_key, &desc_sz, &desc_ver);
    if (st != EFI_BUFFER_TOO_SMALL) efi_die(SystemTable, L"TATER EFI E11 mmap size\r\n");
    map_sz += desc_sz * 4;
    UINTN map_pages = (map_sz + 4095) / 4096;
    void *map_buf = alloc_pages_below(bs, (EFI_PHYSICAL_ADDRESS)TATER_BOOT_IDENTITY_MAX_ADDR, map_pages);
    if (!map_buf) efi_die(SystemTable, L"TATER EFI E12 mmap pages\r\n");
    st = bs->GetMemoryMap(&map_sz, (EFI_MEMORY_DESCRIPTOR*)map_buf, &map_key, &desc_sz, &desc_ver);
    if (EFI_ERROR(st)) efi_die(SystemTable, L"TATER EFI E13 mmap get\r\n");
    handoff->mmap_base = (uint64_t)(uintptr_t)map_buf;
    handoff->mmap_size = (uint64_t)map_sz;
    handoff->mmap_desc_size = (uint64_t)desc_sz;
    handoff->boot_identity_limit = TATER_BOOT_IDENTITY_LIMIT;
    if (gop) fb_mark(gop, 0x00606000u); // stage 8 (pre-ExitBootServices)
    st = bs->ExitBootServices(ImageHandle, map_key);
    if (EFI_ERROR(st)) {
        st = bs->GetMemoryMap(&map_sz, (EFI_MEMORY_DESCRIPTOR*)map_buf, &map_key, &desc_sz, &desc_ver);
        if (EFI_ERROR(st)) efi_die(SystemTable, L"TATER EFI E14 mmap retry\r\n");
        st = bs->ExitBootServices(ImageHandle, map_key);
        if (EFI_ERROR(st)) efi_die(SystemTable, L"TATER EFI E15 ExitBS\r\n");
    }
    if (gop) fb_mark(gop, 0x00804000u); // stage 9 (post-ExitBootServices)
    kernel_entry_t entry = (kernel_entry_t)(uintptr_t)entry_pa;
    if (gop) fb_mark(gop, 0x00A02000u); // stage 10 (jumping to kernel)
    entry(handoff);
    for (;;) { __asm__ volatile ("hlt"); }
    return EFI_SUCCESS;
}
