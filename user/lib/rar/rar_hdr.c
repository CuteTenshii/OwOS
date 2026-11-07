/* rar_hdr.c - RAR 5 and RAR 3/4 block header parsing, header decryption, volume switching. */
#include "crc32.h"
#include "owos.h"
#include "rar_int.h"

static const uint8_t sig4[7] = {0x52, 0x61, 0x72, 0x21, 0x1a, 0x07, 0x00};

static uint32_t le16(const uint8_t *p) { return p[0] | ((uint32_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return le16(p) | (le16(p + 2) << 16); }

static bool get_vint(const uint8_t **pp, const uint8_t *end, uint64_t *v)
{
  const uint8_t *p = *pp;
  uint64_t x = 0;
  unsigned sh = 0;
  while (p < end && sh < 64) {
    uint8_t b = *p++;
    x |= (uint64_t)(b & 0x7f) << sh;
    sh += 7;
    if (!(b & 0x80)) {
      *pp = p;
      *v = x;
      return true;
    }
  }
  return false;
}

void rar_file_free(struct rar_file *f)
{
  free(f->name);
  free(f->link);
  free(f->chunks);
  memset(f, 0, sizeof *f);
}

static int ensure_hbuf(struct rar *r, size_t n)
{
  if (r->hcap >= n)
    return RAR_OK;
  uint8_t *b = realloc(r->hbuf, n);
  if (!b)
    return RAR_ENOMEM;
  r->hbuf = b;
  r->hcap = n;
  return RAR_OK;
}

int rar_find_sig(struct rar *r, struct rar_vol *v, uint64_t *end, int *fmt)
{
  const uint32_t chunk = 0x10000, limit = 0x100000 + 8;
  uint8_t *buf = malloc(chunk + 8);
  if (!buf)
    return RAR_ENOMEM;
  uint64_t off = 0;
  int rc = RAR_EFORMAT;
  uint32_t carry = 0;
  while (off < limit) {
    int n = rar_vol_read(r, v, off, buf + carry, chunk);
    if (n < 0) {
      rc = n;
      break;
    }
    uint32_t total = carry + (uint32_t)n;
    if (total < 7)
      break;
    for (uint32_t i = 0; i + 7 <= total; i++) {
      if (buf[i] != 0x52 || memcmp(buf + i, sig4, 6) != 0)
        continue;
      if (buf[i + 6] == 0) {
        *end = off - carry + i + 7;
        *fmt = 4;
        rc = RAR_OK;
      } else if (buf[i + 6] == 1 && i + 8 <= total && buf[i + 7] == 0) {
        *end = off - carry + i + 8;
        *fmt = 5;
        rc = RAR_OK;
      }
      if (rc == RAR_OK)
        break;
    }
    if (rc == RAR_OK || n == 0)
      break;
    carry = total > 7 ? 7 : total;
    memmove(buf, buf + total - carry, carry);
    off += (uint32_t)n;
  }
  free(buf);
  return rc;
}

int rar_get_key(struct rar *r, const struct rar_enc *e, struct rar_keycache *kc)
{
  if (!r->password)
    return RAR_EPASSWORD;
  unsigned saltlen = e->ver == 5 ? 16 : 8;
  if (!(kc->valid && kc->ver == e->ver && kc->lg2 == e->lg2 && memcmp(kc->salt, e->salt, saltlen) == 0)) {
    kc->valid = false;
    memset(kc->iv, 0, 16);
    if (e->ver == 5) {
      if (!rar5_kdf((const uint8_t *)r->password, (uint32_t)strlen(r->password), e->salt, e->lg2, kc->key, kc->hashkey,
                    kc->check))
        return RAR_EUNSUPPORTED;
    } else {
      size_t n = strlen(r->password);
      uint8_t *w = malloc(n * 4 + 2);
      if (!w)
        return RAR_ENOMEM;
      uint32_t wl = 0;
      const unsigned char *s = (const unsigned char *)r->password, *se = s + n;
      while (s < se) {
        uint32_t cp = *s++;
        if (cp >= 0xc0 && cp < 0xf8) {
          unsigned extra = cp >= 0xf0 ? 3 : cp >= 0xe0 ? 2 : 1;
          cp &= 0x3f >> extra;
          while (extra-- && s < se)
            cp = (cp << 6) | (*s++ & 0x3f);
        }
        if (cp >= 0x10000) {
          cp -= 0x10000;
          uint32_t hi = 0xd800 + (cp >> 10), lo = 0xdc00 + (cp & 0x3ff);
          w[wl++] = (uint8_t)hi;
          w[wl++] = (uint8_t)(hi >> 8);
          w[wl++] = (uint8_t)lo;
          w[wl++] = (uint8_t)(lo >> 8);
        } else {
          w[wl++] = (uint8_t)cp;
          w[wl++] = (uint8_t)(cp >> 8);
        }
      }
      rar3_kdf(w, wl, e->salt, kc->key, kc->iv);
      free(w);
    }
    kc->ver = e->ver;
    kc->lg2 = e->lg2;
    memcpy(kc->salt, e->salt, 16);
    kc->valid = true;
  }
  if (e->ver == 5 && e->has_check && memcmp(kc->check, e->check, 8) != 0)
    return RAR_EPASSWORD;
  return RAR_OK;
}

/* ------------------------------------------------------------------ volumes */

static char *vol_name(const char *first, int idx)
{
  size_t n = strlen(first);
  char *out = malloc(n + 16);
  if (!out)
    return NULL;
  if (idx == 0) {
    memcpy(out, first, n + 1);
    return out;
  }
  const char *dot = NULL;
  for (const char *p = first; *p; p++)
    if (*p == '.')
      dot = p;
  if (!dot || strlen(dot) < 4) {
    free(out);
    return NULL;
  }
  const char *ext = dot + 1;
  bool upper = ext[0] >= 'A' && ext[0] <= 'Z';
  bool is_rar = strlen(ext) == 3 && (ext[0] | 32) == 'r' && (ext[1] | 32) == 'a' && (ext[2] | 32) == 'r';
  if (is_rar) {
    const char *e = dot;
    while (e > first && e[-1] >= '0' && e[-1] <= '9')
      e--;
    size_t digits = (size_t)(dot - e);
    if (digits && e - first >= 5 && (e[-1] | 32) == 't' && (e[-2] | 32) == 'r' && (e[-3] | 32) == 'a' &&
        (e[-4] | 32) == 'p' && e[-5] == '.') {
      uint64_t v = 0;
      for (const char *p = e; p < dot; p++)
        v = v * 10 + (uint64_t)(*p - '0');
      v += (uint64_t)idx;
      memcpy(out, first, (size_t)(e - first));
      int k = (int)(e - first);
      char num[24];
      int nl = snprintf(num, sizeof num, "%llu", (unsigned long long)v);
      for (int i = nl; i < (int)digits; i++)
        out[k++] = '0';
      memcpy(out + k, num, (size_t)nl);
      k += nl;
      memcpy(out + k, dot, strlen(dot) + 1);
      return out;
    }
    memcpy(out, first, (size_t)(dot - first) + 1);
    int k = (int)(dot - first) + 1;
    int v = idx - 1;
    out[k++] = (char)((upper ? 'R' : 'r') + v / 100);
    out[k++] = (char)('0' + (v / 10) % 10);
    out[k++] = (char)('0' + v % 10);
    out[k] = 0;
    return out;
  }
  if (strlen(ext) == 3 && (ext[0] | 32) >= 'r' && (ext[0] | 32) <= 'z' && ext[1] >= '0' && ext[1] <= '9' &&
      ext[2] >= '0' && ext[2] <= '9') {
    int v = ((ext[0] | 32) - 'r') * 100 + (ext[1] - '0') * 10 + (ext[2] - '0') + idx;
    memcpy(out, first, (size_t)(dot - first) + 1);
    int k = (int)(dot - first) + 1;
    out[k++] = (char)((upper ? 'R' : 'r') + v / 100);
    out[k++] = (char)('0' + (v / 10) % 10);
    out[k++] = (char)('0' + v % 10);
    out[k] = 0;
    return out;
  }
  free(out);
  return NULL;
}

void rar_vol_close(struct rar_vol *v)
{
  if (v->open && v->src.close)
    v->src.close(v->src.ctx);
  v->open = false;
}

int rar_open_volume(struct rar *r, struct rar_vol *v, int idx)
{
  char *name = r->first_name ? vol_name(r->first_name, idx) : NULL;
  struct rar_src src;
  if (!name || !r->cfg.open_volume || r->cfg.open_volume(r->cfg.ctx, name, &src) != 0) {
    free(r->missing_name);
    r->missing_name = name;
    return RAR_EVOLUME;
  }
  free(name);
  rar_vol_close(v);
  v->src = src;
  v->open = true;
  v->idx = idx;
  return RAR_OK;
}

/* ------------------------------------------------------------------ time and names */

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

static uint32_t dos_to_unix(uint32_t t)
{
  unsigned date = t >> 16, tm = t & 0xffff;
  unsigned y = 1980 + (date >> 9), mo = (date >> 5) & 15, d = date & 31;
  if (mo < 1 || mo > 12 || d < 1)
    return 0;
  int64_t days = days_from_civil(y, mo, d);
  int64_t s = days * 86400 + (tm >> 11) * 3600 + ((tm >> 5) & 63) * 60 + (tm & 31) * 2;
  return s < 0 || s > 0xFFFFFFFFll ? 0 : (uint32_t)s;
}

static bool utf8_ok(const uint8_t *s, size_t n)
{
  for (size_t i = 0; i < n;) {
    uint8_t c = s[i];
    unsigned extra = c < 0x80                  ? 0
                     : (c >= 0xc2 && c < 0xe0) ? 1
                     : (c >= 0xe0 && c < 0xf0) ? 2
                     : (c >= 0xf0 && c < 0xf5) ? 3
                                               : 9;
    if (extra == 9 || i + extra + 1 > n)
      return false;
    for (unsigned k = 1; k <= extra; k++)
      if ((s[i + k] & 0xc0) != 0x80)
        return false;
    i += extra + 1;
  }
  return true;
}

static size_t put_utf8(uint8_t *o, uint32_t cp)
{
  if (cp < 0x80) {
    o[0] = (uint8_t)cp;
    return 1;
  }
  if (cp < 0x800) {
    o[0] = (uint8_t)(0xc0 | (cp >> 6));
    o[1] = (uint8_t)(0x80 | (cp & 0x3f));
    return 2;
  }
  if (cp < 0x10000) {
    o[0] = (uint8_t)(0xe0 | (cp >> 12));
    o[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3f));
    o[2] = (uint8_t)(0x80 | (cp & 0x3f));
    return 3;
  }
  o[0] = (uint8_t)(0xf0 | (cp >> 18));
  o[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3f));
  o[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3f));
  o[3] = (uint8_t)(0x80 | (cp & 0x3f));
  return 4;
}

static char *utf16_to_utf8(const uint16_t *w, size_t n)
{
  uint8_t *o = malloc(n * 3 + 1);
  if (!o)
    return NULL;
  size_t k = 0;
  for (size_t i = 0; i < n; i++) {
    uint32_t c = w[i];
    if (c >= 0xd800 && c < 0xdc00 && i + 1 < n && w[i + 1] >= 0xdc00 && w[i + 1] < 0xe000) {
      c = 0x10000 + ((c - 0xd800) << 10) + (w[i + 1] - 0xdc00);
      i++;
    }
    k += put_utf8(o + k, c);
  }
  o[k] = 0;
  return (char *)o;
}

static char *latin1_to_utf8(const uint8_t *s, size_t n)
{
  uint8_t *o = malloc(n * 2 + 1);
  if (!o)
    return NULL;
  size_t k = 0;
  for (size_t i = 0; i < n; i++)
    k += put_utf8(o + k, s[i]);
  o[k] = 0;
  return (char *)o;
}

static char *decode_name4(const uint8_t *nm, uint32_t n, bool unicode)
{
  uint32_t zero = 0;
  while (zero < n && nm[zero])
    zero++;
  if (unicode && zero < n) {
    const uint8_t *enc = nm + zero + 1;
    uint32_t en = n - zero - 1;
    uint16_t *w = malloc((size_t)(n + 1) * sizeof *w);
    if (!w)
      return NULL;
    uint32_t ep = 0, dp = 0, hi = 0, flags = 0, fbits = 0;
    if (en)
      hi = (uint32_t)enc[ep++] << 8;
    while (ep < en && dp < n) {
      if (fbits == 0) {
        flags = enc[ep++];
        fbits = 8;
        if (ep >= en)
          break;
      }
      switch (flags >> 6) {
      case 0:
        w[dp++] = enc[ep++];
        break;
      case 1:
        w[dp++] = (uint16_t)(enc[ep++] + hi);
        break;
      case 2:
        if (ep + 1 >= en)
          ep = en;
        else {
          w[dp++] = (uint16_t)(enc[ep] | (enc[ep + 1] << 8));
          ep += 2;
        }
        break;
      default: {
        uint32_t len = enc[ep++];
        if (len & 0x80) {
          if (ep >= en)
            break;
          uint32_t corr = enc[ep++];
          for (len = (len & 0x7f) + 2; len > 0 && dp < n; len--, dp++)
            w[dp] = (uint16_t)((((dp < zero ? nm[dp] : 0) + corr) & 0xff) + hi);
        } else
          for (len += 2; len > 0 && dp < n; len--, dp++)
            w[dp] = dp < zero ? nm[dp] : 0;
      }
      }
      flags = (flags << 2) & 0xff;
      fbits -= 2;
    }
    char *s = utf16_to_utf8(w, dp);
    free(w);
    return s;
  }
  if (utf8_ok(nm, zero)) {
    char *s = malloc(zero + 1);
    if (s) {
      memcpy(s, nm, zero);
      s[zero] = 0;
    }
    return s;
  }
  return latin1_to_utf8(nm, zero);
}

static void norm_sep(char *s)
{
  for (; *s; s++)
    if (*s == '\\')
      *s = '/';
}

/* ------------------------------------------------------------------ RAR 5 headers */

struct h5 {
  uint64_t type, flags, extra_size, data_size;
  const uint8_t *body;
  uint32_t body_len;
  uint64_t data_off;
};

static int read_h5(struct rar *r, uint64_t off, struct h5 *h)
{
  uint8_t pre[7];
  uint32_t total, vlen;
  uint64_t size;
  int n;
  if (!r->hdr_enc) {
    n = rar_vol_read(r, &r->hv, off, pre, 7);
    if (n < 0)
      return n;
    if (n == 0)
      return 0;
    if (n < 7)
      return RAR_EFORMAT;
    const uint8_t *p = pre + 4;
    if (!get_vint(&p, pre + 7, &size))
      return RAR_EFORMAT;
    vlen = (uint32_t)(p - (pre + 4));
    if (size == 0 || size > RAR_MAX_HDR)
      return RAR_EFORMAT;
    total = 4 + vlen + (uint32_t)size;
    int rc = ensure_hbuf(r, total + 16);
    if (rc)
      return rc;
    n = rar_vol_read(r, &r->hv, off, r->hbuf, total);
    if (n < 0)
      return n;
    if ((uint32_t)n < total)
      return RAR_EFORMAT;
    if (le32(r->hbuf) != crc32(0, r->hbuf + 4, total - 4))
      return RAR_ECRC;
    h->data_off = off + total;
  } else {
    uint8_t blk[32];
    n = rar_vol_read(r, &r->hv, off, blk, 32);
    if (n < 0)
      return n;
    if (n == 0)
      return 0;
    if (n < 32)
      return RAR_EFORMAT;
    uint8_t iv[16];
    memcpy(iv, blk, 16);
    rar_aes_cbc_decrypt(&r->haes, iv, blk + 16, 16);
    const uint8_t *p = blk + 20;
    if (!get_vint(&p, blk + 32, &size) || size == 0 || size > RAR_MAX_HDR)
      return RAR_EPASSWORD;
    vlen = (uint32_t)(p - (blk + 20));
    total = 4 + vlen + (uint32_t)size;
    uint32_t padded = (total + 15) & ~15u;
    int rc = ensure_hbuf(r, padded + 16);
    if (rc)
      return rc;
    n = rar_vol_read(r, &r->hv, off, r->hbuf, 16 + padded);
    if (n < 0)
      return n;
    if ((uint32_t)n < 16 + padded)
      return RAR_EFORMAT;
    memcpy(iv, r->hbuf, 16);
    memmove(r->hbuf, r->hbuf + 16, padded);
    rar_aes_cbc_decrypt(&r->haes, iv, r->hbuf, padded);
    if (le32(r->hbuf) != crc32(0, r->hbuf + 4, total - 4))
      return RAR_EPASSWORD;
    h->data_off = off + 16 + padded;
  }
  const uint8_t *p = r->hbuf + 4 + vlen, *end = r->hbuf + total;
  uint64_t type, flags, extra = 0, data = 0;
  if (!get_vint(&p, end, &type) || !get_vint(&p, end, &flags))
    return RAR_EFORMAT;
  if ((flags & 1) && !get_vint(&p, end, &extra))
    return RAR_EFORMAT;
  if ((flags & 2) && !get_vint(&p, end, &data))
    return RAR_EFORMAT;
  if (extra > (uint64_t)(end - p))
    return RAR_EFORMAT;
  h->type = type;
  h->flags = flags;
  h->extra_size = extra;
  h->data_size = data;
  h->body = p;
  h->body_len = (uint32_t)(end - p);
  return 1;
}

static int parse_extra5(struct h5 *h, struct fh *o)
{
  const uint8_t *p = h->body + h->body_len - h->extra_size, *end = h->body + h->body_len;
  while (p < end) {
    uint64_t rsize, type;
    if (!get_vint(&p, end, &rsize) || rsize == 0 || rsize > (uint64_t)(end - p))
      return RAR_EFORMAT;
    const uint8_t *rec = p, *rend = p + rsize;
    if (!get_vint(&p, rend, &type))
      return RAR_EFORMAT;
    uint64_t v, flags;
    switch (type) {
    case 1: {
      struct rar_enc *e = &o->c.enc;
      if (!get_vint(&p, rend, &v) || !get_vint(&p, rend, &flags))
        return RAR_EFORMAT;
      if (v != 0)
        return RAR_EUNSUPPORTED;
      if (rend - p < 1 + 16 + 16 + ((flags & 1) ? 12 : 0))
        return RAR_EFORMAT;
      e->ver = 5;
      e->lg2 = *p++;
      memcpy(e->salt, p, 16);
      memcpy(e->iv, p + 16, 16);
      p += 32;
      e->has_check = (flags & 1) != 0;
      e->hash_mac = (flags & 2) != 0;
      if (e->has_check)
        memcpy(e->check, p, 8);
      o->f.encrypted = true;
      break;
    }
    case 2:
      if (!get_vint(&p, rend, &v))
        return RAR_EFORMAT;
      if (v == 0 && rend - p >= 32) {
        memcpy(o->f.b2, p, 32);
        o->f.has_b2 = true;
      }
      break;
    case 3: {
      if (!get_vint(&p, rend, &flags))
        return RAR_EFORMAT;
      if (flags & 2) {
        if (flags & 1) {
          if (rend - p < 4)
            return RAR_EFORMAT;
          o->f.mtime = le32(p);
        } else {
          if (rend - p < 8)
            return RAR_EFORMAT;
          uint64_t ft = le32(p) | ((uint64_t)le32(p + 4) << 32);
          uint64_t s = ft / 10000000ull;
          o->f.mtime = s > 11644473600ull && s - 11644473600ull <= 0xFFFFFFFFull ? (uint32_t)(s - 11644473600ull) : 0;
        }
      }
      break;
    }
    case 5: {
      uint64_t rtype, rflags, nlen;
      if (!get_vint(&p, rend, &rtype) || !get_vint(&p, rend, &rflags) || !get_vint(&p, rend, &nlen) ||
          nlen > (uint64_t)(rend - p))
        return RAR_EFORMAT;
      char *t = malloc((size_t)nlen + 1);
      if (!t)
        return RAR_ENOMEM;
      memcpy(t, p, (size_t)nlen);
      t[nlen] = 0;
      norm_sep(t);
      free(o->f.link);
      o->f.link = t;
      if (rtype == 4 || rtype == 5)
        o->f.is_hardlink = true;
      else
        o->f.is_symlink = true;
      break;
    }
    default:
      break;
    }
    p = rec + rsize;
  }
  return RAR_OK;
}

static int parse_file5(struct h5 *h, struct fh *o)
{
  const uint8_t *p = h->body, *end = h->body + h->body_len - h->extra_size;
  uint64_t fflags, size, attr, cinfo, host, nlen;
  struct rar_file *f = &o->f;
  if (!get_vint(&p, end, &fflags) || !get_vint(&p, end, &size) || !get_vint(&p, end, &attr))
    return RAR_EFORMAT;
  if (fflags & 2) {
    if (end - p < 4)
      return RAR_EFORMAT;
    f->mtime = le32(p);
    p += 4;
  }
  if (fflags & 4) {
    if (end - p < 4)
      return RAR_EFORMAT;
    f->crc = le32(p);
    f->has_crc = true;
    p += 4;
  }
  if (!get_vint(&p, end, &cinfo) || !get_vint(&p, end, &host) || !get_vint(&p, end, &nlen) ||
      nlen > (uint64_t)(end - p) || nlen == 0)
    return RAR_EFORMAT;
  f->name = malloc((size_t)nlen + 1);
  if (!f->name)
    return RAR_ENOMEM;
  memcpy(f->name, p, (size_t)nlen);
  f->name[nlen] = 0;
  if (strlen(f->name) != nlen)
    return RAR_EFORMAT;
  norm_sep(f->name);
  f->is_dir = (fflags & 1) != 0;
  f->size_unknown = (fflags & 8) != 0;
  f->size = size;
  f->attr = (uint32_t)attr;
  f->host = host == 1 ? 1 : 0;
  f->mode = f->host ? (uint32_t)(attr & 07777) : 0;
  f->version = 50;
  unsigned ver = cinfo & 0x3f;
  f->comp_solid = (cinfo >> 6) & 1;
  f->method = (cinfo >> 7) & 7;
  if (ver > 1)
    return RAR_EUNSUPPORTED;
  unsigned shift = (cinfo >> 10) & 0x1f;
  f->dict = 0x20000ull << shift;
  if (ver == 1)
    f->dict += (f->dict >> 5) * ((cinfo >> 15) & 0x1f);
  f->solid = f->comp_solid;
  f->v7 = ver == 1;
  o->split_before = (h->flags & 8) != 0;
  o->split_after = (h->flags & 0x10) != 0;
  o->c.size = h->data_size;
  o->c.off = h->data_off;
  return parse_extra5(h, o);
}

/* ------------------------------------------------------------------ RAR 3/4 headers */

struct h4 {
  uint32_t type, flags, size;
  uint64_t add_size, data_off;
  const uint8_t *b;
};

static int read_h4(struct rar *r, uint64_t off, struct h4 *h)
{
  uint8_t pre[16];
  int n;
  uint32_t hsize;
  if (!r->hdr_enc) {
    n = rar_vol_read(r, &r->hv, off, pre, 7);
    if (n < 0)
      return n;
    if (n == 0)
      return 0;
    if (n < 7)
      return RAR_EFORMAT;
    hsize = le16(pre + 5);
    if (hsize < 7)
      return RAR_EFORMAT;
    int rc = ensure_hbuf(r, hsize + 16);
    if (rc)
      return rc;
    n = rar_vol_read(r, &r->hv, off, r->hbuf, hsize);
    if (n < 0)
      return n;
    if ((uint32_t)n < hsize)
      return RAR_EFORMAT;
    h->data_off = off + hsize;
  } else {
    uint8_t blk[24];
    n = rar_vol_read(r, &r->hv, off, blk, 24);
    if (n < 0)
      return n;
    if (n == 0)
      return 0;
    if (n < 24)
      return RAR_EFORMAT;
    struct rar_enc e;
    memset(&e, 0, sizeof e);
    e.ver = 3;
    memcpy(e.salt, blk, 8);
    int rc = rar_get_key(r, &e, &r->hkc);
    if (rc)
      return rc;
    struct rar_aes a;
    rar_aes_init(&a, r->hkc.key, 16);
    uint8_t iv[16], first[16];
    memcpy(iv, r->hkc.iv, 16);
    memcpy(first, blk + 8, 16);
    rar_aes_cbc_decrypt(&a, iv, first, 16);
    hsize = le16(first + 5);
    if (hsize < 7)
      return RAR_EPASSWORD;
    uint32_t padded = (hsize + 15) & ~15u;
    rc = ensure_hbuf(r, padded + 16);
    if (rc)
      return rc;
    n = rar_vol_read(r, &r->hv, off + 8, r->hbuf, padded);
    if (n < 0)
      return n;
    if ((uint32_t)n < padded)
      return RAR_EFORMAT;
    memcpy(iv, r->hkc.iv, 16);
    rar_aes_cbc_decrypt(&a, iv, r->hbuf, padded);
    h->data_off = off + 8 + padded;
  }
  const uint8_t *b = r->hbuf;
  h->b = b;
  h->size = hsize;
  h->type = b[2];
  h->flags = le16(b + 3);
  h->add_size = 0;
  if (h->flags & 0x8000) {
    if (hsize < 11)
      return RAR_EFORMAT;
    h->add_size = le32(b + 7);
  }
  if (r->hdr_enc || h->type == 0x73 || h->type == 0x74 || h->type == 0x7a || h->type == 0x7b) {
    if (le16(b) != (crc32(0, b + 2, hsize - 2) & 0xffff))
      return r->hdr_enc ? RAR_EPASSWORD : RAR_ECRC;
  }
  return 1;
}

static int parse_file4(struct rar *r, struct h4 *h, struct fh *o)
{
  const uint8_t *b = h->b;
  struct rar_file *f = &o->f;
  uint32_t hs = h->size;
  if (hs < 32)
    return RAR_EFORMAT;
  uint64_t pack = le32(b + 7), unp = le32(b + 11);
  uint32_t fcrc = le32(b + 16), ftime = le32(b + 20);
  unsigned unp_ver = b[24], method = b[25];
  uint32_t nlen = le16(b + 26), attr = le32(b + 28);
  uint32_t pos = 32;
  if (h->flags & 0x100) {
    if (hs < 40)
      return RAR_EFORMAT;
    pack |= (uint64_t)le32(b + 32) << 32;
    unp |= (uint64_t)le32(b + 36) << 32;
    pos = 40;
  }
  if (nlen > hs - pos)
    return RAR_EFORMAT;
  f->name = decode_name4(b + pos, nlen, (h->flags & 0x200) != 0);
  if (!f->name)
    return RAR_ENOMEM;
  pos += nlen;
  if (h->flags & 0x400) {
    if (hs - pos < 8)
      return RAR_EFORMAT;
    o->c.enc.ver = 3;
    memcpy(o->c.enc.salt, b + pos, 8);
    f->encrypted = true;
  } else if (h->flags & 4) {
    return RAR_EFORMAT;
  }
  norm_sep(f->name);
  f->has_crc = true;
  f->crc = fcrc;
  f->size = unp;
  f->mtime = dos_to_unix(ftime);
  f->attr = attr;
  f->host = b[15] == 3 ? 1 : 0;
  f->method = method >= 0x30 && method <= 0x35 ? (uint8_t)(method - 0x30) : 0xff;
  f->comp_solid = (h->flags & 0x10) != 0;
  f->solid = f->comp_solid;
  f->is_dir = (h->flags & 0xe0) == 0xe0;
  if (f->host) {
    f->mode = attr & 07777;
    if ((attr & 0xf000) == 0xa000)
      f->is_symlink = true;
    if ((attr & 0xf000) == 0x4000)
      f->is_dir = true;
  } else if (attr & 0x10)
    f->is_dir = true;
  f->version = (uint8_t)unp_ver;
  f->dict = 0x400000;
  o->split_before = (h->flags & 1) != 0;
  o->split_after = (h->flags & 2) != 0;
  o->c.size = pack;
  o->c.off = h->data_off;
  (void)r;
  return RAR_OK;
}

/* ------------------------------------------------------------------ iteration */

int rar_start_volume(struct rar *r)
{
  uint64_t end;
  int fmt;
  int rc = rar_find_sig(r, &r->hv, &end, &fmt);
  if (rc)
    return rc;
  if (fmt != r->fmt)
    return RAR_EFORMAT;
  r->hoff = end;
  r->hdr_enc = false;
  r->haes_ready = false;
  return RAR_OK;
}

int rar_next_volume(struct rar *r)
{
  int rc = rar_open_volume(r, &r->hv, r->hv.idx + 1);
  if (rc)
    return rc;
  return rar_start_volume(r);
}

static int scan5(struct rar *r, bool stop_at_main, struct fh *fh)
{
  for (;;) {
    struct h5 h;
    int rc = read_h5(r, r->hoff, &h);
    if (rc <= 0)
      return rc;
    uint64_t next = h.data_off + h.data_size;
    if (next < h.data_off)
      return RAR_EFORMAT;
    switch (h.type) {
    case 4: {
      const uint8_t *p = h.body, *end = h.body + h.body_len;
      uint64_t ver, flags;
      if (!get_vint(&p, end, &ver) || !get_vint(&p, end, &flags))
        return RAR_EFORMAT;
      if (ver != 0)
        return RAR_EUNSUPPORTED;
      if (end - p < 1 + 16 + ((flags & 1) ? 12 : 0))
        return RAR_EFORMAT;
      struct rar_enc e;
      memset(&e, 0, sizeof e);
      e.ver = 5;
      e.lg2 = *p++;
      memcpy(e.salt, p, 16);
      p += 16;
      e.has_check = (flags & 1) != 0;
      if (e.has_check)
        memcpy(e.check, p, 8);
      rc = rar_get_key(r, &e, &r->hkc);
      if (rc)
        return rc;
      rar_aes_init(&r->haes, r->hkc.key, 32);
      r->hdr_enc = true;
      r->haes_ready = true;
      break;
    }
    case 1: {
      const uint8_t *p = h.body, *end = h.body + h.body_len;
      uint64_t flags;
      if (!get_vint(&p, end, &flags))
        return RAR_EFORMAT;
      if (r->hv.idx == 0 && !r->main_seen) {
        r->solid = (flags & 4) != 0;
        r->multivol = (flags & 1) != 0;
      }
      r->main_seen = true;
      if (stop_at_main) {
        r->hoff = next;
        return 1;
      }
      break;
    }
    case 2: {
      memset(fh, 0, sizeof *fh);
      rc = parse_file5(&h, fh);
      if (rc) {
        rar_file_free(&fh->f);
        return rc;
      }
      r->hoff = next;
      return 2;
    }
    case 5: {
      const uint8_t *p = h.body, *end = h.body + h.body_len;
      uint64_t flags = 0;
      if (!get_vint(&p, end, &flags))
        return RAR_EFORMAT;
      if (flags & 1) {
        rc = rar_next_volume(r);
        if (rc)
          return rc == RAR_EVOLUME ? RAR_EVOLUME : rc;
        continue;
      }
      return 0;
    }
    default:
      break;
    }
    r->hoff = next;
  }
}

static int scan4(struct rar *r, bool stop_at_main, struct fh *fh)
{
  for (;;) {
    struct h4 h;
    int rc = read_h4(r, r->hoff, &h);
    if (rc <= 0)
      return rc;
    uint64_t next = h.data_off;
    if (h.type != 0x74 && (h.flags & 0x8000))
      next += h.add_size;
    switch (h.type) {
    case 0x73:
      if (r->hv.idx == 0 && !r->main_seen) {
        r->solid = (h.flags & 8) != 0;
        r->multivol = (h.flags & 1) != 0;
      }
      r->main_seen = true;
      if (h.flags & 0x80)
        r->hdr_enc = true;
      if (stop_at_main) {
        r->hoff = next;
        return 1;
      }
      break;
    case 0x74:
      memset(fh, 0, sizeof *fh);
      rc = parse_file4(r, &h, fh);
      if (rc) {
        rar_file_free(&fh->f);
        return rc;
      }
      r->hoff = next + fh->c.size;
      return 2;
    case 0x7b:
      if (h.flags & 1) {
        rc = rar_next_volume(r);
        if (rc)
          return rc;
        continue;
      }
      return 0;
    default:
      break;
    }
    r->hoff = next;
  }
}

/* 1 = reached main (stop_at_main), 2 = file header in fh, 0 = end, < 0 = error */
int rar_scan(struct rar *r, bool stop_at_main, struct fh *fh)
{
  return r->fmt == 5 ? scan5(r, stop_at_main, fh) : scan4(r, stop_at_main, fh);
}
