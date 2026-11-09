/* oggplay.c, streaming Ogg Vorbis file reader (see oggplay.h). */
#include "oggplay.h"
#include "ogg.h"
#include "owos.h"
#include <stdint.h>

#define CHUNK 16384

struct oggplay {
  const char *path;
  unsigned size, off; /* file size; next byte to feed the demuxer */
  uint32_t serial;
  unsigned rate, channels, total;
  unsigned char *id, *setup;
  unsigned id_len, setup_len;
  ogg_demux *dm;
  vorbis_decoder *vd;
  short *pcm;
  int have, bufpos;
  int pos;       /* granule index of the next frame to emit or skip */
  unsigned skip; /* frames still to drop (stream start trim, seek) */
  unsigned trim; /* leading frames the first page's granule position cuts off */
  int drop_first;
  unsigned char rbuf[CHUNK];
};

typedef struct {
  unsigned off, len;
  int64_t gp;
  unsigned char htype;
} page_t;

static int64_t rd_i64(const unsigned char *p)
{
  uint64_t v = 0;
  for (int i = 7; i >= 0; i--)
    v = v << 8 | p[i];
  return (int64_t)v;
}

/* First page of this stream starting at or after `from`. A capture pattern
 * inside payload is rejected by requiring the next page to follow directly. */
static int find_page(const oggplay *o, unsigned from, page_t *pg)
{
  unsigned char b[512];
  while (from + 27 <= o->size) {
    int n = readfile_at(o->path, from, b, sizeof b);
    if (n < 27)
      return 0;
    for (int i = 0; i + 27 <= n; i++) {
      if (b[i] != 'O' || b[i + 1] != 'g' || b[i + 2] != 'g' || b[i + 3] != 'S' || b[i + 4] != 0)
        continue;
      unsigned char h[27 + 255];
      if (readfile_at(o->path, from + i, h, 27) < 27)
        return 0;
      unsigned nseg = h[26], body = 0;
      if (readfile_at(o->path, from + i + 27, h + 27, nseg) < (int)nseg)
        return 0;
      for (unsigned s = 0; s < nseg; s++)
        body += h[27 + s];
      unsigned len = 27 + nseg + body, next = from + i + len;
      unsigned char t[4];
      if (next > o->size || (next < o->size && (readfile_at(o->path, next, t, 4) < 4 || memcmp(t, "OggS", 4))))
        continue;
      uint32_t ser = h[14] | h[15] << 8 | h[16] << 16 | (uint32_t)h[17] << 24;
      if (ser != o->serial)
        continue;
      pg->off = from + i;
      pg->len = len;
      pg->gp = rd_i64(h + 6);
      pg->htype = h[5];
      return 1;
    }
    from += n > 27 ? (unsigned)n - 27 : 1;
  }
  return 0;
}

static int restart(oggplay *o, unsigned off)
{
  if (o->dm)
    ogg_demux_free(o->dm);
  if (o->vd)
    vorbis_decoder_close(o->vd);
  vorbis_info info;
  o->dm = ogg_demux_new();
  o->vd = vorbis_decoder_open(o->id, o->id_len, o->setup, o->setup_len, &info);
  o->off = off;
  o->have = o->bufpos = 0;
  o->skip = 0;
  o->drop_first = 0;
  return o->dm && o->vd;
}

/* Back to the start of the stream, dropping the leading frames the first page's
 * granule position says are not part of the audio. */
static int restart_start(oggplay *o)
{
  if (!restart(o, 0))
    return 0;
  o->pos = -(int)o->trim;
  o->skip = o->trim;
  return 1;
}

static int refill(oggplay *o)
{
  for (;;) {
    const unsigned char *p;
    unsigned pl;
    uint32_t ser;
    int r = ogg_demux_packet(o->dm, &p, &pl, NULL, &ser, NULL, NULL);
    if (r == OGG_NEED) {
      int n = readfile_at(o->path, o->off, o->rbuf, CHUNK);
      if (n <= 0 || ogg_demux_feed(o->dm, o->rbuf, (unsigned)n) != OGG_OK)
        return 0;
      o->off += (unsigned)n;
      continue;
    }
    if (r != OGG_OK)
      return 0;
    if (ser != o->serial)
      continue;
    if (o->drop_first) { /* tail of a packet that began before the seek point */
      o->drop_first = 0;
      continue;
    }
    int n = vorbis_decode(o->vd, p, pl, o->pcm);
    if (n > 0) {
      o->have = n;
      o->bufpos = 0;
      return 1;
    }
  }
}

oggplay *oggplay_open(const char *path, unsigned size)
{
  oggplay *o = malloc(sizeof *o);
  if (!o)
    return NULL;
  memset(o, 0, sizeof *o);
  o->path = path;
  o->size = size;

  ogg_demux *dm = ogg_demux_new();
  unsigned char *hdr[3] = {0, 0, 0};
  unsigned hlen[3] = {0, 0, 0};
  int nh = 0, have_ser = 0;
  while (dm && nh < 3 && o->off < size && o->off < (1u << 20)) {
    int n = readfile_at(path, o->off, o->rbuf, CHUNK);
    if (n <= 0 || ogg_demux_feed(dm, o->rbuf, (unsigned)n) != OGG_OK)
      break;
    o->off += (unsigned)n;
    const unsigned char *p;
    unsigned pl;
    uint32_t ser;
    while (nh < 3 && ogg_demux_packet(dm, &p, &pl, NULL, &ser, NULL, NULL) == OGG_OK) {
      if (!have_ser) {
        if (pl < 7 || p[0] != 1 || memcmp(p + 1, "vorbis", 6))
          goto fail;
        o->serial = ser;
        have_ser = 1;
      }
      if (ser != o->serial)
        continue;
      if (pl < 7 || p[0] != (unsigned char)(2 * nh + 1) || memcmp(p + 1, "vorbis", 6))
        goto fail;
      hdr[nh] = malloc(pl);
      if (!hdr[nh])
        goto fail;
      memcpy(hdr[nh], p, pl);
      hlen[nh++] = pl;
    }
  }
  ogg_demux_free(dm);
  dm = NULL;
  if (nh < 3)
    goto fail;

  o->id = hdr[0];
  o->id_len = hlen[0];
  o->setup = hdr[2];
  o->setup_len = hlen[2];
  free(hdr[1]);
  hdr[0] = hdr[1] = hdr[2] = NULL;

  vorbis_info info;
  o->vd = vorbis_decoder_open(o->id, o->id_len, o->setup, o->setup_len, &info);
  if (!o->vd || info.channels < 1 || info.channels > 2 || info.sample_rate == 0)
    goto fail;
  o->rate = info.sample_rate;
  o->channels = info.channels;
  o->pcm = malloc((unsigned long)vorbis_max_frames(o->vd) * info.channels * sizeof(short));
  if (!o->pcm)
    goto fail;

  /* Length: granule position of the last page of the stream. */
  page_t pg, last = {0, 0, -1, 0};
  unsigned from = size > 65536 ? size - 65536 : 0;
  while (find_page(o, from, &pg)) {
    if (pg.gp >= 0)
      last = pg;
    from = pg.off + pg.len;
  }
  o->total = last.gp > 0 && last.gp < 0x7fffffff ? (unsigned)last.gp : 0;

  /* Decode up to the first page that carries a granule position to learn the trim. */
  if (!restart(o, 0))
    goto fail;
  for (int got = 0;;) {
    const unsigned char *p;
    unsigned pl;
    int64_t gp;
    uint32_t ser;
    int r = ogg_demux_packet(o->dm, &p, &pl, &gp, &ser, NULL, NULL);
    if (r == OGG_NEED) {
      int n = readfile_at(path, o->off, o->rbuf, CHUNK);
      if (n <= 0 || ogg_demux_feed(o->dm, o->rbuf, (unsigned)n) != OGG_OK)
        break;
      o->off += (unsigned)n;
      continue;
    }
    if (r != OGG_OK)
      break;
    if (ser != o->serial || (pl && (p[0] & 1)))
      continue;
    int n = vorbis_decode(o->vd, p, pl, o->pcm);
    got += n > 0 ? n : 0;
    if (gp >= 0) {
      o->trim = got > gp ? (unsigned)(got - gp) : 0;
      break;
    }
  }
  if (!restart_start(o))
    goto fail;
  return o;

fail:
  if (dm)
    ogg_demux_free(dm);
  for (int i = 0; i < 3; i++)
    free(hdr[i]);
  oggplay_close(o);
  return NULL;
}

void oggplay_close(oggplay *o)
{
  if (!o)
    return;
  if (o->dm)
    ogg_demux_free(o->dm);
  if (o->vd)
    vorbis_decoder_close(o->vd);
  free(o->id);
  free(o->setup);
  free(o->pcm);
  free(o);
}

unsigned oggplay_rate(const oggplay *o) { return o->rate; }
unsigned oggplay_total_frames(const oggplay *o) { return o->total; }

int oggplay_read(oggplay *o, short *out, int maxframes)
{
  int done = 0;
  while (done < maxframes) {
    if (o->total && o->pos >= (int)o->total)
      break;
    if (o->bufpos >= o->have && !refill(o))
      break;
    int avail = o->have - o->bufpos;
    if (o->skip) {
      unsigned k = o->skip < (unsigned)avail ? o->skip : (unsigned)avail;
      o->bufpos += (int)k;
      o->skip -= k;
      o->pos += (int)k;
      continue;
    }
    int n = maxframes - done;
    if (n > avail)
      n = avail;
    if (o->total && n > (int)o->total - o->pos)
      n = (int)o->total - o->pos;
    const short *src = o->pcm + o->bufpos * o->channels;
    for (int i = 0; i < n; i++) {
      out[(done + i) * 2] = src[i * o->channels];
      out[(done + i) * 2 + 1] = src[i * o->channels + (o->channels - 1)];
    }
    o->bufpos += n;
    o->pos += n;
    done += n;
  }
  return done;
}

unsigned oggplay_seek(oggplay *o, unsigned target)
{
  unsigned lo = 0, hi = o->size, iters = 0;
  page_t best = {0, 0, 0, 0}, pg;
  int found = 0;

  while (o->total && target && hi - lo > 8192 && iters++ < 32) {
    unsigned mid = lo + (hi - lo) / 2;
    if (!find_page(o, mid, &pg) || pg.gp > (int64_t)target) {
      hi = mid;
    } else {
      if (pg.gp >= 0) {
        best = pg;
        found = 1;
      }
      lo = pg.off + pg.len;
    }
  }

  page_t next;
  if (found) {
    int more = find_page(o, best.off + best.len, &next);
    restart(o, more ? next.off : o->size);
    o->drop_first = more && (next.htype & OGG_F_CONTINUED);
    o->pos = (int)best.gp;
    o->skip = (int)target > o->pos ? target - (unsigned)o->pos : 0;
  } else {
    restart_start(o);
    o->skip += target;
  }
  return target;
}
