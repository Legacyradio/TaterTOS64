; UEFI stack handoff stub (x86_64, SysV ABI)
; Switch to the kernel stack before entering _fry_start.

global efi_start
extern __kernel_stack_top
extern _fry_start

section .text
efi_start:
    mov rax, __kernel_stack_top
    mov rsp, rax
    and rsp, -16
    call _fry_start
.hang:
    hlt
    jmp .hang
