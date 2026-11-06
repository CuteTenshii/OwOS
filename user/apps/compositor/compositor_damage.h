#ifndef OWOS_COMPOSITOR_DAMAGE_H
#define OWOS_COMPOSITOR_DAMAGE_H

#define MAXDMG 16

struct rect {
  int x, y, w, h;
};

extern struct rect dmg[MAXDMG];
extern int ndmg;

void damage_add(int x, int y, int w, int h);
void damage_add_union(int x0, int y0, int w0, int h0, int x1, int y1, int w1, int h1);

#endif
