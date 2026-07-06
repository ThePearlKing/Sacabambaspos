/* paging.c - the kernel's own 4-level page tables.
 *
 * Until this runs we live on whatever tables the firmware left behind -
 * identity-mapped, but with unknown permissions (entry.S clears CR0.WP to
 * survive them). Here we build our own:
 *
 *   - identity map of the first 4 GiB with 2 MiB pages (covers legacy MMIO,
 *     LAPIC, nearly all PCI BARs), plus explicit mappings for anything the
 *     boot info points above 4 GiB (heap, framebuffer, kernel image on
 *     large-RAM boxes) - vm_ensure_identity() adds more at runtime (xHCI).
 *   - the kernel image gets real W^X: .text RX, .rodata RO+NX, .data/.bss
 *     RW+NX, with the containing 2 MiB pages split into 4 KiB entries.
 *   - everything else is RW+NX; CR0.WP goes back on, EFER.NXE too (when the
 *     CPU has NX - pre-2004 parts without it just skip the NX bits).
 *   - ring-3 window at USER_BASE: vm_user_map() sets U at every level, and
 *     since no kernel mapping ever carries U, user code can't even read
 *     kernel memory, let alone write it.
 *
 * Page-table pages come from kalloc_page() (kernel heap, never freed);
 * a full 4 GiB identity map costs 4 PDs + 1 PDPT + 1 PML4 = 24 KiB. */
#include "kernel.h"

#define PTE_P   (1ULL<<0)
#define PTE_W   (1ULL<<1)
#define PTE_U   (1ULL<<2)
#define PTE_PS  (1ULL<<7)
#define PTE_NXB (1ULL<<63)
#define PTE_ADDR 0x000FFFFFFFFFF000ULL

extern char __text_start[], __text_end[], __rodata_start[], __rodata_end[],
            __data_start[], __kernel_end[];

static u64 *pml4;
static int nx_ok;

static u64 nx(void){ return nx_ok ? PTE_NXB : 0; }

/* walk one level down, allocating if absent. `user` adds U to the entry
 * (needed at EVERY level for ring-3 access); kernel walks never set it. */
static u64 *next_level(u64 *tab, u32 idx, int user){
  if(!(tab[idx] & PTE_P)){
    u64 *n = kalloc_page();
    if(!n) return NULL;
    tab[idx] = (u64)n | PTE_P | PTE_W | (user ? PTE_U : 0);
  } else if(tab[idx] & PTE_PS){
    return NULL;                        /* huge page here: caller splits */
  } else if(user){
    tab[idx] |= PTE_U;
  }
  return (u64*)(tab[idx] & PTE_ADDR);
}

static int map_2m(u64 va, u64 pa, u64 flags){
  u64 *pdpt = next_level(pml4, (va>>39)&511, 0); if(!pdpt) return 0;
  u64 *pd   = next_level(pdpt, (va>>30)&511, 0); if(!pd)   return 0;
  pd[(va>>21)&511] = pa | PTE_PS | flags;
  return 1;
}

/* point at the PT for va, splitting a 2 MiB mapping into 4 KiB entries
 * with identical permissions if one is in the way */
static u64 *pt_for(u64 va, int user){
  u64 *pdpt = next_level(pml4, (va>>39)&511, user); if(!pdpt) return NULL;
  u64 *pd   = next_level(pdpt, (va>>30)&511, user); if(!pd)   return NULL;
  u32 di = (va>>21)&511;
  if((pd[di] & PTE_P) && (pd[di] & PTE_PS)){
    u64 *pt = kalloc_page();
    if(!pt) return NULL;
    u64 base  = pd[di] & PTE_ADDR;
    u64 flags = pd[di] & ~(PTE_ADDR | PTE_PS);
    for(u64 i=0; i<512; i++) pt[i] = (base + (i<<12)) | flags;
    pd[di] = (u64)pt | PTE_P | PTE_W | (user ? PTE_U : 0);
  }
  return next_level(pd, di, user);
}

static int map_4k(u64 va, u64 pa, u64 flags, int user){
  u64 *pt = pt_for(va, user);
  if(!pt) return 0;
  pt[(va>>12)&511] = pa | flags;
  return 1;
}

/* re-permission an identity-mapped kernel range with 4 KiB granularity */
static int protect(u64 start, u64 end, u64 flags){
  start &= ~4095ULL; end = (end + 4095) & ~4095ULL;
  for(u64 a = start; a < end; a += 4096)
    if(!map_4k(a, a, flags, 0)) return 0;
  return 1;
}

void vm_ensure_identity(u64 base, u64 len){
  if(!pml4 || !len) return;
  u64 a = base & ~0x1FFFFFULL, end = base + len;
  for(; a < end; a += 0x200000){
    /* leave already-present mappings alone (they may be split/protected) */
    u64 *pdpt = next_level(pml4, (a>>39)&511, 0); if(!pdpt) return;
    u64 *pd   = next_level(pdpt, (a>>30)&511, 0); if(!pd)   return;
    if(pd[(a>>21)&511] & PTE_P) continue;
    pd[(a>>21)&511] = a | PTE_PS | PTE_P | PTE_W | nx();
  }
  __asm__ __volatile__("mov %%cr3,%%rax; mov %%rax,%%cr3" ::: "rax");
}

int vm_user_map(u64 va, u64 pa, int writable, int executable){
  if(va < USER_BASE || va >= USER_TOP) return 0;
  u64 f = PTE_P | PTE_U | (writable ? PTE_W : 0) | (executable ? 0 : nx());
  return map_4k(va, pa, f, 1);
}

void vm_user_unmap_range(u64 va, u64 len){
  u64 end = va + len;
  for(va &= ~4095ULL; va < end; va += 4096){
    u64 *pdpt = (u64*)(pml4[(va>>39)&511] & PTE_ADDR); if(!pdpt) continue;
    u64 e = pdpt[(va>>30)&511]; if(!(e & PTE_P)) continue;
    u64 *pd = (u64*)(e & PTE_ADDR);
    e = pd[(va>>21)&511]; if(!(e & PTE_P) || (e & PTE_PS)) continue;
    ((u64*)(e & PTE_ADDR))[(va>>12)&511] = 0;
  }
  __asm__ __volatile__("mov %%cr3,%%rax; mov %%rax,%%cr3" ::: "rax");
}

int paging_init(SbosBootInfo *bi){
  u32 a, b, c, d;
  cpuid(0x80000000u, &a, &b, &c, &d);
  if(a >= 0x80000001u){
    cpuid(0x80000001u, &a, &b, &c, &d);
    nx_ok = (d >> 20) & 1;
  }

  pml4 = kalloc_page();
  if(!pml4) return 0;

  /* first 4 GiB, identity, 2 MiB pages, RW + NX */
  for(u64 p = 0; p < 0x100000000ULL; p += 0x200000)
    if(!map_2m(p, p, PTE_P | PTE_W | nx())) return 0;

  /* anything the boot info references that might sit above 4 GiB. The pml4
   * isn't live yet, so add entries directly (vm_ensure_identity would try
   * to flush a CR3 we haven't loaded - harmless reload of the firmware
   * tables, but do it the plain way for clarity). */
  struct { u64 base, len; } extra[] = {
    { bi->heap_base,   bi->heap_size },
    { bi->fb_base,     (u64)bi->fb_pitch * bi->fb_height * 4 },
    { bi->splash_base, (u64)bi->fb_width * bi->fb_height * 4 },
    { bi->initrc_base, bi->initrc_size },
    { (u64)__text_start, (u64)(__kernel_end - __text_start) },
    { rdmsr(0x1B) & PTE_ADDR, 0x1000 },          /* LAPIC MMIO */
  };
  for(unsigned i = 0; i < sizeof(extra)/sizeof(extra[0]); i++){
    if(!extra[i].base || !extra[i].len) continue;
    u64 s = extra[i].base & ~0x1FFFFFULL, e = extra[i].base + extra[i].len;
    for(; s < e; s += 0x200000)
      if(s >= 0x100000000ULL && !map_2m(s, s, PTE_P | PTE_W | nx())) return 0;
  }

  /* kernel W^X (runtime addresses: PIE symbols resolve RIP-relative) */
  if(!protect((u64)__text_start,   (u64)__text_end,   PTE_P))                 return 0;
  if(!protect((u64)__rodata_start, (u64)__rodata_end, PTE_P | nx()))          return 0;
  if(!protect((u64)__data_start,   (u64)__kernel_end, PTE_P | PTE_W | nx()))  return 0;

  if(nx_ok) wrmsr(0xC0000080, rdmsr(0xC0000080) | (1ULL<<11));   /* EFER.NXE */
  __asm__ __volatile__("mov %0, %%cr3" :: "r"((u64)pml4) : "memory");
  /* firmware tables may have used global pages, which survive a CR3 load -
   * clearing CR4.PGE flushes those too. Also drop SMAP if the firmware set
   * it: the kernel reads/writes user buffers directly (syscalls, exec). */
  u64 cr4; __asm__ __volatile__("mov %%cr4,%0":"=r"(cr4));
  __asm__ __volatile__("mov %0,%%cr4" :: "r"(cr4 & ~((1ULL<<7)|(1ULL<<21))));
  u64 cr0; __asm__ __volatile__("mov %%cr0,%0":"=r"(cr0));
  __asm__ __volatile__("mov %0,%%cr0" :: "r"(cr0 | (1ULL<<16)));  /* WP on */
  return 1;
}

/* is [va, va+len) fully mapped ring-3 accessible? Syscalls call this before
 * touching any user-supplied buffer, so a bad pointer becomes an error code
 * instead of a kernel page fault. */
int vm_user_ok(u64 va, u64 len){
  if(!len) return 1;
  if(va < USER_BASE || va >= USER_TOP || len > USER_TOP - va) return 0;
  for(u64 a = va & ~4095ULL; a < va + len; a += 4096){
    u64 *pdpt = (u64*)(pml4[(a>>39)&511] & PTE_ADDR); if(!pdpt) return 0;
    u64 e = pdpt[(a>>30)&511]; if(!(e & PTE_P)) return 0;
    u64 *pd = (u64*)(e & PTE_ADDR);
    e = pd[(a>>21)&511]; if(!(e & PTE_P) || (e & PTE_PS)) return 0;
    e = ((u64*)(e & PTE_ADDR))[(a>>12)&511];
    if(!(e & PTE_P) || !(e & PTE_U)) return 0;
  }
  return 1;
}
