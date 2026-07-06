/* linker.ld - SacabambaspOS kernel, static PIE.
 * Linked at 0, loaded anywhere: the loader applies R_X86_64_RELATIVE
 * relocations with the actual load address. */
ENTRY(_start)

PHDRS {
  text    PT_LOAD FLAGS(5);   /* r-x */
  rodata  PT_LOAD FLAGS(4);   /* r-- */
  data    PT_LOAD FLAGS(6);   /* rw- */
  dynamic PT_DYNAMIC FLAGS(4);
}

SECTIONS {
  . = 0;

  .text : { *(.text .text.*) } :text

  . = ALIGN(4096);
  .rodata   : { *(.rodata .rodata.*) } :rodata
  .rela.dyn : { *(.rela.dyn .rela.*) } :rodata
  .dynamic  : { *(.dynamic) } :rodata :dynamic
  .dynsym   : { *(.dynsym) } :rodata
  .dynstr   : { *(.dynstr) } :rodata
  .hash     : { *(.hash) } :rodata
  .gnu.hash : { *(.gnu.hash) } :rodata

  . = ALIGN(4096);
  .data : { *(.data .data.*) *(.got) *(.got.plt) } :data
  .bss  : { *(.bss .bss.*) *(COMMON) } :data

  /DISCARD/ : { *(.comment) *(.eh_frame) *(.note.*) *(.interp) }
}
