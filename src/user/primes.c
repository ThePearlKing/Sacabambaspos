/* primes.c - sieve of Eratosthenes in ring 3, with a stopwatch on it. */
#include "sbos.h"

#define N 100000
static u8 comp[N + 1];          /* .bss: fresh zeroed pages from the kernel */

int main(void){
  unsigned long t0 = now_ms();
  int count = 0, last = 0;
  for(int i = 2; i <= N; i++){
    if(comp[i]) continue;
    count++; last = i;
    for(int j = i + i; j <= N; j += i) comp[j] = 1;
  }
  unsigned long dt = now_ms() - t0;

  color(SBOS_C_LGREY, SBOS_C_BLACK);
  print("  primes up to ");   printu(N);
  print(": ");
  color(SBOS_C_LCYAN, SBOS_C_BLACK);  printu(count);
  color(SBOS_C_LGREY, SBOS_C_BLACK);
  print("  (largest ");
  color(SBOS_C_YELLOW, SBOS_C_BLACK); printu(last);
  color(SBOS_C_LGREY, SBOS_C_BLACK);
  print(")  in ");
  color(SBOS_C_LGREEN, SBOS_C_BLACK); printu(dt);
  color(SBOS_C_LGREY, SBOS_C_BLACK);
  print(" ms\n  the last ten: ");
  int shown = 0;
  for(int i = N; i >= 2 && shown < 10; i--){
    if(comp[i]) continue;
    color(SBOS_C_LCYAN, SBOS_C_BLACK); printu(i);
    if(++shown < 10){ color(SBOS_C_DGREY, SBOS_C_BLACK); print(", "); }
  }
  print("\n");
  return 0;
}
