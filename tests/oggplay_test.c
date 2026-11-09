/* oggplay_test.c, host test for user/lib/ogg/oggplay.c (streaming Ogg Vorbis).
 *
 * Decodes an .ogg through oggplay and compares against a libvorbis decode of the
 * same file (native rate, stereo s16le; scripts/ogg_gen.sh uses VLC, since
 * ffmpeg's own Vorbis decoder disagrees with libvorbis): length from the last
 * granule position, sample accuracy (PSNR) over the reference's span, and that
 * oggplay_seek lands a clean decode near the target.
 *
 * Build + run (scripts/ogg_gen.sh generates the vectors):
 *   cc -std=gnu11 -fsanitize=address -Iscripts -Iuser/lib/ogg -Iuser/lib/math \
 *      tests/oggplay_test.c user/lib/ogg/ogg.c user/lib/ogg/vorbis.c \
 *      user/lib/ogg/oggplay.c -lm -o /tmp/oggplay_test
 *   /tmp/oggplay_test in.ogg ref.s16
 */
#include "oggplay.h"
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
    fprintf(stderr, "usage: %s in.ogg ref.s16\n", argv[0]);
    return 2;
  }
  long rlen;
  int16_t *ref = slurp(argv[2], &rlen);
  long refn = rlen / 4;
  FILE *f = fopen(argv[1], "rb");
  fseek(f, 0, SEEK_END);
  unsigned size = (unsigned)ftell(f);
  fclose(f);

  oggplay *o = oggplay_open(argv[1], size);
  if (!o) {
    printf("FAIL: open\n");
    return 1;
  }
  printf("%u Hz, total %u, ref %ld\n", oggplay_rate(o), oggplay_total_frames(o), refn);
  long total = oggplay_total_frames(o);

  short *out = malloc((refn + 4096) * 4);
  long got = 0;
  for (;;) {
    int n = oggplay_read(o, out + got * 2, 1000);
    if (n <= 0)
      break;
    got += n;
    if (got > refn + 2048)
      break;
  }
  /* The reference tool drops a stream-dependent run of leading frames; find it. */
  long lead = 0;
  {
    double bq = 0;
    for (long l = 0; l <= 4096; l++) {
      long at = 20000;
      if (at + 2000 + l > got || at + 2000 > refn)
        break;
      double q = psnr(out + (at + l) * 2, ref + at * 2, 4000);
      if (q > bq) {
        bq = q;
        lead = l;
      }
    }
    printf("reference lead %ld frames\n", lead);
    int16_t *pad = calloc(rlen / 2 + lead * 2 + 1, 2);
    memcpy(pad + lead * 2, ref, rlen);
    ref = pad;
    refn += lead;
  }
  CHECK(total >= refn - lead, "total %ld shorter than ref %ld", total, refn);
  long n = got < refn ? got : refn;
  double p = psnr(out + lead * 2, ref + lead * 2, (n - lead) * 2);
  printf("decoded %ld frames; PSNR %.2f dB\n", got, p);
  CHECK(got == total, "frame count %ld != total %ld", got, total);
  CHECK(p >= 60.0, "PSNR %.2f < 60", p);

  static const unsigned pct[] = {0, 10, 50, 90};
  for (unsigned i = 0; i < sizeof pct / sizeof *pct; i++) {
    unsigned target = (unsigned)((long long)refn * pct[i] / 100) + (pct[i] ? 0 : (unsigned)lead);
    oggplay_seek(o, target);
    int blk = oggplay_read(o, out, 4096);
    long best = 0;
    double bp = 0;
    for (long d = -4096; d <= 4096 && blk > 0; d++) {
      long s = (long)target + d;
      if (s < lead || s + blk > refn)
        continue;
      double q = psnr(out, ref + s * 2, (long)blk * 2);
      if (q > bp) {
        bp = q;
        best = d;
      }
    }
    printf("seek %u%% target %u, aligned drift %ld, PSNR %.2f dB\n", pct[i], target, best, bp);
    CHECK(blk > 0, "no data after seek");
    CHECK(bp >= 60.0, "post-seek PSNR %.2f", bp);
    CHECK(best >= -2304 && best <= 2304, "drift %ld", best);
  }
  oggplay_close(o);
  printf(fails ? "FAILED\n" : "ok\n");
  return fails != 0;
}
