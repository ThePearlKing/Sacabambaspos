/* usb.c - from-scratch xHCI host driver + USB HID boot-protocol keyboard.
 *
 * Why this exists: kbd.c only talks to the 8042. Boards shipped in the last
 * decade increasingly have no 8042 and no SMM legacy emulation - the status
 * port floats 0xFF and the machine simply has no keyboard without a real USB
 * stack. This is that stack, cut down to exactly what a shell needs.
 *
 * Design rules, same spirit as kbd.c:
 *  - Polled, never interrupt-driven. The event ring is drained from
 *    usbkbd_poll(), which kbd_getc() calls on every wakeup; the PIT's 100 Hz
 *    hlt-wakeup bounds key latency at ~10 ms. No MSI setup, no IOAPIC
 *    routing, nothing that varies per board can kill input.
 *  - Every hardware wait is bounded by the PIT tick counter; a wedged
 *    controller degrades to "no USB keyboard", never to a hang.
 *  - Single-threaded by construction: init and poll both run on the one
 *    kernel thread, so rings and the one-command-at-a-time waiter need no
 *    locking. Nothing here may be called from an IRQ handler.
 *
 * Coverage: any HID keyboard that does boot protocol (the USB spec makes
 * that mandatory for keyboards) on any root port of any xHCI controller,
 * incl. hotplug after boot, plus keyboards behind one tier of external
 * USB2 hub (monitor/dock case; hub ports are rescanned every ~1.5 s).
 * Composite devices (kbd+mouse combos) work: every interface is searched.
 * Memory comes from the bump kalloc and is never freed; a plug/unplug
 * cycle leaks ~12 KiB of a 64 MiB heap - acceptable until a real
 * allocator lands in stage 2. */
#include "kernel.h"

/* ---- PCI config space (legacy 0xCF8 mechanism, present on every x86) ---- */
static u32 pci_r(u32 b, u32 d, u32 f, u32 off){
  outl(0xCF8, 0x80000000u | b<<16 | d<<11 | f<<8 | (off & 0xFC));
  return inl(0xCFC);
}
static void pci_w(u32 b, u32 d, u32 f, u32 off, u32 v){
  outl(0xCF8, 0x80000000u | b<<16 | d<<11 | f<<8 | (off & 0xFC));
  outl(0xCFC, v);
}

/* ---- MMIO (32-bit accesses only; the spec allows them on 64-bit regs) ---- */
static inline u32  rr(volatile u8 *a){ return *(volatile u32*)a; }
static inline void rw(volatile u8 *a, u32 v){ *(volatile u32*)a = v; }
static inline void rw64(volatile u8 *a, u64 v){ rw(a, (u32)v); rw(a+4, (u32)(v>>32)); }
#define barrier() __asm__ __volatile__("":::"memory")

/* identity-mapped, so virt == phys */
static void *dma_alloc(u64 n, u64 align){
  u8 *p = kalloc(n + align);
  if(!p) return NULL;
  return (void*)(((u64)p + align - 1) & ~(align - 1));
}

/* interrupts must be on (PIT drives ticks); callers guarantee that */
static void sleep_ms(u32 ms){
  u64 end = ticks + ms/10 + 2;
  while(ticks < end) hlt();
}

/* ---- rings -------------------------------------------------------------- */
typedef struct { u64 p; u32 s, c; } Trb;
#define RING_TRBS 256                    /* 4 KiB, the no-64K-crossing size */

typedef struct { Trb *t; u64 phys; u32 enq, cyc; } Ring;

static int ring_init(Ring *r){
  r->t = dma_alloc(4096, 4096);
  if(!r->t) return 0;
  r->phys = (u64)r->t; r->enq = 0; r->cyc = 1;
  return 1;
}

/* producer push; cycle bit written last so the HC never sees a half TRB */
static Trb *ring_push(Ring *r, u64 p, u32 s, u32 ctl){
  if(r->enq == RING_TRBS-1){             /* wrap via link TRB, toggle cycle */
    Trb *l = &r->t[r->enq];
    l->p = r->phys; l->s = 0;
    barrier();
    l->c = (6u<<10) | 2u | r->cyc;       /* Link, Toggle Cycle */
    r->enq = 0; r->cyc ^= 1;
  }
  Trb *t = &r->t[r->enq++];
  t->p = p; t->s = s;
  barrier();
  t->c = (ctl & ~1u) | r->cyc;
  return t;
}

/* ---- controller / device state ------------------------------------------ */
typedef struct UsbDev UsbDev;
struct UsbDev {
  u8 used, slot, speed, rootport;        /* speed: xHCI code FS1 LS2 HS3 SS4 */
  u32 route;
  UsbDev *parent;                        /* hub above, NULL = on root port */
  u8 *octx;                              /* output device context */
  Ring ep0;
  u16 mps0;
  /* keyboard */
  u8 is_kbd, dci, need_recover;
  Ring epi;
  u8 *rbuf; u16 rlen;
  u8 prev[8];                            /* last boot report */
  /* hub */
  u8 is_hub, nports;
  u32 conn;                              /* ports we believe are attached */
};

#define MAX_CTRL 4
#define MAX_DEVS 16

typedef struct {
  volatile u8 *base, *op, *rt, *db;
  u32 csz, slots, ports, pgsz;
  u64 *dcbaa;
  u8 *ictx;                              /* scratch input context */
  u8 *xbuf;                              /* scratch control-transfer buffer */
  Ring cmd;
  Trb *ev; u64 ev_phys; u32 ev_deq, ev_cyc;
  u64 *erst;
  u8 alive;
  u8 pmajor[256];                        /* root port (1-based) -> USB major */
  u8 pscan[256];                         /* root ports flagged for (re)scan */
  UsbDev dev[MAX_DEVS];
  /* the single outstanding waiter (one command/transfer at a time) */
  volatile u8 wdone;
  u8 wt, wslot, wdci, wcc, wslotout;     /* wt: 0 none, 1 command, 2 transfer */
  u64 wp;
  u64 hubscan_at;
} Xhci;

static Xhci hcs[MAX_CTRL];
static int nhc;
static int usb_ready;

/* ---- decoded-key queue (what kbd_getc drains) ---------------------------- */
#define KQ 64
static u16 kq[KQ];
static u32 kq_r, kq_w;
static void kq_push(int c){
  u32 n = (kq_w + 1) % KQ;
  if(n != kq_r){ kq[kq_w] = (u16)c; kq_w = n; }
}
int usbkbd_pop(void){
  if(kq_r == kq_w) return -1;
  int c = kq[kq_r]; kq_r = (kq_r + 1) % KQ;
  return c;
}

/* USB reports state, not repeats - typematic is ours: 500 ms, then ~33 cps */
static u8  rep_usage;
static int rep_key;
static u64 rep_next;
static int usb_caps;

/* HID usage -> key, US layout (boot protocol usage page 7) */
static int usage_to_key(u8 u, int shift){
  if(u >= 0x04 && u <= 0x1D){                       /* a-z */
    char c = 'a' + (u - 0x04);
    if(shift ^ usb_caps) c += 'A'-'a';
    return c;
  }
  if(u >= 0x1E && u <= 0x27)                        /* top row digits */
    return shift ? "!@#$%^&*()"[u-0x1E] : "1234567890"[u-0x1E];
  switch(u){
    case 0x28: return '\n';  case 0x29: return 27;   case 0x2A: return '\b';
    case 0x2B: return '\t';  case 0x2C: return ' ';
    case 0x2D: return shift ? '_' : '-';  case 0x2E: return shift ? '+' : '=';
    case 0x2F: return shift ? '{' : '[';  case 0x30: return shift ? '}' : ']';
    case 0x31: return shift ? '|' : '\\'; case 0x32: return shift ? '~' : '#';
    case 0x33: return shift ? ':' : ';';  case 0x34: return shift ? '"' : '\'';
    case 0x35: return shift ? '~' : '`';  case 0x36: return shift ? '<' : ',';
    case 0x37: return shift ? '>' : '.';  case 0x38: return shift ? '?' : '/';
    case 0x4F: return KEY_RIGHT; case 0x50: return KEY_LEFT;
    case 0x51: return KEY_DOWN;  case 0x52: return KEY_UP;
    case 0x54: return '/'; case 0x55: return '*';
    case 0x56: return '-'; case 0x57: return '+'; case 0x58: return '\n';
    case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D:
    case 0x5E: case 0x5F: case 0x60: case 0x61:
      return "123456789"[u-0x59];                   /* keypad, numlock-on */
    case 0x62: return '0'; case 0x63: return '.';
  }
  return 0;                                         /* F-keys, GUI, etc */
}

static void kbd_report(UsbDev *d){
  u8 *r = d->rbuf;
  /* [0] modifiers, [1] reserved, [2..7] held usages */
  for(int i = 2; i < 8; i++){
    u8 u = r[i];
    if(!u) continue;
    if(u <= 3) return;                  /* rollover/error report: keep prev */
    int held = 0;
    for(int j = 2; j < 8; j++) if(d->prev[j] == u) held = 1;
    if(held) continue;                  /* only newly-pressed keys emit */
    if(u == 0x39){ usb_caps = !usb_caps; continue; }
    int k = usage_to_key(u, (r[0] & 0x22) != 0);
    if(!k) continue;
    kq_push(k);
    rep_usage = u; rep_key = k; rep_next = ticks + 50;
  }
  if(rep_usage){                        /* repeat dies with its key */
    int still = 0;
    for(int j = 2; j < 8; j++) if(r[j] == rep_usage) still = 1;
    if(!still) rep_usage = 0;
  }
  memcpy(d->prev, r, 8);
}

/* ---- event ring ----------------------------------------------------------- */
static UsbDev *dev_by_slot(Xhci *x, u8 slot){
  for(int i = 0; i < MAX_DEVS; i++)
    if(x->dev[i].used && x->dev[i].slot == slot) return &x->dev[i];
  return NULL;
}

/* queue one interrupt-IN TRB; the HC NAK-polls the device until it has a
 * report, so a single outstanding TRB never loses keys, only delays them
 * to the next poll */
static void kbd_arm(Xhci *x, UsbDev *d){
  memset(d->rbuf, 0, 64);
  ring_push(&d->epi, (u64)d->rbuf, d->rlen, (1u<<10) | (1u<<5) | (1u<<2));
  rw(x->db + 4*d->slot, d->dci);
}

static void handle_event(Xhci *x, Trb *e){
  u32 type = (e->c >> 10) & 0x3F;
  if(type == 33){                                    /* command completion */
    if(x->wt == 1 && e->p == x->wp){
      x->wcc = (u8)(e->s >> 24); x->wslotout = (u8)(e->c >> 24); x->wdone = 1;
    }
  } else if(type == 32){                             /* transfer event */
    u8 slot = (u8)(e->c >> 24), dci = (e->c >> 16) & 0x1F, cc = (u8)(e->s >> 24);
    UsbDev *d = dev_by_slot(x, slot);
    if(d && d->is_kbd && dci == d->dci){
      if(cc == 1 || cc == 13){ kbd_report(d); kbd_arm(x, d); }  /* ok/short */
      else d->need_recover = 1;         /* recover at top level, not here -
                                         * we may be inside a cmd wait */
      return;
    }
    if(x->wt == 2 && slot == x->wslot && dci == x->wdci){
      x->wcc = cc; x->wdone = 1;
    }
  } else if(type == 34){                             /* port status change */
    u32 port = e->p >> 24;
    if(port >= 1 && port <= 255) x->pscan[port] = 1;
  }                                                  /* others: ignore */
}

static int poll_events(Xhci *x){
  int n = 0;
  for(;;){
    volatile Trb *e = (volatile Trb*)&x->ev[x->ev_deq];
    if((e->c & 1) != x->ev_cyc) break;
    barrier();
    Trb ev = { e->p, e->s, e->c };
    handle_event(x, &ev);
    if(++x->ev_deq == RING_TRBS){ x->ev_deq = 0; x->ev_cyc ^= 1; }
    if(++n > 512) break;                             /* runaway guard */
  }
  if(n) rw64(x->rt + 0x38, (x->ev_phys + (u64)x->ev_deq*16) | 8);  /* ERDP+EHB */
  return n;
}

/* wait for the armed waiter; bounded, drains events meanwhile */
static int wait_done(Xhci *x, u32 ms){
  u64 end = ticks + ms/10 + 2;
  while(!x->wdone && ticks < end){
    poll_events(x);
    __asm__ __volatile__("pause");
  }
  int ok = x->wdone;
  x->wt = 0; x->wdone = 0;
  return ok ? x->wcc : -1;
}

/* run one command TRB; returns completion code (1 = success), -1 = timeout */
static int cmd_exec(Xhci *x, u64 param, u32 ctl, u8 *slot_out){
  Trb *t = ring_push(&x->cmd, param, 0, ctl);
  x->wt = 1; x->wp = (u64)t; x->wdone = 0;
  rw(x->db, 0);
  int cc = wait_done(x, 1500);
  if(slot_out) *slot_out = x->wslotout;
  return cc;
}

/* control transfer on EP0; returns completion code, -1 = timeout */
static int ctrl_xfer(Xhci *x, UsbDev *d, u8 bm, u8 req, u16 val, u16 idx,
                     void *buf, u16 len){
  u64 setup = bm | (u64)req<<8 | (u64)val<<16 | (u64)idx<<32 | (u64)len<<48;
  u32 trt = len ? ((bm & 0x80) ? 3u : 2u) : 0;
  ring_push(&d->ep0, setup, 8, (2u<<10) | (1u<<6) | (trt<<16));
  if(len)
    ring_push(&d->ep0, (u64)buf, len, (3u<<10) | ((u32)(bm>>7)<<16));
  u32 stdir = (len && (bm & 0x80)) ? 0 : 1u;         /* status opposes data */
  ring_push(&d->ep0, 0, 0, (4u<<10) | (1u<<5) | (stdir<<16));
  x->wt = 2; x->wslot = d->slot; x->wdci = 1; x->wdone = 0;
  rw(x->db + 4*d->slot, 1);
  return wait_done(x, 1500);
}

/* ---- contexts -------------------------------------------------------------- */
static u32 *ictx_ctl (Xhci *x){ return (u32*)x->ictx; }
static u32 *ictx_slot(Xhci *x){ return (u32*)(x->ictx + x->csz); }
static u32 *ictx_ep  (Xhci *x, u32 dci){ return (u32*)(x->ictx + x->csz*(1+dci)); }

/* LS/FS bInterval is in ms, HS/SS is an exponent; xHCI wants log2 µframes */
static u32 ep_interval(u8 speed, u8 bi){
  if(speed >= 3){ u32 v = bi ? bi-1 : 0; return v > 15 ? 15 : v; }
  u32 f = (bi ? bi : 8) * 8, n = 3;
  while((1u << (n+1)) <= f && n < 10) n++;
  return n;
}

/* ---- device enumeration ----------------------------------------------------- */
static void hub_setup(Xhci *x, UsbDev *d);

typedef struct { u8 iface, ep, interval; u16 mps; } KbdIf;

/* walk a configuration descriptor for a HID keyboard interface (class 3,
 * protocol 1; subclass not required - some keyboards report 0) and its
 * interrupt-IN endpoint */
static int find_kbd_if(u8 *b, u32 tot, KbdIf *k){
  u32 i = 0; int cur = -1;
  while(i + 1 < tot){
    u8 l = b[i], t = b[i+1];
    if(l < 2 || i + l > tot) break;
    if(t == 4)                                       /* interface */
      cur = (b[i+5] == 3 && b[i+7] == 1 && b[i+3] == 0) ? (int)b[i+2] : -1;
    else if(t == 5 && cur >= 0 && (b[i+2] & 0x80) && (b[i+3] & 3) == 3){
      k->iface = (u8)cur; k->ep = b[i+2] & 0xF;
      k->mps = (u16)(b[i+4] | (b[i+5] << 8)); k->interval = b[i+6];
      return 1;
    }
    i += l;
  }
  return 0;
}

static UsbDev *dev_alloc(Xhci *x){
  for(int i = 0; i < MAX_DEVS; i++)
    if(!x->dev[i].used){ memset(&x->dev[i], 0, sizeof(UsbDev)); return &x->dev[i]; }
  return NULL;
}

static void dev_drop(Xhci *x, UsbDev *d){
  if(d->slot){
    cmd_exec(x, 0, (10u<<10) | ((u32)d->slot<<24), NULL);   /* Disable Slot */
    x->dcbaa[d->slot] = 0;
  }
  if(rep_usage && d->is_kbd) rep_usage = 0;
  d->used = 0;
}

static int enum_device(Xhci *x, u8 rootport, u32 route, u8 speed,
                       UsbDev *parent, u8 parentport){
  UsbDev *d = dev_alloc(x);
  if(!d) return 0;
  d->used = 1; d->speed = speed; d->rootport = rootport;
  d->route = route; d->parent = parent;

  u8 slot = 0;
  if(cmd_exec(x, 0, 9u<<10, &slot) != 1 || !slot || slot > x->slots){
    d->used = 0; return 0;
  }
  d->slot = slot;
  d->octx = dma_alloc((u64)x->csz * 32, 64);
  if(!d->octx || !ring_init(&d->ep0)){ dev_drop(x, d); return 0; }
  x->dcbaa[slot] = (u64)d->octx;

  d->mps0 = speed == 3 ? 64 : speed >= 4 ? 512 : 8;

  /* Address Device: slot + EP0 contexts */
  memset(x->ictx, 0, (u64)x->csz * 33);
  ictx_ctl(x)[1] = 3;                                /* add A0|A1 */
  u32 *sl = ictx_slot(x);
  sl[0] = route | ((u32)speed << 20) | (1u << 27);
  sl[1] = (u32)rootport << 16;
  if(parent && parent->speed == 3 && (speed == 1 || speed == 2))
    sl[2] = parent->slot | ((u32)parentport << 8);   /* TT for LS/FS below HS hub */
  u32 *ep = ictx_ep(x, 1);
  ep[1] = (3u<<1) | (4u<<3) | ((u32)d->mps0 << 16);  /* CErr 3, control */
  ep[2] = (u32)(d->ep0.phys | 1); ep[3] = (u32)((d->ep0.phys | 1) >> 32);
  ep[4] = 8;
  if(cmd_exec(x, (u64)x->ictx, (11u<<10) | ((u32)slot<<24), NULL) != 1){
    dev_drop(x, d); return 0;
  }

  /* descriptors; learn EP0's real max packet first (LS/FS guess is 8) */
  u8 *b = x->xbuf;
  if(ctrl_xfer(x, d, 0x80, 6, 0x0100, 0, b, 8) != 1){ dev_drop(x, d); return 0; }
  u16 mps = speed >= 4 ? (u16)(1u << b[7]) : b[7];
  if(mps >= 8 && mps != d->mps0){
    d->mps0 = mps;
    memset(x->ictx, 0, (u64)x->csz * 33);
    ictx_ctl(x)[1] = 2;                              /* A1 only */
    ep = ictx_ep(x, 1);
    ep[1] = (3u<<1) | (4u<<3) | ((u32)mps << 16);
    ep[2] = 0; ep[3] = 0; ep[4] = 8;                 /* keep dequeue as-is */
    cmd_exec(x, (u64)x->ictx, (13u<<10) | ((u32)slot<<24), NULL);
  }
  if(ctrl_xfer(x, d, 0x80, 6, 0x0100, 0, b, 18) != 1){ dev_drop(x, d); return 0; }
  u8 devclass = b[4];

  if(ctrl_xfer(x, d, 0x80, 6, 0x0200, 0, b, 9) != 1){ dev_drop(x, d); return 0; }
  u32 tot = (u32)(b[2] | (b[3] << 8));
  if(tot < 9) { dev_drop(x, d); return 0; }
  if(tot > 512) tot = 512;
  if(ctrl_xfer(x, d, 0x80, 6, 0x0200, 0, b, (u16)tot) != 1){ dev_drop(x, d); return 0; }
  u8 cfgval = b[5];

  KbdIf k = {0, 0, 0, 0};
  int has_kbd = find_kbd_if(b, tot, &k);

  if(ctrl_xfer(x, d, 0x00, 9, cfgval, 0, NULL, 0) != 1){ dev_drop(x, d); return 0; }

  if(devclass == 9){                                 /* hub */
    if(!parent) hub_setup(x, d);   /* one tier only: hubs under hubs skipped */
    return 1;
  }
  if(!has_kbd) return 1;           /* not a keyboard (mouse, storage, ...) */

  /* boot protocol + no idle reports; failures tolerated (report layout is
   * boot-compatible on most keyboards regardless) */
  ctrl_xfer(x, d, 0x21, 0x0B, 0, k.iface, NULL, 0);
  ctrl_xfer(x, d, 0x21, 0x0A, 0, k.iface, NULL, 0);

  d->dci = (u8)(k.ep*2 + 1);
  if(!ring_init(&d->epi)) return 1;
  d->rbuf = dma_alloc(64, 64);
  if(!d->rbuf) return 1;
  d->rlen = k.mps < 8 ? 8 : (k.mps > 64 ? 64 : k.mps);

  /* Configure Endpoint: add the interrupt-IN pipe */
  memset(x->ictx, 0, (u64)x->csz * 33);
  ictx_ctl(x)[1] = 1u | (1u << d->dci);
  sl = ictx_slot(x);
  u32 *os = (u32*)d->octx;
  sl[0] = (os[0] & ~(0x1Fu << 27)) | ((u32)d->dci << 27);
  sl[1] = os[1]; sl[2] = os[2]; sl[3] = os[3];
  ep = ictx_ep(x, d->dci);
  ep[0] = ep_interval(speed, k.interval) << 16;
  ep[1] = (3u<<1) | (7u<<3) | ((u32)k.mps << 16);    /* CErr 3, interrupt IN */
  ep[2] = (u32)(d->epi.phys | 1); ep[3] = (u32)((d->epi.phys | 1) >> 32);
  ep[4] = (u32)k.mps | ((u32)k.mps << 16);
  if(cmd_exec(x, (u64)x->ictx, (12u<<10) | ((u32)slot<<24), NULL) != 1) return 1;

  d->is_kbd = 1;
  kbd_arm(x, d);
  return 1;
}

/* stalled/errored keyboard pipe: reset it and restart the ring */
static void kbd_recover(Xhci *x, UsbDev *d){
  d->need_recover = 0;
  if(cmd_exec(x, 0, (14u<<10) | ((u32)d->dci<<16) | ((u32)d->slot<<24), NULL) < 0)
    { d->is_kbd = 0; return; }
  u64 deq = (u64)&d->epi.t[d->epi.enq] | d->epi.cyc;
  if(cmd_exec(x, deq, (16u<<10) | ((u32)d->dci<<16) | ((u32)d->slot<<24), NULL) != 1)
    { d->is_kbd = 0; return; }
  kbd_arm(x, d);
}

/* ---- external hub (USB2, one tier) ---------------------------------------- */
static int hub_port_status(Xhci *x, UsbDev *d, u8 port, u16 *st, u16 *ch){
  u8 *b = x->xbuf;
  if(ctrl_xfer(x, d, 0xA3, 0, 0, port, b, 4) != 1) return 0;
  *st = (u16)(b[0] | (b[1] << 8)); *ch = (u16)(b[2] | (b[3] << 8));
  return 1;
}

static void hub_scan(Xhci *x, UsbDev *d){
  for(u8 p = 1; p <= d->nports; p++){
    u16 st, ch;
    if(!hub_port_status(x, d, p, &st, &ch)) return;
    if(ch & 1) ctrl_xfer(x, d, 0x23, 1, 16, p, NULL, 0);  /* ack C_CONNECTION */
    int conn = st & 1, was = (d->conn >> p) & 1;
    if(conn && !was){
      d->conn |= 1u << p;              /* set now: a failed enum won't retry-loop */
      ctrl_xfer(x, d, 0x23, 3, 4, p, NULL, 0);            /* PORT_RESET */
      int ok = 0;
      for(int i = 0; i < 50; i++){
        sleep_ms(10);
        if(!hub_port_status(x, d, p, &st, &ch)) return;
        if(ch & (1u << 4)){ ok = 1; break; }              /* C_PORT_RESET */
      }
      ctrl_xfer(x, d, 0x23, 1, 20, p, NULL, 0);
      if(!ok || !(st & 2)) continue;                      /* not enabled */
      u8 spd = (st & (1u<<9)) ? 2 : (st & (1u<<10)) ? 3 : 1;
      sleep_ms(20);                                       /* reset recovery */
      enum_device(x, d->rootport, p, spd, d, p);
    } else if(!conn && was){
      d->conn &= ~(1u << p);
      for(int i = 0; i < MAX_DEVS; i++){
        UsbDev *c = &x->dev[i];
        if(c->used && c->parent == d && c->route == p) dev_drop(x, c);
      }
    }
  }
}

static void hub_setup(Xhci *x, UsbDev *d){
  u8 *b = x->xbuf;
  if(ctrl_xfer(x, d, 0xA0, 6, 0x2900, 0, b, 9) != 1) return;
  d->nports = b[2] > 15 ? 15 : b[2];   /* route string nibble caps at 15 */
  u32 pwrgood = (u32)b[5] * 2 + 100;

  /* mark the slot as a hub so the HC routes TT traffic through it */
  memset(x->ictx, 0, (u64)x->csz * 33);
  ictx_ctl(x)[1] = 1;
  u32 *sl = ictx_slot(x);
  u32 *os = (u32*)d->octx;
  sl[0] = os[0] | (1u << 26);
  sl[1] = (os[1] & 0x00FFFFFF) | ((u32)d->nports << 24);
  sl[2] = os[2]; sl[3] = os[3];
  if(cmd_exec(x, (u64)x->ictx, (12u<<10) | ((u32)d->slot<<24), NULL) != 1) return;

  for(u8 p = 1; p <= d->nports; p++)
    ctrl_xfer(x, d, 0x23, 3, 8, p, NULL, 0);              /* PORT_POWER */
  sleep_ms(pwrgood);
  d->is_hub = 1;
  hub_scan(x, d);
}

/* ---- root ports ------------------------------------------------------------- */
static UsbDev *dev_on_root(Xhci *x, u8 port){
  for(int i = 0; i < MAX_DEVS; i++)
    if(x->dev[i].used && x->dev[i].rootport == port && x->dev[i].route == 0)
      return &x->dev[i];
  return NULL;
}

/* PORTSC writes: always keep PP (bit 9), never write 1 to PED (bit 1 -
 * that disables the port), only echo change bits (17-23) when clearing */
static void port_scan(Xhci *x, u8 port){
  volatile u8 *pr = x->op + 0x400 + (u32)(port-1)*0x10;
  u32 sc = rr(pr);
  if(sc == 0xFFFFFFFF) return;
  rw(pr, (1u<<9) | (sc & 0x00FE0000));                    /* ack changes */

  if(!(sc & 1)){                                          /* disconnected */
    for(int i = 0; i < MAX_DEVS; i++)
      if(x->dev[i].used && x->dev[i].rootport == port) dev_drop(x, &x->dev[i]);
    return;
  }
  if(dev_on_root(x, port)) return;                        /* already up */

  if(!(sc & (1u<<9))){ rw(pr, 1u<<9); sleep_ms(30); }     /* power the port */
  sleep_ms(60);                                           /* connect debounce */

  u8 major = x->pmajor[port] ? x->pmajor[port] : 2;
  if(major == 2){
    rw(pr, (1u<<9) | (1u<<4));                            /* USB2 needs reset */
    int ok = 0;
    for(int i = 0; i < 60; i++){
      sleep_ms(10);
      sc = rr(pr);
      if(sc & (1u<<21)){ ok = 1; break; }                 /* reset done (PRC) */
    }
    rw(pr, (1u<<9) | (sc & 0x00FE0000));
    if(!ok || !(sc & 2)) return;
  } else {                                                /* USB3 self-trains */
    int ok = 0;
    for(int i = 0; i < 30; i++){
      sc = rr(pr);
      if(sc & 2){ ok = 1; break; }
      sleep_ms(10);
    }
    rw(pr, (1u<<9) | (sc & 0x00FE0000));
    if(!ok) return;
  }
  sleep_ms(10);                                           /* reset recovery */
  enum_device(x, port, 0, (sc >> 10) & 0xF, NULL, 0);
}

/* ---- controller bring-up ---------------------------------------------------- */
/* extended capabilities: take ownership from the BIOS, kill its SMIs, and
 * learn which root ports are USB2 vs USB3 */
static void xhci_extcaps(Xhci *x){
  u32 off = (rr(x->base + 0x10) >> 16) << 2;
  for(int guard = 0; off && guard < 64; guard++){
    u32 cap = rr(x->base + off);
    u8 id = cap & 0xFF;
    if(id == 1){                                          /* USB legacy support */
      if(cap & (1u<<16)){                                 /* BIOS owns it */
        rw(x->base + off, cap | (1u<<24));                /* request ownership */
        u64 end = ticks + 120;
        while((rr(x->base + off) & (1u<<16)) && ticks < end) hlt();
      }
      u32 ctl = rr(x->base + off + 4);
      rw(x->base + off + 4, ctl & 0xE0000000);            /* SMIs off, acks on */
    } else if(id == 2){                                   /* supported protocol */
      u8 major = (u8)(cap >> 24);
      u32 dw2 = rr(x->base + off + 8);
      u32 p0 = dw2 & 0xFF, cnt = (dw2 >> 8) & 0xFF;
      for(u32 i = 0; i < cnt; i++)
        if(p0 + i >= 1 && p0 + i <= 255) x->pmajor[p0 + i] = major;
    }
    u32 nxt = (cap >> 8) & 0xFF;
    off = nxt ? off + (nxt << 2) : 0;
  }
}

static int xhci_wait_clear(volatile u8 *reg, u32 mask, u32 ms){
  u64 end = ticks + ms/10 + 2;
  while(ticks < end){
    if(!(rr(reg) & mask)) return 1;
    hlt();
  }
  return !(rr(reg) & mask);
}

static int xhci_init_one(u64 bar){
  if(nhc >= MAX_CTRL) return 0;
  Xhci *x = &hcs[nhc];
  memset(x, 0, sizeof(*x));
  x->base = (volatile u8*)bar;

  u32 caplen = rr(x->base) & 0xFF;
  x->op = x->base + caplen;
  x->db = x->base + (rr(x->base + 0x14) & ~0x3u);
  x->rt = x->base + (rr(x->base + 0x18) & ~0x1Fu);
  u32 hcs1 = rr(x->base + 4);
  x->slots = hcs1 & 0xFF;
  x->ports = (hcs1 >> 24) & 0xFF;
  x->csz = (rr(x->base + 0x10) & 4) ? 64 : 32;
  if(!x->slots || !x->ports || x->slots > 255) return 0;
  if(x->slots >= MAX_DEVS) x->slots = MAX_DEVS - 1;  /* we track that many */

  xhci_extcaps(x);

  /* halt, then reset; a controller that won't do either is left alone */
  u32 cmd = rr(x->op);
  if(cmd & 1){
    rw(x->op, cmd & ~1u);
    u64 end = ticks + 10;                                 /* HCH within 16 ms */
    while(!(rr(x->op + 4) & 1) && ticks < end) hlt();
  }
  rw(x->op, 2);                                           /* HCRST */
  if(!xhci_wait_clear(x->op, 2, 1000)) return 0;
  if(!xhci_wait_clear(x->op + 4, 1u<<11, 1000)) return 0; /* CNR */
  sleep_ms(10);                                 /* post-reset settle (Intel) */

  /* page size for scratchpads */
  u32 psr = rr(x->op + 8);
  x->pgsz = 4096;
  for(int i = 0; i < 16; i++) if(psr & (1u << i)){ x->pgsz = 1u << (12 + i); break; }

  rw(x->op + 0x38, x->slots);                             /* CONFIG */

  x->dcbaa = dma_alloc(((u64)x->slots + 1) * 8, 64);
  x->ictx  = dma_alloc((u64)x->csz * 33, 64);
  x->xbuf  = dma_alloc(512, 64);
  if(!x->dcbaa || !x->ictx || !x->xbuf) return 0;

  u32 hcs2 = rr(x->base + 8);
  u32 nsp = ((hcs2 >> 21 & 0x1F) << 5) | (hcs2 >> 27 & 0x1F);
  if(nsp){
    u64 *arr = dma_alloc((u64)nsp * 8, 64);
    if(!arr) return 0;
    for(u32 i = 0; i < nsp; i++){
      void *pg = dma_alloc(x->pgsz, x->pgsz);
      if(!pg) return 0;
      arr[i] = (u64)pg;
    }
    x->dcbaa[0] = (u64)arr;
  }
  rw64(x->op + 0x30, (u64)x->dcbaa);                      /* DCBAAP */

  if(!ring_init(&x->cmd)) return 0;
  rw64(x->op + 0x18, x->cmd.phys | 1);                    /* CRCR, RCS=1 */

  x->ev = dma_alloc(4096, 4096);
  x->erst = dma_alloc(16, 64);
  if(!x->ev || !x->erst) return 0;
  x->ev_phys = (u64)x->ev; x->ev_deq = 0; x->ev_cyc = 1;
  x->erst[0] = x->ev_phys;
  ((u32*)x->erst)[2] = RING_TRBS;
  rw(x->rt + 0x28, 1);                                    /* ERSTSZ */
  rw64(x->rt + 0x38, x->ev_phys | 8);                     /* ERDP */
  rw64(x->rt + 0x30, (u64)x->erst);                       /* ERSTBA (latches) */

  rw(x->op, rr(x->op) | 1);                               /* run, INTE off */
  if(!xhci_wait_clear(x->op + 4, 1, 200)) return 0;       /* HCH must drop */

  x->alive = 1;
  x->hubscan_at = (u64)-1;
  nhc++;

  sleep_ms(150);                        /* let connect states settle */
  for(u8 p = 1; p <= (u8)x->ports; p++) port_scan(x, p);
  if(x->hubscan_at == (u64)-1){         /* arm periodic rescan only if hubs */
    for(int i = 0; i < MAX_DEVS; i++)
      if(x->dev[i].used && x->dev[i].is_hub) x->hubscan_at = ticks + 150;
  }
  return 1;
}

/* ---- public API --------------------------------------------------------------- */
static int kbd_count(void){
  int n = 0;
  for(int c = 0; c < nhc; c++)
    for(int i = 0; i < MAX_DEVS; i++)
      if(hcs[c].dev[i].used && hcs[c].dev[i].is_kbd) n++;
  return n;
}

int usbkbd_init(void){
  int found = 0;
  for(u32 bus = 0; bus < 256; bus++)
    for(u32 dv = 0; dv < 32; dv++){
      if((pci_r(bus, dv, 0, 0) & 0xFFFF) == 0xFFFF) continue;
      u32 nf = (pci_r(bus, dv, 0, 0x0C) & (1u << 23)) ? 8 : 1;
      for(u32 fn = 0; fn < nf; fn++){
        if((pci_r(bus, dv, fn, 0) & 0xFFFF) == 0xFFFF) continue;
        if((pci_r(bus, dv, fn, 8) >> 8) != 0x0C0330) continue;   /* xHCI */
        u32 bl = pci_r(bus, dv, fn, 0x10);
        if(bl & 1) continue;                          /* I/O BAR: not xHCI */
        u64 bar = bl & ~0xFULL;
        if(((bl >> 1) & 3) == 2) bar |= (u64)pci_r(bus, dv, fn, 0x14) << 32;
        if(!bar) continue;
        /* memory space + bus master on, INTx off (we poll) */
        pci_w(bus, dv, fn, 4, pci_r(bus, dv, fn, 4) | 0x406);
        found++;
        xhci_init_one(bar);
      }
    }
  usb_ready = nhc > 0;
  return found ? kbd_count() : -1;
}

void usbkbd_poll(void){
  if(!usb_ready) return;
  for(int c = 0; c < nhc; c++){
    Xhci *x = &hcs[c];
    if(!x->alive) continue;
    poll_events(x);
    for(u32 p = 1; p <= x->ports && p < 256; p++)
      if(x->pscan[p]){
        x->pscan[p] = 0;
        port_scan(x, (u8)p);
        if(!x->hubscan_at || x->hubscan_at == (u64)-1) x->hubscan_at = ticks + 150;
      }
    for(int i = 0; i < MAX_DEVS; i++)
      if(x->dev[i].used && x->dev[i].need_recover) kbd_recover(x, &x->dev[i]);
    if(x->hubscan_at != (u64)-1 && ticks >= x->hubscan_at){
      x->hubscan_at = ticks + 150;                    /* hub hotplug: ~1.5 s */
      int hubs = 0;
      for(int i = 0; i < MAX_DEVS; i++)
        if(x->dev[i].used && x->dev[i].is_hub){ hubs = 1; hub_scan(x, &x->dev[i]); }
      if(!hubs) x->hubscan_at = (u64)-1;
    }
  }
  if(rep_usage && ticks >= rep_next){                 /* software typematic */
    kq_push(rep_key);
    rep_next = ticks + 3;
  }
}
