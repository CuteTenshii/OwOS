/* mpeg.c — a small MPEG-1 video command-line tool.
 *
 *   mpeg info <file.mpg>            decode the stream, print dimensions + a
 * frame summary (per-picture I/P/B type) mpeg gen  <out.mpg> [frames]    encode
 * a synthetic animated clip (a moving gradient) — exercises the encoder end to
 * end
 *
 * The codec itself lives in user/lib/mpeg; this is just the front end. An
 * encoded clip round-trips: `mpeg gen /tmp/x.mpg` then `mpeg info /tmp/x.mpg`.
 */
#include "mpeg.h"
#include "owos.h"

static unsigned char *read_whole(const char *path, unsigned *out_len)
{
  struct statinfo st;
  unsigned char *buf;
  unsigned off = 0;
  if (stat(path, &st) != 0 || st.size == 0)
    return 0;
  buf = malloc(st.size);
  if (!buf)
    return 0;
  while (off < st.size) {
    int n = readfile_at(path, off, buf + off, st.size - off);
    if (n <= 0)
      break;
    off += (unsigned)n;
  }
  if (off != st.size) {
    free(buf);
    return 0;
  }
  *out_len = st.size;
  return buf;
}

static int cmd_info(const char *path)
{
  unsigned len = 0, nf = 0, ni = 0, np = 0, nb = 0;
  unsigned char *buf = read_whole(path, &len);
  mpeg_decoder *d;
  int rc;
  if (!buf) {
    printf("mpeg: cannot read %s\n", path);
    return 1;
  }
  rc = mpeg_decoder_open(buf, len, &d);
  if (rc != MPEG_OK) {
    printf("mpeg: not an MPEG-1 video stream (%d)\n", rc);
    free(buf);
    return 1;
  }
  printf("%s: MPEG-1 video %dx%d\n", path, mpeg_width(d), mpeg_height(d));
  while (mpeg_decode_frame(d) == MPEG_OK) {
    int t = mpeg_frame_type(d);
    nf++;
    if (t == MPEG_PIC_I)
      ni++;
    else if (t == MPEG_PIC_P)
      np++;
    else if (t == MPEG_PIC_B)
      nb++;
  }
  printf("  %d frames  (I:%d P:%d B:%d)\n", nf, ni, np, nb);
  mpeg_decoder_close(d);
  free(buf);
  return 0;
}

/* One synthetic RGB frame: a diagonal colour gradient scrolled by `t` pixels,
 * so consecutive frames differ by translation (real work for the P-frame path).
 */
static void gen_frame(unsigned char *rgb, int w, int h, int t)
{
  int x, y;
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++) {
      unsigned char *p = rgb + (y * w + x) * 3;
      int u = x + t, v = y + t;
      p[0] = (unsigned char)((u * 3) & 0xFF);
      p[1] = (unsigned char)((v * 5) & 0xFF);
      p[2] = (unsigned char)(((u + v) * 2) & 0xFF);
    }
}

static int cmd_gen(const char *out, int frames)
{
  int w = 176, h = 144, i, rc;
  unsigned char *rgb;
  unsigned char *stream = 0;
  unsigned slen = 0;
  mpeg_encoder *e;
  if (frames < 1)
    frames = 12;
  rgb = malloc((unsigned)w * h * 3);
  if (!rgb) {
    printf("mpeg: out of memory\n");
    return 1;
  }
  if (mpeg_encoder_open(w, h, 25, 1, 4, 12, &e) != MPEG_OK) {
    printf("mpeg: encoder init failed\n");
    free(rgb);
    return 1;
  }
  for (i = 0; i < frames; i++) {
    gen_frame(rgb, w, h, i * 2);
    if ((rc = mpeg_encode_frame_rgb(e, rgb)) != MPEG_OK) {
      printf("mpeg: encode failed (%d)\n", rc);
      free(rgb);
      mpeg_encoder_close(e);
      return 1;
    }
  }
  if (mpeg_encoder_finish(e, &stream, &slen) != MPEG_OK) {
    printf("mpeg: finish failed\n");
    free(rgb);
    mpeg_encoder_close(e);
    return 1;
  }
  mpeg_encoder_close(e);
  free(rgb);
  if (writefile(out, stream, (int)slen) != 0) {
    printf("mpeg: cannot write %s\n", out);
    free(stream);
    return 1;
  }
  printf("wrote %s: %d frames, %u bytes\n", out, frames, slen);
  free(stream);
  return 0;
}

static int parse_int(const char *s)
{
  int v = 0;
  while (*s >= '0' && *s <= '9')
    v = v * 10 + (*s++ - '0');
  return v;
}

int main(int argc, char **argv)
{
  if (argc >= 3 && streq(argv[1], "info"))
    return cmd_info(argv[2]);
  if (argc >= 3 && streq(argv[1], "gen"))
    return cmd_gen(argv[2], argc >= 4 ? parse_int(argv[3]) : 0);
  printf("usage: mpeg info <file.mpg>\n       mpeg gen <out.mpg> [frames]\n");
  return 1;
}
