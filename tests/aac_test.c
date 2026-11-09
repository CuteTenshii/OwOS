/* aac_test.c — host unit test for user/lib/aac (the AAC-LC decoder).
 *
 * Decodes an ADTS .aac elementary stream with our decoder and compares the PCM
 * against a reference produced by ffmpeg (the same .aac decoded to s16le). Both
 * decoders turn identical access units into identical 1024-sample frames, so the
 * streams align frame-for-frame with no relative delay; a high PSNR confirms the
 * whole pipeline (Huffman, dequant, stereo tools, TNS, IMDCT filterbank).
 *
 * Build + run (see tests/aac_gen.sh which generates the vectors):
 *   cc -std=c11 -Itests -Iuser/lib/math -Iuser/lib/aac \
 *      tests/aac_test.c user/lib/aac/aac.c -lm -o /tmp/aac_test
 *   /tmp/aac_test in.aac ref.s16
 */
#include "aac.h"
#include "owos.h"
#include <math.h>
#include <stdint.h>

static uint8_t *slurp(const char *path, long *len)
{
  FILE *f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "open %s failed\n", path);
    return NULL;
  }
  fseek(f, 0, SEEK_END);
  *len = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *b = malloc(*len);
  if (fread(b, 1, *len, f) != (size_t)*len) {
    fclose(f);
    return NULL;
  }
  fclose(f);
  return b;
}

int main(int argc, char **argv)
{
  if (argc < 3) {
    fprintf(stderr, "usage: %s in.aac ref.s16\n", argv[0]);
    return 2;
  }

  long alen = 0, rlen = 0;
  uint8_t *adts = slurp(argv[1], &alen);
  uint8_t *ref = slurp(argv[2], &rlen);
  if (!adts || !ref)
    return 2;

  aac_decoder *d = aac_open_adts();
  if (!d) {
    fprintf(stderr, "aac_open_adts failed\n");
    return 2;
  }

  /* Decode all ADTS frames into one interleaved int16 buffer. */
  long cap = 1 << 20;
  int16_t *out = malloc(cap * sizeof(int16_t));
  long total = 0; /* interleaved samples produced           */
  uint32_t off = 0, channels = 0, rate = 0;
  int16_t frame[2048 * AAC_MAX_CHANNELS];
  int frames = 0;

  while (off + 7 <= (uint32_t)alen) {
    uint32_t consumed = 0, ns = 0, nc = 0;
    int rc = aac_decode_adts(d, adts + off, (uint32_t)alen - off, &consumed, frame, &ns, &nc);
    if (rc != 0) {
      fprintf(stderr, "decode error %d at off %u\n", rc, off);
      break;
    }
    if (consumed == 0)
      break;
    off += consumed;
    if (!channels) {
      aac_info info;
      aac_get_info(d, &info);
      channels = nc;
      rate = info.sample_rate;
    }
    long need = total + (long)ns * nc;
    if (need > cap) {
      cap = need * 2;
      out = realloc(out, cap * sizeof(int16_t));
    }
    for (uint32_t i = 0; i < ns * nc; i++)
      out[total + i] = frame[i];
    total += (long)ns * nc;
    frames++;
  }

  long refn = rlen / 2; /* reference interleaved samples          */
  long n = total < refn ? total : refn;
  printf("decoded %d frames, %ld samples (%u ch @ %u Hz); ref %ld samples\n", frames, total, channels, rate, refn);
  if (n == 0) {
    fprintf(stderr, "no overlap to compare\n");
    return 1;
  }

  const int16_t *r16 = (const int16_t *)ref;
  double noise = 0;
  long maxdiff = 0;
  for (long i = 0; i < n; i++) {
    long diff = (long)out[i] - (long)r16[i];
    if (diff < 0)
      diff = -diff;
    if (diff > maxdiff)
      maxdiff = diff;
    noise += (double)diff * (double)diff;
  }
  double mse = noise / n;
  double psnr = mse > 0 ? 10.0 * log10((32767.0 * 32767.0) / mse) : 999.0;
  printf("PSNR %.2f dB, max abs diff %ld, MSE %.4f over %ld samples\n", psnr, maxdiff, mse, n);

  /* A correct decoder differs from ffmpeg only by float rounding (and any PNS
   * bands, which are random): expect well above 60 dB on tonal content. */
  return psnr >= 60.0 ? 0 : 1;
}
