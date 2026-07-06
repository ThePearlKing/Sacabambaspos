/* kernel.h - shared kernel-internal declarations for SacabambaspOS.
 * Freestanding x86-64 only. */
#ifndef SBOS_KERNEL_H
#define SBOS_KERNEL_H

#include "../shared/bootinfo.h"
#include "../shared/sbos_abi.h"

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
static inline void outl(u16 p, u32 v){ __asm__ __volatile__("outl %0,%1"::"a"(v),"Nd"(p)); }
static inline u32  inl(u16 p){ u32 v; __asm__ __volatile__("inl %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void io_wait(void){ outb(0x80, 0); }
static inline void cli(void){ __asm__ __volatile__("cli"); }
static inline void sti(void){ __asm__ __volatile__("sti"); }
static inline void hlt(void){ __asm__ __volatile__("hlt"); }
static inline u64 rdmsr(u32 m){
  u32 lo, hi; __asm__ __volatile__("rdmsr":"=a"(lo),"=d"(hi):"c"(m));
  return ((u64)hi<<32)|lo;
}
static inline void wrmsr(u32 m, u64 v){
  __asm__ __volatile__("wrmsr"::"c"(m),"a"((u32)v),"d"((u32)(v>>32)));
}
static inline void cpuid(u32 leaf, u32 *a, u32 *b, u32 *c, u32 *d){
  __asm__ __volatile__("cpuid":"=a"(*a),"=b"(*b),"=c"(*c),"=d"(*d):"a"(leaf),"c"(0));
}

/* ---- libk (kmain.c) ---- */
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
int   strcmp(const char *a, const char *b);
int   strncmp(const char *a, const char *b, size_t n);
int   memcmp_k(const void *a, const void *b, size_t n);   /* proc.c */
size_t strlen(const char *s);
void *kalloc(size_t n);                 /* bump allocator, never freed */
void *kalloc_page(void);                /* one zeroed, 4K-aligned page */
u64   kalloc_used(void);

/* ---- paging (paging.c) ----
 * Our own 4-level tables: identity map of RAM/MMIO, kernel W^X + NX, and a
 * user window (high canonical low-half) for ring-3 processes. */
int  paging_init(SbosBootInfo *bi);       /* build + switch CR3; 0 = failed */
void vm_ensure_identity(u64 base, u64 len); /* map phys range 1:1 (MMIO >4G) */
int  vm_user_map(u64 va, u64 pa, int writable, int executable);
void vm_user_unmap_range(u64 va, u64 len);  /* also flushes TLB */
int  vm_user_ok(u64 va, u64 len);           /* range fully user-mapped? */
#define USER_BASE      0x0000700000000000ULL   /* user image link address */
#define USER_TOP       0x0000700100000000ULL   /* 4 GiB user window */
#define USER_STACK_TOP 0x00007000FFFFF000ULL   /* grows down, guard below */

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
int  kbd_trygetc(void);                 /* non-blocking, -1 = nothing pending */
#define KEY_UP    SBOS_KEY_UP           /* one namespace, shared with userland */
#define KEY_DOWN  SBOS_KEY_DOWN
#define KEY_LEFT  SBOS_KEY_LEFT
#define KEY_RIGHT SBOS_KEY_RIGHT
#define KEY_DEL   SBOS_KEY_DEL
#define KEY_HOME  SBOS_KEY_HOME
#define KEY_END   SBOS_KEY_END

/* ---- USB keyboard, xHCI (usb.c) ----
 * Polled driver: no IRQ line needed, kbd_getc pumps it. All three calls are
 * main-thread only (never from an interrupt handler). */
int  usbkbd_init(void);   /* after sti (needs ticks); -1 = no xHCI, else #kbds */
void usbkbd_poll(void);   /* pump event rings, hotplug; interrupts must be on */
int  usbkbd_pop(void);    /* -1 = empty, else ASCII / KEY_* */

/* ---- processes / ring 3 (proc.c + entry.S) ---- */
void proc_init(SbosBootInfo *bi);       /* arm syscall MSRs + TSS rsp0 */
void proc_fault(u64 vec, u64 rip);      /* user CPU fault: kill, no return */
int  proc_exec(const char *name);       /* run /bin ELF; <0 = not found/bad */
int  proc_count(void);                  /* programs in BIN.PAK */
const char *proc_name(int i);           /* i-th program name, NULL past end */
int  proc_has(const char *name, size_t n);   /* exists? (name not NUL-term) */
int  user_enter(u64 entry, u64 ursp);   /* entry.S: drop to ring 3 */
void user_abort(u64 code);              /* entry.S: longjmp out, no return */
void set_rsp0(u64 sp);                  /* kmain.c: TSS ring-0 stack */
const char *exc_name(u64 vec);          /* kmain.c: exception mnemonic */

/* ---- shell (shell.c) ---- */
void shell_run_line(const char *line, int echo);   /* execute one command */
void shell_main(SbosBootInfo *bi);                 /* interactive loop, no return */

extern SbosBootInfo *g_bi;

#endif
