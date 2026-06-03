/*
 * linux_elf.c — Linux ELF loader for the TaterTOS compatibility layer.
 *
 * Loads a bare (non-FRY-wrapped) Linux x86_64 ELF into a fresh address
 * space and constructs a conforming System V / Linux initial process stack:
 *
 *     [ argc ][ argv... ][ NULL ][ envp... ][ NULL ][ auxv pairs ][ AT_NULL ]
 *     ... then up high: argv/envp strings, AT_PLATFORM, AT_EXECFN, AT_RANDOM
 *
 * This is original TaterTOS code — no Linux source is incorporated. It is
 * the loader half of the "personality"; the syscall-translation half lives
 * in syscall.c (linux_syscall_dispatch).
 *
 * Static ET_EXEC binaries enter at their own entry point. Dynamically linked
 * ET_EXEC binaries with PT_INTERP map the requested interpreter as ET_DYN,
 * set AT_BASE, and enter the interpreter just like the Linux kernel does.
 */

#include <stdint.h>
#include <errno.h>
#include "linux_compat.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"

/* ---- kernel services (declared locally, same style as elf.c) ---- */
struct vfs_file;
struct vfs_file *vfs_open(const char *path);
int vfs_read(struct vfs_file *f, void *buf, uint32_t len);
int64_t vfs_seek(struct vfs_file *f, int64_t offset, int whence);
void vfs_close(struct vfs_file *f);
uint32_t vfs_size(struct vfs_file *f);
#define LX_SEEK_SET 0
/* Files at/above this size are STREAMED: only the ELF header + program
 * headers are buffered; PT_LOAD segments are read on demand from disk into
 * the mapped pages. This lets huge binaries (e.g. the 243MB claude binary
 * on /nvme) load without a giant contiguous kernel buffer. */
#define LX_STREAM_THRESHOLD (16u * 1024u * 1024u)
int entropy_getbytes(void *buf, uint32_t len);
void kprint(const char *fmt, ...);

/* ---- ELF on-disk structures (self-contained, like elf.c) ---- */
struct lx_ehdr {
    uint8_t  e_ident[16];
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

struct lx_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed));

#define LX_ET_EXEC   2
#define LX_ET_DYN    3
#define LX_EM_X86_64 62
#define LX_PT_LOAD   1
#define LX_PT_INTERP 3
#define LX_PF_X      1
#define LX_PF_W      2

/* Auxiliary vector tags (Linux) */
#define LXAT_NULL      0
#define LXAT_PHDR      3
#define LXAT_PHENT     4
#define LXAT_PHNUM     5
#define LXAT_PAGESZ    6
#define LXAT_BASE      7
#define LXAT_FLAGS     8
#define LXAT_ENTRY     9
#define LXAT_UID      11
#define LXAT_EUID     12
#define LXAT_GID      13
#define LXAT_EGID     14
#define LXAT_PLATFORM 15
#define LXAT_CLKTCK   17
#define LXAT_SECURE   23
#define LXAT_RANDOM   25
#define LXAT_EXECFN   31
#define LXAT_HWCAP    16

/* Number of auxv pairs we emit, INCLUDING the AT_NULL terminator. Must
 * exactly match the AUX() emissions in build_initial_stack(). */
#define LX_AUX_PAIRS  18

#define LX_PAGE       4096ULL
#define LX_STACK_PAGES 2048ULL          /* 8 MiB stack, Linux-conventional */
#define LX_STACK_TOP   USER_VA_TOP       /* 0x0000800000000000 */
#define LX_FRAME_MASK  0x000FFFFFFFFFF000ULL
#define LX_INTERP_BASE 0x0000600000000000ULL

#define LX_ARGV_CAP   64
#define LX_ENVP_CAP   64
#define LX_INTERP_PATH_CAP 128

struct lx_file_image {
    uint64_t phys;            /* backing pages for buf (buffered) or meta (streamed) */
    uint64_t pages;
    uint32_t size;           /* total file size */
    uint8_t *buf;            /* buffered: whole file; streamed: 0 */
    const uint8_t *meta;     /* ELF header + phdr region (= buf when buffered) */
    struct vfs_file *file;   /* streamed: kept-open handle; buffered: 0 */
    uint8_t  streamed;
    const struct lx_ehdr *eh;
    const struct lx_phdr *ph;
};

static uint64_t lx_strlen(const char *s) {
    uint64_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static void lx_memzero(uint8_t *p, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) p[i] = 0;
}

static void lx_memcopy(uint8_t *d, const uint8_t *s, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) d[i] = s[i];
}

/* Write `len` bytes from kernel `src` into user VA `va` of address space
 * `cr3`, walking page by page. Pages must already be mapped present. */
static int lx_poke(uint64_t cr3, uint64_t va, const void *src, uint64_t len) {
    const uint8_t *s = (const uint8_t *)src;
    while (len) {
        uint64_t mapped = vmm_virt_to_phys_user(cr3, va);
        uint64_t frame = mapped & LX_FRAME_MASK;
        if (!frame) return -1;
        uint64_t off = va & 0xFFFULL;
        uint64_t n = LX_PAGE - off;
        if (n > len) n = len;
        uint8_t *kv = (uint8_t *)(uintptr_t)(vmm_phys_to_virt(frame) + off);
        lx_memcopy(kv, s, n);
        s += n;
        va += n;
        len -= n;
    }
    return 0;
}

static int lx_poke64(uint64_t cr3, uint64_t va, uint64_t val) {
    return lx_poke(cr3, va, &val, 8);
}

/* Map one PT_LOAD segment into the target address space. */
static int lx_read_image(const char *path, struct lx_file_image *img) {
    if (!path || !img) return -EINVAL;
    lx_memzero((uint8_t *)img, sizeof(*img));

    struct vfs_file *f = vfs_open(path);
    if (!f) return -ENOENT;
    uint32_t size = vfs_size(f);
    if (size < sizeof(struct lx_ehdr)) {
        vfs_close(f);
        return -ENOEXEC;
    }

    if (size < LX_STREAM_THRESHOLD) {
        /* Whole-file buffered path (proven; used by all small binaries). */
        uint64_t pages = (size + 4095u) / 4096u;
        uint64_t phys = pmm_alloc_pages(pages);
        if (!phys) { vfs_close(f); return -ENOMEM; }
        uint8_t *buf = (uint8_t *)(uintptr_t)vmm_phys_to_virt(phys);
        uint32_t rd = (uint32_t)vfs_read(f, buf, size);
        vfs_close(f);
        if (rd != size) { pmm_free_pages(phys, pages); return -EIO; }
        img->phys = phys;
        img->pages = pages;
        img->size = size;
        img->buf = buf;
        img->meta = buf;
        img->file = 0;
        img->streamed = 0;
        img->eh = (const struct lx_ehdr *)buf;
        return 0;
    }

    /* STREAMED path: buffer only the ELF header + program header table; keep
     * the file open and read PT_LOAD segments on demand at map time. */
    struct lx_ehdr hdr;
    if (vfs_seek(f, 0, LX_SEEK_SET) < 0) { vfs_close(f); return -EIO; }
    if ((uint32_t)vfs_read(f, &hdr, sizeof(hdr)) != sizeof(hdr)) { vfs_close(f); return -EIO; }
    if (hdr.e_phentsize != sizeof(struct lx_phdr) || hdr.e_phnum == 0) { vfs_close(f); return -ENOEXEC; }
    uint64_t ph_table = (uint64_t)hdr.e_phnum * hdr.e_phentsize;
    if (hdr.e_phoff > size || ph_table > (uint64_t)size - hdr.e_phoff) { vfs_close(f); return -ENOEXEC; }
    uint64_t meta_len = hdr.e_phoff + ph_table;   /* [0 .. end of phdr table) */
    if (meta_len > 1u * 1024u * 1024u) { vfs_close(f); return -ENOEXEC; }  /* sanity cap */
    uint64_t pages = (meta_len + 4095u) / 4096u;
    uint64_t phys = pmm_alloc_pages(pages);
    if (!phys) { vfs_close(f); return -ENOMEM; }
    uint8_t *meta = (uint8_t *)(uintptr_t)vmm_phys_to_virt(phys);
    if (vfs_seek(f, 0, LX_SEEK_SET) < 0) { pmm_free_pages(phys, pages); vfs_close(f); return -EIO; }
    if ((uint32_t)vfs_read(f, meta, (uint32_t)meta_len) != (uint32_t)meta_len) {
        pmm_free_pages(phys, pages); vfs_close(f); return -EIO;
    }
    img->phys = phys;
    img->pages = pages;
    img->size = size;
    img->buf = 0;
    img->meta = meta;
    img->file = f;          /* kept open for streamed segment reads */
    img->streamed = 1;
    img->eh = (const struct lx_ehdr *)meta;
    return 0;
}

static void lx_free_image(struct lx_file_image *img) {
    if (!img) return;
    if (img->streamed && img->file) vfs_close(img->file);
    if (img->phys && img->pages) pmm_free_pages(img->phys, img->pages);
    lx_memzero((uint8_t *)img, sizeof(*img));
}

static int lx_validate_image(struct lx_file_image *img, uint16_t want_type,
                             const char *label) {
    if (!img || !img->meta || !img->eh) return -EINVAL;
    const struct lx_ehdr *eh = img->eh;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F' ||
        eh->e_ident[4] != 2 /*ELFCLASS64*/ || eh->e_ident[5] != 1 /*LSB*/) {
        return -ENOEXEC;
    }
    if (eh->e_machine != LX_EM_X86_64) {
        kprint("LINUXELF: %s not x86_64 (e_machine=%u)\n",
               label ? label : "image", eh->e_machine);
        return -ENOEXEC;
    }
    if (eh->e_type != want_type) {
        kprint("LINUXELF: %s wrong ELF type got=%u want=%u\n",
               label ? label : "image", eh->e_type, want_type);
        return -ENOEXEC;
    }
    if (eh->e_phentsize != sizeof(struct lx_phdr) || eh->e_phnum == 0) {
        return -ENOEXEC;
    }
    uint64_t ph_table = eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize;
    if (eh->e_phoff > img->size || ph_table > img->size) {
        return -ENOEXEC;
    }
    img->ph = (const struct lx_phdr *)(img->meta + eh->e_phoff);
    return 0;
}

static int lx_copy_interp_path(const struct lx_file_image *img,
                               char out[LX_INTERP_PATH_CAP]) {
    if (!img || !img->ph || !out) return -EINVAL;
    out[0] = 0;
    for (uint16_t i = 0; i < img->eh->e_phnum; i++) {
        const struct lx_phdr *ph = &img->ph[i];
        if (ph->p_type != LX_PT_INTERP) continue;
        if (ph->p_filesz == 0 || ph->p_filesz >= LX_INTERP_PATH_CAP) return -ENOEXEC;
        if (ph->p_offset > img->size || ph->p_filesz > img->size - ph->p_offset)
            return -ENOEXEC;
        if (img->streamed) {
            if (vfs_seek(img->file, (int64_t)ph->p_offset, LX_SEEK_SET) < 0) return -EIO;
            if ((uint32_t)vfs_read(img->file, out, (uint32_t)ph->p_filesz) != (uint32_t)ph->p_filesz)
                return -EIO;
        } else {
            for (uint64_t j = 0; j < ph->p_filesz; j++)
                out[j] = (char)img->buf[ph->p_offset + j];
        }
        out[ph->p_filesz] = 0;
        if (out[ph->p_filesz - 1] != 0) return -ENOEXEC;
        return 1;
    }
    return 0;
}

static int lx_map_segment_at(uint64_t cr3, const struct lx_phdr *ph,
                             const uint8_t *payload, uint64_t payload_size,
                             uint64_t load_bias, struct vfs_file *stream);

static int lx_map_segment(uint64_t cr3, const struct lx_phdr *ph,
                          const uint8_t *payload, uint64_t payload_size,
                          struct vfs_file *stream) {
    return lx_map_segment_at(cr3, ph, payload, payload_size, 0, stream);
}

static int lx_map_segment_at(uint64_t cr3, const struct lx_phdr *ph,
                             const uint8_t *payload, uint64_t payload_size,
                             uint64_t load_bias, struct vfs_file *stream) {
    uint64_t vaddr = ph->p_vaddr;
    uint64_t memsz = ph->p_memsz;
    uint64_t filesz = ph->p_filesz;
    uint64_t off = ph->p_offset;

    if (vaddr > USER_VA_TOP || load_bias > USER_VA_TOP - vaddr) return -ENOEXEC;
    vaddr += load_bias;
    if (vaddr >= USER_VA_TOP) return -ENOEXEC;
    if (filesz > memsz) return -ENOEXEC;
    if (off > payload_size || filesz > payload_size - off) return -ENOEXEC;
    if (memsz > USER_VA_TOP - vaddr) return -ENOEXEC;

    uint64_t start = vaddr & ~0xFFFULL;
    uint64_t end = (vaddr + memsz + 0xFFFULL) & ~0xFFFULL;

    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
    if (ph->p_flags & LX_PF_W) flags |= VMM_FLAG_WRITE;
    if (!(ph->p_flags & LX_PF_X)) flags |= VMM_FLAG_NO_EXECUTE;

    for (uint64_t v = start; v < end; v += LX_PAGE) {
        /* A segment may share a page with a previously mapped one (when
         * RO and RW segments straddle a page boundary); skip if present. */
        if (vmm_virt_to_phys_user(cr3, v) & LX_FRAME_MASK) continue;
        uint64_t phys = pmm_alloc_page();
        if (!phys) return -ENOMEM;
        vmm_map_user(cr3, v, phys, flags);
        lx_memzero((uint8_t *)(uintptr_t)vmm_phys_to_virt(phys), LX_PAGE);
    }

    if (filesz > 0) {
        if (stream) {
            /* Streamed: read p_filesz bytes from the file at p_offset directly
             * into the freshly-mapped pages (no whole-file buffer). */
            if (vfs_seek(stream, (int64_t)off, LX_SEEK_SET) < 0) return -EIO;
            uint64_t rem = filesz;
            uint64_t v2 = vaddr;
            while (rem > 0) {
                uint64_t frame = vmm_virt_to_phys_user(cr3, v2) & LX_FRAME_MASK;
                if (!frame) return -EFAULT;
                uint64_t poff = v2 & 0xFFFULL;
                uint64_t n = LX_PAGE - poff;
                if (n > rem) n = rem;
                uint8_t *kv = (uint8_t *)(uintptr_t)(vmm_phys_to_virt(frame) + poff);
                int rd = vfs_read(stream, kv, (uint32_t)n);
                if (rd <= 0) return -EIO;
                v2 += (uint64_t)rd;
                rem -= (uint64_t)rd;
            }
        } else {
            if (lx_poke(cr3, vaddr, payload + off, filesz) != 0) return -EFAULT;
        }
    }
    return 0;
}

/* Build the SysV/Linux initial stack. Returns the final user rsp (points
 * at argc, 16-byte aligned) via *rsp_out, or negative errno. */
static int build_initial_stack(uint64_t cr3, const char *path,
                               const char *const *argv, int argc,
                               const char *const *envp, int envc,
                               const struct lx_ehdr *eh,
                               uint64_t phdr_va,
                               uint64_t at_base,
                               uint64_t at_entry,
                               uint64_t *rsp_out) {
    uint64_t argp[LX_ARGV_CAP];
    uint64_t envpp[LX_ENVP_CAP];
    if (argc < 0) argc = 0;
    if (envc < 0) envc = 0;
    if (argc > LX_ARGV_CAP) argc = LX_ARGV_CAP;
    if (envc > LX_ENVP_CAP) envc = LX_ENVP_CAP;

    uint64_t sp = LX_STACK_TOP;

    /* 16 random bytes for AT_RANDOM (stack-guard/PRNG seed). */
    uint8_t rnd[16];
    if (entropy_getbytes(rnd, 16) != 0) {
        for (int i = 0; i < 16; i++) rnd[i] = (uint8_t)(0x9Eu * (i + 1) + 0x37u);
    }
    sp -= 16;
    uint64_t a_random = sp;
    if (lx_poke(cr3, sp, rnd, 16) != 0) return -EFAULT;

    /* AT_PLATFORM string */
    static const char plat[] = "x86_64";
    sp -= sizeof(plat);
    uint64_t a_platform = sp;
    if (lx_poke(cr3, sp, plat, sizeof(plat)) != 0) return -EFAULT;

    /* AT_EXECFN string (the program path) */
    uint64_t pathlen = lx_strlen(path) + 1;
    sp -= pathlen;
    uint64_t a_execfn = sp;
    if (lx_poke(cr3, sp, path, pathlen) != 0) return -EFAULT;

    /* envp strings */
    for (int i = envc - 1; i >= 0; i--) {
        uint64_t len = lx_strlen(envp[i]) + 1;
        sp -= len;
        envpp[i] = sp;
        if (lx_poke(cr3, sp, envp[i], len) != 0) return -EFAULT;
    }
    /* argv strings */
    for (int i = argc - 1; i >= 0; i--) {
        uint64_t len = lx_strlen(argv[i]) + 1;
        sp -= len;
        argp[i] = sp;
        if (lx_poke(cr3, sp, argv[i], len) != 0) return -EFAULT;
    }

    /* 16-align the bottom of the string area. */
    sp &= ~0xFULL;

    /* Total 8-byte slots in the info block. */
    uint64_t nslots = 1 + (uint64_t)(argc + 1) + (uint64_t)(envc + 1)
                    + (uint64_t)(2 * LX_AUX_PAIRS);
    if (nslots & 1ULL) nslots++;   /* pad so &argc stays 16-aligned */
    sp -= nslots * 8ULL;
    uint64_t base = sp;
    uint64_t va = sp;

    if (lx_poke64(cr3, va, (uint64_t)argc) != 0) { return -EFAULT; }
    va += 8;
    for (int i = 0; i < argc; i++) {
        if (lx_poke64(cr3, va, argp[i]) != 0) { return -EFAULT; }
        va += 8;
    }
    if (lx_poke64(cr3, va, 0) != 0) { return -EFAULT; }   /* argv NULL */
    va += 8;
    for (int i = 0; i < envc; i++) {
        if (lx_poke64(cr3, va, envpp[i]) != 0) { return -EFAULT; }
        va += 8;
    }
    if (lx_poke64(cr3, va, 0) != 0) { return -EFAULT; }   /* envp NULL */
    va += 8;

#define AUX(t, v) do { \
        if (lx_poke64(cr3, va, (uint64_t)(t)) != 0) { return -EFAULT; } \
        va += 8; \
        if (lx_poke64(cr3, va, (uint64_t)(v)) != 0) { return -EFAULT; } \
        va += 8; \
    } while (0)
    AUX(LXAT_PHDR,     phdr_va);
    AUX(LXAT_PHENT,    eh->e_phentsize);
    AUX(LXAT_PHNUM,    eh->e_phnum);
    AUX(LXAT_PAGESZ,   LX_PAGE);
    AUX(LXAT_BASE,     at_base);
    AUX(LXAT_FLAGS,    0);
    AUX(LXAT_ENTRY,    at_entry);
    AUX(LXAT_UID,      0);
    AUX(LXAT_EUID,     0);
    AUX(LXAT_GID,      0);
    AUX(LXAT_EGID,     0);
    AUX(LXAT_SECURE,   0);
    AUX(LXAT_RANDOM,   a_random);
    AUX(LXAT_CLKTCK,   100);
    AUX(LXAT_HWCAP,    0);
    AUX(LXAT_PLATFORM, a_platform);
    AUX(LXAT_EXECFN,   a_execfn);
    AUX(LXAT_NULL,     0);
#undef AUX

    *rsp_out = base;
    return 0;
}

int elf_load_linux(const char *path,
                   const char *const *argv, int argc,
                   const char *const *envp, int envc,
                   uint64_t *cr3_out, uint64_t *entry_out,
                   uint64_t *rsp_out, uint64_t *brk_out) {
    if (!path || !cr3_out || !entry_out || !rsp_out || !brk_out)
        return -EINVAL;

    struct lx_file_image main_img;
    struct lx_file_image interp_img;
    lx_memzero((uint8_t *)&main_img, sizeof(main_img));
    lx_memzero((uint8_t *)&interp_img, sizeof(interp_img));

    int rc = lx_read_image(path, &main_img);
    if (rc != 0) return rc;
    rc = lx_validate_image(&main_img, LX_ET_EXEC, "main");
    if (rc != 0) {
        lx_free_image(&main_img);
        return rc;
    }

    char interp_path[LX_INTERP_PATH_CAP];
    int has_interp = lx_copy_interp_path(&main_img, interp_path);
    if (has_interp < 0) {
        lx_free_image(&main_img);
        return has_interp;
    }
    if (has_interp) {
        rc = lx_read_image(interp_path, &interp_img);
        if (rc != 0) {
            kprint("LINUXELF: PT_INTERP %s unavailable rc=%d\n", interp_path, rc);
            lx_free_image(&main_img);
            return rc;
        }
        rc = lx_validate_image(&interp_img, LX_ET_DYN, "interpreter");
        if (rc != 0) {
            lx_free_image(&interp_img);
            lx_free_image(&main_img);
            return rc;
        }
    }

    uint64_t cr3 = vmm_create_address_space();
    if (!cr3) {
        lx_free_image(&interp_img);
        lx_free_image(&main_img);
        return -ENOMEM;
    }

    /* Map all PT_LOAD segments; track highest end for brk and locate the
     * segment that contains the program header table (for AT_PHDR). */
    uint64_t brk_end = 0;
    uint64_t phdr_va = 0;
    const struct lx_ehdr *eh = main_img.eh;
    const struct lx_phdr *ph = main_img.ph;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != LX_PT_LOAD) continue;
        rc = lx_map_segment(cr3, &ph[i], main_img.buf, main_img.size, main_img.file);
        if (rc != 0) {
            vmm_destroy_address_space(cr3);
            lx_free_image(&interp_img);
            lx_free_image(&main_img);
            return rc;
        }
        uint64_t seg_end = ph[i].p_vaddr + ph[i].p_memsz;
        if (seg_end > brk_end) brk_end = seg_end;
        if (eh->e_phoff >= ph[i].p_offset &&
            eh->e_phoff <  ph[i].p_offset + ph[i].p_filesz) {
            phdr_va = ph[i].p_vaddr + (eh->e_phoff - ph[i].p_offset);
        }
    }

    if (eh->e_entry >= USER_VA_TOP) {
        vmm_destroy_address_space(cr3);
        lx_free_image(&interp_img);
        lx_free_image(&main_img);
        return -ENOEXEC;
    }

    uint64_t entry = eh->e_entry;
    uint64_t at_base = 0;
    uint64_t at_entry = eh->e_entry;
    if (has_interp) {
        at_base = LX_INTERP_BASE;
        const struct lx_ehdr *ieh = interp_img.eh;
        const struct lx_phdr *iph = interp_img.ph;
        for (uint16_t i = 0; i < ieh->e_phnum; i++) {
            if (iph[i].p_type != LX_PT_LOAD) continue;
            rc = lx_map_segment_at(cr3, &iph[i], interp_img.buf,
                                   interp_img.size, LX_INTERP_BASE,
                                   interp_img.file);
            if (rc != 0) {
                vmm_destroy_address_space(cr3);
                lx_free_image(&interp_img);
                lx_free_image(&main_img);
                return rc;
            }
        }
        if (ieh->e_entry > USER_VA_TOP ||
            LX_INTERP_BASE > USER_VA_TOP - ieh->e_entry) {
            vmm_destroy_address_space(cr3);
            lx_free_image(&interp_img);
            lx_free_image(&main_img);
            return -ENOEXEC;
        }
        entry = LX_INTERP_BASE + ieh->e_entry;
        kprint("LINUXELF: dynamic PT_INTERP=%s base=%lx entry=%lx\n",
               interp_path, (unsigned long)LX_INTERP_BASE,
               (unsigned long)entry);
    }

    /* Allocate + zero the stack. */
    uint64_t stack_base = LX_STACK_TOP - LX_STACK_PAGES * LX_PAGE;
    for (uint64_t i = 0; i < LX_STACK_PAGES; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            vmm_destroy_address_space(cr3);
            lx_free_image(&interp_img);
            lx_free_image(&main_img);
            return -ENOMEM;
        }
        vmm_map_user(cr3, stack_base + i * LX_PAGE, phys,
                     VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER | VMM_FLAG_NO_EXECUTE);
        lx_memzero((uint8_t *)(uintptr_t)vmm_phys_to_virt(phys), LX_PAGE);
    }

    uint64_t rsp = 0;
    rc = build_initial_stack(cr3, path, argv, argc, envp, envc,
                             eh, phdr_va, at_base, at_entry, &rsp);
    lx_free_image(&interp_img);
    lx_free_image(&main_img);
    if (rc != 0) {
        vmm_destroy_address_space(cr3);
        return rc;
    }

    *cr3_out = cr3;
    *entry_out = entry;
    *rsp_out = rsp;
    *brk_out = (brk_end + 0xFFFULL) & ~0xFFFULL;
    return 0;
}
