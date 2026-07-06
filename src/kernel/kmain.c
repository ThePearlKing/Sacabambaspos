/* kmain.c - SacabambaspOS kernel, stage 1.
 *
 * The loader has already exited boot services; we own the machine. Set up
 * the CPU tables and drivers the shell needs and drop into it:
 *   GDT -> IDT/exceptions -> PIC remap -> PIT 100 Hz -> PS/2 kbd -> shell.
 * Still identity-mapped on the firmware's page tables; our own paging,
 * ring 3 and real userland come next stage. */
#include "kernel.h"
#include "../efi/version.h"

SbosBootInfo *g_bi;

/* ---- tiny libk -------------------------------------------------------- */
void *memset(void *d, int c, size_t n){ u8 *p=d; while(n--) *p++=(u8)c; return d; }
void *memcpy(void *d, const void *s, size_t n){ u8 *a=d; const u8 *b=s; while(n--) *a++=*b++; return d; }
void *memmove(void *d, const void *s, size_t n){
  u8 *a=d; const u8 *b=s;
  if(a==b || !n) return d;
  if(a<b){ while(n--) *a++=*b++; }
  else { a+=n; b+=n; while(n--) *--a=*--b; }
  return d;
}
size_t strlen(const char *s){ size_t n=0; while(s[n]) n++; return n; }
int strcmp(const char *a, const char *b){
  while(*a && *a==*b){ a++; b++; } return (u8)*a - (u8)*b;
}
int strncmp(const char *a, const char *b, size_t n){
  while(n && *a && *a==*b){ a++; b++; n--; }
  return n ? (u8)*a - (u8)*b : 0;
}

/* ---- bump heap (kalloc, never freed - fine for stage 1) --------------- */
static u64 heap_at, heap_end, heap_start;
static void kalloc_init(SbosBootInfo *bi){
  heap_start = heap_at = bi->heap_base;
  heap_end = bi->heap_base + bi->heap_size;
}
void *kalloc(size_t n){
  heap_at = (heap_at + 15) & ~15ULL;
  if(heap_at + n > heap_end) return NULL;
  void *p = (void*)heap_at; heap_at += n;
  return memset(p, 0, n);
}
u64 kalloc_used(void){ return heap_at - heap_start; }

/* ---- GDT --------------------------------------------------------------- */
static u64 gdt[3] __attribute__((aligned(16))) = {
  0,
  0x00209A0000000000ULL,     /* 0x08: 64-bit code, ring 0 */
  0x0000920000000000ULL,     /* 0x10: data, ring 0 */
};
struct __attribute__((packed)) dtr { u16 limit; u64 base; };
void gdt_flush(struct dtr *g);   /* entry.S */

static void gdt_init(void){
  struct dtr g = { sizeof(gdt)-1, (u64)gdt };
  gdt_flush(&g);
}

/* ---- IDT --------------------------------------------------------------- */
struct __attribute__((packed)) idt_ent {
  u16 off_lo, sel; u8 ist, attr; u16 off_mid; u32 off_hi, zero;
};
static struct idt_ent idt[256] __attribute__((aligned(16)));

#define ISR(n) extern void isr##n(void);
ISR(0) ISR(1) ISR(2) ISR(3) ISR(4) ISR(5) ISR(6) ISR(7)
ISR(8) ISR(9) ISR(10) ISR(11) ISR(12) ISR(13) ISR(14) ISR(15)
ISR(16) ISR(17) ISR(18) ISR(19) ISR(20) ISR(21) ISR(22) ISR(23)
ISR(24) ISR(25) ISR(26) ISR(27) ISR(28) ISR(29) ISR(30) ISR(31)
ISR(32) ISR(33) ISR(34) ISR(35) ISR(36) ISR(37) ISR(38) ISR(39)
ISR(40) ISR(41) ISR(42) ISR(43) ISR(44) ISR(45) ISR(46) ISR(47)
#undef ISR

static void idt_set(int v, void (*fn)(void)){
  u64 a = (u64)fn;
  idt[v] = (struct idt_ent){ a & 0xFFFF, 0x08, 0, 0x8E, (a>>16)&0xFFFF, a>>32, 0 };
}

static void idt_init(void){
  void (*stubs[48])(void) = {
    isr0,isr1,isr2,isr3,isr4,isr5,isr6,isr7,isr8,isr9,isr10,isr11,
    isr12,isr13,isr14,isr15,isr16,isr17,isr18,isr19,isr20,isr21,isr22,isr23,
    isr24,isr25,isr26,isr27,isr28,isr29,isr30,isr31,isr32,isr33,isr34,isr35,
    isr36,isr37,isr38,isr39,isr40,isr41,isr42,isr43,isr44,isr45,isr46,isr47,
  };
  for(int i=0; i<48; i++) idt_set(i, stubs[i]);
  struct dtr d = { sizeof(idt)-1, (u64)idt };
  __asm__ __volatile__("lidt %0" :: "m"(d));
}

/* ---- PIC + PIT ---------------------------------------------------------- */
#define PIC1_CMD 0x20
#define PIC1_DAT 0x21
#define PIC2_CMD 0xA0
#define PIC2_DAT 0xA1

static void pic_init(void){
  outb(PIC1_CMD,0x11); io_wait(); outb(PIC2_CMD,0x11); io_wait();
  outb(PIC1_DAT,0x20); io_wait(); outb(PIC2_DAT,0x28); io_wait();  /* remap */
  outb(PIC1_DAT,0x04); io_wait(); outb(PIC2_DAT,0x02); io_wait();
  outb(PIC1_DAT,0x01); io_wait(); outb(PIC2_DAT,0x01); io_wait();
  outb(PIC1_DAT,0xFC);            /* unmask IRQ0 (PIT) + IRQ1 (kbd) only */
  outb(PIC2_DAT,0xFF);
}

static void pit_init(void){
  u32 div = 1193182 / 100;        /* 100 Hz */
  outb(0x43, 0x36);
  outb(0x40, div & 0xFF);
  outb(0x40, (div >> 8) & 0xFF);
}

/* ---- interrupt dispatch -------------------------------------------------- */
volatile u64 ticks;

struct frame {
  u64 r15,r14,r13,r12,r11,r10,r9,r8,rbp,rdi,rsi,rdx,rcx,rbx,rax;
  u64 vec,err,rip,cs,rflags,rsp,ss;
};

static const char *exc_name(u64 v){
  static const char *n[] = {
    "#DE divide","#DB debug","NMI","#BP breakpoint","#OF overflow",
    "#BR bound","#UD invalid opcode","#NM fpu","#DF double fault","x87 seg",
    "#TS tss","#NP segment","#SS stack","#GP general protection","#PF page fault",
    "reserved","#MF x87","#AC align","#MC machine check","#XM simd","#VE virt","#CP cet",
  };
  return v < sizeof(n)/sizeof(n[0]) ? n[v] : "?";
}

static void panic(struct frame *f){
  con_color(C_WHITE, C_RED);
  con_puts("\n  KERNEL PANIC  ");
  con_color(C_LRED, C_BLACK);
  con_puts(" cpu exception ");
  con_put_u64(f->vec);
  con_puts(" (");   con_puts(exc_name(f->vec)); con_puts(")\n");
  con_fg(C_LGREY);
  con_puts("  rip="); con_put_hex(f->rip);
  con_puts("  err="); con_put_hex(f->err);
  if(f->vec == 14){ u64 cr2; __asm__ __volatile__("mov %%cr2,%0":"=r"(cr2));
    con_puts("  cr2="); con_put_hex(cr2); }
  con_puts("\n  rsp="); con_put_hex(f->rsp);
  con_puts("  rax="); con_put_hex(f->rax);
  con_puts("  rdi="); con_put_hex(f->rdi);
  con_puts("\n\n  system halted - power off or reboot. nothing was written to disk.\n");
  con_cursor(0);
  cli();
  for(;;) hlt();
}

void isr_common(struct frame *f){
  if(f->vec < 32){ panic(f); return; }
  if(f->vec == 32) ticks++;
  else if(f->vec == 33) kbd_irq();
  /* EOI */
  if(f->vec >= 40) outb(PIC2_CMD, 0x20);
  outb(PIC1_CMD, 0x20);
}

/* ---- init.rc ------------------------------------------------------------- */
static void run_initrc(SbosBootInfo *bi){
  if(!bi->initrc_base || !bi->initrc_size) return;
  klog_tagged(" INIT ", C_LCYAN, "running /SBOS/INIT.RC");
  char *s = (char*)bi->initrc_base;
  u64 n = bi->initrc_size;
  char line[240]; u32 li = 0;
  for(u64 i=0; i<=n; i++){
    char c = i<n ? s[i] : '\n';
    if(c=='\r') continue;
    if(c=='\n'){
      line[li]=0;
      /* strip leading spaces; '#' comments */
      char *p=line; while(*p==' ') p++;
      if(*p && *p!='#') shell_run_line(p, 1);
      li=0; continue;
    }
    if(li < sizeof(line)-1) line[li++]=c;
  }
}

/* ---- main ----------------------------------------------------------------- */
void kmain(SbosBootInfo *bi){
  if(!bi || bi->magic != SBOS_BOOTINFO_MAGIC) for(;;) hlt();
  g_bi = bi;
  kalloc_init(bi);
  con_init(bi);

  con_putc('\n');
  con_fg(C_WHITE);  con_puts("  Sacabambasp");
  con_fg(C_LCYAN);  con_puts("OS ");
  con_fg(C_YELLOW); con_puts(SBOS_VERSION);
  con_fg(C_DGREY);  con_puts("  -  kernel stage 1\n");
  con_fg(C_DGREY);  con_puts("  ==========================================\n\n");

  klog_ok("boot services exited, kernel owns the machine");
  gdt_init();  klog_ok("GDT loaded (own descriptors, firmware's gone)");
  idt_init();  klog_ok("IDT armed, 32 exceptions + 16 IRQs");
  pic_init();  klog_ok("PIC remapped to 0x20, IRQ0+IRQ1 unmasked");
  pit_init();  klog_ok("PIT ticking at 100 Hz");
  kbd_init();  klog_ok("PS/2 keyboard ready");
  sti();       klog_ok("interrupts enabled");

  con_fg(C_DGREY); con_puts("  [ MEM  ] ");
  con_fg(C_LGREY); con_puts("usable RAM ");
  con_fg(C_LCYAN); con_put_u64(bi->total_ram >> 20);
  con_fg(C_LGREY); con_puts(" MiB, kernel heap ");
  con_fg(C_LCYAN); con_put_u64(bi->heap_size >> 20);
  con_fg(C_LGREY); con_puts(" MiB\n");

  klog_ok("nothing written to any disk (live mode)");
  con_putc('\n');

  run_initrc(bi);
  con_putc('\n');
  shell_main(bi);
}
