/* jpeg_test.c, host-side unit test for user/lib/jpeg/jpeg.c against cjpeg-encoded fixtures of a known gradient. */
#define OWOS_SUITE_TEST
#include "jpeg.h"
#include "jpeg_fixtures.h"
#include <stdlib.h>
#include <string.h>

#define W 35
#define H 19

static int expect_px(int x, int y, int c)
{
  int v[3] = {x * 255 / (W - 1), y * 255 / (H - 1), 128 + (x - y) * 3};
  return v[c] < 0 ? 0 : v[c] > 255 ? 255 : v[c];
}

/* mean absolute error against the gradient; gray compares against the luma of each pixel */
static double mean_error(const unsigned char *rgba, unsigned w, unsigned h, int gray)
{
  long sum = 0;
  for (unsigned y = 0; y < h; y++)
    for (unsigned x = 0; x < w; x++)
      for (int c = 0; c < 3; c++) {
        int want = gray ? (299 * expect_px(x, y, 0) + 587 * expect_px(x, y, 1) + 114 * expect_px(x, y, 2)) / 1000
                        : expect_px(x, y, c);
        sum += abs(rgba[(y * w + x) * 4 + c] - want);
      }
  return (double)sum / (w * h * 3);
}

static void check(const unsigned char *jpg, unsigned len, int gray, double tol)
{
  unsigned char *out;
  unsigned w, h;
  expect_eq(jpeg_decode(jpg, (int)len, 0, &out, &w, &h), JPEG_OK);
  expect_eq(w, W);
  expect_eq(h, H);
  expect(out && mean_error(out, w, h, gray) <= tol);
  expect(out && out[3] == 255);
  free(out);
}

static void test_baseline_444(void) { check(jpg_b444, sizeof jpg_b444, 0, 1.5); }
static void test_baseline_420(void) { check(jpg_b420, sizeof jpg_b420, 0, 5); }
static void test_baseline_422(void) { check(jpg_b422, sizeof jpg_b422, 0, 5); }
static void test_progressive(void) { check(jpg_prog420, sizeof jpg_prog420, 0, 5); }
static void test_restart_intervals(void) { check(jpg_rst420, sizeof jpg_rst420, 0, 5); }
static void test_grayscale(void) { check(jpg_gray, sizeof jpg_gray, 1, 1.5); }

static void test_downscale(void)
{
  unsigned char *out;
  unsigned w, h;
  expect_eq(jpeg_decode(jpg_b444, sizeof jpg_b444, 10, &out, &w, &h), JPEG_OK);
  expect_eq(w, 18);
  expect_eq(h, 10);
  expect(out && abs(out[(5 * w + 9) * 4] - expect_px(18, 10, 0)) <= 8);
  free(out);
  expect_eq(jpeg_decode(jpg_b444, sizeof jpg_b444, 1000, &out, &w, &h), JPEG_OK);
  expect_eq(w, W);
  free(out);
}

static void test_rejects_garbage(void)
{
  unsigned char *out = (unsigned char *)1;
  unsigned w, h;
  const unsigned char junk[] = "not a jpeg at all";
  expect_eq(jpeg_decode(junk, sizeof junk, 0, &out, &w, &h), JPEG_EFORMAT);
  expect(out == 0);
  expect_eq(jpeg_decode(jpg_b444, 2, 0, &out, &w, &h), JPEG_EFORMAT);
}

/* every truncation and a byte-flip sweep must return cleanly, with *out set only on success */
static void test_damaged_input_is_safe(void)
{
  const unsigned char *srcs[] = {jpg_b420, jpg_prog420, jpg_rst420};
  const unsigned lens[] = {sizeof jpg_b420, sizeof jpg_prog420, sizeof jpg_rst420};
  for (int s = 0; s < 3; s++) {
    unsigned char *buf = malloc(lens[s]);
    for (unsigned n = 0; n < lens[s]; n += 7) {
      unsigned char *out;
      unsigned w, h;
      int rc = jpeg_decode(srcs[s], (int)n, 0, &out, &w, &h);
      expect((rc == JPEG_OK) == (out != 0));
      free(out);
    }
    for (unsigned i = 0; i < lens[s]; i += 3) {
      unsigned char *out;
      unsigned w, h;
      memcpy(buf, srcs[s], lens[s]);
      buf[i] ^= 0xA5;
      int rc = jpeg_decode(buf, (int)lens[s], 0, &out, &w, &h);
      expect((rc == JPEG_OK) == (out != 0));
      free(out);
    }
    free(buf);
  }
}

int main(void)
{
  test("JPEG baseline 4:4:4", test_baseline_444);
  test("JPEG baseline 4:2:0", test_baseline_420);
  test("JPEG baseline 4:2:2", test_baseline_422);
  test("JPEG progressive", test_progressive);
  test("JPEG restart intervals", test_restart_intervals);
  test("JPEG grayscale", test_grayscale);
  test("JPEG downscale", test_downscale);
  test("JPEG rejects garbage", test_rejects_garbage);
  test("JPEG damaged input is safe", test_damaged_input_is_safe);
  return 0;
}
