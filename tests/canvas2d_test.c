/* Host test for the canvas 2D rasterizer (user/lib/canvas2d). Expected pixels are derived by hand from the
 * geometry (areas, exact blend arithmetic), not from the implementation's own output. */
#define OWOS_SUITE_TEST
#include "canvas2d.h"
#include "crc32.h"
#include "font.h"
#include "test.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const unsigned char *px(const struct c2d *c, int x, int y) { return c->px + ((unsigned long)y * c->w + x) * 4; }

static void set_fill(struct c2d *c, int r, int g, int b, int a)
{
  c2d_set_paint_color(&c->st[c->depth].fill, (unsigned char)r, (unsigned char)g, (unsigned char)b, (unsigned char)a);
}

static void set_stroke(struct c2d *c, int r, int g, int b, int a)
{
  c2d_set_paint_color(&c->st[c->depth].stroke, (unsigned char)r, (unsigned char)g, (unsigned char)b, (unsigned char)a);
}

/* Sum of alpha over the surface, in pixels */
static double coverage(const struct c2d *c)
{
  double sum = 0;
  for (int i = 0; i < c->w * c->h; i++)
    sum += c->px[i * 4 + 3] / 255.0;
  return sum;
}

static void test_fill_rect_and_alpha_blend(void)
{
  struct c2d *c = c2d_new(8, 8);
  set_fill(c, 255, 0, 0, 255);
  c2d_fill_rect(c, 2, 2, 4, 4);
  expect(px(c, 2, 2)[0] == 255 && px(c, 2, 2)[3] == 255);
  expect(px(c, 5, 5)[3] == 255 && px(c, 6, 6)[3] == 0 && px(c, 1, 1)[3] == 0);
  c->st[c->depth].alpha = 0.5f;
  set_fill(c, 0, 0, 255, 255);
  c2d_fill_rect(c, 2, 2, 1, 1);
  /* 50% blue over opaque red: 128 * 255/255 of blue (alpha 128/255 rounded), red keeps (255 - 128) */
  expect(px(c, 2, 2)[3] == 255);
  expect(abs(px(c, 2, 2)[2] - 128) <= 1 && abs(px(c, 2, 2)[0] - 127) <= 1);
  c2d_free(c);
}

static void test_fractional_rect_coverage(void)
{
  struct c2d *c = c2d_new(8, 8);
  set_fill(c, 0, 0, 0, 255);
  c2d_fill_rect(c, 1.5f, 1, 2, 2); /* covers x in [1.5, 3.5): half pixels at the left and right edges */
  expect(abs(px(c, 1, 1)[3] - 128) <= 8);
  expect(px(c, 2, 1)[3] == 255);
  expect(abs(px(c, 3, 1)[3] - 128) <= 8);
  expect(px(c, 4, 1)[3] == 0);
  expect(fabs(coverage(c) - 4.0) < 0.1);
  c2d_free(c);
}

static void test_triangle_area_and_fill_rules(void)
{
  struct c2d *c = c2d_new(64, 64);
  struct c2d_path p = {0};
  set_fill(c, 0, 0, 0, 255);
  c2d_path_move(&p, 0, 4, 4);
  c2d_path_line(&p, 0, 44, 4);
  c2d_path_line(&p, 0, 4, 44);
  c2d_path_close(&p);
  c2d_fill(c, &p, false);
  expect(fabs(coverage(c) - 800.0) < 6.0); /* 40*40/2 */
  c2d_path_free(&p);
  c2d_free(c);

  /* Two nested same-direction squares: nonzero fills the hole, even-odd leaves it */
  for (int eo = 0; eo < 2; eo++) {
    c = c2d_new(32, 32);
    set_fill(c, 0, 0, 0, 255);
    memset(&p, 0, sizeof p);
    c2d_path_rect(&p, 0, 2, 2, 28, 28);
    c2d_path_rect(&p, 0, 8, 8, 16, 16);
    c2d_fill(c, &p, eo);
    expect(px(c, 4, 4)[3] == 255);
    expect(px(c, 16, 16)[3] == (eo ? 0 : 255));
    c2d_path_free(&p);
    c2d_free(c);
  }
}

static void test_stroke_width_joins_and_caps(void)
{
  struct c2d *c = c2d_new(40, 40);
  struct c2d_path p = {0};
  set_stroke(c, 0, 0, 0, 255);
  c->st[c->depth].line_width = 4;
  c2d_path_move(&p, 0, 5, 10);
  c2d_path_line(&p, 0, 25, 10);
  c2d_stroke(c, &p);
  /* butt caps: exactly rows 8..11 over x 5..24 */
  expect(px(c, 10, 8)[3] == 255 && px(c, 10, 11)[3] == 255);
  expect(px(c, 10, 7)[3] == 0 && px(c, 10, 12)[3] == 0);
  expect(px(c, 4, 10)[3] == 0 && px(c, 25, 10)[3] == 0);
  expect(fabs(coverage(c) - 80.0) < 1.0);
  c2d_free(c);

  c = c2d_new(40, 40);
  set_stroke(c, 0, 0, 0, 255);
  c->st[c->depth].line_width = 4;
  c->st[c->depth].cap = C2D_SQUARE;
  c2d_stroke(c, &p);
  expect(px(c, 3, 10)[3] == 255 && px(c, 2, 10)[3] == 0 && px(c, 26, 10)[3] == 255 && px(c, 27, 10)[3] == 0);
  c2d_free(c);

  c = c2d_new(40, 40);
  set_stroke(c, 0, 0, 0, 255);
  c->st[c->depth].line_width = 8;
  c->st[c->depth].cap = C2D_ROUND;
  c2d_stroke(c, &p);
  expect(px(c, 3, 10)[3] > 200); /* inside the round cap */
  expect(px(c, 1, 6)[3] == 0);   /* outside the circle's corner */
  c2d_path_free(&p);
  c2d_free(c);

  /* Miter join of a right angle fills the outer corner; bevel does not */
  for (int join = 0; join < 2; join++) {
    c = c2d_new(40, 40);
    memset(&p, 0, sizeof p);
    set_stroke(c, 0, 0, 0, 255);
    c->st[c->depth].line_width = 6;
    c->st[c->depth].join = join ? C2D_BEVEL : C2D_MITER;
    c2d_path_move(&p, 0, 5, 30);
    c2d_path_line(&p, 0, 5, 5);
    c2d_path_line(&p, 0, 30, 5);
    c2d_stroke(c, &p);
    expect(px(c, 2, 2)[3] == (join ? 0 : 255));
    c2d_path_free(&p);
    c2d_free(c);
  }
}

static void test_dash_pattern(void)
{
  struct c2d *c = c2d_new(60, 12);
  struct c2d_path p = {0};
  float d[2] = {10, 5};
  set_stroke(c, 0, 0, 0, 255);
  c->st[c->depth].line_width = 2;
  c2d_set_dash(c, d, 2);
  c2d_path_move(&p, 0, 0, 6);
  c2d_path_line(&p, 0, 60, 6);
  c2d_stroke(c, &p);
  expect(px(c, 5, 6)[3] == 255 && px(c, 12, 6)[3] == 0 && px(c, 17, 6)[3] == 255 && px(c, 27, 6)[3] == 0);
  c2d_path_free(&p);
  c2d_free(c);
}

static void test_gradients(void)
{
  struct c2d *c = c2d_new(100, 4);
  struct c2d_gradient *g = c2d_gradient_new(C2D_LINEAR, 0, 0, 0, 100, 0, 0);
  c2d_gradient_add_stop(g, 0, 0, 0, 0, 255);
  c2d_gradient_add_stop(g, 1, 255, 255, 255, 255);
  c2d_set_paint_gradient(&c->st[c->depth].fill, g);
  c2d_gradient_unref(g);
  c2d_fill_rect(c, 0, 0, 100, 4);
  expect(px(c, 0, 1)[0] <= 3);
  expect(abs(px(c, 49, 1)[0] - 127) <= 4);
  expect(px(c, 99, 1)[0] >= 252);
  c2d_free(c);

  c = c2d_new(21, 21);
  g = c2d_gradient_new(C2D_RADIAL, 10.5f, 10.5f, 0, 10.5f, 10.5f, 10);
  c2d_gradient_add_stop(g, 0, 255, 0, 0, 255);
  c2d_gradient_add_stop(g, 1, 0, 0, 255, 255);
  c2d_set_paint_gradient(&c->st[c->depth].fill, g);
  c2d_gradient_unref(g);
  c2d_fill_rect(c, 0, 0, 21, 21);
  expect(px(c, 10, 10)[0] > 240 && px(c, 10, 10)[2] < 15);
  expect(px(c, 20, 10)[2] > 220); /* beyond the radius: clamped to the last stop */
  c2d_free(c);
}

static void test_clip_and_save_restore(void)
{
  struct c2d *c = c2d_new(20, 20);
  struct c2d_path p = {0};
  c2d_save(c);
  c2d_path_rect(&p, 0, 5, 5, 10, 10);
  c2d_clip(c, &p, false);
  set_fill(c, 0, 255, 0, 255);
  c2d_fill_rect(c, 0, 0, 20, 20);
  expect(px(c, 4, 10)[3] == 0 && px(c, 5, 10)[3] == 255 && px(c, 14, 10)[3] == 255 && px(c, 15, 10)[3] == 0);
  c2d_restore(c);
  /* the clip and the fill colour both went with the state */
  c2d_fill_rect(c, 0, 0, 2, 2);
  expect(px(c, 0, 0)[3] == 255 && px(c, 0, 0)[1] == 0);
  c2d_path_free(&p);
  c2d_free(c);
}

static void test_composite_operations(void)
{
  struct c2d *c = c2d_new(8, 8);
  set_fill(c, 255, 0, 0, 255);
  c2d_fill_rect(c, 0, 0, 8, 8);
  c->st[c->depth].comp = C2D_DEST_OUT;
  c2d_fill_rect(c, 2, 2, 2, 2);
  expect(px(c, 2, 2)[3] == 0 && px(c, 4, 4)[3] == 255);
  c2d_free(c);

  /* source-in keeps the new shape only where it overlaps, and clears everything else */
  c = c2d_new(8, 8);
  set_fill(c, 255, 0, 0, 255);
  c2d_fill_rect(c, 0, 0, 4, 8);
  set_fill(c, 0, 0, 255, 255);
  c->st[c->depth].comp = C2D_SOURCE_IN;
  c2d_fill_rect(c, 2, 0, 4, 8);
  expect(px(c, 2, 3)[3] == 255 && px(c, 2, 3)[2] == 255);
  expect(px(c, 0, 3)[3] == 0 && px(c, 5, 3)[3] == 0);
  c2d_free(c);

  c = c2d_new(4, 4);
  set_fill(c, 100, 100, 100, 255);
  c2d_fill_rect(c, 0, 0, 4, 4);
  set_fill(c, 200, 50, 255, 255);
  c->st[c->depth].comp = C2D_LIGHTER;
  c2d_fill_rect(c, 0, 0, 4, 4);
  expect(px(c, 1, 1)[0] == 255 && px(c, 1, 1)[1] == 150 && px(c, 1, 1)[2] == 255);
  c->st[c->depth].comp = C2D_MULTIPLY;
  set_fill(c, 128, 128, 128, 255);
  c2d_fill_rect(c, 0, 0, 4, 4);
  expect(abs(px(c, 1, 1)[1] - 75) <= 1); /* 150 * 128/255 */
  c2d_free(c);
}

static void test_shadow(void)
{
  struct c2d *c = c2d_new(30, 30);
  set_fill(c, 255, 0, 0, 255);
  c->st[c->depth].shadow_a = 255;
  c->st[c->depth].shadow_ox = 6;
  c->st[c->depth].shadow_oy = 6;
  c2d_fill_rect(c, 2, 2, 10, 10);
  expect(px(c, 5, 5)[0] == 255 && px(c, 5, 5)[3] == 255);                            /* shape on top */
  expect(px(c, 14, 14)[3] == 255 && px(c, 14, 14)[0] == 0 && px(c, 14, 14)[1] == 0); /* hard black shadow */
  expect(px(c, 20, 20)[3] == 0);
  c2d_free(c);

  c = c2d_new(40, 40);
  set_fill(c, 255, 0, 0, 255);
  c->st[c->depth].shadow_a = 255;
  c->st[c->depth].shadow_blur = 8;
  c2d_fill_rect(c, 14, 14, 12, 12);
  expect(px(c, 12, 20)[3] > 0 && px(c, 12, 20)[3] < 255); /* soft edge outside the shape */
  expect(px(c, 20, 20)[0] == 255);
  c2d_free(c);
}

static void test_draw_image_and_pixel_access(void)
{
  unsigned char img[2 * 2 * 4] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255};
  struct c2d *c = c2d_new(8, 8);
  c->st[c->depth].smoothing = false;
  c2d_draw_image(c, img, 2, 2, 0, 0, 2, 2, 0, 0, 8, 8);
  expect(px(c, 1, 1)[0] == 255 && px(c, 1, 1)[1] == 0); /* top-left red */
  expect(px(c, 6, 1)[1] == 255 && px(c, 6, 1)[0] == 0); /* top-right green */
  expect(px(c, 1, 6)[2] == 255 && px(c, 6, 6)[0] == 255 && px(c, 6, 6)[1] == 255);

  unsigned char out[3 * 2 * 4];
  c2d_get_image_data(c, -1, 0, 3, 2, out);
  expect(out[3] == 0 && out[4 + 3] == 255); /* x = -1 is transparent, x = 0 is not */

  unsigned char patch[2 * 2 * 4];
  memset(patch, 77, sizeof patch);
  c2d_put_image_data(c, patch, 2, 2, 3, 3, 0, 0, 2, 2);
  expect(px(c, 3, 3)[0] == 77 && px(c, 4, 4)[3] == 77 && px(c, 5, 5)[3] == 255);
  c2d_free(c);
}

static void test_transform_and_path_queries(void)
{
  struct c2d *c = c2d_new(40, 40);
  struct c2d_state *st = &c->st[c->depth];
  st->m = c2d_mat_mul((struct c2d_mat){1, 0, 0, 1, 10, 10}, (struct c2d_mat){2, 0, 0, 2, 0, 0});
  set_fill(c, 0, 0, 0, 255);
  c2d_fill_rect(c, 0, 0, 5, 5); /* scaled x2 then moved to (10,10): device 10..20 */
  expect(px(c, 10, 10)[3] == 255 && px(c, 19, 19)[3] == 255 && px(c, 20, 20)[3] == 0 && px(c, 9, 9)[3] == 0);

  struct c2d_path p = {0};
  c2d_path_arc(&p, 0, 20, 20, 10, 0, 6.2831853f, false);
  expect(c2d_path_contains(&p, 20, 20, false) && c2d_path_contains(&p, 26, 20, false));
  expect(!c2d_path_contains(&p, 31, 20, false) && !c2d_path_contains(&p, 28, 28, false));
  c2d_path_free(&p);

  c2d_path_move(&p, 0, 0, 0);
  c2d_path_line(&p, 0, 20, 0);
  expect(c2d_path_stroke_contains(&p, 10, 1.5f, 4) && !c2d_path_stroke_contains(&p, 10, 3, 4));
  c2d_path_free(&p);
  c2d_free(c);
}

static void test_arc_bezier_and_round_rect_areas(void)
{
  struct c2d *c = c2d_new(64, 64);
  struct c2d_path p = {0};
  set_fill(c, 0, 0, 0, 255);
  c2d_path_arc(&p, 0, 32, 32, 20, 0, 6.2831853f, false);
  c2d_fill(c, &p, false);
  expect(fabs(coverage(c) - 3.14159265 * 400) < 12);
  c2d_path_free(&p);
  c2d_free(c);

  c = c2d_new(64, 64);
  set_fill(c, 0, 0, 0, 255);
  float r[4] = {8, 8, 8, 8};
  c2d_path_round_rect(&p, 0, 4, 4, 40, 40, r);
  c2d_fill(c, &p, false);
  expect(fabs(coverage(c) - (1600 - 4 * 64 + 3.14159265 * 64)) < 8);
  expect(px(c, 4, 4)[3] == 0 && px(c, 24, 4)[3] == 255);
  c2d_path_free(&p);
  c2d_free(c);

  c = c2d_new(64, 64);
  memset(&p, 0, sizeof p);
  set_fill(c, 0, 0, 0, 255);
  c2d_path_move(&p, 0, 4, 60);
  c2d_path_bezier(&p, 0, 4, 4, 60, 4, 60, 60); /* an arch over the chord at y = 60 */
  c2d_path_close(&p);
  c2d_fill(c, &p, false);
  /* area between the chord and the curve, integrated numerically: 1881.6 */
  expect(fabs(coverage(c) - 1881.6) < 12);
  c2d_path_free(&p);
  c2d_free(c);
}

static void test_resize_clears_and_resets(void)
{
  struct c2d *c = c2d_new(4, 4);
  set_fill(c, 255, 0, 0, 255);
  c->st[c->depth].line_width = 9;
  c2d_save(c);
  c2d_fill_rect(c, 0, 0, 4, 4);
  expect(c2d_resize(c, 6, 3));
  expect(c->w == 6 && c->h == 3 && c->depth == 0 && c->st[0].line_width == 1);
  expect(coverage(c) == 0);
  expect(!c2d_resize(c, 0, 5) && !c2d_resize(c, 5000, 5000));
  expect(c2d_new(5000, 5000) == 0);
  c2d_free(c);
}

/* Walk the stored-block zlib stream and CRC-check each chunk; returns the raw scanlines */
static unsigned char *png_unpack(const unsigned char *png, int len, unsigned *w, unsigned *h)
{
  unsigned char *idat = malloc((size_t)len), *raw = malloc((size_t)len);
  int idat_n = 0, pos = 8;
  *w = *h = 0;
  expect(len > 8 && memcmp(png, "\x89PNG\r\n\x1a\n", 8) == 0);
  while (pos + 12 <= len) {
    unsigned n = (unsigned)png[pos] << 24 | png[pos + 1] << 16 | png[pos + 2] << 8 | png[pos + 3];
    const unsigned char *type = png + pos + 4, *data = png + pos + 8;
    unsigned crc = (unsigned)data[n] << 24 | data[n + 1] << 16 | data[n + 2] << 8 | data[n + 3];
    expect(crc32(0, type, n + 4) == crc);
    if (!memcmp(type, "IHDR", 4)) {
      *w = (unsigned)data[0] << 24 | data[1] << 16 | data[2] << 8 | data[3];
      *h = (unsigned)data[4] << 24 | data[5] << 16 | data[6] << 8 | data[7];
      expect(data[8] == 8 && data[9] == 6);
    } else if (!memcmp(type, "IDAT", 4)) {
      memcpy(idat + idat_n, data, n);
      idat_n += (int)n;
    }
    pos += 12 + (int)n;
  }
  int rn = 0, zp = 2;
  for (;;) {
    int final = idat[zp] & 1;
    unsigned n = idat[zp + 1] | idat[zp + 2] << 8;
    expect((idat[zp + 3] | idat[zp + 4] << 8) == (~n & 0xffff));
    memcpy(raw + rn, idat + zp + 5, n);
    rn += (int)n;
    zp += 5 + (int)n;
    if (final)
      break;
  }
  free(idat);
  return raw;
}

static void test_png_export(void)
{
  struct c2d *c = c2d_new(300, 250); /* big enough for several stored blocks */
  set_fill(c, 10, 20, 30, 255);
  c2d_fill_rect(c, 0, 0, 300, 250);
  set_fill(c, 200, 100, 50, 128);
  c2d_fill_rect(c, 7, 9, 1, 1);
  int len = 0;
  unsigned char *png = c2d_encode_png(c, &len);
  unsigned w, h;
  unsigned char *raw = png_unpack(png, len, &w, &h);
  expect(w == 300 && h == 250);
  size_t row = 300 * 4 + 1;
  expect(raw[0] == 0 && raw[1] == 10 && raw[2] == 20 && raw[3] == 30 && raw[4] == 255);
  for (unsigned y = 0; y < 250; y++)
    expect(memcmp(raw + y * row + 1, c->px + y * 1200, 1200) == 0);
  free(raw);
  free(png);
  c2d_free(c);
}

static unsigned char *read_file(const char *path, int *n)
{
  FILE *f = fopen(path, "rb");
  if (!f)
    return 0;
  fseek(f, 0, SEEK_END);
  *n = (int)ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *b = malloc((size_t)*n);
  expect(fread(b, 1, (size_t)*n, f) == (size_t)*n);
  fclose(f);
  return b;
}

static void test_text(void)
{
  int n = 0;
  unsigned char *ttf = read_file("fonts/LiberationSans-Regular.ttf", &n);
  expect(ttf != 0);
  struct font *f = font_load(ttf, n);
  expect(f != 0);
  struct c2d *c = c2d_new(120, 40);
  c->sans = f;
  c->st[c->depth].font_px = 20;
  float w1 = c2d_measure_text(c, "i"), w3 = c2d_measure_text(c, "iii"), wm = c2d_measure_text(c, "M");
  expect(w1 > 0 && wm > w1 && fabs(w3 - 3 * w1) < 0.5);
  set_fill(c, 0, 0, 0, 255);
  c2d_fill_text(c, "Hi", 10, 30, false);
  double ink = coverage(c);
  expect(ink > 20);
  int below = 0, rows_above = 0;
  for (int y = 0; y < 40; y++)
    for (int x = 0; x < 120; x++)
      if (px(c, x, y)[3]) {
        if (y > 31)
          below++;
        if (y < 14)
          rows_above++;
      }
  expect(below == 0 && rows_above == 0); /* "Hi" has no descender and its cap height is under 16px */

  /* textAlign right puts the same ink to the left of x */
  struct c2d *r = c2d_new(120, 40);
  r->sans = f;
  r->st[r->depth].font_px = 20;
  r->st[r->depth].text_align = C2D_ALIGN_RIGHT;
  set_fill(r, 0, 0, 0, 255);
  c2d_fill_text(r, "Hi", 110, 30, false);
  int maxx = 0;
  for (int y = 0; y < 40; y++)
    for (int x = 0; x < 120; x++)
      if (px(r, x, y)[3] && x > maxx)
        maxx = x;
  expect(maxx <= 110 && maxx > 100);
  expect(fabs(coverage(r) - ink) < 2);

  /* a rotated transform still draws, with equal total ink */
  struct c2d *t = c2d_new(60, 60);
  t->sans = f;
  t->st[t->depth].font_px = 20;
  t->st[t->depth].m = (struct c2d_mat){0, 1, -1, 0, 40, 10}; /* 90 degrees */
  set_fill(t, 0, 0, 0, 255);
  c2d_fill_text(t, "Hi", 0, 0, false);
  expect(fabs(coverage(t) - ink) < ink * 0.15);
  c2d_free(t);
  c2d_free(r);
  c2d_free(c);
  font_free(f);
  free(ttf);
}

int main(void)
{
  test("fillRect blends with alpha", test_fill_rect_and_alpha_blend);
  test("fractional rect edges are anti-aliased", test_fractional_rect_coverage);
  test("triangle area and fill rules", test_triangle_area_and_fill_rules);
  test("stroke widths, caps and joins", test_stroke_width_joins_and_caps);
  test("dash patterns", test_dash_pattern);
  test("linear and radial gradients", test_gradients);
  test("clip and save/restore", test_clip_and_save_restore);
  test("composite operations", test_composite_operations);
  test("shadows", test_shadow);
  test("drawImage and image data", test_draw_image_and_pixel_access);
  test("transform and path queries", test_transform_and_path_queries);
  test("arc, bezier and round rect areas", test_arc_bezier_and_round_rect_areas);
  test("resize clears and resets state", test_resize_clears_and_resets);
  test("PNG export", test_png_export);
  test("text", test_text);
  return 0;
}
