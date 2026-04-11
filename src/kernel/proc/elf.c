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
#define PF_X 1
#define PF_W 2

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

int elf_load_fry(const char *path, uint64_t *cr3_out, uint64_t *entry_out, uint64_t *user_rsp_out) {
    if (!path || !cr3_out || !entry_out || !user_rsp_out) return elf_fail(ELF_LOAD_ERR_BAD_ARGS);
    g_elf_last_error = ELF_LOAD_OK;

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

    *cr3_out = pml4_phys;
    *entry_out = eh->e_entry;
    // iretq does not push a return address; adjust to keep 16-byte ABI alignment.
    *user_rsp_out = USER_STACK_TOP - 8;
    temp_free_pages(tmp_phys, tmp_pages);
    g_elf_last_error = ELF_LOAD_OK;
    return 0;
}
