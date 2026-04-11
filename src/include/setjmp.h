/*
 * setjmp.h — TaterTOS64v3 setjmp/longjmp for x86_64
 *
 * jmp_buf stores callee-saved registers: RBX, RBP, R12-R15, RSP,
 * and the return address (from the stack at the point of setjmp).
 */

#ifndef _TATERTOS_SETJMP_H
#define _TATERTOS_SETJMP_H

typedef unsigned long jmp_buf[8];

int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

/* POSIX aliases (no signal mask save/restore on TaterTOS) */
#define sigsetjmp(env, savesigs)   setjmp(env)
#define siglongjmp(env, val)       longjmp(env, val)
typedef jmp_buf sigjmp_buf;

#endif /* _TATERTOS_SETJMP_H */
