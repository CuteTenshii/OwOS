/* tags.c, title/artist/album and embedded cover art from ID3v2/ID3v1 (MP3, ADTS), MP4 ilst (.m4a), FLAC,
 * Ogg Vorbis/Opus comments and RIFF INFO (WAV). Files are read in pieces with readfile_at, never loaded whole. */
#include "media.h"
#include "owos.h"

#define TEXT_MAX 300
#define WALK_MAX 4096
#define COMMENT_MAX (256u << 10) /* comment block bytes read when only tags are wanted */
#define COVER_MAX (8u << 20)

struct ctx {
  struct media_tags *t;
  int want_cover;
  unsigned char *cover;
  unsigned cover_len;
  int cover_type; /* ID3/FLAC picture type of the kept image, -1 if none */
};

/* Keep an image, preferring the front cover (type 3) over any earlier one */
static void offer_cover(struct ctx *x, int type, const unsigned char *d, unsigned n)
{
  if (!x->want_cover || !n || n > COVER_MAX || (x->cover && (x->cover_type == 3 || type != 3)))
    return;
  unsigned char *c = malloc(n);
  if (!c)
    return;
  memcpy(c, d, n);
  free(x->cover);
  x->cover = c;
  x->cover_len = n;
  x->cover_type = type;
}

/* Read n bytes at off into a new buffer; NULL when n exceeds cap or the file is short */
static unsigned char *slurp(const char *path, unsigned off, unsigned n, unsigned cap)
{
  if (n > cap)
    return 0;
  unsigned char *b = malloc(n + 1);
  if (b && readfile_at(path, off, b, n) != (int)n) {
    free(b);
    return 0;
  }
  return b;
}

static unsigned be32(const unsigned char *p) { return (unsigned)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static unsigned le32(const unsigned char *p) { return (unsigned)p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0]; }

static unsigned syncsafe(const unsigned char *p)
{
  return (unsigned)(p[0] & 0x7f) << 21 | (p[1] & 0x7f) << 14 | (p[2] & 0x7f) << 7 | (p[3] & 0x7f);
}

/* Convert `n` bytes of text to UTF-8 in dst[MEDIA_TAG_MAX]. enc: 0 Latin-1, 1 UTF-16 (BOM), 2 UTF-16BE, 3 UTF-8 */
static void set_text(char *dst, int enc, const unsigned char *s, unsigned n)
{
  int o = 0;
  if (enc == 0 || enc == 3) {
    for (unsigned i = 0; i < n && s[i]; i++) {
      if (enc == 3) {
        if (o < MEDIA_TAG_MAX - 1)
          dst[o++] = (char)s[i];
      } else {
        char u[4];
        int k = utf8_encode(s[i], u);
        if (o + k < MEDIA_TAG_MAX)
          for (int j = 0; j < k; j++)
            dst[o++] = u[j];
      }
    }
    if (enc == 3 && o == MEDIA_TAG_MAX - 1 && o < (int)n && s[o]) {
      while (o > 0 && (s[o] & 0xC0) == 0x80)
        o--; /* cut fell inside a multibyte character */
    }
  } else {
    int le = 0;
    unsigned i = 0;
    if (enc == 1 && n >= 2 && s[0] == 0xFF && s[1] == 0xFE)
      le = 1, i = 2;
    else if (enc == 1 && n >= 2 && s[0] == 0xFE && s[1] == 0xFF)
      i = 2;
    for (; i + 1 < n; i += 2) {
      unsigned cp = le ? s[i] | s[i + 1] << 8 : s[i] << 8 | s[i + 1];
      if (!cp)
        break;
      if (cp >= 0xD800 && cp < 0xDC00 && i + 3 < n) {
        unsigned lo = le ? s[i + 2] | s[i + 3] << 8 : s[i + 2] << 8 | s[i + 3];
        if (lo < 0xDC00 || lo > 0xDFFF)
          continue;
        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        i += 2;
      }
      char u[4];
      int k = utf8_encode(cp, u);
      if (o + k >= MEDIA_TAG_MAX)
        break;
      for (int j = 0; j < k; j++)
        dst[o++] = u[j];
    }
  }
  while (o > 0 && dst[o - 1] == ' ')
    o--;
  dst[o] = '\0';
}

/* Read a text value of up to `size` bytes at `off` into dst, unless dst is already set */
static void read_text(const char *path, unsigned off, unsigned size, int enc, char *dst)
{
  unsigned char b[TEXT_MAX];
  if (!dst || dst[0])
    return;
  if (size > TEXT_MAX)
    size = TEXT_MAX;
  int g = readfile_at(path, off, b, size);
  if (g <= 0)
    return;
  if (enc < 0) {
    if (g < 2)
      return;
    set_text(dst, b[0], b + 1, (unsigned)g - 1);
  } else {
    set_text(dst, enc, b, (unsigned)g);
  }
}

/* ID3v2 */
static char *id3_field(struct media_tags *t, unsigned ver, const unsigned char *id)
{
  static const char *const v2[3] = {"TT2", "TP1", "TAL"};
  static const char *const v34[3] = {"TIT2", "TPE1", "TALB"};
  char *const dst[3] = {t->title, t->artist, t->album};
  for (int i = 0; i < 3; i++)
    if (memcmp(id, ver == 2 ? v2[i] : v34[i], ver == 2 ? 3 : 4) == 0)
      return dst[i];
  return 0;
}

/* Skip an ID3 text field of encoding enc starting at i; returns the index after its terminator */
static unsigned id3_skip_text(const unsigned char *b, unsigned n, unsigned i, int enc)
{
  if (enc == 1 || enc == 2) {
    while (i + 1 < n && (b[i] || b[i + 1]))
      i += 2;
    return i + 2;
  }
  while (i < n && b[i])
    i++;
  return i + 1;
}

static void read_apic(struct ctx *x, unsigned ver, const unsigned char *b, unsigned n)
{
  if (n < 4)
    return;
  int enc = b[0];
  unsigned i = ver == 2 ? 4 : id3_skip_text(b, n, 1, 0);
  if (i >= n)
    return;
  int type = b[i++];
  i = id3_skip_text(b, n, i, enc);
  if (i < n)
    offer_cover(x, type, b + i, n - i);
}

/* Returns the offset just past the tag, or 0 when the file has none */
static unsigned read_id3v2(const char *path, unsigned fsize, struct ctx *x)
{
  struct media_tags *t = x->t;
  unsigned char h[10], f[10];
  if (readfile_at(path, 0, h, 10) != 10 || memcmp(h, "ID3", 3) || h[3] < 2 || h[3] > 4)
    return 0;
  unsigned ver = h[3], pos = 10, end = 10 + syncsafe(h + 6), hl = ver == 2 ? 6 : 10;
  if (end > fsize)
    end = fsize;
  if (ver >= 3 && (h[5] & 0x40)) {
    if (readfile_at(path, pos, f, 4) != 4)
      return 0;
    pos += ver == 4 ? syncsafe(f) : be32(f) + 4;
  }
  for (int n = 0; n < WALK_MAX && pos < end && end - pos >= hl; n++) {
    if (readfile_at(path, pos, f, hl) != (int)hl || !f[0])
      break;
    unsigned sz = ver == 2 ? f[3] << 16 | f[4] << 8 | f[5] : ver == 4 ? syncsafe(f + 4) : be32(f + 4);
    unsigned body = pos + hl;
    if (sz > end - body)
      break;
    pos = body + sz;
    char *dst = id3_field(t, ver, f);
    int pic = x->want_cover && (ver == 2 ? memcmp(f, "PIC", 3) == 0 : memcmp(f, "APIC", 4) == 0);
    if (!dst && !pic)
      continue;
    unsigned skip = 0;
    if (ver == 3 && (f[9] & 0xC0))
      continue; /* compressed or encrypted */
    if (ver == 4) {
      if (f[9] & 0x0C)
        continue;
      skip = ((f[9] & 0x40) ? 1 : 0) + ((f[9] & 0x01) ? 4 : 0);
    }
    if (sz <= skip)
      continue;
    if (pic) {
      unsigned char *b = slurp(path, body + skip, sz - skip, COVER_MAX);
      if (b)
        read_apic(x, ver, b, sz - skip);
      free(b);
    } else {
      read_text(path, body + skip, sz - skip, -1, dst);
    }
  }
  return 10 + syncsafe(h + 6);
}

static void read_id3v1(const char *path, unsigned fsize, struct media_tags *t)
{
  unsigned char b[128];
  if (fsize < 128 || readfile_at(path, fsize - 128, b, 128) != 128 || memcmp(b, "TAG", 3))
    return;
  if (!t->title[0])
    set_text(t->title, 0, b + 3, 30);
  if (!t->artist[0])
    set_text(t->artist, 0, b + 33, 30);
  if (!t->album[0])
    set_text(t->album, 0, b + 63, 30);
}

/* ISO base media boxes */
static int next_box(const char *path, unsigned off, unsigned end, unsigned char *type, unsigned *hdr, unsigned *size)
{
  unsigned char h[16];
  if (off >= end || end - off < 8 || readfile_at(path, off, h, 16 > end - off ? end - off : 16) < 8)
    return 0;
  unsigned s = be32(h);
  *hdr = 8;
  if (s == 1) {
    if (end - off < 16 || readfile_at(path, off, h, 16) != 16)
      return 0;
    s = be32(h + 8) ? end - off : be32(h + 12);
    *hdr = 16;
  } else if (s == 0) {
    s = end - off;
  }
  if (s < *hdr)
    return 0;
  memcpy(type, h + 4, 4);
  *size = s > end - off ? end - off : s;
  return 1;
}

/* Find the first child box `type` in [off, end); sets its payload range */
static int find_box(const char *path, unsigned off, unsigned end, const char *type, unsigned *body, unsigned *bend)
{
  unsigned char ty[4];
  unsigned hdr, size;
  for (int n = 0; n < WALK_MAX && next_box(path, off, end, ty, &hdr, &size); n++) {
    if (memcmp(ty, type, 4) == 0) {
      *body = off + hdr;
      *bend = off + size;
      return 1;
    }
    off += size;
  }
  return 0;
}

static void read_mp4(const char *path, unsigned fsize, struct ctx *x)
{
  struct media_tags *t = x->t;
  unsigned b, e;
  if (!find_box(path, 0, fsize, "moov", &b, &e) || !find_box(path, b, e, "udta", &b, &e) ||
      !find_box(path, b, e, "meta", &b, &e) || e - b < 4 || !find_box(path, b + 4, e, "ilst", &b, &e))
    return;
  static const char *const names[3] = {"\xA9nam",
                                       "\xA9"
                                       "ART",
                                       "\xA9"
                                       "alb"};
  char *const dst[3] = {t->title, t->artist, t->album};
  for (int i = 0; i < 3; i++) {
    unsigned ib, ie, db, de;
    if (find_box(path, b, e, names[i], &ib, &ie) && find_box(path, ib, ie, "data", &db, &de) && de - db > 8)
      read_text(path, db + 8, de - db - 8, 3, dst[i]);
  }
  unsigned cb, ce, db, de;
  if (x->want_cover && find_box(path, b, e, "covr", &cb, &ce) && find_box(path, cb, ce, "data", &db, &de) &&
      de - db > 8) {
    unsigned char *img = slurp(path, db + 8, de - db - 8, COVER_MAX);
    if (img)
      offer_cover(x, 3, img, de - db - 8);
    free(img);
  }
}

/* RIFF WAVE LIST/INFO */
static void read_wav(const char *path, unsigned fsize, struct media_tags *t)
{
  unsigned char h[12];
  unsigned off = 12;
  for (int n = 0; n < WALK_MAX && off < fsize && fsize - off >= 8; n++) {
    if (readfile_at(path, off, h, 12) < 8)
      return;
    unsigned sz = le32(h + 4);
    if (memcmp(h, "LIST", 4) == 0 && sz >= 4 && memcmp(h + 8, "INFO", 4) == 0) {
      unsigned p = off + 12, end = off + 8 + sz;
      if (end > fsize)
        end = fsize;
      for (int k = 0; k < WALK_MAX && p < end && end - p >= 8; k++) {
        unsigned char c[8];
        if (readfile_at(path, p, c, 8) != 8)
          break;
        unsigned cs = le32(c + 4);
        if (cs > end - p - 8)
          break;
        if (memcmp(c, "INAM", 4) == 0)
          read_text(path, p + 8, cs, 3, t->title);
        else if (memcmp(c, "IART", 4) == 0)
          read_text(path, p + 8, cs, 3, t->artist);
        else if (memcmp(c, "IPRD", 4) == 0)
          read_text(path, p + 8, cs, 3, t->album);
        p += 8 + cs + (cs & 1);
      }
      return;
    }
    if (sz > fsize - off - 8)
      return;
    off += 8 + sz + (sz & 1);
  }
}

/* Vorbis comments, shared by FLAC, Ogg Vorbis and Opus */
static int b64val(int c)
{
  if (c >= 'A' && c <= 'Z')
    return c - 'A';
  if (c >= 'a' && c <= 'z')
    return c - 'a' + 26;
  if (c >= '0' && c <= '9')
    return c - '0' + 52;
  return c == '+' ? 62 : c == '/' ? 63 : -1;
}

static unsigned b64decode(const unsigned char *s, unsigned n, unsigned char *out)
{
  unsigned o = 0, acc = 0;
  int bits = 0;
  for (unsigned i = 0; i < n; i++) {
    int v = b64val(s[i]);
    if (v < 0)
      continue;
    acc = acc << 6 | (unsigned)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out[o++] = (unsigned char)(acc >> bits);
    }
  }
  return o;
}

static void parse_flac_picture(struct ctx *x, const unsigned char *p, unsigned n)
{
  if (n < 8)
    return;
  unsigned ml = be32(p + 4), i = 8 + ml;
  if (ml > n - 8 || n - i < 4)
    return;
  unsigned dl = be32(p + i);
  i += 4;
  if (dl > n - i || n - i - dl < 20)
    return;
  i += dl;
  unsigned len = be32(p + i + 16);
  i += 20;
  if (len <= n - i)
    offer_cover(x, (int)be32(p), p + i, len);
}

/* returns the length of `e`'s value if its key is `key` (case-insensitive, with '='), else 0 */
static unsigned comment_value(const unsigned char *e, unsigned l, const char *key, unsigned *off)
{
  unsigned k = 0;
  for (; key[k]; k++) {
    int c = k < l ? e[k] : 0;
    if (c >= 'a' && c <= 'z')
      c -= 32;
    if (c != key[k])
      return 0;
  }
  if (k >= l || e[k] != '=')
    return 0;
  *off = k + 1;
  return l - k - 1;
}

static void parse_vorbis_comments(struct ctx *x, const unsigned char *p, unsigned n)
{
  struct media_tags *t = x->t;
  if (n < 8)
    return;
  unsigned pos = 4 + le32(p);
  if (pos > n || n - pos < 4)
    return;
  unsigned count = le32(p + pos);
  pos += 4;
  for (unsigned i = 0; i < count && i < WALK_MAX && n - pos >= 4; i++) {
    unsigned l = le32(p + pos), off;
    pos += 4;
    if (l > n - pos)
      return;
    const unsigned char *e = p + pos;
    pos += l;
    unsigned vl;
    if ((vl = comment_value(e, l, "TITLE", &off)) && !t->title[0])
      set_text(t->title, 3, e + off, vl);
    else if ((vl = comment_value(e, l, "ARTIST", &off)) && !t->artist[0])
      set_text(t->artist, 3, e + off, vl);
    else if ((vl = comment_value(e, l, "ALBUM", &off)) && !t->album[0])
      set_text(t->album, 3, e + off, vl);
    else if (x->want_cover && (vl = comment_value(e, l, "METADATA_BLOCK_PICTURE", &off))) {
      unsigned char *pic = malloc(vl / 4 * 3 + 3);
      if (pic) {
        parse_flac_picture(x, pic, b64decode(e + off, vl, pic));
        free(pic);
      }
    }
  }
}

static void read_flac(const char *path, unsigned fsize, unsigned off, struct ctx *x)
{
  unsigned char h[4];
  for (int n = 0; n < WALK_MAX && off < fsize && fsize - off >= 4; n++) {
    if (readfile_at(path, off, h, 4) != 4)
      return;
    unsigned type = h[0] & 0x7f, len = h[1] << 16 | h[2] << 8 | h[3], body = off + 4;
    if (len > fsize - body)
      return;
    if (type == 4 || (type == 6 && x->want_cover)) {
      unsigned cap = x->want_cover ? COVER_MAX : COMMENT_MAX;
      unsigned char *b = slurp(path, body, len < cap ? len : cap, cap);
      if (b) {
        if (type == 4)
          parse_vorbis_comments(x, b, len < cap ? len : cap);
        else
          parse_flac_picture(x, b, len);
      }
      free(b);
    }
    if (h[0] & 0x80)
      return;
    off = body + len;
  }
}

/* Reassemble the second packet of the first logical stream (the comment header), possibly spanning pages */
static unsigned char *ogg_comment_packet(const char *path, unsigned fsize, unsigned cap, unsigned *out_len)
{
  unsigned char h[27 + 255];
  unsigned char *buf = 0;
  unsigned len = 0, off = 0, serial = 0;
  int state = 0;
  for (int pg = 0; pg < WALK_MAX && off < fsize && fsize - off >= 27; pg++) {
    if (readfile_at(path, off, h, 27) != 27 || memcmp(h, "OggS", 4))
      break;
    unsigned nseg = h[26], sn = le32(h + 14);
    if (readfile_at(path, off + 27, h + 27, nseg) != (int)nseg)
      break;
    if (pg == 0)
      serial = sn;
    unsigned body = off + 27 + nseg, total = 0, pos = body, run = 0;
    int done = 0;
    for (unsigned i = 0; i < nseg; i++) {
      unsigned l = h[27 + i];
      total += l;
      if (sn != serial || done)
        continue;
      if (state == 0) {
        pos += l;
        if (l < 255)
          state = 1;
      } else {
        run += l;
        if (l < 255)
          done = 1;
      }
    }
    if (sn == serial && state == 1 && run) {
      if (run > cap - len)
        run = cap - len, done = 1;
      unsigned char *nb = realloc(buf, len + run);
      if (!nb || readfile_at(path, pos, nb + len, run) != (int)run) {
        free(nb ? nb : buf);
        return 0;
      }
      buf = nb;
      len += run;
    }
    if (done) {
      *out_len = len;
      return buf;
    }
    off = body + total;
  }
  free(buf);
  return 0;
}

static void read_ogg(const char *path, unsigned fsize, struct ctx *x)
{
  unsigned len;
  unsigned char *b = ogg_comment_packet(path, fsize, x->want_cover ? COVER_MAX : COMMENT_MAX, &len);
  if (!b)
    return;
  if (len > 8 && memcmp(b, "OpusTags", 8) == 0)
    parse_vorbis_comments(x, b + 8, len - 8);
  else if (len > 7 && b[0] == 3 && memcmp(b + 1, "vorbis", 6) == 0)
    parse_vorbis_comments(x, b + 7, len - 7);
  free(b);
}

static void scan(const char *path, struct ctx *x)
{
  struct statinfo st;
  unsigned char sn[12];
  memset(x->t, 0, sizeof *x->t);
  if (!path || stat(path, &st) != 0 || st.is_dir)
    return;
  int g = readfile_at(path, 0, sn, sizeof sn);
  if (g >= 12 && memcmp(sn, "RIFF", 4) == 0 && memcmp(sn + 8, "WAVE", 4) == 0) {
    read_wav(path, st.size, x->t);
  } else if (g >= 8 && memcmp(sn + 4, "ftyp", 4) == 0) {
    read_mp4(path, st.size, x);
  } else if (g >= 4 && memcmp(sn, "fLaC", 4) == 0) {
    read_flac(path, st.size, 4, x);
  } else if (g >= 4 && memcmp(sn, "OggS", 4) == 0) {
    read_ogg(path, st.size, x);
  } else {
    unsigned after = read_id3v2(path, st.size, x);
    if (after && readfile_at(path, after, sn, 4) == 4 && memcmp(sn, "fLaC", 4) == 0)
      read_flac(path, st.size, after + 4, x);
    else
      read_id3v1(path, st.size, x->t);
  }
}

int media_tags_read(const char *path, struct media_tags *t)
{
  struct ctx x = {t, 0, 0, 0, -1};
  scan(path, &x);
  return t->title[0] || t->artist[0] || t->album[0];
}

unsigned char *media_cover_read(const char *path, unsigned *len)
{
  struct media_tags t;
  struct ctx x = {&t, 1, 0, 0, -1};
  scan(path, &x);
  *len = x.cover_len;
  return x.cover;
}
