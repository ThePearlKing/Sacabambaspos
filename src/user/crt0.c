/* crt0.c - userland entry: call main, exit with its return value.
 * The kernel enters at _start with a fresh 16-aligned stack and zeroed
 * registers; .bss is already zero (fresh pages). */
#include "sbos.h"

void _start(void){
  exit(main());
}
