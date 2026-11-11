/* Host test for the PDF reader and renderer (user/lib/pdf). Documents are assembled in this file; the expected
 * pixels come from the geometry (fills, transforms, blends) and from the font rasterizer for text. */
#define OWOS_SUITE_TEST
#include "canvas2d.h"
#include "font.h"
#include "pdf.h"
#include "pdfint.h"
#include "test.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct doc {
  unsigned char *b;
  int n, cap, off[64], max;
};

static void put(struct doc *d, const void *s, int n)
{
  if (d->n + n > d->cap) {
    d->cap = (d->n + n) * 2;
    d->b = realloc(d->b, (size_t)d->cap);
  }
  memcpy(d->b + d->n, s, (size_t)n);
  d->n += n;
}

static void putf(struct doc *d, const char *fmt, ...)
{
  char tmp[4096];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  put(d, tmp, n);
}

static void begin(struct doc *d)
{
  memset(d, 0, sizeof *d);
  putf(d, "%%PDF-1.7\n");
}

static void obj(struct doc *d, int num, const char *body)
{
  d->off[num] = d->n;
  if (num > d->max)
    d->max = num;
  putf(d, "%d 0 obj\n%s\nendobj\n", num, body);
}

static void stream(struct doc *d, int num, const char *extra, const void *data, int len)
{
  d->off[num] = d->n;
  if (num > d->max)
    d->max = num;
  putf(d, "%d 0 obj\n<< %s /Length %d >>\nstream\n", num, extra, len);
  put(d, data, len);
  putf(d, "\nendstream\nendobj\n");
}

/* zlib stream of stored blocks, which inflate reads without needing a compressor here */
static unsigned char *zstore(const void *data, int len, int *out)
{
  unsigned char *z = malloc((size_t)len + 16);
  z[0] = 0x78;
  z[1] = 0x01;
  z[2] = 1;
  z[3] = (unsigned char)len;
  z[4] = (unsigned char)(len >> 8);
  z[5] = (unsigned char)~len;
  z[6] = (unsigned char)(~len >> 8);
  memcpy(z + 7, data, (size_t)len);
  memset(z + 7 + len, 0, 4);
  *out = len + 11;
  return z;
}

static void finish(struct doc *d, const char *trailer_extra)
{
  int xref = d->n;
  putf(d, "xref\n0 %d\n0000000000 65535 f \n", d->max + 1);
  for (int i = 1; i <= d->max; i++)
    putf(d, "%010d 00000 n \n", d->off[i]);
  putf(d, "trailer\n<< /Size %d /Root 1 0 R %s >>\nstartxref\n%d\n%%%%EOF\n", d->max + 1, trailer_extra, xref);
}

/* Objects 1 to 4: catalog, page tree, one page, its content stream. */
static void page(struct doc *d, int w, int h, const char *resources, const char *page_extra, const char *content)
{
  begin(d);
  obj(d, 1, "<< /Type /Catalog /Pages 2 0 R >>");
  obj(d, 2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
  char buf[1024];
  snprintf(buf, sizeof buf,
           "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] /Resources << %s >> /Contents 4 0 R %s >>", w, h,
           resources, page_extra);
  obj(d, 3, buf);
  stream(d, 4, "", content, (int)strlen(content));
}

static unsigned char *g_sans_data, *g_mono_data;
static struct font *g_sans, *g_mono;
static int g_sans_len;

static unsigned char *slurp(const char *path, int *len)
{
  FILE *fp = fopen(path, "rb");
  if (!fp)
    return 0;
  unsigned char *b = malloc(1 << 20);
  *len = (int)fread(b, 1, 1 << 20, fp);
  fclose(fp);
  return b;
}

static void load_fonts(void)
{
  if (g_sans)
    return;
  int n;
  g_sans_data = slurp("fonts/LiberationSans-Regular.ttf", &g_sans_len);
  g_mono_data = slurp("fonts/LiberationMono-Regular.ttf", &n);
  g_sans = font_load(g_sans_data, g_sans_len);
  g_mono = font_load(g_mono_data, n);
}

struct shot {
  struct pdf *pdf;
  struct c2d *c;
  int rc;
};

static struct shot render(struct doc *d, int pg, float scale)
{
  struct shot s = {0, 0, 0};
  load_fonts();
  s.rc = pdf_open(d->b, d->n, &s.pdf);
  if (s.rc)
    return s;
  pdf_set_fonts(s.pdf, g_sans, g_mono);
  float w, h;
  pdf_page_size(s.pdf, pg, &w, &h);
  s.c = c2d_new((int)ceil(w * scale), (int)ceil(h * scale));
  pdf_render_page(s.pdf, pg, s.c, scale, 0, 0);
  return s;
}

static void done(struct shot *s, struct doc *d)
{
  c2d_free(s->c);
  pdf_close(s->pdf);
  free(d->b);
}

static const unsigned char *px(const struct shot *s, int x, int y) { return s->c->px + ((size_t)y * s->c->w + x) * 4; }

static int is_rgb(const struct shot *s, int x, int y, int r, int g, int b, int tol)
{
  const unsigned char *p = px(s, x, y);
  return abs(p[0] - r) <= tol && abs(p[1] - g) <= tol && abs(p[2] - b) <= tol;
}

static void fill_and_clip(void)
{
  struct doc d;
  page(&d, 100, 100, "", "", "1 0 0 rg 10 10 50 30 re f q 70 70 20 20 re W n 0 0 1 rg 0 0 100 100 re f Q");
  finish(&d, "");
  struct shot s = render(&d, 0, 1);
  expect_eq(s.rc, PDF_OK);
  if (!s.c)
    return;
  expect(is_rgb(&s, 30, 75, 255, 0, 0, 0));
  expect(is_rgb(&s, 5, 5, 255, 255, 255, 0));
  expect(is_rgb(&s, 30, 20, 255, 255, 255, 0));
  expect(is_rgb(&s, 80, 20, 0, 0, 255, 0));
  expect(is_rgb(&s, 60, 20, 255, 255, 255, 0));
  expect(is_rgb(&s, 80, 50, 255, 255, 255, 0));
  done(&s, &d);
}

static void scale_and_rotate(void)
{
  struct doc d;
  page(&d, 100, 50, "", "/Rotate 90", "0 1 0 rg 0 0 10 10 re f");
  finish(&d, "");
  struct pdf *p;
  expect_eq(pdf_open(d.b, d.n, &p), PDF_OK);
  float w = 0, h = 0;
  expect_eq(pdf_page_size(p, 0, &w, &h), 0);
  expect_eq((int)w, 50);
  expect_eq((int)h, 100);
  expect_eq(pdf_page_size(p, 1, &w, &h), -1);
  pdf_close(p);
  struct shot s = render(&d, 0, 2);
  expect_eq(s.c->w, 100);
  expect_eq(s.c->h, 200);
  expect(is_rgb(&s, 10, 10, 0, 255, 0, 0));
  expect(is_rgb(&s, 10, 100, 255, 255, 255, 0));
  expect(is_rgb(&s, 60, 10, 255, 255, 255, 0));
  done(&s, &d);

  page(&d, 100, 50, "", "/Rotate 180", "0 1 0 rg 0 0 10 10 re f");
  finish(&d, "");
  s = render(&d, 0, 1);
  expect(is_rgb(&s, 95, 5, 0, 255, 0, 0));
  expect(is_rgb(&s, 5, 45, 255, 255, 255, 0));
  done(&s, &d);
}

static void viewport_offset(void)
{
  struct doc d;
  page(&d, 100, 100, "", "", "1 0 0 rg 50 50 50 50 re f");
  finish(&d, "");
  struct pdf *p;
  expect_eq(pdf_open(d.b, d.n, &p), PDF_OK);
  struct c2d *c = c2d_new(40, 40);
  pdf_render_page(p, 0, c, 2, 120, 20);
  /* The red square covers scaled x 100..200, y 0..100; this window starts at (120, 20) */
  expect(c->px[(5 * 40 + 5) * 4] == 255 && c->px[(5 * 40 + 5) * 4 + 1] == 0);
  pdf_render_page(p, 0, c, 2, 0, 0);
  expect(c->px[(5 * 40 + 5) * 4 + 1] == 255);
  c2d_free(c);
  pdf_close(p);
  free(d.b);
}

static void compressed_objects(void)
{
  struct doc d;
  begin(&d);
  const char *o1 = "<< /Type /Catalog /Pages 2 0 R >>", *o2 = "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
             *o3 = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 60 60] /Contents 4 0 R >>";
  char os[512];
  int l1 = (int)strlen(o1), l2 = (int)strlen(o2);
  int hdr = snprintf(os, sizeof os, "1 0 2 %d 3 %d ", l1, l1 + l2);
  int n = hdr;
  n += snprintf(os + n, sizeof os - (size_t)n, "%s%s%s", o1, o2, o3);
  int zl;
  unsigned char *z = zstore(os, n, &zl);
  char ex[128];
  snprintf(ex, sizeof ex, "/Type /ObjStm /N 3 /First %d /Filter /FlateDecode", hdr);
  const char *content = "0 0 1 rg 0 0 30 30 re f";
  stream(&d, 4, "", content, (int)strlen(content));
  stream(&d, 5, ex, z, zl);
  free(z);
  d.off[6] = d.n;
  d.max = 6;
  unsigned char xr[7 * 4];
  int e[7][3] = {{0, 0, 0}, {2, 5, 0}, {2, 5, 1}, {2, 5, 2}, {1, d.off[4], 0}, {1, d.off[5], 0}, {1, d.off[6], 0}};
  for (int i = 0; i < 7; i++) {
    xr[i * 4] = (unsigned char)e[i][0];
    xr[i * 4 + 1] = (unsigned char)(e[i][1] >> 8);
    xr[i * 4 + 2] = (unsigned char)e[i][1];
    xr[i * 4 + 3] = (unsigned char)e[i][2];
  }
  stream(&d, 6, "/Type /XRef /Size 7 /W [1 2 1] /Root 1 0 R", xr, (int)sizeof xr);
  putf(&d, "startxref\n%d\n%%%%EOF\n", d.off[6]);
  struct shot s = render(&d, 0, 1);
  expect_eq(s.rc, PDF_OK);
  if (s.c) {
    expect(is_rgb(&s, 10, 45, 0, 0, 255, 0));
    expect(is_rgb(&s, 45, 10, 255, 255, 255, 0));
  }
  done(&s, &d);
}

static void damaged_xref(void)
{
  struct doc d;
  page(&d, 50, 50, "", "", "1 0 0 rg 0 0 25 25 re f");
  finish(&d, "");
  char *sx = strstr((char *)d.b, "startxref\n");
  sx[10] = '9';
  sx[11] = '9';
  struct shot s = render(&d, 0, 1);
  expect_eq(s.rc, PDF_OK);
  if (s.c)
    expect(is_rgb(&s, 10, 40, 255, 0, 0, 0));
  done(&s, &d);

  page(&d, 50, 50, "", "", "1 0 0 rg 0 0 25 25 re f");
  finish(&d, "");
  d.n -= 80;
  s = render(&d, 0, 1);
  expect_eq(s.rc, PDF_OK);
  if (s.c)
    expect(is_rgb(&s, 10, 40, 255, 0, 0, 0));
  done(&s, &d);
}

static void rejects(void)
{
  struct pdf *p;
  expect_eq(pdf_open((const unsigned char *)"hello world", 11, &p), PDF_EFORMAT);
  expect_eq(pdf_open((const unsigned char *)"", 0, &p), PDF_EFORMAT);
  expect_eq(pdf_open((const unsigned char *)"%PDF-1.4\n1 0 obj\n<< >>\nendobj\n", 31, &p), PDF_EFORMAT);
  struct doc d;
  page(&d, 10, 10, "", "", "");
  finish(&d, "/Encrypt << /Filter /Standard >>");
  expect_eq(pdf_open(d.b, d.n, &p), PDF_EENCRYPT);
  free(d.b);
}

static void filters(void)
{
  const unsigned char lzw[] = {0x80, 0x0B, 0x60, 0x50, 0x22, 0x0C, 0x0C, 0x85, 0x01};
  struct pdf *p;
  struct doc d;
  page(&d, 10, 10, "", "", "");
  finish(&d, "");
  expect_eq(pdf_open(d.b, d.n, &p), PDF_OK);
  struct pobj nm = pdf_null();
  int len = 0;
  const char *left;
  nm.t = PO_NAME;
  nm.n = 9;
  nm.u.s = "LZWDecode";
  unsigned char *out = pdf_decode(p, lzw, sizeof lzw, nm, pdf_null(), &len, &left, 0);
  expect_eq(len, 10);
  expect(out && !memcmp(out, "-----A---B", 10));
  free(out);
  nm.u.s = "ASCII85Decode";
  out = pdf_decode(p, (const unsigned char *)"9jqo^~>", 7, nm, pdf_null(), &len, &left, 0);
  expect(len == 4 && !memcmp(out, "Man ", 4));
  free(out);
  nm.u.s = "ASCIIHexDecode";
  out = pdf_decode(p, (const unsigned char *)"48 65 6c6C 6F7>", 15, nm, pdf_null(), &len, &left, 0);
  expect(len == 6 && !memcmp(out, "Hello\x70", 6));
  free(out);
  nm.u.s = "RunLengthDecode";
  const unsigned char rl[] = {2, 'A', 'B', 'C', 0xFE, 'D', 0x80};
  out = pdf_decode(p, rl, sizeof rl, nm, pdf_null(), &len, &left, 0);
  expect(len == 6 && !memcmp(out, "ABCDDD", 6));
  free(out);
  nm.u.s = "DCTDecode";
  out = pdf_decode(p, rl, sizeof rl, nm, pdf_null(), &len, &left, 0);
  expect(left && !strcmp(left, "DCTDecode") && len == (int)sizeof rl);
  free(out);

  /* PNG "Up" predictor over a stored zlib stream: rows [1 2 3] and [+1 +1 +1] */
  const unsigned char rows[] = {2, 1, 2, 3, 2, 1, 1, 1};
  int zl;
  unsigned char *z = zstore(rows, sizeof rows, &zl);
  struct pkv kv[3] = {
      {"Predictor", {PO_INT, 12, 0, {0}}}, {"Columns", {PO_INT, 3, 0, {0}}}, {"Colors", {PO_INT, 1, 0, {0}}}};
  struct pobj parms = {PO_DICT, 3, 0, {.kv = kv}};
  nm.u.s = "FlateDecode";
  out = pdf_decode(p, z, zl, nm, parms, &len, &left, 0);
  expect(len == 6 && !memcmp(out, "\1\2\3\2\3\4", 6));
  free(out);
  free(z);
  pdf_close(p);
  free(d.b);

  const char *c = "0 0 1 rg 0 0 40 40 re f";
  char enc[600];
  int k = 0;
  for (const char *q = c; *q; q++)
    k += snprintf(enc + k, sizeof enc - (size_t)k, "%02X", (unsigned char)*q);
  enc[k++] = '>';
  begin(&d);
  obj(&d, 1, "<< /Type /Catalog /Pages 2 0 R >>");
  obj(&d, 2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
  obj(&d, 3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 40 40] /Contents 4 0 R >>");
  stream(&d, 4, "/Filter /ASCIIHexDecode", enc, k);
  finish(&d, "");
  struct shot s = render(&d, 0, 1);
  expect_eq(s.rc, PDF_OK);
  if (s.c)
    expect(is_rgb(&s, 20, 20, 0, 0, 255, 0));
  done(&s, &d);
}

static void colors_and_functions(void)
{
  struct doc d;
  const char *res = "/ColorSpace << /Sep [/Separation /Spot /DeviceRGB << /FunctionType 2 /Domain [0 1] /C0 [1 1 1] "
                    "/C1 [1 0 0] /N 1 >>] /Calc [/Separation /Calc /DeviceGray 5 0 R] /Idx [/Indexed /DeviceRGB 1 "
                    "<00FF00 0000FF>] >>";
  page(&d, 100, 20, res, "",
       "/Sep cs 1 scn 0 0 20 20 re f /Sep cs 0.5 scn 20 0 20 20 re f /Calc cs 0.25 scn 40 0 20 20 re f "
       "/Idx cs 1 sc 60 0 20 20 re f 0 0 0 1 k 80 0 20 20 re f");
  const char *ps = "{ dup 0.5 gt { pop 1 } { 2 mul 1 exch sub } ifelse }";
  stream(&d, 5, "/FunctionType 4 /Domain [0 1] /Range [0 1]", ps, (int)strlen(ps));
  finish(&d, "");
  struct shot s = render(&d, 0, 1);
  expect_eq(s.rc, PDF_OK);
  if (s.c) {
    expect(is_rgb(&s, 10, 10, 255, 0, 0, 1));
    expect(is_rgb(&s, 30, 10, 255, 128, 128, 2));
    /* tint 0.25: 1 - 2 * 0.25 = 0.5 gray */
    expect(is_rgb(&s, 50, 10, 128, 128, 128, 2));
    expect(is_rgb(&s, 70, 10, 0, 0, 255, 0));
    expect(is_rgb(&s, 90, 10, 0, 0, 0, 0));
  }
  done(&s, &d);
}

static void images(void)
{
  struct doc d;
  page(&d, 60, 60, "/XObject << /Im0 5 0 R /Mk 6 0 R /Sm 7 0 R >>", "",
       "q 40 0 0 40 10 10 cm /Im0 Do Q 0 0 1 rg q 10 0 0 10 0 50 cm /Mk Do Q q 10 0 0 10 50 50 cm /Sm Do Q");
  const unsigned char rgb[] = {255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 0};
  stream(&d, 5, "/Type /XObject /Subtype /Image /Width 2 /Height 2 /ColorSpace /DeviceRGB /BitsPerComponent 8", rgb,
         sizeof rgb);
  const unsigned char bits[] = {0x00};
  stream(&d, 6, "/Type /XObject /Subtype /Image /Width 1 /Height 1 /ImageMask true /BitsPerComponent 1", bits, 1);
  const unsigned char col[] = {200, 0, 0};
  const unsigned char alpha[] = {128};
  stream(&d, 7,
         "/Type /XObject /Subtype /Image /Width 1 /Height 1 /ColorSpace /DeviceRGB /BitsPerComponent 8 /SMask 8 0 R",
         col, sizeof col);
  stream(&d, 8, "/Type /XObject /Subtype /Image /Width 1 /Height 1 /ColorSpace /DeviceGray /BitsPerComponent 8", alpha,
         1);
  finish(&d, "");
  struct shot s = render(&d, 0, 1);
  expect_eq(s.rc, PDF_OK);
  if (s.c) {
    expect(is_rgb(&s, 20, 20, 255, 0, 0, 14));
    expect(is_rgb(&s, 40, 20, 0, 255, 0, 14));
    expect(is_rgb(&s, 20, 40, 0, 0, 255, 14));
    expect(is_rgb(&s, 40, 40, 255, 255, 0, 14));
    expect(is_rgb(&s, 5, 5, 0, 0, 255, 0));
    expect(is_rgb(&s, 55, 5, 227, 127, 127, 3));
  }
  done(&s, &d);
}

static void shading(void)
{
  struct doc d;
  page(&d, 100, 20, "/Shading << /Sh0 5 0 R >>", "", "q 0 0 100 20 re W n /Sh0 sh Q");
  obj(&d, 5,
      "<< /ShadingType 2 /ColorSpace /DeviceRGB /Coords [0 0 100 0] /Extend [true true] /Function << /FunctionType 2 "
      "/Domain [0 1] /C0 [1 0 0] /C1 [0 0 1] /N 1 >> >>");
  finish(&d, "");
  struct shot s = render(&d, 0, 1);
  expect_eq(s.rc, PDF_OK);
  if (s.c) {
    expect(is_rgb(&s, 2, 10, 250, 0, 5, 8));
    expect(is_rgb(&s, 50, 10, 128, 0, 128, 10));
    expect(is_rgb(&s, 97, 10, 5, 0, 250, 8));
  }
  done(&s, &d);
}

static void annotations(void)
{
  struct doc d;
  page(&d, 100, 100, "", "/Annots [5 0 R 7 0 R]", "");
  obj(&d, 5, "<< /Type /Annot /Subtype /Widget /Rect [10 10 50 30] /AP << /N 6 0 R >> >>");
  const char *ap = "1 0 0 rg 0 0 1 1 re f";
  stream(&d, 6, "/Type /XObject /Subtype /Form /BBox [0 0 1 1]", ap, (int)strlen(ap));
  obj(&d, 7, "<< /Type /Annot /Subtype /Widget /F 2 /Rect [60 60 90 90] /AP << /N 6 0 R >> >>");
  finish(&d, "");
  struct shot s = render(&d, 0, 1);
  expect_eq(s.rc, PDF_OK);
  if (s.c) {
    expect(is_rgb(&s, 30, 80, 255, 0, 0, 0));
    expect(is_rgb(&s, 75, 25, 255, 255, 255, 0));
  }
  done(&s, &d);
}

/* Every pixel a glyph covers fully must be dark in the page render, which pins the glyph's position and size. */
static int glyph_matches(const struct shot *s, const struct font *f, int ch, float size, int x, int y)
{
  int w, h, ox, oy, adv, gi = font_glyph_index(f, ch);
  unsigned char *g = font_render_glyph(f, gi, size, &w, &h, &ox, &oy, &adv);
  int ink = 0, bad = 0;
  for (int j = 0; g && j < h; j++)
    for (int i = 0; i < w; i++)
      if (g[j * w + i] == 255) {
        ink++;
        if (px(s, x + ox + i, y + oy + j)[0] > 60)
          bad++;
      }
  free(g);
  return ink > 20 && bad == 0;
}

static void text_embedded_and_fallback(void)
{
  load_fonts();
  struct doc d;
  char content[] = "BT /F1 40 Tf 10 30 Td (HI) Tj ET";
  page(&d, 100, 60, "/Font << /F1 5 0 R >>", "", content);
  obj(&d, 5,
      "<< /Type /Font /Subtype /TrueType /BaseFont /Liberation /Encoding /WinAnsiEncoding /FontDescriptor 6 0 R "
      "/FirstChar 72 /LastChar 73 /Widths [722 278] >>");
  obj(&d, 6, "<< /Type /FontDescriptor /FontName /Liberation /Flags 32 /FontFile2 7 0 R >>");
  stream(&d, 7, "/Length1 0", g_sans_data, g_sans_len);
  finish(&d, "");
  struct shot s = render(&d, 0, 1);
  expect_eq(s.rc, PDF_OK);
  if (s.c) {
    expect(glyph_matches(&s, g_sans, 'H', 40, 10, 30));
    expect(glyph_matches(&s, g_sans, 'I', 40, 10 + (int)(0.722 * 40 + 0.5), 30));
    expect(is_rgb(&s, 95, 5, 255, 255, 255, 0));
  }
  done(&s, &d);

  page(&d, 100, 60, "/Font << /F1 5 0 R >>", "", "BT /F1 40 Tf 10 30 Td (HI) Tj ET BT /F2 20 Tf 10 8 Td (Hx) Tj ET");
  obj(&d, 5, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");
  finish(&d, "");
  s = render(&d, 0, 1);
  if (s.c) {
    expect(glyph_matches(&s, g_sans, 'H', 40, 10, 30));
    int adv = 0, lsb;
    font_glyph_hmetrics(g_sans, font_glyph_index(g_sans, 'H'), 40, &adv, &lsb);
    expect(glyph_matches(&s, g_sans, 'I', 40, 10 + adv, 30));
  }
  done(&s, &d);

  /* Composite font: the string holds glyph ids, found through the font's own cmap */
  int gh = font_glyph_index(g_sans, 'H');
  char cc[64];
  snprintf(cc, sizeof cc, "BT /F1 40 Tf 10 30 Td <%04X> Tj ET", gh);
  page(&d, 100, 60, "/Font << /F1 5 0 R >>", "", cc);
  obj(&d, 5, "<< /Type /Font /Subtype /Type0 /BaseFont /Liberation /Encoding /Identity-H /DescendantFonts [8 0 R] >>");
  obj(&d, 8,
      "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /Liberation /CIDToGIDMap /Identity /FontDescriptor 6 0 R /DW "
      "700 >>");
  obj(&d, 6, "<< /Type /FontDescriptor /FontName /Liberation /Flags 4 /FontFile2 7 0 R >>");
  stream(&d, 7, "/Length1 0", g_sans_data, g_sans_len);
  finish(&d, "");
  s = render(&d, 0, 1);
  if (s.c)
    expect(glyph_matches(&s, g_sans, 'H', 40, 10, 30));
  done(&s, &d);

  /* Rotated text goes through the transformed-tile path: some ink lands, and in the rotated place */
  page(&d, 100, 100, "/Font << /F1 5 0 R >>", "", "BT /F1 30 Tf 0 1 -1 0 60 20 Tm (HI) Tj ET");
  obj(&d, 5, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");
  finish(&d, "");
  s = render(&d, 0, 1);
  if (s.c) {
    int dark = 0, right = 0;
    for (int y = 0; y < 100; y++)
      for (int x = 0; x < 100; x++)
        if (px(&s, x, y)[0] < 100) {
          dark++;
          if (x > 62)
            right++;
        }
    expect(dark > 80);
    expect_eq(right, 0);
  }
  done(&s, &d);
}

static void survives_corruption(void)
{
  struct doc d;
  page(
      &d, 100, 60, "/Font << /F1 5 0 R >> /XObject << /Im 6 0 R >>", "/Annots [7 0 R]",
      "q 0.5 0 0 0.5 0 0 cm 1 0 0 rg 10 10 80 40 re f Q BT /F1 20 Tf 10 30 Td (Hi) Tj ET q 20 0 0 20 40 5 cm /Im Do Q");
  obj(&d, 5, "<< /Type /Font /Subtype /Type1 /BaseFont /Courier /Encoding << /Differences [65 /B /uni0041] >> >>");
  const unsigned char rgb[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  stream(&d, 6,
         "/Type /XObject /Subtype /Image /Width 2 /Height 2 /ColorSpace [/Indexed /DeviceRGB 3 <000000FFFFFF>] "
         "/BitsPerComponent 8",
         rgb, sizeof rgb);
  obj(&d, 7, "<< /Subtype /Square /Rect [0 0 10 10] /AP << /N 4 0 R >> >>");
  finish(&d, "");
  load_fonts();
  unsigned seed = 12345;
  int opened = 0;
  for (int i = 0; i < 400; i++) {
    struct doc m = d;
    m.b = malloc((size_t)d.n);
    memcpy(m.b, d.b, (size_t)d.n);
    int flips = 1 + i % 4;
    for (int k = 0; k < flips; k++) {
      seed = seed * 1103515245u + 12345u;
      m.b[(seed >> 8) % (unsigned)m.n] = (unsigned char)(seed >> 3);
    }
    if (i % 5 == 0)
      m.n = (int)((seed >> 4) % (unsigned)m.n);
    struct pdf *p;
    if (pdf_open(m.b, m.n, &p) == PDF_OK) {
      opened++;
      pdf_set_fonts(p, g_sans, g_mono);
      struct c2d *c = c2d_new(100, 60);
      for (int pg = 0; pg < pdf_page_count(p); pg++)
        pdf_render_page(p, pg, c, 1, 0, 0);
      c2d_free(c);
      pdf_close(p);
    }
    free(m.b);
  }
  expect(opened > 100);
  free(d.b);
}

int main(void)
{
  test("fill_and_clip", fill_and_clip);
  test("scale_and_rotate", scale_and_rotate);
  test("viewport_offset", viewport_offset);
  test("compressed_objects", compressed_objects);
  test("damaged_xref", damaged_xref);
  test("rejects", rejects);
  test("filters", filters);
  test("colors_and_functions", colors_and_functions);
  test("images", images);
  test("shading", shading);
  test("annotations", annotations);
  test("text_embedded_and_fallback", text_embedded_and_fallback);
  test("survives_corruption", survives_corruption);
  return 0;
}
