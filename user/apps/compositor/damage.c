/* damage.c: dirty-rect list for partial repaints */
#include "comp.h"

#include <stdbool.h>

struct rect dmg[MAXDMG];
int ndmg;

void damage_add(int x, int y, int w, int h)
{
  if (w <= 0 || h <= 0)
    return;
  for (;;) {
    bool merged = false;
    for (int i = 0; i < ndmg; i++) {
      struct rect *r = &dmg[i];
      int x1 = x + w, y1 = y + h;
      int r1 = r->x + r->w, r2 = r->y + r->h;
      if (x >= r1 || r->x >= x1 || y >= r2 || r->y >= y1)
        continue;
      int nx = x < r->x ? x : r->x, ny = y < r->y ? y : r->y;
      int nx1 = x1 > r1 ? x1 : r1, ny1 = y1 > r2 ? y1 : r2;
      x = nx;
      y = ny;
      w = nx1 - nx;
      h = ny1 - ny;
      dmg[i] = dmg[--ndmg];
      merged = true;
      break;
    }
    if (!merged)
      break;
  }
  if (ndmg >= MAXDMG) {
    /* Never drop damage: collapse the list to its bounding rectangle */
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    for (int i = 0; i < ndmg; i++) {
      int r1 = dmg[i].x + dmg[i].w, r2 = dmg[i].y + dmg[i].h;
      if (dmg[i].x < x0)
        x0 = dmg[i].x;
      if (dmg[i].y < y0)
        y0 = dmg[i].y;
      if (r1 > x1)
        x1 = r1;
      if (r2 > y1)
        y1 = r2;
    }
    dmg[0].x = x0;
    dmg[0].y = y0;
    dmg[0].w = x1 - x0;
    dmg[0].h = y1 - y0;
    ndmg = 1;
    return;
  }
  dmg[ndmg].x = x;
  dmg[ndmg].y = y;
  dmg[ndmg].w = w;
  dmg[ndmg].h = h;
  ndmg++;
}

void damage_add_union(int x0, int y0, int w0, int h0, int x1, int y1, int w1, int h1)
{
  int ax0 = x0 < x1 ? x0 : x1, ay0 = y0 < y1 ? y0 : y1;
  int ax1 = (x0 + w0) > (x1 + w1) ? (x0 + w0) : (x1 + w1);
  int ay1 = (y0 + h0) > (y1 + h1) ? (y0 + h0) : (y1 + h1);
  damage_add(ax0, ay0, ax1 - ax0, ay1 - ay0);
}
