/* c2int.h - canvas2d internals shared between its translation units. */
#ifndef OWOS_CANVAS2D_INT_H
#define OWOS_CANVAS2D_INT_H

#include "canvas2d.h"

#define C2D_PI 3.14159265358979323846f

bool c2d_grow(void **buf, int *cap, int need, unsigned elem);

/* Anti-aliased coverage of the closed subpaths in `p` over the box [bx,bx+bw) x [by,by+bh); out is bw*bh bytes. */
bool c2d_rasterize(const struct c2d_path *p, bool evenodd, int bx, int by, int bw, int bh, unsigned char *out);

/* Polygons covering the stroke of `p` (fill them nonzero). dash may be NULL. */
bool c2d_stroke_to_path(const struct c2d_path *p, float width, int cap, int join, float miter_limit, const float *dash,
                        int ndash, float dash_off, struct c2d_path *out);

/* Device-space bounds of all points, clamped to the surface; false if empty. */
bool c2d_path_bounds(const struct c2d_path *p, float pad, int w, int h, int *x0, int *y0, int *x1, int *y1);

/* A resolved paint: what colour a device pixel gets. */
struct c2d_src {
  int kind;
  unsigned char col[4];
  const struct c2d_gradient *grad;
  const unsigned char *img; /* pattern / image pixels */
  int iw, ih;
  int cx0, cy0, cx1, cy1; /* sampling clamp box inside img */
  bool repeat_x, repeat_y, edge_clamp, smooth;
  struct c2d_mat inv; /* device -> paint space */
};

void c2d_src_color(struct c2d_src *s, const unsigned char rgba[4]);
bool c2d_src_from_paint(struct c2d_src *s, const struct c2d_paint *p, const struct c2d_mat *ctm, bool smooth);

/* Paint `src` through coverage `cov` (bw*bh at bx,by; NULL = full) onto the surface honouring clip, alpha and the
 * composite operator. The shadow, if any, is drawn first when `shadow` is set. cov is consumed (may be modified). */
void c2d_draw_cov(struct c2d *c, unsigned char *cov, int bx, int by, int bw, int bh, const struct c2d_src *src,
                  float alpha, int comp, bool shadow);

#endif /* OWOS_CANVAS2D_INT_H */
