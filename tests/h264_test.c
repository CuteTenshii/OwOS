/* h264_test.c — host-side test for user/lib/h264 (the H.264 decoder).
 *
 * Decodes an Annex-B H.264 elementary stream and compares each frame's YCbCr
 * planes against the ground truth ffmpeg produces from the same stream. H.264
 * reconstruction is integer-exact, so every frame must match bit for bit (PSNR
 * 99 dB). tests/h264_vectors.sh builds a set of libx264 streams and runs this:
 *
 *   cc -std=c11 -Itests -Iuser/lib/h264 tests/h264_test.c user/lib/h264/<all .c> \
 *      -lm -o /tmp/h264_test
 *   /tmp/h264_test stream.264 truth.yuv 176 144
 */
#include "h264.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *slurp(const char *path, long *len)
{
  FILE *f = fopen(path, "rb");
  unsigned char *b;
  if (!f) {
    perror(path);
    exit(2);
  }
  fseek(f, 0, SEEK_END);
  *len = ftell(f);
  fseek(f, 0, SEEK_SET);
  b = malloc(*len);
  if (fread(b, 1, *len, f) != (size_t)*len) {
    fprintf(stderr, "read %s\n", path);
    exit(2);
  }
  fclose(f);
  return b;
}

static double psnr(const unsigned char *a, int as, const unsigned char *b, int bs, int w, int h)
{
  double mse = 0;
  int x, y;
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++) {
      int dd = a[y * as + x] - b[y * bs + x];
      mse += (double)dd * dd;
    }
  mse /= (double)w * h;
  if (mse < 1e-9)
    return 99.0;
  return 10.0 * log10(255.0 * 255.0 / mse);
}

int main(int argc, char **argv)
{
  long slen, ylen;
  unsigned char *stream, *truth;
  int W, H, cw, ch, fsz, nframes, fi, fails = 0;
  double minp = 99.0;
  h264_decoder *d;

  if (argc < 5) {
    fprintf(stderr, "usage: %s stream.264 truth.yuv W H\n", argv[0]);
    return 2;
  }
  W = atoi(argv[3]);
  H = atoi(argv[4]);
  cw = (W + 1) / 2;
  ch = (H + 1) / 2;
  fsz = W * H + 2 * cw * ch;

  stream = slurp(argv[1], &slen);
  truth = slurp(argv[2], &ylen);
  nframes = (int)(ylen / fsz);

  if (h264_decoder_open(stream, (unsigned)slen, &d) != H264_OK) {
    fprintf(stderr, "h264_decoder_open failed\n");
    return 1;
  }
  if (h264_width(d) != W || h264_height(d) != H) {
    fprintf(stderr, "dims %dx%d != %dx%d\n", h264_width(d), h264_height(d), W, H);
    fails++;
  }
  printf("stream %ld bytes, %dx%d, %d ground-truth frames\n", slen, W, H, nframes);

  for (fi = 0; fi < nframes; fi++) {
    int ys, cbs, crs;
    const unsigned char *Y, *Cb, *Cr, *tY, *tCb, *tCr;
    double py, pcb, pcr;
    const char *types = "PBI";
    int t, rc = h264_decode_frame(d);
    if (rc != H264_OK) {
      printf("frame %d: decode ended (%d)\n", fi, rc);
      fails++;
      break;
    }
    t = h264_frame_type(d);
    Y = h264_plane_y(d, &ys);
    Cb = h264_plane_cb(d, &cbs);
    Cr = h264_plane_cr(d, &crs);
    tY = truth + (long)fi * fsz;
    tCb = tY + W * H;
    tCr = tCb + cw * ch;
    py = psnr(Y, ys, tY, W, W, H);
    pcb = psnr(Cb, cbs, tCb, cw, cw, ch);
    pcr = psnr(Cr, crs, tCr, cw, cw, ch);
    if (py < minp)
      minp = py;
    printf("frame %d [%c]  Y %.1f dB  Cb %.1f dB  Cr %.1f dB\n", fi, types[t >= 0 && t < 3 ? t : 2], py, pcb, pcr);
    if (py < 99.0 || pcb < 99.0 || pcr < 99.0)
      fails++;
  }

  h264_decoder_close(d);
  free(stream);
  free(truth);
  if (fails) {
    printf("%d failure(s), min Y PSNR %.1f dB\n", fails, minp);
    return 1;
  }
  printf("all h264 decoder frames bit-exact (min Y %.1f dB)\n", minp);
  return 0;
}
