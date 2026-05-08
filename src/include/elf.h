/*
 * TaterTOS64v3 — <elf.h>
 *
 * Minimal stub with ELF constants needed by partition_alloc's
 * stack trace support. TaterTOS binaries are statically linked
 * so the actual ELF parsing will always fail gracefully.
 */

#ifndef _TATERTOS_ELF_H
#define _TATERTOS_ELF_H

/* ELF header magic */
#define EI_MAG0        0
#define ELFMAG0        0x7f
#define ELFMAG         "\177ELF"
#define SELFMAG        4
#define EI_CLASS       4
#define ELFCLASS64     2
#define EI_DATA        5
#define ELFDATA2LSB    1
#define EI_OSABI       7

/* ELF file types */
#define ET_NONE        0
#define ET_EXEC        2
#define ET_DYN         3

/* ELF program header types */
#define PT_NULL        0
#define PT_LOAD        1
#define PT_DYNAMIC     2
#define PT_INTERP      3
#define PT_GNU_EH_FRAME 0x6474e550
#define PT_GNU_STACK   0x6474e551
#define PT_GNU_RELRO   0x6474e552

/* Phdr flags */
#define PF_X           1
#define PF_W           2
#define PF_R           4

/* ELF section header types */
#define SHT_NULL       0
#define SHT_PROGBITS   1
#define SHT_SYMTAB     2
#define SHT_STRTAB     3
#define SHT_NOBITS     8

/* ELF identifiers for per-architecture OS ranges */
#define ELFOSABI_NONE     0
#define ELFOSABI_LINUX    3

/* Machine types */
#define EM_X86_64     62

/* ELF class for 64-bit */
typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef int32_t  Elf64_Sword;
typedef uint64_t Elf64_Xword;
typedef int64_t  Elf64_Sxword;

typedef struct {
    unsigned char e_ident[16];
    Elf64_Half    e_type;
    Elf64_Half    e_machine;
    Elf64_Word    e_version;
    Elf64_Addr    e_entry;
    Elf64_Off     e_phoff;
    Elf64_Off     e_shoff;
    Elf64_Word    e_flags;
    Elf64_Half    e_ehsize;
    Elf64_Half    e_phentsize;
    Elf64_Half    e_phnum;
    Elf64_Half    e_shentsize;
    Elf64_Half    e_shnum;
    Elf64_Half    e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    Elf64_Word   p_type;
    Elf64_Word   p_flags;
    Elf64_Off    p_offset;
    Elf64_Addr   p_vaddr;
    Elf64_Addr   p_paddr;
    Elf64_Xword  p_filesz;
    Elf64_Xword  p_memsz;
    Elf64_Xword  p_align;
} Elf64_Phdr;

typedef struct {
    Elf64_Word   sh_name;
    Elf64_Word   sh_type;
    Elf64_Xword  sh_flags;
    Elf64_Addr   sh_addr;
    Elf64_Off    sh_offset;
    Elf64_Xword  sh_size;
    Elf64_Word   sh_link;
    Elf64_Word   sh_info;
    Elf64_Xword  sh_addralign;
    Elf64_Xword  sh_entsize;
} Elf64_Shdr;

/* ElfW macro to dispatch to 64-bit types */
#define ElfW(type) Elf64_ ## type

#endif /* _TATERTOS_ELF_H */
