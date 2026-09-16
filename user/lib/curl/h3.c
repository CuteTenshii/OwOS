/* h3.c — HTTP/3 (RFC 9114) framing + QPACK (RFC 9204) over the QUIC
 * transport (quic.c). See h3.h. One request on stream 0; QPACK uses the static
 * table only (we advertise a zero-capacity dynamic table via empty SETTINGS),
 * so the request encodes with static indices/literals and the response decodes
 * without any dynamic-table state. HPACK's Huffman code (shared by QPACK) is
 * reused here for literal-string decoding. */

#include "h3.h"
#include "owos.h"
#include "quic.h"

/* ------------------------------------------------------------ QPACK Huffman */
/* RFC 7541 Appendix B canonical Huffman code (same table HPACK uses). Decode
 * only — the request side emits raw literals. */
static const struct {
  unsigned code;
  unsigned char nbits;
} HUFF[257] = {
    {0x1ff8, 13},    {0x7fffd8, 23},   {0xfffffe2, 28}, {0xfffffe3, 28},  {0xfffffe4, 28},  {0xfffffe5, 28},
    {0xfffffe6, 28}, {0xfffffe7, 28},  {0xfffffe8, 28}, {0xffffea, 24},   {0x3ffffffc, 30}, {0xfffffe9, 28},
    {0xfffffea, 28}, {0x3ffffffd, 30}, {0xfffffeb, 28}, {0xfffffec, 28},  {0xfffffed, 28},  {0xfffffee, 28},
    {0xfffffef, 28}, {0xffffff0, 28},  {0xffffff1, 28}, {0xffffff2, 28},  {0x3ffffffe, 30}, {0xffffff3, 28},
    {0xffffff4, 28}, {0xffffff5, 28},  {0xffffff6, 28}, {0xffffff7, 28},  {0xffffff8, 28},  {0xffffff9, 28},
    {0xffffffa, 28}, {0xffffffb, 28},  {0x14, 6},       {0x3f8, 10},      {0x3f9, 10},      {0xffa, 12},
    {0x1ff9, 13},    {0x15, 6},        {0xf8, 8},       {0xffb, 12},      {0x3fa, 10},      {0x3fb, 10},
    {0xf9, 8},       {0xffc, 12},      {0xfa, 8},       {0x16, 6},        {0x17, 6},        {0x18, 6},
    {0x0, 5},        {0x1, 5},         {0x2, 5},        {0x19, 6},        {0x1a, 6},        {0x1b, 6},
    {0x1c, 6},       {0x1d, 6},        {0x1e, 6},       {0x1f, 6},        {0x5c, 7},        {0xfb, 8},
    {0x7ffc, 15},    {0x20, 6},        {0xffd, 12},     {0x3fc, 10},      {0x1ffa, 13},     {0x21, 6},
    {0x5d, 7},       {0x5e, 7},        {0x5f, 7},       {0x60, 7},        {0x61, 7},        {0x62, 7},
    {0x63, 7},       {0x64, 7},        {0x65, 7},       {0x66, 7},        {0x67, 7},        {0x68, 7},
    {0x69, 7},       {0x6a, 7},        {0x6b, 7},       {0x6c, 7},        {0x6d, 7},        {0x6e, 7},
    {0x6f, 7},       {0x70, 7},        {0x71, 7},       {0x72, 7},        {0xfc, 8},        {0x73, 7},
    {0xfd, 8},       {0x1ffb, 13},     {0x7fff0, 19},   {0x1ffc, 13},     {0x3ffc, 14},     {0x22, 6},
    {0x7ffd, 15},    {0x3, 5},         {0x23, 6},       {0x4, 5},         {0x24, 6},        {0x5, 5},
    {0x25, 6},       {0x26, 6},        {0x27, 6},       {0x6, 5},         {0x74, 7},        {0x75, 7},
    {0x28, 6},       {0x29, 6},        {0x2a, 6},       {0x7, 5},         {0x2b, 6},        {0x76, 7},
    {0x2c, 6},       {0x8, 5},         {0x9, 5},        {0x2d, 6},        {0x77, 7},        {0x78, 7},
    {0x79, 7},       {0x7a, 7},        {0x7b, 7},       {0x7ffe, 15},     {0x7fc, 11},      {0x3ffd, 14},
    {0x1ffd, 13},    {0xffffffc, 28},  {0xfffe6, 20},   {0x3fffd2, 22},   {0xfffe7, 20},    {0xfffe8, 20},
    {0x3fffd3, 22},  {0x3fffd4, 22},   {0x3fffd5, 22},  {0x7fffd9, 23},   {0x3fffd6, 22},   {0x7fffda, 23},
    {0x7fffdb, 23},  {0x7fffdc, 23},   {0x7fffdd, 23},  {0x7fffde, 23},   {0xffffeb, 24},   {0x7fffdf, 23},
    {0xffffec, 24},  {0xffffed, 24},   {0x3fffd7, 22},  {0x7fffe0, 23},   {0xffffee, 24},   {0x7fffe1, 23},
    {0x3fffd8, 22},  {0x7fffe2, 23},   {0x7fffe3, 23},  {0x7fffe4, 23},   {0x1fffdc, 21},   {0x3fffd9, 22},
    {0x7fffe5, 23},  {0x3fffda, 22},   {0x7fffe6, 23},  {0x7fffe7, 23},   {0xffffef, 24},   {0x3fffdb, 22},
    {0x1fffdd, 21},  {0xfffe9, 20},    {0x3fffdc, 22},  {0x3fffdd, 22},   {0x7fffe8, 23},   {0x7fffe9, 23},
    {0x1fffde, 21},  {0x7fffea, 23},   {0x3fffde, 22},  {0x7fffeb, 23},   {0x7fffec, 23},   {0x3fffdf, 22},
    {0x3fffe0, 22},  {0x7fffed, 23},   {0x3fffe1, 22},  {0x7fffee, 23},   {0x7fffef, 23},   {0x1fffdf, 21},
    {0x3fffe2, 22},  {0x3fffe3, 22},   {0x3fffe4, 22},  {0x7ffff0, 23},   {0x3fffe5, 22},   {0x1fffe0, 21},
    {0x7ffff1, 23},  {0x3fffe6, 22},   {0x3fffe7, 22},  {0x3fffe8, 22},   {0x1fffe1, 21},   {0x3ffffe0, 26},
    {0x3ffffe1, 26}, {0xfffeb, 20},    {0x7fff1, 19},   {0x3fffe9, 22},   {0x7ffff2, 23},   {0x3fffea, 22},
    {0x1ffffec, 25}, {0x3ffffe2, 26},  {0x3ffffe3, 26}, {0x3ffffe4, 26},  {0x7ffffde, 27},  {0x7ffffdf, 27},
    {0x3ffffe5, 26}, {0xfffff1, 24},   {0x1ffffed, 25}, {0x7fff2, 19},    {0x1fffe2, 21},   {0x3ffffe6, 26},
    {0x7ffffe0, 27}, {0x7ffffe1, 27},  {0x3ffffe7, 26}, {0x7ffffe2, 27},  {0xfffff2, 24},   {0x1fffe3, 21},
    {0x1fffe4, 21},  {0x3ffffe8, 26},  {0x3ffffe9, 26}, {0xffffffd, 28},  {0x7ffffe3, 27},  {0x7ffffe4, 27},
    {0x7ffffe5, 27}, {0xfffec, 20},    {0xfffff3, 24},  {0xfffed, 20},    {0x1fffe5, 21},   {0x3fffea, 22},
    {0x1fffe6, 21},  {0x1fffe7, 21},   {0x7ffff3, 23},  {0x3fffeb, 22},   {0x3fffec, 22},   {0x1ffffee, 25},
    {0x1ffffef, 25}, {0xfffff4, 24},   {0xfffff5, 24},  {0x3ffffea, 26},  {0x7ffff4, 23},   {0x3ffffeb, 26},
    {0x7ffffe6, 27}, {0x3ffffec, 26},  {0x3ffffed, 26}, {0x7ffffe7, 27},  {0x7ffffe8, 27},  {0x7ffffe9, 27},
    {0x7ffffea, 27}, {0x7ffffeb, 27},  {0xffffffe, 28}, {0x7ffffec, 27},  {0x7ffffed, 27},  {0x7ffffee, 27},
    {0x7ffffef, 27}, {0x7fffff0, 27},  {0x3ffffee, 26}, {0x3fffffff, 30},
};

static int huff_decode(const unsigned char *in, int inlen, char *out, int outcap, int *outlen)
{
  unsigned cur = 0;
  int nbits = 0, o = 0;
  for (int i = 0; i < inlen; i++) {
    for (int bit = 7; bit >= 0; bit--) {
      cur = (cur << 1) | ((in[i] >> bit) & 1u);
      nbits++;
      if (nbits > 30)
        return -1;
      int sym = -1;
      for (int s = 0; s < 256; s++)
        if (HUFF[s].nbits == (unsigned char)nbits && HUFF[s].code == cur) {
          sym = s;
          break;
        }
      if (sym >= 0) {
        if (o >= outcap)
          return -1;
        out[o++] = (char)sym;
        cur = 0;
        nbits = 0;
      }
    }
  }
  if (nbits >= 8)
    return -1;
  *outlen = o;
  return 0;
}

/* ----------------------------------------------------- QPACK static table */
/* RFC 9204 Appendix A, indices 0..98. */
#define QP_N 99
static const char *const QPN[QP_N] = {
    ":authority",
    ":path",
    "age",
    "content-disposition",
    "content-length",
    "cookie",
    "date",
    "etag",
    "if-modified-since",
    "if-none-match",
    "last-modified",
    "link",
    "location",
    "referer",
    "set-cookie",
    ":method",
    ":method",
    ":method",
    ":method",
    ":method",
    ":method",
    ":method",
    ":scheme",
    ":scheme",
    ":status",
    ":status",
    ":status",
    ":status",
    ":status",
    "accept",
    "accept",
    "accept-encoding",
    "accept-ranges",
    "access-control-allow-headers",
    "access-control-allow-headers",
    "access-control-allow-origin",
    "cache-control",
    "cache-control",
    "cache-control",
    "cache-control",
    "cache-control",
    "cache-control",
    "content-encoding",
    "content-encoding",
    "content-type",
    "content-type",
    "content-type",
    "content-type",
    "content-type",
    "content-type",
    "content-type",
    "content-type",
    "content-type",
    "content-type",
    "content-type",
    "range",
    "strict-transport-security",
    "strict-transport-security",
    "strict-transport-security",
    "vary",
    "vary",
    "x-content-type-options",
    "x-xss-protection",
    ":status",
    ":status",
    ":status",
    ":status",
    ":status",
    ":status",
    ":status",
    ":status",
    ":status",
    "accept-language",
    "access-control-allow-credentials",
    "access-control-allow-credentials",
    "access-control-allow-headers",
    "access-control-allow-methods",
    "access-control-allow-methods",
    "access-control-allow-methods",
    "access-control-expose-headers",
    "access-control-request-headers",
    "access-control-request-method",
    "access-control-request-method",
    "alt-svc",
    "authorization",
    "content-security-policy",
    "early-data",
    "expect-ct",
    "forwarded",
    "if-range",
    "origin",
    "purpose",
    "server",
    "timing-allow-origin",
    "upgrade-insecure-requests",
    "user-agent",
    "x-forwarded-for",
    "x-frame-options",
    "x-frame-options",
};
static const char *const QPV[QP_N] = {
    "",
    "/",
    "0",
    "",
    "0",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "CONNECT",
    "DELETE",
    "GET",
    "HEAD",
    "OPTIONS",
    "POST",
    "PUT",
    "http",
    "https",
    "103",
    "200",
    "304",
    "404",
    "503",
    "*/*",
    "application/dns-message",
    "gzip, deflate, br",
    "bytes",
    "cache-control",
    "content-type",
    "*",
    "max-age=0",
    "max-age=2592000",
    "max-age=604800",
    "no-cache",
    "no-store",
    "public, max-age=31536000",
    "br",
    "gzip",
    "application/dns-message",
    "application/javascript",
    "application/json",
    "application/x-www-form-urlencoded",
    "image/gif",
    "image/jpeg",
    "image/png",
    "text/css",
    "text/html; charset=utf-8",
    "text/plain",
    "text/plain;charset=utf-8",
    "bytes=0-",
    "max-age=31536000",
    "max-age=31536000; includesubdomains",
    "max-age=31536000; includesubdomains; preload",
    "accept-encoding",
    "origin",
    "nosniff",
    "1; mode=block",
    "100",
    "204",
    "206",
    "302",
    "400",
    "403",
    "421",
    "425",
    "500",
    "",
    "FALSE",
    "TRUE",
    "*",
    "get",
    "get, post, options",
    "options",
    "content-length",
    "content-type",
    "get",
    "post",
    "clear",
    "",
    "script-src 'none'; object-src 'none'; base-uri 'none'",
    "1",
    "",
    "",
    "",
    "",
    "prefetch",
    "",
    "*",
    "1",
    "",
    "",
    "deny",
    "sameorigin",
};

/* -------------------------------------------------- prefix integers / strings
 */

static int dec_int(const uint8_t *p, uint32_t *o, uint32_t len, int prefix, uint64_t *out)
{
  if (*o >= len)
    return 0;
  uint32_t max = (1u << prefix) - 1;
  uint64_t v = p[*o] & max;
  (*o)++;
  if (v < max) {
    *out = v;
    return 1;
  }
  int shift = 0;
  uint8_t b;
  do {
    if (*o >= len)
      return 0;
    b = p[(*o)++];
    v += (uint64_t)(b & 0x7f) << shift;
    shift += 7;
    if (shift > 63)
      return 0;
  } while (b & 0x80);
  *out = v;
  return 1;
}

/* Decode a QPACK string literal whose length prefix is `prefix` bits and whose
 * Huffman flag is bit `hbit` of the first byte. */
static int dec_str(const uint8_t *p, uint32_t *o, uint32_t len, int prefix, uint8_t hbit, char *out, int outcap,
                   int *outlen)
{
  if (*o >= len)
    return -1;
  int H = p[*o] & hbit;
  uint64_t slen;
  if (!dec_int(p, o, len, prefix, &slen))
    return -1;
  if (*o + slen > len)
    return -1;
  if (H) {
    if (huff_decode(p + *o, (int)slen, out, outcap, outlen) < 0)
      return -1;
  } else {
    uint32_t cp = slen < (uint64_t)outcap ? (uint32_t)slen : (uint32_t)outcap;
    memcpy(out, p + *o, cp);
    *outlen = (int)cp;
  }
  *o += (uint32_t)slen;
  return 0;
}

static int enc_int(uint8_t *b, uint32_t *p, uint64_t v, int prefix, uint8_t flags)
{
  uint32_t max = (1u << prefix) - 1;
  if (v < max) {
    b[(*p)++] = flags | (uint8_t)v;
  } else {
    b[(*p)++] = flags | (uint8_t)max;
    v -= max;
    while (v >= 128) {
      b[(*p)++] = (uint8_t)((v & 0x7f) | 0x80);
      v >>= 7;
    }
    b[(*p)++] = (uint8_t)v;
  }
  return 0;
}

static void enc_str(uint8_t *b, uint32_t *p, const char *s, int n)
{
  enc_int(b, p, (uint64_t)n, 7, 0x00); /* H=0 */
  memcpy(b + *p, s, n);
  *p += n;
}

/* --------------------------------------------------- response header sink */

static int h3_status;
static char h3_lines[4096]; /* decoded "name: value\r\n" block */
static int h3_lines_len;
static http_body_fn h3_body;
static void *h3_ctx;
static int h3_abort;

static void emit_field(const uint8_t *name, int nl, const uint8_t *val, int vl)
{
  if (nl == 7 && eq_ci_n(":status", (const char *)name, 7)) {
    int s = 0;
    for (int i = 0; i < vl && val[i] >= '0' && val[i] <= '9'; i++)
      s = s * 10 + (val[i] - '0');
    h3_status = s;
    return;
  }
  if (nl > 0 && name[0] == ':') /* skip other pseudo-headers */
    return;
  int need = nl + 2 + vl + 2;
  if (h3_lines_len + need > (int)sizeof h3_lines)
    return;
  memcpy(h3_lines + h3_lines_len, name, nl);
  h3_lines_len += nl;
  h3_lines[h3_lines_len++] = ':';
  h3_lines[h3_lines_len++] = ' ';
  memcpy(h3_lines + h3_lines_len, val, vl);
  h3_lines_len += vl;
  h3_lines[h3_lines_len++] = '\r';
  h3_lines[h3_lines_len++] = '\n';
}

/* Decode a QPACK encoded field section (response HEADERS payload). */
static void qpack_decode(const uint8_t *b, uint32_t len)
{
  uint32_t o = 0;
  uint64_t ric, base;
  if (!dec_int(b, &o, len, 8, &ric) || !dec_int(b, &o, len, 7, &base))
    return; /* prefix: insert count + base */
  while (o < len) {
    uint8_t c = b[o];
    char val[2048];
    int vl = 0;
    if (c & 0x80) { /* Indexed Field Line */
      int T = c & 0x40;
      uint64_t idx;
      if (!dec_int(b, &o, len, 6, &idx))
        return;
      if (T && idx < QP_N)
        emit_field((const uint8_t *)QPN[idx], (int)strlen(QPN[idx]), (const uint8_t *)QPV[idx], (int)strlen(QPV[idx]));
    } else if (c & 0x40) { /* Literal w/ Name Reference */
      int T = c & 0x10;
      uint64_t idx;
      if (!dec_int(b, &o, len, 4, &idx))
        return;
      if (dec_str(b, &o, len, 7, 0x80, val, sizeof val, &vl) < 0)
        return;
      if (T && idx < QP_N)
        emit_field((const uint8_t *)QPN[idx], (int)strlen(QPN[idx]), (const uint8_t *)val, vl);
    } else if (c & 0x20) { /* Literal w/ Literal Name */
      char nm[256];
      int nl = 0;
      if (dec_str(b, &o, len, 3, 0x08, nm, sizeof nm, &nl) < 0)
        return;
      if (dec_str(b, &o, len, 7, 0x80, val, sizeof val, &vl) < 0)
        return;
      emit_field((const uint8_t *)nm, nl, (const uint8_t *)val, vl);
    } else {
      return; /* post-base forms unsupported */
    }
  }
}

/* ----------------------------------------------- stream-0 frame parser */

static uint8_t h3_acc[16384];
static uint32_t h3_acclen;
static int h3_state; /* 0 = expect frame header, 1 = in payload */
static uint64_t h3_ftype, h3_frem;
static uint8_t h3_hb[8192]; /* HEADERS payload accumulator */
static uint32_t h3_hblen;

static void acc_consume(uint32_t k)
{
  memmove(h3_acc, h3_acc + k, h3_acclen - k);
  h3_acclen -= k;
}

static void feed_stream0(const uint8_t *data, uint32_t n)
{
  if (h3_acclen + n > sizeof h3_acc)
    n = sizeof h3_acc - h3_acclen;
  memcpy(h3_acc + h3_acclen, data, n);
  h3_acclen += n;

  for (;;) {
    if (h3_state == 0) { /* parse type + length varints */
      if (h3_acclen < 1)
        break;
      int tl = 1 << (h3_acc[0] >> 6);
      if ((uint32_t)tl > h3_acclen)
        break;
      uint64_t ty = h3_acc[0] & 0x3f;
      for (int i = 1; i < tl; i++)
        ty = (ty << 8) | h3_acc[i];
      if ((uint32_t)tl >= h3_acclen)
        break;
      int ll = 1 << (h3_acc[tl] >> 6);
      if ((uint32_t)(tl + ll) > h3_acclen)
        break;
      uint64_t ln = h3_acc[tl] & 0x3f;
      for (int i = 1; i < ll; i++)
        ln = (ln << 8) | h3_acc[tl + i];
      acc_consume(tl + ll);
      h3_ftype = ty;
      h3_frem = ln;
      h3_state = 1;
      if (ty == 0x01)
        h3_hblen = 0; /* HEADERS: start accumulating */
    } else {
      if (h3_acclen == 0)
        break;
      uint32_t take = h3_acclen;
      if (take > h3_frem)
        take = (uint32_t)h3_frem;
      if (h3_ftype == 0x00) { /* DATA -> body sink */
        if (h3_body && !h3_abort && take)
          if (h3_body(h3_ctx, h3_acc, (int)take) != 0)
            h3_abort = 1;
      } else if (h3_ftype == 0x01) { /* HEADERS -> accumulate */
        uint32_t room = sizeof h3_hb - h3_hblen;
        uint32_t cp = take < room ? take : room;
        memcpy(h3_hb + h3_hblen, h3_acc, cp);
        h3_hblen += cp;
      }
      acc_consume(take);
      h3_frem -= take;
      if (h3_frem == 0) {
        if (h3_ftype == 0x01)
          qpack_decode(h3_hb, h3_hblen);
        h3_state = 0;
      }
    }
  }
}

/* ----------------------------------------------------- request encoding */

/* QPACK-encode the request field section into b; returns its length. */
static uint32_t build_request(uint8_t *b, const char *method, const char *path, const char *authority,
                              const char *extra)
{
  uint32_t p = 0;
  b[p++] = 0x00;
  b[p++] = 0x00; /* Required Insert Count=0, Base=0 */

  if (streq(method, "GET"))
    enc_int(b, &p, 17, 6, 0xC0); /* Indexed: :method GET */
  else {
    enc_int(b, &p, 15, 4, 0x50); /* name ref ":method" */
    enc_str(b, &p, method, (int)strlen(method));
  }
  enc_int(b, &p, 23, 6, 0xC0); /* Indexed: :scheme https */
  if (streq(path, "/"))
    enc_int(b, &p, 1, 6, 0xC0); /* Indexed: :path / */
  else {
    enc_int(b, &p, 1, 4, 0x50); /* name ref ":path" */
    enc_str(b, &p, path, (int)strlen(path));
  }
  enc_int(b, &p, 0, 4, 0x50); /* name ref ":authority" */
  enc_str(b, &p, authority, (int)strlen(authority));

  /* extra "Name: value\r\n" lines (curl -H, incl. User-Agent), literal name +
   * value */
  for (const char *s = extra; s && *s;) {
    const char *eol = s;
    while (*eol && *eol != '\n')
      eol++;
    const char *colon = s;
    while (colon < eol && *colon != ':')
      colon++;
    if (colon < eol) {
      int nl = (int)(colon - s);
      while (nl > 0 && (s[nl - 1] == '\r'))
        nl--;
      const char *v = colon + 1;
      while (v < eol && (*v == ' ' || *v == '\t'))
        v++;
      int vl = (int)(eol - v);
      while (vl > 0 && (v[vl - 1] == '\r'))
        vl--;
      if (nl > 0 && nl < 200) {
        char lname[200];
        for (int i = 0; i < nl; i++)
          lname[i] = lower(s[i]);
        enc_int(b, &p, (uint64_t)nl, 3, 0x20);
        memcpy(b + p, lname, nl);
        p += nl;
        enc_str(b, &p, v, vl);
      }
    }
    s = *eol ? eol + 1 : eol;
  }
  return p;
}

static uint32_t put_qvar(uint8_t *p, uint64_t v)
{
  if (v < 0x40) {
    p[0] = (uint8_t)v;
    return 1;
  }
  if (v < 0x4000) {
    p[0] = 0x40 | (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
    return 2;
  }
  p[0] = 0x80 | (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
  return 4;
}

/* ------------------------------------------------------------- public */

int h3_run(const char *host, int port, const char *authority, const char *method, const char *path,
           const char *extra_headers, char *hdr, int hdrcap, int *hdrlen, http_body_fn body, void *ctx)
{
  h3_status = 0;
  h3_lines_len = 0;
  h3_abort = 0;
  h3_acclen = 0;
  h3_state = 0;
  h3_hblen = 0;
  h3_body = body;
  h3_ctx = ctx;
  if (!method)
    method = "GET";

  int rc = quic_connect(host, (uint16_t)port, "h3");
  if (rc != QUIC_OK)
    return rc == QUIC_EINTR ? HTTP_EINTR : rc == QUIC_ERESOLVE ? HTTP_ERESOLVE : HTTP_ECONNECT;

  /* Client control stream (2) + empty SETTINGS, then the QPACK encoder (6)
   * and decoder (10) streams (type byte only — we never use the dynamic table).
   */
  uint8_t ctrl[3] = {0x00, 0x04, 0x00};
  uint8_t enc[1] = {0x02}, dec[1] = {0x03};
  quic_send(2, ctrl, 3, 0);
  quic_send(6, enc, 1, 0);
  quic_send(10, dec, 1, 0);

  /* Request: HEADERS frame on stream 0, with FIN (no request body). */
  static uint8_t qbuf[4096];
  uint32_t qlen = build_request(qbuf, method, path, authority, extra_headers ? extra_headers : "");
  static uint8_t reqf[4200];
  uint32_t fp = 0;
  reqf[fp++] = 0x01; /* HEADERS frame type */
  fp += put_qvar(reqf + fp, qlen);
  memcpy(reqf + fp, qbuf, qlen);
  fp += qlen;
  if (quic_send(0, reqf, fp, 1) < 0) {
    quic_close();
    return HTTP_ESEND;
  }

  int idle = 0, fin = 0, done = 0;
  while (!done) {
    int r = quic_poll();
    if (r == QUIC_EINTR) {
      quic_close();
      return HTTP_EINTR;
    }
    if (r == QUIC_ECLOSED)
      break;
    if (r == 0) {
      if (++idle > 5)
        break;
      continue;
    }
    idle = 0;
    uint8_t tmp[4096];
    int got;
    while ((got = quic_read(0, tmp, sizeof tmp, &fin)) > 0) {
      feed_stream0(tmp, (uint32_t)got);
      if (h3_abort)
        break;
    }
    if (h3_abort) {
      quic_close();
      return HTTP_EABORT;
    }
    if (fin)
      done = 1;
  }
  quic_close();

  if (hdr && hdrcap > 0) {
    int n = 0;
    const char *pre = "HTTP/3 ";
    for (const char *q = pre; *q && n < hdrcap - 1; q++)
      hdr[n++] = *q;
    int s = h3_status;
    char num[8];
    int k = 0;
    if (s <= 0)
      s = 0;
    if (s == 0)
      num[k++] = '0';
    else {
      char t[8];
      int j = 0;
      while (s) {
        t[j++] = (char)('0' + s % 10);
        s /= 10;
      }
      while (j)
        num[k++] = t[--j];
    }
    for (int i = 0; i < k && n < hdrcap - 1; i++)
      hdr[n++] = num[i];
    if (n < hdrcap - 1)
      hdr[n++] = '\r';
    if (n < hdrcap - 1)
      hdr[n++] = '\n';
    for (int i = 0; i < h3_lines_len && n < hdrcap - 1; i++)
      hdr[n++] = h3_lines[i];
    if (n < hdrcap - 1)
      hdr[n++] = '\r';
    if (n < hdrcap - 1)
      hdr[n++] = '\n';
    hdr[n] = '\0';
    if (hdrlen)
      *hdrlen = n;
  }

  return h3_status > 0 ? h3_status : HTTP_ENORESP;
}
