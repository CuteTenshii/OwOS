/* pdf.h, a PDF reader and page renderer.
 *
 * Parses classic and stream cross-reference tables (rebuilding them by scanning when they are damaged),
 * object streams, and the page tree, then draws a page onto a canvas2d surface: paths, clipping, text,
 * images (Flate/LZW/RunLength raw samples and JPEG), form XObjects, annotation appearances, axial and
 * radial shadings, and the PDF function types 0, 2, 3 and 4.
 *
 * Text uses an embedded TrueType/OpenType font when there is one, else the sans or mono fallback the
 * caller supplies. NOT supported: encryption, Type 3 glyph procedures, tiling patterns, soft masks in
 * graphics states, blend modes, CCITT/JBIG2/JPEG 2000 images, bare CFF (Type1C) font programs. */
#ifndef OWOS_PDF_H
#define OWOS_PDF_H

struct c2d;
struct font;
struct pdf;

enum {
  PDF_OK = 0,
  PDF_EFORMAT = -1,  /* not a PDF, or too damaged to find any page */
  PDF_EENCRYPT = -2, /* password-protected */
  PDF_ENOMEM = -3,
};

/* Parse `data`, which must stay valid until pdf_close. */
int pdf_open(const unsigned char *data, int len, struct pdf **out);
void pdf_close(struct pdf *d);

/* Fonts drawn in place of ones the file does not embed; borrowed, may be NULL (such text is skipped). */
void pdf_set_fonts(struct pdf *d, const struct font *sans, const struct font *mono);

int pdf_page_count(const struct pdf *d);

/* Page size in points (1/72 in), after the page's /Rotate. */
int pdf_page_size(struct pdf *d, int page, float *w, float *h);

/* Fill `c` with white and draw page `page` at `scale` (1 = 72 dpi). Surface pixel (0,0) is pixel (ox,oy) of the
 * scaled page, so a viewport onto a page too large for one surface can be rendered at any zoom.
 * Returns 0, or -1 for a bad page index. */
int pdf_render_page(struct pdf *d, int page, struct c2d *c, float scale, int ox, int oy);

/* One drawn character: its box in page points (after /Rotate, origin top-left, y down) and its Unicode code point.
 * Line breaks and word gaps are synthesized as glyphs with ch '\n' / ' ' and a zero-width or gap-wide box. */
struct pdf_glyph {
  float x0, y0, x1, y1;
  unsigned ch;
};

/* The page's text in reading (content stream) order, malloc'd; *n gets the count. Free with free(). Characters
 * whose font gives no Unicode mapping are left out. Returns 0, or -1 for a bad page index or no memory. */
int pdf_page_text(struct pdf *d, int page, struct pdf_glyph **out, int *n);

#endif
