/* proc.c - ring-3 processes: syscalls, the BIN.PAK program archive, and the
 * ELF loader that puts programs in the user window.
 *
 * Model for this stage: one foreground process, run to completion. exec
 * maps the program at its link address inside [USER_BASE, USER_TOP), gives
 * it a stack under USER_STACK_TOP (with an unmapped guard page below), and
 * user_enter() drops to ring 3. The process ends by the exit syscall or by
 * faulting; both land in user_abort(), which longjmps back into proc_exec.
 * User pages carry U at every table level and kernel pages never do, so
 * ring 3 cannot so much as read kernel memory; W^X holds in user space too
 * (a segment asking for W+X is refused).
 *
 * Physical pages come from the bump allocator and are not reclaimed yet -
 * a real page allocator is on the roadmap; at ~300 KiB per run of the
 * shipped programs, the 64 MiB heap is nowhere near the limit in practice.
 *
 * Syscalls run on their own kernel stack (sstack in entry.S, also the TSS
 * rsp0 stack for interrupts arriving from ring 3) with interrupts on, so a
 * blocking getkey keeps the PIT ticking and the USB keyboard polled. */
#include "kernel.h"

/* ---- BIN.PAK ------------------------------------------------------------ */
typedef struct { char magic[8]; u32 count, pad; } PakHdr;
typedef struct { char name[24]; u64 off, size; } PakEnt;

static const PakHdr *pak;
static const PakEnt *pak_ent;
static u64 pak_size;

static void pak_init(SbosBootInfo *bi){
  if(!bi->pak_base || bi->pak_size < sizeof(PakHdr)) return;
  const PakHdr *h = (const PakHdr*)bi->pak_base;
  if(memcmp_k(h->magic, "SBOSPAK1", 8)) return;
  if(h->count > 256 ||
     sizeof(PakHdr) + (u64)h->count * sizeof(PakEnt) > bi->pak_size) return;
  pak = h;
  pak_ent = (const PakEnt*)(h + 1);
  pak_size = bi->pak_size;
}

int proc_count(void){ return pak ? (int)pak->count : 0; }

const char *proc_name(int i){
  if(!pak || i < 0 || i >= (int)pak->count) return NULL;
  return pak_ent[i].name;
}

static const PakEnt *pak_find(const char *name){
  if(!pak) return NULL;
  for(u32 i = 0; i < pak->count; i++){
    const PakEnt *e = &pak_ent[i];
    /* names are NUL-padded by mkpak.py; last byte always NUL */
    if(!strcmp(e->name, name)) return e;
  }
  return NULL;
}

/* does a program with this (unterminated) name exist? The shell's live
 * syntax coloring asks on every keystroke. */
int proc_has(const char *name, size_t n){
  if(!pak || n == 0 || n >= sizeof(pak_ent[0].name)) return 0;
  for(u32 i = 0; i < pak->count; i++)
    if(!strncmp(pak_ent[i].name, name, n) && !pak_ent[i].name[n]) return 1;
  return 0;
}

/* memcmp lives here for now - proc.c is its only user */
int memcmp_k(const void *a, const void *b, size_t n){
  const u8 *x = a, *y = b;
  for(; n--; x++, y++) if(*x != *y) return *x - *y;
  return 0;
}

/* ---- ELF64 loader -------------------------------------------------------- */
typedef struct {
  u8  ident[16];
  u16 type, machine; u32 version;
  u64 entry, phoff, shoff; u32 flags;
  u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} Ehdr;
typedef struct {
  u32 type, flags; u64 off, vaddr, paddr, filesz, memsz, align;
} Phdr;

#define PT_LOAD 1
#define PF_X 1
#define PF_W 2

#define USTACK_PAGES 64                       /* 256 KiB */
#define USTACK_LO (USER_STACK_TOP - USTACK_PAGES*4096ULL)

static u64 img_lo, img_hi;                    /* mapped program span */

/* map [va, va+memsz) as fresh zeroed pages and copy in filesz bytes from
 * src. Copies go through the identity-mapped physical page, not the user
 * VA - CR0.WP would (rightly) refuse a supervisor write to user .text. */
static int load_segment(u64 va, const u8 *src, u64 filesz, u64 memsz,
                        int writable, int executable){
  u64 start = va & ~4095ULL, end = (va + memsz + 4095) & ~4095ULL;
  for(u64 page = start; page < end; page += 4096){
    u8 *pa = kalloc_page();
    if(!pa) return 0;
    /* slice of the file that lands inside this page */
    u64 flo = page > va ? page - va : 0;              /* file offset at page */
    u64 pgo = page > va ? 0 : va - page;              /* offset inside page */
    if(flo < filesz){
      u64 n = filesz - flo;
      if(n > 4096 - pgo) n = 4096 - pgo;
      memcpy(pa + pgo, src + flo, n);
    }
    if(!vm_user_map(page, (u64)pa, writable, executable)) return 0;
  }
  if(start < img_lo) img_lo = start;
  if(end   > img_hi) img_hi = end;
  return 1;
}

static int elf_run(const u8 *file, u64 size, int *exit_code){
  if(size < sizeof(Ehdr)) return -1;
  const Ehdr *eh = (const Ehdr*)file;
  static const u8 want[8] = {0x7F,'E','L','F',2,1,1,0};   /* 64-bit LSB */
  if(memcmp_k(eh->ident, want, 7)) return -1;
  if(eh->type != 2 || eh->machine != 0x3E) return -1;     /* ET_EXEC amd64 */
  if(!eh->phnum || eh->phnum > 16 || eh->phentsize != sizeof(Phdr)) return -1;
  if(eh->phoff > size || (u64)eh->phnum * sizeof(Phdr) > size - eh->phoff)
    return -1;
  if(eh->entry < USER_BASE || eh->entry >= USER_TOP) return -1;

  img_lo = (u64)-1; img_hi = 0;
  const Phdr *ph = (const Phdr*)(file + eh->phoff);
  for(int i = 0; i < eh->phnum; i++){
    if(ph[i].type != PT_LOAD) continue;
    u64 va = ph[i].vaddr, fsz = ph[i].filesz, msz = ph[i].memsz;
    if(!msz) continue;      /* ld emits empty RW LOADs for .data-less files */
    if(fsz > msz || msz > (32ULL<<20)) return -1;
    if(va < USER_BASE || va >= USTACK_LO - 4096 ||
       msz > USTACK_LO - 4096 - va) return -1;            /* keep off the stack */
    if(ph[i].off > size || fsz > size - ph[i].off) return -1;
    int w = !!(ph[i].flags & PF_W), x = !!(ph[i].flags & PF_X);
    if(w && x) return -1;                                 /* W^X, no exceptions */
    if(!load_segment(va, file + ph[i].off, fsz, msz, w, x)) return -2;
  }
  if(img_hi == 0) return -1;                              /* no PT_LOAD at all */

  for(u64 page = USTACK_LO; page < USER_STACK_TOP; page += 4096){
    u8 *pa = kalloc_page();
    if(!pa || !vm_user_map(page, (u64)pa, 1, 0)) return -2;
  }

  con_cursor(0);                       /* the program owns the screen now */
  *exit_code = user_enter(eh->entry, USER_STACK_TOP - 16);
  con_cursor(1);

  vm_user_unmap_range(img_lo, img_hi - img_lo);
  vm_user_unmap_range(USTACK_LO, USTACK_PAGES*4096ULL);
  return 0;
}

/* -1 = no such program (silently, the shell owns that message); anything in
 * the pak is handled here, errors included, and returns 0 */
int proc_exec(const char *name){
  const PakEnt *e = pak_find(name);
  if(!e) return -1;

  int code = 0, r = -1;
  if(e->off <= pak_size && e->size <= pak_size - e->off)
    r = elf_run((const u8*)pak + e->off, e->size, &code);
  if(r < 0){
    con_fg(C_LRED); con_puts("  can't run "); con_puts(name);
    con_fg(C_DGREY); con_puts(r == -2 ? "  (out of memory)\n" : "  (bad ELF)\n");
    return 0;                          /* handled: message already printed */
  }
  con_fg(C_LGREY);                     /* whatever colors it left behind */
  if(code >= 0x180){                   /* 0x180 + vec = killed by a fault */
    /* proc_fault already printed the details */
  } else if(code){
    con_fg(C_DGREY); con_puts("  exit code "); con_put_u64(code); con_putc('\n');
  }
  return 0;
}

/* ---- user faults ---------------------------------------------------------- */
void proc_fault(u64 vec, u64 rip){
  con_fg(C_LRED);  con_puts("\n  process killed: ");
  con_fg(C_WHITE); con_puts(exc_name(vec));
  con_fg(C_LGREY); con_puts("  rip="); con_put_hex(rip);
  if(vec == 14){
    u64 cr2; __asm__ __volatile__("mov %%cr2,%0":"=r"(cr2));
    con_puts("  addr="); con_put_hex(cr2);
  }
  con_putc('\n');
  user_abort(0x180 + vec);
}

/* ---- syscalls ------------------------------------------------------------- */
u64 syscall_dispatch(u64 nr, u64 a, u64 b, u64 c){
  (void)c;
  switch(nr){
    case SYS_EXIT:
      user_abort(a & 0xFF);            /* never returns */
      return 0;
    case SYS_WRITE: {
      if(b > 65536 || !vm_user_ok(a, b)) return (u64)-1;
      const char *s = (const char*)a;
      for(u64 i = 0; i < b; i++){
        char ch = s[i];
        if(ch == '\n' || ch == '\t' || ch == '\b' ||
           (ch >= 0x20 && ch <= 0x7E)) con_putc(ch);
      }
      return b;
    }
    case SYS_GETKEY:
      return a ? (u64)kbd_getc() : (u64)(s64)kbd_trygetc();
    case SYS_SLEEP: {
      if(a > 60000) a = 60000;                    /* keep hangs debuggable */
      u64 until = ticks + (a + 9) / 10;
      while(ticks < until) hlt();
      return 0;
    }
    case SYS_MS:      return ticks * 10;
    case SYS_COLOR:   con_color(a & 15, b & 15); return 0;
    case SYS_SETXY:   con_setxy((u32)a, (u32)b); return 0;
    case SYS_CONSIZE: return ((u64)con_cols() << 16) | con_rows();
    case SYS_CLEAR:   con_clear(); return 0;
    case SYS_CURSOR:  con_cursor(a != 0); return 0;
  }
  return (u64)-1;
}

/* ---- init ------------------------------------------------------------------ */
extern char sstack_top[];              /* entry.S */
void syscall_entry(void);

void proc_init(SbosBootInfo *bi){
  pak_init(bi);
  set_rsp0((u64)sstack_top);
  wrmsr(0xC0000080, rdmsr(0xC0000080) | 1);            /* EFER.SCE */
  wrmsr(0xC0000081, (0x13ULL << 48) | (0x08ULL << 32)); /* STAR: CS pairs */
  wrmsr(0xC0000082, (u64)syscall_entry);               /* LSTAR */
  wrmsr(0xC0000084, 0x700);                            /* FMASK: IF|DF|TF off */
  if(pak){
    klog_ok("ring 3 armed: syscalls + user paging");
  } else {
    klog_tagged(" INFO ", C_DGREY, "no BIN.PAK - shell builtins only");
  }
}
