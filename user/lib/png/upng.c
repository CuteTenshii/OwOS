/* upng.c — streaming PNG decoder (see upng.h). Built on inflate.c.
 *
 * Flow: verify signature -> read IHDR (+ PLTE) -> stream the concatenated IDAT
 * data (spanning chunks, skipping their length/CRC framing) through the
 * streaming inflater -> reassemble each filtered scanline as inflate output
 * arrives -> reverse the PNG filter -> expand samples to 8-bit RGBA -> hand the
 * row to the caller.
 *
 * Supported: all five colour types — grayscale(0), RGB(2), palette(3),
 * gray+alpha(4), RGBA(6) — at bit depths 1/2/4/8/16, interlaced or not. Rows
 * are RGBA: alpha comes from the alpha channel, palette tRNS or a colour-key.
 * CRCs are not verified. Non-interlaced images stream with a working set of ~2
 * scanlines whatever the height; interlaced images need a full RGBA frame
 * buffer (Adam7 passes deposit pixels across the whole image), so they are
 * capped at IL_MAXPX pixels and otherwise return UPNG_EUNSUP.
 */
#include "upng.h"
#include "inflate.h"
#include <stdint.h>

#define MAXW 4096              /* widest image we'll decode (bounds buffers)   */
#define ROWBYTES (MAXW * 8)    /* max stride: 16-bit RGBA = w*4*2 bytes        */
#define IL_MAXPX (512u * 384u) /* interlaced frame budget (RGBA): 768 KiB      */

struct dec {
  upng_reader rd;
  void *rctx;
  upng_row row;
  void *row_ctx;
  unsigned w, h;
  unsigned depth, color, samples; /* IHDR depth, colour type, samples/pixel  */
  unsigned stride;                /* bytes per (sub-)scanline                */
  unsigned filt_bpp;              /* filter "bpp": ceil(samples*depth/8), >=1*/
  unsigned cur_y;
  unsigned col;   /* bytes filled in cur[] (incl filter byte)*/
  int prev_valid; /* prev[] holds a real previous row        */
  int rowstop;    /* the row callback asked to stop          */
  int badfilt;    /* a scanline had an invalid filter byte   */
  /* interlace pass state */
  int interlaced;
  int pass; /* current Adam7 pass 0..6, 7 = done       */
  unsigned pass_w, pass_h, pass_stride, pass_row;
  /* palette */
  uint8_t plte[256 * 3];
  unsigned plte_n;
  /* transparency (tRNS) */
  uint8_t trns[256];         /* palette: per-index alpha               */
  unsigned trns_n;           /* number of palette alphas given         */
  int has_ckey;              /* gray/RGB single transparent colour set */
  unsigned ck_r, ck_g, ck_b; /* that colour, as 8-bit RGB              */
  /* IDAT chunk framing for idat_src(). */
  uint32_t chunk_left;
  int in_chunk;
  int idat_eof;
  uint8_t cur[1 + ROWBYTES]; /* current scanline: filter byte + samples */
  uint8_t prev[ROWBYTES];    /* previous reconstructed samples          */
  uint8_t rgba[MAXW * 4];    /* one RGBA row handed to the callback     */
};

static struct dec D;
static uint8_t il_frame[IL_MAXPX * 4]; /* interlaced de-interlace buffer   */

/* Adam7 interlace pass geometry. */
static const unsigned IL_XSTART[7] = {0, 4, 0, 2, 0, 1, 0};
static const unsigned IL_YSTART[7] = {0, 0, 4, 0, 2, 0, 1};
static const unsigned IL_XSTEP[7] = {8, 8, 4, 4, 2, 2, 1};
static const unsigned IL_YSTEP[7] = {8, 8, 8, 4, 4, 2, 2};

static int rb(struct dec *d) { return d->rd(d->rctx); }

static int be32(struct dec *d, uint32_t *v)
{
  int a = rb(d), b = rb(d), c = rb(d), e = rb(d);
  if (a < 0 || b < 0 || c < 0 || e < 0)
    return -1;
  *v = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)e;
  return 0;
}

static int chunk_hdr(struct dec *d, uint32_t *len, uint8_t ty[4])
{
  if (be32(d, len) < 0)
    return -1;
  for (int i = 0; i < 4; i++) {
    int c = rb(d);
    if (c < 0)
      return -1;
    ty[i] = (uint8_t)c;
  }
  return 0;
}

static int is_type(const uint8_t *ty, const char *s)
{
  return ty[0] == s[0] && ty[1] == s[1] && ty[2] == s[2] && ty[3] == s[3];
}

static int skip(struct dec *d, uint32_t n)
{
  for (uint32_t i = 0; i < n; i++)
    if (rb(d) < 0)
      return -1;
  return 0;
}

/* Reader handed to inflate: yields the concatenated IDAT *data* bytes, jumping
 * over each IDAT's CRC and the next chunk's length/type, and signalling EOF
 * when a non-IDAT chunk (IEND) is reached. */
static int idat_src(void *ctx)
{
  struct dec *d = ctx;
  while (d->chunk_left == 0) {
    if (d->idat_eof)
      return -1;
    if (d->in_chunk) { /* consume the finished chunk's CRC */
      if (skip(d, 4) < 0) {
        d->idat_eof = 1;
        return -1;
      }
      d->in_chunk = 0;
    }
    uint32_t len;
    uint8_t ty[4];
    if (chunk_hdr(d, &len, ty) < 0) {
      d->idat_eof = 1;
      return -1;
    }
    if (is_type(ty, "IDAT")) {
      d->chunk_left = len;
      d->in_chunk = 1; /* even an empty IDAT has a CRC      */
    } else {
      d->idat_eof = 1; /* IEND or other: IDAT data ends    */
      return -1;
    }
  }
  int b = rb(d);
  if (b < 0) {
    d->idat_eof = 1;
    return -1;
  }
  d->chunk_left--;
  return b;
}

static uint8_t paeth(int a, int b, int c)
{
  int p = a + b - c;
  int pa = p > a ? p - a : a - p;
  int pb = p > b ? p - b : b - p;
  int pc = p > c ? p - c : c - p;
  if (pa <= pb && pa <= pc)
    return (uint8_t)a;
  if (pb <= pc)
    return (uint8_t)b;
  return (uint8_t)c;
}

/* Reverse the PNG filter on cur[] in place, using filt_bpp + the given stride
 * (which is the full-row stride or a pass stride). Returns 0, or <0 on a bad
 * filter byte. Rolls the result into prev[] for the next row. */
static int unfilter(struct dec *d, unsigned st)
{
  uint8_t ft = d->cur[0];
  uint8_t *row = d->cur + 1;
  unsigned bpp = d->filt_bpp;
  for (unsigned x = 0; x < st; x++) {
    int a = x >= bpp ? row[x - bpp] : 0;
    int b = d->prev_valid ? d->prev[x] : 0;
    int c = (d->prev_valid && x >= bpp) ? d->prev[x - bpp] : 0;
    int v = row[x];
    switch (ft) {
    case 0:
      break;
    case 1:
      v += a;
      break;
    case 2:
      v += b;
      break;
    case 3:
      v += (a + b) / 2;
      break;
    case 4:
      v += paeth(a, b, c);
      break;
    default:
      return -1;
    }
    row[x] = (uint8_t)v;
  }
  for (unsigned x = 0; x < st; x++)
    d->prev[x] = row[x];
  d->prev_valid = 1;
  return 0;
}

/* One 8-bit-scaled colour sample (sample index `si` within the unfiltered row).
 * Handles 16-bit (take the high byte), 8-bit, and sub-byte depths (scaled to
 * 0..255). */
static unsigned chan8(const uint8_t *row, unsigned si, unsigned depth)
{
  if (depth == 16)
    return row[si * 2];
  if (depth == 8)
    return row[si];
  unsigned per = 8 / depth, mask = (1u << depth) - 1;
  unsigned byte = row[si / per];
  unsigned shift = (per - 1 - (si % per)) * depth;
  return ((byte >> shift) & mask) * 255u / mask;
}

/* One raw (unscaled) sample — used for palette indices (depth 1/2/4/8). */
static unsigned raw_sample(const uint8_t *row, unsigned si, unsigned depth)
{
  if (depth == 8)
    return row[si];
  unsigned per = 8 / depth, mask = (1u << depth) - 1;
  unsigned byte = row[si / per];
  unsigned shift = (per - 1 - (si % per)) * depth;
  return (byte >> shift) & mask;
}

/* Expand `n` pixels of the unfiltered row into RGBA quads in dst — alpha comes
 * from an explicit alpha channel (types 4/6), a palette tRNS table (type 3), or
 * a single transparent colour-key (gray/RGB tRNS); otherwise 255 (opaque). */
static void row_to_rgba(struct dec *d, const uint8_t *row, uint8_t *dst, unsigned n)
{
  unsigned depth = d->depth;
  for (unsigned x = 0; x < n; x++) {
    uint8_t *o = dst + x * 4;
    o[3] = 255;
    switch (d->color) {
    case 0: { /* grayscale               */
      unsigned g = chan8(row, x, depth);
      o[0] = o[1] = o[2] = (uint8_t)g;
      if (d->has_ckey && g == d->ck_r)
        o[3] = 0;
      break;
    }
    case 2: /* RGB                     */
      o[0] = (uint8_t)chan8(row, x * 3 + 0, depth);
      o[1] = (uint8_t)chan8(row, x * 3 + 1, depth);
      o[2] = (uint8_t)chan8(row, x * 3 + 2, depth);
      if (d->has_ckey && o[0] == d->ck_r && o[1] == d->ck_g && o[2] == d->ck_b)
        o[3] = 0;
      break;
    case 3: { /* palette (indexed)       */
      unsigned idx = raw_sample(row, x, depth);
      if (idx >= d->plte_n)
        idx = 0;
      const uint8_t *p = d->plte + idx * 3;
      o[0] = p[0];
      o[1] = p[1];
      o[2] = p[2];
      if (idx < d->trns_n)
        o[3] = d->trns[idx];
      break;
    }
    case 4: /* gray + alpha            */
      o[0] = o[1] = o[2] = (uint8_t)chan8(row, x * 2 + 0, depth);
      o[3] = (uint8_t)chan8(row, x * 2 + 1, depth);
      break;
    case 6: /* RGBA                    */
      o[0] = (uint8_t)chan8(row, x * 4 + 0, depth);
      o[1] = (uint8_t)chan8(row, x * 4 + 1, depth);
      o[2] = (uint8_t)chan8(row, x * 4 + 2, depth);
      o[3] = (uint8_t)chan8(row, x * 4 + 3, depth);
      break;
    }
  }
}

/* ------------------------------------------------------- non-interlaced sink
 */

static int emit_scanline(struct dec *d)
{
  if (unfilter(d, d->stride) < 0) {
    d->badfilt = 1;
    return -1;
  }
  row_to_rgba(d, d->cur + 1, d->rgba, d->w);
  return d->row(d->row_ctx, d->cur_y, d->rgba, d->w);
}

static int sink_plain(struct dec *d, const unsigned char *buf, unsigned n)
{
  unsigned i = 0;
  while (i < n) {
    /* Copy as much of the current scanline as this buffer holds in one run,
     * so the per-row boundary check happens once per row, not per byte. */
    unsigned need = (1 + d->stride) - d->col;
    unsigned take = (n - i < need) ? (n - i) : need;
    for (unsigned k = 0; k < take; k++)
      d->cur[d->col + k] = buf[i + k];
    d->col += take;
    i += take;
    if (d->col == 1 + d->stride) {
      int r = emit_scanline(d);
      d->col = 0;
      d->cur_y++;
      if (r < 0) {
        d->rowstop = 1;
        return -1;
      }
      if (d->cur_y >= d->h)
        return -1; /* all rows done            */
    }
  }
  return 0;
}

/* ----------------------------------------------------------- interlaced sink
 */

/* Advance to the next non-empty Adam7 pass (or pass 7 = finished). Sets the
 * pass width/height/stride and resets the per-pass filter state. */
static void il_next_pass(struct dec *d)
{
  while (++d->pass < 7) {
    unsigned xs = IL_XSTART[d->pass], ys = IL_YSTART[d->pass];
    unsigned xst = IL_XSTEP[d->pass], yst = IL_YSTEP[d->pass];
    d->pass_w = (d->w > xs) ? (d->w - xs + xst - 1) / xst : 0;
    d->pass_h = (d->h > ys) ? (d->h - ys + yst - 1) / yst : 0;
    if (d->pass_w && d->pass_h) {
      d->pass_stride = (d->pass_w * d->samples * d->depth + 7) / 8;
      d->pass_row = 0;
      d->col = 0;
      d->prev_valid = 0;
      return;
    }
  }
}

static int sink_interlaced(struct dec *d, const unsigned char *buf, unsigned n)
{
  for (unsigned i = 0; i < n; i++) {
    if (d->pass >= 7)
      return -1;
    d->cur[d->col++] = buf[i];
    if (d->col == 1 + d->pass_stride) {
      if (unfilter(d, d->pass_stride) < 0) {
        d->rowstop = 0;
        return -1;
      }
      row_to_rgba(d, d->cur + 1, d->rgba, d->pass_w);
      unsigned fy = IL_YSTART[d->pass] + d->pass_row * IL_YSTEP[d->pass];
      unsigned xs = IL_XSTART[d->pass], xst = IL_XSTEP[d->pass];
      uint8_t *base = il_frame + (unsigned long)fy * d->w * 4;
      for (unsigned x = 0; x < d->pass_w; x++) {
        uint8_t *o = base + (unsigned long)(xs + x * xst) * 4;
        const uint8_t *s = d->rgba + x * 4;
        o[0] = s[0];
        o[1] = s[1];
        o[2] = s[2];
        o[3] = s[3];
      }
      d->col = 0;
      if (++d->pass_row >= d->pass_h) {
        il_next_pass(d);
        if (d->pass >= 7)
          return -1; /* all passes done          */
      }
    }
  }
  return 0;
}

/* inflate output sink: dispatch to the plain or interlaced reassembler. */
static int scan_sink(void *ctx, const unsigned char *buf, unsigned n)
{
  struct dec *d = ctx;
  return d->interlaced ? sink_interlaced(d, buf, n) : sink_plain(d, buf, n);
}

int upng_dimensions(upng_reader rd, void *rctx, unsigned *w, unsigned *h)
{
  /* Read inline (no struct dec on the stack — it is large and would blow a
   * small user stack). Signature, then the IHDR chunk's length+type, then
   * its first 8 bytes (width, height). */
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  for (int i = 0; i < 8; i++)
    if (rd(rctx) != sig[i])
      return UPNG_EBADSIG;
  unsigned char b[8];
  for (int i = 0; i < 8; i++) { /* chunk length(4) + type(4)        */
    int c = rd(rctx);
    if (c < 0)
      return UPNG_ETRUNC;
    b[i] = (unsigned char)c;
  }
  if (!(b[4] == 'I' && b[5] == 'H' && b[6] == 'D' && b[7] == 'R'))
    return UPNG_EBADHDR;
  unsigned char d[8];
  for (int i = 0; i < 8; i++) { /* IHDR: width(4), height(4)        */
    int c = rd(rctx);
    if (c < 0)
      return UPNG_ETRUNC;
    d[i] = (unsigned char)c;
  }
  if (w)
    *w = ((unsigned)d[0] << 24) | ((unsigned)d[1] << 16) | ((unsigned)d[2] << 8) | d[3];
  if (h)
    *h = ((unsigned)d[4] << 24) | ((unsigned)d[5] << 16) | ((unsigned)d[6] << 8) | d[7];
  return UPNG_OK;
}

/* ----------------------------------------------------------- metadata probe */
/*
 * A second, decode-free pass over the chunk stream that collects the IHDR
 * fields and the ancillary metadata chunks (tRNS/pHYs/gAMA/tIME/tEXt/iTXt/zTXt/
 * eXIf) for an "image properties" view. It never inflates IDAT — it skips the
 * image data byte-for-byte through the reader and stops at IEND.
 */

struct rdr {
  upng_reader rd;
  void *rctx;
};
static int prb(struct rdr *r) { return r->rd(r->rctx); }

static int pbe32(struct rdr *r, uint32_t *v)
{
  int a = prb(r), b = prb(r), c = prb(r), e = prb(r);
  if (a < 0 || b < 0 || c < 0 || e < 0)
    return -1;
  *v = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)e;
  return 0;
}

static int pskip(struct rdr *r, uint32_t n)
{
  for (uint32_t i = 0; i < n; i++)
    if (prb(r) < 0)
      return -1;
  return 0;
}

/* Read `want` bytes; store the first min(want,cap) into buf, drop the rest.
 * *got is the stored count. Returns 0, or -1 on a short stream. */
static int pread(struct rdr *r, unsigned char *buf, uint32_t want, uint32_t cap, uint32_t *got)
{
  uint32_t g = 0;
  for (uint32_t i = 0; i < want; i++) {
    int c = prb(r);
    if (c < 0) {
      *got = g;
      return -1;
    }
    if (i < cap)
      buf[g++] = (unsigned char)c;
  }
  *got = g;
  return 0;
}

static void m_zero(void *p, unsigned n)
{
  unsigned char *b = p;
  for (unsigned i = 0; i < n; i++)
    b[i] = 0;
}

/* Copy up to cap-1 printable Latin-1 bytes from src into dst, NUL-terminated.
 */
static void m_textcopy(char *dst, unsigned cap, const unsigned char *src, unsigned n)
{
  unsigned o = 0;
  for (unsigned i = 0; i < n && o < cap - 1; i++) {
    unsigned char c = src[i];
    if (c == 0)
      break;
    if (c < 0x20 && c != '\t')
      c = ' '; /* keep it one printable line */
    dst[o++] = (char)c;
  }
  dst[o] = 0;
}

/* Record one keyword/value text pair (tEXt/iTXt/zTXt) into the meta table. */
static void m_addtext(struct upng_meta *m, const unsigned char *p, unsigned n)
{
  if (m->n_text >= UPNG_TEXT_MAX)
    return;
  unsigned k = 0;
  while (k < n && p[k] != 0)
    k++; /* keyword runs to the first NUL  */
  struct upng_text *t = &m->text[m->n_text];
  m_textcopy(t->key, UPNG_KEY_MAX, p, k);
  if (t->key[0] == 0)
    return; /* a keyword is mandatory          */
  const unsigned char *v = p + (k < n ? k + 1 : n);
  unsigned vn = (k + 1 <= n) ? n - (k + 1) : 0;
  m_textcopy(t->val, UPNG_VAL_MAX, v, vn);
  m->n_text++;
}

int upng_probe(upng_reader rd, void *rctx, struct upng_meta *m)
{
  m_zero(m, sizeof *m);
  struct rdr R = {rd, rctx}, *r = &R;

  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  for (int i = 0; i < 8; i++)
    if (prb(r) != sig[i])
      return UPNG_EBADSIG;

  int have_ihdr = 0;
  for (;;) {
    uint32_t len;
    uint8_t ty[4];
    if (pbe32(r, &len) < 0)
      break;
    for (int i = 0; i < 4; i++) {
      int c = prb(r);
      if (c < 0)
        goto done;
      ty[i] = (uint8_t)c;
    }

    if (is_type(ty, "IHDR")) {
      unsigned char hb[13];
      uint32_t got;
      if (pread(r, hb, len, 13, &got) < 0 || got < 13)
        return UPNG_EBADHDR;
      m->w = ((unsigned)hb[0] << 24) | (hb[1] << 16) | (hb[2] << 8) | hb[3];
      m->h = ((unsigned)hb[4] << 24) | (hb[5] << 16) | (hb[6] << 8) | hb[7];
      m->depth = hb[8];
      m->color = hb[9];
      m->interlaced = hb[12] == 1;
      switch (m->color) {
      case 0:
        m->channels = 1;
        break;
      case 2:
        m->channels = 3;
        break;
      case 3:
        m->channels = 1;
        break;
      case 4:
        m->channels = 2;
        m->has_alpha = 1;
        break;
      case 6:
        m->channels = 4;
        m->has_alpha = 1;
        break;
      default:
        m->channels = 0;
        break;
      }
      have_ihdr = 1;
      if (pskip(r, 4) < 0)
        goto done; /* CRC */
    } else if (is_type(ty, "PLTE")) {
      m->palette_n = len / 3;
      if (pskip(r, len + 4) < 0)
        goto done;
    } else if (is_type(ty, "tRNS")) {
      m->has_trns = 1;
      if (pskip(r, len + 4) < 0)
        goto done;
    } else if (is_type(ty, "pHYs")) {
      unsigned char b[9];
      uint32_t got;
      if (pread(r, b, len, 9, &got) < 0)
        goto done;
      if (got >= 9) {
        m->ppu_x = ((unsigned)b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
        m->ppu_y = ((unsigned)b[4] << 24) | (b[5] << 16) | (b[6] << 8) | b[7];
        m->phys_metre = (b[8] == 1);
        m->have_phys = 1;
      }
      if (pskip(r, (len > 9 ? len - 9 : 0) + 4) < 0)
        goto done;
    } else if (is_type(ty, "gAMA")) {
      unsigned char b[4];
      uint32_t got;
      if (pread(r, b, len, 4, &got) < 0)
        goto done;
      if (got >= 4) {
        m->gamma = ((unsigned)b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
        m->have_gamma = 1;
      }
      if (pskip(r, (len > 4 ? len - 4 : 0) + 4) < 0)
        goto done;
    } else if (is_type(ty, "tIME")) {
      unsigned char b[7];
      uint32_t got;
      if (pread(r, b, len, 7, &got) < 0)
        goto done;
      if (got >= 7) {
        m->year = (b[0] << 8) | b[1];
        m->mon = b[2];
        m->day = b[3];
        m->hour = b[4];
        m->min = b[5];
        m->sec = b[6];
        m->have_time = 1;
      }
      if (pskip(r, (len > 7 ? len - 7 : 0) + 4) < 0)
        goto done;
    } else if (is_type(ty, "tEXt")) {
      unsigned char b[256];
      uint32_t got;
      if (pread(r, b, len, sizeof b, &got) < 0)
        goto done;
      m_addtext(m, b, got);
      if (pskip(r, (len > got ? len - got : 0) + 4) < 0)
        goto done;
    } else if (is_type(ty, "iTXt")) {
      /* keyword\0 cflag(1) cmethod(1) lang\0 transkw\0 text */
      unsigned char b[512];
      uint32_t got;
      if (pread(r, b, len, sizeof b, &got) < 0)
        goto done;
      unsigned k = 0;
      while (k < got && b[k])
        k++;
      if (k + 2 < got) {
        int compressed = b[k + 1];
        unsigned p = k + 3; /* past cflag+cmethod      */
        while (p < got && b[p])
          p++; /* skip language tag       */
        if (p < got)
          p++;
        while (p < got && b[p])
          p++; /* skip translated keyword */
        if (p < got)
          p++;
        if (m->n_text < UPNG_TEXT_MAX) {
          struct upng_text *t = &m->text[m->n_text];
          m_textcopy(t->key, UPNG_KEY_MAX, b, k);
          if (t->key[0]) {
            if (compressed)
              m_textcopy(t->val, UPNG_VAL_MAX, (const unsigned char *)"(compressed)", 12);
            else
              m_textcopy(t->val, UPNG_VAL_MAX, b + p, p < got ? got - p : 0);
            m->n_text++;
          }
        }
      }
      if (pskip(r, (len > got ? len - got : 0) + 4) < 0)
        goto done;
    } else if (is_type(ty, "zTXt")) {
      unsigned char b[256];
      uint32_t got;
      if (pread(r, b, len, sizeof b, &got) < 0)
        goto done;
      unsigned k = 0;
      while (k < got && b[k])
        k++;
      if (m->n_text < UPNG_TEXT_MAX && k > 0) {
        struct upng_text *t = &m->text[m->n_text];
        m_textcopy(t->key, UPNG_KEY_MAX, b, k);
        if (t->key[0]) {
          m_textcopy(t->val, UPNG_VAL_MAX, (const unsigned char *)"(compressed)", 12);
          m->n_text++;
        }
      }
      if (pskip(r, (len > got ? len - got : 0) + 4) < 0)
        goto done;
    } else if (is_type(ty, "eXIf")) {
      if (len > UPNG_EXIF_MAX) {
        m->exif_len = -1; /* present but over our cap */
        if (pskip(r, len + 4) < 0)
          goto done;
      } else {
        uint32_t got;
        if (pread(r, m->exif, len, UPNG_EXIF_MAX, &got) < 0)
          goto done;
        m->exif_len = (int)got;
        if (pskip(r, 4) < 0)
          goto done; /* CRC */
      }
    } else if (is_type(ty, "IDAT") || is_type(ty, "IEND")) {
      /* Stop at the image data: every metadata chunk we report is mandated
       * (or by convention placed) before IDAT, and scanning megabytes of
       * pixel data just to catch a rare trailing text chunk would freeze a
       * caller's UI. So the probe ends here. */
      break;
    } else {
      if (pskip(r, len + 4) < 0)
        goto done; /* ancillary + CRC */
    }
  }
done:
  return have_ihdr ? UPNG_OK : UPNG_EBADHDR;
}

/* ------------------------------------------------------------- EXIF decoding
 */

/* TIFF integer reads honouring the block's byte order, all bounds-checked. */
static unsigned ex16(const unsigned char *e, int len, int o, int le)
{
  if (o < 0 || o + 2 > len)
    return 0;
  return le ? e[o] | (e[o + 1] << 8) : (e[o] << 8) | e[o + 1];
}
static unsigned ex32(const unsigned char *e, int len, int o, int le)
{
  if (o < 0 || o + 4 > len)
    return 0;
  return le ? e[o] | (e[o + 1] << 8) | (e[o + 2] << 16) | ((unsigned)e[o + 3] << 24)
            : ((unsigned)e[o] << 24) | (e[o + 1] << 16) | (e[o + 2] << 8) | e[o + 3];
}

static unsigned ex_typesize(unsigned t)
{
  switch (t) {
  case 1:
  case 2:
  case 6:
  case 7:
    return 1; /* BYTE/ASCII/SBYTE/UNDEFINED */
  case 3:
  case 8:
    return 2; /* SHORT/SSHORT               */
  case 4:
  case 9:
  case 11:
    return 4; /* LONG/SLONG/FLOAT           */
  case 5:
  case 10:
  case 12:
    return 8; /* RATIONAL/SRATIONAL/DOUBLE  */
  default:
    return 0;
  }
}

/* Small freestanding string helpers (upng.c is standalone — no libc here). */
static char *e_str(char *p, const char *s)
{
  while (*s)
    *p++ = *s++;
  *p = 0;
  return p;
}
static char *e_uint(char *p, unsigned v)
{
  char t[12];
  int n = 0;
  if (v == 0)
    t[n++] = '0';
  while (v) {
    t[n++] = (char)('0' + v % 10);
    v /= 10;
  }
  while (n)
    *p++ = t[--n];
  *p = 0;
  return p;
}

static unsigned e_gcd(unsigned a, unsigned b)
{
  while (b) {
    unsigned t = a % b;
    a = b;
    b = t;
  }
  return a;
}

/* Format the value field of one IFD entry by tag into `out`; return the human
 * label, or 0 to drop the entry. `dptr` points at the value's data in e[]. */
static const char *ex_format(unsigned tag, unsigned type, unsigned cnt, const unsigned char *e, int len, int dptr,
                             int le, char *out)
{
  char *p = out;
  out[0] = 0;

  /* ASCII string tags. */
  if (type == 2) {
    const char *label = 0;
    switch (tag) {
    case 0x010E:
      label = "Description";
      break;
    case 0x010F:
      label = "Make";
      break;
    case 0x0110:
      label = "Model";
      break;
    case 0x0131:
      label = "Software";
      break;
    case 0x0132:
      label = "Date/Time";
      break;
    case 0x013B:
      label = "Artist";
      break;
    case 0x8298:
      label = "Copyright";
      break;
    case 0x9003:
      label = "Date taken";
      break;
    case 0x9004:
      label = "Digitized";
      break;
    case 0xA434:
      label = "Lens";
      break;
    default:
      return 0;
    }
    unsigned o = 0, n = cnt;
    for (unsigned i = 0; i < n && o < UPNG_VAL_MAX - 1; i++) {
      if (dptr + (int)i >= len)
        break;
      unsigned char c = e[dptr + i];
      if (c == 0)
        break;
      if (c < 0x20)
        c = ' ';
      p[o++] = (char)c;
    }
    p[o] = 0;
    return o ? label : 0;
  }

  /* Rational tags (num/den). */
  if (type == 5 || type == 10) {
    unsigned num = ex32(e, len, dptr, le), den = ex32(e, len, dptr + 4, le);
    switch (tag) {
    case 0x829A: { /* ExposureTime          */
      if (!den)
        return 0;
      unsigned g = e_gcd(num, den);
      if (g) {
        num /= g;
        den /= g;
      }
      if (den == 1) {
        p = e_uint(p, num);
        p = e_str(p, " s");
      } else {
        p = e_uint(p, num);
        *p++ = '/';
        p = e_uint(p, den);
        p = e_str(p, " s");
      }
      return "Exposure";
    }
    case 0x829D: { /* FNumber               */
      if (!den)
        return 0;
      unsigned f10 = num * 10 / den;
      p = e_str(p, "f/");
      p = e_uint(p, f10 / 10);
      *p++ = '.';
      p = e_uint(p, f10 % 10);
      return "Aperture";
    }
    case 0x920A: { /* FocalLength           */
      if (!den)
        return 0;
      p = e_uint(p, num / den);
      p = e_str(p, " mm");
      return "Focal length";
    }
    case 0x011A:
    case 0x011B: { /* X/YResolution         */
      if (!den)
        return 0;
      p = e_uint(p, num / den);
      return tag == 0x011A ? "X resolution" : "Y resolution";
    }
    default:
      return 0;
    }
  }

  /* SHORT / LONG numeric tags. */
  if (type == 3 || type == 4) {
    unsigned v = (type == 3) ? ex16(e, len, dptr, le) : ex32(e, len, dptr, le);
    switch (tag) {
    case 0x0112: { /* Orientation           */
      static const char *const o[] = {"?",          "Normal",        "Mirror H",
                                      "Rotate 180", "Mirror V",      "Mirror+Rot 270",
                                      "Rotate 90",  "Mirror+Rot 90", "Rotate 270"};
      e_str(p, v >= 1 && v <= 8 ? o[v] : "?");
      return "Orientation";
    }
    case 0x0128: /* ResolutionUnit        */
      e_str(p, v == 2 ? "inch" : v == 3 ? "cm" : "none");
      return "Res. unit";
    case 0x8827: /* ISO                   */
      p = e_str(p, "ISO ");
      e_uint(p, v);
      return "ISO speed";
    case 0x8822: { /* ExposureProgram       */
      static const char *const pr[] = {"Not defined", "Manual", "Normal",   "Aperture priority", "Shutter priority",
                                       "Creative",    "Action", "Portrait", "Landscape"};
      e_str(p, v <= 8 ? pr[v] : "?");
      return "Program";
    }
    case 0x9209: /* Flash                 */
      e_str(p, (v & 1) ? "Fired" : "Did not fire");
      return "Flash";
    case 0xA002: /* PixelXDimension       */
      e_uint(p, v);
      return "Stored width";
    case 0xA003: /* PixelYDimension       */
      e_uint(p, v);
      return "Stored height";
    default:
      return 0;
    }
  }
  return 0;
}

/* Walk one IFD at byte offset `off`. Emits recognised tags; if !is_sub, stores
 * the Exif sub-IFD pointer (tag 0x8769) into *sub for a single follow-up walk.
 */
static void ex_walk(const unsigned char *e, int len, int off, int le, int is_sub, int *sub, upng_kv emit, void *ctx)
{
  if (off < 0 || off + 2 > len)
    return;
  unsigned count = ex16(e, len, off, le);
  if (count > 256)
    count = 256; /* sane bound            */
  for (unsigned i = 0; i < count; i++) {
    int ep = off + 2 + (int)i * 12;
    if (ep + 12 > len)
      break;
    unsigned tag = ex16(e, len, ep, le);
    unsigned type = ex16(e, len, ep + 2, le);
    unsigned cnt = ex32(e, len, ep + 4, le);
    if (!is_sub && tag == 0x8769) {
      if (sub)
        *sub = (int)ex32(e, len, ep + 8, le);
      continue;
    }
    unsigned ts = ex_typesize(type);
    if (!ts)
      continue;
    unsigned total = ts * cnt;
    int dptr = (total <= 4) ? ep + 8 : (int)ex32(e, len, ep + 8, le);
    if (dptr < 0 || dptr >= len)
      continue;
    char val[UPNG_VAL_MAX];
    const char *label = ex_format(tag, type, cnt, e, len, dptr, le, val);
    if (label && val[0])
      emit(ctx, label, val);
  }
}

void upng_exif_emit(const unsigned char *exif, int len, upng_kv emit, void *ctx)
{
  if (len < 8)
    return;
  int le;
  if (exif[0] == 'I' && exif[1] == 'I')
    le = 1;
  else if (exif[0] == 'M' && exif[1] == 'M')
    le = 0;
  else
    return;
  if (ex16(exif, len, 2, le) != 42)
    return;
  int ifd0 = (int)ex32(exif, len, 4, le);
  int sub = 0;
  ex_walk(exif, len, ifd0, le, 0, &sub, emit, ctx);
  if (sub > 0 && sub < len)
    ex_walk(exif, len, sub, le, 1, 0, emit, ctx);
}

/* Validate the IHDR depth/colour combination per the PNG spec, and set samples.
 */
static int set_format(struct dec *d)
{
  unsigned dep = d->depth;
  switch (d->color) {
  case 0:
    d->samples = 1;
    if (!(dep == 1 || dep == 2 || dep == 4 || dep == 8 || dep == 16))
      return -1;
    break;
  case 2:
    d->samples = 3;
    if (!(dep == 8 || dep == 16))
      return -1;
    break;
  case 3:
    d->samples = 1;
    if (!(dep == 1 || dep == 2 || dep == 4 || dep == 8))
      return -1;
    break;
  case 4:
    d->samples = 2;
    if (!(dep == 8 || dep == 16))
      return -1;
    break;
  case 6:
    d->samples = 4;
    if (!(dep == 8 || dep == 16))
      return -1;
    break;
  default:
    return -1;
  }
  d->filt_bpp = (d->samples * dep + 7) / 8;
  if (d->filt_bpp == 0)
    d->filt_bpp = 1;
  d->stride = (d->w * d->samples * dep + 7) / 8;
  return 0;
}

int upng_decode(upng_reader rd, void *rctx, upng_row row, void *row_ctx, unsigned *out_w, unsigned *out_h)
{
  struct dec *d = &D;
  d->rd = rd;
  d->rctx = rctx;
  d->row = row;
  d->row_ctx = row_ctx;
  d->cur_y = 0;
  d->col = 0;
  d->prev_valid = 0;
  d->rowstop = 0;
  d->badfilt = 0;
  d->chunk_left = 0;
  d->in_chunk = 0;
  d->idat_eof = 0;
  d->plte_n = 0;
  d->pass = -1;
  d->trns_n = 0;
  d->has_ckey = 0;

  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  for (int i = 0; i < 8; i++)
    if (rb(d) != sig[i])
      return UPNG_EBADSIG;

  unsigned interlace = 0;
  int have_ihdr = 0;
  for (;;) {
    uint32_t len;
    uint8_t ty[4];
    if (chunk_hdr(d, &len, ty) < 0)
      return UPNG_ETRUNC;
    if (is_type(ty, "IHDR")) {
      if (len < 13)
        return UPNG_EBADHDR;
      uint8_t hb[13];
      for (int i = 0; i < 13; i++) {
        int c = rb(d);
        if (c < 0)
          return UPNG_ETRUNC;
        hb[i] = (uint8_t)c;
      }
      if (len > 13 && skip(d, len - 13) < 0)
        return UPNG_ETRUNC;
      d->w = ((unsigned)hb[0] << 24) | (hb[1] << 16) | (hb[2] << 8) | hb[3];
      d->h = ((unsigned)hb[4] << 24) | (hb[5] << 16) | (hb[6] << 8) | hb[7];
      d->depth = hb[8];
      d->color = hb[9];
      interlace = hb[12];
      if (skip(d, 4) < 0)
        return UPNG_ETRUNC; /* CRC               */
      have_ihdr = 1;
    } else if (is_type(ty, "PLTE")) {
      if (!have_ihdr)
        return UPNG_EBADHDR;
      unsigned n = len / 3;
      if (n > 256)
        n = 256;
      for (unsigned i = 0; i < n * 3; i++) {
        int c = rb(d);
        if (c < 0)
          return UPNG_ETRUNC;
        d->plte[i] = (uint8_t)c;
      }
      if (skip(d, len - n * 3 + 4) < 0)
        return UPNG_ETRUNC; /* rest+CRC */
      d->plte_n = n;
    } else if (is_type(ty, "tRNS")) {
      if (!have_ihdr)
        return UPNG_EBADHDR;
      if (d->color == 3) { /* per-index alpha    */
        unsigned n = len > 256 ? 256 : len;
        for (unsigned i = 0; i < n; i++) {
          int c = rb(d);
          if (c < 0)
            return UPNG_ETRUNC;
          d->trns[i] = (uint8_t)c;
        }
        if (skip(d, len - n + 4) < 0)
          return UPNG_ETRUNC;
        d->trns_n = n;
      } else if (d->color == 0 && len >= 2) { /* gray colour-key    */
        uint8_t b0 = (uint8_t)rb(d), b1 = (uint8_t)rb(d);
        d->ck_r = d->ck_g = d->ck_b = (d->depth == 16) ? b0 : d->depth == 8 ? b1 : b1 * 255u / ((1u << d->depth) - 1);
        d->has_ckey = 1;
        if (skip(d, len - 2 + 4) < 0)
          return UPNG_ETRUNC;
      } else if (d->color == 2 && len >= 6) { /* RGB colour-key     */
        uint8_t v[6];
        for (int i = 0; i < 6; i++) {
          int c = rb(d);
          if (c < 0)
            return UPNG_ETRUNC;
          v[i] = (uint8_t)c;
        }
        d->ck_r = (d->depth == 16) ? v[0] : v[1];
        d->ck_g = (d->depth == 16) ? v[2] : v[3];
        d->ck_b = (d->depth == 16) ? v[4] : v[5];
        d->has_ckey = 1;
        if (skip(d, len - 6 + 4) < 0)
          return UPNG_ETRUNC;
      } else if (skip(d, len + 4) < 0) {
        return UPNG_ETRUNC;
      }
    } else if (is_type(ty, "IDAT")) {
      if (!have_ihdr)
        return UPNG_EBADHDR;
      d->chunk_left = len;
      d->in_chunk = 1; /* first IDAT open    */
      break;
    } else if (is_type(ty, "IEND")) {
      return UPNG_ETRUNC;              /* no image data     */
    } else if (skip(d, len + 4) < 0) { /* ancillary + CRC    */
      return UPNG_ETRUNC;
    }
  }
  if (!have_ihdr || d->w == 0 || d->h == 0)
    return UPNG_EBADHDR;
  if (interlace > 1 || d->w > MAXW)
    return UPNG_EUNSUP;
  if (set_format(d) < 0)
    return UPNG_EUNSUP;
  if (d->color == 3 && d->plte_n == 0)
    return UPNG_EBADHDR; /* palette needs PLTE */
  d->interlaced = (interlace == 1);
  if (d->interlaced) {
    if ((unsigned long)d->w * d->h > IL_MAXPX)
      return UPNG_EUNSUP;
    il_next_pass(d); /* prime the first pass   */
  }

  if (out_w)
    *out_w = d->w;
  if (out_h)
    *out_h = d->h;

  /* IDAT is a zlib stream; inflate_http detects the 0x78 header and inflates.
   */
  inflate_http(idat_src, d, scan_sink, d);

  if (d->interlaced) {
    if (d->pass < 7)
      return UPNG_EINFLATE;               /* didn't finish passes   */
    for (unsigned y = 0; y < d->h; y++) { /* emit the assembled rows*/
      if (d->row(d->row_ctx, y, il_frame + (unsigned long)y * d->w * 4, d->w) < 0)
        return UPNG_OK; /* caller stopped: fine   */
    }
    return UPNG_OK;
  }

  if (d->badfilt)
    return UPNG_EINFLATE;
  if (d->rowstop)
    return UPNG_OK; /* caller stopped: fine      */
  if (d->cur_y < d->h)
    return UPNG_EINFLATE; /* didn't get every row      */
  return UPNG_OK;
}
