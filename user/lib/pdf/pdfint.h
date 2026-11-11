/* pdfint.h, internals shared by the pdf library's files */
#ifndef OWOS_PDFINT_H
#define OWOS_PDFINT_H

#include "canvas2d.h"
#include "font.h"
#include "pdf.h"

enum { PO_NULL, PO_BOOL, PO_INT, PO_REAL, PO_STR, PO_NAME, PO_ARR, PO_DICT, PO_STREAM, PO_REF, PO_KW };

struct pkv;
struct pobj {
  unsigned char t;
  int n;   /* BOOL/INT value; STR/NAME/KW length; ARR/DICT/STREAM count; REF object number */
  int aux; /* STREAM: data offset in the file; REF: generation */
  union {
    double r;
    const char *s;
    struct pobj *a;
    struct pkv *kv;
  } u;
};
struct pkv {
  const char *k;
  struct pobj v;
};

/* Bump allocator for everything a parse produces; freed in one go. */
struct ablock;
struct arena {
  struct ablock *head;
};
void *arena_alloc(struct arena *a, unsigned n); /* zeroed; NULL when out of memory */
void arena_reset(struct arena *a);              /* frees all but the first block */
void arena_free(struct arena *a);

struct lex {
  const unsigned char *b;
  int len, pos;
  struct arena *ar;
  int refs; /* fold "n g R" into a reference (file objects, not content streams) */
};
int pdf_lex(struct lex *l, struct pobj *o); /* 1, or 0 at the end of input */

struct xent {
  unsigned char type;  /* 0 unset, 1 in the file, 2 in an object stream, 3 free */
  unsigned char state; /* 0 unloaded, 1 loading, 2 loaded */
  int a, b;            /* file: offset, generation; objstm: stream number, index */
  struct pobj obj;
};

struct ostm {
  int num;
  unsigned char *data;
  int len, n, first;
  int *nums, *offs;
};

struct ppage {
  struct pobj dict, res, media, crop;
  int rot;
};

struct pfont;

struct pdf {
  const unsigned char *buf;
  int len;
  struct arena ar;
  struct xent *x;
  int nx, xcap;
  struct pobj trailer;
  struct ostm os[8];
  int os_next;
  struct ppage *pages;
  int npages, pcap;
  int rebuilt;
  const struct font *sans, *mono;
  struct pfont *fonts;
};

/* objects */
struct pobj pdf_deref(struct pdf *d, struct pobj o);
struct pobj pdf_get(struct pdf *d, struct pobj dict, const char *key);    /* dereferenced; null if absent */
struct pobj pdf_getraw(struct pdf *d, struct pobj dict, const char *key); /* as written (may be a reference) */
struct pobj pdf_at(struct pdf *d, struct pobj arr, int i);
double pdf_num(struct pobj o, double def);
int pdf_name_is(struct pobj o, const char *s);
int pdf_is_dict(struct pobj o);
struct pobj pdf_null(void);

/* filters */
unsigned char *pdf_decode(struct pdf *d, const unsigned char *in, int n, struct pobj filter, struct pobj parms,
                          int *outlen, const char **left, struct pobj *left_parms);
unsigned char *pdf_stream_data(struct pdf *d, struct pobj st, int *len, const char **left, struct pobj *left_parms);

/* functions (PDF 1.7 section 7.10) */
struct pfunc;
struct pfunc *pdf_func_load(struct pdf *d, struct arena *ar, struct pobj fn);
int pdf_func_eval(const struct pfunc *f, const double *in, int nin, double *out, int nout_max); /* outputs written */

/* rendering */
enum { CS_GRAY, CS_RGB, CS_CMYK, CS_LAB, CS_INDEXED, CS_TINT, CS_PATTERN };

struct cspace {
  int kind, n;
  struct cspace *alt; /* Indexed: the base space; Tint: the alternate space; Pattern: the underlying space */
  int hival, nlookup;
  const unsigned char *lookup;
  struct pfunc *fn;
  double range[4]; /* Lab a* and b* limits */
};

struct color {
  struct cspace *cs;
  double c[8];
  unsigned char rgb[3];
  struct pobj pattern; /* the pattern dictionary when cs is a Pattern space */
};

struct gstate {
  struct c2d_mat ctm;
  struct color fill, stroke;
  double lw, miter, dash[8], dphase;
  int cap, join, ndash;
  double fa, sa;
  struct pfont *font;
  double fs, tc, tw, th, tl, rise;
  int tr;
};

struct gcache_ent {
  const struct font *f;
  int gid, px8;
  unsigned char *bmp;
  int w, h, ox, oy;
};

struct rctx {
  struct pdf *d;
  struct c2d *c;
  struct c2d_mat base;
  struct gstate gs, stack[32];
  int sp;
  struct pobj res;
  struct c2d_path path;
  int clip_pending; /* 0 none, 1 nonzero, 2 even-odd */
  double tm[6], tlm[6];
  struct arena ar, tmp;
  int depth;
  long ops;
  struct gcache_ent glyphs[256];
  struct {
    const void *res;
    char name[24];
    struct cspace *cs;
  } cs_cache[16];
  int cs_next;
};

void pdf_set_color_rgb(struct color *c, struct cspace *cs, const double *comp, int n);
struct cspace *pdf_cs_parse(struct rctx *rc, struct pobj o, int depth);
void pdf_color_update(struct color *c);
void pdf_cs_rgb(const struct cspace *cs, const double *c, unsigned char *rgb);
void pdf_draw_image(struct rctx *rc, struct pobj dict, unsigned char *data, int len, const char *left, struct pobj lp);
void pdf_show_text(struct rctx *rc, const unsigned char *s, int n);
void pdf_glyphs_free(struct rctx *rc);
void pdf_paint_shading(struct rctx *rc, struct pobj sh, const struct c2d_mat *m);
void pdf_run_content(struct rctx *rc, const unsigned char *data, int len);

/* fonts */
struct pfont *pdf_font_load(struct pdf *d, struct pobj ref_or_dict);
void pdf_fonts_free(struct pdf *d);
struct glyph_ref {
  const struct font *f;
  int gid;
  int ok;
};
/* Next character code of `s` (1 or 2 bytes by font kind); width in 1/1000 em; glyph to draw. */
int pdf_font_next(struct pdf *d, struct pfont *pf, const unsigned char *s, int n, int *code, double *w,
                  struct glyph_ref *g, int *is_space);

#endif
