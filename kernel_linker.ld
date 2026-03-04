ENTRY(efi_boot_start)

KERNEL_VMA = 0xFFFFFFFF80000000;
BOOT_LMA   = 0x02000000;
KERNEL_LMA = 0x02200000;

SECTIONS
{
  . = BOOT_LMA;
  .boot : AT(BOOT_LMA) {
    KEEP(*(.multiboot))
    KEEP(*(.boot))
  }
  __boot_lma_start = LOADADDR(.boot);
  __boot_lma_end = LOADADDR(.boot) + SIZEOF(.boot);

  . = KERNEL_VMA;
  __kernel_start = .;

  .text : AT(KERNEL_LMA) {
    *(.text .text.*)
  }
  .rodata : AT(ALIGN(LOADADDR(.text) + SIZEOF(.text), ALIGNOF(.rodata))) {
    *(.rodata .rodata.*)
  }
  .data : AT(ALIGN(LOADADDR(.rodata) + SIZEOF(.rodata), ALIGNOF(.data))) {
    *(.data .data.*)
  }
  .bss : AT(ALIGN(LOADADDR(.data) + SIZEOF(.data), ALIGNOF(.bss))) {
    *(COMMON)
    *(.bss .bss.*)
  }

  /DISCARD/ : {
    *(.eh_frame .eh_frame_hdr)
  }

  __kernel_end = .;

  . = ALIGN(0x200000);
  __kernel_stack_base = .;
  .stack : AT(ALIGN(LOADADDR(.bss) + SIZEOF(.bss), 0x200000)) {
    *(.stack)
  }
  __kernel_stack_top = .;
  __kernel_image_end = .;

  . = ALIGN(0x1000);
  __ist_stacks_start = .;
  .iststack : AT(ALIGN(LOADADDR(.stack) + SIZEOF(.stack), 0x1000)) {
    *(.iststack)
  }
  __ist_stacks_end = .;

  __kernel_lma_base = KERNEL_LMA;
  __kernel_lma_start = LOADADDR(.text);
  __kernel_lma_end = ALIGN(LOADADDR(.iststack) + SIZEOF(.iststack), 0x1000);
  __kernel_stack_lma_start = LOADADDR(.stack);
  __kernel_stack_lma_end = LOADADDR(.stack) + SIZEOF(.stack);
  __ist_stacks_lma_start = LOADADDR(.iststack);
  __ist_stacks_lma_end = LOADADDR(.iststack) + SIZEOF(.iststack);
}
