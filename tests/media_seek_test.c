/* media_seek_test.c, host test for media_open/media_read/media_seek_ms on AAC
 * (ADTS and MP4) and Ogg Vorbis files.
 *
 * Plays a file through the media engine at 48 kHz (so the resampler is the
 * identity) and compares against a reference decode: the whole stream, then
 * after seeks. The reference span is searched for the best alignment, so the
 * check is that seeks land cleanly, and the reported position is compared with
 * where the audio really is.
 *
 * Build + run (scripts/aac_seek_gen.sh generates the vectors):
 *   cc -std=gnu11 -Iscripts ... tests/media_seek_test.c -o /tmp/media_seek_test
 *   /tmp/media_seek_test in.{aac,m4a} ref.s16 max-drift-frames
 */
#include "media.h"
#include "owos.h"
#include <math.h>

int audio_open(void) { return -1; }
int audio_write(const void *p, int n)
{
  (void)p;
  return n;
}
int audio_close(void) { return 0; }
int thread_create(void (*fn)(void), void *stack)
{
  (void)fn;
  (void)stack;
  return -1;
}
void msleep(unsigned ms) { (void)ms; }

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
  if (argc < 4) {
    fprintf(stderr, "usage: %s in ref.s16 max-drift\n", argv[0]);
    return 2;
  }
  long rlen, maxdrift = atol(argv[3]);
  int16_t *ref = slurp(argv[2], &rlen);
  long refn = rlen / 4;

  media_decoder *d = media_open(argv[1]);
  if (!d) {
    printf("FAIL: open\n");
    return 1;
  }
  printf("duration %u ms, ref %ld frames (%ld ms)\n", media_duration_ms(d), refn, refn * 1000 / MEDIA_RATE);

  short *out = malloc((refn + 8192) * 4);
  long got = 0;
  for (;;) {
    int n = media_read(d, out + got * 2, 4096);
    if (n <= 0)
      break;
    got += n;
    if (got > refn + 4096)
      break;
  }
  long n = got < refn ? got : refn;
  double p = psnr(out, ref, n * 2);
  printf("decoded %ld frames; PSNR %.2f dB\n", got, p);
  CHECK(labs(got - refn) <= 2048, "length %ld vs ref %ld", got, refn);
  CHECK(p >= 60.0, "PSNR %.2f < 60", p);
  long dur = media_duration_ms(d);
  CHECK(dur > refn * 1000 / MEDIA_RATE * 9 / 10 && dur < refn * 1000 / MEDIA_RATE * 11 / 10, "duration %ld ms off",
        dur);

  static const unsigned pct[] = {0, 10, 50, 90};
  for (unsigned i = 0; i < sizeof pct / sizeof *pct; i++) {
    unsigned ms = (unsigned)((long long)refn * pct[i] / 100 * 1000 / MEDIA_RATE);
    media_seek_ms(d, ms);
    long claimed = (long)media_position_ms(d) * MEDIA_RATE / 1000;
    int blk = media_read(d, out, 4096);
    long best = 0;
    double bp = 0;
    for (long s = claimed - 8192; s <= claimed + 8192 && blk > 0; s++) {
      if (s < 0 || s + blk > refn)
        continue;
      double q = psnr(out, ref + s * 2, (long)blk * 2);
      if (q > bp) {
        bp = q;
        best = s;
      }
    }
    long drift = best - claimed;
    printf("seek %u%% (%u ms): claimed %ld, audio at %ld, drift %ld frames, PSNR %.2f dB\n", pct[i], ms, claimed, best,
           drift, bp);
    CHECK(blk > 0, "no data after seek");
    CHECK(bp >= 60.0, "post-seek PSNR %.2f (first block unclean?)", bp);
    CHECK(labs(drift) <= maxdrift, "drift %ld > %ld", drift, maxdrift);
  }
  media_close(d);
  printf(fails ? "FAILED\n" : "ok\n");
  return fails != 0;
}
