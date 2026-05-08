/*
 * TaterTOS64v3 — <link.h>
 *
 * Minimal stub for partition_alloc's stack trace support.
 * TaterTOS does not have a dynamic linker, so the ELF types
 * come from <elf.h> instead.
 */

#ifndef _TATERTOS_LINK_H
#define _TATERTOS_LINK_H

#include <elf.h>

/* ElfW macro — resolves to Elf64_* on x86_64 */
#define ElfW(type) Elf64_ ## type

#endif /* _TATERTOS_LINK_H */
