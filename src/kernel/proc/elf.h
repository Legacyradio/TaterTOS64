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
    ELF_LOAD_ERR_STACK_ALLOC = -113
};

int elf_load_fry(const char *path, uint64_t *cr3_out, uint64_t *entry_out, uint64_t *user_rsp_out);

#endif
