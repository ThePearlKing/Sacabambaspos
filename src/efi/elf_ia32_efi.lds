/* 32-bit UEFI variant of the linker script (for 32-bit EFI Macs / firmware). */
OUTPUT_FORMAT("elf32-i386", "elf32-i386", "elf32-i386")
OUTPUT_ARCH(i386)
ENTRY(efi_main)
SECTIONS
{
  . = 0;
  ImageBase = .;
  .hash : { *(.hash) }
  .gnu.hash : { *(.gnu.hash) }
  . = ALIGN(4096);
  .eh_frame : { *(.eh_frame) }
  . = ALIGN(4096);
  .text : { _text = .; *(.text) *(.text.*) *(.gnu.linkonce.t.*) . = ALIGN(16); }
  _etext = .;
  . = ALIGN(4096);
  .reloc : { *(.reloc) }
  . = ALIGN(4096);
  .data : {
    _data = .;
    *(.rodata*) *(.got.plt) *(.got) *(.data*) *(.sdata)
    *(.dynbss) *(.bss) *(COMMON)
    . = ALIGN(16);
  }
  .dynamic : { *(.dynamic) }
  . = ALIGN(4096);
  .rel : { *(.rel.dyn) *(.rel.plt) *(.rel.got) *(.rel.data*) }
  . = ALIGN(4096);
  .dynsym : { *(.dynsym) }
  .dynstr : { *(.dynstr) }
  _edata = .;
  . = ALIGN(4096);
  /DISCARD/ : { *(.rel.reloc) *(.note.GNU-stack) *(.comment) }
}
