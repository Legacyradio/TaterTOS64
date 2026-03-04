; UEFI boot entry stub: set up kernel paging and jump to _fry_start

BITS 64

section .boot
align 16

global efi_boot_start
extern _fry_start
extern __kernel_stack_top

; efi_loader.c is compiled with x86_64-w64-mingw32-gcc (Windows/ms_abi).
; Windows ABI passes the first argument in RCX, not RDI.
; _fry_start uses System V ABI (x86_64-elf-gcc) and expects arg1 in RDI.
; Translate here before jumping into the kernel.
efi_boot_start:
    cli

    ; Load our page tables (identity map low 4GB + high-half map)
    mov rax, pml4_table
    mov cr3, rax

    ; Switch to kernel stack (high half)
    mov rax, __kernel_stack_top
    mov rsp, rax
    and rsp, -16

    ; Translate Windows ABI arg1 (RCX = handoff) to SysV ABI arg1 (RDI).
    ; RAX is scratch here; RCX was set by the MinGW caller for us.
    mov rdi, rcx

    mov rax, _fry_start
    call rax

.hang:
    hlt
    jmp .hang

align 4096
pml4_table:
    dq pdpt_low + 0x003
    times 510 dq 0
    dq pdpt_high + 0x003

align 4096
pdpt_low:
    dq pd_low0 + 0x003
    dq pd_low1 + 0x003
    dq pd_low2 + 0x003
    dq pd_low3 + 0x003
    times 508 dq 0

align 4096
pdpt_high:
    times 510 dq 0
    dq pd_high + 0x003
    dq 0

align 4096
pd_low0:
%assign i 0
%rep 512
    dq (i * 0x200000) | 0x083
%assign i i+1
%endrep

align 4096
pd_low1:
%assign j 0
%rep 512
    dq ((j * 0x200000) + 0x40000000) | 0x083
%assign j j+1
%endrep

align 4096
pd_low2:
%assign k 0
%rep 512
    dq ((k * 0x200000) + 0x80000000) | 0x083
%assign k k+1
%endrep

align 4096
pd_low3:
%assign l 0
%rep 512
    dq ((l * 0x200000) + 0xC0000000) | 0x083
%assign l l+1
%endrep

align 4096
pd_high:
; Start at KERNEL_LMA (0x2200000) so virt ffffffff80000000 -> phys 0x2200000,
; matching the physical load address from the linker script (KERNEL_LMA).
; KERNEL_LMA MUST be 2MiB-aligned for this mapping to be correct.
; Each 2MiB page: phys = m * 0x200000 + 0x2200000.
%assign m 0
%rep 512
    dq (m * 0x200000 + 0x2200000) | 0x083
%assign m m+1
%endrep
