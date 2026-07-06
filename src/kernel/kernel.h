/* kernel.h - shared kernel-internal declarations for SacabambaspOS stage 1.
 * Freestanding x86-64 only. */
#ifndef SBOS_KERNEL_H
#define SBOS_KERNEL_H

#include "../shared/bootinfo.h"

typedef unsigned char       u8;
typedef unsigned short      u16;
typedef unsigned int        u32;
typedef unsigned long long  u64;
typedef signed long long    s64;
typedef unsigned long       size_t;

#define NULL ((void*)0)

/* ---- port I/O ---- */
static inline void outb(u16 p, u8 v){ __asm__ __volatile__("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline u8   inb(u16 p){ u8 v; __asm__ __volatile__("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void io_wait(void){ outb(0x80, 0); }
static inline void cli(void){ __asm__ __volatile__("cli"); }
static inline void sti(void){ __asm__ __volatile__("sti"); }
static inline void hlt(void){ __asm__ __volatile__("hlt"); }

/* ---- libk (kmain.c) ---- */
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
int   strcmp(const char *a, const char *b);
int   strncmp(const char *a, const char *b, size_t n);
size_t strlen(const char *s);
void *kalloc(size_t n);                 /* bump allocator, never freed */
u64   kalloc_used(void);

/* ---- console (console.c) ---- */
/* 16-color palette, VGA-ish indices */
enum {
  C_BLACK, C_BLUE, C_GREEN, C_CYAN, C_RED, C_MAGENTA, C_BROWN, C_LGREY,
  C_DGREY, C_LBLUE, C_LGREEN, C_LCYAN, C_LRED, C_PINK, C_YELLOW, C_WHITE,
};
int  con_init(SbosBootInfo *bi);        /* 0 = unusable fb/heap (reason on serial) */
void con_color(u8 fg, u8 bg);
void con_fg(u8 fg);
void con_putc(char c);
void con_puts(const char *s);
void con_put_u64(u64 v);
void con_put_hex(u64 v);
void con_clear(void);
void con_cursor(int on);
u32  con_cols(void), con_rows(void);
void con_getxy(u32 *x, u32 *y);
void con_setxy(u32 x, u32 y);

/* log helpers, same look as the boot stage */
void klog_ok(const char *m);
void klog_fail(const char *m);
void klog_tagged(const char *tag, u8 tagcolor, const char *m);

/* ---- interrupts / time (kmain.c) ---- */
extern volatile u64 ticks;              /* PIT, 100 Hz */

/* ---- keyboard (kbd.c) ---- */
int  kbd_init(void);                    /* 0 = no PS/2 controller */
void kbd_irq(void);                     /* called from IRQ1 stub */
int  kbd_getc(void);                    /* blocking, returns ASCII (or KEY_*) */
#define KEY_UP    0x100
#define KEY_DOWN  0x101
#define KEY_LEFT  0x102
#define KEY_RIGHT 0x103

/* ---- shell (shell.c) ---- */
void shell_run_line(const char *line, int echo);   /* execute one command */
void shell_main(SbosBootInfo *bi);                 /* interactive loop, no return */

extern SbosBootInfo *g_bi;

#endif
