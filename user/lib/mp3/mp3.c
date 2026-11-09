/* mp3.c — streaming MPEG audio decoder for /bin/play.
 *
 * The decode core is minimp3 by Lion (lieff), public domain / CC0, vendored at
 * minimp3.h and adapted for OwOS (see the patch list at the top of that file).
 * This file is the OwOS part: a small pull-style streaming API (mp3.h) over the
 * minimp3 frame decoder, plus the few libc routines it needs.
 *
 * minimp3 is freestanding: no libm (it ships precomputed gain/window tables),
 * no malloc, no 64-bit division — exactly what the OwOS user libc can link
 * (bare ld, no libgcc). Its only libc dependency is memcpy/memset/memmove,
 * resolved by the OwOS user libc (lib.c) for both the explicit calls and the
 * compiler's struct-copy lowering. SIMD is compiled out (MINIMP3_NO_SIMD).
 */

#include <stddef.h>
#include <stdint.h>

#include "mp3.h"  /* mp3_t + minimp3 declarations            */
#include "owos.h" /* readfile_at */

/* ---- the decoder core (scalar, no SIMD) -------------------------------- */
#define MINIMP3_NO_SIMD
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

static void mp3_fill(mp3_t *m)
{
  while (!m->src_eof && m->in_len < (int)sizeof m->in) {
    unsigned want = (unsigned)((int)sizeof m->in - m->in_len);
    if (m->file_off >= m->data_end || m->data_end - m->file_off < want)
      want = m->file_off < m->data_end ? m->data_end - m->file_off : 0;
    int n = want ? readfile_at(m->fn, m->file_off, m->in + m->in_len, want) : 0;
    if (n <= 0) {
      m->src_eof = 1;
      break;
    }
    m->in_len += n;
    m->file_off += (unsigned)n;
  }
}

/* Decode one frame into m->pcm. Returns frames produced (0 at end of stream).
 * Refills the input window from the file as needed and skips junk/ID3 tags
 * (minimp3 reports those as frames consumed with zero samples). */
static int mp3_decode_one(mp3_t *m)
{
  for (;;) {
    mp3_fill(m);
    if (m->in_len <= 0)
      return 0;

    mp3dec_frame_info_t info = {0};
    int samples = mp3dec_decode_frame(&m->dec, m->in, m->in_len, (mp3d_sample_t *)m->pcm, &info);

    if (info.channels > 0)
      m->fidx++;                /* a real frame, even if it yielded no samples (bit reservoir not yet filled) */
    if (info.frame_bytes > 0) { /* consume what was used */
      int rem = m->in_len - info.frame_bytes;
      if (rem > 0)
        memmove(m->in, m->in + info.frame_bytes, (unsigned)rem);
      m->in_len = rem > 0 ? rem : 0;
    } else if (m->src_eof) {
      return 0; /* no frame and no more data */
    } else if (m->in_len >= (int)sizeof m->in) {
      /* Full window, no frame: skip to the next 0xFF (every frame sync starts
       * with one) so garbage resync stays ~linear. */
      int skip = 1;
      while (skip < m->in_len && m->in[skip] != 0xFF)
        skip++;
      memmove(m->in, m->in + skip, (unsigned)(m->in_len - skip));
      m->in_len -= skip;
    }

    if (samples > 0) {
      if (m->warm > 0) {
        m->warm--;
        continue;
      }
      if ((unsigned)info.hz != m->rate) {
        m->exact = 0; /* a rate change invalidates the header's length */
        m->total = m->limit = 0;
      }
      m->rate = (unsigned)info.hz;
      m->channels = (unsigned)info.channels;
      m->out_ch = info.channels;
      m->out_frames = samples;
      m->out_pos = 0;
      return samples;
    }
    /* samples == 0: skipped a tag or a bad frame, keep going */
  }
}

static unsigned rd_be32(const unsigned char *p) { return (unsigned)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* Read a Xing/Info (+LAME) or VBRI header out of the first frame `f` (`fb`
 * bytes). Returns 1 if the frame is a metadata frame and must not be played. */
static int mp3_parse_vbr_header(mp3_t *m, const unsigned char *f, int fb, unsigned *delay, unsigned *pad)
{
  int mpeg1 = (f[1] >> 3 & 3) == 3;
  int mono = (f[3] >> 6) == 3;
  int side = mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
  const unsigned char *end = f + fb;
  const unsigned char *x = f + 4 + ((f[1] & 1) ? 0 : 2) + side;

  if (x + 8 <= end && (!memcmp(x, "Xing", 4) || !memcmp(x, "Info", 4))) {
    unsigned flags = rd_be32(x + 4);
    const unsigned char *p = x + 8;
    if ((flags & 1) && p + 4 <= end) {
      m->nframes = rd_be32(p);
      m->exact = m->nframes > 0;
      p += 4;
    }
    if ((flags & 2) && p + 4 <= end) {
      m->toc_bytes = rd_be32(p);
      p += 4;
    }
    if ((flags & 4) && p + 100 <= end) {
      memcpy(m->toc_tab, p, 100);
      m->toc = 1;
      p += 100;
    }
    if (flags & 8)
      p += 4;
    if (p + 24 <= end && (!memcmp(p, "LAME", 4) || !memcmp(p, "Lavf", 4) || !memcmp(p, "Lavc", 4))) {
      unsigned v = (unsigned)p[21] << 16 | p[22] << 8 | p[23];
      *delay = v >> 12;
      *pad = v & 0xfff;
    }
    return 1;
  }
  x = f + 4 + 32;
  if (x + 18 <= end && !memcmp(x, "VBRI", 4)) {
    m->toc_bytes = rd_be32(x + 10);
    m->nframes = rd_be32(x + 14);
    m->exact = m->nframes > 0;
    return 1;
  }
  return 0;
}

static unsigned mp3_frame_bytes(const mp3_t *m, int kbps)
{
  unsigned fb = m->spf * (unsigned)kbps * 125 / (m->rate ? m->rate : 1);
  return fb ? fb : 1;
}

int mp3_open(mp3_t *m, const char *fn, unsigned size)
{
  unsigned char h[10];
  unsigned off = 0;

  memset(m, 0, sizeof *m);
  m->fn = fn;
  m->size = size;
  while (off + 10 <= size && readfile_at(fn, off, h, 10) == 10 && !memcmp(h, "ID3", 3) && h[3] != 0xFF)
    off += 10 + ((unsigned)h[6] << 21 | h[7] << 14 | h[8] << 7 | h[9]) + ((h[5] & 0x10) ? 10 : 0);
  if (off > size)
    off = size;
  m->audio0 = m->first = off;
  m->data_end = size;
  if (size >= off + 128 && readfile_at(fn, size - 128, h, 3) == 3 && !memcmp(h, "TAG", 3))
    m->data_end = size - 128;

  mp3dec_init(&m->dec);
  m->file_off = off;
  mp3_fill(m);
  mp3dec_frame_info_t info;
  int samples = mp3dec_decode_frame(&m->dec, m->in, m->in_len, (mp3d_sample_t *)m->pcm, &info);
  if (samples <= 0 || info.frame_bytes <= 0) {
    /* Junk before the first frame: no header metadata, let the decoder resync. */
    mp3dec_init(&m->dec);
    m->in_len = m->src_eof = 0;
    m->file_off = off;
    m->spf = 1152;
    return mp3_decode_one(m) > 0 ? 0 : -1;
  }

  unsigned delay = 0, pad = 0;
  m->spf = (unsigned)samples;
  m->rate = (unsigned)info.hz;
  m->channels = (unsigned)info.channels;
  m->first = off + (unsigned)info.frame_offset;
  m->audio0 = m->first;
  if (mp3_parse_vbr_header(m, m->in + info.frame_offset, info.frame_bytes, &delay, &pad))
    m->audio0 = m->first + (unsigned)info.frame_bytes;
  if (m->audio0 > m->data_end)
    m->audio0 = m->data_end;

  unsigned span = m->data_end - m->audio0;
  if (!m->exact)
    m->nframes = span / mp3_frame_bytes(m, info.bitrate_kbps);
  if (m->toc && !m->toc_bytes)
    m->toc_bytes = m->data_end - m->first;
  if (m->exact && (delay || pad)) {
    m->start_skip = delay + 529;
    pad = pad > 529 ? pad - 529 : 0;
  } else {
    pad = 0;
  }
  m->total = m->nframes * m->spf;
  m->total = m->total > m->start_skip + pad ? m->total - m->start_skip - pad : 0;
  if (m->exact)
    m->limit = m->total;

  mp3dec_init(&m->dec);
  m->in_len = m->src_eof = 0;
  m->file_off = m->audio0;
  m->skip = m->start_skip;
  m->out_frames = m->out_pos = 0;
  m->out_ch = info.channels;
  return 0;
}

unsigned mp3_total_frames(const mp3_t *m) { return m->total; }
int mp3_total_exact(const mp3_t *m) { return m->exact; }

unsigned mp3_seek(mp3_t *m, unsigned target)
{
  unsigned fb = m->nframes ? (m->data_end - m->audio0) / m->nframes : 0;
  int warm = 4 + (fb ? 511 / (int)fb : 0);
  if (warm > 24)
    warm = 24;
  unsigned fi = (target + m->start_skip) / m->spf;
  unsigned land = fi > (unsigned)warm ? fi - (unsigned)warm : 0;
  unsigned off = m->audio0;

  if (!m->nframes)
    land = 0;
  else if (land >= m->nframes)
    land = m->nframes - 1;
  if (land > 0 && m->nframes) {
    if (m->toc) { /* interpolate between the two surrounding table entries */
      unsigned long long n = m->nframes, x = (unsigned long long)land * 100;
      unsigned pct = (unsigned)(x / n);
      unsigned lo = m->toc_tab[pct], hi = pct < 99 ? m->toc_tab[pct + 1] : 256;
      unsigned long long b256 = (lo * n + (unsigned long long)(hi - lo) * (x - pct * n)) / n;
      off = m->first + (unsigned)(b256 * m->toc_bytes / 256);
    } else {
      off = m->audio0 + (unsigned)((unsigned long long)land * (m->data_end - m->audio0) / m->nframes);
    }
  }
  if (off < m->audio0)
    off = m->audio0;
  if (off > m->data_end)
    off = m->data_end;

  mp3dec_init(&m->dec);
  m->file_off = off;
  m->in_len = m->src_eof = 0;
  m->out_pos = m->out_frames = 0;
  m->fidx = land;
  m->skip = 0;
  m->warm = 0;
  if (land == 0) {
    m->skip = m->start_skip + target;
    m->pos = target;
    return target;
  }
  m->warm = 2; /* refill the MDCT overlap and filterbank history */
  for (;;) {
    if (mp3_decode_one(m) <= 0) {
      m->pos = m->total;
      return m->pos;
    }
    unsigned at = (m->fidx - 1) * m->spf;
    m->pos = at > m->start_skip ? at - m->start_skip : 0;
    if (m->pos + (unsigned)m->out_frames > target)
      break;
  }
  if (m->pos < target) {
    m->skip = target - m->pos;
    m->pos = target;
  }
  return m->pos;
}

unsigned mp3_rate(const mp3_t *m) { return m->rate; }
unsigned mp3_channels(const mp3_t *m) { return m->channels; }

int mp3_read(mp3_t *m, short *out, int maxframes)
{
  int done = 0;
  while (done < maxframes) {
    if (m->limit && m->pos >= m->limit)
      break;
    if (m->out_pos >= m->out_frames) {
      if (mp3_decode_one(m) <= 0)
        break; /* end of stream */
    }
    int avail = m->out_frames - m->out_pos;
    if (m->skip) {
      unsigned k = m->skip < (unsigned)avail ? m->skip : (unsigned)avail;
      m->out_pos += (int)k;
      m->skip -= k;
      continue;
    }
    int n = maxframes - done;
    if (n > avail)
      n = avail;
    if (m->limit && (unsigned)n > m->limit - m->pos)
      n = (int)(m->limit - m->pos);

    if (m->out_ch == 2) {
      const short *src = m->pcm + m->out_pos * 2;
      for (int i = 0; i < n; i++) {
        out[(done + i) * 2 + 0] = src[i * 2 + 0];
        out[(done + i) * 2 + 1] = src[i * 2 + 1];
      }
    } else { /* mono -> duplicate */
      const short *src = m->pcm + m->out_pos;
      for (int i = 0; i < n; i++) {
        short s = src[i];
        out[(done + i) * 2 + 0] = s;
        out[(done + i) * 2 + 1] = s;
      }
    }
    m->out_pos += n;
    m->pos += (unsigned)n;
    done += n;
  }
  return done;
}

void mp3_close(mp3_t *m) { (void)m; }
