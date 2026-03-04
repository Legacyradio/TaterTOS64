; SMP trampoline
BITS 16

global smp_trampoline
global smp_trampoline_data
global smp_trampoline_end

%define DATA_OFF   (smp_trampoline_data - smp_trampoline)
%define DATA_CR3   0
%define DATA_STACK 8
%define DATA_ENTRY 16
%define DATA_CPU   24
%define DATA_APIC  28
%define DATA_READY 32
%define GDT_PTR16_OFF (gdt_ptr16 - smp_trampoline)
%define GDT_START_OFF (gdt_start - smp_trampoline)
%define TRAMP_STACK_TOP_OFF (tramp_stack_top - smp_trampoline)
%define TRAMP_BASE_OFF (tramp_base - smp_trampoline)
%define PM_ENTRY_OFF (pm_entry - smp_trampoline)
%define LM_ENTRY_OFF (lm_entry - smp_trampoline)
%define PM_FARPTR_OFF (pm_farptr - smp_trampoline)
%define LM_FARPTR_OFF (lm_farptr - smp_trampoline)

section .text

smp_trampoline:
    cli
    cld
    mov ax, cs
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, TRAMP_STACK_TOP_OFF

    ; Enable A20 (fast)
    in al, 0x92
    or al, 0x02
    out 0x92, al

    ; Save trampoline base (linear)
    mov bx, cs
    movzx ebx, bx
    shl ebx, 4
    mov [TRAMP_BASE_OFF], ebx

    ; Patch GDT base in pointer
    mov di, GDT_PTR16_OFF
    mov eax, ebx
    add eax, GDT_START_OFF
    mov [di + 2], eax

    lgdt [di]

    ; Build far pointer for protected-mode entry (linear = base + offset)
    mov di, PM_FARPTR_OFF
    mov eax, ebx
    add eax, PM_ENTRY_OFF
    mov [di], eax
    mov word [di + 4], 0x08

    mov eax, cr0
    or eax, 0x1
    mov cr0, eax
    jmp dword far [di]

BITS 32
pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax

    mov eax, cr4
    or eax, (1 << 5) | (1 << 9) | (1 << 10)   ; PAE, OSFXSR, OSXMMEXCPT (enables SSE2 for AP)
    mov cr4, eax

    mov eax, [ebx + DATA_OFF + DATA_CR3]
    mov cr3, eax

    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax

    ; Build far pointer for long-mode entry (linear = base + offset)
    mov eax, ebx
    add eax, LM_ENTRY_OFF
    mov [LM_FARPTR_OFF], eax
    mov word [LM_FARPTR_OFF + 4], 0x18
    jmp dword far [LM_FARPTR_OFF]

BITS 64
lm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax

    mov rax, [rbx + DATA_OFF + DATA_STACK]
    mov rsp, rax
    mov rax, [rbx + DATA_OFF + DATA_ENTRY]
    mov edi, dword [rbx + DATA_OFF + DATA_CPU]
    mov esi, dword [rbx + DATA_OFF + DATA_APIC]
    mov rdx, [rbx + DATA_OFF + DATA_READY]
    call rax

.halt:
    hlt
    jmp .halt

align 8
tramp_base: dd 0

align 8
gdt_ptr16:
    dw gdt_end - gdt_start - 1
    dd 0

align 8
gdt_start:
    dq 0x0000000000000000
    dq 0x00CF9A000000FFFF   ; 0x08: 32-bit code
    dq 0x00CF92000000FFFF   ; 0x10: 32-bit data
    dq 0x00AF9A000000FFFF   ; 0x18: 64-bit code
gdt_end:

align 16
tramp_stack:
    times 256 db 0
tramp_stack_top:

align 8
pm_farptr: dd 0
    dw 0

align 8
lm_farptr: dd 0
    dw 0

align 8
smp_trampoline_data:
    dq 0        ; cr3
    dq 0        ; stack_top
    dq 0        ; entry
    dd 0        ; cpu_index
    dd 0        ; apic_id
    dq 0        ; ready flag pointer

smp_trampoline_end:
