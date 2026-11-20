#ifndef TATER_ELF_H
#define TATER_ELF_H

#include <stdint.h>

enum elf_load_error {
    ELF_LOAD_OK = 0,
    ELF_LOAD_ERR_BAD_ARGS = -100,
    ELF_LOAD_ERR_OPEN = -101,
    ELF_LOAD_ERR_SHORT_HEADER = -102,
    ELF_LOAD_ERR_NOMEM = -103,
    ELF_LOAD_ERR_READ = -104,
    ELF_LOAD_ERR_BAD_MAGIC = -105,
    ELF_LOAD_ERR_BOUNDS = -106,
    ELF_LOAD_ERR_BAD_CRC = -107,
    ELF_LOAD_ERR_BAD_ELF_HEADER = -108,
    ELF_LOAD_ERR_BAD_ELF_MAGIC = -109,
    ELF_LOAD_ERR_VMM_SPACE = -110,
    ELF_LOAD_ERR_SEG_ALLOC = -111,
    ELF_LOAD_ERR_SEG_TRANSLATE = -112,
    ELF_LOAD_ERR_STACK_ALLOC = -113,
    ELF_LOAD_ERR_TLS_ALLOC = -114
};

/* fry1790: PT_TLS template, captured at load so spawned threads can build their
 * OWN variant-II TLS block (the loader only sets up the MAIN thread). vaddr is
 * the PT_TLS p_vaddr — the pristine .tdata init image, mapped read-only in the
 * process image (the main thread's live __thread vars live in a separate TLS
 * region, so this stays pristine). tls_alloc = align_up(memsz, align) = the tp
 * offset from a block's base. */
struct fry_tls_template {
    int      has_tls;
    uint64_t vaddr;       /* pristine .tdata source VA (in the process image) */
    uint64_t filesz;      /* .tdata init image bytes */
    uint64_t memsz;       /* total TLS bytes (.tdata + .tbss) */
    uint64_t align;
    uint64_t tls_alloc;   /* align_up(memsz, align) */
};

/* tls_base_out receives the thread-pointer (%fs base) for the main thread when
 * the image carries a PT_TLS segment, or 0 when it does not.  Callers store it
 * in fry_process.user_fs_base so process_start() programs MSR_FS_BASE before
 * entry.  Pass NULL only from callers that never load PT_TLS images.
 * tmpl_out (fry1790) receives the PT_TLS template for per-thread TLS; NULL ok. */
int elf_load_fry(const char *path, uint64_t *cr3_out, uint64_t *entry_out, uint64_t *user_rsp_out,
                 uint64_t *tls_base_out, struct fry_tls_template *tmpl_out);

#endif
