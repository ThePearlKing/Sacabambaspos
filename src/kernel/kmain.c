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
  if(heap_at > heap_end || n > heap_end - heap_at) return NULL;   /* no wrap */
  void *p = (void*)heap_at; heap_at += n;
  return memset(p, 0, n);
}
u64 kalloc_used(void){ return heap_at - heap_start; }

/* ---- GDT + TSS ---------------------------------------------------------- */
/* The TSS gives #DF and NMI their own known-good stacks (IST): a fault on a
 * corrupted kernel stack then still reaches the panic screen instead of
 * triple-faulting into a silent reboot. */
struct __attribute__((packed)) tss64 {
  u32 r0; u64 rsp0, rsp1, rsp2; u64 r1;
  u64 ist[7]; u64 r2; u16 r3, iomap;
};
static struct tss64 tss;
static u8 ist_df[8192]  __attribute__((aligned(16)));
static u8 ist_nmi[8192] __attribute__((aligned(16)));

static u64 gdt[5] __attribute__((aligned(16))) = {
  0,
  0x00209A0000000000ULL,     /* 0x08: 64-bit code, ring 0 */
  0x0000920000000000ULL,     /* 0x10: data, ring 0 */
  0, 0,                      /* 0x18: TSS descriptor (16 bytes), filled at init */
};
struct __attribute__((packed)) dtr { u16 limit; u64 base; };
void gdt_flush(struct dtr *g);   /* entry.S */

static void gdt_init(void){
  tss.iomap = sizeof(tss);                      /* no I/O bitmap */
  tss.ist[0] = (u64)ist_df  + sizeof(ist_df);   /* IST1: #DF */
  tss.ist[1] = (u64)ist_nmi + sizeof(ist_nmi);  /* IST2: NMI */
  u64 b = (u64)&tss, lim = sizeof(tss)-1;
  gdt[3] = (lim & 0xFFFF) | ((b & 0xFFFFFF) << 16) | (0x89ULL << 40) |
           (((lim >> 16) & 0xF) << 48) | (((b >> 24) & 0xFF) << 56);
  gdt[4] = b >> 32;
  struct dtr g = { sizeof(gdt)-1, (u64)gdt };
  gdt_flush(&g);
  __asm__ __volatile__("ltr %0" :: "r"((u16)0x18));
}

/* ---- IDT --------------------------------------------------------------- */
struct __attribute__((packed)) idt_ent {
  u16 off_lo, sel; u8 ist, attr; u16 off_mid; u32 off_hi, zero;
};
static struct idt_ent idt[256] __attribute__((aligned(16)));

extern char vector_stubs[];      /* entry.S: 256 stubs, 16 bytes apart */

static void idt_set(int v, u64 a, u8 ist){
  idt[v] = (struct idt_ent){ a & 0xFFFF, 0x08, ist, 0x8E, (a>>16)&0xFFFF, a>>32, 0 };
}

static void idt_init(void){
  for(int i=0; i<256; i++)
    idt_set(i, (u64)vector_stubs + (u64)i*16,
            i==8 ? 1 : i==2 ? 2 : 0);           /* #DF -> IST1, NMI -> IST2 */
  struct dtr d = { sizeof(idt)-1, (u64)idt };
  __asm__ __volatile__("lidt %0" :: "m"(d));
}

/* ---- local APIC ---------------------------------------------------------
 * Firmware may hand off with LVT sources (timer, thermal, error) armed.
 * Mask them all and program virtual-wire mode (LINT0 = ExtINT so the 8259
 * lines reach the CPU, LINT1 = NMI) before enabling interrupts. Handles
 * both xAPIC (MMIO) and x2APIC (MSR) handoff states. */
static int lapic_x2;
static volatile u32 *lapic_mmio;

static inline u64 rdmsr(u32 m){
  u32 lo, hi; __asm__ __volatile__("rdmsr":"=a"(lo),"=d"(hi):"c"(m));
  return ((u64)hi<<32)|lo;
}
static inline void wrmsr(u32 m, u64 v){
  __asm__ __volatile__("wrmsr"::"c"(m),"a"((u32)v),"d"((u32)(v>>32)));
}
static void lapic_write(u32 reg, u32 val){
  if(lapic_x2) wrmsr(0x800 + (reg>>4), val);
  else if(lapic_mmio) lapic_mmio[reg>>2] = val;
}
static u32 lapic_read(u32 reg){
  if(lapic_x2) return (u32)rdmsr(0x800 + (reg>>4));
  return lapic_mmio ? lapic_mmio[reg>>2] : 0;
}

static int lapic_init(void){
  u32 a, b, c, d;
  __asm__ __volatile__("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1));
  if(!(d & (1u<<9))) return 0;                  /* no local APIC */
  u64 base = rdmsr(0x1B);
  if(!(base & (1u<<11))) return 0;              /* globally disabled */
  lapic_x2 = !!(base & (1u<<10));
  lapic_mmio = (volatile u32*)(base & 0x000FFFFFFFFFF000ULL);

  u32 maxlvt = (lapic_read(0x30) >> 16) & 0xFF;
  lapic_write(0xF0, 0x100 | 0xFF);              /* soft-enable, spurious vec 0xFF */
  lapic_write(0x320, 1u<<16);                   /* LVT timer: masked */
  if(maxlvt >= 4) lapic_write(0x340, 1u<<16);   /* LVT perf: masked */
  if(maxlvt >= 5) lapic_write(0x330, 1u<<16);   /* LVT thermal: masked */
  lapic_write(0x370, 1u<<16);                   /* LVT error: masked */
  lapic_write(0x350, 0x700);                    /* LINT0 = ExtINT (8259 wire) */
  lapic_write(0x360, 0x400);                    /* LINT1 = NMI */
  return 1;
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

/* read a PIC's in-service register (OCW3) */
static u8 pic_isr(u16 cmdport){ outb(cmdport, 0x0B); return inb(cmdport); }

void isr_common(struct frame *f){
  if(f->vec < 32){ panic(f); return; }

  if(f->vec >= 48) return;   /* firmware-armed LAPIC leftovers: LVTs are
                              * masked at init; nothing to EOI, never panic */

  /* spurious IRQ7/IRQ15: the 8259 raises them on line glitches with no
   * in-service bit set - EOIing a phantom would eat a real interrupt */
  if(f->vec == 39 && !(pic_isr(PIC1_CMD) & 0x80)) return;
  if(f->vec == 47 && !(pic_isr(PIC2_CMD) & 0x80)){
    outb(PIC1_CMD, 0x20);    /* master still saw the cascade line */
    return;
  }

  if(f->vec == 32) ticks++;
  else if(f->vec == 33) kbd_irq();

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
  if(!con_init(bi)) for(;;) hlt();   /* reason already on serial */

  con_putc('\n');
  con_fg(C_WHITE);  con_puts("  Sacabambasp");
  con_fg(C_LCYAN);  con_puts("OS ");
  con_fg(C_YELLOW); con_puts(SBOS_VERSION);
  con_fg(C_DGREY);  con_puts("  -  kernel stage 1\n");
  con_fg(C_DGREY);  con_puts("  ==========================================\n\n");

  klog_ok("boot services exited, kernel owns the machine");
  gdt_init();  klog_ok("GDT + TSS loaded (fault stacks armed)");
  idt_init();  klog_ok("IDT armed, all 256 vectors");
  if(lapic_init()) klog_ok("local APIC masked, virtual-wire mode set");
  else             klog_tagged(" INFO ", C_DGREY, "no local APIC (pre-APIC CPU)");
  pic_init();  klog_ok("PIC remapped to 0x20, IRQ0+IRQ1 unmasked");
  pit_init();  klog_ok("PIT ticking at 100 Hz");
  if(kbd_init()) klog_ok("PS/2 keyboard ready");
  else klog_tagged(" WARN ", C_YELLOW,
                   "no PS/2 controller - USB keyboard needs firmware legacy mode");
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
