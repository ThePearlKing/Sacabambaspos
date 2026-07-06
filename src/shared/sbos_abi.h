/* sbos_abi.h - the kernel <-> userland contract: syscall numbers, key codes,
 * console colors. Included by the kernel and by every user program; keep it
 * dependency-free. Syscalls use the syscall instruction, Linux-flavored:
 * rax = number, args in rdi/rsi/rdx, result in rax, rcx+r11 clobbered,
 * everything else preserved. */
#ifndef SBOS_ABI_H
#define SBOS_ABI_H

#define SYS_EXIT    0   /* exit(code)            never returns              */
#define SYS_WRITE   1   /* write(buf, len)       text to the console        */
#define SYS_GETKEY  2   /* getkey(block)         key or -1 (block = wait)   */
#define SYS_SLEEP   3   /* sleep(ms)                                        */
#define SYS_MS      4   /* ms()                  milliseconds since boot    */
#define SYS_COLOR   5   /* color(fg, bg)         SBOS_C_* palette indices   */
#define SYS_SETXY   6   /* setxy(col, row)       move the cursor            */
#define SYS_CONSIZE 7   /* consize()             cols << 16 | rows          */
#define SYS_CLEAR   8   /* clear()                                          */
#define SYS_CURSOR  9   /* cursor(on)            show/hide the cell cursor  */

/* getkey() results past ASCII */
#define SBOS_KEY_UP    0x100
#define SBOS_KEY_DOWN  0x101
#define SBOS_KEY_LEFT  0x102
#define SBOS_KEY_RIGHT 0x103
#define SBOS_KEY_DEL   0x104
#define SBOS_KEY_HOME  0x105
#define SBOS_KEY_END   0x106

/* console palette (VGA-ish) */
enum {
  SBOS_C_BLACK, SBOS_C_BLUE,  SBOS_C_GREEN,  SBOS_C_CYAN,
  SBOS_C_RED,   SBOS_C_MAGENTA, SBOS_C_BROWN, SBOS_C_LGREY,
  SBOS_C_DGREY, SBOS_C_LBLUE, SBOS_C_LGREEN, SBOS_C_LCYAN,
  SBOS_C_LRED,  SBOS_C_PINK,  SBOS_C_YELLOW, SBOS_C_WHITE,
};

#endif
