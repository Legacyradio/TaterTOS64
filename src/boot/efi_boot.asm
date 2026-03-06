; UEFI boot entry stub: set up kernel paging and jump to _fry_start

BITS 64

section .boot
align 16

global efi_boot_start
extern _fry_start
extern __kernel_stack_top

%define HO_FB_BASE      0
%define HO_FB_WIDTH     8
%define HO_FB_HEIGHT   16
%define HO_FB_STRIDE   24
%define HO_ID_LIMIT    72

; Draw a 12x12 stage block in the top row of the framebuffer.
; IN: RCX = struct fry_handoff*, EDX = stage index (x = stage * 20 pixels)
boot_fb_mark:
    push rbx
    push r12
    push r13
    push r14
    push r15

    mov rbx, rcx
    test rbx, rbx
    jz .done

    mov rax, [rbx + HO_FB_BASE]
    mov r8,  [rbx + HO_FB_WIDTH]
    mov r9,  [rbx + HO_FB_HEIGHT]
    mov r10, [rbx + HO_FB_STRIDE]
    test rax, rax
    jz .done
    test r8, r8
    jz .done
    test r9, r9
    jz .done
    test r10, r10
    jz .done

    ; Only touch framebuffer if it is inside the identity-mapped boot range.
    mov r11, [rbx + HO_ID_LIMIT]
    test r11, r11
    jz .done
    cmp rax, r11
    jae .done

    mov r12d, edx
    imul r12, r12, 20
    cmp r12, r8
    jae .done

    mov r13, 12
    mov r14, r8
    sub r14, r12
    cmp r14, r13
    cmovb r13, r14

    mov r15, 12
    cmp r9, r15
    cmovb r15, r9

    xor ecx, ecx
.yloop:
    cmp rcx, r15
    jae .done

    mov rdx, rcx
    imul rdx, r10
    add rdx, r12
    lea rdx, [rax + rdx*4]

    xor r14d, r14d
.xloop:
    cmp r14, r13
    jae .ynext
    mov dword [rdx + r14*4], 0x00F0F0F0
    inc r14
    jmp .xloop

.ynext:
    inc rcx
    jmp .yloop

.done:
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

; efi_loader.c is compiled with x86_64-w64-mingw32-gcc (Windows/ms_abi).
; Windows ABI passes the first argument in RCX, not RDI.
; _fry_start uses System V ABI (x86_64-elf-gcc) and expects arg1 in RDI.
; Translate here before jumping into the kernel.
efi_boot_start:
    cli
    mov rbx, rcx

    ; Stage 1: entered efi_boot_start.
    mov rcx, rbx
    mov edx, 1
    call boot_fb_mark

    ; Load our page tables (identity map low 4GB + high-half map)
    mov rax, pml4_table
    mov cr3, rax

    ; Stage 2: CR3 switched to kernel bootstrap tables.
    mov rcx, rbx
    mov edx, 2
    call boot_fb_mark

    ; Switch to kernel stack (high half)
    mov rax, __kernel_stack_top
    mov rsp, rax
    and rsp, -16

    ; Stage 3: stack switched, about to enter _fry_start.
    mov rcx, rbx
    mov edx, 3
    call boot_fb_mark

    ; Translate Windows ABI arg1 (RCX = handoff) to SysV ABI arg1 (RDI).
    ; RAX is scratch here; RCX was set by the MinGW caller for us.
    mov rdi, rbx

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
