#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/kernel/proc/elf.h"

#define FRY_MAGIC 0x30595246u
#define VERSION 1
#define FLAG_ELF64 0x0001

#define PT_LOAD 1
#define PF_X 1

struct test_fry_header {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t crc32;
    uint32_t payload_size;
} __attribute__((packed));

struct test_elf64_ehdr {
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

struct test_elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed));

struct vfs_file {
    FILE *fp;
    uint32_t size;
};

static uint32_t crc32_calc(const uint8_t *buf, uint32_t len) {
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        c ^= buf[i];
        for (uint32_t b = 0; b < 8; b++) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
    }
    return c ^ 0xFFFFFFFFu;
}

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) {
        fprintf(stderr, "malloc(%zu) failed\n", n);
        exit(2);
    }
    return p;
}

static void write_file_or_die(const char *path, const uint8_t *buf, size_t len) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "fopen(%s) failed: %s\n", path, strerror(errno));
        exit(2);
    }
    if (fwrite(buf, 1, len, fp) != len) {
        fprintf(stderr, "fwrite(%s) failed\n", path);
        fclose(fp);
        exit(2);
    }
    fclose(fp);
}

static void build_base_payload(uint8_t **payload_out, uint32_t *payload_len_out) {
    uint32_t payload_len = 0x180u;
    uint8_t *payload = (uint8_t *)calloc(1, payload_len);
    if (!payload) {
        fprintf(stderr, "calloc failed\n");
        exit(2);
    }

    struct test_elf64_ehdr *eh = (struct test_elf64_ehdr *)payload;
    eh->e_ident[0] = 0x7Fu;
    eh->e_ident[1] = 'E';
    eh->e_ident[2] = 'L';
    eh->e_ident[3] = 'F';
    eh->e_ident[4] = 2;
    eh->e_ident[5] = 1;
    eh->e_ident[6] = 1;
    eh->e_phoff = sizeof(*eh);
    eh->e_ehsize = (uint16_t)sizeof(*eh);
    eh->e_phentsize = (uint16_t)sizeof(struct test_elf64_phdr);
    eh->e_phnum = 1;
    eh->e_entry = 0x0000000000400000ULL;

    struct test_elf64_phdr *ph = (struct test_elf64_phdr *)(payload + eh->e_phoff);
    ph->p_type = PT_LOAD;
    ph->p_flags = PF_X;
    ph->p_offset = 0x100u;
    ph->p_vaddr = 0x0000000000400000ULL;
    ph->p_filesz = 4;
    ph->p_memsz = 4;
    ph->p_align = 0x1000u;

    payload[0x100] = 0xAA;
    payload[0x101] = 0xBB;
    payload[0x102] = 0xCC;
    payload[0x103] = 0xDD;

    *payload_out = payload;
    *payload_len_out = payload_len;
}

static uint8_t *build_container(const uint8_t *payload, uint32_t payload_len, size_t *out_len) {
    struct test_fry_header h;
    h.magic = FRY_MAGIC;
    h.version = VERSION;
    h.flags = FLAG_ELF64;
    h.payload_size = payload_len;
    h.crc32 = crc32_calc(payload, payload_len);

    *out_len = sizeof(h) + payload_len;
    uint8_t *buf = (uint8_t *)xmalloc(*out_len);
    memcpy(buf, &h, sizeof(h));
    memcpy(buf + sizeof(h), payload, payload_len);
    return buf;
}

static int run_case(const char *name, void (*mutator)(uint8_t *, uint32_t), int expected_rc) {
    uint8_t *payload = 0;
    uint32_t payload_len = 0;
    build_base_payload(&payload, &payload_len);
    mutator(payload, payload_len);

    size_t container_len = 0;
    uint8_t *container = build_container(payload, payload_len, &container_len);

    char path[] = "/tmp/elf_bounds_case_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        fprintf(stderr, "mkstemp failed: %s\n", strerror(errno));
        free(container);
        free(payload);
        return 1;
    }
    close(fd);
    write_file_or_die(path, container, container_len);

    uint64_t cr3 = 0;
    uint64_t entry = 0;
    uint64_t rsp = 0;
    int rc = elf_load_fry(path, &cr3, &entry, &rsp);
    remove(path);
    free(container);
    free(payload);

    if (rc != expected_rc) {
        fprintf(stderr, "FAIL %s: expected %d got %d\n", name, expected_rc, rc);
        return 1;
    }
    printf("PASS %s\n", name);
    return 0;
}

static void mut_bad_phentsize(uint8_t *payload, uint32_t payload_len) {
    (void)payload_len;
    struct test_elf64_ehdr *eh = (struct test_elf64_ehdr *)payload;
    eh->e_phentsize = 0;
}

static void mut_e_phoff_oob(uint8_t *payload, uint32_t payload_len) {
    struct test_elf64_ehdr *eh = (struct test_elf64_ehdr *)payload;
    eh->e_phoff = (uint64_t)payload_len + 1;
}

static void mut_ph_table_oob(uint8_t *payload, uint32_t payload_len) {
    struct test_elf64_ehdr *eh = (struct test_elf64_ehdr *)payload;
    eh->e_phoff = (uint64_t)payload_len - 1;
    eh->e_phnum = 1;
}

static void mut_p_offset_oob(uint8_t *payload, uint32_t payload_len) {
    struct test_elf64_ehdr *eh = (struct test_elf64_ehdr *)payload;
    struct test_elf64_phdr *ph = (struct test_elf64_phdr *)(payload + eh->e_phoff);
    ph->p_offset = (uint64_t)payload_len + 1;
}

static void mut_p_filesz_oob(uint8_t *payload, uint32_t payload_len) {
    struct test_elf64_ehdr *eh = (struct test_elf64_ehdr *)payload;
    struct test_elf64_phdr *ph = (struct test_elf64_phdr *)(payload + eh->e_phoff);
    ph->p_offset = (uint64_t)payload_len - 1;
    ph->p_filesz = 2;
    ph->p_memsz = 2;
}

static void mut_p_filesz_gt_memsz(uint8_t *payload, uint32_t payload_len) {
    (void)payload_len;
    struct test_elf64_ehdr *eh = (struct test_elf64_ehdr *)payload;
    struct test_elf64_phdr *ph = (struct test_elf64_phdr *)(payload + eh->e_phoff);
    ph->p_filesz = 8;
    ph->p_memsz = 4;
}

static void mut_vaddr_memsz_overflow(uint8_t *payload, uint32_t payload_len) {
    (void)payload_len;
    struct test_elf64_ehdr *eh = (struct test_elf64_ehdr *)payload;
    struct test_elf64_phdr *ph = (struct test_elf64_phdr *)(payload + eh->e_phoff);
    ph->p_vaddr = 1;
    ph->p_memsz = UINT64_MAX;
    ph->p_filesz = 0;
}

static void mut_seg_end_align_overflow(uint8_t *payload, uint32_t payload_len) {
    (void)payload_len;
    struct test_elf64_ehdr *eh = (struct test_elf64_ehdr *)payload;
    struct test_elf64_phdr *ph = (struct test_elf64_phdr *)(payload + eh->e_phoff);
    ph->p_vaddr = UINT64_MAX - 0x800ULL;
    ph->p_memsz = 0x700ULL;
    ph->p_filesz = 0;
}

struct vfs_file *vfs_open(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return 0;
    }
    long sz = ftell(fp);
    if (sz < 0) {
        fclose(fp);
        return 0;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return 0;
    }
    struct vfs_file *vf = (struct vfs_file *)xmalloc(sizeof(*vf));
    vf->fp = fp;
    vf->size = (uint32_t)sz;
    return vf;
}

int vfs_read(struct vfs_file *f, void *buf, uint32_t len) {
    return (int)fread(buf, 1, len, f->fp);
}

void vfs_close(struct vfs_file *f) {
    if (!f) return;
    if (f->fp) fclose(f->fp);
    free(f);
}

uint32_t vfs_size(struct vfs_file *f) {
    return f->size;
}

uint64_t pmm_alloc_page(void) {
    return (uint64_t)(uintptr_t)calloc(1, 4096);
}

uint64_t pmm_alloc_pages(uint64_t count) {
    if (count == 0) count = 1;
    return (uint64_t)(uintptr_t)calloc((size_t)count, 4096);
}

void pmm_free_page(uint64_t phys) {
    (void)phys;
}

uint64_t vmm_phys_to_virt(uint64_t phys) {
    return phys;
}

uint64_t vmm_create_address_space(void) {
    return 1;
}

void vmm_destroy_address_space(uint64_t pml4_phys) {
    (void)pml4_phys;
}

void vmm_map_user(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags) {
    (void)pml4_phys;
    (void)virt;
    (void)phys;
    (void)flags;
}

int main(void) {
    int failed = 0;
    failed |= run_case("bad_phentsize", mut_bad_phentsize, ELF_LOAD_ERR_BAD_ELF_HEADER);
    failed |= run_case("e_phoff_oob", mut_e_phoff_oob, ELF_LOAD_ERR_BOUNDS);
    failed |= run_case("ph_table_oob", mut_ph_table_oob, ELF_LOAD_ERR_BOUNDS);
    failed |= run_case("p_offset_oob", mut_p_offset_oob, ELF_LOAD_ERR_BOUNDS);
    failed |= run_case("p_filesz_oob", mut_p_filesz_oob, ELF_LOAD_ERR_BOUNDS);
    failed |= run_case("p_filesz_gt_memsz", mut_p_filesz_gt_memsz, ELF_LOAD_ERR_BOUNDS);
    failed |= run_case("vaddr_memsz_overflow", mut_vaddr_memsz_overflow, ELF_LOAD_ERR_BOUNDS);
    failed |= run_case("seg_end_align_overflow", mut_seg_end_align_overflow, ELF_LOAD_ERR_BOUNDS);
    return failed ? 1 : 0;
}
