/*
 * lmmap.S - unmodified static Linux x86_64 ELF for file-backed mmap testing.
 *
 * Exercises:
 *   - openat(2)
 *   - mmap(2) with fd and syscall arg6 offset
 *   - munmap(2)
 *   - close(2)
 *   - write(2)
 *   - exit_group(2)
 */

.intel_syntax noprefix
.global _start
.text

_start:
    lea   rsi, [rip+banner]
    mov   rdx, OFFSET banner_len
    call  write1

    /* openat(AT_FDCWD, "/LXTEST.TXT", O_RDONLY, 0) */
    mov   rax, 257
    mov   rdi, -100
    lea   rsi, [rip+path]
    xor   rdx, rdx
    xor   r10, r10
    syscall
    test  rax, rax
    js    fail_open
    mov   r12, rax

    /* mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 0) */
    mov   rax, 9
    xor   rdi, rdi
    mov   rsi, 4096
    mov   rdx, 1
    mov   r10, 2
    mov   r8, r12
    xor   r9, r9
    syscall
    cmp   rax, -4095
    jae   fail_mmap
    mov   r13, rax

    cmp   byte ptr [r13+0], 'T'
    jne   fail_bytes
    cmp   byte ptr [r13+1], 'a'
    jne   fail_bytes
    cmp   byte ptr [r13+2], 't'
    jne   fail_bytes
    cmp   byte ptr [r13+9], 'l'
    jne   fail_bytes
    cmp   byte ptr [r13+10], 'i'
    jne   fail_bytes
    cmp   byte ptr [r13+11], 'n'
    jne   fail_bytes
    cmp   byte ptr [r13+12], 'u'
    jne   fail_bytes

    /* munmap(mapped, 4096) */
    mov   rax, 11
    mov   rdi, r13
    mov   rsi, 4096
    syscall
    test  rax, rax
    js    fail_munmap

    /* close(fd) */
    mov   rax, 3
    mov   rdi, r12
    syscall
    test  rax, rax
    js    fail_close

    lea   rsi, [rip+okline]
    mov   rdx, OFFSET okline_len
    call  write1
    xor   rdi, rdi
    jmp   exit_group

fail_open:
    lea   rsi, [rip+msg_open]
    mov   rdx, OFFSET msg_open_len
    jmp   fail
fail_mmap:
    lea   rsi, [rip+msg_mmap]
    mov   rdx, OFFSET msg_mmap_len
    jmp   fail_close_fd
fail_bytes:
    lea   rsi, [rip+msg_bytes]
    mov   rdx, OFFSET msg_bytes_len
    jmp   fail_close_fd
fail_munmap:
    lea   rsi, [rip+msg_munmap]
    mov   rdx, OFFSET msg_munmap_len
    jmp   fail_close_fd
fail_close:
    lea   rsi, [rip+msg_close]
    mov   rdx, OFFSET msg_close_len
    jmp   fail

fail_close_fd:
    call  write1
    mov   rax, 3
    mov   rdi, r12
    syscall
    mov   rdi, 1
    jmp   exit_group

fail:
    call  write1
    mov   rdi, 1

exit_group:
    mov   rax, 231
    syscall
    hlt

write1:
    mov   rax, 1
    mov   rdi, 1
    syscall
    ret

.section .rodata
banner:     .ascii "==== Tater Bridge: file-backed mmap probe ====\n"
            banner_len = . - banner
okline:     .ascii "[OK] mmap(fd, offset=0) exposed LXTEST.TXT bytes\n"
            okline_len = . - okline
msg_open:   .ascii "[FAIL] openat before mmap\n"
            msg_open_len = . - msg_open
msg_mmap:   .ascii "[FAIL] file-backed mmap\n"
            msg_mmap_len = . - msg_mmap
msg_bytes:  .ascii "[FAIL] mapped bytes mismatch\n"
            msg_bytes_len = . - msg_bytes
msg_munmap: .ascii "[FAIL] munmap(mapped)\n"
            msg_munmap_len = . - msg_munmap
msg_close:  .ascii "[FAIL] close(fd)\n"
            msg_close_len = . - msg_close
path:       .asciz "/LXTEST.TXT"
