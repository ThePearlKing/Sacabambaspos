/* bootinfo.h - the handoff contract between the UEFI boot stage and the
 * kernel. The loader fills this in before ExitBootServices and passes a
 * pointer to the kernel entry point (SysV ABI, first argument).
 * Keep layout stable: bump SBOS_BOOTINFO_MAGIC when it changes. */
#ifndef SBOS_BOOTINFO_H
#define SBOS_BOOTINFO_H

#define SBOS_BOOTINFO_MAGIC 0x53414341424F4F54ULL   /* "SACABOOT" */

/* framebuffer pixel layout, 32bpp only (loader refuses anything else) */
#define SBOS_PIXFMT_BGRX 0   /* byte order B,G,R,X - most UEFI GOPs */
#define SBOS_PIXFMT_RGBX 1

typedef struct {
  unsigned long long magic;

  /* framebuffer (identity-mapped, 32bpp) */
  unsigned long long fb_base;
  unsigned int       fb_width, fb_height;
  unsigned int       fb_pitch;          /* in PIXELS, not bytes */
  unsigned int       fb_format;         /* SBOS_PIXFMT_* */

  /* contiguous conventional-memory block the loader reserved for the kernel
   * heap (EfiLoaderData, so it stays ours after ExitBootServices) */
  unsigned long long heap_base, heap_size;

  unsigned long long total_ram;         /* usable RAM per the UEFI memory map */
  unsigned long long rsdp;              /* ACPI RSDP, 0 if not found */

  /* INIT.RC boot script, loaded from the ESP (0/0 if absent) */
  unsigned long long initrc_base, initrc_size;
} SbosBootInfo;

#endif
