/* media.c — see media.h. A file decoder (WAV/MPEG -> 48 kHz/S16/stereo) and a
 * software mixer thread that sums active sources into the one kernel PCM
 * stream.
 *
 * The decode/resample core mirrors /bin/player (decode_next + a 16.16
 * fixed-point resampler): the file is pulled in small reads so a long song
 * never sits in memory, and a decode fault is a contained user-space crash,
 * never a kernel hang. The mixer adds the part the kernel can't: the device
 * offers a single exclusive stream, so two sounds at once must be blended
 * before audio_write — that blend happens here.
 */
#include "media.h"
#include "aac.h"
#include "alac.h"
#include "mp3.h"
#include "mp4.h"
#include "owos.h"

/* ---- small spinlock (mirrors the user malloc/blk_lock pattern) ------------
 */
static inline int xchg32(volatile int *p, int v)
{
  __asm__ volatile("xchgl %0, %1" : "+r"(v), "+m"(*p) : : "memory");
  return v;
}

static volatile int g_lock;
static void mlock(void)
{
  while (xchg32(&g_lock, 1))
    msleep(0);
}
static void munlock(void)
{
  __asm__ volatile("" ::: "memory");
  g_lock = 0;
}

/* ---- decoder ------------------------------------------------------------- */
struct media_decoder {
  int is_mp3;
  int is_alac;
  int is_aac;
  char path[256];
  unsigned size; /* whole-file bytes                         */
  unsigned rate; /* native sample rate, Hz                   */

  /* AAC (ADTS .aac streamed, or .m4a/.mp4 demuxed) decoded one AU at a time */
  aac_decoder *aac;
  unsigned aac_channels;
  short *aac_pcm; /* one decoded AU, interleaved int16        */
  unsigned aac_have, aac_pos;
  unsigned char *aac_buf; /* ADTS sliding input window                */
  unsigned aac_bufcap, aac_buflen, aac_bufpos;
  unsigned aac_fileoff; /* next file byte for the ADTS window       */
  int aac_src_eof;
  int aac_is_mp4;
  unsigned char *mp4_data; /* whole-file buffer (mp4 needs random I/O) */
  mp4 *mp4h;
  const mp4_track *aac_trk;
  unsigned aac_samp, aac_nsamp;

  /* WAV cursor (PCM s8/s16, mono/stereo) */
  unsigned w_data_off, w_data_len, w_bits, w_channels, w_frame;
  unsigned w_filepos;        /* byte cursor into the data region         */
  unsigned char w_in[65536]; /* raw source PCM, refilled by readfile_at  */
  unsigned w_inlen, w_inpos;

  mp3_t m3; /* MPEG decoder state                       */

  /* ALAC (.caf) cursor: one packet decoded to native PCM at a time */
  alac_decoder *a_dec;
  alac_config a_cfg;
  unsigned *a_pkt_size; /* per-packet byte sizes (malloc'd)         */
  unsigned a_num_packets;
  unsigned a_pkt;         /* next packet index to decode              */
  unsigned a_pkt_off;     /* file offset of packet a_pkt              */
  unsigned a_data_off;    /* file offset of the first packet          */
  unsigned a_bpf;         /* native bytes per interleaved frame       */
  unsigned char *a_in;    /* one raw packet (malloc'd)                */
  unsigned char *a_pcm;   /* one decoded packet, native PCM (malloc'd)*/
  unsigned a_have, a_pos; /* frames decoded in a_pcm, and the cursor  */

  /* shared decode/resample cursor */
  short curL, curR; /* current (held) source frame              */
  int eof;
  unsigned pos_frames;   /* native source frames consumed (the clock)*/
  unsigned total_frames; /* WAV: exact; MPEG: 0 (estimated instead)  */
  unsigned step, acc;    /* 16.16 input-frames per output-frame      */
  int primed;
};

static unsigned rd_le32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }
static unsigned rd_le16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned rd_be32(const unsigned char *p) { return ((unsigned)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static unsigned long long rd_be64(const unsigned char *p)
{
  return ((unsigned long long)rd_be32(p) << 32) | rd_be32(p + 4);
}
static double rd_bef64(const unsigned char *p)
{
  union {
    double f;
    unsigned long long u;
  } x;
  x.u = rd_be64(p);
  return x.f;
}
static int tageq(const unsigned char *p, const char *s)
{
  return p[0] == s[0] && p[1] == s[1] && p[2] == s[2] && p[3] == s[3];
}

/* Slide unconsumed ADTS bytes to the front of the window and top it up from the
 * file. Bounds the largest peekable frame to the window capacity. */
static void aac_refill_adts(media_decoder *d)
{
  if (d->aac_bufpos > 0) {
    unsigned rem = d->aac_buflen - d->aac_bufpos;
    memmove(d->aac_buf, d->aac_buf + d->aac_bufpos, rem);
    d->aac_buflen = rem;
    d->aac_bufpos = 0;
  }
  while (d->aac_buflen < d->aac_bufcap && !d->aac_src_eof) {
    int n = readfile_at(d->path, d->aac_fileoff, d->aac_buf + d->aac_buflen, d->aac_bufcap - d->aac_buflen);
    if (n <= 0) {
      d->aac_src_eof = 1;
      break;
    }
    d->aac_fileoff += (unsigned)n;
    d->aac_buflen += (unsigned)n;
  }
}

/* Decode the next access unit into d->aac_pcm; returns 1 on success, 0 at EOS.
 */
static int aac_next_au(media_decoder *d)
{
  uint32_t ns = 0, nc = 0;

  if (d->aac_is_mp4) {
    const unsigned char *data;
    unsigned sz = 0;
    if (d->aac_samp >= d->aac_nsamp)
      return 0;
    data = mp4_sample_data(d->mp4h, d->aac_trk, d->aac_samp, &sz);
    d->aac_samp++;
    if (!data || sz == 0)
      return 0;
    if (aac_decode(d->aac, data, sz, d->aac_pcm, &ns, &nc) != AAC_OK || ns == 0)
      return 0;
    d->aac_have = ns;
    d->aac_pos = 0;
    d->aac_channels = nc;
    return 1;
  }

  /* ADTS: make sure a whole frame is buffered, then decode it. */
  for (;;) {
    unsigned avail = d->aac_buflen - d->aac_bufpos;
    const unsigned char *p = d->aac_buf + d->aac_bufpos;
    unsigned flen;
    uint32_t consumed = 0;

    if (avail < 7) {
      if (d->aac_src_eof && avail < 7)
        return 0;
      aac_refill_adts(d);
      if (d->aac_buflen < 7)
        return 0;
      continue;
    }
    /* resync to the 0xFFFx sync word if the window drifted */
    if (p[0] != 0xFF || (p[1] & 0xF6) != 0xF0) {
      d->aac_bufpos++;
      continue;
    }
    flen = ((unsigned)(p[3] & 3) << 11) | ((unsigned)p[4] << 3) | (p[5] >> 5);
    if (flen < 7) {
      d->aac_bufpos++;
      continue;
    }
    if (avail < flen) {
      if (d->aac_src_eof)
        return 0;
      aac_refill_adts(d);
      continue;
    }
    if (aac_decode_adts(d->aac, p, avail, &consumed, d->aac_pcm, &ns, &nc) != AAC_OK || consumed == 0) {
      d->aac_bufpos++; /* skip a byte and try to resync */
      continue;
    }
    d->aac_bufpos += consumed;
    if (ns == 0)
      continue;
    d->aac_have = ns;
    d->aac_pos = 0;
    d->aac_channels = nc;
    return 1;
  }
}

static void decode_next(media_decoder *d)
{
  if (d->eof) {
    d->curL = d->curR = 0;
    return;
  }

  if (d->is_alac) {
    if (d->a_pos >= d->a_have) { /* decode next packet */
      unsigned ps;
      uint32_t frames = 0;
      if (d->a_pkt >= d->a_num_packets) {
        d->eof = 1;
        d->curL = d->curR = 0;
        return;
      }
      ps = d->a_pkt_size[d->a_pkt];
      /* A truncated packet (e.g. a stereo frame carrying only one element)
       * leaves some channels unwritten; zero first so those play as silence
       * rather than leaking stale heap into the output. */
      memset(d->a_pcm, 0, (unsigned)d->a_cfg.frame_length * d->a_bpf);
      if (ps == 0 || ps > alac_max_packet_size(&d->a_cfg) ||
          (int)readfile_at(d->path, d->a_pkt_off, d->a_in, ps) < (int)ps ||
          alac_decode(d->a_dec, d->a_in, ps, d->a_pcm, &frames) != ALAC_OK || frames == 0) {
        d->eof = 1;
        d->curL = d->curR = 0;
        return;
      }
      d->a_have = frames;
      d->a_pos = 0;
      d->a_pkt++;
      d->a_pkt_off += ps;
    }
    {
      unsigned ch = d->a_cfg.num_channels, bits = d->a_cfg.bit_depth;
      int L, R;
      if (bits == 16) {
        short *p = (short *)d->a_pcm + (unsigned)d->a_pos * ch;
        L = p[0];
        R = (ch == 2) ? p[1] : L;
      } else if (bits == 32) { /* take top 16 bits */
        int *p = (int *)d->a_pcm + (unsigned)d->a_pos * ch;
        L = p[0] >> 16;
        R = (ch == 2) ? (p[1] >> 16) : L;
      } else { /* 20/24: 3-byte LE, top 16 bits */
        unsigned char *s = d->a_pcm + (unsigned)d->a_pos * ch * 3;
        L = (short)(s[1] | (s[2] << 8));
        R = (ch == 2) ? (short)(s[4] | (s[5] << 8)) : L;
      }
      d->curL = (short)L;
      d->curR = (short)R;
    }
    d->a_pos++;
    d->pos_frames++;
    return;
  }

  if (d->is_aac) {
    if (d->aac_pos >= d->aac_have) { /* decode the next AU */
      if (!aac_next_au(d)) {
        d->eof = 1;
        d->curL = d->curR = 0;
        return;
      }
    }
    {
      unsigned ch = d->aac_channels ? d->aac_channels : 1;
      const short *p = d->aac_pcm + d->aac_pos * ch;
      d->curL = p[0];
      d->curR = (ch >= 2) ? p[1] : p[0];
    }
    d->aac_pos++;
    d->pos_frames++;
    return;
  }

  if (d->is_mp3) {
    short f[2];
    if (mp3_read(&d->m3, f, 1) <= 0) {
      d->eof = 1;
      d->curL = d->curR = 0;
      return;
    }
    d->curL = f[0];
    d->curR = f[1];
    d->pos_frames++;
    return;
  }

  if (d->w_inpos + d->w_frame > d->w_inlen) { /* refill input buffer */
    if (d->w_filepos >= d->w_data_len) {
      d->eof = 1;
      d->curL = d->curR = 0;
      return;
    }
    unsigned want = d->w_data_len - d->w_filepos;
    if (want > sizeof d->w_in)
      want = sizeof d->w_in;
    want -= want % d->w_frame;
    if (want == 0)
      want = d->w_frame;
    int got = readfile_at(d->path, d->w_data_off + d->w_filepos, d->w_in, want);
    if (got <= 0 || (unsigned)got < d->w_frame) {
      d->eof = 1;
      d->curL = d->curR = 0;
      return;
    }
    d->w_inlen = (unsigned)got;
    d->w_inpos = 0;
    d->w_filepos += (unsigned)got;
  }

  const unsigned char *s = d->w_in + d->w_inpos;
  d->w_inpos += d->w_frame;
  if (d->w_bits == 8) {
    int l = ((int)s[0] - 128) << 8;
    d->curL = (short)l;
    d->curR = (short)((d->w_channels == 2) ? (((int)s[1] - 128) << 8) : l);
  } else {
    d->curL = (short)rd_le16(s);
    d->curR = (d->w_channels == 2) ? (short)rd_le16(s + 2) : d->curL;
  }
  d->pos_frames++;
}

int media_read(media_decoder *d, short *out, int frames)
{
  if (!d || frames <= 0)
    return 0;
  if (!d->primed) {
    decode_next(d);
    d->primed = 1;
    d->acc = 0;
  }
  if (d->eof)
    return 0;
  int produced = 0;
  while (produced < frames) {
    out[produced * 2 + 0] = d->curL;
    out[produced * 2 + 1] = d->curR;
    produced++;
    if (d->eof)
      break;
    d->acc += d->step;
    while (d->acc >= 0x10000u && !d->eof) {
      decode_next(d);
      d->acc -= 0x10000u;
    }
  }
  return produced;
}

/* MPEG total frames: extrapolate from bytes consumed vs. file size, 0 until
 * something has decoded. WAV is exact. */
static unsigned est_total_frames(const media_decoder *d)
{
  if (!d->is_mp3)
    return d->total_frames;
  if (d->pos_frames == 0 || d->m3.file_off == 0 || d->size == 0)
    return 0;
  unsigned elapsed = d->pos_frames / (d->rate ? d->rate : 1);
  if (elapsed == 0)
    return 0;
  unsigned bps = d->m3.file_off / elapsed; /* source bytes per second */
  if (bps == 0)
    return 0;
  return (d->size / bps) * (d->rate ? d->rate : 1);
}

unsigned media_position_ms(const media_decoder *d)
{
  if (!d || !d->rate)
    return 0;
  return (unsigned)((unsigned long long)d->pos_frames * 1000ull / d->rate);
}

unsigned media_duration_ms(const media_decoder *d)
{
  if (!d || !d->rate)
    return 0;
  unsigned tf = est_total_frames(d);
  return (unsigned)((unsigned long long)tf * 1000ull / d->rate);
}

void media_seek_ms(media_decoder *d, unsigned ms)
{
  if (!d || !d->rate)
    return;
  unsigned tgt = (unsigned)((unsigned long long)ms * d->rate / 1000ull); /* native frame */

  if (d->is_aac) {
    if (d->aac_is_mp4) {
      unsigned ts = d->aac_trk->timescale ? d->aac_trk->timescale : d->rate;
      long long pts = (long long)((unsigned long long)tgt * ts / (d->rate ? d->rate : 1));
      unsigned si = mp4_seek_sample(d->aac_trk, pts);
      d->aac_samp = si;
      d->pos_frames = si * 1024; /* ~1024 samples per AU        */
    } else {
      unsigned dur = media_duration_ms(d); /* 0 for raw ADTS -> to start */
      unsigned off = dur ? (unsigned)((unsigned long long)d->size * ms / dur) : 0;
      if (off > d->size)
        off = d->size;
      d->aac_fileoff = off;
      d->aac_buflen = d->aac_bufpos = 0;
      d->aac_src_eof = 0;
      d->pos_frames = tgt;
    }
    d->aac_have = d->aac_pos = 0;
  } else if (d->is_alac) {
    /* seek to a packet boundary (frame_length granularity, like MPEG's coarse
     * seek) */
    unsigned fl = d->a_cfg.frame_length ? d->a_cfg.frame_length : 4096;
    unsigned pkt = tgt / fl, i, off = d->a_data_off;
    if (pkt > d->a_num_packets)
      pkt = d->a_num_packets;
    for (i = 0; i < pkt; i++)
      off += d->a_pkt_size[i];
    d->a_pkt = pkt;
    d->a_pkt_off = off;
    d->a_have = d->a_pos = 0;
    d->pos_frames = pkt * fl;
  } else if (d->is_mp3) {
    unsigned dur = media_duration_ms(d);
    unsigned off = dur ? (unsigned)((unsigned long long)d->size * ms / dur) : 0;
    if (off > d->size)
      off = d->size;
    d->m3.file_off = off;
    d->m3.in_len = 0;
    d->m3.out_pos = d->m3.out_frames; /* force a refill on the next read */
    d->m3.src_eof = 0;
    d->pos_frames = tgt;
  } else {
    if (d->total_frames && tgt > d->total_frames)
      tgt = d->total_frames;
    d->w_filepos = tgt * d->w_frame;
    if (d->w_filepos > d->w_data_len)
      d->w_filepos = d->w_data_len - (d->w_frame ? d->w_data_len % d->w_frame : 0);
    d->w_inlen = d->w_inpos = 0;
    d->pos_frames = tgt;
  }
  d->eof = 0;
  d->primed = 0; /* re-prime curL/curR from the new position */
  d->acc = 0;
}

static int load_wav(media_decoder *d, unsigned size)
{
  unsigned char hdr[256];
  int got = readfile_at(d->path, 0, hdr, sizeof hdr);
  if (got < 44 || !tageq(hdr, "RIFF") || !tageq(hdr + 8, "WAVE"))
    return -1;

  unsigned fmt = 0, channels = 0, bits = 0, rate = 0, data_off = 0, data_len = 0;
  for (unsigned pos = 12; pos + 8 <= (unsigned)got;) {
    const unsigned char *c = hdr + pos;
    unsigned sz = rd_le32(c + 4);
    if (tageq(c, "fmt ")) {
      if (pos + 24 <= (unsigned)got && sz >= 16) { /* fmt reads up to c+23 */
        fmt = rd_le16(c + 8);
        channels = rd_le16(c + 10);
        rate = rd_le32(c + 12);
        bits = rd_le16(c + 22);
      }
    } else if (tageq(c, "data")) {
      data_off = pos + 8;
      data_len = sz;
      break;
    }
    unsigned long long next = (unsigned long long)pos + 8 + sz + (sz & 1);
    if (next <= pos || next > (unsigned)got)
      break; /* untrusted sz: must advance and stay in buffer */
    pos = (unsigned)next;
  }
  if (!data_off || fmt != 1 || (bits != 8 && bits != 16) || channels < 1 || channels > 2)
    return -1;
  if (data_len > size - data_off) /* subtraction form: the add could wrap */
    data_len = size - data_off;

  d->is_mp3 = 0;
  d->size = size;
  d->rate = rate;
  d->w_data_off = data_off;
  d->w_data_len = data_len;
  d->w_filepos = 0;
  d->w_bits = bits;
  d->w_channels = channels;
  d->w_frame = channels * (bits / 8);
  d->w_inlen = d->w_inpos = 0;
  d->total_frames = d->w_frame ? d->w_data_len / d->w_frame : 0;
  return 0;
}

static int load_mp3(media_decoder *d, unsigned size)
{
  if (mp3_open(&d->m3, d->path, size) != 0)
    return -1;
  d->is_mp3 = 1;
  d->size = size;
  d->rate = mp3_rate(&d->m3);
  d->total_frames = 0; /* unknown for MPEG */
  return 0;
}

/* CAF packet-table variable-length integer (base-128, big-endian, 0x80 = more).
 */
static unsigned caf_varint(const unsigned char *p, const unsigned char *end, int *used)
{
  unsigned v = 0;
  int n = 0;
  unsigned char b;
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

/* Read a whole file region into a fresh buffer (for the small CAF chunks). */
static unsigned char *read_range(const char *path, unsigned off, unsigned len)
{
  unsigned char *b = malloc(len ? len : 1);
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

/* Apple Lossless in a CAF container: walk the chunk list for
 * desc/kuki/pakt/data, build the packet-size table, and open the codec. Packets
 * are then pulled and decoded one at a time by decode_next (a long song never
 * sits in memory). */
static int load_alac(media_decoder *d, unsigned size)
{
  unsigned char hdr[8];
  unsigned pos, valid = 0;
  int have_desc = 0, have_kuki = 0, have_pakt = 0;

  if (readfile_at(d->path, 0, hdr, 8) < 8 || !tageq(hdr, "caff"))
    goto fail;
  pos = 8;
  while (pos + 12 <= size) {
    unsigned char ch[12];
    unsigned long long sz;
    unsigned body;
    if (readfile_at(d->path, pos, ch, 12) < 12)
      goto fail;
    sz = rd_be64(ch + 4);
    body = pos + 12;
    if (tageq(ch, "desc")) {
      unsigned char de[32];
      if (sz < 32 || readfile_at(d->path, body, de, 32) < 32)
        goto fail;
      if (!tageq(de + 8, "alac"))
        goto fail;
      d->a_cfg.sample_rate = (unsigned)rd_bef64(de);
      d->a_cfg.frame_length = rd_be32(de + 20);
      d->a_cfg.num_channels = rd_be32(de + 24);
      have_desc = 1;
    } else if (tageq(ch, "kuki")) {
      unsigned char *k = read_range(d->path, body, (unsigned)sz);
      int rc;
      if (!k)
        goto fail;
      rc = alac_parse_cookie(k, (unsigned)sz, &d->a_cfg);
      free(k);
      if (rc != ALAC_OK)
        goto fail;
      have_kuki = 1;
    } else if (tageq(ch, "pakt")) {
      unsigned char *pk = read_range(d->path, body, (unsigned)sz), *p, *end;
      unsigned i;
      if (!pk || sz < 24) {
        free(pk);
        goto fail;
      }
      d->a_num_packets = (unsigned)rd_be64(pk + 0);
      valid = (unsigned)rd_be64(pk + 8);
      /* a_num_packets is untrusted: reject counts that overflow the size-
       * table alloc, or exceed the chunk (each varint is >= 1 byte, table
       * starts at pk+24). */
      if (d->a_num_packets > (0xFFFFFFFFu / sizeof(unsigned)) || d->a_num_packets > (sz - 24)) {
        free(pk);
        goto fail;
      }
      d->a_pkt_size = malloc((d->a_num_packets ? d->a_num_packets : 1) * sizeof(unsigned));
      if (!d->a_pkt_size) {
        free(pk);
        goto fail;
      }
      p = pk + 24;
      end = pk + sz;
      for (i = 0; i < d->a_num_packets; i++) {
        int used;
        d->a_pkt_size[i] = caf_varint(p, end, &used);
        if (!used) {
          free(pk);
          goto fail;
        }
        p += used;
      }
      free(pk);
      have_pakt = 1;
    } else if (tageq(ch, "data")) {
      d->a_data_off = body + 4; /* skip 4-byte mEditCount */
    }
    if (sz == (unsigned long long)-1)
      break; /* data-to-EOF sentinel */
    unsigned long long next = (unsigned long long)body + sz;
    if (next <= pos || next > size)
      break; /* untrusted sz: must advance and stay in file */
    pos = (unsigned)next;
  }
  if (!have_desc || !have_kuki || !have_pakt || !d->a_data_off)
    goto fail;
  if (d->a_cfg.num_channels < 1 || d->a_cfg.num_channels > 2)
    goto fail;

  d->a_dec = alac_decoder_open(&d->a_cfg);
  d->a_bpf = alac_bytes_per_frame(&d->a_cfg);
  d->a_in = malloc(alac_max_packet_size(&d->a_cfg));
  d->a_pcm = malloc(d->a_cfg.frame_length * d->a_bpf);
  if (!d->a_dec || !d->a_in || !d->a_pcm)
    goto fail;

  d->is_alac = 1;
  d->size = size;
  d->rate = d->a_cfg.sample_rate;
  d->total_frames = valid;
  d->a_pkt = 0;
  d->a_pkt_off = d->a_data_off;
  d->a_have = d->a_pos = 0;
  return 0;

  /* A malformed .caf can fail after partial allocation; free it here so
   * media_open only has to free `d` (pointers are zeroed so media_close stays
   * double-free safe). */
fail:
  if (d->a_dec)
    alac_decoder_close(d->a_dec);
  free(d->a_pkt_size);
  free(d->a_in);
  free(d->a_pcm);
  d->a_dec = 0;
  d->a_pkt_size = 0;
  d->a_in = 0;
  d->a_pcm = 0;
  return -1;
}

/* Raw ADTS AAC: open a streaming decoder and prime the first access unit so the
 * native rate/channels are known (a long song is pulled through a small
 * window). */
static int load_aac_adts(media_decoder *d, unsigned size)
{
  aac_info info;
  d->aac = aac_open_adts();
  if (!d->aac)
    return -1;
  d->aac_bufcap = 16384;
  d->aac_buf = malloc(d->aac_bufcap);
  d->aac_pcm = malloc((unsigned)1024 * AAC_MAX_CHANNELS * sizeof(short));
  if (!d->aac_buf || !d->aac_pcm)
    goto fail;
  d->aac_is_mp4 = 0;
  d->aac_fileoff = d->aac_buflen = d->aac_bufpos = 0;
  d->aac_src_eof = 0;
  aac_refill_adts(d);
  if (!aac_next_au(d))
    goto fail; /* prime + learn the format    */
  aac_get_info(d->aac, &info);
  d->rate = info.sample_rate;
  d->size = size;
  d->total_frames = 0; /* raw ADTS has no duration    */
  d->is_aac = 1;
  return 0;
fail:
  if (d->aac)
    aac_close(d->aac);
  free(d->aac_buf);
  free(d->aac_pcm);
  d->aac = 0;
  d->aac_buf = 0;
  d->aac_pcm = 0;
  return -1;
}

/* AAC in an MP4/.m4a container: demux the whole file, take the AAC audio track,
 * and configure the decoder from its AudioSpecificConfig. Samples (access
 * units) are then pulled one at a time by decode_next. */
static int load_aac_mp4(media_decoder *d, unsigned size)
{
  aac_info info;
  const mp4_track *t;
  d->mp4_data = read_range(d->path, 0, size);
  if (!d->mp4_data)
    return -1;
  if (mp4_open(d->mp4_data, size, &d->mp4h) != MP4_OK)
    goto fail;
  t = mp4_audio_track(d->mp4h);
  if (!t || t->codec != MP4_CODEC_AAC || !t->extradata || t->extradata_len == 0)
    goto fail;
  d->aac = aac_open(t->extradata, t->extradata_len, &info);
  if (!d->aac)
    goto fail;
  d->aac_pcm = malloc((unsigned)1024 * AAC_MAX_CHANNELS * sizeof(short));
  if (!d->aac_pcm)
    goto fail;
  d->aac_is_mp4 = 1;
  d->aac_trk = t;
  d->aac_nsamp = t->n_samples;
  d->aac_samp = 0;
  d->aac_channels = info.channels;
  d->rate = info.sample_rate ? info.sample_rate : (t->samplerate ? t->samplerate : 44100);
  d->size = size;
  if (t->timescale)
    d->total_frames = (unsigned)((unsigned long long)t->duration * d->rate / t->timescale);
  else
    d->total_frames = t->n_samples * 1024;
  d->is_aac = 1;
  return 0;
fail:
  if (d->aac)
    aac_close(d->aac);
  if (d->mp4h)
    mp4_close(d->mp4h);
  free(d->mp4_data);
  free(d->aac_pcm);
  d->aac = 0;
  d->mp4h = 0;
  d->mp4_data = 0;
  d->aac_pcm = 0;
  return -1;
}

media_decoder *media_open(const char *path)
{
  struct statinfo st;
  int i;
  if (!path || stat(path, &st) != 0 || st.is_dir)
    return 0;

  media_decoder *d = (media_decoder *)malloc(sizeof *d);
  if (!d)
    return 0;
  memset(d, 0, sizeof *d);
  for (i = 0; i < (int)sizeof d->path - 1 && path[i]; i++)
    d->path[i] = path[i];
  d->path[i] = '\0';

  unsigned char sn[12];
  int g = readfile_at(d->path, 0, sn, sizeof sn);
  int rc;
  if (g >= 12 && tageq(sn, "RIFF") && tageq(sn + 8, "WAVE"))
    rc = load_wav(d, st.size);
  else if (g >= 8 && tageq(sn, "caff"))
    rc = load_alac(d, st.size);
  else if (g >= 8 && tageq(sn + 4, "ftyp")) /* .m4a / .mp4 (AAC)   */
    rc = load_aac_mp4(d, st.size);
  else if (g >= 2 && sn[0] == 0xFF && (sn[1] & 0xF6) == 0xF0) /* ADTS .aac    */
    rc = load_aac_adts(d, st.size);
  else
    rc = load_mp3(d, st.size);
  if (rc != 0) {
    free(d);
    return 0;
  }

  if (d->rate < 4000)
    d->rate = 4000;
  if (d->rate > 192000)
    d->rate = 192000;
  /* 16.16 input-frames per output-frame at the device's 48 kHz. */
  d->step = ((d->rate / MEDIA_RATE) << 16) + (((d->rate % MEDIA_RATE) << 16) / MEDIA_RATE);
  d->pos_frames = 0;
  d->eof = 0;
  d->primed = 0;
  return d;
}

void media_close(media_decoder *d)
{
  if (!d)
    return;
  if (d->is_mp3)
    mp3_close(&d->m3);
  if (d->is_alac) {
    alac_decoder_close(d->a_dec);
    free(d->a_pkt_size);
    free(d->a_in);
    free(d->a_pcm);
  }
  if (d->is_aac) {
    aac_close(d->aac);
    if (d->mp4h)
      mp4_close(d->mp4h);
    free(d->mp4_data);
    free(d->aac_buf);
    free(d->aac_pcm);
  }
  free(d);
}

/* ---- software mixer ------------------------------------------------------ */
#define MIX_FRAMES 1024 /* ~21 ms per chunk at 48 kHz              */
#define MAX_SOURCES 16

struct media_source {
  media_decoder *dec;
  volatile int in_use;       /* slot is live (set last by _new)         */
  volatile int free_pending; /* glue asked to drop it; mixer reaps      */
  volatile int playing;      /* desired play state (glue sets)          */
  volatile int ended;        /* mixer hit EOS, not looping              */
  volatile int loop;
  volatile int vol; /* 0..256                                  */
  volatile unsigned seek_ms;
  volatile int seek_pending;
  volatile unsigned pos_ms; /* mixer publishes                         */
  volatile unsigned dur_ms; /* mixer publishes                         */
  media_end_cb on_end;
  void *user;
  volatile int end_fired; /* on_end already called once              */
};

static struct media_source g_src[MAX_SOURCES];
static volatile int g_started;
static int g_stream_open; /* mixer-thread-only        */

static int g_acc[MIX_FRAMES * MEDIA_CH];   /* mix accumulators         */
static short g_tmp[MIX_FRAMES * MEDIA_CH]; /* one source's decode      */
static short g_out[MIX_FRAMES * MEDIA_CH]; /* clamped output           */
static unsigned char g_mixer_stack[131072];

/* Pull a full MIX_FRAMES chunk from one source into g_tmp, honoring loop.
 * Returns frames filled (0 = the source is out of audio). */
static int fill_source(media_source *s)
{
  int filled = 0, dry = 0;
  while (filled < MIX_FRAMES) {
    int got = media_read(s->dec, g_tmp + filled * MEDIA_CH, MIX_FRAMES - filled);
    if (got <= 0) {
      if (s->loop && dry++ < 2) {
        media_seek_ms(s->dec, 0);
        continue;
      }
      break;
    }
    filled += got;
  }
  return filled;
}

static void mixer_thread(void)
{
  for (;;) {
    int i, max_filled = 0, want = 0;

    /* Reap freed slots and publish clocks for every live source. */
    mlock();
    for (i = 0; i < MAX_SOURCES; i++) {
      media_source *s = &g_src[i];
      if (s->free_pending) {
        media_close(s->dec);
        memset(s, 0, sizeof *s);
        continue;
      }
      if (!s->in_use)
        continue;
      s->pos_ms = media_position_ms(s->dec);
      s->dur_ms = media_duration_ms(s->dec);
      if (s->playing && !s->ended)
        want = 1;
    }
    munlock();

    if (!want) { /* nothing to play */
      if (g_stream_open) {
        audio_close();
        g_stream_open = 0;
      }
      msleep(15);
      continue;
    }
    if (!g_stream_open) {
      if (audio_open() != 0) {
        msleep(25);
        continue;
      } /* no device yet */
      g_stream_open = 1;
    }

    memset(g_acc, 0, sizeof g_acc);
    for (i = 0; i < MAX_SOURCES; i++) {
      media_source *s = &g_src[i];
      int filled, f, vol;
      if (!s->in_use || s->free_pending)
        continue;
      if (s->seek_pending) {
        media_seek_ms(s->dec, s->seek_ms);
        s->seek_pending = 0;
        s->ended = 0;
        s->end_fired = 0;
      }
      if (!s->playing || s->ended)
        continue;

      filled = fill_source(s);
      s->pos_ms = media_position_ms(s->dec);
      s->dur_ms = media_duration_ms(s->dec);
      if (filled == 0) { /* end of stream */
        s->ended = 1;
        if (s->on_end && !s->end_fired) {
          s->end_fired = 1;
          s->on_end(s->user);
        }
        continue;
      }
      vol = s->vol;
      for (f = 0; f < filled * MEDIA_CH; f++)
        g_acc[f] += (g_tmp[f] * vol) >> 8;
      if (filled > max_filled)
        max_filled = filled;
    }

    if (max_filled == 0) {
      msleep(5);
      continue;
    }

    for (i = 0; i < max_filled * MEDIA_CH; i++) {
      int v = g_acc[i];
      if (v > 32767)
        v = 32767;
      else if (v < -32768)
        v = -32768;
      g_out[i] = (short)v;
    }
    /* audio_write may accept fewer bytes than offered; feed the remainder
     * rather than dropping the tail. w <= 0 is a device error / Ctrl+C latch.
     */
    int total = max_filled * MEDIA_CH * 2, off = 0;
    while (off < total) {
      int w = audio_write((unsigned char *)g_out + off, total - off);
      if (w <= 0) {
        audio_close();
        g_stream_open = 0;
        msleep(10);
        break;
      }
      off += w;
    }
  }
}

void media_mixer_start(void)
{
  mlock();
  if (!g_started) {
    g_started = 1;
    thread_create(mixer_thread, &g_mixer_stack[sizeof g_mixer_stack]);
  }
  munlock();
}

media_source *media_source_new(media_decoder *d, media_end_cb on_end, void *user)
{
  int i;
  media_source *slot = 0;
  if (!d)
    return 0;
  media_mixer_start();
  mlock();
  for (i = 0; i < MAX_SOURCES; i++) {
    if (!g_src[i].in_use && !g_src[i].free_pending) {
      slot = &g_src[i];
      break;
    }
  }
  if (slot) {
    slot->dec = d;
    slot->playing = 0;
    slot->ended = 0;
    slot->loop = 0;
    slot->vol = 256;
    slot->seek_pending = 0;
    slot->pos_ms = 0;
    slot->dur_ms = media_duration_ms(d);
    slot->on_end = on_end;
    slot->user = user;
    slot->end_fired = 0;
    slot->free_pending = 0;
    __asm__ volatile("" ::: "memory");
    slot->in_use = 1;
  }
  munlock();
  return slot;
}

void media_source_free(media_source *s)
{
  if (!s)
    return;
  s->playing = 0;
  __asm__ volatile("" ::: "memory");
  s->free_pending = 1;
}

void media_source_play(media_source *s)
{
  if (s)
    s->playing = 1;
}
void media_source_pause(media_source *s)
{
  if (s)
    s->playing = 0;
}
void media_source_set_loop(media_source *s, int loop)
{
  if (s)
    s->loop = loop ? 1 : 0;
}

void media_source_set_volume(media_source *s, int vol256)
{
  if (!s)
    return;
  if (vol256 < 0)
    vol256 = 0;
  else if (vol256 > 256)
    vol256 = 256;
  s->vol = vol256;
}

void media_source_seek_ms(media_source *s, unsigned ms)
{
  if (!s)
    return;
  s->seek_ms = ms;
  s->pos_ms = ms;
  s->ended = 0;
  __asm__ volatile("" ::: "memory");
  s->seek_pending = 1;
}

int media_source_playing(const media_source *s) { return s ? (s->playing && !s->ended) : 0; }
int media_source_ended(const media_source *s) { return s ? s->ended : 0; }
unsigned media_source_position_ms(const media_source *s) { return s ? s->pos_ms : 0; }
unsigned media_source_duration_ms(const media_source *s) { return s ? s->dur_ms : 0; }
