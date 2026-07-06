/* kbd.c - PS/2 keyboard, scancode set 1, US layout.
 *
 * IRQ1 pushes scancodes into a ring buffer; kbd_getc() drains it and does
 * the scancode -> ASCII translation (shift, caps lock, arrows). USB
 * keyboards reach this through the firmware's PS/2 legacy emulation; a
 * native USB HID stack is a later stage. */
#include "kernel.h"

#define BUFSZ 256
static volatile u8 buf[BUFSZ];
static volatile u32 rd, wr;

static int shift, caps, ext;

static const char map_lo[128] = {
  0,27,'1','2','3','4','5','6','7','8','9','0','-','=','\b',
  '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
  0,'a','s','d','f','g','h','j','k','l',';','\'','`',
  0,'\\','z','x','c','v','b','n','m',',','.','/',0,'*',
  0,' ',0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,'7','8','9','-','4','5','6','+','1','2','3','0','.',
};
static const char map_hi[128] = {
  0,27,'!','@','#','$','%','^','&','*','(',')','_','+','\b',
  '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
  0,'A','S','D','F','G','H','J','K','L',':','"','~',
  0,'|','Z','X','C','V','B','N','M','<','>','?',0,'*',
  0,' ',0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,'7','8','9','-','4','5','6','+','1','2','3','0','.',
};

void kbd_init(void){
  rd = wr = 0; shift = caps = ext = 0;
  /* drain any scancodes queued while firmware owned the controller */
  while(inb(0x64) & 1) (void)inb(0x60);
}

void kbd_irq(void){
  u8 sc = inb(0x60);
  u32 n = (wr + 1) % BUFSZ;
  if(n != rd){ buf[wr] = sc; wr = n; }
}

static int pop(void){
  if(rd == wr) return -1;
  u8 v = buf[rd]; rd = (rd + 1) % BUFSZ;
  return v;
}

/* blocking read; hlt between interrupts so the idle loop costs nothing */
int kbd_getc(void){
  for(;;){
    int sc = pop();
    if(sc < 0){ hlt(); continue; }
    if(sc == 0xE0){ ext = 1; continue; }
    if(ext){
      ext = 0;
      if(sc & 0x80) continue;                 /* extended release */
      switch(sc){
        case 0x48: return KEY_UP;
        case 0x50: return KEY_DOWN;
        case 0x4B: return KEY_LEFT;
        case 0x4D: return KEY_RIGHT;
        case 0x1C: return '\n';               /* keypad enter */
        default: continue;
      }
    }
    if(sc & 0x80){                            /* release */
      u8 mk = sc & 0x7F;
      if(mk == 0x2A || mk == 0x36) shift = 0;
      continue;
    }
    if(sc == 0x2A || sc == 0x36){ shift = 1; continue; }
    if(sc == 0x3A){ caps = !caps; continue; }
    if(sc >= 128) continue;
    char c = shift ? map_hi[sc] : map_lo[sc];
    if(!c) continue;
    if(caps && c >= 'a' && c <= 'z' && !shift) c += 'A'-'a';
    else if(caps && c >= 'A' && c <= 'Z' && shift) c += 'a'-'A';
    return c;
  }
}
