ENTRY(main)

SECTIONS
{
  . = 0x0000000000400000;
  .text : { *(.text .text.*) }
  .rodata : { *(.rodata .rodata.*) }
  .data : { *(.data .data.*) }
  .bss : { *(COMMON) *(.bss .bss.*) }
}
