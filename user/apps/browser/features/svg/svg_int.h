/* svg_int.h: shared types and cross-file helpers of the SVG renderer. */
#ifndef OWOS_SVG_INT_H
#define OWOS_SVG_INT_H

#include "color.h"
#include "dom.h"
#include "fmt.h"
#include "math.h"
#include "owos.h"
#include <stdbool.h>

#define SUBS 8 /* coverage samples per pixel row */
#define MAX_STOPS 32
#define MAX_DASH 16
#define MAX_RULES 256
#define MAX_DEPTH 48
#define MAX_LAYERS 8
#define MAX_PIXELS (16 << 20)
#define ELEM_BUDGET 100000
#define SVG_PI 3.14159265f
#define ABSF(v) ((v) < 0 ? -(v) : (v))

/* affine matrix: x' = a x + c y + e,  y' = b x + d y + f */
struct mat {
  float a, b, c, d, e, f;
};

/* paths */
struct path {
  float *x, *y;
  int n, cap;
  int *sub; /* start index of each subpath */
  unsigned char *closed;
  int ns, scap;
  int *vtx; /* indices of the points that are path vertices (marker positions) */
  int nv, vcap;
};

struct bb {
  float x0, y0, x1, y1;
  bool ok;
};

/* preserveAspectRatio */
struct par {
  int ax, ay; /* 0 min, 1 mid, 2 max */
  bool none, slice;
};

/* style sheet and property lookup */
struct rule {
  char tag[32], cls[64], id[64];
  int spec;
  const char *decl;
  int dlen;
};

struct idref {
  const char *id;
  const struct dom_node *n;
};

#define PAINT_NONE 0

#define PAINT_COLOR 1

#define PAINT_CURRENT 2

#define PAINT_URL 3

struct paint {
  int kind;
  int rgb, alpha;
  const struct dom_node *ref;
};

enum { CAP_BUTT, CAP_ROUND, CAP_SQUARE };

enum { JOIN_MITER, JOIN_ROUND, JOIN_BEVEL };

/* Inherited drawing state. */
struct style {
  struct paint fill, stroke;
  float fill_op, stroke_op;
  float sw, miter;
  int cap, join;
  float dash[MAX_DASH];
  int ndash;
  float dash_off;
  int color; /* 0xRRGGBB, what currentColor resolves to */
  bool evenodd, hidden;
  float font_size, letter_spacing;
  int anchor; /* text-anchor: 0 start, 1 middle, 2 end */
  bool bold, mono;
  const struct dom_node *marker[3]; /* marker-start, -mid, -end */
};

struct ctx {
  const struct dom_node *root;
  int w, h;
  unsigned *buf; /* current target, premultiplied ARGB */
  unsigned *layers[MAX_LAYERS];
  int nlayers;
  unsigned char *clip; /* active clip mask, or NULL */
  float *acc;
  float *xs;
  int *wd;
  int xcap;
  struct path tp[3];  /* scratch: user path, device path, stroke outline */
  struct path aux[2]; /* scratch: de-duplicated points, current dash */
  struct rule *rules;
  int nrules;
  struct idref *ids;
  int nids, idcap;
  float vw, vh; /* viewport in user units, for % lengths */
  int budget, use_depth;
  int sub_depth; /* nesting of patterns, masks and images */
};

/* paint sources */
struct src {
  bool grad, radial;
  unsigned rgb;
  int alpha;  /* solid alpha, or the gradient's opacity (0..255) */
  int spread; /* 0 pad, 1 reflect, 2 repeat */
  struct mat inv;
  float x1, y1, vx, vy, ilen2; /* linear */
  float fx, fy, cfx, cfy, ga;  /* radial */
  unsigned lut[256];           /* straight ARGB */
  bool tex, repeat;            /* texture: pattern tile or image; `inv` maps device pixels to texels */
  unsigned *pix;               /* premultiplied ARGB texels, owned (see svg_src_free) */
  int tw, th;
};

struct stop {
  float off;
  int rgb, alpha;
};

/* shapes */
enum { K_NONE, K_GROUP, K_SWITCH, K_SVG, K_USE, K_SHAPE, K_TEXT, K_IMAGE };

/* svg_geom.c */
struct mat svg_mat_id(void);
struct mat svg_mat_mul(struct mat A, struct mat B) /* apply B, then A */;
void svg_mat_apply(struct mat m, float x, float y, float *ox, float *oy);
bool svg_mat_inv(struct mat m, struct mat *out);
float svg_mat_scale(struct mat m);
struct mat svg_mat_translate(float x, float y);
bool svg_is_ws(char c);
bool svg_parse_num(const char **pp, const char *end, float *out);
bool svg_parse_len(const char *p, float ref, float *out);
float svg_attr_len(const struct dom_node *n, const char *name, float def, float ref);
float svg_parse_unit(const char *v, float def);
bool svg_parse_viewbox(const char *vb, float out[4]);
void svg_path_reset(struct path *p);
void svg_path_free(struct path *p);
bool svg_path_pt(struct path *p, float x, float y);
void svg_path_move(struct path *p, float x, float y);
void svg_path_line(struct path *p, float x, float y);
void svg_path_close(struct path *p);
int svg_sub_end(const struct path *p, int s);
void svg_path_xform(struct path *dst, const struct path *src, struct mat m);
void svg_bb_add(struct bb *b, float x, float y);
struct bb svg_path_bbox(const struct path *p, struct mat m);
void svg_path_arc(struct path *p, float scale, float x0, float y0, float rx, float ry, float phi_deg, bool large,
                  bool sweep, float x, float y);
void svg_parse_path(struct path *p, float scale, const char *d);
struct mat svg_parse_transform(const char *s);
struct par svg_parse_par(const char *v);
struct mat svg_viewbox_mat(const float vb[4], struct par par, float x, float y, float w, float h);

/* svg_style.c */
bool svg_get_prop(const struct ctx *cx, const struct dom_node *n, const char *name, char *out, int cap);
void svg_collect(struct ctx *cx, const struct dom_node *n, int depth);
const struct dom_node *svg_ref_target(const struct ctx *cx, const char *v);
const struct dom_node *svg_href_target(const struct ctx *cx, const struct dom_node *n);
void svg_resolve_style(const struct ctx *cx, const struct dom_node *n, struct style *s);
float svg_elem_opacity(const struct ctx *cx, const struct dom_node *n);
bool svg_display_none(const struct ctx *cx, const struct dom_node *n);

/* svg_paint.c */
unsigned svg_grad_px(const struct src *s, float px, float py);
bool svg_make_src(struct ctx *cx, const struct paint *p, float opacity, int color, const struct bb *bb, struct mat m,
                  struct src *s);
void svg_src_free(struct src *s);
const char *svg_chain_attr(const struct ctx *cx, const struct dom_node *g, const char *name, bool same_type);
float svg_grad_coord(const char *v, float def, bool obb, float ref);
unsigned svg_tex_px(const struct src *s, float px, float py);

/* svg_raster.c */
void svg_raster_fill(struct ctx *cx, const struct path *p, bool evenodd, const struct src *s, unsigned char *mout);
void svg_blend_mask(struct ctx *cx, const unsigned char *cov, int bx, int by, int bw, int bh, const struct src *s);
void svg_composite_layer(const struct ctx *cx, unsigned *dst, const unsigned *layer, float opacity);

/* svg_stroke.c */
void svg_stroke_build(struct ctx *cx, struct path *out, const struct path *src, const struct style *st, float scale);

/* svg_shape.c */
int svg_elem_kind(const char *t);
bool svg_build_shape(const struct ctx *cx, const struct dom_node *n, struct path *p, float scale);
unsigned char *svg_rect_clip(struct ctx *cx, struct mat m, float x, float y, float w, float h);
unsigned char *svg_make_mask(struct ctx *cx, const struct dom_node *mk, struct mat m, const struct dom_node *target,
                             int depth);
void svg_node_bbox(const struct ctx *cx, const struct dom_node *n, struct mat m, struct bb *b, int depth);
unsigned char *svg_make_clip(struct ctx *cx, const struct dom_node *cp, struct mat m, const struct dom_node *target,
                             int depth);

/* svg.c */
void svg_render_children(struct ctx *cx, const struct dom_node *n, struct mat m, const struct style *st, int depth,
                         bool first_only);
void svg_style_base(struct style *s, int color);

/* svg_tex.c: pattern paint and <image> */
bool svg_pattern_src(struct ctx *cx, const struct dom_node *pat, const struct bb *bb, struct mat m, float opacity,
                     int color, struct src *s);
void svg_draw_image(struct ctx *cx, const struct dom_node *n, struct mat m, const struct style *st);

/* svg_text.c */
void svg_draw_text(struct ctx *cx, const struct dom_node *n, struct mat m, const struct style *st);

/* svg_marker.c */
void svg_draw_markers(struct ctx *cx, const struct path *up, struct mat m, const struct style *st);

/* svg_filter.c */
bool svg_apply_filter(struct ctx *cx, const struct dom_node *f, unsigned *layer, struct mat m,
                      const struct dom_node *target);
void svg_path_mark(struct path *p);

#endif /* OWOS_SVG_INT_H */
