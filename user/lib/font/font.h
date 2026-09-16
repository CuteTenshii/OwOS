/* font.h — a small TrueType/OpenType font rasterizer.
 *
 * Parses an SFNT font (.ttf, or .otf with TrueType `glyf` outlines) from memory
 * and rasterizes glyphs to 8-bit antialiased coverage bitmaps at any pixel
 * size. General-purpose (the browser's @font-face, a font viewer, an editor's
 * font picker, …) — it has no dependency on the browser. Outlines are flattened
 * to polygons and scanline-filled with supersampled antialiasing.
 *
 * Scope: cmap formats 0/4/6/12, simple + composite glyf outlines, horizontal
 * metrics, hhea/OS2 vertical metrics. NOT yet: CFF/Type2 (PostScript) outlines
 * — a pure-CFF .otf returns NULL from font_load for now. */
#ifndef OWOS_FONT_H
#define OWOS_FONT_H

struct font;

/* Parse an SFNT font from `data` (which must stay valid for the font's lifetime
 * — the parser keeps pointers into it). Returns NULL on a malformed or
 * unsupported (e.g. pure-CFF) file. */
struct font *font_load(const unsigned char *data, int len);
void font_free(struct font *f);

/* Scaled vertical metrics at pixel size `px` (the em square maps to `px`
 * pixels). `descent` is negative (below the baseline); any of the out pointers
 * may be 0. */
void font_vmetrics(const struct font *f, float px, int *ascent, int *descent, int *linegap);

/* Unicode codepoint -> glyph index (0 = .notdef / not present). */
int font_glyph_index(const struct font *f, int codepoint);

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

#endif /* OWOS_FONT_H */
