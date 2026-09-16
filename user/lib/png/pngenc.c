/* pngenc.c — XRGB framebuffer -> PNG (uncompressed), user space. See pngenc.h.
 *
 * The IDAT carries the raw filtered scanlines (filter 0 per row) wrapped in a
 * zlib stream of stored DEFLATE blocks with an adler32 trailer; each PNG chunk
 * is length + type + data + CRC-32. A port of the kernel encoder (was
 * kernel/png.c), now sharing the zlib CRC-32 and building the whole image in
 * one malloc'd buffer (user space has heap to spare) instead of streaming to
 * disk. */

#include "pngenc.h"
#include "crc32.h"
#include "owos.h"

static const unsigned char PNG_SIG[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

static void be32(unsigned char *p, unsigned v)
{
  p[0] = v >> 24;
  p[1] = v >> 16;
  p[2] = v >> 8;
  p[3] = v;
}

int png_write(const char *path, const unsigned *pixels, unsigned w, unsigned h, unsigned pitch)
{
  if (!w || !h || w > 0xFFFF || h > 0xFFFF || !pixels)
    return -1;

  unsigned rowbytes = 1 + w * 3; /* filter byte + RGB          */
  unsigned raw_size = h * rowbytes;
  unsigned nblk = (raw_size + 65534) / 65535;
  unsigned idat_len = 2 + nblk * 5 + raw_size + 4; /* zlib hdr + blocks + adler */
  unsigned total = 8 + (12 + 13) + (12 + idat_len) + (12 + 0);

  unsigned char *out = malloc(total);
  unsigned char *rowbuf = malloc(rowbytes);
  if (!out || !rowbuf) {
    free(out);
    free(rowbuf);
    return -1;
  }

  unsigned o = 0;
  memcpy(out + o, PNG_SIG, 8);
  o += 8;

  /* IHDR: width, height, 8-bit depth, colour type 2 (truecolour RGB). */
  be32(out + o, 13);
  o += 4;
  unsigned ihdr_type = o;
  out[o++] = 'I';
  out[o++] = 'H';
  out[o++] = 'D';
  out[o++] = 'R';
  be32(out + o, w);
  o += 4;
  be32(out + o, h);
  o += 4;
  out[o++] = 8;
  out[o++] = 2;
  out[o++] = 0;
  out[o++] = 0;
  out[o++] = 0;
  be32(out + o, crc32(0, out + ihdr_type, 4 + 13));
  o += 4;

  /* IDAT: length + type, then the zlib stream built inline. */
  be32(out + o, idat_len);
  o += 4;
  unsigned idat_type = o;
  out[o++] = 'I';
  out[o++] = 'D';
  out[o++] = 'A';
  out[o++] = 'T';
  out[o++] = 0x78;
  out[o++] = 0x01; /* zlib header                */

  unsigned a = 1, b = 0; /* adler32 over raw bytes     */
  unsigned raw_left = raw_size, blk_left = 0;
  for (unsigned y = 0; y < h; y++) {
    rowbuf[0] = 0; /* filter: None                */
    const unsigned *row = pixels + (unsigned)y * pitch;
    unsigned char *q = rowbuf + 1;
    for (unsigned x = 0; x < w; x++) {
      unsigned px = row[x];
      *q++ = (unsigned char)(px >> 16);
      *q++ = (unsigned char)(px >> 8);
      *q++ = (unsigned char)px;
    }
    unsigned done = 0;
    while (done < rowbytes) {
      if (blk_left == 0) { /* open a stored block         */
        unsigned chunk = raw_left > 65535 ? 65535 : raw_left;
        out[o++] = (unsigned char)(chunk == raw_left); /* BFINAL */
        out[o++] = (unsigned char)chunk;
        out[o++] = (unsigned char)(chunk >> 8);
        out[o++] = (unsigned char)~chunk;
        out[o++] = (unsigned char)(~chunk >> 8);
        blk_left = chunk;
      }
      unsigned avail = rowbytes - done;
      unsigned k = avail < blk_left ? avail : blk_left;
      memcpy(out + o, rowbuf + done, k);
      o += k;
      for (unsigned i = 0; i < k; i++) {
        a += rowbuf[done + i];
        if (a >= 65521)
          a -= 65521;
        b += a;
        if (b >= 65521)
          b -= 65521;
      }
      done += k;
      blk_left -= k;
      raw_left -= k;
    }
  }
  out[o++] = (unsigned char)(b >> 8);
  out[o++] = (unsigned char)b; /* adler32 */
  out[o++] = (unsigned char)(a >> 8);
  out[o++] = (unsigned char)a;
  be32(out + o, crc32(0, out + idat_type, 4 + idat_len));
  o += 4;

  /* IEND. */
  be32(out + o, 0);
  o += 4;
  unsigned iend_type = o;
  out[o++] = 'I';
  out[o++] = 'E';
  out[o++] = 'N';
  out[o++] = 'D';
  be32(out + o, crc32(0, out + iend_type, 4));
  o += 4;

  int r = writefile(path, out, (int)o);
  free(out);
  free(rowbuf);
  return r;
}
