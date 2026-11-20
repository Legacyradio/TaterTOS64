ENTRY(main)

PHDRS
{
  text PT_LOAD FLAGS(5);
  rodata PT_LOAD FLAGS(4);
  data PT_LOAD FLAGS(6);
  tls PT_TLS FLAGS(4);
}

SECTIONS
{
  . = 0x0000000000400000;
  .text ALIGN(0x1000) : { *(.text .text.*) } :text
  .rodata ALIGN(0x1000) : { *(.rodata .rodata.*) } :rodata
  .data ALIGN(0x1000) : { *(.data .data.*) } :data

  /* x86-64 variant-II TLS. With an explicit PHDRS, ld will NOT auto-create
   * PT_TLS, so declare it and assign the __thread sections here. Otherwise the
   * native .fry loader (elf_load_fry) sees no PT_TLS and __thread vars
   * (CPython _Py_tss_tstate, mimalloc _mi_heap_default) get no backing. .tdata is
   * initialized (lives in BOTH the data PT_LOAD and the TLS template); .tbss is
   * NOBITS (template only). ALIGN(0x40) keeps PT_TLS p_align >= the loader's
   * 16-byte floor so the linker's tpoffs match the loader's tls_alloc rounding. */
  .tdata ALIGN(0x40) : { *(.tdata .tdata.* .gnu.linkonce.td.*) } :data :tls
  .tbss ALIGN(0x40) (NOLOAD) : { *(.tbss .tbss.* .gnu.linkonce.tb.*) *(.tcommon) } :tls

  .bss ALIGN(0x1000) (NOLOAD) : { *(COMMON) *(.bss .bss.*) } :data
}
