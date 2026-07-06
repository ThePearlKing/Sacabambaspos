/* crash.c - deliberately fault, to show ring 3 can't hurt the kernel.
 * The store below hits an identity-mapped kernel page whose entries carry
 * no U bit, so the CPU raises #PF at CPL 3 and the kernel kills only us. */
#include "sbos.h"

int main(void){
  color(SBOS_C_LGREY, SBOS_C_BLACK);
  print("  writing to kernel memory from ring 3 in 3... 2... 1...\n");
  sleep_ms(400);
  *(volatile unsigned long*)0x1000 = 0xDEADBEEF;
  print("  ...this line can never print\n");
  return 0;
}
