ENTRY(efi_boot_start)

KERNEL_VMA = 0xFFFFFFFF80000000;
BOOT_LMA   = 0x02000000;
KERNEL_LMA = 0x02200000;

PHDRS
{
  boot   PT_LOAD FLAGS(5);
  text   PT_LOAD FLAGS(5);
  rodata PT_LOAD FLAGS(4);
  data   PT_LOAD FLAGS(6);
  stack  PT_LOAD FLAGS(6);
}

SECTIONS
{
  . = BOOT_LMA;
  .boot : AT(BOOT_LMA) {
    KEEP(*(.multiboot))
    KEEP(*(.boot))
  } :boot
  __boot_lma_start = LOADADDR(.boot);
  __boot_lma_end = LOADADDR(.boot) + SIZEOF(.boot);

  . = KERNEL_VMA;
  __kernel_start = .;

  .text ALIGN(0x1000) : AT(KERNEL_LMA) {
    __text_start = .;
    *(.text .text.*)
    __text_end = .;
  } :text
  __text_lma_start = LOADADDR(.text);

  .rodata ALIGN(0x1000) : AT(ALIGN(LOADADDR(.text) + SIZEOF(.text), 0x1000)) {
    __rodata_start = .;
    *(.rodata .rodata.*)
    __rodata_end = .;
  } :rodata
  __rodata_lma_start = LOADADDR(.rodata);

  .data ALIGN(0x1000) : AT(ALIGN(LOADADDR(.rodata) + SIZEOF(.rodata), 0x1000)) {
    __data_start = .;
    *(.data .data.*)
    __data_end = .;
  } :data
  __data_lma_start = LOADADDR(.data);

  .bss ALIGN(0x1000) (NOLOAD) : AT(ALIGN(LOADADDR(.data) + SIZEOF(.data), 0x1000)) {
    __bss_start = .;
    *(COMMON)
    *(.bss .bss.*)
    __bss_end = .;
  } :data

  /DISCARD/ : {
    *(.eh_frame .eh_frame_hdr)
  }

  . = ALIGN(0x1000);
  __kernel_end = .;

  . = ALIGN(0x200000);
  __kernel_stack_base = .;
  .stack : AT(ALIGN(LOADADDR(.bss) + SIZEOF(.bss), 0x200000)) {
    *(.stack)
  } :stack
  __kernel_stack_top = .;
  __kernel_image_end = .;

  . = ALIGN(0x1000);
  __ist_stacks_start = .;
  .iststack : AT(ALIGN(LOADADDR(.stack) + SIZEOF(.stack), 0x1000)) {
    *(.iststack)
  } :stack
  __ist_stacks_end = .;

  __kernel_lma_base = KERNEL_LMA;
  __kernel_lma_start = LOADADDR(.text);
  __kernel_lma_end = ALIGN(LOADADDR(.iststack) + SIZEOF(.iststack), 0x1000);
  __kernel_stack_lma_start = LOADADDR(.stack);
  __kernel_stack_lma_end = LOADADDR(.stack) + SIZEOF(.stack);
  __ist_stacks_lma_start = LOADADDR(.iststack);
  __ist_stacks_lma_end = LOADADDR(.iststack) + SIZEOF(.iststack);
}
