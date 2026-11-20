// ELF loader for userspace containers (.fry + SHELL.TOT)

#include <stdint.h>
#include "elf.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"

struct vfs_file;
struct vfs_file *vfs_open(const char *path);
int vfs_read(struct vfs_file *f, void *buf, uint32_t len);
void vfs_close(struct vfs_file *f);
uint32_t vfs_size(struct vfs_file *f);

#define FRY_MAGIC 0x30595246u // "FRY0"
#define TOT_MAGIC 0x31544F54u // "TOT1"
#define USER_STACK_TOP USER_VA_TOP
#define USER_STACK_PAGES 4096   /* 16MB — TaterSurf build_page needs ~5MB frame */

struct fry_header {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t crc32;
    uint32_t payload_size;
} __attribute__((packed));

struct elf64_ehdr {
    uint8_t e_ident[16];
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
} __attribute__((packed));

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed));

#define PT_LOAD 1
#define PT_TLS  7
#define PF_X 1
#define PF_W 2

/* Native .fry ELF TLS (variant II, x86-64).  The TLS image + thread-control
 * block live at a fixed VA in the unused gap between the mmap arena top
 * (FRY_VM_USER_LIMIT = 0x7FFF00000000) and the user stack base
 * (USER_STACK_TOP - 16MB = 0x7FFFFF000000).  Above VM_USER_LIMIT, so SYS_MMAP /
 * vm_find_free_range never collide with it. */
#define FRY_TLS_REGION_BASE 0x00007FFF80000000ULL
#define FRY_TLS_TCB_RESERVE 0x1000ULL          /* room for libc struct fry_tls_block */
#define FRY_TLS_MEMSZ_MAX   0x00400000ULL       /* 4MB sanity cap on the TLS image */

static uint32_t crc32_table[256];
static int crc_init_done;
static int g_elf_last_error;

static int elf_fail(int code) {
    g_elf_last_error = code;
    return code;
}

static void crc32_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) {
            if (c & 1) c = 0xEDB88320u ^ (c >> 1);
            else c >>= 1;
        }
        crc32_table[i] = c;
    }
    crc_init_done = 1;
}

static uint32_t crc32_calc(const uint8_t *buf, uint32_t len) {
    if (!crc_init_done) crc32_init();
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        c = crc32_table[(c ^ buf[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

static void mem_zero(uint8_t *p, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) p[i] = 0;
}

static void mem_copy(uint8_t *dst, const uint8_t *src, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) dst[i] = src[i];
}

static char ascii_upper(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - ('a' - 'A'));
    return c;
}

static const char *path_basename(const char *path) {
    const char *base = path;
    if (!path) return "";
    while (*path) {
        if (*path == '/') base = path + 1;
        path++;
    }
    return base;
}

static int streq_ci(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (ascii_upper(*a) != ascii_upper(*b)) return 0;
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static int path_is_shell_tot(const char *path) {
    return streq_ci(path_basename(path), "SHELL.TOT");
}

static uint8_t *temp_alloc_pages(uint32_t size, uint64_t *phys_out, uint64_t *pages_out) {
    if (!phys_out || !pages_out) return 0;
    uint64_t pages = (size + 4095ULL) / 4096ULL;
    if (pages == 0) pages = 1;
    uint64_t phys = pmm_alloc_pages(pages);
    if (!phys) return 0;
    *phys_out = phys;
    *pages_out = pages;
    return (uint8_t *)(uintptr_t)vmm_phys_to_virt(phys);
}

static void temp_free_pages(uint64_t phys, uint64_t pages) {
    if (!phys || !pages) return;
    pmm_free_pages(phys, pages);
}

static uint64_t pml4_virt_to_phys(uint64_t pml4_phys, uint64_t virt) {
    uint64_t pml4_i = (virt >> 39) & 0x1FF;
    uint64_t pdpt_i = (virt >> 30) & 0x1FF;
    uint64_t pd_i   = (virt >> 21) & 0x1FF;
    uint64_t pt_i   = (virt >> 12) & 0x1FF;

    uint64_t *pml4 = (uint64_t *)(uintptr_t)vmm_phys_to_virt(pml4_phys);
    if (!(pml4[pml4_i] & VMM_FLAG_PRESENT)) return 0;
    uint64_t *pdpt = (uint64_t *)(uintptr_t)vmm_phys_to_virt(pml4[pml4_i] & 0x000FFFFFFFFFF000ULL);
    if (!(pdpt[pdpt_i] & VMM_FLAG_PRESENT)) return 0;
    uint64_t pdpe = pdpt[pdpt_i];
    if (pdpe & VMM_FLAG_LARGE) {
        return (pdpe & 0x000FFFFFFFFFF000ULL) + (virt & 0x3FFFFFFFULL);
    }
    uint64_t *pd = (uint64_t *)(uintptr_t)vmm_phys_to_virt(pdpe & 0x000FFFFFFFFFF000ULL);
    if (!(pd[pd_i] & VMM_FLAG_PRESENT)) return 0;
    uint64_t pde = pd[pd_i];
    if (pde & VMM_FLAG_LARGE) {
        return (pde & 0x000FFFFFFFFFF000ULL) + (virt & 0x1FFFFFULL);
    }
    uint64_t *pt = (uint64_t *)(uintptr_t)vmm_phys_to_virt(pde & 0x000FFFFFFFFFF000ULL);
    if (!(pt[pt_i] & VMM_FLAG_PRESENT)) return 0;
    return (pt[pt_i] & 0x000FFFFFFFFFF000ULL) + (virt & 0xFFFULL);
}

int elf_load_fry(const char *path, uint64_t *cr3_out, uint64_t *entry_out, uint64_t *user_rsp_out,
                 uint64_t *tls_base_out, struct fry_tls_template *tmpl_out) {
    if (!path || !cr3_out || !entry_out || !user_rsp_out) return elf_fail(ELF_LOAD_ERR_BAD_ARGS);
    g_elf_last_error = ELF_LOAD_OK;
    if (tls_base_out) *tls_base_out = 0;
    if (tmpl_out) { tmpl_out->has_tls = 0; tmpl_out->vaddr = 0; tmpl_out->filesz = 0;
                    tmpl_out->memsz = 0; tmpl_out->align = 0; tmpl_out->tls_alloc = 0; }

    struct vfs_file *f = vfs_open(path);
    if (!f) return elf_fail(ELF_LOAD_ERR_OPEN);
    uint64_t tmp_phys = 0;
    uint64_t tmp_pages = 0;
    uint32_t size = vfs_size(f);
    if (size < sizeof(struct fry_header)) { vfs_close(f); return elf_fail(ELF_LOAD_ERR_SHORT_HEADER); }
    uint8_t *buf = temp_alloc_pages(size, &tmp_phys, &tmp_pages);
    if (!buf) { vfs_close(f); return elf_fail(ELF_LOAD_ERR_NOMEM); }
    uint32_t rd = (uint32_t)vfs_read(f, buf, size);
    vfs_close(f);
    if (rd != size) { temp_free_pages(tmp_phys, tmp_pages); return elf_fail(ELF_LOAD_ERR_READ); }

    const struct fry_header *h = (const struct fry_header *)buf;
    if (h->magic == TOT_MAGIC) {
        if (!path_is_shell_tot(path)) {
            temp_free_pages(tmp_phys, tmp_pages);
            return elf_fail(ELF_LOAD_ERR_BAD_MAGIC);
        }
    } else if (h->magic != FRY_MAGIC) {
        temp_free_pages(tmp_phys, tmp_pages);
        return elf_fail(ELF_LOAD_ERR_BAD_MAGIC);
    }
    if (sizeof(struct fry_header) + h->payload_size > size) {
        temp_free_pages(tmp_phys, tmp_pages);
        return elf_fail(ELF_LOAD_ERR_BOUNDS);
    }

    const uint8_t *payload = buf + sizeof(struct fry_header);
    uint32_t crc = crc32_calc(payload, h->payload_size);
    if (crc != h->crc32) {
        temp_free_pages(tmp_phys, tmp_pages);
        return elf_fail(ELF_LOAD_ERR_BAD_CRC);
    }

    if (h->payload_size < sizeof(struct elf64_ehdr)) {
        temp_free_pages(tmp_phys, tmp_pages);
        return elf_fail(ELF_LOAD_ERR_BAD_ELF_HEADER);
    }
    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)payload;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' || eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F') {
        temp_free_pages(tmp_phys, tmp_pages);
        return elf_fail(ELF_LOAD_ERR_BAD_ELF_MAGIC);
    }

    uint64_t pml4_phys = vmm_create_address_space();
    if (!pml4_phys) { temp_free_pages(tmp_phys, tmp_pages); return elf_fail(ELF_LOAD_ERR_VMM_SPACE); }

    if (eh->e_phentsize != sizeof(struct elf64_phdr)) {
        vmm_destroy_address_space(pml4_phys);
        temp_free_pages(tmp_phys, tmp_pages);
        return elf_fail(ELF_LOAD_ERR_BAD_ELF_HEADER);
    }
    if (eh->e_phoff > h->payload_size) {
        vmm_destroy_address_space(pml4_phys);
        temp_free_pages(tmp_phys, tmp_pages);
        return elf_fail(ELF_LOAD_ERR_BOUNDS);
    }
    uint64_t ph_table_size = (uint64_t)eh->e_phnum * (uint64_t)eh->e_phentsize;
    if (ph_table_size > (uint64_t)h->payload_size - eh->e_phoff) {
        vmm_destroy_address_space(pml4_phys);
        temp_free_pages(tmp_phys, tmp_pages);
        return elf_fail(ELF_LOAD_ERR_BOUNDS);
    }

    const struct elf64_phdr *ph = (const struct elf64_phdr *)(payload + eh->e_phoff);
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        uint64_t vaddr = ph[i].p_vaddr;
        uint64_t memsz = ph[i].p_memsz;
        uint64_t filesz = ph[i].p_filesz;
        uint64_t off = ph[i].p_offset;
        if (vaddr >= USER_VA_TOP) {
            vmm_destroy_address_space(pml4_phys);
            temp_free_pages(tmp_phys, tmp_pages);
            return elf_fail(ELF_LOAD_ERR_BOUNDS);
        }
        if (filesz > memsz) {
            vmm_destroy_address_space(pml4_phys);
            temp_free_pages(tmp_phys, tmp_pages);
            return elf_fail(ELF_LOAD_ERR_BOUNDS);
        }
        if (off > h->payload_size || filesz > (uint64_t)h->payload_size - off) {
            vmm_destroy_address_space(pml4_phys);
            temp_free_pages(tmp_phys, tmp_pages);
            return elf_fail(ELF_LOAD_ERR_BOUNDS);
        }
        if (memsz > UINT64_MAX - vaddr) {
            vmm_destroy_address_space(pml4_phys);
            temp_free_pages(tmp_phys, tmp_pages);
            return elf_fail(ELF_LOAD_ERR_BOUNDS);
        }
        if (memsz > USER_VA_TOP - vaddr) {
            vmm_destroy_address_space(pml4_phys);
            temp_free_pages(tmp_phys, tmp_pages);
            return elf_fail(ELF_LOAD_ERR_BOUNDS);
        }
        uint64_t seg_end = vaddr + memsz;
        if (seg_end > UINT64_MAX - 0xFFFULL) {
            vmm_destroy_address_space(pml4_phys);
            temp_free_pages(tmp_phys, tmp_pages);
            return elf_fail(ELF_LOAD_ERR_BOUNDS);
        }

        uint64_t start = vaddr & ~0xFFFULL;
        uint64_t end = (seg_end + 0xFFFULL) & ~0xFFFULL;

        uint64_t flags = VMM_FLAG_USER;
        if (ph[i].p_flags & PF_W) flags |= VMM_FLAG_WRITE;
        if (!(ph[i].p_flags & PF_X)) flags |= VMM_FLAG_NO_EXECUTE;

        for (uint64_t va = start; va < end; va += 4096) {
            uint64_t phys = pmm_alloc_page();
            if (!phys) { vmm_destroy_address_space(pml4_phys); temp_free_pages(tmp_phys, tmp_pages); return elf_fail(ELF_LOAD_ERR_SEG_ALLOC); }
            vmm_map_user(pml4_phys, va, phys, flags);
            mem_zero((uint8_t *)(uintptr_t)vmm_phys_to_virt(phys), 4096);
        }

        if (filesz > 0) {
            const uint8_t *src = payload + off;
            uint64_t remaining = filesz;
            uint64_t va = vaddr;
            while (remaining > 0) {
                uint64_t phys = pml4_virt_to_phys(pml4_phys, va);
                if (!phys) { vmm_destroy_address_space(pml4_phys); temp_free_pages(tmp_phys, tmp_pages); return elf_fail(ELF_LOAD_ERR_SEG_TRANSLATE); }
                uint64_t page_off = va & 0xFFFULL;
                uint64_t to_copy = 4096 - page_off;
                if (to_copy > remaining) to_copy = remaining;
                mem_copy((uint8_t *)(uintptr_t)(vmm_phys_to_virt(phys) + page_off), src, to_copy);
                src += to_copy;
                va += to_copy;
                remaining -= to_copy;
            }
        }
    }

    if (eh->e_entry >= USER_VA_TOP) {
        vmm_destroy_address_space(pml4_phys);
        temp_free_pages(tmp_phys, tmp_pages);
        return elf_fail(ELF_LOAD_ERR_BOUNDS);
    }

    uint64_t stack_base = USER_STACK_TOP - USER_STACK_PAGES * 4096ULL;
    for (uint64_t i = 0; i < USER_STACK_PAGES; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) { vmm_destroy_address_space(pml4_phys); temp_free_pages(tmp_phys, tmp_pages); return elf_fail(ELF_LOAD_ERR_STACK_ALLOC); }
        vmm_map_user(pml4_phys, stack_base + i * 4096ULL, phys, VMM_FLAG_WRITE | VMM_FLAG_USER | VMM_FLAG_NO_EXECUTE);
        mem_zero((uint8_t *)(uintptr_t)vmm_phys_to_virt(phys), 4096);
    }

    /* ---- ELF TLS (variant II, x86-64) ----------------------------------
     * Lay out the PT_TLS image so __thread vars (CPython _Py_tss_tstate,
     * mimalloc _mi_heap_default, ...) have real backing at their linker-
     * assigned negative offsets from %fs.  Segments are already mapped, and
     * the file payload buffer is still live, so the .tdata init image is read
     * straight from the payload. */
    uint64_t tls_off = 0, tls_filesz = 0, tls_memsz = 0, tls_align = 0, tls_vaddr = 0;
    int has_tls = 0;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_TLS) continue;
        tls_off    = ph[i].p_offset;
        tls_vaddr  = ph[i].p_vaddr;     /* fry1790: pristine .tdata source VA */
        tls_filesz = ph[i].p_filesz;
        tls_memsz  = ph[i].p_memsz;
        tls_align  = ph[i].p_align;
        has_tls = 1;
        break;                          /* ABI permits at most one PT_TLS */
    }

    if (has_tls && tls_memsz > 0) {
        /* Honor the ELF p_align exactly: the linker computed the local-exec tpoffs
         * using tls_block_size = align_up(p_memsz, p_align).  Clamping the align UP
         * (was: <16 -> 16) makes the kernel's tls_alloc larger than the linker's
         * block, shifting tp so every __thread var misses by the delta (fry1720).
         * p_align is a power of two; guard only the degenerate 0 case. */
        if (tls_align < 1) tls_align = 1;
        if (tls_align > 0x200000ULL ||                       /* > 2MB align: refuse */
            tls_filesz > tls_memsz ||
            tls_memsz > FRY_TLS_MEMSZ_MAX ||
            tls_off > h->payload_size ||
            tls_filesz > (uint64_t)h->payload_size - tls_off) {
            vmm_destroy_address_space(pml4_phys);
            temp_free_pages(tmp_phys, tmp_pages);
            return elf_fail(ELF_LOAD_ERR_BOUNDS);
        }

        uint64_t tls_alloc  = (tls_memsz + (tls_align - 1)) & ~(tls_align - 1);
        uint64_t region_base = FRY_TLS_REGION_BASE;          /* TLS image start */
        uint64_t tp          = region_base + tls_alloc;      /* %fs / thread pointer */
        uint64_t map_start   = region_base;                  /* page-aligned by const */
        uint64_t map_end     = (tp + FRY_TLS_TCB_RESERVE + 0xFFFULL) & ~0xFFFULL;

        if (map_end > USER_STACK_TOP - USER_STACK_PAGES * 4096ULL) {
            vmm_destroy_address_space(pml4_phys);
            temp_free_pages(tmp_phys, tmp_pages);
            return elf_fail(ELF_LOAD_ERR_BOUNDS);
        }

        for (uint64_t va = map_start; va < map_end; va += 4096) {
            uint64_t phys = pmm_alloc_page();
            if (!phys) {
                vmm_destroy_address_space(pml4_phys);
                temp_free_pages(tmp_phys, tmp_pages);
                return elf_fail(ELF_LOAD_ERR_TLS_ALLOC);
            }
            vmm_map_user(pml4_phys, va, phys, VMM_FLAG_WRITE | VMM_FLAG_USER | VMM_FLAG_NO_EXECUTE);
            mem_zero((uint8_t *)(uintptr_t)vmm_phys_to_virt(phys), 4096);
        }

        /* Copy .tdata init image into [region_base, region_base+filesz).
         * region_base is page-aligned so each chunk starts at page offset 0. */
        uint64_t copied = 0;
        while (copied < tls_filesz) {
            uint64_t va = region_base + copied;
            uint64_t phys = pml4_virt_to_phys(pml4_phys, va & ~0xFFFULL);
            if (!phys) {
                vmm_destroy_address_space(pml4_phys);
                temp_free_pages(tmp_phys, tmp_pages);
                return elf_fail(ELF_LOAD_ERR_SEG_TRANSLATE);
            }
            uint64_t page_off = va & 0xFFFULL;
            uint64_t chunk = 4096 - page_off;
            if (chunk > tls_filesz - copied) chunk = tls_filesz - copied;
            mem_copy((uint8_t *)(uintptr_t)(vmm_phys_to_virt(phys) + page_off),
                     payload + tls_off + copied, chunk);
            copied += chunk;
        }
        /* x86-64 variant-II ABI: the word at the thread pointer (fs:0) MUST hold
         * the thread-pointer self-value (glibc tcbhead_t.tcb).  CPython & mimalloc
         * read the thread pointer via __builtin_thread_pointer() (mov %fs:0,%reg)
         * for every __thread access, so without this self-pointer all __thread
         * vars resolve against a garbage base and this TLS region is never used.
         * Write it through the user page (the kernel cannot deref the user VA). */
        {
            uint64_t tcb_phys = pml4_virt_to_phys(pml4_phys, tp & ~0xFFFULL);
            if (!tcb_phys) {
                vmm_destroy_address_space(pml4_phys);
                temp_free_pages(tmp_phys, tmp_pages);
                return elf_fail(ELF_LOAD_ERR_SEG_TRANSLATE);
            }
            /* tp is >=16-aligned (tls_align>=16), so the 8-byte store never
             * crosses the page boundary at tp & ~0xFFF. */
            *(uint64_t *)(uintptr_t)(vmm_phys_to_virt(tcb_phys) + (tp & 0xFFFULL)) = tp;
        }
        /* .tbss [region_base+filesz, tp) stays zero from the map loop; the TCB
         * positive area (tp+8 .. tp+reserve) is libc's struct fry_tls_block. */

        if (tls_base_out) *tls_base_out = tp;
        if (tmpl_out) {
            tmpl_out->has_tls   = 1;
            tmpl_out->vaddr     = tls_vaddr;
            tmpl_out->filesz    = tls_filesz;
            tmpl_out->memsz     = tls_memsz;
            tmpl_out->align     = tls_align;
            tmpl_out->tls_alloc = tls_alloc;
        }
    }

    *cr3_out = pml4_phys;
    *entry_out = eh->e_entry;
    // iretq does not push a return address; adjust to keep 16-byte ABI alignment.
    *user_rsp_out = USER_STACK_TOP - 8;
    temp_free_pages(tmp_phys, tmp_pages);
    g_elf_last_error = ELF_LOAD_OK;
    return 0;
}
