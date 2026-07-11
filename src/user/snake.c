/* snake.c - the obligatory snake, as a ring-3 SacabambaspOS program.
 * Arrows steer, q quits. Walls and your own tail are fatal, as is tradition. */
#include "sbos.h"

#define MAXW 128
#define MAXH 64

static u16 body[MAXW * MAXH];        /* ring buffer of x<<8|y, tail..head */
static u8  occ[MAXW][MAXH];
static int W, H;

static void cell(int x, int y, char c, int fg){
  setxy(x, y);
  color(fg, SBOS_C_BLACK);
  writen(&c, 1);
}

static void status(int score){
  setxy(2, 0);
  color(SBOS_C_YELLOW, SBOS_C_BLACK);
  print("snake  ");
  color(SBOS_C_LGREY, SBOS_C_BLACK);
  print("score ");
  color(SBOS_C_LCYAN, SBOS_C_BLACK);
  printu(score);
  print("   ");
}

int main(void){
  W = cols(); H = rows();
  if(W > MAXW) W = MAXW;
  if(H > MAXH) H = MAXH;
  if(W < 20 || H < 10){
    print("  console too small for snake\n");
    return 1;
  }

  clear(); cursor(0);
  unsigned long seed = now_ms() | 1;

  /* border: rows 1 and H-1, cols 0 and W-1; row 0 is the status line */
  color(SBOS_C_DGREY, SBOS_C_BLACK);
  for(int x = 0; x < W; x++){ cell(x, 1, '-', SBOS_C_DGREY); cell(x, H-1, '-', SBOS_C_DGREY); }
  for(int y = 1; y < H; y++){ cell(0, y, '|', SBOS_C_DGREY); cell(W-1, y, '|', SBOS_C_DGREY); }

  /* constant speed: the danger comes from your own growing tail */
  int hx = W/2, hy = H/2, dx = 1, dy = 0;
  int tail = 0, head = 0, score = 0, grow = 2;
  const int delay = 120;
  body[0] = (u16)(hx << 8 | hy);
  occ[hx][hy] = 1;
  cell(hx, hy, 'O', SBOS_C_LGREEN);

  int fx = hx, fy = hy;
  do {
    fx = 1 + (int)(rnd_next(&seed) % (W - 2));
    fy = 2 + (int)(rnd_next(&seed) % (H - 3));
  } while(occ[fx][fy]);
  cell(fx, fy, '*', SBOS_C_LRED);
  status(0);

  for(;;){
    int k, ndx = dx, ndy = dy;
    while((k = getkey(0)) >= 0){
      if(k == 'q' || k == 27) goto over;
      if(k == SBOS_KEY_UP)    { ndx = 0;  ndy = -1; }
      if(k == SBOS_KEY_DOWN)  { ndx = 0;  ndy = 1;  }
      if(k == SBOS_KEY_LEFT)  { ndx = -1; ndy = 0;  }
      if(k == SBOS_KEY_RIGHT) { ndx = 1;  ndy = 0;  }
    }
    if(!(ndx == -dx && ndy == -dy)){ dx = ndx; dy = ndy; }  /* no U-turns */

    int px = hx, py = hy;
    hx += dx; hy += dy;
    if(hx <= 0 || hx >= W-1 || hy <= 1 || hy >= H-1 || occ[hx][hy]) break;

    cell(px, py, 'o', SBOS_C_GREEN);
    head = (head + 1) % (MAXW * MAXH);
    body[head] = (u16)(hx << 8 | hy);
    occ[hx][hy] = 1;
    cell(hx, hy, 'O', SBOS_C_LGREEN);

    if(hx == fx && hy == fy){
      score++; grow += 1;      /* og rules: one segment per food */
      status(score);
      do {
        fx = 1 + (int)(rnd_next(&seed) % (W - 2));
        fy = 2 + (int)(rnd_next(&seed) % (H - 3));
      } while(occ[fx][fy]);
      cell(fx, fy, '*', SBOS_C_LRED);
    }

    if(grow > 0) grow--;
    else {
      int tx = body[tail] >> 8, ty = body[tail] & 0xFF;
      occ[tx][ty] = 0;
      cell(tx, ty, ' ', SBOS_C_LGREY);
      tail = (tail + 1) % (MAXW * MAXH);
    }

    sleep_ms(delay);
  }

over:
  setxy(W/2 - 6, H/2);
  color(SBOS_C_WHITE, SBOS_C_RED);
  print(" GAME  OVER ");
  setxy(W/2 - 9, H/2 + 1);
  color(SBOS_C_LGREY, SBOS_C_BLACK);
  print("score ");  printu(score);
  print(" - any key");
  getkey(1);
  color(SBOS_C_LGREY, SBOS_C_BLACK);
  clear(); cursor(1);
  return 0;
}
