/*
 * TaterTOS64v3 — <tatertos/ucontext.h>
 *
 * TaterTOS canonical register structure for x86_64.
 * This defines the OS-native names for CPU registers.
 */

#ifndef _TATERTOS_ARCH_UCONTEXT_H
#define _TATERTOS_ARCH_UCONTEXT_H

#include <stdint.h>

typedef struct {
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t rdx;
    uint64_t rax;
    uint64_t rcx;
    uint64_t rsp;
    uint64_t rip;
    uint64_t rflags;
    uint64_t csgsfs;
    uint64_t err;
    uint64_t trapno;
    uint64_t oldmask;
    uint64_t cr2;
} tatertos_gregs_t;

#endif
