/* pkgdb.c: parse, write and query the installed-file manifest (see pkgdb.h). */
#include "pkgdb.h"
#include "crc32c.h"

#define PKGDB_FORMAT 1u
#define RECORD_FIXED 12u

static const uint8_t magic[4] = {'O', 'W', 'P', 'K'};

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

static int str_cmp(const char *a, const char *b)
{
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return (unsigned char)*a - (unsigned char)*b;
}

static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8u; }
static uint32_t get32(const uint8_t *p) { return get16(p) | get16(p + 2) << 16u; }

static void put16(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8u);
}

static void put32(uint8_t *p, uint32_t v)
{
  put16(p, v);
  put16(p + 2, v >> 16u);
}

static uint32_t body_crc(const uint8_t *buf, uint32_t len)
{
  return crc32c_raw(0xFFFFFFFFu, buf + PKGDB_HEADER_SIZE, len - PKGDB_HEADER_SIZE) ^ 0xFFFFFFFFu;
}

/* Length including the NUL. */
static uint32_t str_size(const char *s)
{
  uint32_t n = 1;
  while (*s++)
    n++;
  return n;
}

/* True if the len bytes at s are one NUL-terminated string with no NUL before the end. */
static bool is_cstring(const uint8_t *s, uint32_t len)
{
  if (len == 0 || s[len - 1])
    return false;
  for (uint32_t i = 0; i + 1 < len; i++)
    if (!s[i])
      return false;
  return true;
}

int pkgdb_count(const uint8_t *buf, uint32_t len)
{
  if (len < PKGDB_HEADER_SIZE)
    return -1;
  for (int i = 0; i < 4; i++)
    if (buf[i] != magic[i])
      return -1;
  uint32_t n = get32(buf + 8);
  if (get16(buf + 4) != PKGDB_FORMAT || get16(buf + 6) != 0 || n > (len - PKGDB_HEADER_SIZE) / RECORD_FIXED)
    return -1;
  return get32(buf + 12) == body_crc(buf, len) ? (int)n : -1;
}

int pkgdb_parse(const uint8_t *buf, uint32_t len, struct pkg_rec *recs, int max)
{
  int n = pkgdb_count(buf, len);
  if (n < 0 || n > max)
    return -1;
  uint32_t at = PKGDB_HEADER_SIZE;
  for (int i = 0; i < n; i++) {
    if (len - at < RECORD_FIXED)
      return -1;
    const uint8_t *p = buf + at;
    uint32_t vlen = p[9], plen = get16(p + 10);
    if (p[8] > PKG_UPDATE || len - at - RECORD_FIXED < vlen + plen)
      return -1;
    const uint8_t *version = p + RECORD_FIXED, *path = version + vlen;
    if (!is_cstring(version, vlen) || !is_cstring(path, plen) || path[0] != '/')
      return -1;
    recs[i] =
        (struct pkg_rec){get32(p), get32(p + 4), (enum pkg_origin)p[8], (const char *)version, (const char *)path};
    at += RECORD_FIXED + vlen + plen;
  }
  return at == len ? n : -1;
}

uint32_t pkgdb_record_size(const struct pkg_rec *r) { return RECORD_FIXED + str_size(r->version) + str_size(r->path); }

uint32_t pkgdb_init(uint8_t *out, uint32_t cap)
{
  if (cap < PKGDB_HEADER_SIZE)
    return 0;
  for (int i = 0; i < 4; i++)
    out[i] = magic[i];
  put16(out + 4, PKGDB_FORMAT);
  put16(out + 6, 0);
  put32(out + 8, 0);
  put32(out + 12, body_crc(out, PKGDB_HEADER_SIZE));
  return PKGDB_HEADER_SIZE;
}

int pkgdb_append(uint8_t *out, uint32_t cap, uint32_t *len, const struct pkg_rec *r)
{
  uint32_t vlen = str_size(r->version), plen = str_size(r->path), need = RECORD_FIXED + vlen + plen;
  if (r->origin > PKG_UPDATE || r->path[0] != '/' || vlen > 0xFFu || plen > 0xFFFFu || *len > cap || cap - *len < need)
    return -1;
  uint8_t *p = out + *len;
  put32(p, r->crc);
  put32(p + 4, r->size);
  p[8] = (uint8_t)r->origin;
  p[9] = (uint8_t)vlen;
  put16(p + 10, plen);
  for (uint32_t i = 0; i < vlen; i++)
    p[RECORD_FIXED + i] = (uint8_t)r->version[i];
  for (uint32_t i = 0; i < plen; i++)
    p[RECORD_FIXED + vlen + i] = (uint8_t)r->path[i];

  /* Extend the stored checksum over the new record instead of rehashing the whole body. */
  uint32_t raw = get32(out + 12) ^ 0xFFFFFFFFu;
  put32(out + 12, crc32c_raw(raw, p, need) ^ 0xFFFFFFFFu);
  put32(out + 8, get32(out + 8) + 1);
  *len += need;
  return 0;
}

void pkgdb_sort(struct pkg_rec *recs, int n)
{
  for (int i = 1; i < n; i++) {
    struct pkg_rec r = recs[i];
    int j = i;
    for (; j > 0 && str_cmp(recs[j - 1].path, r.path) > 0; j--)
      recs[j] = recs[j - 1];
    recs[j] = r;
  }
}

const struct pkg_rec *pkgdb_find(const struct pkg_rec *recs, int n, const char *path)
{
  int lo = 0, hi = n;
  while (lo < hi) {
    int mid = lo + (hi - lo) / 2;
    int c = str_cmp(recs[mid].path, path);
    if (!c)
      return &recs[mid];
    if (c < 0)
      lo = mid + 1;
    else
      hi = mid;
  }
  return 0;
}

/* major, minor, patch, then commits past that tag. */
static bool ver_parse(const char *s, uint32_t v[4])
{
  v[0] = v[1] = v[2] = v[3] = 0;
  if (*s == 'v')
    s++;
  if (!is_digit(*s))
    return false;
  for (int i = 0; i < 3; i++) {
    while (is_digit(*s))
      v[i] = v[i] * 10 + (uint32_t)(*s++ - '0');
    if (*s != '.' || !is_digit(s[1]))
      break;
    s++;
  }
  if (*s == '-' && is_digit(s[1])) {
    uint32_t commits = 0;
    const char *q = s + 1;
    while (is_digit(*q))
      commits = commits * 10 + (uint32_t)(*q++ - '0');
    if (q[0] == '-' && q[1] == 'g')
      v[3] = commits;
  }
  return true;
}

bool pkgdb_vercmp(const char *a, const char *b, int *cmp)
{
  uint32_t va[4], vb[4];
  if (!ver_parse(a, va) || !ver_parse(b, vb))
    return false;
  *cmp = 0;
  for (int i = 0; i < 4 && !*cmp; i++)
    *cmp = va[i] < vb[i] ? -1 : va[i] > vb[i];
  return true;
}

enum pkg_action pkgdb_decide(const struct pkg_rec *rec, int disk_size, uint32_t crc, uint32_t size,
                             const char *base_version)
{
  if (!rec || disk_size < 0 || (uint32_t)disk_size != rec->size)
    return PKG_INSTALL;
  if (rec->origin == PKG_BASE)
    return rec->crc == crc && rec->size == size ? PKG_KEEP : PKG_INSTALL;
  int cmp;
  return pkgdb_vercmp(rec->version, base_version, &cmp) && cmp >= 0 ? PKG_KEEP : PKG_INSTALL;
}
