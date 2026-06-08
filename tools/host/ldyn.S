/*
 * ldyn.S - unmodified dynamically linked Linux x86_64 ELF probe.
 *
 * Built by build_iso.sh with a real PT_INTERP:
 *   /lib64/ld-linux-x86-64.so.2
 *
 * The program itself uses only raw Linux syscalls, so this isolates the
 * Tater Bridge dynamic-loader handoff from libc application behavior.
 */

.intel_syntax noprefix
.global _start
.text

_start:
    lea   rsi, [rip+banner]
    mov   rdx, OFFSET banner_len
    call  write1

    lea   rsi, [rip+okline]
    mov   rdx, OFFSET okline_len
    call  write1

    xor   rdi, rdi
    mov   rax, 231
    syscall
    hlt

write1:
    mov   rax, 1
    mov   rdi, 1
    syscall
    ret

.section .rodata
banner: .ascii "==== Tater Bridge: dynamic PT_INTERP probe ====\n"
        banner_len = . - banner
okline: .ascii "[OK] PT_INTERP loader reached user entry\n"
        okline_len = . - okline
