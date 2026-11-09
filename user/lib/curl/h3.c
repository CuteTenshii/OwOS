/* h3.c — HTTP/3 (RFC 9114) framing + QPACK (RFC 9204) over the QUIC
 * transport (quic.c). See h3.h. A session is one QUIC connection carrying
 * request streams one after another (or a batch at once); QPACK uses the static
 * table only (we advertise a zero-capacity dynamic table via empty SETTINGS),
 * so the request encodes with static indices/literals and the response decodes
 * without any dynamic-table state. HPACK's Huffman code (shared by QPACK) is
 * reused here for literal-string decoding. */

#include "h3.h"
#include "netconn.h"
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
      *outlen = 0; /* too big for the buffer: skip the value, keep decoding the block */
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

#define H3_ACC 4096 /* inbound bytes carried between reads: a partial frame header */
#define H3_HB_MAX 16384

struct h3;

/* The request a server announced with PUSH_PROMISE. */
struct promise {
  int used;
  uint64_t push_id;
  char scheme[8], authority[256], path[1024];
};

/* One request stream's parser state and sinks. */
struct h3stream {
  struct h3 *H;
  struct promise *pr; /* decoding a PUSH_PROMISE's request: its pseudo-headers go here */
  uint64_t sid;
  int refused; /* the server's GOAWAY says it never processed this request */
  int done;    /* the response ended (FIN reached and drained)   */
  int aborted; /* a sink asked to stop                           */
  int bad;     /* the stream could not be decoded                */
  int headers_done;
  int state; /* 0 = expect frame header, 1 = in payload */
  uint64_t ftype, frem;
  uint8_t acc[H3_ACC + 32];
  uint32_t acclen;
  uint8_t *hb; /* HEADERS payload accumulator */
  uint32_t hblen;
  char *hbuf; /* decoded header block ("HTTP/3 NNN\r\nname: value\r\n…") */
  int hcap, hlen, status;
  char scratch[64]; /* header sink when the caller wants no header text */
  http_body_fn body;
  void *bctx;
  const struct h3_opts *opts;
};

#define H3_UNI_MAX 12
#define H3_PUSH_MAX 8 /* push ids 0..7: the MAX_PUSH_ID we grant is 7 */
#define H3_PUSH_BODY_MAX (1u << 20)

/* A stream the server opened: its control stream (GOAWAY), QPACK streams (ignored) or a push. */
struct h3uni {
  uint64_t sid;
  int type; /* -1 until the stream type is read; 0 control, 1 push, else ignored */
  uint8_t acc[16];
  int acclen; /* a varint being read */
  int cstate; /* control: 0 frame type, 1 frame length, 2 payload */
  uint64_t cftype, cfrem;
  uint8_t pay[8];
  int paylen;
  int pushed; /* push: the push id has been read */
  uint64_t push_id;
  struct h3stream *ps; /* push: parser of the response */
  char *phdr;
  unsigned char *pbody;
  int pblen, pbcap, too_big, complete;
};

struct h3 {
  quic *q;
  int broken; /* not safe for another request */
  int served;
  int goaway;
  uint64_t goaway_id;  /* requests on streams from here up were not processed */
  char authority[264]; /* host[:port] this session is connected to */
  struct promise prom[H3_PUSH_MAX];
  struct h3uni *uni[H3_UNI_MAX];
  uint8_t rx[H3_ACC]; /* read buffer, off the small stack */
};

static h3_push_fn g_push_fn;
static void *g_push_ctx;

void h3_set_push_handler(h3_push_fn fn, void *ctx)
{
  g_push_ctx = ctx;
  g_push_fn = fn;
}

static void hs_raw(struct h3stream *st, const char *p, int n)
{
  for (int i = 0; i < n && st->hlen < st->hcap - 1; i++)
    st->hbuf[st->hlen++] = p[i];
}

static void copy_field(char *dst, int cap, const uint8_t *v, int vl)
{
  int n = vl < cap - 1 ? vl : cap - 1;
  memcpy(dst, v, (unsigned)n);
  dst[n] = '\0';
}

static void emit_field(struct h3stream *st, const uint8_t *name, int nl, const uint8_t *val, int vl)
{
  if (st->pr) { /* a PUSH_PROMISE: only the request's address matters */
    if (nl == 10 && eq_ci_n(":authority", (const char *)name, 10))
      copy_field(st->pr->authority, sizeof st->pr->authority, val, vl);
    else if (nl == 5 && eq_ci_n(":path", (const char *)name, 5))
      copy_field(st->pr->path, sizeof st->pr->path, val, vl);
    else if (nl == 7 && eq_ci_n(":scheme", (const char *)name, 7))
      copy_field(st->pr->scheme, sizeof st->pr->scheme, val, vl);
    else if (nl == 7 && eq_ci_n(":method", (const char *)name, 7) && !(vl == 3 && memcmp(val, "GET", 3) == 0))
      st->pr->used = -1; /* only GET may be pushed */
    return;
  }
  if (st->headers_done)
    return; /* trailers: not kept */
  if (nl == 7 && eq_ci_n(":status", (const char *)name, 7)) {
    int s = 0;
    for (int i = 0; i < vl && val[i] >= '0' && val[i] <= '9'; i++)
      s = s * 10 + (val[i] - '0');
    st->status = s;
    hs_raw(st, "HTTP/3 ", 7);
    hs_raw(st, (const char *)val, vl);
    hs_raw(st, "\r\n", 2);
    return;
  }
  if (nl > 0 && name[0] == ':') /* skip other pseudo-headers */
    return;
  hs_raw(st, (const char *)name, nl);
  hs_raw(st, ": ", 2);
  hs_raw(st, (const char *)val, vl);
  hs_raw(st, "\r\n", 2);
}

/* Decode a QPACK encoded field section (response HEADERS payload). 0, or -1 if it is malformed or needs the dynamic
 * table. */
static int qpack_decode(struct h3stream *st, const uint8_t *b, uint32_t len)
{
  uint32_t o = 0;
  uint64_t ric, base;
  if (!dec_int(b, &o, len, 8, &ric) || !dec_int(b, &o, len, 7, &base) || ric != 0)
    return -1; /* prefix: insert count + base */
  char *val = malloc(H3_HB_MAX);
  if (!val)
    return -1;
  int rc = 0;
  while (o < len && rc == 0) {
    uint8_t c = b[o];
    int vl = 0;
    if (c & 0x80) { /* Indexed Field Line */
      int T = c & 0x40;
      uint64_t idx;
      if (!dec_int(b, &o, len, 6, &idx) || !T || idx >= QP_N)
        rc = -1;
      else
        emit_field(st, (const uint8_t *)QPN[idx], (int)strlen(QPN[idx]), (const uint8_t *)QPV[idx],
                   (int)strlen(QPV[idx]));
    } else if (c & 0x40) { /* Literal w/ Name Reference */
      int T = c & 0x10;
      uint64_t idx;
      if (!dec_int(b, &o, len, 4, &idx) || !T || idx >= QP_N || dec_str(b, &o, len, 7, 0x80, val, H3_HB_MAX, &vl) < 0)
        rc = -1;
      else
        emit_field(st, (const uint8_t *)QPN[idx], (int)strlen(QPN[idx]), (const uint8_t *)val, vl);
    } else if (c & 0x20) { /* Literal w/ Literal Name */
      char nm[256];
      int nl = 0;
      if (dec_str(b, &o, len, 3, 0x08, nm, sizeof nm, &nl) < 0 || dec_str(b, &o, len, 7, 0x80, val, H3_HB_MAX, &vl) < 0)
        rc = -1;
      else
        emit_field(st, (const uint8_t *)nm, nl, (const uint8_t *)val, vl);
    } else {
      rc = -1; /* post-base forms unsupported */
    }
  }
  free(val);
  return rc;
}

/* A complete HEADERS frame of this stream has arrived. */
static void headers_frame(struct h3stream *st)
{
  if (st->headers_done) /* trailers: not kept */
    return;
  if (qpack_decode(st, st->hb, st->hblen) < 0) {
    st->bad = 1;
    return;
  }
  st->hbuf[st->hlen < st->hcap ? st->hlen : st->hcap - 1] = '\0';
  if (st->status >= 100 && st->status < 200) { /* interim, e.g. 103 Early Hints: hand over, then start again */
    if (st->opts && st->opts->on_headers && st->hbuf != st->scratch)
      st->opts->on_headers(st->opts->ctx, st->hbuf, st->hlen);
    st->hlen = 0;
    st->status = 0;
    return;
  }
  st->headers_done = 1;
  if (st->opts && st->opts->on_headers && st->hbuf != st->scratch &&
      st->opts->on_headers(st->opts->ctx, st->hbuf, st->hlen))
    st->aborted = 1;
}

/* A PUSH_PROMISE arrived: note which push id will carry which request. */
static void promise_frame(struct h3stream *st)
{
  uint32_t o = 0;
  uint64_t id;
  if (!st->H || !g_push_fn || st->hblen < 1)
    return; /* nobody asked for pushes */
  int l = 1 << (st->hb[0] >> 6);
  if ((uint32_t)l > st->hblen)
    return;
  id = st->hb[0] & 0x3f;
  for (int i = 1; i < l; i++)
    id = (id << 8) | st->hb[i];
  o = (uint32_t)l;
  if (id >= H3_PUSH_MAX)
    return; /* beyond the MAX_PUSH_ID we granted */
  struct promise p;
  memset(&p, 0, sizeof p);
  p.used = 1;
  p.push_id = id;
  st->pr = &p;
  int rc = qpack_decode(st, st->hb + o, st->hblen - o);
  st->pr = 0;
  if (rc == 0 && p.used == 1 && p.authority[0] && p.path[0])
    st->H->prom[id] = p;
}

/* ----------------------------------------------- response frame parser */

static void acc_consume(struct h3stream *st, uint32_t k)
{
  memmove(st->acc, st->acc + k, st->acclen - k);
  st->acclen -= k;
}

static void feed(struct h3stream *st, const uint8_t *data, uint32_t n)
{
  if (st->acclen + n > sizeof st->acc)
    n = (uint32_t)sizeof st->acc - st->acclen;
  memcpy(st->acc + st->acclen, data, n);
  st->acclen += n;

  while (!st->aborted && !st->bad) {
    if (st->state == 0) { /* parse type + length varints */
      if (st->acclen < 1)
        break;
      int tl = 1 << (st->acc[0] >> 6);
      if ((uint32_t)tl >= st->acclen)
        break;
      int ll = 1 << (st->acc[tl] >> 6);
      if ((uint32_t)(tl + ll) > st->acclen)
        break;
      uint64_t ty = st->acc[0] & 0x3f;
      for (int i = 1; i < tl; i++)
        ty = (ty << 8) | st->acc[i];
      uint64_t ln = st->acc[tl] & 0x3f;
      for (int i = 1; i < ll; i++)
        ln = (ln << 8) | st->acc[tl + i];
      acc_consume(st, (uint32_t)(tl + ll));
      st->ftype = ty;
      st->frem = ln;
      st->state = 1;
      if (ty == 0x01 || ty == 0x05)
        st->hblen = 0; /* HEADERS, PUSH_PROMISE: start accumulating */
      if (ln == 0 && ty == 0x01) {
        headers_frame(st);
        st->state = 0;
      } else if (ln == 0) {
        st->state = 0;
      }
    } else {
      if (st->acclen == 0)
        break;
      uint32_t take = st->acclen;
      if (take > st->frem)
        take = (uint32_t)st->frem;
      if (st->ftype == 0x00) { /* DATA -> body sink */
        if (st->body && take && st->body(st->bctx, st->acc, (int)take) != 0)
          st->aborted = 1;
      } else if (st->ftype == 0x01 || st->ftype == 0x05) { /* HEADERS, PUSH_PROMISE -> accumulate */
        uint32_t room = H3_HB_MAX - st->hblen;
        uint32_t cp = take < room ? take : room;
        memcpy(st->hb + st->hblen, st->acc, cp);
        st->hblen += cp;
        if (cp < take)
          st->bad = 1; /* a header block bigger than we keep */
      }
      acc_consume(st, take);
      st->frem -= take;
      if (st->frem == 0) {
        if (st->ftype == 0x01 && !st->bad)
          headers_frame(st);
        else if (st->ftype == 0x05 && !st->bad)
          promise_frame(st);
        st->state = 0;
      }
    }
  }
}

/* ----------------------------------------------------- request encoding */

/* QPACK-encode the request field section into b; returns its length. */
static uint32_t build_request(uint8_t *b, uint32_t cap, const char *method, const char *path, const char *authority,
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
      if (nl > 0 && nl < 200 && p + (uint32_t)nl + (uint32_t)vl + 16 < cap) {
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

/* ------------------------------------------------------------- sessions */

static void stream_setup(struct h3stream *st, uint64_t sid, char *hbuf, int hcap)
{
  memset(st, 0, sizeof *st);
  st->sid = sid;
  st->hbuf = hbuf && hcap > 0 ? hbuf : st->scratch;
  st->hcap = hbuf && hcap > 0 ? hcap : (int)sizeof st->scratch;
  st->hb = malloc(H3_HB_MAX);
  if (!st->hb)
    st->bad = 1;
}

static int h3_send_all(quic *q, uint64_t sid, const uint8_t *p, uint32_t n, int fin)
{
  return quic_send(q, sid, p, n, fin) < 0 ? -1 : 0;
}

/* HEADERS (and a DATA frame for the body) of one request on stream `sid`. */
static int send_request(struct h3 *H, uint64_t sid, const char *method, const char *authority, const char *path,
                        const char *extra, const unsigned char *body, int blen)
{
  int extra_len = extra ? (int)strlen(extra) : 0;
  char *xh = malloc((unsigned)extra_len + 48);
  uint8_t *qbuf = malloc(8192);
  uint8_t *frame = malloc(8192 + 16);
  if (!xh || !qbuf || !frame) {
    free(xh), free(qbuf), free(frame);
    return -1;
  }
  int n = 0;
  if (extra_len) {
    memcpy(xh, extra, (unsigned)extra_len);
    n = extra_len;
  }
  if (blen > 0) { /* the body's length rides in the header block */
    memcpy(xh + n, "content-length: ", 16);
    n += 16;
    char num[12];
    int k = 0;
    for (unsigned v = (unsigned)blen; v; v /= 10)
      num[k++] = (char)('0' + v % 10);
    while (k)
      xh[n++] = num[--k];
    xh[n++] = '\r';
    xh[n++] = '\n';
  }
  xh[n] = '\0';
  uint32_t qlen = build_request(qbuf, 8192, method, path, authority, xh);
  uint32_t fp = 0;
  frame[fp++] = 0x01; /* HEADERS frame type */
  fp += put_qvar(frame + fp, qlen);
  memcpy(frame + fp, qbuf, qlen);
  fp += qlen;
  int rc = h3_send_all(H->q, sid, frame, fp, blen <= 0);
  if (rc == 0 && blen > 0) {
    uint8_t dh[5];
    uint32_t dl = 0;
    dh[dl++] = 0x00; /* DATA frame type */
    dl += put_qvar(dh + dl, (uint64_t)blen);
    rc = h3_send_all(H->q, sid, dh, dl, 0);
    if (rc == 0)
      rc = h3_send_all(H->q, sid, body, (uint32_t)blen, 1);
  }
  free(xh), free(qbuf), free(frame);
  return rc;
}

/* ------------------------------------------------------- server streams */

static void stream_setup(struct h3stream *st, uint64_t sid, char *hbuf, int hcap);

static int uni_body(void *ctx, const unsigned char *p, int n)
{
  struct h3uni *u = ctx;
  if (u->too_big)
    return 0;
  if ((unsigned)u->pblen + (unsigned)n > H3_PUSH_BODY_MAX) {
    u->too_big = 1;
    return 0;
  }
  if (u->pblen + n > u->pbcap) {
    int nc = u->pbcap ? u->pbcap * 2 : 8192;
    while (nc < u->pblen + n)
      nc *= 2;
    unsigned char *nb = realloc(u->pbody, (unsigned)nc);
    if (!nb) {
      u->too_big = 1;
      return 0;
    }
    u->pbody = nb;
    u->pbcap = nc;
  }
  memcpy(u->pbody + u->pblen, p, (unsigned)n);
  u->pblen += n;
  return 0;
}

static void uni_free(struct h3uni *u)
{
  if (u->ps) {
    free(u->ps->hb);
    free(u->ps);
  }
  free(u->phdr);
  free(u->pbody);
  free(u);
}

/* Read a varint from u->acc one byte at a time; 1 once it is complete (value in *v). */
static int uni_varint(struct h3uni *u, uint8_t b, uint64_t *v)
{
  u->acc[u->acclen++] = b;
  int need = 1 << (u->acc[0] >> 6);
  if (u->acclen < need)
    return 0;
  uint64_t x = u->acc[0] & 0x3f;
  for (int i = 1; i < need; i++)
    x = (x << 8) | u->acc[i];
  *v = x;
  u->acclen = 0;
  return 1;
}

static uint64_t varint_of(const uint8_t *p, int len)
{
  if (len < 1 || len < (1 << (p[0] >> 6)))
    return 0;
  uint64_t x = p[0] & 0x3f;
  for (int i = 1; i < (1 << (p[0] >> 6)); i++)
    x = (x << 8) | p[i];
  return x;
}

/* Bytes of a server stream: the control stream's GOAWAY, or a pushed response. */
static void uni_feed(struct h3 *H, struct h3uni *u, const uint8_t *d, uint32_t n)
{
  for (uint32_t i = 0; i < n; i++) {
    uint64_t v;
    if (u->type < 0) {
      if (uni_varint(u, d[i], &v))
        u->type = (int)v == 1 && !g_push_fn ? 99 : (int)v;
    } else if (u->type == 0) { /* control: frames, of which only GOAWAY matters */
      if (u->cstate == 0) {
        if (uni_varint(u, d[i], &v)) {
          u->cftype = v;
          u->cstate = 1;
        }
      } else if (u->cstate == 1) {
        if (uni_varint(u, d[i], &v)) {
          u->cfrem = v;
          u->paylen = 0;
          u->cstate = u->cfrem ? 2 : 0;
        }
      } else {
        if (u->cftype == 0x07 && u->paylen < (int)sizeof u->pay)
          u->pay[u->paylen++] = d[i];
        if (--u->cfrem == 0) {
          if (u->cftype == 0x07) {
            uint64_t id = varint_of(u->pay, u->paylen);
            if (!H->goaway || id < H->goaway_id)
              H->goaway_id = id;
            H->goaway = 1;
          }
          u->cstate = 0;
        }
      }
    } else if (u->type == 1) { /* push: the push id, then the response like any other */
      if (!u->pushed) {
        if (uni_varint(u, d[i], &v)) {
          u->push_id = v;
          u->pushed = 1;
          u->ps = malloc(sizeof *u->ps);
          u->phdr = malloc(8192);
          if (!u->ps || !u->phdr) {
            u->too_big = 1;
            return;
          }
          stream_setup(u->ps, u->sid, u->phdr, 8192);
          u->ps->body = uni_body;
          u->ps->bctx = u;
        }
      } else {
        feed(u->ps, d + i, n - i);
        return;
      }
    } else {
      return; /* QPACK streams: nothing we use */
    }
  }
}

/* A finished push that matches a promise goes to the handler. */
static void push_deliver(struct h3 *H, struct h3uni *u)
{
  struct h3stream *ps = u->ps;
  if (!ps || !u->complete || u->push_id >= H3_PUSH_MAX)
    return;
  struct promise *p = &H->prom[u->push_id];
  if (!p->used || u->too_big || ps->bad || !ps->headers_done)
    return;
  if (g_push_fn)
    g_push_fn(g_push_ctx, H->authority, p->scheme, p->authority, p->path, u->phdr, (const unsigned char *)u->pbody,
              u->pblen);
  p->used = 0;
  u->ps->done = 2; /* delivered */
}

/* Pushes whose promise arrived after the push itself, and the ones that are complete: hand them over. */
static void push_flush(struct h3 *H)
{
  for (int i = 0; i < H3_UNI_MAX; i++) {
    struct h3uni *u = H->uni[i];
    if (!u || u->type != 1 || !u->complete)
      continue;
    push_deliver(H, u);
    if (u->ps && (u->ps->done == 2 || u->too_big || u->ps->bad)) {
      uni_free(u);
      H->uni[i] = 0;
    }
  }
}

/* Read what the server has sent on the streams it opened. */
static void drain_uni(struct h3 *H)
{
  uint64_t sid;
  while (quic_next_uni(H->q, &sid)) {
    struct h3uni *u = 0;
    int slot = -1;
    for (int i = 0; i < H3_UNI_MAX; i++) {
      if (H->uni[i] && H->uni[i]->sid == sid)
        u = H->uni[i];
      if (!H->uni[i] && slot < 0)
        slot = i;
    }
    if (!u && slot >= 0) {
      u = calloc(1, sizeof *u);
      if (u) {
        u->sid = sid;
        u->type = -1;
        H->uni[slot] = u;
      }
    }
    int got, fin = 0;
    while ((got = quic_read(H->q, sid, H->rx, H3_ACC, &fin)) > 0)
      if (u)
        uni_feed(H, u, H->rx, (uint32_t)got);
    if (fin) {
      if (u && u->type == 1 && u->pushed)
        u->complete = 1;
      quic_stream_done(H->q, sid);
      if (u && u->type != 1) {
        free(u);
        for (int i = 0; i < H3_UNI_MAX; i++)
          if (H->uni[i] == u)
            H->uni[i] = 0;
      }
    } else if (got <= 0) {
      break; /* nothing more to read right now */
    }
  }
}

/* Streams the server's GOAWAY left unprocessed will never be answered. */
static void apply_goaway(struct h3 *H, struct h3stream **sts, int n)
{
  if (!H->goaway)
    return;
  for (int i = 0; i < n; i++) {
    struct h3stream *st = sts[i];
    if (st && !st->done && !st->bad && !st->aborted && st->sid >= H->goaway_id) {
      st->refused = 1;
      st->bad = 1;
    }
  }
}

/* Read what has arrived on every unfinished stream. */
static void drain(struct h3 *H, struct h3stream **sts, int n)
{
  uint8_t *tmp = H->rx;
  drain_uni(H);
  apply_goaway(H, sts, n);
  for (int i = 0; i < n; i++) {
    struct h3stream *st = sts[i];
    if (!st || st->done || st->aborted || st->bad)
      continue;
    int got, fin = 0;
    while (!st->aborted && !st->bad && (got = quic_read(H->q, st->sid, tmp, H3_ACC, &fin)) > 0)
      feed(st, tmp, (uint32_t)got);
    if (!st->aborted && !st->bad && fin)
      st->done = 1;
  }
}

static int streams_open(struct h3stream **sts, int n)
{
  int open = 0;
  for (int i = 0; i < n; i++)
    if (sts[i] && !sts[i]->done && !sts[i]->aborted && !sts[i]->bad)
      open++;
  return open;
}

/* Drive the connection until every stream has ended or the server goes quiet. Returns 0, HTTP_EINTR or
 * HTTP_ENORESP; `finished(ctx, i)` runs the moment stream i ends. */
static int pump(struct h3 *H, struct h3stream **sts, int n, void (*finished)(void *ctx, int i), void *ctx)
{
  int idle = 0, was_done[H2_FETCH_MAX] = {0};
  for (;;) {
    drain(H, sts, n);
    for (int i = 0; i < n && finished; i++)
      if (sts[i] && sts[i]->done && !was_done[i]) {
        was_done[i] = 1;
        finished(ctx, i);
      }
    if (!streams_open(sts, n))
      return 0;
    int r = quic_poll(H->q);
    if (r == QUIC_EINTR)
      return HTTP_EINTR;
    if (r == QUIC_ECLOSED) {
      drain(H, sts, n); /* bytes that arrived with the close */
      return HTTP_ENORESP;
    }
    if (r == 0 && ++idle > 5)
      return HTTP_ENORESP;
    if (r > 0)
      idle = 0;
  }
}

/* Open a connection: QUIC handshake, then the HTTP/3 control stream (empty SETTINGS) and the QPACK encoder and
 * decoder streams (type byte only: we never use the dynamic table). */
int h3_session_open(h3_session **out, const char *host, int port)
{
  struct h3 *H = calloc(1, sizeof *H);
  if (!H)
    return HTTP_ESEND;
  int rc = quic_connect(&H->q, host, (uint16_t)port, "h3");
  if (rc != QUIC_OK) {
    free(H);
    return rc == QUIC_EINTR ? HTTP_EINTR : rc == QUIC_ERESOLVE ? HTTP_ERESOLVE : HTTP_ECONNECT;
  }
  int an = 0;
  for (const char *c = host; *c && an < 250; c++)
    H->authority[an++] = *c;
  if (port != 443) {
    char num[8];
    int k = 0;
    for (unsigned v = (unsigned)port; v; v /= 10)
      num[k++] = (char)('0' + v % 10);
    H->authority[an++] = ':';
    while (k)
      H->authority[an++] = num[--k];
  }
  H->authority[an] = '\0';
  /* Control stream: type 0, empty SETTINGS, and (only when a handler wants them) MAX_PUSH_ID allowing 8 pushes. */
  uint8_t ctrl[6] = {0x00, 0x04, 0x00, 0x0d, 0x01, H3_PUSH_MAX - 1};
  uint8_t enc[1] = {0x02}, dec[1] = {0x03};
  quic_send(H->q, 2, ctrl, g_push_fn ? 6 : 3, 0);
  quic_send(H->q, 6, enc, 1, 0);
  quic_send(H->q, 10, dec, 1, 0);
  *out = H;
  return 0;
}

void h3_session_close(h3_session *H)
{
  for (int i = 0; i < H3_UNI_MAX; i++)
    if (H->uni[i])
      uni_free(H->uni[i]);
  quic_close(H->q);
  free(H);
}

int h3_session_used(const h3_session *H) { return H->served > 0; }

int h3_session_run(h3_session *H, const char *authority, const char *method, const char *path,
                   const char *extra_headers, const unsigned char *reqbody, int reqlen, const struct h3_opts *opts,
                   char *hdr, int hdrcap, int *hdrlen, http_body_fn body, void *ctx)
{
  if (!method)
    method = "GET";
  uint64_t sid;
  if (H->broken || quic_open(H->q, &sid) < 0 || (H->goaway && sid >= H->goaway_id)) {
    H->broken = 1;
    return HTTP_ESEND;
  }
  H->served++;
  struct h3stream *st = malloc(sizeof *st);
  if (!st) {
    H->broken = 1;
    return HTTP_ESEND;
  }
  stream_setup(st, sid, hdr, hdr ? hdrcap : 0);
  st->H = H;
  st->body = body;
  st->bctx = ctx;
  st->opts = opts;
  int rc;
  if (st->bad || send_request(H, sid, method, authority, path, extra_headers, reqbody, reqlen) < 0)
    rc = HTTP_ESEND;
  else
    rc = pump(H, &st, 1, 0, 0);
  push_flush(H);
  if (st->refused)
    rc = HTTP_ESEND; /* the server's GOAWAY: this request was never processed */
  else if (rc == 0 && st->aborted)
    rc = HTTP_EABORT;
  else if (rc == 0 && st->bad)
    rc = HTTP_ENORESP;
  else if (rc == 0 && !st->done)
    rc = HTTP_ENORESP;
  int status = st->headers_done ? st->status : 0;
  if (!(rc == 0 && st->done) || st->bad)
    H->broken = 1;
  else
    quic_stream_done(H->q, sid);
  if (hdr && hdrcap > 0) {
    if (st->hlen + 2 < hdrcap) { /* room for the blank line *and* the NUL */
      hdr[st->hlen++] = '\r';
      hdr[st->hlen++] = '\n';
    }
    hdr[st->hlen] = '\0';
    if (hdrlen)
      *hdrlen = st->hlen;
  }
  free(st->hb);
  free(st);
  /* A response that began is still a response when the connection drops after it. */
  if ((rc == HTTP_ENORESP) && status > 0)
    rc = status;
  return rc == 0 ? status : rc;
}

/* Several GETs at once, one stream each, like h2_fetch_many. */
struct h3mx {
  struct h3stream *st[H2_FETCH_MAX];
  unsigned char *body[H2_FETCH_MAX];
  int blen[H2_FETCH_MAX], bcap[H2_FETCH_MAX], oversize[H2_FETCH_MAX];
  struct h2_fetch *reqs;
  void (*on_done)(void *ctx, int i, const unsigned char *body, int blen);
  void *cb_ctx;
};

struct mxsink {
  struct h3mx *m;
  int i;
};

static int mx_body(void *ctx, const unsigned char *p, int n)
{
  struct mxsink *k = ctx;
  struct h3mx *m = k->m;
  int i = k->i;
  if (m->blen[i] + n > m->bcap[i]) {
    int nc = m->bcap[i] ? m->bcap[i] : 8192;
    while (nc < m->blen[i] + n)
      nc += nc / 2 + 1;
    unsigned char *nb = nc > 16 * 1024 * 1024 ? 0 : realloc(m->body[i], (unsigned)nc);
    if (!nb) {
      m->oversize[i] = 1;
      return 1;
    }
    m->body[i] = nb;
    m->bcap[i] = nc;
  }
  memcpy(m->body[i] + m->blen[i], p, (unsigned)n);
  m->blen[i] += n;
  return 0;
}

static void mx_finished(void *ctx, int i)
{
  struct h3mx *m = ctx;
  struct h3stream *st = m->st[i];
  if (m->on_done && st->headers_done && st->status == 200 && m->body[i] && m->blen[i] > 0)
    m->on_done(m->cb_ctx, i, m->body[i], m->blen[i]);
}

int h3_session_fetch_many(h3_session *H, const char *authority, const char *extra_headers, struct h2_fetch *reqs,
                          int nreq, void (*on_done)(void *ctx, int i, const unsigned char *body, int blen),
                          void *cb_ctx)
{
  if (nreq <= 0 || nreq > H2_FETCH_MAX)
    return HTTP_ENORESP;
  if (H->broken)
    return HTTP_ESEND;
  struct h3mx *m = calloc(1, sizeof *m);
  struct mxsink sinks[H2_FETCH_MAX];
  if (!m)
    return HTTP_ESEND;
  m->reqs = reqs;
  m->on_done = on_done;
  m->cb_ctx = cb_ctx;
  H->served++;
  int rc = 0, sent = 0;
  for (int i = 0; i < nreq; i++) {
    uint64_t sid;
    reqs[i].status = 0;
    reqs[i].body = 0;
    reqs[i].blen = 0;
    reqs[i].hdrlen = 0;
    m->st[i] = malloc(sizeof **m->st);
    if (!m->st[i] || quic_open(H->q, &sid) < 0) {
      free(m->st[i]);
      m->st[i] = 0;
      continue; /* no stream for this one: the caller retries it elsewhere */
    }
    stream_setup(m->st[i], sid, reqs[i].hdr, reqs[i].hdr ? reqs[i].hdrcap : 0);
    m->st[i]->H = H;
    if (H->goaway && sid >= H->goaway_id) { /* refused up front */
      m->st[i]->refused = m->st[i]->bad = 1;
      continue;
    }
    sinks[i].m = m;
    sinks[i].i = i;
    m->st[i]->body = mx_body;
    m->st[i]->bctx = &sinks[i];
    if (m->st[i]->bad || send_request(H, sid, "GET", authority, reqs[i].path, extra_headers, 0, 0) < 0) {
      m->st[i]->bad = 1;
      H->broken = 1;
      rc = HTTP_ESEND;
      break;
    }
    sent++;
  }
  if (sent && rc == 0)
    rc = pump(H, m->st, nreq, mx_finished, m);
  push_flush(H);
  for (int i = 0; i < nreq; i++) {
    struct h3stream *st = m->st[i];
    int ok = st && st->done && !st->bad && !st->aborted;
    if (st && !ok)
      H->broken = 1;
    if (st && ok)
      quic_stream_done(H->q, st->sid);
    reqs[i].status = st && st->headers_done ? st->status : 0;
    if (st && reqs[i].hdr && reqs[i].hdrcap > 0) {
      if (st->hlen + 2 < reqs[i].hdrcap) {
        reqs[i].hdr[st->hlen++] = '\r';
        reqs[i].hdr[st->hlen++] = '\n';
      }
      reqs[i].hdr[st->hlen] = '\0';
      reqs[i].hdrlen = st->hlen;
    }
    if (on_done) {
      free(m->body[i]);
      reqs[i].body = 0;
      reqs[i].blen = 0;
    } else {
      reqs[i].body = m->body[i];
      reqs[i].blen = m->blen[i];
    }
    reqs[i].failed = !ok || m->oversize[i] || reqs[i].status != 200;
    if (st) {
      free(st->hb);
      free(st);
    }
  }
  free(m);
  if (rc != 0)
    H->broken = 1;
  return rc;
}

/* A session is worth keeping only while the server still lets us open streams and will not time it out first. */
int h3_session_reusable(const h3_session *H)
{
  unsigned idle = quic_idle_ms(H->q);
  return !H->broken && !H->goaway && quic_streams_left(H->q) > 1 && (idle == 0 || idle >= 20000);
}

static void h3_session_destroy(void *s) { h3_session_close(s); }
static int h3_session_dead(void *s) { return ((h3_session *)s)->broken || quic_peer_closed(((h3_session *)s)->q); }

h3_session *h3_session_acquire(const char *host, int port) { return nc_idle_take(host, port, 1, NC_IDLE_H3); }

void h3_session_release(h3_session *s, const char *host, int port)
{
  if (h3_session_reusable(s))
    nc_idle_put(host, port, 1, NC_IDLE_H3, s, h3_session_destroy, h3_session_dead);
  else
    h3_session_close(s);
}

/* ----------------------------------------------------------- one-shot */

int h3_run(const char *host, int port, const char *authority, const char *method, const char *path,
           const char *extra_headers, char *hdr, int hdrcap, int *hdrlen, http_body_fn body, void *ctx)
{
  h3_session *s;
  int rc = h3_session_open(&s, host, port);
  if (rc != 0)
    return rc;
  rc = h3_session_run(s, authority, method, path, extra_headers, 0, 0, 0, hdr, hdrcap, hdrlen, body, ctx);
  h3_session_close(s);
  return rc;
}
