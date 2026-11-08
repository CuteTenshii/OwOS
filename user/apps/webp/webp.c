/* webp.c, a small WebP command-line tool */
#include "webp.h"
#include "owos.h"
#include "png.h"
#include <stdbool.h>

/* a buffered file reader over readfile_at (for the streaming PNG decoder) */
#define FR_BUF 65536
static unsigned char fr_buffer[FR_BUF];
struct freader {
  const char *name;
  unsigned off, len, pos;
  bool eof;
};

static void fr_init(struct freader *f, const char *name)
{
  f->name = name;
  f->off = 0;
  f->len = 0;
  f->pos = 0;
  f->eof = false;
}

static int fr_get(void *c)
{
  struct freader *f = c;
  if (f->pos >= f->len) {
    if (f->eof)
      return -1;
    int n = readfile_at(f->name, f->off, fr_buffer, FR_BUF);
    if (n <= 0) {
      f->eof = true;
      return -1;
    }
    f->off += (unsigned)n;
    f->len = (unsigned)n;
    f->pos = 0;
  }
  return fr_buffer[f->pos++];
}

/* read a whole file into a malloc'd buffer */
static unsigned char *read_whole(const char *path, int *out_len)
{
  struct statinfo st;
  if (stat(path, &st) != 0 || st.size == 0)
    return 0;
  unsigned char *buf = malloc(st.size);
  if (!buf)
    return 0;
  unsigned off = 0;
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
  *out_len = (int)st.size;
  return buf;
}

/* decode a PNG file into a fresh RGBA buffer */
struct pngdst {
  unsigned char *buf;
  unsigned w, h;
};
static int webp_png_row(void *c, unsigned y, const unsigned char *rgba, unsigned w)
{
  struct pngdst *d = c;
  if (y < d->h) {
    unsigned n = (w < d->w ? w : d->w) * 4u;
    memcpy(d->buf + (unsigned long)y * d->w * 4u, rgba, n);
  }
  return 0;
}

static unsigned char *decode_png(const char *path, unsigned *w, unsigned *h)
{
  struct freader fr;
  fr_init(&fr, path);
  unsigned pw = 0, ph = 0;
  if (png_dimensions(fr_get, &fr, &pw, &ph) != 0 || !pw || !ph)
    return 0;
  unsigned char *buf = malloc(pw * ph * 4u);
  if (!buf)
    return 0;
  memset(buf, 0, pw * ph * 4u);
  fr_init(&fr, path);
  struct pngdst d = {buf, pw, ph};
  if (png_decode(fr_get, &fr, webp_png_row, &d, 0, 0) != PNG_OK) {
    free(buf);
    return 0;
  }
  *w = pw;
  *h = ph;
  return buf;
}

static int cmd_info(const char *path)
{
  int len = 0;
  unsigned char *buf = read_whole(path, &len);
  if (!buf) {
    printf("webp: cannot read %s\n", path);
    return 1;
  }
  struct webp_info info;
  int rc = webp_probe(buf, len, &info);
  free(buf);
  if (rc != WEBP_OK) {
    printf("webp: not a WebP file (%d)\n", rc);
    return 1;
  }
  printf("%s\n", path);
  printf("  dimensions : %u x %u\n", info.w, info.h);
  printf("  format     : %s\n", info.lossless ? "lossless (VP8L)" : "lossy (VP8)");
  printf("  alpha      : %s\n", info.has_alpha ? "yes" : "no");
  printf("  container  : %s\n", info.extended ? "extended (VP8X)" : "simple");
  if (info.animation)
    printf("  animation  : yes\n");
  return 0;
}

static int cmd_encode(const char *in, const char *out)
{
  unsigned w = 0, h = 0;
  unsigned char *rgba = decode_png(in, &w, &h);
  if (!rgba) {
    printf("webp: cannot decode PNG %s\n", in);
    return 1;
  }
  unsigned char *wb = 0;
  int wl = 0;
  int rc = webp_encode(rgba, w, h, &wb, &wl);
  free(rgba);
  if (rc != WEBP_OK) {
    printf("webp: encode failed (%d)\n", rc);
    return 1;
  }
  if (writefile(out, wb, wl) != 0) {
    printf("webp: cannot write %s\n", out);
    free(wb);
    return 1;
  }
  free(wb);
  printf("wrote %s (%u x %u, %d bytes)\n", out, w, h, wl);
  return 0;
}

static int cmd_verify(const char *in)
{
  unsigned w = 0, h = 0;
  unsigned char *rgba = decode_png(in, &w, &h);
  if (!rgba) {
    printf("webp: cannot decode PNG %s\n", in);
    return 1;
  }

  unsigned char *wb = 0;
  int wl = 0;
  if (webp_encode(rgba, w, h, &wb, &wl) != WEBP_OK) {
    printf("webp: encode failed\n");
    free(rgba);
    return 1;
  }

  unsigned char *back = 0;
  unsigned dw = 0, dh = 0;
  int rc = webp_decode(wb, wl, &back, &dw, &dh);
  free(wb);
  if (rc != WEBP_OK) {
    printf("webp: decode failed (%d)\n", rc);
    free(rgba);
    return 1;
  }

  bool ok = (dw == w && dh == h);
  for (unsigned i = 0; ok && i < w * h * 4u; i++)
    if (rgba[i] != back[i])
      ok = false;
  free(rgba);
  free(back);

  unsigned raw = w * h * 4u;
  printf("%s: %u x %u  raw=%u webp=%d  -> %s\n", in, w, h, raw, wl, ok ? "lossless OK" : "MISMATCH");
  return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
  if (argc >= 3 && argv[1][0] == 'i') /* info <file> */
    return cmd_info(argv[2]);
  if (argc >= 4 && argv[1][0] == 'e') /* encode <in.png> <out> */
    return cmd_encode(argv[2], argv[3]);
  if (argc >= 3 && argv[1][0] == 'v') /* verify <in.png> */
    return cmd_verify(argv[2]);

  puts("usage:\n");
  puts("  webp info   <file.webp>          show dimensions + format\n");
  puts("  webp encode <in.png> <out.webp>  PNG -> lossless WebP\n");
  puts("  webp verify <in.png>             confirm a lossless round trip\n");
  return 1;
}
