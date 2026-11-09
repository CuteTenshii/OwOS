/* Host test for the lossy VP8 WebP decoder (user/lib/webp) against libwebp on generated images: each is encoded by
 * cwebp at several qualities and sizes (odd dimensions, alpha) and must decode bit-exactly to dwebp's RGBA. Skipped
 * when ImageMagick or the libwebp tools are not installed. */
#define OWOS_SUITE_TEST
#include "test.h"
#include "webp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char dir[512];

static unsigned char *slurp(const char *path, long *len)
{
  FILE *f = fopen(path, "rb");
  *len = -1;
  if (!f)
    return 0;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *buf = malloc(n ? (size_t)n : 1);
  if (fread(buf, 1, (size_t)n, f) == (size_t)n)
    *len = n;
  fclose(f);
  return buf;
}

/* RGBA pixels of a binary PAM (P7) file as dwebp writes it. */
static unsigned char *read_pam(const char *path, int *w, int *h)
{
  long len;
  unsigned char *buf = slurp(path, &len);
  *w = *h = 0;
  if (!buf || len < 0) {
    free(buf);
    return 0;
  }
  const char *end = memmem(buf, (size_t)len, "ENDHDR\n", 7);
  const char *wp = memmem(buf, (size_t)len, "WIDTH ", 6), *hp = memmem(buf, (size_t)len, "HEIGHT ", 7);
  if (!end || !wp || !hp || sscanf(wp + 6, "%d", w) != 1 || sscanf(hp + 7, "%d", h) != 1 ||
      (const unsigned char *)end + 7 + (long)*w * *h * 4 > buf + len) {
    free(buf);
    return 0;
  }
  unsigned char *px = malloc((size_t)*w * (size_t)*h * 4);
  memcpy(px, end + 7, (size_t)*w * (size_t)*h * 4);
  free(buf);
  return px;
}

static void check_one(const char *magick_args, int quality)
{
  char cmd[2048], webp[600], pam[600];
  snprintf(webp, sizeof webp, "%s/t.webp", dir);
  snprintf(pam, sizeof pam, "%s/t.pam", dir);
  snprintf(cmd, sizeof cmd,
           "magick %s '%s/src.png' && cwebp -quiet -q %d '%s/src.png' -o '%s' && dwebp -quiet '%s' -pam -o '%s'",
           magick_args, dir, quality, dir, webp, webp, pam);
  expect_eq(system(cmd), 0);

  long wlen;
  unsigned char *data = slurp(webp, &wlen), *out = 0;
  unsigned w = 0, h = 0;
  int rw, rh;
  unsigned char *ref = read_pam(pam, &rw, &rh);
  expect(data && wlen > 0 && ref);
  if (data && wlen > 0 && ref) {
    expect_eq(webp_decode(data, (int)wlen, &out, &w, &h), WEBP_OK);
    expect_eq(w, rw);
    expect_eq(h, rh);
    long diff = -1;
    if (out && (int)w == rw && (int)h == rh) {
      diff = 0;
      for (long i = 0; i < (long)w * h * 4; i++)
        diff += out[i] != ref[i];
    }
    expect_eq(diff, 0);
  }
  free(out);
  free(ref);
  free(data);
}

static void check_against_libwebp(void)
{
  static const char *const images[] = {
      "-size 64x64 gradient:red-blue",
      "-size 37x21 plasma:fractal -seed 3",
      "-size 129x77 xc:gray50 +noise Gaussian -seed 5",
      "-size 50x33 gradient:none-green -alpha set",
      "-size 1x1 xc:orange",
  };
  static const int qualities[] = {5, 50, 95};
  for (size_t i = 0; i < sizeof images / sizeof images[0]; i++)
    for (size_t q = 0; q < sizeof qualities / sizeof qualities[0]; q++)
      check_one(images[i], qualities[q]);
}

int main(void)
{
  if (system("{ command -v magick && command -v cwebp && command -v dwebp; } >/dev/null 2>&1") != 0) {
    printf("vp8: ImageMagick or libwebp tools not installed, skipping\n");
    return 0;
  }
  const char *tmp = getenv("TMPDIR");
  snprintf(dir, sizeof dir, "%s/owos-vp8-XXXXXX", tmp && *tmp ? tmp : "/tmp");
  expect(mkdtemp(dir) != 0);
  test("bit-exact with dwebp", check_against_libwebp);
  char cmd[600];
  snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
  (void)!system(cmd);
  return 0;
}
