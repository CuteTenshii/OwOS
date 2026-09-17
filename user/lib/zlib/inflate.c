/* inflate.c — streaming DEFLATE (RFC 1951) + gzip/zlib framing, user space.
 *
 * Ported from the kernel's gzip.c inflate engine, with the two ends made
 * streaming: the bit reader pulls bytes through a reader callback instead of
 * indexing a fixed buffer, and output goes to a 32 KiB circular window (to
 * resolve back-references) while being flushed to a writer callback. Nothing
 * scales with the body size — only the window and a small flush buffer.
 */
#include "inflate.h"
#include "owos.h" /* malloc/free — per-call state, so decodes are reentrant */
#include <stdint.h>

#define MAXBITS 15
#define MAXLCODES 286
#define MAXDCODES 30
#define FIXLCODES 288
#define WINSZ 32768 /* DEFLATE max back-reference distance         */
#define WINMASK (WINSZ - 1)
#define OUTBUF 4096 /* flush granularity to the writer             */

static const uint16_t lbase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dbase[30] = {1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                   33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                   1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dext[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

struct state {
  inf_reader rd;
  void *rctx;
  inf_writer wr;
  void *wctx;
  uint32_t bitbuf;
  int bitcnt;
  int err;            /* malformed stream                              */
  int stop;           /* writer asked to stop early                    */
  uint32_t total;     /* total bytes produced (window write cursor)    */
  uint8_t win[WINSZ]; /* sliding window for back-references             */
  uint8_t ob[OUTBUF]; /* pending output, flushed to wr() in batches     */
  uint32_t obn;
};

struct huff {
  short count[MAXBITS + 1];
  short symbol[FIXLCODES]; /* must hold the 288 fixed codes, not just 286 */
};

/* ---- input bit reader (pull model) ------------------------------------- */

static int get_bit(struct state *s)
{
  if (s->bitcnt == 0) {
    int c = s->rd(s->rctx);
    if (c < 0) {
      s->err = 1;
      return 0;
    }
    s->bitbuf = (uint32_t)c;
    s->bitcnt = 8;
  }
  int b = s->bitbuf & 1;
  s->bitbuf >>= 1;
  s->bitcnt--;
  return b;
}

static uint32_t get_bits(struct state *s, int n)
{
  uint32_t v = 0;
  for (int i = 0; i < n; i++)
    v |= (uint32_t)get_bit(s) << i;
  return v;
}

/* ---- output (window + flushing writer) --------------------------------- */

static void flush(struct state *s)
{
  if (s->obn && !s->stop) {
    if (s->wr(s->wctx, s->ob, s->obn) < 0)
      s->stop = 1;
    s->obn = 0;
  }
}

static void emit(struct state *s, uint8_t b)
{
  s->win[s->total & WINMASK] = b;
  s->total++;
  s->ob[s->obn++] = b;
  if (s->obn >= OUTBUF)
    flush(s);
}

/* ---- Huffman ----------------------------------------------------------- */

static void huff_build(struct huff *h, const short *length, int n)
{
  short offs[MAXBITS + 1];
  for (int len = 0; len <= MAXBITS; len++)
    h->count[len] = 0;
  for (int sym = 0; sym < n; sym++)
    h->count[length[sym]]++;
  if (h->count[0] == n)
    return;
  offs[1] = 0;
  for (int len = 1; len < MAXBITS; len++)
    offs[len + 1] = offs[len] + h->count[len];
  for (int sym = 0; sym < n; sym++)
    if (length[sym] != 0)
      h->symbol[offs[length[sym]]++] = (short)sym;
}

static int huff_decode(struct state *s, const struct huff *h)
{
  int code = 0, first = 0, index = 0;
  for (int len = 1; len <= MAXBITS; len++) {
    code |= get_bit(s);
    if (s->err)
      return -1;
    int cnt = h->count[len];
    if (code - first < cnt)
      return h->symbol[index + (code - first)];
    index += cnt;
    first += cnt;
    first <<= 1;
    code <<= 1;
  }
  return -1;
}

/* ---- block decoders ---------------------------------------------------- */

static int block_huff(struct state *s, const struct huff *lit, const struct huff *dist)
{
  for (;;) {
    if (s->stop)
      return 0;
    int sym = huff_decode(s, lit);
    if (sym < 0)
      return -1;
    if (sym == 256)
      return 0; /* end of block                    */
    if (sym < 256) {
      emit(s, (uint8_t)sym);
    } else {
      sym -= 257;
      if (sym >= 29)
        return -1;
      uint32_t len = lbase[sym] + get_bits(s, lext[sym]);
      int dsym = huff_decode(s, dist);
      if (dsym < 0 || dsym >= 30)
        return -1;
      uint32_t d = dbase[dsym] + get_bits(s, dext[dsym]);
      if (d > s->total) /* reference before stream start   */
        return -1;
      for (uint32_t i = 0; i < len; i++)
        emit(s, s->win[(s->total - d) & WINMASK]);
    }
    if (s->err)
      return -1;
  }
}

static int block_stored(struct state *s)
{
  s->bitcnt = 0; /* align to a byte boundary        */
  int l0 = s->rd(s->rctx), l1 = s->rd(s->rctx);
  int n0 = s->rd(s->rctx), n1 = s->rd(s->rctx);
  if (l0 < 0 || l1 < 0 || n0 < 0 || n1 < 0)
    return -1;
  uint32_t len = (uint32_t)l0 | ((uint32_t)l1 << 8);
  if ((uint16_t) ~(l0 | (l1 << 8)) != (uint16_t)(n0 | (n1 << 8)))
    return -1; /* LEN / ~LEN mismatch             */
  for (uint32_t i = 0; i < len; i++) {
    int c = s->rd(s->rctx);
    if (c < 0)
      return -1;
    emit(s, (uint8_t)c);
    if (s->stop)
      return 0;
  }
  return 0;
}

static void fixed_tables(struct huff *lit, struct huff *dist)
{
  short lengths[FIXLCODES];
  int i = 0;
  for (; i < 144; i++)
    lengths[i] = 8;
  for (; i < 256; i++)
    lengths[i] = 9;
  for (; i < 280; i++)
    lengths[i] = 7;
  for (; i < 288; i++)
    lengths[i] = 8;
  huff_build(lit, lengths, FIXLCODES);
  for (i = 0; i < 30; i++)
    lengths[i] = 5;
  huff_build(dist, lengths, 30);
}

static int dynamic_tables(struct state *s, struct huff *lit, struct huff *dist)
{
  static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
  int hlit = (int)get_bits(s, 5) + 257;
  int hdist = (int)get_bits(s, 5) + 1;
  int hclen = (int)get_bits(s, 4) + 4;
  if (hlit > MAXLCODES || hdist > MAXDCODES)
    return -1;

  short cl[19];
  for (int i = 0; i < 19; i++)
    cl[i] = 0;
  for (int i = 0; i < hclen; i++)
    cl[order[i]] = (short)get_bits(s, 3);
  struct huff clh;
  huff_build(&clh, cl, 19);

  short lengths[MAXLCODES + MAXDCODES];
  int idx = 0;
  while (idx < hlit + hdist) {
    int sym = huff_decode(s, &clh);
    if (sym < 0)
      return -1;
    if (sym < 16) {
      lengths[idx++] = (short)sym;
    } else {
      int rep;
      short val = 0;
      if (sym == 16) {
        if (idx == 0)
          return -1;
        val = lengths[idx - 1];
        rep = 3 + (int)get_bits(s, 2);
      } else if (sym == 17) {
        rep = 3 + (int)get_bits(s, 3);
      } else {
        rep = 11 + (int)get_bits(s, 7);
      }
      if (idx + rep > hlit + hdist)
        return -1;
      while (rep--)
        lengths[idx++] = val;
    }
    if (s->err)
      return -1;
  }
  huff_build(lit, lengths, hlit);
  huff_build(dist, lengths + hlit, hdist);
  return 0;
}

/* ---- top level --------------------------------------------------------- */

static int inflate_run(struct state *s)
{
  int last;
  do {
    last = get_bit(s);
    int type = (int)get_bits(s, 2);
    if (s->err)
      return -1;
    if (type == 0) {
      if (block_stored(s) < 0)
        return -1;
    } else if (type == 1 || type == 2) {
      struct huff lit, dist;
      if (type == 1) {
        fixed_tables(&lit, &dist);
      } else if (dynamic_tables(s, &lit, &dist) < 0) {
        return -1;
      }
      if (block_huff(s, &lit, &dist) < 0)
        return -1;
    } else {
      return -1; /* reserved block type             */
    }
  } while (!last && !s->err && !s->stop);

  flush(s);
  return s->err ? -1 : 0;
}

int inflate_stream(inf_reader rd, void *rctx, inf_writer wr, void *wctx)
{
  /* Per-call state (~37 KiB: window + flush buffer) so concurrent decodes on
   * separate threads — e.g. the browser's per-image workers — never share the
   * window or the output cursor. */
  struct state *s = malloc(sizeof *s);
  if (!s)
    return -1;
  s->rd = rd;
  s->rctx = rctx;
  s->wr = wr;
  s->wctx = wctx;
  s->bitbuf = 0;
  s->bitcnt = 0;
  s->err = 0;
  s->stop = 0;
  s->total = 0;
  s->obn = 0;

  int rc = inflate_run(s);
  free(s);
  return rc;
}

/* Read exactly one byte or flag EOF. */
static int rd1(inf_reader rd, void *ctx, int *eof)
{
  int c = rd(ctx);
  if (c < 0) {
    *eof = 1;
    return 0;
  }
  return c;
}

/* A reader that replays up to two peeked bytes, then delegates — used by the
 * raw-DEFLATE fallback to "un-consume" the bytes we read to sniff the wrapper.
 */
struct prefix_rd {
  inf_reader rd;
  void *ctx;
  int pre[2];
  int n;
};
static int prefix_reader(void *c)
{
  struct prefix_rd *p = c;
  if (p->n < 2)
    return p->pre[p->n++];
  return p->rd(p->ctx);
}

int inflate_http(inf_reader rd, void *rctx, inf_writer wr, void *wctx)
{
  /* Peek the first two bytes to recognise the wrapper. Because the reader is
   * a one-way pull, we consume them and branch; a raw-DEFLATE stream is the
   * fallback, which we must then decode starting from those two bytes. So for
   * the raw case we splice them back in via a tiny prefix reader. */
  int eof = 0;
  int b0 = rd1(rd, rctx, &eof);
  if (eof)
    return -1;
  int b1 = rd1(rd, rctx, &eof);
  if (eof)
    return -1;

  if (b0 == 0x1F && b1 == 0x8B) {
    /* gzip: CM, FLG, then MTIME(4) XFL OS = 10-byte fixed header. */
    int cm = rd1(rd, rctx, &eof);
    int flg = rd1(rd, rctx, &eof);
    for (int i = 0; i < 6; i++)
      rd1(rd, rctx, &eof); /* MTIME/XFL/OS    */
    if (eof || cm != 8)
      return -1;
    if (flg & 0x04) { /* FEXTRA          */
      int x0 = rd1(rd, rctx, &eof), x1 = rd1(rd, rctx, &eof);
      int xlen = x0 | (x1 << 8);
      for (int i = 0; i < xlen; i++)
        rd1(rd, rctx, &eof);
    }
    if (flg & 0x08)
      while (!eof && rd1(rd, rctx, &eof))
        ; /* FNAME      */
    if (flg & 0x10)
      while (!eof && rd1(rd, rctx, &eof))
        ; /* FCOMMENT   */
    if (flg & 0x02) {
      rd1(rd, rctx, &eof);
      rd1(rd, rctx, &eof);
    } /* FHCRC */
    if (eof)
      return -1;
    return inflate_stream(rd, rctx, wr, wctx);
  }

  if ((b0 & 0x0F) == 0x08 && ((b0 << 8 | b1) % 31) == 0) {
    /* zlib: 2-byte header already consumed (no FDICT in HTTP). */
    return inflate_stream(rd, rctx, wr, wctx);
  }

  /* Raw DEFLATE: replay the two peeked bytes, then the rest of the stream.
   * Local (not static): prefix_reader only runs synchronously below, and a
   * shared static would corrupt a concurrent decode. */
  struct prefix_rd pr;
  pr.rd = rd;
  pr.ctx = rctx;
  pr.pre[0] = b0;
  pr.pre[1] = b1;
  pr.n = 0;
  return inflate_stream(prefix_reader, &pr, wr, wctx);
}
