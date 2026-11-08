/* raster.c - scanline coverage of a flattened path: SS sub-scanlines per pixel row, exact horizontal span overlap. */
#include "c2int.h"
#include "math.h"
#include "owos.h"

#define SS 8

struct edge {
  float x0, y0, y1, dxdy;
  int dir;
};

static void sort_edges(struct edge *e, int n)
{
  for (int gap = n / 2; gap > 0; gap /= 2)
    for (int i = gap; i < n; i++) {
      struct edge t = e[i];
      int j = i;
      for (; j >= gap && e[j - gap].y0 > t.y0; j -= gap)
        e[j] = e[j - gap];
      e[j] = t;
    }
}

static int collect_edges(const struct c2d_path *p, float ytop, float ybot, struct edge *e)
{
  int n = 0;
  for (int s = 0; s < p->nsub; s++) {
    const struct c2d_sub *sb = &p->sub[s];
    if (sb->n < 2)
      continue;
    for (int i = 0; i < sb->n; i++) {
      int j = i + 1 == sb->n ? 0 : i + 1;
      float ax = p->pt[(sb->start + i) * 2], ay = p->pt[(sb->start + i) * 2 + 1];
      float bx = p->pt[(sb->start + j) * 2], by = p->pt[(sb->start + j) * 2 + 1];
      if (ay == by || (ay <= ytop && by <= ytop) || (ay >= ybot && by >= ybot))
        continue;
      if (e) {
        int dir = 1;
        if (ay > by) {
          float t = ax;
          ax = bx;
          bx = t;
          t = ay;
          ay = by;
          by = t;
          dir = -1;
        }
        e[n] = (struct edge){ax, ay, by, (bx - ax) / (by - ay), dir};
      }
      n++;
    }
  }
  return n;
}

static void add_span(float *row, float *diff, int bw, float xa, float xb)
{
  if (xa < 0)
    xa = 0;
  if (xb > (float)bw)
    xb = (float)bw;
  if (xb <= xa)
    return;
  int ia = (int)xa, ib = (int)xb;
  if (ia == ib) {
    row[ia] += xb - xa;
    return;
  }
  row[ia] += (float)(ia + 1) - xa;
  diff[ia + 1] += 1;
  diff[ib] -= 1;
  row[ib] += xb - (float)ib;
}

bool c2d_rasterize(const struct c2d_path *p, bool evenodd, int bx, int by, int bw, int bh, unsigned char *out)
{
  memset(out, 0, (unsigned)bw * (unsigned)bh);
  int ne = collect_edges(p, (float)by, (float)(by + bh), 0);
  if (!ne)
    return true;
  struct edge *e = malloc((unsigned)ne * sizeof *e);
  float *row = calloc((unsigned)(bw + 2) * 2, sizeof *row);
  int *act = malloc((unsigned)ne * sizeof *act);
  float *xs = malloc((unsigned)ne * sizeof *xs);
  int *ds = malloc((unsigned)ne * sizeof *ds);
  if (!e || !row || !act || !xs || !ds) {
    free(e);
    free(row);
    free(act);
    free(xs);
    free(ds);
    return false;
  }
  float *diff = row + bw + 2;
  collect_edges(p, (float)by, (float)(by + bh), e);
  sort_edges(e, ne);
  int next = 0, nact = 0;
  for (int y = 0; y < bh; y++) {
    for (int k = 0; k < SS; k++) {
      float sy = (float)(by + y) + ((float)k + 0.5f) / SS;
      while (next < ne && e[next].y0 <= sy)
        act[nact++] = next++;
      int m = 0, nx = 0;
      for (int i = 0; i < nact; i++) {
        const struct edge *ed = &e[act[i]];
        if (ed->y1 <= sy)
          continue;
        act[m++] = act[i];
        float x = ed->x0 + (sy - ed->y0) * ed->dxdy - (float)bx;
        int j = nx++;
        for (; j > 0 && xs[j - 1] > x; j--) {
          xs[j] = xs[j - 1];
          ds[j] = ds[j - 1];
        }
        xs[j] = x;
        ds[j] = ed->dir;
      }
      nact = m;
      int wind = 0;
      float start = 0;
      for (int i = 0; i < nx; i++) {
        bool was = evenodd ? (wind & 1) != 0 : wind != 0;
        wind += evenodd ? 1 : ds[i];
        bool now = evenodd ? (wind & 1) != 0 : wind != 0;
        if (!was && now)
          start = xs[i];
        else if (was && !now)
          add_span(row, diff, bw, start, xs[i]);
      }
    }
    float run = 0;
    unsigned char *o = out + (unsigned long)y * bw;
    for (int x = 0; x < bw; x++) {
      run += diff[x];
      float v = (row[x] + run) * (255.0f / SS) + 0.5f;
      o[x] = v >= 255 ? 255 : v <= 0 ? 0 : (unsigned char)v;
      row[x] = 0;
      diff[x] = 0;
    }
    row[bw] = row[bw + 1] = diff[bw] = diff[bw + 1] = 0;
  }
  free(e);
  free(row);
  free(act);
  free(xs);
  free(ds);
  return true;
}
