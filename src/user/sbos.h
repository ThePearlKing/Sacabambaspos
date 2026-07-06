/* sbos.h - the SacabambaspOS userland library, such as it is.
 * Syscall stubs plus the handful of helpers every program wants. Programs
 * are freestanding: no libc, no heap, just this header and the kernel. */
#ifndef SBOS_USER_H
#define SBOS_USER_H

#include "../shared/sbos_abi.h"

typedef unsigned char       u8;
typedef unsigned short      u16;
typedef unsigned int        u32;
typedef unsigned long long  u64;

static inline long __sys(long n, long a, long b, long c){
  long r;
  __asm__ __volatile__("syscall"
    : "=a"(r)
    : "a"(n), "D"(a), "S"(b), "d"(c)
    : "rcx", "r11", "memory");
  return r;
}

static inline void exit(int code){ __sys(SYS_EXIT, code, 0, 0); __builtin_unreachable(); }
static inline void writen(const char *s, unsigned long n){ __sys(SYS_WRITE, (long)s, (long)n, 0); }
static inline int  getkey(int block){ return (int)__sys(SYS_GETKEY, block, 0, 0); }
static inline void sleep_ms(unsigned ms){ __sys(SYS_SLEEP, ms, 0, 0); }
static inline unsigned long now_ms(void){ return (unsigned long)__sys(SYS_MS, 0, 0, 0); }
static inline void color(int fg, int bg){ __sys(SYS_COLOR, fg, bg, 0); }
static inline void setxy(int x, int y){ __sys(SYS_SETXY, x, y, 0); }
static inline int  cols(void){ return (int)(__sys(SYS_CONSIZE,0,0,0) >> 16); }
static inline int  rows(void){ return (int)(__sys(SYS_CONSIZE,0,0,0) & 0xFFFF); }
static inline void clear(void){ __sys(SYS_CLEAR, 0, 0, 0); }
static inline void cursor(int on){ __sys(SYS_CURSOR, on, 0, 0); }

static inline unsigned long slen(const char *s){
  unsigned long n = 0; while(s[n]) n++; return n;
}
static inline void print(const char *s){ writen(s, slen(s)); }
static inline void putch(char c){ writen(&c, 1); }

static inline void printu(unsigned long v){
  char b[21]; int i = 20; b[20] = 0;
  do { b[--i] = '0' + v % 10; v /= 10; } while(v);
  print(b + i);
}

/* xorshift64 - good enough for games, seed with now_ms() */
static inline unsigned long rnd_next(unsigned long *s){
  unsigned long x = *s ? *s : 88172645463325252UL;
  x ^= x << 13; x ^= x >> 7; x ^= x << 17;
  return *s = x;
}

int main(void);

#endif
