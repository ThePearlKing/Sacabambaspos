/* reloc.S - dummy PE base-relocation block.
 *
 * Our EFI app is built -fpic (fully RIP-relative, zero runtime relocations),
 * but objcopy would then emit the PE with IMAGE_FILE_RELOCS_STRIPPED set and
 * no .reloc directory. EDK2 / real UEFI firmware refuses to load such an image
 * (it can only honour ImageBase, which is 0). Providing one empty, well-formed
 * base-relocation block clears the stripped flag and makes the loader relocate
 * the image to any address it likes (a no-op for PIC code) - so it loads
 * everywhere. This is the same trick gnu-efi's crt0 uses. */
	.section .reloc, "a"
	.long 0        /* Page RVA  */
	.long 10       /* Block size: 8-byte header + one 2-byte entry */
	.word 0        /* Type 0 (IMAGE_REL_BASED_ABSOLUTE) offset 0 = no-op */

	.section .note.GNU-stack, "", @progbits   /* no executable stack */
