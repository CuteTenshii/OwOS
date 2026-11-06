/* alac.c — Apple Lossless command-line tool.
 *
 *   alac info   <file.caf>            print stream parameters
 *   alac encode <in.wav>  <out.caf>   WAV (PCM) -> Apple Lossless in a CAF file
 *   alac decode <in.caf>  <out.wav>   Apple Lossless -> WAV (PCM)
 *   alac verify <in.wav>              encode -> decode and confirm bit-exact
 *
 * The codec lives in user/lib/alac; this front end adds the containers: a WAV
 * (RIFF/PCM) reader/writer and a minimal CAF (Core Audio Format) reader/writer,
 * the canonical raw-ALAC container. PCM is streamed a packet at a time through
 * readfile_at / stream_write so a long song never sits in memory whole; only
 * the (smaller) compressed stream and its packet-size table are buffered.
 */
#include "alac.h"
#include "owos.h"
#include <stdbool.h>

/* ---- big-endian scalar helpers (CAF is big-endian) ----------------------- */
static void put_be16(uint8_t *p, unsigned v)
{
  p[0] = v >> 8;
  p[1] = v;
}
static void put_be32(uint8_t *p, unsigned v)
{
  p[0] = v >> 24;
  p[1] = v >> 16;
  p[2] = v >> 8;
  p[3] = v;
}
static void put_be64(uint8_t *p, unsigned long long v)
{
  put_be32(p, (unsigned)(v >> 32));
  put_be32(p + 4, (unsigned)v);
}
static unsigned get_be16(const uint8_t *p) { return (p[0] << 8) | p[1]; }
static unsigned get_be32(const uint8_t *p) { return ((unsigned)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static unsigned long long get_be64(const uint8_t *p)
{
  return ((unsigned long long)get_be32(p) << 32) | get_be32(p + 4);
}

/* IEEE-754 double <-> big-endian 8 bytes (CAF stores mSampleRate as Float64).
 */
static void put_bef64(uint8_t *p, double d)
{
  union {
    double f;
    unsigned long long u;
  } x;
  x.f = d;
  put_be64(p, x.u);
}
static double get_bef64(const uint8_t *p)
{
  union {
    double f;
    unsigned long long u;
  } x;
  x.u = get_be64(p);
  return x.f;
}

static bool tag4(const uint8_t *p, const char *s)
{
  return p[0] == s[0] && p[1] == s[1] && p[2] == s[2] && p[3] == s[3];
}

static uint8_t fmt_flag(int bits) { return bits == 16 ? 1 : bits == 20 ? 2 : bits == 24 ? 3 : 4; }

/* CAF packet-table variable-length integer: base-128, big-endian, 0x80 = more.
 */
static int caf_varint_put(uint8_t *out, unsigned v)
{
  uint8_t tmp[5];
  int n = 0;
  tmp[n++] = v & 0x7f;
  v >>= 7;
  while (v) {
    tmp[n++] = (v & 0x7f) | 0x80;
    v >>= 7;
  }
  for (int i = 0; i < n; i++)
    out[i] = tmp[n - 1 - i];
  return n;
}
static unsigned caf_varint_get(const uint8_t *p, const uint8_t *end, int *used)
{
  unsigned v = 0;
  int n = 0;
  uint8_t b;
  do {
    if (p + n >= end) {
      *used = 0;
      return 0;
    }
    b = p[n++];
    v = (v << 7) | (b & 0x7f);
  } while (b & 0x80);
  *used = n;
  return v;
}

/* ---- WAV (RIFF/PCM) -------------------------------------------------------
 */
struct wav {
  unsigned channels, rate, bits, data_off, data_len;
};

static int wav_parse(const char *path, struct wav *w)
{
  uint8_t hdr[256];
  int got = readfile_at(path, 0, hdr, sizeof hdr);
  if (got < 44 || !tag4(hdr, "RIFF") || !tag4(hdr + 8, "WAVE"))
    return -1;
  unsigned fmt = 0, pos;
  memset(w, 0, sizeof *w);
  for (pos = 12; pos + 8 <= (unsigned)got;) {
    const uint8_t *c = hdr + pos;
    unsigned sz = c[4] | (c[5] << 8) | (c[6] << 16) | ((unsigned)c[7] << 24);
    if (tag4(c, "fmt ")) {
      fmt = get_be16(c + 8) >> 8 | ((c[9]) << 8); /* little-endian fields below */
      fmt = c[8] | (c[9] << 8);
      w->channels = c[10] | (c[11] << 8);
      w->rate = c[12] | (c[13] << 8) | (c[14] << 16) | ((unsigned)c[15] << 24);
      w->bits = c[22] | (c[23] << 8);
    } else if (tag4(c, "data")) {
      w->data_off = pos + 8;
      w->data_len = sz;
      break;
    }
    pos += 8 + sz + (sz & 1);
  }
  if (!w->data_off || fmt != 1)
    return -1;
  if (w->bits != 16 && w->bits != 24 && w->bits != 32)
    return -1;
  if (w->channels < 1 || w->channels > 2)
    return -1;
  return 0;
}

/* Write a 44-byte canonical WAV header. */
static void wav_header(uint8_t h[44], unsigned channels, unsigned rate, unsigned bits, unsigned data_len)
{
  unsigned byte_rate = rate * channels * (bits / 8), block = channels * (bits / 8);
  memcpy(h, "RIFF", 4);
  put_be32(h + 4, 0); /* patched below (little-endian) */
  memcpy(h + 8, "WAVE", 4);
  memcpy(h + 12, "fmt ", 4);
  h[16] = 16;
  h[17] = h[18] = h[19] = 0; /* fmt chunk size = 16 */
  h[20] = 1;
  h[21] = 0; /* PCM */
  h[22] = channels;
  h[23] = 0;
  h[24] = rate;
  h[25] = rate >> 8;
  h[26] = rate >> 16;
  h[27] = rate >> 24;
  h[28] = byte_rate;
  h[29] = byte_rate >> 8;
  h[30] = byte_rate >> 16;
  h[31] = byte_rate >> 24;
  h[32] = block;
  h[33] = block >> 8;
  h[34] = bits;
  h[35] = bits >> 8;
  memcpy(h + 36, "data", 4);
  h[40] = data_len;
  h[41] = data_len >> 8;
  h[42] = data_len >> 16;
  h[43] = data_len >> 24;
  unsigned riff = 36 + data_len;
  h[4] = riff;
  h[5] = riff >> 8;
  h[6] = riff >> 16;
  h[7] = riff >> 24;
}

/* ---- CAF parse ------------------------------------------------------------
 */
struct caf {
  alac_config cfg;
  unsigned data_off; /* file offset of the first packet byte        */
  unsigned num_packets;
  unsigned long long valid_frames;
  unsigned *pkt_size; /* per-packet byte sizes (malloc'd)             */
};

/* Read a whole file region into a fresh buffer (used for the small CAF chunks).
 */
static uint8_t *read_range(const char *path, unsigned off, unsigned len)
{
  uint8_t *b = malloc(len ? len : 1);
  unsigned p = 0;
  if (!b)
    return 0;
  while (p < len) {
    int n = readfile_at(path, off + p, b + p, len - p);
    if (n <= 0)
      break;
    p += (unsigned)n;
  }
  if (p != len) {
    free(b);
    return 0;
  }
  return b;
}

static int caf_parse(const char *path, struct caf *c)
{
  struct statinfo st;
  uint8_t hdr[8];
  unsigned pos;
  bool have_desc = false;
  bool have_kuki = false;
  bool have_pakt = false;
  if (stat(path, &st) != 0 || st.size < 8)
    return -1;
  if (readfile_at(path, 0, hdr, 8) < 8 || !tag4(hdr, "caff"))
    return -1;
  memset(c, 0, sizeof *c);

  pos = 8;
  while (pos + 12 <= st.size) {
    uint8_t ch[12];
    unsigned long long sz;
    unsigned body;
    if (readfile_at(path, pos, ch, 12) < 12)
      return -1;
    sz = get_be64(ch + 4);
    body = pos + 12;
    /* Reject a 64-bit size that truncates to unsigned or runs past EOF; the
       read_range/end math below is all 32-bit. -1 is the data-to-EOF sentinel.
     */
    if (sz != (unsigned long long)-1 && (sz > 0xFFFFFFFFu || (unsigned long long)body + sz > st.size))
      return -1;
    if (tag4(ch, "desc")) {
      uint8_t d[32];
      if (sz < 32 || readfile_at(path, body, d, 32) < 32)
        return -1;
      if (!tag4(d + 8, "alac"))
        return -1;
      c->cfg.sample_rate = (unsigned)get_bef64(d);
      c->cfg.frame_length = get_be32(d + 20);
      c->cfg.num_channels = get_be32(d + 24);
      have_desc = true;
    } else if (tag4(ch, "kuki")) {
      uint8_t *k = read_range(path, body, (unsigned)sz);
      int rc;
      if (!k)
        return -1;
      rc = alac_parse_cookie(k, (unsigned)sz, &c->cfg);
      free(k);
      if (rc != ALAC_OK)
        return -1;
      have_kuki = true;
    } else if (tag4(ch, "pakt")) {
      uint8_t *pk = read_range(path, body, (unsigned)sz), *p, *end;
      unsigned i;
      if (!pk)
        return -1;
      if (sz < 24) {
        free(pk);
        return -1;
      } /* header: 2x be64 + 2x be32 */
      c->num_packets = (unsigned)get_be64(pk + 0);
      c->valid_frames = get_be64(pk + 8);
      /* Cap the count: the table starts at pk+24 and each varint is >=1 byte,
         so it can't exceed sz-24; also guard the malloc against 32-bit wrap. */
      if (c->num_packets > 0xFFFFFFFFu / sizeof(unsigned) || c->num_packets > (unsigned)(sz - 24)) {
        free(pk);
        return -1;
      }
      c->pkt_size = malloc((c->num_packets ? c->num_packets : 1) * sizeof(unsigned));
      if (!c->pkt_size) {
        free(pk);
        return -1;
      }
      p = pk + 24;
      end = pk + (unsigned)sz;
      for (i = 0; i < c->num_packets; i++) {
        int used;
        c->pkt_size[i] = caf_varint_get(p, end, &used);
        if (!used) {
          free(pk);
          free(c->pkt_size);
          c->pkt_size = 0;
          return -1;
        }
        p += used;
      }
      free(pk);
      have_pakt = true;
    } else if (tag4(ch, "data")) {
      c->data_off = body + 4; /* skip 4-byte mEditCount */
    }
    if (sz == (unsigned long long)-1)
      break; /* data-to-EOF, must be last */
    /* Advance in 64-bit: a truncated add could fail to move pos and spin. */
    {
      unsigned long long next = (unsigned long long)body + sz;
      if (next <= pos || next > st.size)
        break;
      pos = (unsigned)next;
    }
  }
  if (!have_desc || !have_kuki || !have_pakt || !c->data_off)
    return -1;
  if (c->cfg.num_channels < 1 || c->cfg.num_channels > 2)
    return -1;
  return 0;
}

/* ---- info -----------------------------------------------------------------
 */
static int cmd_info(const char *path)
{
  struct caf c;
  unsigned long long comp = 0;
  unsigned i;
  if (caf_parse(path, &c) != 0) {
    printf("alac: not a readable ALAC/CAF file: %s\n", path);
    return 1;
  }
  for (i = 0; i < c.num_packets; i++)
    comp += c.pkt_size[i];
  printf("%s\n", path);
  printf("  format      : Apple Lossless (ALAC)\n");
  printf("  sample rate : %u Hz\n", c.cfg.sample_rate);
  printf("  channels    : %u\n", c.cfg.num_channels);
  printf("  bit depth   : %u\n", c.cfg.bit_depth);
  printf("  frame length: %u\n", c.cfg.frame_length);
  printf("  packets     : %u\n", c.num_packets);
  printf("  frames      : %u\n", (unsigned)c.valid_frames);
  {
    unsigned long long raw = c.valid_frames * alac_bytes_per_frame(&c.cfg);
    unsigned ms = c.cfg.sample_rate ? (unsigned)(c.valid_frames * 1000ull / c.cfg.sample_rate) : 0;
    printf("  duration    : %u.%03u s\n", ms / 1000, ms % 1000);
    printf("  compressed  : %u bytes", (unsigned)comp);
    if (raw)
      printf("  (%u%% of %u raw)", (unsigned)(comp * 100 / raw), (unsigned)raw);
    printf("\n");
  }
  free(c.pkt_size);
  return 0;
}

/* ---- encode: WAV -> CAF ---------------------------------------------------
 */
/* A growing byte buffer for the compressed packet stream. */
struct growbuf {
  uint8_t *buf;
  unsigned len, cap;
};
static int gb_add(struct growbuf *g, const uint8_t *p, unsigned n)
{
  if (g->len + n > g->cap) {
    unsigned nc = g->cap ? g->cap * 2 : 65536;
    while (nc < g->len + n)
      nc *= 2;
    uint8_t *nb = realloc(g->buf, nc);
    if (!nb)
      return -1;
    g->buf = nb;
    g->cap = nc;
  }
  memcpy(g->buf + g->len, p, n);
  g->len += n;
  return 0;
}

static int cmd_encode(const char *in, const char *out)
{
  struct wav w;
  alac_config cfg;
  alac_encoder *enc;
  struct growbuf data = {0, 0, 0};
  unsigned *sizes = 0, sizes_cap = 0, num_packets = 0;
  uint8_t *pcm, *pkt;
  unsigned bpf, total_frames, done = 0;
  int rc = 1;

  if (wav_parse(in, &w) != 0) {
    printf("alac: cannot read WAV %s\n", in);
    return 1;
  }
  alac_config_default(&cfg, w.rate, w.bits, w.channels);
  bpf = alac_bytes_per_frame(&cfg);
  total_frames = w.data_len / bpf;

  enc = alac_encoder_open(&cfg);
  pcm = malloc(cfg.frame_length * bpf);
  pkt = malloc(alac_max_packet_size(&cfg));
  if (!enc || !pcm || !pkt) {
    printf("alac: out of memory\n");
    goto done;
  }

  while (done < total_frames) {
    unsigned nf = total_frames - done;
    int plen, n;
    if (nf > cfg.frame_length)
      nf = cfg.frame_length;
    n = readfile_at(in, w.data_off + done * bpf, pcm, nf * bpf);
    if (n < (int)(nf * bpf)) {
      printf("alac: short read on %s\n", in);
      goto done;
    }
    plen = alac_encode(enc, pcm, nf, pkt);
    if (plen < 0) {
      printf("alac: encode error %d\n", plen);
      goto done;
    }
    if (gb_add(&data, pkt, plen) != 0) {
      printf("alac: out of memory\n");
      goto done;
    }
    if (num_packets >= sizes_cap) {
      unsigned nc = sizes_cap ? sizes_cap * 2 : 1024;
      unsigned *ns = realloc(sizes, nc * sizeof(unsigned));
      if (!ns) {
        printf("alac: out of memory\n");
        goto done;
      }
      sizes = ns;
      sizes_cap = nc;
    }
    sizes[num_packets++] = (unsigned)plen;
    done += nf;
  }

  /* assemble the CAF: caff header + desc + kuki + pakt + data */
  {
    uint8_t cookie[ALAC_COOKIE_SIZE];
    uint8_t *pakt_body = malloc(24 + num_packets * 5 + 8);
    unsigned pakt_len = 24, i;
    int wrote = -1;
    alac_build_cookie(&cfg, cookie);
    if (!pakt_body) {
      printf("alac: out of memory\n");
      goto done;
    }
    put_be64(pakt_body + 0, num_packets);
    put_be64(pakt_body + 8, total_frames);
    put_be32(pakt_body + 16, 0); /* priming */
    put_be32(pakt_body + 20, 0); /* remainder */
    for (i = 0; i < num_packets; i++)
      pakt_len += caf_varint_put(pakt_body + pakt_len, sizes[i]);

    if (stream_begin(out) == 0) {
      uint8_t fh[8], chdr[12], desc[32], mec[4] = {0, 0, 0, 0};
      memcpy(fh, "caff", 4);
      put_be16(fh + 4, 1);
      put_be16(fh + 6, 0);
      /* desc */
      put_bef64(desc + 0, (double)w.rate);
      memcpy(desc + 8, "alac", 4);
      put_be32(desc + 12, fmt_flag(w.bits));
      put_be32(desc + 16, 0);                /* bytes/packet: variable */
      put_be32(desc + 20, cfg.frame_length); /* frames/packet          */
      put_be32(desc + 24, w.channels);
      put_be32(desc + 28, 0); /* bits/channel: compressed */

      wrote = stream_write(fh, 8);
      memcpy(chdr, "desc", 4);
      put_be64(chdr + 4, 32);
      if (!wrote)
        wrote = stream_write(chdr, 12) | stream_write(desc, 32);
      memcpy(chdr, "kuki", 4);
      put_be64(chdr + 4, ALAC_COOKIE_SIZE);
      if (!wrote)
        wrote = stream_write(chdr, 12) | stream_write(cookie, ALAC_COOKIE_SIZE);
      memcpy(chdr, "pakt", 4);
      put_be64(chdr + 4, pakt_len);
      if (!wrote)
        wrote = stream_write(chdr, 12) | stream_write(pakt_body, pakt_len);
      memcpy(chdr, "data", 4);
      put_be64(chdr + 4, 4 + data.len);
      if (!wrote)
        wrote = stream_write(chdr, 12) | stream_write(mec, 4) | stream_write(data.buf, data.len);
      if (stream_end() < 0)
        wrote = -1;
    }
    free(pakt_body);
    if (wrote != 0) {
      printf("alac: cannot write %s\n", out);
      goto done;
    }
  }

  {
    unsigned raw = total_frames * bpf;
    printf("wrote %s  (%u Hz, %u ch, %u-bit, %u frames)  %u -> %u bytes (%u%%)\n", out, w.rate, w.channels, w.bits,
           total_frames, raw, data.len, raw ? (data.len * 100 / raw) : 0);
  }
  rc = 0;
done:
  alac_encoder_close(enc);
  free(pcm);
  free(pkt);
  free(data.buf);
  free(sizes);
  return rc;
}

/* ---- decode: CAF -> WAV ---------------------------------------------------
 */
static int cmd_decode(const char *in, const char *out)
{
  struct caf c;
  alac_decoder *dec;
  uint8_t *pkt, *pcm, wh[44];
  unsigned bpf, i, poff, maxpkt;
  unsigned long long emitted = 0;
  int rc = 1;

  if (caf_parse(in, &c) != 0) {
    printf("alac: not a readable ALAC/CAF file: %s\n", in);
    return 1;
  }
  bpf = alac_bytes_per_frame(&c.cfg);
  dec = alac_decoder_open(&c.cfg);
  maxpkt = alac_max_packet_size(&c.cfg);
  pkt = malloc(maxpkt);
  pcm = malloc(c.cfg.frame_length * bpf);
  if (!dec || !pkt || !pcm) {
    printf("alac: out of memory\n");
    goto done;
  }

  if (stream_begin(out) != 0) {
    printf("alac: cannot write %s\n", out);
    goto done;
  }
  wav_header(wh, c.cfg.num_channels, c.cfg.sample_rate, c.cfg.bit_depth, (unsigned)(c.valid_frames * bpf));
  if (stream_write(wh, 44) != 0) {
    stream_end();
    printf("alac: write error\n");
    goto done;
  }

  poff = c.data_off;
  for (i = 0; i < c.num_packets; i++) {
    unsigned ps = c.pkt_size[i];
    uint32_t frames = 0;
    int n;
    if (ps == 0 || ps > maxpkt) {
      stream_end();
      printf("alac: bad packet %u\n", i);
      goto done;
    }
    n = readfile_at(in, poff, pkt, ps);
    if (n < (int)ps) {
      stream_end();
      printf("alac: short read\n");
      goto done;
    }
    if (alac_decode(dec, pkt, ps, pcm, &frames) != ALAC_OK) {
      stream_end();
      printf("alac: decode error at packet %u\n", i);
      goto done;
    }
    if (stream_write(pcm, frames * bpf) != 0) {
      stream_end();
      printf("alac: write error\n");
      goto done;
    }
    emitted += frames;
    poff += ps;
  }
  if (stream_end() < 0) {
    printf("alac: commit failed\n");
    goto done;
  }

  printf("wrote %s  (%u Hz, %u ch, %u-bit, %u frames)\n", out, c.cfg.sample_rate, c.cfg.num_channels, c.cfg.bit_depth,
         (unsigned)emitted);
  rc = 0;
done:
  alac_decoder_close(dec);
  free(pkt);
  free(pcm);
  free(c.pkt_size);
  return rc;
}

/* ---- verify: encode -> decode, bit-exact, no files written ----------------
 */
static int cmd_verify(const char *in)
{
  struct wav w;
  alac_config cfg;
  alac_encoder *enc;
  alac_decoder *dec;
  uint8_t *pin, *pout, *pkt;
  unsigned bpf, total_frames, done = 0;
  unsigned long long comp = 0;
  bool ok = true;
  int rc = 1;

  if (wav_parse(in, &w) != 0) {
    printf("alac: cannot read WAV %s\n", in);
    return 1;
  }
  alac_config_default(&cfg, w.rate, w.bits, w.channels);
  bpf = alac_bytes_per_frame(&cfg);
  total_frames = w.data_len / bpf;

  enc = alac_encoder_open(&cfg);
  dec = alac_decoder_open(&cfg);
  pin = malloc(cfg.frame_length * bpf);
  pout = malloc(cfg.frame_length * bpf);
  pkt = malloc(alac_max_packet_size(&cfg));
  if (!enc || !dec || !pin || !pout || !pkt) {
    printf("alac: out of memory\n");
    goto done;
  }

  while (done < total_frames && ok) {
    unsigned nf = total_frames - done;
    uint32_t gf = 0;
    int plen, n;
    if (nf > cfg.frame_length)
      nf = cfg.frame_length;
    n = readfile_at(in, w.data_off + done * bpf, pin, nf * bpf);
    if (n < (int)(nf * bpf)) {
      printf("alac: short read\n");
      goto done;
    }
    plen = alac_encode(enc, pin, nf, pkt);
    if (plen < 0) {
      printf("alac: encode error\n");
      goto done;
    }
    comp += plen;
    if (alac_decode(dec, pkt, plen, pout, &gf) != ALAC_OK) {
      printf("alac: decode error\n");
      goto done;
    }
    if (gf != nf)
      ok = false;
    for (unsigned k = 0; ok && k < nf * bpf; k++)
      if (pin[k] != pout[k])
        ok = false;
    done += nf;
  }
  {
    unsigned raw = total_frames * bpf;
    printf("%s: %u Hz %u-ch %u-bit, %u frames  raw=%u alac=%u (%u%%)  -> %s\n", in, w.rate, w.channels, w.bits,
           total_frames, raw, (unsigned)comp, raw ? (unsigned)(comp * 100 / raw) : 0, ok ? "lossless OK" : "MISMATCH");
  }
  rc = ok ? 0 : 1;
done:
  alac_encoder_close(enc);
  alac_decoder_close(dec);
  free(pin);
  free(pout);
  free(pkt);
  return rc;
}

int main(int argc, char **argv)
{
  if (argc >= 3 && argv[1][0] == 'i')
    return cmd_info(argv[2]);
  if (argc >= 4 && argv[1][0] == 'e')
    return cmd_encode(argv[2], argv[3]);
  if (argc >= 4 && argv[1][0] == 'd')
    return cmd_decode(argv[2], argv[3]);
  if (argc >= 3 && argv[1][0] == 'v')
    return cmd_verify(argv[2]);

  puts("usage:\n");
  puts("  alac info   <file.caf>          show ALAC stream parameters\n");
  puts("  alac encode <in.wav> <out.caf>  WAV (PCM) -> Apple Lossless\n");
  puts("  alac decode <in.caf> <out.wav>  Apple Lossless -> WAV (PCM)\n");
  puts("  alac verify <in.wav>            confirm a bit-exact round trip\n");
  return 1;
}
