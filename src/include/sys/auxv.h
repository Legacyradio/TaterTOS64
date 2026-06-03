/*
 * TaterTOS64v3 — <sys/auxv.h>
 *
 * Linux auxiliary vector stub.
 * TaterTOS has no ELF auxv; provide enough for abseil to compile.
 */

#ifndef _TATERTOS_SYS_AUXV_H
#define _TATERTOS_SYS_AUXV_H

#include <linux/auxvec.h>

unsigned long getauxval(unsigned long type);

#endif
