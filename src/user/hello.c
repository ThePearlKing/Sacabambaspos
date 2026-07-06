/* hello.c - the first ring-3 program SacabambaspOS ever ran. */
#include "sbos.h"

int main(void){
  color(SBOS_C_LCYAN, SBOS_C_BLACK);
  print("  hello from ring 3!\n");
  color(SBOS_C_LGREY, SBOS_C_BLACK);
  print("  this text traveled: user code -> syscall -> kernel console.\n");
  print("  the kernel is ");
  color(SBOS_C_YELLOW, SBOS_C_BLACK);
  printu(now_ms() / 1000);
  color(SBOS_C_LGREY, SBOS_C_BLACK);
  print(" seconds old and cannot be touched from here -\n");
  print("  every kernel page is unmapped for user code. try ");
  color(SBOS_C_LGREEN, SBOS_C_BLACK);
  print("crash");
  color(SBOS_C_LGREY, SBOS_C_BLACK);
  print(" and watch\n  the kernel shrug it off.\n");
  return 0;
}
