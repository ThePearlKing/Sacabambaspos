/* Linker script to emit a PE/COFF-convertible shared object for a UEFI app.
 * Adapted from the classic gnu-efi elf_x86_64_efi.lds layout. */
OUTPUT_FORMAT("elf64-x86-64", "elf64-x86-64", "elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(efi_main)
SECTIONS
{
  . = 0;
  ImageBase = .;
  .hash : { *(.hash) }          /* keep dynamic loader happy */
  .gnu.hash : { *(.gnu.hash) }
  . = ALIGN(4096);
  .eh_frame : { *(.eh_frame) }
  . = ALIGN(4096);
  .text : {
    _text = .;
    *(.text)
    *(.text.*)
    *(.gnu.linkonce.t.*)
    . = ALIGN(16);
  }
  _etext = .;
  _text_size = . - _text;
  . = ALIGN(4096);
  .reloc : { *(.reloc) }
  . = ALIGN(4096);
  .data : {
    _data = .;
    *(.rodata*)
    *(.got.plt)
    *(.got)
    *(.data*)
    *(.sdata)
    /* the EFI loader zeroes .bss, but be safe and keep it in the image */
    *(.dynbss)
    *(.bss)
    *(COMMON)
    . = ALIGN(16);
  }
  . = ALIGN(4096);   /* PE SectionAlignment: every section RVA must be 4K */
  .dynamic  : { *(.dynamic) }
  . = ALIGN(4096);
  .rela : { *(.rela.dyn) *(.rela.plt) *(.rela.got) *(.rela.data*) }
  . = ALIGN(4096);
  .dynsym   : { *(.dynsym) }
  .dynstr   : { *(.dynstr) }
  _edata = .;
  _data_size = . - _data;
  . = ALIGN(4096);
  /DISCARD/ : { *(.rel.reloc) *(.note.GNU-stack) *(.comment) }
}
