/*
 * TaterTOS64v3 — <sys/ucontext.h>
 *
 * POSIX user context.
 */

#ifndef _TATERTOS_SYS_UCONTEXT_H
#define _TATERTOS_SYS_UCONTEXT_H

#include <bits/types/sigset_t.h>
#include <bits/types/stack_t.h>
#include <tatertos/ucontext.h>

/* x86_64 mcontext */
#define NGREG 23
typedef uint64_t greg_t;
typedef struct {
    greg_t gregs[NGREG];
} mcontext_t;

/* Linux-compatible register indices for Chromium/POSIX compatibility.
   These map to the TaterTOS canonical tatertos_gregs_t layout. */
#define REG_R8          0
#define REG_R9          1
#define REG_R10         2
#define REG_R11         3
#define REG_R12         4
#define REG_R13         5
#define REG_R14         6
#define REG_R15         7
#define REG_RDI         8
#define REG_RSI         9
#define REG_RBP         10
#define REG_RBX         11
#define REG_RDX         12
#define REG_RAX         13
#define REG_RCX         14
#define REG_RSP         15
#define REG_RIP         16
#define REG_EFL         17
#define REG_CSGSFS      18
#define REG_ERR         19
#define REG_TRAPNO      20
#define REG_OLDMASK     21
#define REG_CR2         22

typedef struct ucontext {
    unsigned long uc_flags;
    struct ucontext *uc_link;
    stack_t         uc_stack;
    mcontext_t      uc_mcontext;
    sigset_t        uc_sigmask;
} ucontext_t;

#endif
