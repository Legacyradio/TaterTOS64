/*
 * lfile.S - unmodified static Linux x86_64 ELF for the TaterTOS linuxulator.
 *
 * Exercises the next compatibility rung after lhello.S:
 *   - access(2)
 *   - openat(2)
 *   - fstat(2)
 *   - read(2)
 *   - lseek(2)
 *   - pread64(2)
 *   - newfstatat(2)
 *   - close(2)
 *   - write(2) for reporting
 *   - exit_group(2)
 */

.intel_syntax noprefix
.global _start
.text

_start:
    lea   rsi, [rip+banner]
    mov   rdx, OFFSET banner_len
    call  write1

    /* access("/LXTEST.TXT", R_OK) */
    mov   rax, 21
    lea   rdi, [rip+path]
    mov   rsi, 4
    syscall
    test  rax, rax
    js    fail_access

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

    /* fstat(fd, &statbuf), check st_size >= 16 */
    mov   rax, 5
    mov   rdi, r12
    lea   rsi, [rip+statbuf]
    syscall
    test  rax, rax
    js    fail_fstat
    mov   rax, [rip+statbuf+48]
    cmp   rax, 16
    jb    fail_fstat

    /* read(fd, readbuf, 64), verify "Tater" prefix */
    mov   rax, 0
    mov   rdi, r12
    lea   rsi, [rip+readbuf]
    mov   rdx, 64
    syscall
    cmp   rax, 8
    jb    fail_read
    cmp   byte ptr [rip+readbuf+0], 'T'
    jne   fail_read
    cmp   byte ptr [rip+readbuf+1], 'a'
    jne   fail_read
    cmp   byte ptr [rip+readbuf+2], 't'
    jne   fail_read
    cmp   byte ptr [rip+readbuf+3], 'e'
    jne   fail_read
    cmp   byte ptr [rip+readbuf+4], 'r'
    jne   fail_read

    /* lseek(fd, 0, SEEK_SET) */
    mov   rax, 8
    mov   rdi, r12
    xor   rsi, rsi
    xor   rdx, rdx
    syscall
    test  rax, rax
    jne   fail_lseek

    /* pread64(fd, readbuf+80, 4, 9), verify "linu" */
    mov   rax, 17
    mov   rdi, r12
    lea   rsi, [rip+readbuf+80]
    mov   rdx, 4
    mov   r10, 9
    syscall
    cmp   rax, 4
    jne   fail_pread
    cmp   byte ptr [rip+readbuf+80], 'l'
    jne   fail_pread
    cmp   byte ptr [rip+readbuf+81], 'i'
    jne   fail_pread
    cmp   byte ptr [rip+readbuf+82], 'n'
    jne   fail_pread
    cmp   byte ptr [rip+readbuf+83], 'u'
    jne   fail_pread

    /* newfstatat(AT_FDCWD, "/LXTEST.TXT", &statbuf2, 0) */
    mov   rax, 262
    mov   rdi, -100
    lea   rsi, [rip+path]
    lea   rdx, [rip+statbuf2]
    xor   r10, r10
    syscall
    test  rax, rax
    js    fail_newfstatat

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

fail_access:
    lea   rsi, [rip+msg_access]
    mov   rdx, OFFSET msg_access_len
    jmp   fail
fail_open:
    lea   rsi, [rip+msg_open]
    mov   rdx, OFFSET msg_open_len
    jmp   fail
fail_fstat:
    lea   rsi, [rip+msg_fstat]
    mov   rdx, OFFSET msg_fstat_len
    jmp   fail
fail_read:
    lea   rsi, [rip+msg_read]
    mov   rdx, OFFSET msg_read_len
    jmp   fail
fail_lseek:
    lea   rsi, [rip+msg_lseek]
    mov   rdx, OFFSET msg_lseek_len
    jmp   fail
fail_pread:
    lea   rsi, [rip+msg_pread]
    mov   rdx, OFFSET msg_pread_len
    jmp   fail
fail_newfstatat:
    lea   rsi, [rip+msg_newfstatat]
    mov   rdx, OFFSET msg_newfstatat_len
    jmp   fail
fail_close:
    lea   rsi, [rip+msg_close]
    mov   rdx, OFFSET msg_close_len
    jmp   fail

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
banner:          .ascii "==== TaterTOS linuxulator: VFS syscall probe ====\n"
                 banner_len = . - banner
okline:          .ascii "[OK] access/openat/fstat/read/lseek/pread64/newfstatat/close passed\n"
                 okline_len = . - okline
msg_access:      .ascii "[FAIL] access(/LXTEST.TXT)\n"
                 msg_access_len = . - msg_access
msg_open:        .ascii "[FAIL] openat(/LXTEST.TXT)\n"
                 msg_open_len = . - msg_open
msg_fstat:       .ascii "[FAIL] fstat(fd)\n"
                 msg_fstat_len = . - msg_fstat
msg_read:        .ascii "[FAIL] read(fd)\n"
                 msg_read_len = . - msg_read
msg_lseek:       .ascii "[FAIL] lseek(fd)\n"
                 msg_lseek_len = . - msg_lseek
msg_pread:       .ascii "[FAIL] pread64(fd)\n"
                 msg_pread_len = . - msg_pread
msg_newfstatat:  .ascii "[FAIL] newfstatat(/LXTEST.TXT)\n"
                 msg_newfstatat_len = . - msg_newfstatat
msg_close:       .ascii "[FAIL] close(fd)\n"
                 msg_close_len = . - msg_close
path:            .asciz "/LXTEST.TXT"

.bss
.align 16
statbuf:  .skip 144
statbuf2: .skip 144
readbuf:  .skip 128
