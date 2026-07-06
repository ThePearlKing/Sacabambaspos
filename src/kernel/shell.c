/* shell.c - the SacabambaspOS interactive shell.
 *
 * Built-in commands running in kernel mode for now; they become real
 * userland binaries once we have ring 3 + syscalls (next stage). The input
 * line is re-rendered on every keystroke with live syntax coloring:
 * valid command green, unknown red, quoted strings yellow, numbers cyan,
 * -flags pink. */
#include "kernel.h"
#include "../efi/version.h"

#define LINE_MAX 240
#define HIST_MAX 16

static char hist[HIST_MAX][LINE_MAX];
static int hist_n, hist_at;

/* ---- builtins ------------------------------------------------------- */

typedef struct { const char *name, *help; void (*fn)(int argc, char **argv); } Cmd;
static const Cmd *find_cmd(const char *name, size_t n);
static void list_cmds(void);

static void cmd_help(int argc, char **argv){
  (void)argc; (void)argv;
  con_fg(C_LCYAN); con_puts("  SacabambaspOS shell builtins\n");
  list_cmds();
}

static void cmd_clear(int argc, char **argv){ (void)argc;(void)argv; con_clear(); }

static void cmd_echo(int argc, char **argv){
  con_fg(C_WHITE);
  for(int i=1; i<argc; i++){ con_puts(argv[i]); if(i+1<argc) con_putc(' '); }
  con_putc('\n');
}

static void cmd_ver(int argc, char **argv){
  (void)argc;(void)argv;
  con_fg(C_WHITE);  con_puts("  Sacabambasp");
  con_fg(C_LCYAN);  con_puts("OS ");
  con_fg(C_YELLOW); con_puts(SBOS_VERSION);
  con_fg(C_LGREY);  con_puts("  stage 1: kernel + shell\n");
  con_fg(C_DGREY);  con_puts("  named after Sacabambaspis janvieri, 460 million years your senior\n");
}

static void cmd_mem(int argc, char **argv){
  (void)argc;(void)argv;
  con_fg(C_LGREY); con_puts("  total RAM   "); con_fg(C_LCYAN);
  con_put_u64(g_bi->total_ram >> 20); con_puts(" MiB\n");
  con_fg(C_LGREY); con_puts("  kernel heap "); con_fg(C_LCYAN);
  con_put_u64(kalloc_used() >> 10); con_puts(" KiB");
  con_fg(C_LGREY); con_puts(" used of "); con_fg(C_LCYAN);
  con_put_u64(g_bi->heap_size >> 20); con_puts(" MiB\n");
}

static void cmd_uptime(int argc, char **argv){
  (void)argc;(void)argv;
  u64 t = ticks;
  con_fg(C_LGREY); con_puts("  up "); con_fg(C_LCYAN);
  con_put_u64(t/100); con_putc('.');
  con_putc('0' + (t/10)%10); con_putc('0' + t%10);
  con_fg(C_LGREY); con_puts(" s ("); con_fg(C_LCYAN); con_put_u64(t);
  con_fg(C_LGREY); con_puts(" ticks @ 100 Hz)\n");
}

static void cmd_fb(int argc, char **argv){
  (void)argc;(void)argv;
  con_fg(C_LGREY); con_puts("  framebuffer "); con_fg(C_LCYAN);
  con_put_u64(g_bi->fb_width); con_putc('x'); con_put_u64(g_bi->fb_height);
  con_fg(C_LGREY); con_puts(" @ "); con_fg(C_LCYAN); con_put_hex(g_bi->fb_base);
  con_fg(C_LGREY); con_puts("  pitch "); con_fg(C_LCYAN); con_put_u64(g_bi->fb_pitch);
  con_fg(C_LGREY); con_puts(" px  ");
  con_fg(C_LCYAN); con_puts(g_bi->fb_format==SBOS_PIXFMT_BGRX ? "BGRX" : "RGBX");
  con_fg(C_LGREY); con_puts("  console ");
  con_fg(C_LCYAN); con_put_u64(con_cols()); con_putc('x'); con_put_u64(con_rows());
  con_putc('\n');
}

static void cmd_colors(int argc, char **argv){
  (void)argc;(void)argv;
  con_puts("  ");
  for(int i=0; i<16; i++){ con_color(i==C_BLACK?C_DGREY:i, C_BLACK); con_puts("##"); }
  con_color(C_LGREY, C_BLACK); con_putc('\n');
}

static void cmd_fish(int argc, char **argv){
  (void)argc;(void)argv;
  static const char *art[] = {
    "        ______________________ ",
    "      /                        \\___ ",
    "     /  O                          \\__",
    "    |  __                     ________/",
    "     \\ \\/                 ___/",
    "      \\___________________\\",
  };
  con_fg(C_LCYAN);
  for(unsigned i=0; i<sizeof(art)/sizeof(art[0]); i++){ con_puts(art[i]); con_putc('\n'); }
  con_fg(C_DGREY); con_puts("  sacabambaspis approves of this boot\n");
}

static void cmd_reboot(int argc, char **argv){
  (void)argc;(void)argv;
  con_fg(C_YELLOW); con_puts("  rebooting...\n");
  for(volatile int i=0;i<20000000;i++);
  u8 t = inb(0x64);                       /* 8042 pulse reset line */
  while(t & 2) t = inb(0x64);
  outb(0x64, 0xFE);
  for(;;) hlt();
}

static void cmd_halt(int argc, char **argv){
  (void)argc;(void)argv;
  con_fg(C_YELLOW); con_puts("  halted. pull the USB whenever - nothing was written.\n");
  con_cursor(0);
  cli();
  for(;;) hlt();
}

static const Cmd cmds[] = {
  {"help",   "list commands",                       cmd_help},
  {"clear",  "wipe the screen",                     cmd_clear},
  {"echo",   "print arguments",                     cmd_echo},
  {"ver",    "version + lineage",                   cmd_ver},
  {"mem",    "RAM + kernel heap usage",             cmd_mem},
  {"uptime", "time since kernel entry",             cmd_uptime},
  {"fb",     "framebuffer + console geometry",      cmd_fb},
  {"colors", "palette test",                        cmd_colors},
  {"fish",   "the mascot",                          cmd_fish},
  {"reboot", "reset the machine",                   cmd_reboot},
  {"halt",   "stop the CPU",                        cmd_halt},
};
#define NCMDS ((int)(sizeof(cmds)/sizeof(cmds[0])))

static void list_cmds(void){
  for(int i=0; i<NCMDS; i++){
    con_fg(C_LGREEN); con_puts("  ");
    con_puts(cmds[i].name);
    for(size_t p=strlen(cmds[i].name); p<10; p++) con_putc(' ');
    con_fg(C_LGREY); con_puts(cmds[i].help); con_putc('\n');
  }
}

static const Cmd *find_cmd(const char *name, size_t n){
  for(int i=0; i<NCMDS; i++)
    if(strlen(cmds[i].name)==n && !strncmp(cmds[i].name, name, n)) return &cmds[i];
  return NULL;
}

/* ---- line editor with live syntax coloring --------------------------- */

static void put_prompt(void){
  con_fg(C_LCYAN);  con_puts("sbos ");
  con_fg(C_YELLOW); con_puts("><> ");
  con_fg(C_LGREY);
}

static int is_digit_tok(const char *s, size_t n){
  if(!n) return 0;
  for(size_t i=0; i<n; i++) if(s[i]<'0'||s[i]>'9') return 0;
  return 1;
}

/* redraw the line buffer after the prompt with coloring; pad clears leftovers */
static void render_line(u32 px, u32 py, const char *b, size_t len, size_t pad){
  con_setxy(px, py);
  size_t i = 0; int first_done = 0;
  while(i < len){
    if(b[i]==' '){ con_fg(C_LGREY); con_putc(' '); i++; continue; }
    size_t j = i; int quoted = b[i]=='"';
    if(quoted){ j++; while(j<len && b[j]!='"') j++; if(j<len) j++; }
    else while(j<len && b[j]!=' ') j++;
    u8 col;
    if(!first_done){ col = find_cmd(b+i, j-i) ? C_LGREEN : C_LRED; first_done = 1; }
    else if(quoted)                col = C_YELLOW;
    else if(b[i]=='-')             col = C_PINK;
    else if(is_digit_tok(b+i,j-i)) col = C_LCYAN;
    else                           col = C_WHITE;
    con_fg(col);
    while(i<j) con_putc(b[i++]);
  }
  con_fg(C_LGREY);
  for(size_t k=0; k<pad; k++) con_putc(' ');
  for(size_t k=0; k<pad; k++) con_putc('\b');
}

static void read_line(char *b, size_t max){
  size_t len = 0;
  u32 px, py; con_getxy(&px, &py);
  hist_at = hist_n;
  for(;;){
    int c = kbd_getc();
    if(c == '\n'){ b[len] = 0; con_putc('\n'); return; }
    if(c == '\b'){
      if(len){ len--; render_line(px, py, b, len, 1); }
      continue;
    }
    if(c == KEY_UP || c == KEY_DOWN){
      int want = c==KEY_UP ? hist_at-1 : hist_at+1;
      if(want < 0 || want > hist_n) continue;
      size_t old = len;
      if(want == hist_n) len = 0;
      else { len = strlen(hist[want]); memcpy(b, hist[want], len); }
      hist_at = want;
      render_line(px, py, b, len, old > len ? old-len : 0);
      continue;
    }
    if(c < 0x20 || c > 0x7E) continue;
    /* stay on one row: leave space for the trailing cursor cell */
    if(px + len + 2 >= con_cols() || len + 1 >= max) continue;
    b[len++] = (char)c;
    render_line(px, py, b, len, 0);
  }
}

/* ---- execution -------------------------------------------------------- */

void shell_run_line(const char *line, int echo){
  char buf[LINE_MAX];
  size_t n = strlen(line); if(n >= LINE_MAX) n = LINE_MAX-1;
  memcpy(buf, line, n); buf[n] = 0;

  /* tokenize in place; honour double quotes */
  char *argv[16]; int argc = 0;
  char *p = buf;
  while(*p && argc < 16){
    while(*p==' ') p++;
    if(!*p) break;
    if(*p=='"'){ argv[argc++] = ++p; while(*p && *p!='"') p++; }
    else { argv[argc++] = p; while(*p && *p!=' ') p++; }
    if(*p) *p++ = 0;
  }
  if(!argc) return;

  if(echo){
    put_prompt();
    /* replay through the live renderer so scripted lines look typed */
    u32 px,py; con_getxy(&px,&py);
    render_line(px, py, line, strlen(line), 0);
    con_putc('\n');
  }

  const Cmd *c = find_cmd(argv[0], strlen(argv[0]));
  if(!c){
    con_fg(C_LRED);  con_puts("  no such command: ");
    con_fg(C_WHITE); con_puts(argv[0]);
    con_fg(C_DGREY); con_puts("   (try ");
    con_fg(C_LGREEN);con_puts("help");
    con_fg(C_DGREY); con_puts(")\n");
    return;
  }
  c->fn(argc, argv);
}

void shell_main(SbosBootInfo *bi){
  (void)bi;
  char line[LINE_MAX];
  for(;;){
    put_prompt();
    read_line(line, sizeof(line));
    if(!line[0]) continue;
    if(hist_n == HIST_MAX){
      memmove(hist[0], hist[1], sizeof(hist) - sizeof(hist[0]));
      hist_n--;
    }
    memcpy(hist[hist_n++], line, strlen(line)+1);
    shell_run_line(line, 0);
  }
}
