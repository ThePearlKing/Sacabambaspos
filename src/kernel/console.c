/* console.c - framebuffer text console for the SacabambaspOS kernel.
 *
 * Two homes. When the loader hands over the pristine splash frame and the
 * translucent panel rect its boot log lived in, the console moves into that
 * same panel: the fish stays up, and what was a splash screen quietly
 * becomes the desktop the shell runs on. Without that handoff (splash
 * fell back, or the rect doesn't fit this mode) the console takes the
 * whole screen, as before.
 *
 * Cell-grid model with a RAM shadow of the whole screen: glyphs render into
 * the shadow (cached, fast), then dirty rectangles are copied to the real
 * framebuffer with big sequential row writes. VRAM is uncached
 * write-combining - per-pixel stores there made scrolling repaint the whole
 * screen glyph by glyph (~megapixels of scattered MMIO per line); with the
 * shadow, a scroll is one RAM memmove plus one linear blit. Nothing ever
 * reads VRAM back (slow everywhere, stale on some firmware).
 *
 * In panel mode the "background" of a default-colored cell is not a flat
 * fill but the tinted splash pixel under it (panelbg holds the whole
 * panel's worth, precomputed), so text keeps the glassy look the boot log
 * had. Cells with an explicit bg (the panic banner's red) stay solid. */
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
static u32 *shadow;             /* fbw x fbh, row stride = fbw */
static int cursor_on = 1;

/* panel mode (0 in fullscreen): the console lives inside the splash panel */
static int windowed;
static u32 pn_x, pn_y, pn_w, pn_h;   /* panel rect in screen pixels */
static u32 win_px, win_py;           /* text grid origin (0,0 fullscreen) */
static u32 *panelbg;                 /* pn_w x pn_h tinted splash, native fmt */
#define WIN_PAD 8                    /* pixels between border and text */

/* COM1 mirror: everything the console prints also goes to the serial port,
 * so headless QEMU runs (and real machines with a serial header) get a log.
 * Boards without a UART float LSR at 0xFF, which reads as "ready" - the
 * writes then go nowhere, harmlessly. */
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
static void serial_puts(const char *s){ while(*s) serial_putc(*s++); }

static u32 native_rgb(u8 r, u8 g, u8 b){
  return fmt == SBOS_PIXFMT_RGBX ? ((u32)b<<16)|((u32)g<<8)|r
                                 : ((u32)r<<16)|((u32)g<<8)|b;
}

static u32 pix(u8 idx){
  const u8 *p = PALETTE[idx & 15];
  return native_rgb(p[0], p[1], p[2]);
}

/* copy a shadow rectangle to VRAM: sequential row bursts, WC-friendly */
static void blit(u32 x, u32 y, u32 w, u32 h){
  for(u32 r=0; r<h; r++)
    memcpy(fb + (u64)(y+r)*pitch + x, shadow + (u64)(y+r)*fbw + x, (u64)w*4);
}

/* render one cell into the shadow (no blit - caller batches). In panel
 * mode a default-bg cell shows the tinted splash through it; the cursor
 * inverts by flipping the glyph bit sense, which on a textured bg reads
 * as a solid block with the glyph carved out. */
static void render_cell(u32 x, u32 y){
  Cell *c = &cells[y*cols + x];
  u32 fg = pix(c->fg), bg = pix(c->bg);
  int solid = !windowed || c->bg != C_BLACK;
  int invert = cursor_on && x == cx && y == cy;
  const unsigned char *g =
    (c->ch >= FONT_FIRST && c->ch <= FONT_LAST) ? font8x16[c->ch - FONT_FIRST]
                                                : font8x16[0];
  u32 px0 = win_px + x*FONT_W, py0 = win_py + y*FONT_H;
  for(u32 gy=0; gy<FONT_H; gy++){
    u32 *row = shadow + (u64)(py0+gy)*fbw + px0;
    const u32 *tex = solid ? 0
      : panelbg + (u64)(py0+gy - pn_y)*pn_w + (px0 - pn_x);
    unsigned char bits = g[gy];
    for(u32 gx=0; gx<FONT_W; gx++){
      int on = bits & (0x80>>gx);
      if(invert) on = !on;
      row[gx] = on ? fg : (solid ? bg : tex[gx]);
    }
  }
}

static void paint_cell(u32 x, u32 y){
  render_cell(x, y);
  blit(win_px + x*FONT_W, win_py + y*FONT_H, FONT_W, FONT_H);
}

/* The splash frame arrives as BGRA rows, fb_width wide, no pitch - a u32
 * of it IS a native BGRX pixel; RGBX needs the red/blue swap. */
static u32 splash_pix(u32 v){
  return fmt == SBOS_PIXFMT_RGBX
    ? ((v&0xFF)<<16) | (v&0xFF00) | ((v>>16)&0xFF) : v;
}

int con_init(SbosBootInfo *bi){
  serial_init();
  fb    = (u32*)bi->fb_base;
  fbw   = bi->fb_width; fbh = bi->fb_height;
  pitch = bi->fb_pitch; fmt = bi->fb_format;

  /* take the panel if the loader passed one and it fits this mode */
  u32 *splash = (u32*)bi->splash_base;
  pn_x = bi->panel_x; pn_y = bi->panel_y;
  pn_w = bi->panel_w; pn_h = bi->panel_h;
  int want_win = splash
    && pn_w && pn_h && pn_w <= fbw && pn_h <= fbh
    && pn_x <= fbw - pn_w && pn_y <= fbh - pn_h
    && pn_w > 2*WIN_PAD + FONT_W && pn_h > 2*WIN_PAD + FONT_H;
  if(want_win){
    panelbg = kalloc((u64)pn_w * pn_h * 4);
    if(!panelbg) want_win = 0;          /* no room: plain fullscreen */
  }
  windowed = want_win;
  win_px = windowed ? pn_x + WIN_PAD : 0;
  win_py = windowed ? pn_y + WIN_PAD : 0;
  cols = (windowed ? pn_w - 2*WIN_PAD : fbw) / FONT_W;
  rows = (windowed ? pn_h - 2*WIN_PAD : fbh) / FONT_H;
  if(!cols || !rows){ serial_puts("console: display too small\n"); return 0; }

  cells  = kalloc((u64)cols * rows * sizeof(Cell));
  shadow = kalloc((u64)fbw * fbh * 4);
  if(!cells || !shadow){ serial_puts("console: kernel heap too small\n"); return 0; }

  if(windowed){
    /* whole splash into the shadow, then bake the panel background: the
     * same ~66% dark glass tint the boot log used, plus the 1px border */
    for(u64 i=0; i<(u64)fbw*fbh; i++) shadow[i] = splash_pix(splash[i]);
    for(u32 y=0; y<pn_h; y++)
      for(u32 x=0; x<pn_w; x++){
        u32 v = splash[(u64)(pn_y+y)*fbw + pn_x + x];
        u8 b = (u8)(((v      & 0xFF)*88 + 26*168) >> 8);
        u8 g = (u8)((((v>>8) & 0xFF)*88 + 14*168) >> 8);
        u8 r = (u8)((((v>>16)& 0xFF)*88 +  8*168) >> 8);
        panelbg[(u64)y*pn_w + x] = native_rgb(r, g, b);
      }
    u32 bd = native_rgb(70, 110, 150);
    for(u32 x=0; x<pn_w; x++){ panelbg[x] = bd; panelbg[(u64)(pn_h-1)*pn_w + x] = bd; }
    for(u32 y=0; y<pn_h; y++){ panelbg[(u64)y*pn_w] = bd; panelbg[(u64)y*pn_w + pn_w-1] = bd; }
  }

  con_clear();
  if(windowed) blit(0, 0, fbw, fbh);    /* repaint splash + fresh panel once */
  return 1;
}

u32 con_cols(void){ return cols; }
u32 con_rows(void){ return rows; }
void con_color(u8 fg, u8 bg){ cur_fg = fg; cur_bg = bg; }
void con_fg(u8 fg){ cur_fg = fg; }
void con_cursor(int on){ cursor_on = on; paint_cell(cx, cy); }
void con_getxy(u32 *x, u32 *y){ *x = cx; *y = cy; }

void con_setxy(u32 x, u32 y){
  u32 ox = cx, oy = cy;
  cx = x < cols ? x : cols-1;
  cy = y < rows ? y : rows-1;
  paint_cell(ox, oy);           /* erase old cursor */
  paint_cell(cx, cy);
}

void con_clear(void){
  for(u32 i=0; i<cols*rows; i++) cells[i] = (Cell){' ', cur_fg, cur_bg};
  cx = cy = 0;
  if(windowed){
    /* restore the panel to its baked background; the splash around it
     * is untouched */
    for(u32 y=0; y<pn_h; y++)
      memcpy(shadow + (u64)(pn_y+y)*fbw + pn_x, panelbg + (u64)y*pn_w, (u64)pn_w*4);
    blit(pn_x, pn_y, pn_w, pn_h);
    return;
  }
  /* paint the whole shadow, including the right/bottom fringe cells miss */
  u32 bg = pix(cur_bg);
  for(u64 i=0; i<(u64)fbw*fbh; i++) shadow[i] = bg;
  blit(0, 0, fbw, fbh);
}

static void scroll(void){
  memmove(cells, cells + cols, (u64)(rows-1)*cols*sizeof(Cell));
  for(u32 x=0; x<cols; x++) cells[(rows-1)*cols + x] = (Cell){' ', cur_fg, cur_bg};
  if(windowed){
    /* can't slide pixels here: the glass tint belongs to the location,
     * not the glyph. Re-render every cell over the baked background and
     * present the text grid as one rectangle. */
    for(u32 y=0; y<rows; y++)
      for(u32 x=0; x<cols; x++) render_cell(x, y);
    blit(win_px, win_py, cols*FONT_W, rows*FONT_H);
    return;
  }
  /* pixels move in RAM; the empty last band is filled, then one linear blit */
  u32 grid_h = rows*FONT_H;
  memmove(shadow, shadow + (u64)FONT_H*fbw, (u64)(grid_h-FONT_H)*fbw*4);
  u32 bg = pix(cur_bg);
  for(u64 i=(u64)(grid_h-FONT_H)*fbw; i<(u64)grid_h*fbw; i++) shadow[i] = bg;
  blit(0, 0, fbw, grid_h);
}

void con_putc(char c){
  serial_putc(c);
  u32 ox = cx, oy = cy;
  if(c == '\n'){ cx = 0; cy++; }
  else if(c == '\r'){ cx = 0; }
  else if(c == '\b'){
    if(cx){ cx--; cells[cy*cols+cx] = (Cell){' ', cur_fg, cur_bg}; }
  }
  else{
    cells[cy*cols+cx] = (Cell){c, cur_fg, cur_bg};
    if(++cx >= cols){ cx = 0; cy++; }
  }
  if(cy >= rows){ cy = rows-1; scroll(); oy = cy; ox = cx; }
  if(ox != cx || oy != cy) paint_cell(ox, oy);   /* repaint cell cursor left */
  paint_cell(cx, cy);
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
