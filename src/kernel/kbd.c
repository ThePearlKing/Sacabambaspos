/* kbd.c - PS/2 keyboard, scancode set 1, US layout.
 *
 * Scancodes land in a ring buffer two ways: IRQ1 when the board delivers
 * it, and a status-register poll from kbd_getc()'s wait loop. The poll is
 * not an optimization - on some boards IRQ1 simply never arrives (IOAPIC-
 * only routing with the legacy PIC line unwired, or USB legacy emulation
 * where SMM traps port 0x60/0x64 but raises no interrupt), and an
 * IRQ-only driver reads as "keyboard dead" there. The PIT ticks at 100 Hz,
 * so the hlt in kbd_getc() wakes and re-polls at least every 10 ms even
 * with IRQ1 completely dead. kbd_getc() drains the buffer and does the
 * scancode -> ASCII translation (shift, caps lock, arrows). USB keyboards
 * reach this through the firmware's PS/2 legacy emulation; a native USB
 * HID stack is a later stage.
 *
 * Hardware diversity handled here:
 *  - No 8042 at all (Intel Macs, some modern boards): the status port
 *    floats 0xFF - detect and bail instead of hanging.
 *  - Firmware handing off with IRQ1 disabled in the 8042 command byte or
 *    scanning stopped: re-enable both explicitly.
 *  - Every controller wait is bounded; a wedged controller degrades to
 *    "no keyboard", never to a boot hang. */
#include "kernel.h"

#define KBC_DATA 0x60
#define KBC_STAT 0x64
#define KBC_CMD  0x64

#define BUFSZ 256
static volatile u8 buf[BUFSZ];
static volatile u32 rd, wr;

static int shift_l, shift_r, caps, ext;

static const char map_lo[128] = {
  0,27,'1','2','3','4','5','6','7','8','9','0','-','=','\b',
  '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
  0,'a','s','d','f','g','h','j','k','l',';','\'','`',
  0,'\\','z','x','c','v','b','n','m',',','.','/',0,'*',
  0,' ',
  /* 0x3A caps, 0x3B-0x44 F1-F10, 0x45 numlock, 0x46 scroll lock */
  0,0,0,0,0,0,0,0,0,0,0,0,0,
  '7','8','9','-','4','5','6','+','1','2','3','0','.',
};
static const char map_hi[128] = {
  0,27,'!','@','#','$','%','^','&','*','(',')','_','+','\b',
  '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
  0,'A','S','D','F','G','H','J','K','L',':','"','~',
  0,'|','Z','X','C','V','B','N','M','<','>','?',0,'*',
  0,' ',
  0,0,0,0,0,0,0,0,0,0,0,0,0,
  '7','8','9','-','4','5','6','+','1','2','3','0','.',
};

/* wait until the controller accepts input; <0 = absent or wedged */
static int kbc_wait_in(void){
  for(int i=0; i<50000; i++){
    u8 s = inb(KBC_STAT);
    if(s == 0xFF) return -1;            /* floating bus: no controller */
    if(!(s & 2)) return 0;
    io_wait();
  }
  return -1;
}

/* returns 1 if a working controller was found and armed, 0 otherwise */
int kbd_init(void){
  rd = wr = 0; shift_l = shift_r = caps = ext = 0;

  if(inb(KBC_STAT) == 0xFF) return 0;   /* Intel Macs and PS/2-less boards */

  /* drain stale scancodes, bounded (a dead controller could report
   * output-full forever) */
  for(int i=0; i<64 && (inb(KBC_STAT) & 1); i++){ (void)inb(KBC_DATA); io_wait(); }

  /* command byte: IRQ1 on, translation on, keyboard clock on. Firmware
   * often leaves IRQ1 off after ExitBootServices (QEMU is the lenient
   * exception), so this write is what makes real hardware type. */
  u8 cmd = 0x45;                        /* sane default if the read fails */
  if(kbc_wait_in()) return 0;
  outb(KBC_CMD, 0x20);
  for(int i=0; i<50000; i++){
    if(inb(KBC_STAT) == 0xFF) return 0;
    if(inb(KBC_STAT) & 1){ cmd = inb(KBC_DATA); break; }
    io_wait();
  }
  cmd |= 0x01 | 0x40;                   /* IRQ1 enable + set-1 translation */
  cmd &= (u8)~0x10;                     /* keyboard clock enabled */
  if(kbc_wait_in()) return 0;
  outb(KBC_CMD, 0x60);
  if(kbc_wait_in()) return 0;
  outb(KBC_DATA, cmd);

  if(!kbc_wait_in()) outb(KBC_DATA, 0xF4);   /* device: enable scanning */
  return 1;
}

/* Move any pending scancodes from the controller into the ring buffer.
 * Callers hold interrupts off (IRQ1 handler, or kbd_getc under cli), so
 * poll and IRQ never race each other. Checking OBF before every read also
 * keeps the two entry paths honest: once one of them takes the byte, the
 * other sees OBF clear and reads nothing (port 0x60 with OBF clear just
 * returns the last byte again - reading it would duplicate keystrokes).
 * Bounded: a controller streaming garbage can't trap us here. */
static void kbd_poll(void){
  for(int i=0; i<16; i++){
    u8 st = inb(KBC_STAT);
    if(st == 0xFF || !(st & 1)) return;   /* absent, or nothing pending */
    u8 sc = inb(KBC_DATA);
    if(st & 0x20) continue;               /* aux (mouse) byte: not ours */
    u32 n = (wr + 1) % BUFSZ;
    if(n != rd){ buf[wr] = sc; wr = n; }
    io_wait();
  }
}

void kbd_irq(void){ kbd_poll(); }

static int pop(void){
  if(rd == wr) return -1;
  u8 v = buf[rd]; rd = (rd + 1) % BUFSZ;
  return v;
}

/* run one scancode through the set-1 state machine; -1 = nothing to emit */
static int decode(u8 sc){
  if(sc == 0xE0){ ext = 1; return -1; }
  if(ext){
    ext = 0;
    if(sc & 0x80) return -1;                  /* extended release */
    switch(sc){
      case 0x48: return KEY_UP;
      case 0x50: return KEY_DOWN;
      case 0x4B: return KEY_LEFT;
      case 0x4D: return KEY_RIGHT;
      case 0x53: return KEY_DEL;
      case 0x47: return KEY_HOME;
      case 0x4F: return KEY_END;
      case 0x1C: return '\n';                 /* keypad enter */
      default: return -1;
    }
  }
  if(sc & 0x80){                              /* release */
    u8 mk = sc & 0x7F;
    if(mk == 0x2A) shift_l = 0;
    if(mk == 0x36) shift_r = 0;
    return -1;
  }
  if(sc == 0x2A){ shift_l = 1; return -1; }
  if(sc == 0x36){ shift_r = 1; return -1; }
  if(sc == 0x3A){ caps = !caps; return -1; }
  if(sc >= 128) return -1;
  int shift = shift_l | shift_r;
  char c = shift ? map_hi[sc] : map_lo[sc];
  if(!c) return -1;
  if(caps && c >= 'a' && c <= 'z' && !shift) c += 'A'-'a';
  else if(caps && c >= 'A' && c <= 'Z' && shift) c += 'a'-'A';
  return c;
}

/* non-blocking read: pump USB and the 8042 once, -1 if no key is pending.
 * Interrupts must be on (usbkbd_poll needs ticks + hlt-bounded waits). */
int kbd_trygetc(void){
  usbkbd_poll();
  int uc = usbkbd_pop();
  if(uc >= 0) return uc;
  for(;;){
    cli();
    kbd_poll();                 /* boards where IRQ1 never fires */
    int sc = pop();
    sti();
    if(sc < 0) return -1;
    int k = decode((u8)sc);
    if(k >= 0) return k;
  }
}

/* blocking read. A key can slip in between the empty check and the hlt, but
 * the PIT ends every hlt within 10 ms and the loop re-polls, so the worst
 * case is one tick of latency - same bound the USB path always had. */
int kbd_getc(void){
  for(;;){
    int c = kbd_trygetc();
    if(c >= 0) return c;
    hlt();
  }
}
