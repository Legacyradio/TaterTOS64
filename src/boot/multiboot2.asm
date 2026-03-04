; Multiboot2 header for GRUB (UEFI/BIOS)
BITS 64

section .multiboot
align 8

multiboot2_header:
    dd 0xE85250D6                ; magic
    dd 0x00000000                ; architecture (i386)
    dd multiboot2_header_end - multiboot2_header
    dd -(0xE85250D6 + 0x00000000 + (multiboot2_header_end - multiboot2_header))

    ; End tag
    dw 0x0000
    dw 0x0000
    dd 8

multiboot2_header_end:
