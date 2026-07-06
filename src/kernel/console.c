/* console.c - framebuffer text console for the SacabambaspOS kernel.
 *
 * Cell-grid model over a 32bpp linear framebuffer: every cell carries its
 * character and colors, rendering always goes cells -> pixels, so scrolling
 * never reads VRAM back (slow on real hardware, and firmware shadow-buffer
 * bugs taught us in stage 0 never to trust read-back). */
#include "kernel.h"
#include "font8x16.h"

/* deep-sea theme: same background the splash uses */
static const u8 PALETTE[16][3] = {   /* r,g,b */
  {  6, 12, 30}, {  0, 80,200}, { 60,180, 90}, { 60,190,200},
  {220, 70, 70}, {190, 80,190}, {180,130, 60}, {180,190,200},
  {100,110,130}, {110,160,255}, {120,230,140}, {  0,255,255},
  {255,110,110}, {255,140,200}, {250,220,100}, {255,255,255},
};

typedef struct { char ch; u8 fg, bg; } Cell;

static u32 *fb;
static u32 fbw, fbh, pitch, fmt;
static u32 cols, rows;
static u32 cx, cy;              /* cursor cell */
static u8  cur_fg = C_LGREY, cur_bg = C_BLACK;
static Cell *cells;
static int cursor_on = 1;

static u32 pix(u8 idx){
  const u8 *p = PALETTE[idx & 15];
  return fmt == SBOS_PIXFMT_RGBX ? ((u32)p[2]<<16)|((u32)p[1]<<8)|p[0]
                                 : ((u32)p[0]<<16)|((u32)p[1]<<8)|p[2];
}

static void render_cell(u32 x, u32 y){
  Cell *c = &cells[y*cols + x];
  u32 fg = pix(c->fg), bg = pix(c->bg);
  int invert = cursor_on && x == cx && y == cy;
  if(invert){ u32 t = fg; fg = bg; bg = t; }
  const unsigned char *g =
    (c->ch >= FONT_FIRST && c->ch <= FONT_LAST) ? font8x16[c->ch - FONT_FIRST]
                                                : font8x16[0];
  u32 px0 = x*FONT_W, py0 = y*FONT_H;
  for(u32 gy=0; gy<FONT_H; gy++){
    u32 *row = fb + (u64)(py0+gy)*pitch + px0;
    unsigned char bits = g[gy];
    for(u32 gx=0; gx<FONT_W; gx++)
      row[gx] = (bits & (0x80>>gx)) ? fg : bg;
  }
}

static void render_row(u32 y){ for(u32 x=0; x<cols; x++) render_cell(x, y); }

/* COM1 mirror: everything the console prints also goes to the serial port,
 * so headless QEMU runs (and real machines with a serial header) get a log */
#define COM1 0x3F8
static void serial_init(void){
  outb(COM1+1,0); outb(COM1+3,0x80); outb(COM1,1); outb(COM1+1,0);
  outb(COM1+3,0x03); outb(COM1+2,0xC7); outb(COM1+4,0x0B);
}
static void serial_putc(char c){
  if(c=='\n') serial_putc('\r');
  for(int i=0; i<10000 && !(inb(COM1+5)&0x20); i++);
  outb(COM1, c);
}

void con_init(SbosBootInfo *bi){
  serial_init();
  fb    = (u32*)bi->fb_base;
  fbw   = bi->fb_width; fbh = bi->fb_height;
  pitch = bi->fb_pitch; fmt = bi->fb_format;
  cols  = fbw / FONT_W; rows = fbh / FONT_H;
  cells = kalloc((u64)cols * rows * sizeof(Cell));
  con_clear();
}

u32 con_cols(void){ return cols; }
u32 con_rows(void){ return rows; }
void con_color(u8 fg, u8 bg){ cur_fg = fg; cur_bg = bg; }
void con_fg(u8 fg){ cur_fg = fg; }
void con_cursor(int on){ cursor_on = on; render_cell(cx, cy); }
void con_getxy(u32 *x, u32 *y){ *x = cx; *y = cy; }

void con_setxy(u32 x, u32 y){
  u32 ox = cx, oy = cy;
  cx = x < cols ? x : cols-1;
  cy = y < rows ? y : rows-1;
  render_cell(ox, oy);          /* erase old cursor */
  render_cell(cx, cy);
}

void con_clear(void){
  for(u32 i=0; i<cols*rows; i++) cells[i] = (Cell){' ', cur_fg, cur_bg};
  cx = cy = 0;
  /* paint the whole screen, including the right/bottom fringe cells miss */
  u32 bg = pix(cur_bg);
  for(u32 y=0; y<fbh; y++){
    u32 *row = fb + (u64)y*pitch;
    for(u32 x=0; x<fbw; x++) row[x] = bg;
  }
}

static void scroll(void){
  memmove(cells, cells + cols, (u64)(rows-1)*cols*sizeof(Cell));
  for(u32 x=0; x<cols; x++) cells[(rows-1)*cols + x] = (Cell){' ', cur_fg, cur_bg};
  for(u32 y=0; y<rows; y++) render_row(y);
}

void con_putc(char c){
  serial_putc(c);
  u32 ox = cx, oy = cy;
  if(c == '\n'){ cx = 0; cy++; }
  else if(c == '\r'){ cx = 0; }
  else if(c == '\b'){ if(cx) cx--; cells[cy*cols+cx] = (Cell){' ', cur_fg, cur_bg}; }
  else{
    cells[cy*cols+cx] = (Cell){c, cur_fg, cur_bg};
    if(++cx >= cols){ cx = 0; cy++; }
  }
  if(cy >= rows){ cy = rows-1; scroll(); oy = cy; ox = cx; }
  if(ox != cx || oy != cy) render_cell(ox, oy);   /* repaint cell cursor left */
  render_cell(cx, cy);
}

void con_puts(const char *s){ while(*s) con_putc(*s++); }

void con_put_u64(u64 v){
  char b[21]; int i = 20; b[20] = 0;
  if(!v) b[--i] = '0';
  while(v){ b[--i] = '0' + v%10; v /= 10; }
  con_puts(b+i);
}

void con_put_hex(u64 v){
  char b[17]; int i = 16; b[16] = 0;
  if(!v) b[--i] = '0';
  while(v){ u64 d = v & 15; b[--i] = d < 10 ? '0'+d : 'a'+d-10; v >>= 4; }
  con_puts("0x"); con_puts(b+i);
}

void klog_tagged(const char *tag, u8 tagcolor, const char *m){
  con_fg(C_DGREY);   con_puts("  [");
  con_fg(tagcolor);  con_puts(tag);
  con_fg(C_DGREY);   con_puts("] ");
  con_fg(C_LGREY);   con_puts(m); con_putc('\n');
}
void klog_ok(const char *m)  { klog_tagged("  OK  ", C_LGREEN, m); }
void klog_fail(const char *m){ klog_tagged(" FAIL ", C_LRED,   m); }
