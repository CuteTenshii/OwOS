/* canvas2d.h - software 2D drawing surface behind the HTML canvas 2D context.
 *
 * Straight (non-premultiplied) RGBA8 pixels, a transform/state stack, paths with
 * anti-aliased even-odd/nonzero fills, strokes (joins, caps, dashes), gradients,
 * patterns, clipping, shadows, Porter-Duff and separable blend compositing, image
 * blits and text through the font rasterizer. No dependency on the browser. Paths are
 * stored flattened in device space (points are transformed when added), so a later
 * transform change never moves what was already built. */
#ifndef OWOS_CANVAS2D_H
#define OWOS_CANVAS2D_H

#include <stdbool.h>

struct font;

struct c2d_mat {
  float a, b, c, d, e, f; /* x' = a*x + c*y + e, y' = b*x + d*y + f */
};

struct c2d_sub {
  int start, n;
  bool closed;
};

struct c2d_path {
  float *pt; /* x,y pairs */
  int npt, ptcap;
  struct c2d_sub *sub;
  int nsub, subcap;
  float ux, uy;   /* current point in the space the caller builds in */
  float sux, suy; /* start of the current subpath, same space */
};

struct c2d_stop {
  float off;
  unsigned char r, g, b, a;
};

enum c2d_paint_kind { C2D_COLOR, C2D_LINEAR, C2D_RADIAL, C2D_PATTERN };

struct c2d_gradient {
  int refs;
  int kind; /* C2D_LINEAR or C2D_RADIAL */
  float x0, y0, r0, x1, y1, r1;
  struct c2d_stop *stop;
  int nstop, cap;
};

struct c2d_pattern {
  int refs;
  unsigned char *rgba; /* owned copy */
  int w, h;
  bool repeat_x, repeat_y;
};

struct c2d_paint {
  int kind;
  unsigned char r, g, b, a;
  struct c2d_gradient *grad;
  struct c2d_pattern *pat;
};

enum c2d_cap { C2D_BUTT, C2D_ROUND, C2D_SQUARE };
enum c2d_join { C2D_MITER, C2D_ROUND_JOIN, C2D_BEVEL };
enum c2d_align { C2D_ALIGN_START, C2D_ALIGN_END, C2D_ALIGN_LEFT, C2D_ALIGN_RIGHT, C2D_ALIGN_CENTER };
enum c2d_baseline {
  C2D_BASE_ALPHABETIC,
  C2D_BASE_TOP,
  C2D_BASE_HANGING,
  C2D_BASE_MIDDLE,
  C2D_BASE_IDEO,
  C2D_BASE_BOTTOM
};

enum c2d_comp {
  C2D_SOURCE_OVER,
  C2D_SOURCE_IN,
  C2D_SOURCE_OUT,
  C2D_SOURCE_ATOP,
  C2D_DEST_OVER,
  C2D_DEST_IN,
  C2D_DEST_OUT,
  C2D_DEST_ATOP,
  C2D_LIGHTER,
  C2D_COPY,
  C2D_XOR,
  C2D_MULTIPLY,
  C2D_SCREEN,
  C2D_OVERLAY,
  C2D_DARKEN,
  C2D_LIGHTEN,
  C2D_COLOR_DODGE,
  C2D_COLOR_BURN,
  C2D_HARD_LIGHT,
  C2D_SOFT_LIGHT,
  C2D_DIFFERENCE,
  C2D_EXCLUSION,
  C2D_COMP_COUNT
};

struct c2d_mask {
  int refs;
  unsigned char px[];
};

struct c2d_state {
  struct c2d_mat m;
  struct c2d_paint fill, stroke;
  float line_width, miter_limit, dash_off;
  float *dash;
  int ndash;
  int cap, join;
  float alpha;
  int comp;
  unsigned char shadow_r, shadow_g, shadow_b, shadow_a;
  float shadow_blur, shadow_ox, shadow_oy;
  float font_px;
  bool font_mono, font_bold;
  int text_align, text_base;
  bool smoothing;
  struct c2d_mask *clip; /* NULL = unclipped */
};

struct c2d {
  int w, h;
  unsigned char *px; /* w*h*4 */
  struct c2d_state *st;
  int depth, stcap;
  struct c2d_path path;
  struct font *sans, *mono; /* borrowed; NULL = text is skipped */
};

/* ---- matrices ---- */
extern const struct c2d_mat C2D_IDENTITY;
struct c2d_mat c2d_mat_mul(struct c2d_mat l, struct c2d_mat r); /* l * r: r applies first */
bool c2d_mat_invert(const struct c2d_mat *m, struct c2d_mat *out);
void c2d_mat_apply(const struct c2d_mat *m, float x, float y, float *ox, float *oy);

/* ---- surface and state ---- */
struct c2d *c2d_new(int w, int h); /* NULL on bad size or no memory */
void c2d_free(struct c2d *c);
bool c2d_resize(struct c2d *c, int w, int h); /* clears and resets all state */
void c2d_save(struct c2d *c);
void c2d_restore(struct c2d *c);
void c2d_reset_state(struct c2d *c);
void c2d_set_paint_color(struct c2d_paint *p, unsigned char r, unsigned char g, unsigned char b, unsigned char a);
void c2d_set_paint_gradient(struct c2d_paint *p, struct c2d_gradient *g);
void c2d_set_paint_pattern(struct c2d_paint *p, struct c2d_pattern *pat);
bool c2d_set_dash(struct c2d *c, const float *d, int n);

/* ---- gradients and patterns (refcounted) ---- */
struct c2d_gradient *c2d_gradient_new(int kind, float x0, float y0, float r0, float x1, float y1, float r1);
bool c2d_gradient_add_stop(struct c2d_gradient *g, float off, unsigned char r, unsigned char gr, unsigned char b,
                           unsigned char a);
void c2d_gradient_unref(struct c2d_gradient *g);
struct c2d_pattern *c2d_pattern_new(const unsigned char *rgba, int w, int h, bool rx, bool ry);
void c2d_pattern_unref(struct c2d_pattern *p);

/* ---- paths (m = transform applied to each point; NULL = identity) ---- */
void c2d_path_free(struct c2d_path *p);
void c2d_path_clear(struct c2d_path *p);
bool c2d_path_copy(struct c2d_path *dst, const struct c2d_path *src);
bool c2d_path_add(struct c2d_path *dst, const struct c2d_path *src, const struct c2d_mat *m);
void c2d_path_move(struct c2d_path *p, const struct c2d_mat *m, float x, float y);
void c2d_path_line(struct c2d_path *p, const struct c2d_mat *m, float x, float y);
void c2d_path_quad(struct c2d_path *p, const struct c2d_mat *m, float cx, float cy, float x, float y);
void c2d_path_bezier(struct c2d_path *p, const struct c2d_mat *m, float c1x, float c1y, float c2x, float c2y, float x,
                     float y);
bool c2d_path_arc_to(struct c2d_path *p, const struct c2d_mat *m, float x1, float y1, float x2, float y2, float r);
bool c2d_path_arc(struct c2d_path *p, const struct c2d_mat *m, float x, float y, float r, float a0, float a1, bool ccw);
bool c2d_path_ellipse(struct c2d_path *p, const struct c2d_mat *m, float x, float y, float rx, float ry, float rot,
                      float a0, float a1, bool ccw);
void c2d_path_rect(struct c2d_path *p, const struct c2d_mat *m, float x, float y, float w, float h);
void c2d_path_round_rect(struct c2d_path *p, const struct c2d_mat *m, float x, float y, float w, float h,
                         const float *radii /* tl, tr, br, bl */);
void c2d_path_close(struct c2d_path *p);
bool c2d_path_has_current(const struct c2d_path *p);
bool c2d_path_contains(const struct c2d_path *p, float x, float y, bool evenodd);
bool c2d_path_stroke_contains(const struct c2d_path *p, float x, float y, float width);

/* ---- drawing; paths are in device space already (see above) ---- */
void c2d_fill(struct c2d *c, const struct c2d_path *p, bool evenodd);
void c2d_stroke(struct c2d *c, const struct c2d_path *p);
void c2d_clip(struct c2d *c, const struct c2d_path *p, bool evenodd);
void c2d_fill_rect(struct c2d *c, float x, float y, float w, float h);
void c2d_stroke_rect(struct c2d *c, float x, float y, float w, float h);
void c2d_clear_rect(struct c2d *c, float x, float y, float w, float h);
void c2d_draw_image(struct c2d *c, const unsigned char *rgba, int iw, int ih, float sx, float sy, float sw, float sh,
                    float dx, float dy, float dw, float dh);
void c2d_get_image_data(const struct c2d *c, int x, int y, int w, int h, unsigned char *out);
void c2d_put_image_data(struct c2d *c, const unsigned char *rgba, int w, int h, int dx, int dy, int sx, int sy, int sw,
                        int sh);

/* ---- text: UTF-8; (x,y) in user space ---- */
void c2d_fill_text(struct c2d *c, const char *utf8, float x, float y, bool stroke_paint);
float c2d_measure_text(const struct c2d *c, const char *utf8);

/* Uncompressed PNG of the surface; malloc'd, NULL on failure. */
unsigned char *c2d_encode_png(const struct c2d *c, int *len);

#endif /* OWOS_CANVAS2D_H */
