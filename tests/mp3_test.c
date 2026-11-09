/* mp3_test.c — host test for user/lib/mp3 (streaming MPEG audio decoder).
 *
 * Decodes an .mp3 with our wrapper and compares against ffmpeg's decode of the
 * same file (native rate, stereo s16le). Checks the gapless trim (frame count
 * equals the reference), sample accuracy (PSNR), the reported length, and that
 * mp3_seek lands near the requested frame with a clean, warmed-up decode.
 *
 * Build + run (scripts/mp3_gen.sh generates the vectors):
 *   cc -std=gnu11 -fsanitize=address -Iscripts -Iuser/lib/mp3 \
 *      tests/mp3_test.c user/lib/mp3/mp3.c -lm -o /tmp/mp3_test
 *   /tmp/mp3_test in.mp3 ref.s16 [exact-length 0|1]
 */
#include "mp3.h"
#include "owos.h"
#include <math.h>

static int16_t *slurp(const char *path, long *len)
{
  FILE *f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "open %s failed\n", path);
    exit(2);
  }
  fseek(f, 0, SEEK_END);
  *len = ftell(f);
  fseek(f, 0, SEEK_SET);
  int16_t *b = malloc(*len + 2);
  if (fread(b, 1, *len, f) != (size_t)*len)
    exit(2);
  fclose(f);
  return b;
}

static double psnr(const short *a, const int16_t *b, long n)
{
  double noise = 0;
  for (long i = 0; i < n; i++) {
    double d = (double)a[i] - (double)b[i];
    noise += d * d;
  }
  double mse = noise / (n ? n : 1);
  return mse > 0 ? 10.0 * log10(32767.0 * 32767.0 / mse) : 999.0;
}

static int fails;
#define CHECK(c, ...)                                                                                                  \
  do {                                                                                                                 \
    if (!(c)) {                                                                                                        \
      fails++;                                                                                                         \
      printf("FAIL: " __VA_ARGS__);                                                                                    \
      printf("\n");                                                                                                    \
    }                                                                                                                  \
  } while (0)

int main(int argc, char **argv)
{
  if (argc < 3) {
    fprintf(stderr, "usage: %s in.mp3 ref.s16 [exact]\n", argv[0]);
    return 2;
  }
  long rlen;
  int16_t *ref = slurp(argv[2], &rlen);
  long refn = rlen / 4;
  int want_exact = argc > 3 && atoi(argv[3]);

  FILE *f = fopen(argv[1], "rb");
  fseek(f, 0, SEEK_END);
  unsigned size = (unsigned)ftell(f);
  fclose(f);

  static mp3_t m;
  if (mp3_open(&m, argv[1], size) != 0) {
    printf("FAIL: open\n");
    return 1;
  }
  printf("%u Hz, %u ch, total %u (%s), ref %ld\n", mp3_rate(&m), mp3_channels(&m), mp3_total_frames(&m),
         mp3_total_exact(&m) ? "exact" : "estimated", refn);

  short *out = malloc((refn + 4096) * 4);
  long got = 0;
  for (;;) {
    int n = mp3_read(&m, out + got * 2, 1000);
    if (n <= 0)
      break;
    got += n;
    if (got > refn + 2048)
      break;
  }
  long n = got < refn ? got : refn;
  double p = psnr(out, ref, n * 2);
  printf("decoded %ld frames; PSNR %.2f dB\n", got, p);
  CHECK(got == refn, "frame count %ld != ref %ld", got, refn);
  CHECK(p >= 60.0, "PSNR %.2f < 60", p);
  if (want_exact) {
    CHECK(mp3_total_exact(&m), "length not exact");
    CHECK((long)mp3_total_frames(&m) == refn, "total %u != ref %ld", mp3_total_frames(&m), refn);
  } else {
    long t = (long)mp3_total_frames(&m);
    CHECK(t > refn * 9 / 10 && t < refn * 11 / 10, "estimated total %ld far from %ld", t, refn);
  }

  static const unsigned pct[] = {0, 10, 50, 90};
  for (unsigned i = 0; i < sizeof pct / sizeof *pct; i++) {
    unsigned target = (unsigned)((long long)refn * pct[i] / 100);
    unsigned at = mp3_seek(&m, target);
    int blk = mp3_read(&m, out, 4096);
    if (pct[i] == 0) {
      CHECK(at == 0, "seek 0 returned %u", at);
    }
    /* decode may resync up to a frame off the reported position */
    long best = -1;
    double bp = 0;
    for (long d = -6000; d <= 6000 && blk > 0; d += 1) {
      long s = (long)at + d;
      if (s < 0 || s + blk > refn)
        continue;
      double q = psnr(out, ref + s * 2, (long)blk * 2);
      if (q > bp) {
        bp = q;
        best = d;
      }
    }
    printf("seek %u%% target %u -> %u, aligned drift %ld, PSNR %.2f dB\n", pct[i], target, at, best, bp);
    CHECK(blk > 0, "no data after seek");
    CHECK(bp >= 60.0, "post-seek PSNR %.2f", bp);
    CHECK(best >= -2304 && best <= 2304, "drift %ld", best);
    CHECK((long)at <= (long)target + 2304 && (long)at + 2304 >= (long)target, "landed %u for target %u", at, target);
  }
  printf(fails ? "FAILED\n" : "ok\n");
  return fails != 0;
}
