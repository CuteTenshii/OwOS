/* font.h — a small TrueType/OpenType font rasterizer.
 *
 * Parses an SFNT font (.ttf, or .otf with TrueType `glyf` outlines) from memory
 * and rasterizes glyphs to 8-bit antialiased coverage bitmaps at any pixel
 * size. General-purpose (the browser's @font-face, a font viewer, an editor's
 * font picker, …) — it has no dependency on the browser. Outlines are flattened
 * to polygons and scanline-filled with supersampled antialiasing.
 *
 * Scope: cmap formats 0/4/6/12, simple + composite glyf outlines, CFF outlines,
 * horizontal metrics, hhea/OS2 vertical metrics, and CBDT colour bitmaps (as
 * PNG bytes; decoding them is the caller's job, so this stays PNG-free). */
#ifndef OWOS_FONT_H
#define OWOS_FONT_H

struct font;

/* Parse an SFNT font from `data` (which must stay valid for the font's lifetime
 * — the parser keeps pointers into it). Returns NULL on a malformed or
 * unsupported (e.g. pure-CFF) file. */
struct font *font_load(const unsigned char *data, int len);
void font_free(struct font *f);

/* Copy the family name (typographic, else legacy) into `out` as ASCII ('?' for
 * other characters). Returns its length, 0 if the font has none. */
int font_family(const struct font *f, char *out, int n);

/* The face's weight (100..900, from OS/2 usWeightClass, else head.macStyle) and whether it is italic or
 * oblique. Either out pointer may be 0. */
void font_style(const struct font *f, int *weight, int *italic);

/* What a font file is, read from its table directory and a few small tables without loading it whole. */
struct font_info {
  char family[64];    /* as font_family */
  int weight, italic; /* as font_style */
  int mono;           /* fixed pitch */
  int color;          /* carries CBDT colour bitmaps, as emoji fonts do */
  int cjk;            /* OS/2 claims Han, kana or Hangul coverage */
};

/* Reads `len` bytes at offset `off` of the file into `buf`; returns the count read, or < 0. */
typedef int (*font_read_fn)(void *ctx, unsigned off, void *buf, unsigned len);

/* Fill `out` for the SFNT file that `read` reads. Returns 1, or 0 if it is not an SFNT with a head table. */
int font_probe(font_read_fn read, void *ctx, struct font_info *out);

/* Scaled vertical metrics at pixel size `px` (the em square maps to `px`
 * pixels). `descent` is negative (below the baseline); any of the out pointers
 * may be 0. */
void font_vmetrics(const struct font *f, float px, int *ascent, int *descent, int *linegap);

/* Unicode codepoint -> glyph index (0 = .notdef / not present). */
int font_glyph_index(const struct font *f, int codepoint);

/* Code -> glyph index through the cmap subtable with this platform/encoding id pair (e.g. 1,0 Macintosh Roman,
 * 3,0 Symbol); 0 if the font has no such subtable. */
int font_glyph_index_enc(const struct font *f, int platform, int encoding, int code);

/* Glyph `gi`'s horizontal advance + left side bearing in pixels at size `px`.
 */
void font_glyph_hmetrics(const struct font *f, int gi, float px, int *advance, int *lsb);

/* Rasterize glyph `gi` at pixel size `px` into a freshly malloc'd 8-bit
 * coverage bitmap (w*h bytes, row-major, 0=transparent..255=opaque). w,h get
 * the bitmap size; ox,oy the pen-relative position of its top-left (oy negative
 * for the part above the baseline); advance the pen advance in px. Returns NULL
 * with w=h=0 for an empty glyph (e.g. space) — advance is still set. Out
 * pointers (except the returned bitmap) must be non-NULL. */
unsigned char *font_render_glyph(const struct font *f, int gi, float px, int *w, int *h, int *ox, int *oy,
                                 int *advance);

/* A colour (CBDT) glyph: its PNG bytes, which point into the font data, and its placement in the pixels of the
 * strike it was drawn for. */
struct font_png {
  const unsigned char *png;
  int len;
  int ppem;   /* the strike's pixels per em */
  int w, h;   /* bitmap size */
  int bx, by; /* left bearing, and the top edge's height above the baseline */
  int adv;    /* pen advance */
};

/* True if the font carries colour bitmaps (a CBLC/CBDT pair). */
int font_has_color(const struct font *f);

/* Glyph `gi`'s colour bitmap from the strike nearest `px`: the smallest at least that size, else the largest.
 * Returns 1, or 0 if the font has none for it (then the glyph is an outline, or absent). */
int font_glyph_png(const struct font *f, int gi, float px, struct font_png *out);

/* The ligature glyph that GSUB substitutes for glyphs gids[0..n) starting at gids[0], from any type-4 lookup
 * (directly or through an extension), the longest match winning. Returns the glyph and sets *used to how many of
 * gids it covers, or returns -1. Script and feature selection are not modelled: the caller asks only where a
 * ligature is mandatory (emoji sequences). */
int font_ligature(const struct font *f, const int *gids, int n, int *used);

#endif /* OWOS_FONT_H */
