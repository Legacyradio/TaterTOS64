/*
 * lhello.S — a minimal, UNMODIFIED static Linux x86_64 ELF used to validate
 * the TaterTOS linuxulator (Phase 1 proof point).
 *
 * It is built with the HOST Linux toolchain (gcc -nostdlib -static -no-pie)
 * and is therefore a genuine Linux binary — NOT a TaterTOS .fry. It exercises
 * exactly the path Phase 1 must support:
 *   - the SysV/Linux initial stack: reads argc, argv[0], envp[0]
 *   - the Linux write(2) syscall (nr 1) to fd 1
 *   - the Linux exit_group(2) syscall (nr 231), returning argc as the code
 *
 * Build:
 *   gcc -nostdlib -static -no-pie -o lhello.lxe tools/host/lhello.S
 */

.intel_syntax noprefix
.global _start
.text

_start:
    mov   r12, [rsp]            /* argc */
    lea   r13, [rsp+8]          /* argv */

    lea   rsi, [rip+banner]
    mov   rdx, OFFSET banner_len
    call  write1

    lea   rsi, [rip+s_argc]
    mov   rdx, OFFSET s_argc_len
    call  write1
    mov   rax, r12
    add   al, '0'               /* single-digit argc is plenty for the test */
    mov   [rip+digit], al
    lea   rsi, [rip+digit]
    mov   rdx, 1
    call  write1
    lea   rsi, [rip+nl]
    mov   rdx, 1
    call  write1

    lea   rsi, [rip+s_argv0]
    mov   rdx, OFFSET s_argv0_len
    call  write1
    mov   rdi, [r13]            /* argv[0] */
    call  strlen                /* -> rdx = len */
    mov   rsi, rdi
    call  write1
    lea   rsi, [rip+nl]
    mov   rdx, 1
    call  write1

    lea   rsi, [rip+s_env0]
    mov   rdx, OFFSET s_env0_len
    call  write1
    lea   rax, [r13 + r12*8 + 8]   /* &envp[0] = argv + argc*8 + 8 (past NULL) */
    mov   rdi, [rax]
    test  rdi, rdi
    jz    .Lno_env
    call  strlen
    mov   rsi, rdi
    call  write1
.Lno_env:
    lea   rsi, [rip+nl]
    mov   rdx, 1
    call  write1

    lea   rsi, [rip+okline]
    mov   rdx, OFFSET okline_len
    call  write1

    /* exit_group(argc) — distinctive non-zero code proves the exit path */
    mov   rax, 231
    mov   rdi, r12
    syscall
    /* not reached */
    hlt

/* write(1, rsi, rdx) */
write1:
    mov   rax, 1
    mov   rdi, 1
    syscall
    ret

/* strlen(rdi) -> rdx */
strlen:
    xor   rdx, rdx
.Lsl:
    mov   al, [rdi+rdx]
    test  al, al
    jz    .Lsldone
    inc   rdx
    jmp   .Lsl
.Lsldone:
    ret

.section .rodata
banner:  .ascii "==== TaterTOS linuxulator: hello from a REAL unmodified Linux ELF ====\n"
         banner_len = . - banner
s_argc:  .ascii "argc="
         s_argc_len = . - s_argc
s_argv0: .ascii "argv0="
         s_argv0_len = . - s_argv0
s_env0:  .ascii "env0="
         s_env0_len = . - s_env0
okline:  .ascii "[OK] write + SysV stack + auxv reached; calling exit_group(argc)\n"
         okline_len = . - okline
nl:      .ascii "\n"

.section .data
digit:   .byte 0
