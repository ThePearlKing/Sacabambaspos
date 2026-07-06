/* user.ld - SacabambaspOS user programs: static ET_EXEC at the bottom of
 * the user window (USER_BASE in the kernel's kernel.h). Segments are
 * page-aligned so the kernel can hand each one its own W^X permissions. */
ENTRY(_start)

PHDRS {
  text   PT_LOAD FLAGS(5);   /* r-x */
  rodata PT_LOAD FLAGS(4);   /* r-- */
  data   PT_LOAD FLAGS(6);   /* rw- */
}

SECTIONS {
  . = 0x700000000000;

  .text : { *(.text .text.*) } :text

  . = ALIGN(4096);
  .rodata : { *(.rodata .rodata.*) } :rodata

  . = ALIGN(4096);
  .data : { *(.data .data.*) } :data
  .bss  : { *(.bss .bss.*) *(COMMON) } :data

  /DISCARD/ : { *(.comment) *(.eh_frame) *(.note.*) }
}
