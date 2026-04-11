/*
 * setjmp.S — TaterTOS64v3 setjmp/longjmp for x86_64
 *
 * jmp_buf layout (8 x uint64_t):
 *   [0] RBX   [1] RBP   [2] R12   [3] R13
 *   [4] R14   [5] R15   [6] RSP   [7] RIP (return address)
 *
 * System V AMD64 ABI: RBX, RBP, R12-R15 are callee-saved.
 * RSP and the return address complete the context.
 */

.text
.global setjmp
.type setjmp, @function
setjmp:
    /* rdi = jmp_buf pointer */
    movq    %rbx,    (%rdi)       /* [0] = RBX */
    movq    %rbp,   8(%rdi)       /* [1] = RBP */
    movq    %r12,  16(%rdi)       /* [2] = R12 */
    movq    %r13,  24(%rdi)       /* [3] = R13 */
    movq    %r14,  32(%rdi)       /* [4] = R14 */
    movq    %r15,  40(%rdi)       /* [5] = R15 */
    leaq    8(%rsp), %rax         /* RSP after setjmp returns (pop ret addr) */
    movq    %rax,  48(%rdi)       /* [6] = RSP */
    movq    (%rsp), %rax          /* return address on stack */
    movq    %rax,  56(%rdi)       /* [7] = RIP */
    xorl    %eax,  %eax          /* setjmp returns 0 */
    ret

.global longjmp
.type longjmp, @function
longjmp:
    /* rdi = jmp_buf pointer, esi = return value */
    movl    %esi,  %eax          /* return value */
    testl   %eax,  %eax
    jnz     1f
    incl    %eax                 /* if val==0, return 1 (POSIX requirement) */
1:
    movq     (%rdi), %rbx        /* restore RBX */
    movq    8(%rdi), %rbp        /* restore RBP */
    movq   16(%rdi), %r12        /* restore R12 */
    movq   24(%rdi), %r13        /* restore R13 */
    movq   32(%rdi), %r14        /* restore R14 */
    movq   40(%rdi), %r15        /* restore R15 */
    movq   48(%rdi), %rsp        /* restore RSP */
    jmpq  *56(%rdi)              /* jump to saved RIP */
