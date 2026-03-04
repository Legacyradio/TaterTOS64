; Multiboot2 32-bit entry stub: switch to long mode and jump to _fry_start

BITS 32

section .boot
align 16
global mb_entry
extern _mb_start
extern __kernel_stack_top

mb_entry:
    cli
    ; GRUB provides multiboot2 info pointer in EBX
    mov [mb_info_ptr], ebx
    mov [mb_magic], eax

    ; Initialize serial (COM1) for early diagnostics
    call serial_init
    mov al, 'A'
    call serial_putc
    call debug_putc

    ; Install minimal IDT to catch early faults
    call idt_init

    ; Mask PIC to prevent IRQs before our handlers are live
    mov al, 0xFF
    out 0x21, al
    out 0xA1, al

    ; Load temporary GDT
    lgdt [gdt_ptr]

    ; Enable PAE
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    ; Load PML4 base
    mov eax, pml4_table
    mov cr3, eax

    ; Enable long mode (EFER.LME)
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    ; Enable paging (CR0.PG)
    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax

    ; Far jump to 64-bit mode
    jmp 0x08:long_mode_entry

BITS 64
long_mode_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; Switch to kernel stack in high half before entering C
    mov rax, __kernel_stack_top
    mov rsp, rax

    mov al, 'L'
    call serial_putc
    call debug_putc

    ; Pass multiboot info pointer + magic to _mb_start
    mov rdi, [mb_info_ptr]
    mov rsi, [mb_magic]
    mov rax, _mb_start
    call rax

.halt:
    hlt
    jmp .halt

; -------- Serial (COM1) --------
serial_init:
    mov dx, 0x3F8 + 1
    mov al, 0x00
    out dx, al
    mov dx, 0x3F8 + 3
    mov al, 0x80
    out dx, al
    mov dx, 0x3F8 + 0
    mov al, 0x03
    out dx, al
    mov dx, 0x3F8 + 1
    mov al, 0x00
    out dx, al
    mov dx, 0x3F8 + 3
    mov al, 0x03
    out dx, al
    mov dx, 0x3F8 + 2
    mov al, 0xC7
    out dx, al
    mov dx, 0x3F8 + 4
    mov al, 0x0B
    out dx, al
    ret

serial_putc:
    mov ah, al
    mov dx, 0x3F8 + 5
.wait:
    in al, dx
    test al, 0x20
    jz .wait
    mov dx, 0x3F8
    mov al, ah
    out dx, al
    ret

; -------- Debug port (0xE9) --------
debug_putc:
    mov dx, 0xE9
    out dx, al
    ret

align 8
mb_info_ptr: dq 0
mb_magic:   dq 0

align 16
gdt_start:
    dq 0x0000000000000000
    dq 0x00AF9A000000FFFF
    dq 0x00AF92000000FFFF
gdt_end:

gdt_ptr:
    dw gdt_end - gdt_start - 1
    dd gdt_start

align 16
idt_start:
    times 32 dq 0
idt_end:

idt_ptr:
    dw idt_end - idt_start - 1
    dd idt_start

; -------- IDT setup (32-bit) --------
idt_init:
    ; Fill vectors 0..31 with a halt handler
    mov edi, idt_start
    mov ecx, 32
.fill:
    mov eax, isr_halt
    mov word [edi + 0], ax
    mov word [edi + 2], 0x08
    mov byte [edi + 4], 0
    mov byte [edi + 5], 0x8E
    shr eax, 16
    mov word [edi + 6], ax
    add edi, 8
    loop .fill
    lidt [idt_ptr]
    ret

isr_halt:
    cli
.h:
    hlt
    jmp .h

align 4096
pml4_table:
    dq pdpt_low + 0x003
    times 510 dq 0
    dq pdpt_high + 0x003

align 4096
pdpt_low:
    dq pd_low + 0x003
    times 511 dq 0

align 4096
pdpt_high:
    times 510 dq 0
    dq pd_high + 0x003
    dq 0

align 4096
pd_low:
%assign i 0
%rep 512
    dq (i * 0x200000) | 0x083
%assign i i+1
%endrep

align 4096
pd_high:
%assign j 0
%rep 512
    dq (0x00200000 + (j * 0x200000)) | 0x083
%assign j j+1
%endrep

align 16
boot_stack:
    times 4096 db 0
boot_stack_top:
