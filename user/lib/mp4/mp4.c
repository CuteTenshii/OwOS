/* mp4.c — see mp4.h. An ISO base media file format demuxer.
 *
 * Two passes over the box tree. The first walks moov -> trak -> mdia -> minf ->
 * stbl and records, per track, a pointer to each sample table (stts/stsc/stsz/
 * stco/...) plus the codec configuration — never copying, just locating. The
 * second expands those tables into one flat mp4_sample array per track (the
 * stsc/stco chunk map crossed with the stsz size table gives every sample's
 * file offset; stts/ctts give timestamps; stss marks keyframes).
 *
 * Every field read is bounds-checked against the enclosing box and the file:
 * the input is untrusted, and the whole point of doing this in user space is
 * that a crafted file faults this process and nothing else.
 */
#include "mp4.h"
#include "owos.h"

/* ---- big-endian readers (all MP4 integers are big-endian) ---------------- */
static unsigned r16(const unsigned char *p) { return (unsigned)(p[0] << 8) | p[1]; }
static unsigned r24(const unsigned char *p) { return ((unsigned)p[0] << 16) | (p[1] << 8) | p[2]; }
static unsigned r32(const unsigned char *p)
{
  return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3];
}
static unsigned long long r64(const unsigned char *p) { return ((unsigned long long)r32(p) << 32) | r32(p + 4); }
static int tag4(const unsigned char *p, const char *s)
{
  return p[0] == (unsigned char)s[0] && p[1] == (unsigned char)s[1] && p[2] == (unsigned char)s[2] &&
         p[3] == (unsigned char)s[3];
}

/* Per-track scratch: pointers to the raw stbl tables (into the file buffer) and
 * their entry counts, filled during the walk and consumed by build_samples. */
typedef struct {
  int present; /* a trak with a handler we understood      */
  int is_video, is_audio;
  mp4_codec codec;
  unsigned timescale;
  unsigned long long duration;
  long long edit_start; /* first non-empty edit's media_time */
  unsigned width, height;
  unsigned channels, samplerate, bits;
  const unsigned char *extradata;
  unsigned extradata_len;
  unsigned nal_length_size;

  const unsigned char *stts;
  unsigned stts_n; /* {count,delta} x N       */
  const unsigned char *ctts;
  unsigned ctts_n;
  int ctts_v1; /* {count,off} xN */
  const unsigned char *stsc;
  unsigned stsc_n; /* {first_chunk,spc,sdi}xN */
  const unsigned char *stsz;
  unsigned stsz_n;
  unsigned stsz_const; /* uint32 xN */
  const unsigned char *stz2;
  unsigned stz2_n;
  unsigned stz2_field; /* 4/8/16b  */
  const unsigned char *stco;
  unsigned stco_n;
  int co64; /* chunk offsets      */
  const unsigned char *stss;
  unsigned stss_n; /* sync sample numbers      */
} trackbuild;

struct mp4 {
  const unsigned char *buf;
  unsigned len;
  mp4_track *tracks;
  int n_tracks;
  int vtrack, atrack; /* indices, or -1                     */
};

#define MAX_TRACKS 8

/* ---- box iteration --------------------------------------------------------
 */
/* A cursor over the children of a container spanning [p, end). box_next
 * advances it, yielding the current box's type, body range [body, bend), or 0
 * at the end / on a malformed length. */
typedef struct {
  const unsigned char *p, *end;
} boxiter;

typedef struct {
  const unsigned char *type;        /* 4-byte fourcc                          */
  const unsigned char *body, *bend; /* payload range                          */
} box;

static int box_next(boxiter *it, box *b)
{
  const unsigned char *p = it->p;
  if (p + 8 > it->end)
    return 0;
  unsigned long long sz = r32(p);
  unsigned hdr = 8;
  if (sz == 1) { /* 64-bit largesize          */
    if (p + 16 > it->end)
      return 0;
    sz = r64(p + 8);
    hdr = 16;
  } else if (sz == 0) { /* extends to container end  */
    sz = (unsigned long long)(it->end - p);
  }
  if (sz < hdr || p + sz > it->end)
    return 0;
  b->type = p + 4;
  b->body = p + hdr;
  b->bend = p + sz;
  it->p = p + sz;
  return 1;
}

/* A fullbox exposes a 1-byte version + 3-byte flags at its head. */
static int fullbox(const box *b, unsigned *version, unsigned *flags)
{
  if (b->body + 4 > b->bend)
    return 0;
  *version = b->body[0];
  *flags = r24(b->body + 1);
  return 1;
}

/* Validate that `count` entries of `stride` bytes fit in [entries, bend);
 * returns entries on success, else NULL (a crafted count can't over-read). */
static const unsigned char *entries_ok(const unsigned char *entries, const unsigned char *bend, unsigned count,
                                       unsigned stride)
{
  unsigned long long need = (unsigned long long)count * stride;
  if (entries > bend)
    return 0;
  if (need > (unsigned long long)(bend - entries))
    return 0;
  return entries;
}

/* ---- esds (audio codec config) parsing ----------------------------------- */
/* MPEG-4 descriptors: a 1-byte tag, an expandable base-128 length, then the
 * body. We only need the ES_Descriptor -> DecoderConfigDescriptor ->
 * object-type and the DecoderSpecificInfo (the AudioSpecificConfig we hand back
 * as extradata). */
static unsigned desc_len(const unsigned char **pp, const unsigned char *end)
{
  const unsigned char *p = *pp;
  unsigned v = 0;
  int i;
  for (i = 0; i < 4 && p < end; i++) {
    unsigned char b = *p++;
    v = (v << 7) | (b & 0x7f);
    if (!(b & 0x80))
      break;
  }
  *pp = p;
  return v;
}

static void parse_esds(trackbuild *tb, const unsigned char *p, const unsigned char *end)
{
  unsigned version, flags;
  if (p + 4 > end)
    return;
  version = p[0];
  flags = r24(p + 1);
  (void)version;
  (void)flags;
  p += 4; /* esds is a fullbox        */
  if (p >= end || *p++ != 0x03)
    return; /* ES_Descriptor tag        */
  desc_len(&p, end);
  if (p + 3 > end)
    return;
  p += 3; /* ES_ID(2) + flags(1)      */
  if (p >= end || *p++ != 0x04)
    return; /* DecoderConfigDescriptor  */
  desc_len(&p, end);
  if (p + 13 > end)
    return;
  {
    unsigned char obj = p[0]; /* objectTypeIndication     */
    if (obj == 0x40)
      tb->codec = MP4_CODEC_AAC;
    else if (obj == 0x66 || obj == 0x67 || obj == 0x68 || obj == 0x69 || obj == 0x6B)
      tb->codec = MP4_CODEC_MP3;
  }
  p += 13; /* config header fields     */
  if (p >= end || *p++ != 0x05)
    return; /* DecoderSpecificInfo      */
  {
    unsigned n = desc_len(&p, end);
    if (p + n <= end && n > 0) {
      tb->extradata = p;
      tb->extradata_len = n;
    }
  }
}

/* ---- avcC (H.264 config) --------------------------------------------------
 */
static void parse_avcc(trackbuild *tb, const unsigned char *p, const unsigned char *end)
{
  if (p + 5 > end)
    return;
  tb->nal_length_size = (p[4] & 3) + 1; /* lengthSizeMinusOne + 1   */
  tb->extradata = p;
  tb->extradata_len = (unsigned)(end - p);
}

/* ---- sample-entry (stsd) parsing ----------------------------------------- */
static void parse_stsd(trackbuild *tb, const box *b)
{
  unsigned version, flags, count;
  const unsigned char *p;
  if (!fullbox(b, &version, &flags))
    return;
  p = b->body + 4;
  if (p + 4 > b->bend)
    return;
  count = r32(p);
  p += 4;
  if (count == 0)
    return;

  /* Only the first sample entry is read; each entry is itself a box. */
  {
    boxiter it = {p, b->bend};
    box e;
    if (!box_next(&it, &e))
      return;
    const unsigned char *ep = e.body; /* entry payload after size+type */
    if (tag4(e.type, "avc1") || tag4(e.type, "avc3"))
      tb->codec = MP4_CODEC_H264;
    else if (tag4(e.type, "hvc1") || tag4(e.type, "hev1"))
      tb->codec = MP4_CODEC_H265;
    else if (tag4(e.type, "mp4v"))
      tb->codec = MP4_CODEC_MPEG4;
    else if (tag4(e.type, "mp4a"))
      tb->codec = MP4_CODEC_AAC;

    if (tb->is_video) {
      /* VisualSampleEntry, offsets from the entry payload (past size+type):
       * 6 reserved + 2 data_ref + 16 pre_defined, then width(2) height(2) at
       * +24/+26; the codec sub-boxes (avcC/...) begin at +78. */
      if (ep + 28 <= e.bend) {
        tb->width = r16(ep + 24);
        tb->height = r16(ep + 26);
      }
      if (ep + 78 <= e.bend) {
        boxiter si = {ep + 78, e.bend};
        box sb;
        while (box_next(&si, &sb)) {
          if (tag4(sb.type, "avcC"))
            parse_avcc(tb, sb.body, sb.bend);
          else if (tag4(sb.type, "hvcC")) {
            tb->nal_length_size = (sb.bend - sb.body > 21) ? ((sb.body[21] & 3) + 1) : 4;
            tb->extradata = sb.body;
            tb->extradata_len = (unsigned)(sb.bend - sb.body);
          }
        }
      }
    } else if (tb->is_audio) {
      /* AudioSampleEntry, offsets from the entry payload: 6 reserved + 2
       * data_ref + 8 reserved, then channels(2) samplesize(2) at +16/+18 and
       * samplerate(4, 16.16) at +24; the esds sub-box begins at +28. */
      if (ep + 28 <= e.bend) {
        tb->channels = r16(ep + 16);
        tb->bits = r16(ep + 18);
        tb->samplerate = r32(ep + 24) >> 16;
        boxiter si = {ep + 28, e.bend};
        box sb;
        while (box_next(&si, &sb)) {
          if (tag4(sb.type, "esds"))
            parse_esds(tb, sb.body, sb.bend);
        }
      }
    }
  }
}

/* ---- stbl child tables (locate, don't expand) ---------------------------- */
static void parse_stbl(trackbuild *tb, const box *stbl)
{
  boxiter it = {stbl->body, stbl->bend};
  box b;
  while (box_next(&it, &b)) {
    unsigned version, flags, n;
    const unsigned char *p;
    if (tag4(b.type, "stsd")) {
      parse_stsd(tb, &b);
      continue;
    }
    if (!fullbox(&b, &version, &flags))
      continue;
    p = b.body + 4;

    if (tag4(b.type, "stts")) {
      if (p + 4 > b.bend)
        continue;
      n = r32(p);
      p += 4;
      if ((tb->stts = entries_ok(p, b.bend, n, 8)))
        tb->stts_n = n;
    } else if (tag4(b.type, "ctts")) {
      if (p + 4 > b.bend)
        continue;
      n = r32(p);
      p += 4;
      if ((tb->ctts = entries_ok(p, b.bend, n, 8))) {
        tb->ctts_n = n;
        tb->ctts_v1 = (version == 1);
      }
    } else if (tag4(b.type, "stsc")) {
      if (p + 4 > b.bend)
        continue;
      n = r32(p);
      p += 4;
      if ((tb->stsc = entries_ok(p, b.bend, n, 12)))
        tb->stsc_n = n;
    } else if (tag4(b.type, "stsz")) {
      if (p + 8 > b.bend)
        continue;
      tb->stsz_const = r32(p);
      n = r32(p + 4);
      p += 8;
      tb->stsz_n = n;
      if (tb->stsz_const == 0) {
        if (!(tb->stsz = entries_ok(p, b.bend, n, 4)))
          tb->stsz_n = 0;
      } else {
        tb->stsz = p; /* constant size: no array  */
      }
    } else if (tag4(b.type, "stz2")) {
      if (p + 8 > b.bend)
        continue;
      tb->stz2_field = p[3]; /* reserved[3] + field_size */
      n = r32(p + 4);
      p += 8;
      {
        unsigned stride_bytes;
        if (tb->stz2_field == 16)
          stride_bytes = n * 2;
        else if (tb->stz2_field == 8)
          stride_bytes = n;
        else if (tb->stz2_field == 4)
          stride_bytes = (n + 1) / 2;
        else
          continue;
        if ((unsigned long long)stride_bytes <= (unsigned long long)(b.bend - p)) {
          tb->stz2 = p;
          tb->stz2_n = n;
        }
      }
    } else if (tag4(b.type, "stco")) {
      if (p + 4 > b.bend)
        continue;
      n = r32(p);
      p += 4;
      if ((tb->stco = entries_ok(p, b.bend, n, 4))) {
        tb->stco_n = n;
        tb->co64 = 0;
      }
    } else if (tag4(b.type, "co64")) {
      if (p + 4 > b.bend)
        continue;
      n = r32(p);
      p += 4;
      if ((tb->stco = entries_ok(p, b.bend, n, 8))) {
        tb->stco_n = n;
        tb->co64 = 1;
      }
    } else if (tag4(b.type, "stss")) {
      if (p + 4 > b.bend)
        continue;
      n = r32(p);
      p += 4;
      if ((tb->stss = entries_ok(p, b.bend, n, 4)))
        tb->stss_n = n;
    }
  }
}

/* ---- trak sub-tree --------------------------------------------------------
 */
static void parse_mdhd(trackbuild *tb, const box *b)
{
  unsigned version, flags;
  if (!fullbox(b, &version, &flags))
    return;
  if (version == 1) {
    if (b->body + 28 > b->bend)
      return;
    tb->timescale = r32(b->body + 20);
    tb->duration = r64(b->body + 24);
  } else {
    if (b->body + 20 > b->bend)
      return;
    tb->timescale = r32(b->body + 12);
    tb->duration = r32(b->body + 16);
  }
}

static void parse_minf(trackbuild *tb, const box *minf)
{
  boxiter it = {minf->body, minf->bend};
  box b;
  while (box_next(&it, &b))
    if (tag4(b.type, "stbl"))
      parse_stbl(tb, &b);
}

static void parse_mdia(trackbuild *tb, const box *mdia)
{
  boxiter it = {mdia->body, mdia->bend};
  box b;
  while (box_next(&it, &b)) {
    if (tag4(b.type, "mdhd"))
      parse_mdhd(tb, &b);
    else if (tag4(b.type, "hdlr")) {
      /* fullbox + pre_defined(4) + handler_type(4) fourcc */
      if (b.body + 12 <= b.bend) {
        const unsigned char *h = b.body + 8;
        if (tag4(h, "vide")) {
          tb->is_video = 1;
          tb->present = 1;
        } else if (tag4(h, "soun")) {
          tb->is_audio = 1;
          tb->present = 1;
        }
      }
    } else if (tag4(b.type, "minf"))
      parse_minf(tb, &b);
  }
}

/* edts/elst: the first edit that plays media (media_time >= 0) says how many
 * leading samples (encoder priming) are not part of the presentation. */
static void parse_edts(trackbuild *tb, const box *edts)
{
  boxiter it = {edts->body, edts->bend};
  box b;
  while (box_next(&it, &b)) {
    unsigned version, flags, n, esz;
    const unsigned char *e;
    if (!tag4(b.type, "elst") || !fullbox(&b, &version, &flags) || b.body + 8 > b.bend)
      continue;
    n = r32(b.body + 4);
    esz = version == 1 ? 20 : 12;
    e = b.body + 8;
    for (; n && e + esz <= b.bend; n--, e += esz) {
      long long mt = version == 1 ? (long long)r64(e + 8) : (long long)(int)r32(e + 4);
      if (mt >= 0) {
        tb->edit_start = mt;
        return;
      }
    }
  }
}

static void parse_trak(trackbuild *tb, const box *trak)
{
  boxiter it = {trak->body, trak->bend};
  box b;
  while (box_next(&it, &b)) {
    if (tag4(b.type, "mdia"))
      parse_mdia(tb, &b);
    else if (tag4(b.type, "edts"))
      parse_edts(tb, &b);
  }
}

/* ---- flat sample table --------------------------------------------------- */
/* samples-per-chunk for 1-based chunk number `chunk1`, from the stsc runs
 * (sorted ascending by first_chunk; the last run covers every remaining chunk).
 */
static unsigned stsc_spc(const trackbuild *tb, unsigned chunk1)
{
  unsigned e, spc = 0;
  for (e = 0; e < tb->stsc_n; e++) {
    unsigned fc = r32(tb->stsc + e * 12);
    if (fc <= chunk1)
      spc = r32(tb->stsc + e * 12 + 4);
    else
      break;
  }
  return spc;
}

static unsigned sample_size(const trackbuild *tb, unsigned i)
{
  if (tb->stsz)
    return tb->stsz_const ? tb->stsz_const : r32(tb->stsz + i * 4);
  if (tb->stz2) {
    if (tb->stz2_field == 16)
      return r16(tb->stz2 + i * 2);
    if (tb->stz2_field == 8)
      return tb->stz2[i];
    {
      unsigned char byte = tb->stz2[i / 2];
      return (i & 1) ? (byte & 0xF) : (byte >> 4);
    }
  }
  return 0;
}

static int build_samples(mp4 *m, const trackbuild *tb, mp4_track *t)
{
  unsigned n = tb->stsz ? tb->stsz_n : tb->stz2_n;
  unsigned num_chunks = tb->stco_n;
  mp4_sample *S;
  unsigned si, c, i;

  t->samples = 0;
  t->n_samples = 0;
  if (n == 0 || num_chunks == 0)
    return MP4_OK; /* an empty track is legal  */

  S = (mp4_sample *)calloc(n, sizeof *S);
  if (!S)
    return MP4_ENOMEM;

  /* Offsets: walk chunks, laying samples end-to-end within each chunk. */
  si = 0;
  for (c = 0; c < num_chunks && si < n; c++) {
    unsigned spc = stsc_spc(tb, c + 1), s;
    unsigned long long off = tb->co64 ? r64(tb->stco + c * 8) : r32(tb->stco + c * 4);
    for (s = 0; s < spc && si < n; s++) {
      unsigned sz = sample_size(tb, si);
      if (off > m->len)
        off = m->len;
      S[si].offset = (unsigned)off;
      if ((unsigned long long)S[si].offset + sz > m->len)
        sz = (S[si].offset <= m->len) ? (m->len - S[si].offset) : 0;
      S[si].size = sz;
      off += sz;
      si++;
    }
  }
  n = si; /* samples actually placed  */

  /* Decode timestamps from stts run-length {count, delta}. */
  {
    long long dts = 0;
    unsigned ei = 0, cnt = 0, delta = 0;
    for (i = 0; i < n; i++) {
      while (cnt == 0 && ei < tb->stts_n) {
        cnt = r32(tb->stts + ei * 8);
        delta = r32(tb->stts + ei * 8 + 4);
        ei++;
      }
      S[i].dts = dts;
      S[i].pts = dts;
      dts += delta;
      if (cnt > 0)
        cnt--;
    }
  }

  /* Composition offsets (B-frame reordering) from ctts {count, offset}. */
  if (tb->ctts) {
    unsigned ei = 0, cnt = 0;
    int off = 0;
    for (i = 0; i < n; i++) {
      while (cnt == 0 && ei < tb->ctts_n) {
        cnt = r32(tb->ctts + ei * 8);
        off = (int)r32(tb->ctts + ei * 8 + 4); /* signed in v1, small +ve in v0 */
        ei++;
      }
      S[i].pts = S[i].dts + off;
      if (cnt > 0)
        cnt--;
    }
  }

  /* Keyframes: stss lists 1-based sync samples; its absence means all-key. */
  if (tb->stss) {
    unsigned e;
    for (e = 0; e < tb->stss_n; e++) {
      unsigned s = r32(tb->stss + e * 4);
      if (s >= 1 && s <= n)
        S[s - 1].keyframe = 1;
    }
  } else {
    for (i = 0; i < n; i++)
      S[i].keyframe = 1;
  }

  t->samples = S;
  t->n_samples = n;
  return MP4_OK;
}

/* ---- open / close ---------------------------------------------------------
 */
int mp4_open(const unsigned char *buf, unsigned len, mp4 **out)
{
  mp4 *m;
  boxiter top;
  box b;
  int have_ftyp = 0, have_moov = 0;
  trackbuild tbs[MAX_TRACKS];
  int nb = 0, i;

  if (out)
    *out = 0;
  if (!buf || len < 8 || !out)
    return MP4_EBADSIG;

  memset(tbs, 0, sizeof tbs);

  /* Top level: require an ftyp and a moov to accept the file. */
  top.p = buf;
  top.end = buf + len;
  while (box_next(&top, &b)) {
    if (tag4(b.type, "ftyp"))
      have_ftyp = 1;
    else if (tag4(b.type, "moov")) {
      boxiter mv = {b.body, b.bend};
      box t;
      have_moov = 1;
      while (box_next(&mv, &t)) {
        if (tag4(t.type, "trak") && nb < MAX_TRACKS) {
          parse_trak(&tbs[nb], &t);
          if (tbs[nb].present)
            nb++;
          else
            memset(&tbs[nb], 0, sizeof tbs[nb]);
        }
      }
    }
  }
  if (!have_ftyp && !have_moov)
    return MP4_EBADSIG;
  if (!have_moov || nb == 0)
    return MP4_EFORMAT;

  m = (mp4 *)calloc(1, sizeof *m);
  if (!m)
    return MP4_ENOMEM;
  m->buf = buf;
  m->len = len;
  m->vtrack = m->atrack = -1;
  m->tracks = (mp4_track *)calloc(nb, sizeof *m->tracks);
  if (!m->tracks) {
    free(m);
    return MP4_ENOMEM;
  }

  for (i = 0; i < nb; i++) {
    trackbuild *tb = &tbs[i];
    mp4_track *t = &m->tracks[i];
    t->is_video = tb->is_video;
    t->codec = tb->codec;
    t->timescale = tb->timescale ? tb->timescale : 1;
    t->duration = tb->duration;
    t->edit_start = tb->edit_start;
    t->width = tb->width;
    t->height = tb->height;
    t->channels = tb->channels;
    t->samplerate = tb->samplerate;
    t->bits = tb->bits;
    t->extradata = tb->extradata;
    t->extradata_len = tb->extradata_len;
    t->nal_length_size = tb->nal_length_size;
    if (build_samples(m, tb, t) != MP4_OK) {
      mp4_close(m);
      return MP4_ENOMEM;
    }
    if (tb->is_video && m->vtrack < 0)
      m->vtrack = i;
    if (tb->is_audio && m->atrack < 0)
      m->atrack = i;
  }
  m->n_tracks = nb;

  *out = m;
  return MP4_OK;
}

void mp4_close(mp4 *m)
{
  int i;
  if (!m)
    return;
  if (m->tracks) {
    for (i = 0; i < m->n_tracks; i++)
      free(m->tracks[i].samples);
    free(m->tracks);
  }
  free(m);
}

int mp4_track_count(const mp4 *m) { return m ? m->n_tracks : 0; }

const mp4_track *mp4_track_at(const mp4 *m, int i) { return (m && i >= 0 && i < m->n_tracks) ? &m->tracks[i] : 0; }

const mp4_track *mp4_video_track(const mp4 *m) { return (m && m->vtrack >= 0) ? &m->tracks[m->vtrack] : 0; }

const mp4_track *mp4_audio_track(const mp4 *m) { return (m && m->atrack >= 0) ? &m->tracks[m->atrack] : 0; }

const unsigned char *mp4_sample_data(const mp4 *m, const mp4_track *t, unsigned i, unsigned *size)
{
  if (!m || !t || i >= t->n_samples)
    return 0;
  if (size)
    *size = t->samples[i].size;
  return m->buf + t->samples[i].offset;
}

unsigned mp4_seek_sample(const mp4_track *t, long long pts)
{
  unsigned i, key = 0;
  int found = 0;
  if (!t || t->n_samples == 0)
    return 0;
  /* The last keyframe whose pts <= target; sample order is decode order, so a
   * keyframe seen so far is a valid restart point for everything after it. */
  for (i = 0; i < t->n_samples; i++) {
    if (t->samples[i].keyframe && t->samples[i].pts <= pts) {
      key = i;
      found = 1;
    }
    if (t->samples[i].pts > pts)
      break;
  }
  return found ? key : 0;
}

int mp4_h264_param_set(const mp4_track *t, int which, int idx, const unsigned char **nal, unsigned *nal_len)
{
  const unsigned char *p, *end;
  int count, i;
  if (!t || t->codec != MP4_CODEC_H264 || !t->extradata || t->extradata_len < 6)
    return MP4_EFORMAT;
  p = t->extradata;
  end = p + t->extradata_len;
  p += 5; /* skip the fixed avcC head */

  if (which == 0) { /* SPS list                 */
    if (p >= end)
      return MP4_EFORMAT;
    count = *p++ & 0x1F; /* numOfSequenceParameterSets */
  } else {               /* skip SPS, then PPS list  */
    int sps;
    if (p >= end)
      return MP4_EFORMAT;
    sps = *p++ & 0x1F;
    for (i = 0; i < sps; i++) {
      unsigned l;
      if (p + 2 > end)
        return MP4_EFORMAT;
      l = r16(p);
      p += 2;
      if (p + l > end)
        return MP4_EFORMAT;
      p += l;
    }
    if (p >= end)
      return MP4_EFORMAT;
    count = *p++; /* numOfPictureParameterSets */
  }

  for (i = 0; i < count; i++) {
    unsigned l;
    if (p + 2 > end)
      return MP4_EFORMAT;
    l = r16(p);
    p += 2;
    if (p + l > end)
      return MP4_EFORMAT;
    if (i == idx) {
      *nal = p;
      *nal_len = l;
      return MP4_OK;
    }
    p += l;
  }
  return MP4_EFORMAT;
}
