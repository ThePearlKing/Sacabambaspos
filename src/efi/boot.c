/* boot.c - SacabambaspOS UEFI boot stage.
 *
 * Foundation of SacabambaspOS. Runs as an EFI application (BOOTX64.EFI /
 * BOOTIA32.EFI) off the USB's EFI System Partition. It:
 *   1. finds the Graphics Output Protocol (the panel framebuffer),
 *   2. loads the raw BGRA image blob (SACABASP.RAW) from the same volume,
 *   3. scales it to fit and blits it centred on a solid background,
 *   4. idles forever (live/try-it mode).
 *
 * It NEVER writes to any disk. Nothing is installed. Pull the USB and the
 * machine is untouched.
 */
#include "efi.h"

#define SBOS_NAME      "SacabambaspOS"
#define SBOS_VERSION   "v0.1.0"
#define SBOS_VERSION_W u"v0.1.0"     /* same string, CHAR16 for console logs */

static EFI_SYSTEM_TABLE *ST;
static EFI_BOOT_SERVICES *BS;

/* freestanding helpers (gcc may emit calls to these) */
void *memset(void *d, int c, unsigned long n){unsigned char*p=d;while(n--)*p++=(unsigned char)c;return d;}
void *memcpy(void *d, const void *s, unsigned long n){unsigned char*a=d;const unsigned char*b=s;while(n--)*a++=*b++;return d;}

static void puts16(CHAR16 *s){ if(ST&&ST->ConOut) ST->ConOut->OutputString(ST->ConOut, s); }

/* ---- boot log ----------------------------------------------------------- */
/* EFI text attributes: fg | bg<<4 */
#define ATTR_DIM    0x08  /* dark grey  */
#define ATTR_TEXT   0x07  /* light grey */
#define ATTR_BRIGHT 0x0F  /* white      */
#define ATTR_OK     0x0A  /* green      */
#define ATTR_FAIL   0x0C  /* red        */
#define ATTR_TITLE  0x0B  /* cyan       */
#define ATTR_WARN   0x0E  /* yellow     */

static void con_attr(UINTN a){ if(ST->ConOut&&ST->ConOut->SetAttribute) ST->ConOut->SetAttribute(ST->ConOut,a); }

static void log_tag(CHAR16 *tag, UINTN color){
  con_attr(ATTR_DIM);  puts16(u"  [");
  con_attr(color);     puts16(tag);
  con_attr(ATTR_DIM);  puts16(u"] ");
  con_attr(ATTR_TEXT);
}
static void log_ok(CHAR16 *m)  { log_tag(u"  OK  ",ATTR_OK);   puts16(m); puts16(u"\r\n"); }
static void log_fail(CHAR16 *m){ log_tag(u" FAIL ",ATTR_FAIL); puts16(m); puts16(u"\r\n"); }

static void print_u(UINT64 v){
  CHAR16 b[21]; UINTN i=20; b[20]=0;
  if(!v) b[--i]=u'0';
  while(v){ b[--i]=(CHAR16)(u'0'+v%10); v/=10; }
  puts16(b+i);
}
static void print_hex(UINT64 v){
  CHAR16 b[19]; UINTN i=18; b[18]=0;
  if(!v) b[--i]=u'0';
  while(v){ UINT64 d=v&0xF; b[--i]=(CHAR16)(d<10?u'0'+d:u'A'+d-10); v>>=4; }
  puts16(u"0x"); puts16(b+i);
}

/* SBMP asset header, must match tools/mkasset.py */
typedef struct {
  UINT8  magic[4];  /* "SBMP" */
  UINT32 version, width, height, bpp, pixfmt, r0, r1;
} SbmpHeader;

/* Build a native framebuffer pixel from 8-bit RGB per the mode's format. */
static UINT32 mkpix(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi, UINT8 r, UINT8 g, UINT8 b){
  switch(mi->PixelFormat){
    case PixelBlueGreenRedReserved8BitPerColor: return ((UINT32)r<<16)|((UINT32)g<<8)|b;
    case PixelRedGreenBlueReserved8BitPerColor: return ((UINT32)b<<16)|((UINT32)g<<8)|r;
    case PixelBitMask: {
      EFI_PIXEL_BITMASK *m=&mi->PixelInformation; UINT32 v=0;
      /* shift each channel into its mask's low bit */
      UINT32 rm=m->RedMask, gm=m->GreenMask, bm=m->BlueMask;
      int rs=0,gs=0,bs=0; while(rm&&!(rm&1)){rm>>=1;rs++;} while(gm&&!(gm&1)){gm>>=1;gs++;} while(bm&&!(bm&1)){bm>>=1;bs++;}
      v |= ((UINT32)r<<rs)&m->RedMask; v |= ((UINT32)g<<gs)&m->GreenMask; v |= ((UINT32)b<<bs)&m->BlueMask;
      return v;
    }
    default: return ((UINT32)r<<16)|((UINT32)g<<8)|b;
  }
}

static EFI_STATUS load_asset(EFI_HANDLE image, UINT8 **out, UINTN *outsz){
  EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
  EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
  EFI_LOADED_IMAGE_PROTOCOL *li=NULL;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs=NULL;
  EFI_FILE_PROTOCOL *root=NULL, *f=NULL;
  EFI_STATUS s;

  s = BS->HandleProtocol(image, &li_guid, (VOID**)&li);
  if(s) return s;
  s = BS->HandleProtocol(li->DeviceHandle, &fs_guid, (VOID**)&fs);
  if(s) return s;
  s = fs->OpenVolume(fs, &root);
  if(s) return s;

  CHAR16 *names[] = { u"\\SACABASP.RAW", u"SACABASP.RAW", u"\\EFI\\BOOT\\SACABASP.RAW" };
  for(int i=0;i<3 && !f;i++){
    if(root->Open(root, &f, names[i], EFI_FILE_MODE_READ, 0)) f=NULL;
  }
  if(!f) return EFI_NOT_FOUND;

  /* read header */
  SbmpHeader h; UINTN n=sizeof(h);
  s=f->Read(f,&n,&h); if(s||n!=sizeof(h)){ f->Close(f); return s?s:EFI_LOAD_ERROR; }
  if(!(h.magic[0]=='S'&&h.magic[1]=='B'&&h.magic[2]=='M'&&h.magic[3]=='P')){ f->Close(f); return EFI_LOAD_ERROR; }
  /* sanity: reject absurd dims before the multiply (also guards 32-bit UINTN) */
  if(h.width==0||h.height==0||h.width>8192||h.height>8192||h.bpp!=32){ f->Close(f); return EFI_LOAD_ERROR; }

  UINTN pixbytes = (UINTN)h.width*h.height*4;
  UINTN total = sizeof(h)+pixbytes;
  UINT8 *buf=NULL;
  s=BS->AllocatePool(EfiLoaderData, total, (VOID**)&buf); if(s){ f->Close(f); return s; }
  memcpy(buf,&h,sizeof(h));
  /* rewind and read whole file for simplicity */
  f->SetPosition(f,0);
  n=total; s=f->Read(f,&n,buf);
  f->Close(f);
  if(s) return s;
  if(n!=total){ BS->FreePool(buf); return EFI_LOAD_ERROR; }  /* truncated file */
  *out=buf; *outsz=n; return EFI_SUCCESS;
}

static void draw(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, UINT8 *asset){
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi = gop->Mode->Info;
  UINT32 *fb = (UINT32*)(UINTN)gop->Mode->FrameBufferBase;
  UINT32 sw = mi->HorizontalResolution, sh = mi->VerticalResolution;
  UINT32 pitch = mi->PixelsPerScanLine;

  SbmpHeader *h=(SbmpHeader*)asset;
  UINT8 *px = asset+sizeof(SbmpHeader);   /* BGRA rows, top-first */
  UINT32 iw=h->width, ih=h->height;

  /* scale-to-fit (integer nearest neighbour), preserve aspect, centre */
  /* dst dims */
  UINT64 dw, dh;
  if((UINT64)iw*sh <= (UINT64)ih*sw){ dh=sh; dw=(UINT64)iw*sh/ih; }
  else                              { dw=sw; dh=(UINT64)ih*sw/iw; }
  if(dw==0)dw=1; if(dh==0)dh=1;
  UINT32 ox=(sw-(UINT32)dw)/2, oy=(sh-(UINT32)dh)/2;

  if(mi->PixelFormat==PixelBltOnly){
    /* No CPU-mappable framebuffer: render the scaled image into a BLT buffer
     * and let the firmware blit it. Asset pixels are already BGRA, the exact
     * EFI_GRAPHICS_OUTPUT_BLT_PIXEL layout. */
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL bgp = {30,12,6,0};   /* deep sea blue */
    gop->Blt(gop,&bgp,EfiBltVideoFill,0,0,0,0,sw,sh,0);
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *buf=NULL;
    if(BS->AllocatePool(EfiLoaderData,(UINTN)dw*dh*4,(VOID**)&buf) || !buf) return;
    for(UINT32 dy=0; dy<dh; dy++){
      UINT32 sy=(UINT32)((UINT64)dy*ih/dh); if(sy>=ih) sy=ih-1;
      UINT8 *srow = px + (UINTN)sy*iw*4;
      EFI_GRAPHICS_OUTPUT_BLT_PIXEL *drow = buf + (UINTN)dy*dw;
      for(UINT32 dx=0; dx<dw; dx++){
        UINT32 sx=(UINT32)((UINT64)dx*iw/dw); if(sx>=iw) sx=iw-1;
        drow[dx]=*(EFI_GRAPHICS_OUTPUT_BLT_PIXEL*)(srow+(UINTN)sx*4);
      }
    }
    gop->Blt(gop,buf,EfiBltBufferToVideo,0,0,ox,oy,dw,dh,0);
    BS->FreePool(buf);
    return;
  }

  /* background: deep sea blue */
  UINT32 bg = mkpix(mi, 6, 12, 30);
  for(UINT32 y=0;y<sh;y++){ UINT32 *row=fb+(UINTN)y*pitch; for(UINT32 x=0;x<sw;x++) row[x]=bg; }

  for(UINT32 dy=0; dy<dh; dy++){
    UINT32 sy = (UINT32)((UINT64)dy*ih/dh); if(sy>=ih) sy=ih-1;
    UINT32 *drow = fb + (UINTN)(oy+dy)*pitch + ox;
    UINT8  *srow = px + (UINTN)sy*iw*4;
    for(UINT32 dx=0; dx<dw; dx++){
      UINT32 sx=(UINT32)((UINT64)dx*iw/dw); if(sx>=iw) sx=iw-1;
      UINT8 *p=srow+(UINTN)sx*4;             /* B,G,R,A */
      drow[dx]=mkpix(mi, p[2], p[1], p[0]);
    }
  }
}

/* ---- version badge ------------------------------------------------------ */

/* 8x8 glyphs, bit 7 = leftmost pixel; just the chars a version string needs. */
typedef struct { char c; UINT8 rows[8]; } Glyph;
static const Glyph font[] = {
  {'0',{0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00}},
  {'1',{0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}},
  {'2',{0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00}},
  {'3',{0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00}},
  {'4',{0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0x00}},
  {'5',{0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00}},
  {'6',{0x3C,0x66,0x60,0x7C,0x66,0x66,0x3C,0x00}},
  {'7',{0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0x00}},
  {'8',{0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00}},
  {'9',{0x3C,0x66,0x66,0x3E,0x06,0x66,0x3C,0x00}},
  {'.',{0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}},
  {'v',{0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x00}},
  {'S',{0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00}},
  {'O',{0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}},
  {'a',{0x00,0x00,0x3C,0x06,0x3E,0x66,0x3E,0x00}},
  {'b',{0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00}},
  {'c',{0x00,0x00,0x3C,0x66,0x60,0x66,0x3C,0x00}},
  {'m',{0x00,0x00,0x66,0x7F,0x6B,0x6B,0x63,0x00}},
  {'p',{0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60}},
  {'s',{0x00,0x00,0x3E,0x60,0x3C,0x06,0x7C,0x00}},
};

static const UINT8 *glyph(char c){
  for(UINTN i=0;i<sizeof(font)/sizeof(font[0]);i++) if(font[i].c==c) return font[i].rows;
  return NULL;
}

/* Paint a string at (x0,y0) = top-left of its padded box. Read-modify-write
 * via GOP Blt so it composites over whatever is there and works on every
 * pixel format, including PixelBltOnly. Box: w=(8*len+4)*scale, h=12*scale. */
static void draw_text(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, const char *s,
                      UINT32 scale, UINT32 x0, UINT32 y0){
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi=gop->Mode->Info;
  UINT32 sw=mi->HorizontalResolution, sh=mi->VerticalResolution;
  UINTN len=0; while(s[len]) len++;

  UINT32 pad=2*scale;
  UINT32 w=(UINT32)len*8*scale+2*pad, h=8*scale+2*pad;
  if(x0+w>sw||y0+h>sh) return;

  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *buf=NULL;
  if(BS->AllocatePool(EfiLoaderData,(UINTN)w*h*4,(VOID**)&buf)||!buf) return;
  if(gop->Blt(gop,buf,EfiBltVideoToBltBuffer,x0,y0,0,0,w,h,0)){ BS->FreePool(buf); return; }

  /* pass 0 = drop shadow (dark, offset), pass 1 = white text */
  for(int pass=0;pass<2;pass++){
    UINT32 off = pass ? 0 : (scale>3 ? scale/3 : 1);
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL col = pass
      ? (EFI_GRAPHICS_OUTPUT_BLT_PIXEL){255,255,255,0}
      : (EFI_GRAPHICS_OUTPUT_BLT_PIXEL){10,16,28,0};
    for(UINTN ci=0;ci<len;ci++){
      const UINT8 *g=glyph(s[ci]); if(!g) continue;
      for(UINT32 gy=0;gy<8;gy++){
        UINT8 bits=g[gy]; if(!bits) continue;
        for(UINT32 gx=0;gx<8;gx++){
          if(!(bits&(0x80>>gx))) continue;
          for(UINT32 py=0;py<scale;py++)for(UINT32 px=0;px<scale;px++){
            UINT32 X=pad+(UINT32)ci*8*scale+gx*scale+px+off;
            UINT32 Y=pad+gy*scale+py+off;
            if(X<w&&Y<h) buf[(UINTN)Y*w+X]=col;
          }
        }
      }
    }
  }
  gop->Blt(gop,buf,EfiBltBufferToVideo,0,0,x0,y0,w,h,0);
  BS->FreePool(buf);
}

/* SBOS_VERSION, large, bottom-right corner. */
static void draw_version(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop){
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi=gop->Mode->Info;
  UINT32 sw=mi->HorizontalResolution, sh=mi->VerticalResolution;
  UINTN len=sizeof(SBOS_VERSION)-1;

  UINT32 scale = sh/120;                       /* ~64px glyphs at 1080p: large */
  if(scale<2) scale=2; if(scale>12) scale=12;
  while(scale>2 && ((UINT32)len*8+8)*scale > sw) scale--;   /* still must fit */

  UINT32 w=((UINT32)len*8+4)*scale, h=12*scale, margin=4*scale;
  if(w+margin>sw||h+margin>sh) return;
  draw_text(gop, SBOS_VERSION, scale, sw-w-margin, sh-h-margin);
}

/* SBOS_NAME, larger still, across the top starting left of centre. */
static void draw_title(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop){
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi=gop->Mode->Info;
  UINT32 sw=mi->HorizontalResolution, sh=mi->VerticalResolution;
  UINTN len=sizeof(SBOS_NAME)-1;

  UINT32 scale = sh/80;                        /* bigger than the version tag */
  if(scale<2) scale=2; if(scale>16) scale=16;
  while(scale>2 && ((UINT32)len*8+8)*scale > sw) scale--;   /* still must fit */

  UINT32 w=((UINT32)len*8+4)*scale, h=12*scale, margin=2*scale;
  if(w+margin>sw||h+margin>sh) return;
  /* centre the text block on the left-middle of the screen (x = sw/4) */
  UINT32 x0 = sw/4 > w/2+margin ? sw/4-w/2 : margin;
  draw_text(gop, SBOS_NAME, scale, x0, margin);
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st){
  ST=st; BS=st->BootServices;
  /* Firmware arms a ~5-minute watchdog that resets the machine unless the app
   * exits or ExitBootServices is called. We idle forever, so disarm it. */
  EFI_STATUS wd = BS->SetWatchdogTimer(0, 0, 0, NULL);

  if(ST->ConOut){
    ST->ConOut->Reset(ST->ConOut, 0);
    if(ST->ConOut->ClearScreen) ST->ConOut->ClearScreen(ST->ConOut);
  }

  /* banner */
  puts16(u"\r\n");
  con_attr(ATTR_TITLE);  puts16(u"  SacabambaspOS ");
  con_attr(ATTR_BRIGHT); puts16(SBOS_VERSION_W);
  con_attr(ATTR_TEXT);   puts16(u" - UEFI boot stage\r\n");
  con_attr(ATTR_DIM);    puts16(u"  ==========================================\r\n\r\n");

  if(wd) log_fail(u"Watchdog disarm refused (will still boot)");
  else   log_ok(u"Watchdog disarmed");

  con_attr(ATTR_BRIGHT);
  if(ST->FirmwareVendor){
    log_tag(u"  OK  ",ATTR_OK); puts16(u"Firmware: ");
    puts16(ST->FirmwareVendor); puts16(u" rev ");
    print_u(ST->FirmwareRevision>>16); puts16(u"."); print_u(ST->FirmwareRevision&0xFFFF);
    puts16(u"\r\n");
  }

  EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *gop=NULL;
  if(BS->LocateProtocol(&gop_guid, NULL, (VOID**)&gop) || !gop){
    log_fail(u"Graphics Output Protocol not found - cannot draw");
    for(;;) __asm__ __volatile__("hlt");
  }
  {
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi=gop->Mode->Info;
    log_tag(u"  OK  ",ATTR_OK); puts16(u"Display ");
    print_u(mi->HorizontalResolution); puts16(u"x"); print_u(mi->VerticalResolution);
    switch(mi->PixelFormat){
      case PixelBlueGreenRedReserved8BitPerColor: puts16(u" BGRX32"); break;
      case PixelRedGreenBlueReserved8BitPerColor: puts16(u" RGBX32"); break;
      case PixelBitMask:                          puts16(u" bitmask"); break;
      case PixelBltOnly:                          puts16(u" blt-only"); break;
      default: break;
    }
    puts16(u" (GOP)\r\n");
  }

  UINT8 *asset=NULL; UINTN sz=0;
  EFI_STATUS s=load_asset(image,&asset,&sz);
  if(s){
    log_tag(u" FAIL ",ATTR_FAIL); puts16(u"SACABASP.RAW load failed, status ");
    print_hex(s); puts16(u"\r\n\r\n");
    con_attr(ATTR_WARN);
    puts16(u"  Boot halted so you can read the log. Machine untouched.\r\n");
    for(;;) __asm__ __volatile__("hlt");
  }
  {
    SbmpHeader *h=(SbmpHeader*)asset;
    log_tag(u"  OK  ",ATTR_OK); puts16(u"SACABASP.RAW loaded: ");
    print_u(h->width); puts16(u"x"); print_u(h->height); puts16(u" BGRA, ");
    print_u(sz); puts16(u" bytes\r\n");
  }
  log_ok(u"Nothing written to any disk (live mode)");

  con_attr(ATTR_WARN);
  puts16(u"\r\n  Booting splash");
  con_attr(ATTR_DIM);
  for(int i=0;i<3;i++){ BS->Stall(650*1000); puts16(u" ."); }
  BS->Stall(650*1000);

  draw(gop, asset);
  draw_title(gop);
  draw_version(gop);
  /* Do NOT touch ConOut after drawing - the firmware text console would paint
   * over the framebuffer. Just idle so the Sacabambaspis stays on screen. */
  for(;;) __asm__ __volatile__("hlt");
  return EFI_SUCCESS;
}
