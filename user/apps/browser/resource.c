/* resource.c: sub-resource fetching (images, favicon, CSS) on per-request sockets. */
#include "animimg.h" /* frame playback for animated GIF/WebP */
#include "base64.h"  /* shared base64 for data: URL decoding */
#include "browser.h"
#include "features/cache/cache.h"             /* the HTTP response cache (RFC 7234, honours headers)   */
#include "features/clienthints/clienthints.h" /* Accept-CH: per-origin Client Hints opt-in        */
#include "features/cors/cors.h"               /* CORS request orchestration + CORP sub-resource guard  */
#include "features/svg/svg.h"                 /* SVG -> ARGB rasterizer (favicon path) */
#include "fetchint.h"
#include "h3.h"
#include "headers.h" /* HTTP response headers (iframe X-Frame-Options / CSP)  */
#include "imgdec.h"  /* shared image decoder (SVG/WebP/GIF/PNG) for <img> */
#include "mutex.h"

/* Origin owning the sub-resources being fetched (set by do_fetch); used for CORP checks. */
static char g_embedder[URL_MAX];
void resource_set_page(const char *page_url)
{
  int k = 0;
  g_embedder[0] = '\0';
  if (page_url)
    sapp(g_embedder, &k, sizeof g_embedder, page_url);
}

int ch_headers_for(const struct purl *u, bool subresource, char *out, int cap)
{
  char url[URL_HOST_MAX + 16];
  int n = 0;
  sapp(url, &n, sizeof url, u->tls ? "https://" : "http://");
  sapp(url, &n, sizeof url, u->host);
  sapp(url, &n, sizeof url, ":");
  uapp(url, &n, sizeof url, (unsigned)u->port);
  if (!subresource)
    return ch_request_headers_for(0, 0, url, out, cap);
  return ch_request_headers_for(g_embedder[0] ? g_embedder : 0, B->pg->perm_policy_hdr, url, out, cap);
}

/* --------------------------------------------------- inline image fetching */

/* A byte reader over a slice of img_pool, for png_dimensions. */
struct memrd {
  const unsigned char *p;
  int pos, len;
};
static int memrd_get(void *c)
{
  struct memrd *m = c;
  return m->pos < m->len ? m->p[m->pos++] : -1;
}

/* Per-thread HTTP reader over its own netconn/buffer, so parallel fetches don't share B->sock. */
struct ireader {
  netconn *conn;
  unsigned char buf[2048];
  int pos, len;
  bool eof;
};
static int ir_getc(struct ireader *r)
{
  if (r->pos >= r->len) {
    if (r->eof)
      return -1;
    int n = nc_read(r->conn, r->buf, (int)sizeof r->buf);
    if (n <= 0) {
      r->eof = true;
      return -1;
    }
    r->pos = 0;
    r->len = n;
  }
  return r->buf[r->pos++];
}

/* Append one byte to a doubling body buffer (cap RAW_MAX); 0 ok, -1 if full or OOM. */
static int body_put(unsigned char **body, int *cap, int *blen, int c)
{
  if (*blen >= *cap) {
    int ncap = *cap * 2;
    if (ncap > (int)RAW_MAX)
      ncap = RAW_MAX;
    if (*blen >= ncap)
      return -1; /* hit the cap */
    unsigned char *nb = realloc(*body, (unsigned)ncap);
    if (!nb)
      return -1;
    *body = nb;
    *cap = ncap;
  }
  (*body)[(*blen)++] = (unsigned char)c;
  return 0;
}

/* Grow *body (doubling, capped at RAW_MAX) to hold `need`; returns capacity, < need if capped/OOM. */
static int body_reserve(unsigned char **body, int *cap, int need)
{
  if (need <= *cap)
    return *cap;
  int ncap = *cap;
  while (ncap < need) {
    if (ncap >= (int)RAW_MAX) {
      ncap = RAW_MAX;
      break;
    }
    ncap *= 2;
    if (ncap > (int)RAW_MAX)
      ncap = RAW_MAX;
  }
  if (ncap > *cap) {
    unsigned char *nb = realloc(*body, (unsigned)ncap);
    if (!nb)
      return *cap;
    *body = nb;
    *cap = ncap;
  }
  return *cap;
}

/* Bulk-copy up to `want` body bytes from the reader; returns count (short = EOF or RAW_MAX cap). */
static long ir_drain(struct ireader *r, unsigned char **body, int *cap, int *blen, long want)
{
  long done = 0;
  while (done < want) {
    if (r->pos >= r->len) {
      if (r->eof)
        break;
      int n = nc_read(r->conn, r->buf, (int)sizeof r->buf);
      if (n <= 0) {
        r->eof = true;
        break;
      }
      r->pos = 0;
      r->len = n;
    }
    long avail = r->len - r->pos, need = want - done;
    int run = (int)(avail < need ? avail : need);
    int room = body_reserve(body, cap, *blen + run);
    if (room < *blen + run)
      run = room - *blen; /* cap/OOM: take what fits */
    if (run <= 0)
      break;
    memcpy(*body + *blen, r->buf + r->pos, (unsigned)run);
    r->pos += run;
    *blen += run;
    done += run;
    if (*blen >= (int)RAW_MAX && done < want)
      break; /* hit the hard cap */
  }
  return done;
}

/* Read a header block into `hdr` (NUL-terminated); returns length, -1 if closed early. */
static int ir_read_header_block(struct ireader *r, char *hdr, int cap)
{
  int hl = 0, m = 0;
  for (;;) {
    int c = ir_getc(r);
    if (c < 0)
      return -1;
    if (hl < cap - 1)
      hdr[hl++] = (char)c;
    if (c == '\r')
      m = (m == 2) ? 3 : 1;
    else if (c == '\n')
      m = (m == 1) ? 2 : (m == 3 ? 4 : 0);
    else
      m = 0;
    if (m == 4)
      break;
  }
  hdr[hl] = '\0';
  return hl;
}

/* Pump up to `want` body bytes (<0 = until close) to `sink`; returns count, -1 if sink aborts. */
static long ir_pump_n(struct ireader *r, long want, http_body_fn sink, void *ctx)
{
  long done = 0;
  while (want < 0 || done < want) {
    if (r->pos >= r->len) {
      if (r->eof)
        break;
      int n = nc_read(r->conn, r->buf, (int)sizeof r->buf);
      if (n <= 0) {
        r->eof = true;
        break;
      }
      r->pos = 0;
      r->len = n;
    }
    long avail = r->len - r->pos;
    long need = (want < 0) ? avail : (want - done);
    int run = (int)(avail < need ? avail : need);
    if (run <= 0)
      break;
    if (sink(ctx, r->buf + r->pos, run))
      return -1;
    r->pos += run;
    done += run;
  }
  return done;
}

/* Stream a framed body (chunked/content-length/to-close) to `sink`; returns bytes, -1 on sink abort. */
static long ir_pump_body(struct ireader *r, bool chunked, long clen, http_body_fn sink, void *ctx)
{
  long total = 0;
  if (chunked) {
    for (;;) {
      unsigned sz = 0;
      int c, any = 0;
      while ((c = ir_getc(r)) >= 0 && c != '\r' && c != ';') {
        int d;
        if (c >= '0' && c <= '9')
          d = c - '0';
        else if (c >= 'a' && c <= 'f')
          d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
          d = c - 'A' + 10;
        else
          break;
        sz = sz * 16 + (unsigned)d;
        any = 1;
      }
      while (c >= 0 && c != '\n')
        c = ir_getc(r);
      if (!any || sz == 0)
        break;
      long got = ir_pump_n(r, (long)sz, sink, ctx);
      if (got < 0)
        return -1;
      total += got;
      if (got != (long)sz)
        break; /* short: connection died */
      c = ir_getc(r);
      if (c == '\r')
        ir_getc(r); /* trailing CRLF */
    }
  } else {
    long got = ir_pump_n(r, clen >= 0 ? clen : -1, sink, ctx);
    if (got < 0)
      return -1;
    total += got;
  }
  return total;
}

/* Status code and Location of a sub-resource response. */
struct resp_meta {
  int status;
  char location[URL_MAX];
  char ctype[160];
  bool http11;     /* HTTP/1.1 status line                         */
  bool conn_close; /* Connection: close                            */
};

/* Parse an HTTP/1.1 or h2-synthesized header block into `meta` plus body framing. */
static void parse_resp_headers(const char *hdr, struct resp_meta *meta, bool *chunked, long *clen)
{
  meta->status = 0;
  meta->location[0] = '\0';
  meta->ctype[0] = '\0';
  meta->http11 = starts_ci(hdr, "HTTP/1.1");
  meta->conn_close = false;
  *chunked = false;
  *clen = -1;

  {
    const char *s = hdr;
    while (*s && *s != ' ')
      s++;
    while (*s == ' ')
      s++;
    while (*s >= '0' && *s <= '9')
      meta->status = meta->status * 10 + (*s++ - '0');
  }

  for (const char *line = hdr; *line;) {
    if (starts_ci(line, "content-length:")) {
      const char *v = line + 15;
      while (*v == ' ')
        v++;
      *clen = 0;
      while (*v >= '0' && *v <= '9')
        *clen = *clen * 10 + (*v++ - '0');
    } else if (starts_ci(line, "transfer-encoding:")) {
      for (const char *q = line; *q && *q != '\n'; q++)
        if (starts_ci(q, "chunked")) {
          *chunked = true;
          break;
        }
    } else if (starts_ci(line, "connection:")) {
      for (const char *q = line; *q && *q != '\n'; q++)
        if (starts_ci(q, "close")) {
          meta->conn_close = true;
          break;
        }
    } else if (starts_ci(line, "location:")) {
      const char *v = line + 9;
      while (*v == ' ')
        v++;
      int k = 0;
      while (*v && *v != '\r' && *v != '\n' && k < (int)sizeof meta->location - 1)
        meta->location[k++] = *v++;
      meta->location[k] = '\0';
    } else if (starts_ci(line, "content-type:")) {
      const char *v = line + 13;
      while (*v == ' ')
        v++;
      int k = 0;
      while (*v && *v != '\r' && *v != '\n' && k < (int)sizeof meta->ctype - 1)
        meta->ctype[k++] = *v++;
      meta->ctype[k] = '\0';
    }
    const char *e = line;
    while (*e && *e != '\n')
      e++;
    if (!*e)
      break;
    line = e + 1;
  }
}

/* Consume the trailer section after the last chunk; false if the connection ended before its blank line. */
static bool ir_skip_trailers(struct ireader *r)
{
  for (;;) {
    int c = ir_getc(r);
    bool empty = true;
    while (c >= 0 && c != '\n') {
      if (c != '\r')
        empty = false;
      c = ir_getc(r);
    }
    if (c < 0)
      return false;
    if (empty)
      return true;
  }
}

/* Read a response body into a malloc'd buffer (caller frees) for any status; NULL on error/OOM. Interim 1xx
 * responses are skipped, and a HEAD, 204 or 304 response has no body. *keep says whether the connection is
 * left at a response boundary and may serve another request. */
static unsigned char *read_http_body(struct ireader *r, int *outlen, struct resp_meta *meta, char *hdr_out, int hdr_cap,
                                     bool head, bool *keep)
{
  char hdr[RESP_HDR_MAX];
  bool chunked;
  long clen;
  *keep = false;
  for (int interim = 0;; interim++) {
    if (ir_read_header_block(r, hdr, (int)sizeof hdr) < 0)
      return 0;
    parse_resp_headers(hdr, meta, &chunked, &clen);
    if (meta->status < 100 || meta->status >= 200 || meta->status == 101)
      break;
    if (interim >= 16)
      return 0;
  }
  if (hdr_out && hdr_cap > 0) { /* hand the raw block to the caller */
    int k = 0;
    for (; hdr[k] && k < hdr_cap - 1; k++)
      hdr_out[k] = hdr[k];
    hdr_out[k] = '\0';
  }

  bool no_body = head || meta->status == 204 || meta->status == 304;
  int cap = (clen > 0 && clen < (long)RAW_MAX) ? (int)clen + 1 : 65536;
  unsigned char *body = malloc((unsigned)cap);
  if (!body)
    return 0;
  int blen = 0;
  bool framed = no_body; /* the body ended where its framing says it does */

  if (no_body) {
    /* nothing follows the header block */
  } else if (chunked) {
    for (;;) {
      unsigned sz = 0;
      int c, any = 0;
      while ((c = ir_getc(r)) >= 0 && c != '\r' && c != ';') {
        int d;
        if (c >= '0' && c <= '9')
          d = c - '0';
        else if (c >= 'a' && c <= 'f')
          d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
          d = c - 'A' + 10;
        else
          break;
        sz = sz * 16 + (unsigned)d;
        any = 1;
      }
      while (c >= 0 && c != '\n')
        c = ir_getc(r);
      if (!any)
        goto done;
      if (sz == 0) {
        framed = ir_skip_trailers(r);
        break;
      }
      if (ir_drain(r, &body, &cap, &blen, (long)sz) != (long)sz)
        goto done;
      c = ir_getc(r);
      if (c == '\r')
        ir_getc(r); /* trailing CRLF */
    }
  } else {
    long limit = (clen >= 0) ? clen : (long)RAW_MAX;
    long got = ir_drain(r, &body, &cap, &blen, limit);
    framed = clen >= 0 && got == clen;
  }
done:
  *outlen = blen;
  *keep = framed && meta->http11 && !meta->conn_close && !r->eof && r->pos == r->len;
  return body;
}

static bool is_redirect(int s) { return s == 301 || s == 302 || s == 303 || s == 307 || s == 308; }

/* GET a sub-resource via browser_fetch (cookies, HSTS, CORP); returns the 200 body or NULL. */
static unsigned char *subres_get(const char *absurl, const char *accept, int *outlen)
{
  char hdr[RESP_HDR_MAX];
  struct http_req rq = {0};
  rq.url = absurl;
  rq.accept = accept;
  struct http_resp rs = {0};
  rs.hdr = hdr;
  rs.hdr_cap = sizeof hdr;
  *outlen = 0;
  unsigned char *body = browser_fetch(&rq, &rs);
  if (!body)
    return 0;
  if (rs.status != 200) {
    free(body);
    return 0;
  }

  /* CORP: a cross-origin no-cors resource may forbid embedding. */
  if (g_embedder[0] && !cors_same_origin(g_embedder, absurl)) {
    struct http_headers H;
    http_headers_parse(&H, hdr);
    bool blocked = !corp_allows(&H, g_embedder, absurl, 0, 0);
    http_headers_free(&H);
    if (blocked) {
      free(body);
      return 0;
    }
  }
  *outlen = rs.len;
  return body;
}

/* Build a request with an arbitrary method, optional Content-Type and Content-Length. */
/* Append a "Name: value\r\n" blob to an h2 header buffer, lowercasing names. */
static void h2_append_headers(char *xh, int *xn, int cap, const char *extra)
{
  const char *p = extra;
  while (p && *p) {
    const char *eol = p;
    while (*eol && *eol != '\n')
      eol++;
    const char *colon = p;
    while (colon < eol && *colon != ':')
      colon++;
    for (const char *q = p; q < colon && *xn < cap - 1; q++) {
      char c = *q;
      xh[(*xn)++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    for (const char *q = colon; *q && q <= eol && *xn < cap - 1; q++)
      xh[(*xn)++] = *q;
    if (*xn < cap)
      xh[*xn] = '\0';
    p = *eol ? eol + 1 : eol;
  }
}

#define VARY_HDRS_MAX 2560

/* Normalise a URL's origin (HSTS-upgrading http) into *u; 0 on a bad URL. */
static bool origin_of(const char *url, struct purl *u)
{
  if (!parse_url(url, u))
    return false;
  if (!u->tls && hsts_has(u->host)) {
    u->tls = 1;
    if (u->port == 80)
      u->port = 443;
  }
  return true;
}

/* The request headers a GET of `url` sends (cookies, UA, accept, client hints, caller extras), as the cache's Vary
 * input. */
static void vary_req_hdrs(const char *url, const char *accept, const char *extra, bool cookies, char *out)
{
  int n = 0;
  out[0] = '\0';
  struct purl u;
  if (!origin_of(url, &u))
    return;
  char ck[1024];
  if (cookies && cookie_header(u.host, ck, sizeof ck) > 0) {
    sapp(out, &n, VARY_HDRS_MAX, "cookie: ");
    sapp(out, &n, VARY_HDRS_MAX, ck);
    sapp(out, &n, VARY_HDRS_MAX, "\r\n");
  }
  sapp(out, &n, VARY_HDRS_MAX, "user-agent: ");
  sapp(out, &n, VARY_HDRS_MAX, browser_ua());
  sapp(out, &n, VARY_HDRS_MAX, "\r\naccept: ");
  sapp(out, &n, VARY_HDRS_MAX, accept);
  sapp(out, &n, VARY_HDRS_MAX, "\r\n");
  char ch[512];
  if (ch_headers_for(&u, true, ch, sizeof ch) > 0)
    sapp(out, &n, VARY_HDRS_MAX, ch);
  if (extra && extra[0])
    sapp(out, &n, VARY_HDRS_MAX, extra);
}

static void build_request(char *req, int *n, int cap, const struct purl *u, const char *method, const char *accept,
                          const struct http_req *rq)
{
  sapp(req, n, cap, method);
  sapp(req, n, cap, " ");
  sapp(req, n, cap, u->path);
  sapp(req, n, cap, " HTTP/1.1\r\nHost: ");
  sapp(req, n, cap, u->host);
  if (!((u->tls && u->port == 443) || (!u->tls && u->port == 80))) {
    sapp(req, n, cap, ":");
    uapp(req, n, cap, (unsigned)u->port);
  }
  if (!rq->no_cookies) {
    char ck[1024];
    if (cookie_header(u->host, ck, sizeof ck) > 0) {
      sapp(req, n, cap, "\r\nCookie: ");
      sapp(req, n, cap, ck);
    }
  }
  sapp(req, n, cap, "\r\nUser-Agent: ");
  sapp(req, n, cap, browser_ua());
  sapp(req, n, cap, "\r\nAccept: ");
  sapp(req, n, cap, accept);
  if (rq->ctype && rq->ctype[0]) {
    sapp(req, n, cap, "\r\nContent-Type: ");
    sapp(req, n, cap, rq->ctype);
  }
  if (rq->body_len > 0) {
    sapp(req, n, cap, "\r\nContent-Length: ");
    uapp(req, n, cap, (unsigned)rq->body_len);
  }
  sapp(req, n, cap, "\r\n");
  {
    char ch[512];
    if (ch_headers_for(u, true, ch, sizeof ch) > 0)
      sapp(req, n, cap, ch);
  }
  if (rq->extra_hdrs && rq->extra_hdrs[0])
    sapp(req, n, cap, rq->extra_hdrs); /* caller-supplied, each "Name: value\r\n" */
  sapp(req, n, cap, "\r\n");
}

/* h2 body sink for browser_fetch: append DATA to a growable buffer. */
struct h2buf {
  unsigned char *body;
  int cap, blen;
};
static int h2buf_cb(void *ctx, const unsigned char *p, int n)
{
  struct h2buf *h = ctx;
  for (int i = 0; i < n; i++)
    if (body_put(&h->body, &h->cap, &h->blen, p[i]) < 0)
      return 1;
  return 0;
}

/* Origins that advertised HTTP/3 via Alt-Svc. One that failed over QUIC is benched for a few minutes. */
#define H3_HOSTS_MAX 32
#define H3_BENCH_MS 300000
static struct h3_host {
  char host[URL_HOST_MAX];
  bool benched;
  unsigned until;
} g_h3_hosts[H3_HOSTS_MAX];
static int g_nh3_hosts;
static mutex_t g_h3_lock = MUTEX_INIT;

static struct h3_host *h3_find(const char *host)
{
  for (int i = 0; i < g_nh3_hosts; i++)
    if (streq(g_h3_hosts[i].host, host))
      return &g_h3_hosts[i];
  return 0;
}

bool h3_known(const char *host)
{
  mutex_lock(&g_h3_lock);
  struct h3_host *e = h3_find(host);
  bool known = e && (!e->benched || (int)(msec() - e->until) >= 0);
  mutex_unlock(&g_h3_lock);
  return known;
}

void h3_remember(const char *host)
{
  mutex_lock(&g_h3_lock);
  if (!h3_find(host) && g_nh3_hosts < H3_HOSTS_MAX) {
    struct h3_host *e = &g_h3_hosts[g_nh3_hosts++];
    int k = 0;
    sapp(e->host, &k, sizeof e->host, host);
    e->benched = false;
  }
  mutex_unlock(&g_h3_lock);
}

void h3_forget(const char *host)
{
  mutex_lock(&g_h3_lock);
  struct h3_host *e = h3_find(host);
  if (e) {
    e->benched = true;
    e->until = msec() + H3_BENCH_MS;
  }
  mutex_unlock(&g_h3_lock);
}

/* Remember an origin whose response carries Alt-Svc: h3. */
static void note_alt_svc(const char *host, const char *hdr)
{
  struct http_headers H;
  http_headers_parse(&H, hdr);
  const char *as = http_headers_get(&H, "alt-svc");
  for (const char *p = as; p && *p; p++)
    if ((p[0] == 'h' || p[0] == 'H') && p[1] == '3' && (p[2] == '=' || p[2] == '-')) {
      h3_remember(host);
      break;
    }
  http_headers_free(&H);
}

/* The request headers an h2 or h3 sub-resource GET carries beyond the :-pseudo ones, lowercased: user-agent (h3 only,
 * h2 takes it as an option), cookies, accept, client hints, content-type and the caller's extras. */
static void req_hdrs(const struct purl *u, const char *accept, const struct http_req *rq, bool with_ua, char *xh,
                     int cap)
{
  int xn = 0;
  if (with_ua) {
    sapp(xh, &xn, cap, "user-agent: ");
    sapp(xh, &xn, cap, browser_ua());
    sapp(xh, &xn, cap, "\r\n");
  }
  char ck[1024];
  if (!rq->no_cookies && cookie_header(u->host, ck, sizeof ck) > 0) {
    sapp(xh, &xn, cap, "cookie: ");
    sapp(xh, &xn, cap, ck);
    sapp(xh, &xn, cap, "\r\n");
  }
  sapp(xh, &xn, cap, "accept: ");
  sapp(xh, &xn, cap, accept);
  sapp(xh, &xn, cap, "\r\n");
  char ch[512];
  if (ch_headers_for(u, true, ch, sizeof ch) > 0)
    sapp(xh, &xn, cap, ch);
  if (rq->ctype && rq->ctype[0]) {
    sapp(xh, &xn, cap, "content-type: ");
    sapp(xh, &xn, cap, rq->ctype);
    sapp(xh, &xn, cap, "\r\n");
  }
  if (rq->extra_hdrs && rq->extra_hdrs[0])
    h2_append_headers(xh, &xn, cap, rq->extra_hdrs);
}

static void authority_of(const struct purl *u, char *auth, int cap)
{
  int an = 0;
  sapp(auth, &an, cap, u->host);
  if (u->port != 443) {
    sapp(auth, &an, cap, ":");
    uapp(auth, &an, cap, (unsigned)u->port);
  }
}

/* An h2 session to u: an idle one, else a new connection when ALPN selects h2. Otherwise NULL, with the HTTP/1.1
 * connection in *plain (NULL if the connection failed). `fresh` skips the idle pool. */
h2_session *h2_open(const struct purl *u, bool fresh, netconn **plain)
{
  *plain = 0;
  if (u->tls && !fresh) {
    h2_session *s = h2_session_acquire(u->host, u->port);
    if (s)
      return s;
  }
  netconn *c = fresh ? nc_open_fresh(u->host, u->port, u->tls, /*offer_h2=*/1) : nc_acquire(u->host, u->port, u->tls);
  if (!c)
    return 0;
  if (u->tls && nc_alpn(c) == ALPN_H2)
    return h2_session_wrap(c);
  *plain = c;
  return 0;
}

/* The HTTP/3 half of http_exchange. *fallback is set when h3 can't carry the request (QUIC unreachable, or a body the
 * server won't take), so the caller goes on over h2 or HTTP/1.1; *stale as in http_exchange. */
static unsigned char *h3_exchange(const struct purl *u, const char *method, const char *accept,
                                  const struct http_req *rq, struct resp_meta *meta, char *rawhdr, int *blen,
                                  bool fresh, bool *stale, bool *fallback)
{
  *fallback = false;
  h3_session *s = fresh ? 0 : h3_session_acquire(u->host, u->port);
  bool reused = s != 0;
  if (!s && h3_session_open(&s, u->host, u->port) != 0) {
    h3_forget(u->host);
    *fallback = true;
    return 0;
  }
  char auth[URL_HOST_MAX + 8], xh[1536];
  authority_of(u, auth, sizeof auth);
  xh[0] = '\0';
  req_hdrs(u, accept, rq, true, xh, sizeof xh);
  struct h2buf hb = {malloc(65536), 65536, 0};
  if (!hb.body) {
    h3_session_release(s, u->host, u->port);
    return 0;
  }
  int hlen = 0;
  int rc = h3_session_run(s, auth, method, u->path, xh, rq->body, rq->body_len, 0, rawhdr, RESP_HDR_MAX, &hlen,
                          h2buf_cb, &hb);
  h3_session_release(s, u->host, u->port);
  if (rc < 0) {
    free(hb.body);
    bool has_body = rq->body && rq->body_len > 0;
    *stale = reused && !has_body && (rc == HTTP_ESEND || rc == HTTP_ENORESP);
    if (!*stale) {
      if (!reused)
        h3_forget(u->host);
      *fallback = !has_body || rc == HTTP_ESEND;
    }
    return 0;
  }
  bool chunked;
  long clen;
  parse_resp_headers(rawhdr, meta, &chunked, &clen);
  *blen = hb.blen;
  return hb.body;
}

/* One request and response on an idle or new connection (h3 or h2 session, or HTTP/1.1); the body, or NULL on
 * failure. *stale is set when a reused connection failed before any response, so retrying on a new one is safe. */
static unsigned char *http_exchange(const struct purl *u, const char *method, const char *accept,
                                    const struct http_req *rq, struct resp_meta *meta, char *rawhdr, int *blen,
                                    bool fresh, bool *stale)
{
  *stale = false;
  *blen = 0;
  if (u->tls && h3_known(u->host)) {
    bool fallback;
    unsigned char *b = h3_exchange(u, method, accept, rq, meta, rawhdr, blen, fresh, stale, &fallback);
    if (b)
      note_alt_svc(u->host, rawhdr);
    if (b || !fallback)
      return b;
    *stale = false;
  }
  netconn *conn;
  h2_session *hs = h2_open(u, fresh, &conn);
  if (hs) {
    /* HTTP/2: :authority + lowercased headers; no accept-encoding, so the body is plain. */
    char auth[URL_HOST_MAX + 8], xh[1408];
    authority_of(u, auth, sizeof auth);
    xh[0] = '\0';
    req_hdrs(u, accept, rq, false, xh, sizeof xh);
    struct h2_opts opts = {browser_ua(), xh, 0};
    struct h2buf hb;
    hb.cap = 65536;
    hb.blen = 0;
    hb.body = malloc((unsigned)hb.cap);
    if (!hb.body) {
      h2_session_release(hs, u->host, u->port);
      return 0;
    }
    bool reused = h2_session_used(hs);
    int hlen = 0;
    int rc = h2_session_run(hs, 1, auth, method, u->path, &opts, rq->body, rq->body_len, rawhdr, RESP_HDR_MAX, &hlen,
                            h2buf_cb, &hb);
    h2_session_release(hs, u->host, u->port);
    if (rc < 0) {
      free(hb.body);
      *stale = reused && (rc == HTTP_ESEND || rc == HTTP_ENORESP);
      return 0;
    }
    bool dchunk;
    long dclen;
    parse_resp_headers(rawhdr, meta, &dchunk, &dclen);
    note_alt_svc(u->host, rawhdr);
    *blen = hb.blen;
    return hb.body;
  }
  if (!conn)
    return 0;

  bool pooled = nc_pooled(conn);
  char req[2048];
  int n = 0;
  build_request(req, &n, sizeof req, u, method, accept, rq);
  if (nc_write(conn, req, n) < 0 ||
      (rq->body && rq->body_len > 0 && nc_write(conn, (const char *)rq->body, rq->body_len) < 0)) {
    nc_close(conn);
    *stale = pooled;
    return 0;
  }
  struct ireader r = {conn, {0}, 0, 0, false};
  bool keep;
  unsigned char *body = read_http_body(&r, blen, meta, rawhdr, RESP_HDR_MAX, streq_ci(method, "HEAD"), &keep);
  if (keep)
    nc_release(conn, u->host, u->port, u->tls);
  else
    nc_close(conn);
  *stale = !body && pooled;
  if (body)
    note_alt_svc(u->host, rawhdr);
  return body;
}

/* Single-request transport (h2 or h1.1, follows redirects); NULL on error. Worker-thread only. */
/* Log an unfinished request as cancelled or failed. */
static void net_fail(int id, const char *method, const char *url)
{
  struct net_log e = {
      method, url, 0, 0, 0, 0, 0, B->abort_load ? NET_CANCELLED : NET_FAILED, B->abort_load ? 0 : "Load failed"};
  devtools_net_update(id, &e);
}

static unsigned char *browser_fetch_raw(const struct http_req *rq, struct http_resp *rs)
{
  const char *method = (rq->method && rq->method[0]) ? rq->method : "GET";
  const char *accept = (rq->accept && rq->accept[0]) ? rq->accept : "*/*";
  bool is_get = streq_ci(method, "GET") || streq_ci(method, "HEAD");
  char full[URL_MAX];
  {
    int k = 0;
    full[0] = '\0';
    sapp(full, &k, sizeof full, rq->url);
  }

  rs->status = 0;
  rs->len = 0;
  rs->ctype[0] = '\0';
  if (rs->final_url && rs->final_cap)
    rs->final_url[0] = '\0';
  if (rs->hdr && rs->hdr_cap)
    rs->hdr[0] = '\0';

  if (starts_ci(full, "blob:")) {
    unsigned char *b = is_get ? blobstore_get(full, &rs->len, rs->ctype, sizeof rs->ctype) : 0;
    rs->status = b ? 200 : 0;
    return b;
  }

  /* Cache: only bodyless GETs; fresh hits skip the wire, stale ones revalidate (304 reuses body). */
  bool cacheable = streq_ci(method, "GET") && !(rq->body && rq->body_len > 0);
  long now = (long)rtc_epoch();
  bool revalidating = false;
  char cond[512];
  cond[0] = '\0';
  char origurl[URL_MAX];
  {
    int k = 0;
    origurl[0] = '\0';
    sapp(origurl, &k, sizeof origurl, rq->url);
  }
  struct http_req rqc; /* request copy carrying the cond headers */
  char combined[1408];
  char vary_hdrs[VARY_HDRS_MAX];
  if (cacheable) {
    vary_req_hdrs(origurl, accept, rq->extra_hdrs, !rq->no_cookies, vary_hdrs);
    unsigned char *cbody = 0;
    int clen = 0;
    int cv = cache_lookup(method, origurl, vary_hdrs, now, &cbody, &clen, rs->ctype, sizeof rs->ctype, rs->hdr,
                          rs->hdr_cap, cond, sizeof cond);
    if (cv == CACHE_FRESH) {
      rs->status = 200;
      rs->len = clen;
      if (rs->final_url && rs->final_cap) {
        int k = 0;
        sapp(rs->final_url, &k, rs->final_cap, origurl);
      }
      return cbody;
    }
    if (cv == CACHE_REVALIDATE) {
      revalidating = 1;
      int n = 0;
      if (rq->extra_hdrs && rq->extra_hdrs[0])
        sapp(combined, &n, sizeof combined, rq->extra_hdrs);
      sapp(combined, &n, sizeof combined, cond);
      rqc = *rq;
      rqc.extra_hdrs = combined;
      rq = &rqc;
    }
  }

  for (int hop = 0; hop <= MAX_REDIR; hop++) {
    struct purl u;
    struct resp_meta meta;
    unsigned char *body = 0;
    int blen = 0;
    char rawhdr[RESP_HDR_MAX];
    rawhdr[0] = '\0';
    if (!parse_url(full, &u))
      return 0;
    if (!u.tls && hsts_has(u.host)) {
      u.tls = 1;
      if (u.port == 80)
        u.port = 443;
    }

    bool stale;
    body = http_exchange(&u, method, accept, rq, &meta, rawhdr, &blen, false, &stale);
    if (!body && stale && !(rq->body && rq->body_len > 0))
      body = http_exchange(&u, method, accept, rq, &meta, rawhdr, &blen, true, &stale);
    if (!body)
      return 0;

    if (is_get && is_redirect(meta.status) && meta.location[0]) {
      char next[URL_MAX];
      resolve_url(full, meta.location, next, sizeof next);
      int k = 0;
      full[0] = '\0';
      sapp(full, &k, sizeof full, next);
      free(body);
      continue;
    }

    /* Revalidated 304: reuse the stored body with refreshed freshness. */
    if (revalidating && meta.status == 304) {
      unsigned char *rb = 0;
      int rl = 0;
      if (cache_revalidated(origurl, vary_hdrs, now, rawhdr, &rb, &rl, rs->ctype, sizeof rs->ctype, rs->hdr,
                            rs->hdr_cap)) {
        free(body);
        rs->status = 200;
        rs->len = rl;
        if (rs->final_url && rs->final_cap) {
          int k = 0;
          sapp(rs->final_url, &k, rs->final_cap, origurl);
        }
        return rb;
      }
      /* Entry evicted mid-flight: return the 304 as-is (callers treat non-200 as a miss). */
    }
    /* Store a fresh 200 under the requested URL. */
    if (cacheable && meta.status == 200)
      cache_store(method, origurl, vary_hdrs, 200, now, rawhdr, meta.ctype, body, blen);

    rs->status = meta.status;
    rs->len = blen;
    {
      int k = 0;
      rs->ctype[0] = '\0';
      sapp(rs->ctype, &k, sizeof rs->ctype, meta.ctype);
    }
    if (rs->final_url && rs->final_cap) {
      int k = 0;
      sapp(rs->final_url, &k, rs->final_cap, full);
    }
    if (rs->hdr && rs->hdr_cap) {
      int k = 0;
      for (; rawhdr[k] && k < rs->hdr_cap - 1; k++)
        rs->hdr[k] = rawhdr[k];
      rs->hdr[k] = '\0';
    }
    return body;
  }
  return 0;
}

/* Public entry: wraps browser_fetch_raw with a devtools Network row (pending, then resolved). */
unsigned char *browser_fetch(const struct http_req *rq, struct http_resp *rs)
{
  const char *method = (rq->method && rq->method[0]) ? rq->method : "GET";
  unsigned t0 = msec();
  int nid = devtools_net_begin(method, rq->url);
  unsigned char *body = browser_fetch_raw(rq, rs);
  if (body) {
    const char *url = (rs->final_url && rs->final_cap && rs->final_url[0]) ? rs->final_url : rq->url;
    struct net_log e = {method, url, rs->status, rs->ctype, rs->len, (int)(msec() - t0), rs->hdr, NET_DONE, 0};
    devtools_net_update(nid, &e);
  } else {
    net_fail(nid, method, rq->url);
  }
  return body;
}

/* Body sink over an open owofs write stream; latches a disk error to abort the pump. */
struct filesink {
  long written;
  bool err;
};
static int filesink_cb(void *ctx, const unsigned char *p, int n)
{
  struct filesink *f = ctx;
  if (n <= 0)
    return 0;
  if (stream_write(p, n) < 0) {
    f->err = true;
    return 1;
  } /* abort the pump */
  f->written += n;
  return 0;
}

/* h2 media streaming: headers go to mstream_headers, DATA to mstream_body; 2xx streams to disk. */
struct mstream {
  const char *file; /* temp path to stream a 2xx body into        */
  const char *full; /* current request URL (for Location resolve) */
  char *loc;
  int loc_cap;
  struct filesink fs;
  int status;
  bool began;    /* stream_begin done (a 2xx is being written) */
  bool redirect; /* saw a 3xx with a Location                  */
  char ctype[160];
};
static int mstream_headers(void *ctx, const char *block, int len)
{
  (void)len;
  struct mstream *m = ctx;
  struct resp_meta meta;
  bool ch;
  long cl;
  parse_resp_headers(block, &meta, &ch, &cl);
  m->status = meta.status;
  {
    int k = 0;
    for (; meta.ctype[k] && k < (int)sizeof m->ctype - 1; k++)
      m->ctype[k] = meta.ctype[k];
    m->ctype[k] = '\0';
  }
  if (is_redirect(meta.status) && meta.location[0]) {
    m->redirect = true;
    resolve_url(m->full, meta.location, m->loc, m->loc_cap);
  } else if (meta.status >= 200 && meta.status < 300 && !m->began && !m->fs.err) {
    if (stream_begin(m->file) == 0)
      m->began = true;
    else
      m->fs.err = true;
  }
  return 0;
}
static int mstream_body(void *ctx, const unsigned char *p, int n)
{
  struct mstream *m = ctx;
  if (m->began && !m->fs.err)
    return filesink_cb(&m->fs, p, n);
  return 0; /* redirect / error body: discard */
}

long browser_fetch_media(const char *url, const char *accept, const char *file, int *status)
{
  char full[URL_MAX];
  {
    int k = 0;
    full[0] = '\0';
    sapp(full, &k, sizeof full, url);
  }
  if (!accept || !accept[0])
    accept = "*/*";
  if (status)
    *status = 0;
  if (starts_ci(full, "blob:")) {
    int blen = 0;
    unsigned char *b = blobstore_get(full, &blen, 0, 0);
    if (!b)
      return -1;
    if (status)
      *status = 200;
    if (stream_begin(file) != 0) {
      free(b);
      return -1;
    }
    int wrote = stream_write(b, blen);
    free(b);
    long committed = stream_end();
    return wrote < 0 ? -1 : committed;
  }
  unsigned t0 = msec();
  int nid = devtools_net_begin("GET", full); /* in-flight until the download resolves */

  for (int hop = 0; hop <= MAX_REDIR; hop++) {
    struct purl u;
    if (!parse_url(full, &u)) {
      net_fail(nid, "GET", full);
      return -1;
    }
    if (!u.tls && hsts_has(u.host)) {
      u.tls = 1;
      if (u.port == 80)
        u.port = 443;
    }

    /* Offer h2; the body streams to disk either way (h3 is document-only). */
    netconn *conn = nc_open(u.host, u.port, u.tls, /*offer_h2=*/1);
    if (!conn) {
      net_fail(nid, "GET", full);
      return -1;
    }

    if (u.tls && nc_alpn(conn) == ALPN_H2) {
      /* HTTP/2: :authority + lowercased headers; no Connection/Transfer-Encoding (forbidden in h2). */
      char auth[URL_HOST_MAX + 8];
      int an = 0;
      sapp(auth, &an, sizeof auth, u.host);
      if (u.port != 443) {
        sapp(auth, &an, sizeof auth, ":");
        uapp(auth, &an, sizeof auth, (unsigned)u.port);
      }
      char xh[1408];
      int xn = 0;
      char ck[1024];
      if (cookie_header(u.host, ck, sizeof ck) > 0) {
        sapp(xh, &xn, sizeof xh, "cookie: ");
        sapp(xh, &xn, sizeof xh, ck);
        sapp(xh, &xn, sizeof xh, "\r\n");
      }
      sapp(xh, &xn, sizeof xh, "accept: ");
      sapp(xh, &xn, sizeof xh, accept);
      sapp(xh, &xn, sizeof xh, "\r\n");
      {
        char ch[512];
        if (ch_headers_for(&u, true, ch, sizeof ch) > 0)
          sapp(xh, &xn, sizeof xh, ch);
      }
      sapp(xh, &xn, sizeof xh, "accept-encoding: identity\r\nrange: bytes=0-\r\n");

      char rawhdr[RESP_HDR_MAX];
      rawhdr[0] = '\0';
      char loc[URL_MAX];
      loc[0] = '\0';
      struct mstream m = {file, full, loc, sizeof loc, {0, 0}, 0, 0, 0, {0}};
      struct h2_opts opts = {browser_ua(), xh, mstream_headers};
      int hlen = 0;
      int rc = h2_run(conn, 1, auth, "GET", u.path, &opts, rawhdr, sizeof rawhdr, &hlen, mstream_body, &m);
      nc_close(conn);

      if (m.redirect && !m.began) { /* follow the redirect */
        int k = 0;
        full[0] = '\0';
        sapp(full, &k, sizeof full, loc);
        continue;
      }
      if (status)
        *status = m.status;
      if (rc < 0 || m.fs.err) { /* stream reset or disk error */
        if (m.began)
          stream_end();
        net_fail(nid, "GET", full);
        return -1;
      }
      if (!m.began) { /* a real non-2xx response */
        struct net_log e = {"GET", full, m.status, m.ctype, 0, (int)(msec() - t0), rawhdr, NET_DONE, 0};
        devtools_net_update(nid, &e);
        return -1;
      }
      long committed = stream_end();
      if (committed < 0) {
        net_fail(nid, "GET", full);
        return -1;
      }
      struct net_log e = {"GET", full, m.status, m.ctype, (int)committed, (int)(msec() - t0), rawhdr, NET_DONE, 0};
      devtools_net_update(nid, &e);
      return committed;
    }

    /* Open-ended Range so servers answer 206; identity coding so the body lands on disk verbatim. */
    char req[2048];
    int n = 0;
    sapp(req, &n, sizeof req, "GET ");
    sapp(req, &n, sizeof req, u.path);
    sapp(req, &n, sizeof req, " HTTP/1.1\r\nHost: ");
    sapp(req, &n, sizeof req, u.host);
    if (!((u.tls && u.port == 443) || (!u.tls && u.port == 80))) {
      sapp(req, &n, sizeof req, ":");
      uapp(req, &n, sizeof req, (unsigned)u.port);
    }
    {
      char ck[1024];
      if (cookie_header(u.host, ck, sizeof ck) > 0) {
        sapp(req, &n, sizeof req, "\r\nCookie: ");
        sapp(req, &n, sizeof req, ck);
      }
    }
    sapp(req, &n, sizeof req, "\r\nUser-Agent: ");
    sapp(req, &n, sizeof req, browser_ua());
    sapp(req, &n, sizeof req, "\r\nAccept: ");
    sapp(req, &n, sizeof req, accept);
    {
      char ch[512];
      if (ch_headers_for(&u, true, ch, sizeof ch) > 0)
        sapp(req, &n, sizeof req, ch);
    }
    sapp(req, &n, sizeof req,
         "\r\nAccept-Encoding: identity\r\nRange: bytes=0-\r\nConnection: "
         "close\r\n\r\n");
    if (nc_write(conn, req, n) < 0) {
      nc_close(conn);
      net_fail(nid, "GET", full);
      return -1;
    }

    struct ireader r = {conn, {0}, 0, 0, false};
    char rawhdr[RESP_HDR_MAX];
    if (ir_read_header_block(&r, rawhdr, sizeof rawhdr) < 0) {
      nc_close(conn);
      net_fail(nid, "GET", full);
      return -1;
    }
    struct resp_meta meta;
    bool chunked;
    long clen;
    parse_resp_headers(rawhdr, &meta, &chunked, &clen);

    if (is_redirect(meta.status) && meta.location[0]) {
      char next[URL_MAX];
      resolve_url(full, meta.location, next, sizeof next);
      int k = 0;
      full[0] = '\0';
      sapp(full, &k, sizeof full, next);
      nc_close(conn);
      continue;
    }
    if (status)
      *status = meta.status;
    if (meta.status < 200 || meta.status >= 300) {
      nc_close(conn);
      /* Real 4xx/5xx response: record as completed, not a connectivity failure. */
      struct net_log e = {"GET", full, meta.status, meta.ctype, 0, (int)(msec() - t0), rawhdr, NET_DONE, 0};
      devtools_net_update(nid, &e);
      return -1;
    }

    /* 2xx: stream the body to disk; stream_begin is deferred so redirects reserve no disk. */
    if (stream_begin(file) != 0) {
      nc_close(conn);
      net_fail(nid, "GET", full);
      return -1;
    }
    struct filesink fs = {0, false};
    long got = ir_pump_body(&r, chunked, clen, filesink_cb, &fs);
    nc_close(conn);
    if (got < 0 || fs.err) {
      stream_end();
      net_fail(nid, "GET", full);
      return -1;
    } /* commit-close, then fail */
    long committed = stream_end();
    if (committed < 0) {
      net_fail(nid, "GET", full);
      return -1;
    }

    {
      struct net_log e = {"GET",    full, meta.status, meta.ctype, (int)committed, (int)(msec() - t0), rawhdr,
                          NET_DONE, 0};
      devtools_net_update(nid, &e);
    }
    return committed;
  }
  net_fail(nid, "GET", full);
  return -1;
}

/* CORS transport for fetch()/XHR: Origin header, preflight for non-simple requests, ACAO checks. */
/* Returns the body (caller frees), or NULL on transport error or CORS denial. */
unsigned char *browser_cors_fetch(const char *page_url, int with_credentials, const struct http_req *rq,
                                  struct http_resp *rs)
{
  const char *method = (rq->method && rq->method[0]) ? rq->method : "GET";
  const char *author = rq->extra_hdrs;

  /* Same-origin: ordinary credentialed request, no CORS dance. */
  if (cors_same_origin(page_url, rq->url))
    return browser_fetch(rq, rs);

  char origin[URL_HOST_MAX + 24];
  cors_origin(page_url, origin, sizeof origin);

  /* Build the cross-origin request headers: Origin first, then the page's. */
  char xh[1280];
  int xn = 0;
  sapp(xh, &xn, sizeof xh, "Origin: ");
  sapp(xh, &xn, sizeof xh, origin);
  sapp(xh, &xn, sizeof xh, "\r\n");
  if (author && author[0])
    sapp(xh, &xn, sizeof xh, author);

  /* Non-simple request: preflight first (uncredentialed, method + non-simple header names only). */
  if (cors_needs_preflight(method, rq->ctype, author)) {
    char ph[1280];
    int pn = 0;
    sapp(ph, &pn, sizeof ph, "Origin: ");
    sapp(ph, &pn, sizeof ph, origin);
    sapp(ph, &pn, sizeof ph, "\r\nAccess-Control-Request-Method: ");
    sapp(ph, &pn, sizeof ph, method);
    sapp(ph, &pn, sizeof ph, "\r\n");
    if (author && author[0]) {
      char names[512];
      cors_request_header_names(author, names, sizeof names);
      if (names[0]) {
        sapp(ph, &pn, sizeof ph, "Access-Control-Request-Headers: ");
        sapp(ph, &pn, sizeof ph, names);
        sapp(ph, &pn, sizeof ph, "\r\n");
      }
    }
    char phdr[4096];
    struct http_req prq = {0};
    prq.url = rq->url;
    prq.method = "OPTIONS";
    prq.accept = "*/*";
    prq.extra_hdrs = ph;
    prq.no_cookies = 1;
    struct http_resp prs = {0};
    prs.hdr = phdr;
    prs.hdr_cap = sizeof phdr;
    unsigned char *pbody = browser_fetch(&prq, &prs);
    if (!pbody)
      return 0;
    free(pbody);
    struct http_headers PH;
    http_headers_parse(&PH, phdr);
    char reason[64];
    bool ok = cors_check_preflight(&PH, origin, method, author, with_credentials, reason, sizeof reason);
    http_headers_free(&PH);
    if (!ok)
      return 0;
  }

  /* Actual request: Origin'd headers, cookies gated on the credentials mode. */
  struct http_req areq = *rq;
  areq.method = method;
  areq.accept = "*/*";
  areq.extra_hdrs = xh;
  areq.no_cookies = !with_credentials;
  char rawhdr[RESP_HDR_MAX];
  struct http_resp ars = {0};
  ars.hdr = rawhdr;
  ars.hdr_cap = sizeof rawhdr;
  unsigned char *body = browser_fetch(&areq, &ars);
  if (!body)
    return 0;

  struct http_headers H;
  http_headers_parse(&H, rawhdr);
  char reason[64];
  bool ok = cors_check_response(&H, origin, with_credentials, reason, sizeof reason);
  http_headers_free(&H);
  if (!ok) {
    free(body);
    return 0;
  }

  rs->status = ars.status;
  rs->len = ars.len;
  {
    int k = 0;
    rs->ctype[0] = '\0';
    sapp(rs->ctype, &k, sizeof rs->ctype, ars.ctype);
  }
  return body;
}

/* --- parallel sub-resource fetch pool --- */
/* Run fn(ctx, i) for i in [0,n) across pool threads plus this one, then join. Worker-thread only. */
/* Jobs use their own sockets and disjoint state; non-nested, threads find the job via g_par. */
#define PAR_FETCHERS 4
#define PAR_FETCH_STACK (128 * 1024)

struct par_job {
  volatile int next; /* next index to claim                       */
  mutex_t lock;      /* guards `next` */
  int n;
  void (*fn)(void *ctx, int i);
  void *ctx;
};
static struct par_job *g_par;

static int par_claim(struct par_job *j)
{
  int got = -1;
  mutex_lock(&j->lock);
  if (j->next < j->n)
    got = j->next++;
  mutex_unlock(&j->lock);
  return got;
}

static void par_run_loop(struct par_job *j)
{
  int i;
  while ((i = par_claim(j)) >= 0)
    j->fn(j->ctx, i);
}

static void par_thread(void)
{
  par_run_loop(g_par);
  exit(0);
} /* a thread fn must not return */

static void par_fetch(int n, void (*fn)(void *ctx, int i), void *ctx)
{
  if (n <= 0)
    return;
  struct par_job job = {0, MUTEX_INIT, n, fn, ctx};
  g_par = &job;

  int want = n - 1;
  if (want > PAR_FETCHERS - 1)
    want = PAR_FETCHERS - 1;
  int tids[PAR_FETCHERS - 1];
  char *stks[PAR_FETCHERS - 1];
  void *tops[PAR_FETCHERS - 1];
  int nthr = 0;
  for (int t = 0; t < want; t++) {
    /* Prefer a guard-paged kernel stack so overflow faults cleanly; else plain malloc. */
    char *stk = 0;
    void *top = thread_stack_alloc(PAR_FETCH_STACK);
    if (!top) {
      stk = malloc(PAR_FETCH_STACK);
      if (!stk)
        break;
      top = stk + PAR_FETCH_STACK;
    }
    int tid = thread_create(par_thread, top);
    if (tid < 0) {
      if (stk)
        free(stk);
      else
        thread_stack_free(top);
      break;
    }
    stks[nthr] = stk;
    tops[nthr] = top;
    tids[nthr] = tid;
    nthr++;
  }
  par_run_loop(&job); /* this thread fetches too */
  for (int t = 0; t < nthr; t++) {
    thread_join(tids[t]);
    if (stks[t])
      free(stks[t]);
    else
      thread_stack_free(tops[t]);
  }
  g_par = 0;
}

/* --- iframe fetching + embedding policy --- */
/* The framed document decides via X-Frame-Options or CSP frame-ancestors, matched by origin. */

/* Two URLs share an origin when scheme (tls), host and port all match. */
static bool origin_eq(const char *a, const char *b)
{
  struct purl ua, ub;
  if (!parse_url(a, &ua) || !parse_url(b, &ub))
    return false;
  return ua.tls == ub.tls && ua.port == ub.port && streq_ci(ua.host, ub.host);
}

/* Value of CSP directive `name` (at a ';'/start boundary) as a pointer past the name, or NULL. */
static const char *csp_directive(const char *csp, const char *name)
{
  if (!csp)
    return 0;
  int nl = (int)strlen(name);
  for (const char *p = csp; *p; p++) {
    if (p != csp && p[-1] != ';' && p[-1] != ' ')
      continue;
    if (!starts_ci(p, name))
      continue;
    const char *q = p + nl;
    if (*q != ' ' && *q != ';' && *q != '\0')
      continue; /* whole token */
    while (*q == ' ')
      q++;
    return q;
  }
  return 0;
}

/* Does frame-ancestors token [tok,tend) admit `parent` ('self' uses `framed`)? */
static bool csp_src_admits(const char *tok, const char *tend, const char *parent, const char *framed)
{
  int n = (int)(tend - tok);
  if (n == 6 && starts_ci(tok, "'self'"))
    return origin_eq(parent, framed);
  if (n == 1 && tok[0] == '*')
    return true;

  char s[256];
  int k = 0;
  for (; k < n && k < (int)sizeof s - 1; k++)
    s[k] = tok[k];
  s[k] = '\0';
  struct purl pp;
  if (!parse_url(parent, &pp))
    return false;
  if (contains_ci(s, "://"))
    return origin_eq(parent, s);    /* full origin */
  if (s[0] == '*' && s[1] == '.') { /* host suffix */
    const char *suf = s + 1;
    int hl = (int)strlen(pp.host), sl = (int)strlen(suf);
    return hl >= sl && streq_ci(pp.host + (hl - sl), suf);
  }
  return streq_ci(pp.host, s); /* bare host */
}

/* Whether `parent` may embed `framed` per XFO/CSP (frame-ancestors wins); sets `reason` if refused. */
static bool frame_embedding_allowed(const char *parent, const char *framed, const char *xfo, const char *csp,
                                    char *reason, int rcap)
{
  if (reason && rcap)
    reason[0] = '\0';

  const char *fa = csp_directive(csp, "frame-ancestors");
  if (fa) {
    const char *p = fa;
    bool any = false, ok = false;
    while (*p && *p != ';') {
      while (*p == ' ')
        p++;
      const char *t = p;
      while (*p && *p != ' ' && *p != ';')
        p++;
      if (p == t)
        break;
      any = true;
      if ((p - t) == 6 && starts_ci(t, "'none'")) {
        if (reason && rcap) {
          int k = 0;
          sapp(reason, &k, rcap, "CSP frame-ancestors 'none'");
        }
        return false;
      }
      if (csp_src_admits(t, p, parent, framed))
        ok = true;
    }
    if (!any || ok)
      return true;
    if (reason && rcap) {
      int k = 0;
      sapp(reason, &k, rcap, "CSP frame-ancestors");
    }
    return false;
  }

  if (xfo && xfo[0]) {
    const char *v = xfo;
    while (*v == ' ')
      v++;
    if (starts_ci(v, "deny")) {
      if (reason && rcap) {
        int k = 0;
        sapp(reason, &k, rcap, "X-Frame-Options: DENY");
      }
      return false;
    }
    if (starts_ci(v, "sameorigin")) {
      if (origin_eq(parent, framed))
        return true;
      if (reason && rcap) {
        int k = 0;
        sapp(reason, &k, rcap, "X-Frame-Options: SAMEORIGIN");
      }
      return false;
    }
    if (starts_ci(v, "allow-from")) {
      const char *u = v + 10;
      while (*u == ' ' || *u == '=')
        u++;
      char uri[URL_MAX];
      int k = 0;
      for (; u[k] && u[k] != ' ' && u[k] != ';' && k < (int)sizeof uri - 1; k++)
        uri[k] = u[k];
      uri[k] = '\0';
      if (uri[0] && origin_eq(parent, uri))
        return true;
      if (reason && rcap) {
        int kk = 0;
        sapp(reason, &kk, rcap, "X-Frame-Options: ALLOW-FROM");
      }
      return false;
    }
    /* Unknown / malformed value: browsers ignore it, so do we. */
  }
  return true;
}

/* Decode a data: URL (base64 or percent-encoded) into a fresh buffer + media type. */
static int rhex(int c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}
unsigned char *decode_data_url(const char *url, int *outlen, char *ctype, int cap)
{
  const char *p = url + 5; /* past "data:" */
  const char *comma = p;
  while (*comma && *comma != ',')
    comma++;
  if (*comma != ',')
    return 0;

  char mt[80];
  int mi = 0;
  bool base64 = false;
  const char *h = p;
  while (h < comma && *h != ';') {
    if (mi < (int)sizeof mt - 1)
      mt[mi++] = lower(*h);
    h++;
  }
  mt[mi] = '\0';
  while (h < comma) {
    h++;
    const char *tok = h;
    while (h < comma && *h != ';')
      h++;
    if (h - tok == 6 && starts_ci(tok, "base64"))
      base64 = true;
  }
  {
    int k = 0;
    ctype[0] = '\0';
    sapp(ctype, &k, cap, mt[0] ? mt : "text/plain");
  }

  const char *d = comma + 1;
  if (base64) { /* shared base64 decoder */
    int plen = (int)strlen(d);
    unsigned char *out = malloc((unsigned)(base64_decoded_cap(plen) + 1));
    if (!out)
      return 0;
    int m = base64_decode(d, plen, out);
    if (m < 0) {
      free(out);
      return 0;
    }
    *outlen = m;
    return out;
  }
  int cap2 = 4096, len = 0; /* percent-decode into a growable buffer */
  unsigned char *out = malloc((unsigned)cap2);
  if (!out)
    return 0;
  for (; *d; d++) {
    int c = (unsigned char)*d;
    if (c == '%') {
      int hi = rhex((unsigned char)d[1]);
      int lo = hi < 0 ? -1 : rhex((unsigned char)d[2]);
      if (lo >= 0) {
        c = hi * 16 + lo;
        d += 2;
      }
    }
    if (body_put(&out, &cap2, &len, c) < 0)
      break;
  }
  *outlen = len;
  return out;
}

#define IFRAME_BATCH 16

/* One iframe's result: filled by a pool thread, consumed on the worker thread (sole DOM mutator). */
struct iframe_res {
  char url[URL_MAX];
  char parent[URL_MAX];
  int kind; /* FRAME_OK / FRAME_ERROR / FRAME_BLOCKED   */
  char reason[96];
  unsigned char *body; /* malloc'd (freed after resolve), or NULL  */
  int len;
  char ctype[160];
};

/* Fetch + policy-check one pending iframe into res[i]; thread-safe (own res entry and sockets). */
static void iframe_job(void *vctx, int i)
{
  struct iframe_res *r = &((struct iframe_res *)vctx)[i];
  r->body = 0;
  r->len = 0;
  r->ctype[0] = '\0';
  r->reason[0] = '\0';

  if (starts_ci(r->url, "data:") || starts_ci(r->url, "blob:")) {
    int len = 0;
    unsigned char *body = starts_ci(r->url, "blob:") ? blobstore_get(r->url, &len, r->ctype, sizeof r->ctype)
                                                     : decode_data_url(r->url, &len, r->ctype, sizeof r->ctype);
    if (body) {
      r->kind = FRAME_OK;
      r->body = body;
      r->len = len;
    } else {
      r->kind = FRAME_ERROR;
      int k = 0;
      sapp(r->reason, &k, sizeof r->reason, "bad data: or blob: URL");
    }
    return;
  }
  if (!starts_ci(r->url, "http://") && !starts_ci(r->url, "https://")) {
    r->kind = FRAME_ERROR;
    int k = 0;
    sapp(r->reason, &k, sizeof r->reason, "unsupported scheme");
    return;
  }

  char final[URL_MAX];
  char hdr[RESP_HDR_MAX];
  struct http_req rq = {0};
  rq.url = r->url;
  rq.method = "GET";
  rq.accept = "text/html,*/*";
  struct http_resp rs = {0};
  rs.final_url = final;
  rs.final_cap = sizeof final;
  rs.hdr = hdr;
  rs.hdr_cap = sizeof hdr;
  unsigned char *body = browser_fetch(&rq, &rs);
  if (!body) {
    r->kind = FRAME_ERROR;
    int k = 0;
    sapp(r->reason, &k, sizeof r->reason, "no response");
    return;
  }
  if (rs.status < 200 || rs.status >= 300) {
    int k = 0;
    sapp(r->reason, &k, sizeof r->reason, "HTTP ");
    uapp(r->reason, &k, sizeof r->reason, (unsigned)rs.status);
    r->kind = FRAME_ERROR;
    free(body);
    return;
  }

  struct http_headers H;
  http_headers_parse(&H, hdr);
  const char *xfo = http_headers_get(&H, "x-frame-options");
  const char *csp = http_headers_get(&H, "content-security-policy");
  const char *framed = final[0] ? final : r->url;
  bool ok = frame_embedding_allowed(r->parent, framed, xfo, csp, r->reason, sizeof r->reason);
  if (ok && !cors_same_origin(r->parent, framed)) /* CORP also guards a framed resource */
    ok = corp_allows(&H, r->parent, framed, r->reason, sizeof r->reason);
  http_headers_free(&H);

  r->kind = ok ? FRAME_OK : FRAME_BLOCKED;
  r->body = body;
  r->len = rs.len;
  {
    int k = 0;
    sapp(r->ctype, &k, sizeof r->ctype, rs.ctype);
  }
}

/* Fetch pending iframes in parallel (data: inline, else GET + XFO/CSP/CORP) and resolve on this thread. */
/* Beyond IFRAME_BATCH or nested frames wait for the next pass. */
void fetch_iframes(void)
{
  struct iframe_res *res = malloc(sizeof *res * IFRAME_BATCH);
  if (!res)
    return;

  int n = 0;
  while (n < IFRAME_BATCH &&
         html_iframe_next_pending(res[n].url, sizeof res[n].url, res[n].parent, sizeof res[n].parent))
    n++;
  if (n == 0) {
    free(res);
    return;
  }

  par_fetch(n, iframe_job, res);

  for (int i = 0; i < n; i++) {
    html_iframe_resolve(res[i].url, res[i].parent, res[i].kind, res[i].reason, res[i].body, res[i].len,
                        res[i].body ? res[i].ctype : 0);
    free(res[i].body);
  }
  free(res);
}

/* Over h3: the batch on one QUIC connection. False if QUIC could not carry it, leaving the requests untouched. */
static bool h3_many(const struct purl *u, const char *accept, struct h2_fetch *reqs, int n,
                    void (*on_done)(void *ctx, int i, const unsigned char *body, int blen), void *cb_ctx)
{
  char auth[URL_HOST_MAX + 8], xh[1280];
  struct http_req none = {0};
  authority_of(u, auth, sizeof auth);
  xh[0] = '\0';
  req_hdrs(u, accept, &none, true, xh, sizeof xh);
  for (int attempt = 0;; attempt++) {
    h3_session *s = attempt ? 0 : h3_session_acquire(u->host, u->port);
    bool reused = s != 0;
    if (!s && h3_session_open(&s, u->host, u->port) != 0) {
      h3_forget(u->host);
      return false;
    }
    int rc = h3_session_fetch_many(s, auth, xh, reqs, n, on_done, cb_ctx);
    h3_session_release(s, u->host, u->port);
    bool answered = false;
    for (int g = 0; g < n; g++)
      answered |= reqs[g].status > 0;
    if (rc >= 0 || answered)
      return true;
    for (int g = 0; g < n; g++) {
      free(reqs[g].body);
      reqs[g].body = 0;
      reqs[g].blen = reqs[g].hdrlen = reqs[g].failed = 0;
    }
    if (!reused || attempt) {
      h3_forget(u->host);
      return false;
    }
  }
}

/* GET reqs[0..n) of one HTTPS origin, multiplexed on one connection: over h3 when the origin advertised it, else h2.
 * If neither carries them (HTTP/1.1 only, unreachable) the requests stay untouched, and *plain holds the HTTP/1.1
 * connection opened on the way, for the caller to park. */
static void origin_fetch_many(const struct purl *u, const char *accept, struct h2_fetch *reqs, int n,
                              void (*on_done)(void *ctx, int i, const unsigned char *body, int blen), void *cb_ctx,
                              netconn **plain)
{
  *plain = 0;
  if (h3_known(u->host) && h3_many(u, accept, reqs, n, on_done, cb_ctx))
    return;
  h2_session *hs = h2_open(u, false, plain);
  if (!hs)
    return;
  char auth[URL_HOST_MAX + 8], xh[1280];
  struct http_req none = {0};
  authority_of(u, auth, sizeof auth);
  xh[0] = '\0';
  req_hdrs(u, accept, &none, false, xh, sizeof xh);
  struct h2_opts o = {browser_ua(), xh, 0};
  for (int attempt = 0; hs; attempt++) {
    bool reused = h2_session_used(hs), answered = false;
    int rc = h2_session_fetch_many(hs, 1, auth, &o, reqs, n, on_done, cb_ctx);
    h2_session_release(hs, u->host, u->port);
    hs = 0;
    for (int g = 0; g < n; g++)
      answered |= reqs[g].status > 0;
    if (rc >= 0 || !reused || answered || attempt)
      break;
    /* An idle session the peer had closed: redo the batch on a new connection. */
    for (int g = 0; g < n; g++) {
      free(reqs[g].body);
      reqs[g].body = 0;
      reqs[g].blen = reqs[g].hdrlen = reqs[g].failed = 0;
    }
    hs = h2_open(u, true, plain);
  }
}

/* Shared state for a parallel subres batch: URLs, outputs, claim bitmap and its lock. */
struct subres_batch {
  char *const *urls;
  int n;
  const char *accept;
  unsigned char **bodies;
  int *lens;
  bool *handled;
  mutex_t lock;
};

/* Fetch the origin group of claimed URL `i`: HTTPS multiplexed over one h2 connection, http over h1.1. */
/* Only the claim is locked; groups for distinct origins run concurrently. */
static void subres_job(void *vctx, int i)
{
  struct subres_batch *c = vctx;
  struct purl u;
  int grp[H2_FETCH_MAX], ng = 0;

  mutex_lock(&c->lock);
  if (c->handled[i]) {
    mutex_unlock(&c->lock);
    return;
  } /* claimed by another group */
  if (!origin_of(c->urls[i], &u)) {
    c->handled[i] = true;
    mutex_unlock(&c->lock);
    return;
  }
  if (!u.tls) {
    c->handled[i] = true;
    grp[ng++] = i; /* plain http: this one */
  } else {
    for (int j = i; j < c->n && ng < H2_FETCH_MAX; j++) { /* same-origin HTTPS group */
      if (c->handled[j])
        continue;
      struct purl v;
      if (!origin_of(c->urls[j], &v)) {
        c->handled[j] = true;
        continue;
      }
      if (!v.tls || !streq_ci(v.host, u.host) || v.port != u.port)
        continue;
      c->handled[j] = true;
      grp[ng++] = j;
    }
  }
  mutex_unlock(&c->lock);

  if (!u.tls) { /* plain http: single h1.1 */
    c->bodies[grp[0]] = subres_get(c->urls[grp[0]], c->accept, &c->lens[grp[0]]);
    return;
  }

  char (*paths)[URL_MAX] = malloc((unsigned)H2_FETCH_MAX * URL_MAX);
  if (!paths) { /* OOM: fall back to h1.1 each */
    for (int g = 0; g < ng; g++)
      c->bodies[grp[g]] = subres_get(c->urls[grp[g]], c->accept, &c->lens[grp[g]]);
    return;
  }
  /* Per-stream header buffers so a fetched 200 can be cached; NULL on OOM = no caching. */
  char (*hdrs)[RESP_HDR_MAX] = malloc((unsigned)ng * RESP_HDR_MAX);
  long now = (long)rtc_epoch();
  struct h2_fetch reqs[H2_FETCH_MAX];
  for (int g = 0; g < ng; g++) {
    struct purl v;
    origin_of(c->urls[grp[g]], &v);
    int k = 0;
    for (; v.path[k] && k < URL_MAX - 1; k++)
      paths[g][k] = v.path[k];
    paths[g][k] = '\0';
    reqs[g].path = paths[g];
    reqs[g].status = 0;
    reqs[g].body = 0;
    reqs[g].blen = 0;
    reqs[g].failed = 0;
    reqs[g].hdr = hdrs ? hdrs[g] : 0;
    reqs[g].hdrcap = hdrs ? RESP_HDR_MAX : 0;
    reqs[g].hdrlen = 0;
  }

  netconn *plain;
  origin_fetch_many(&u, c->accept, reqs, ng, 0, 0, &plain);
  if (plain)
    nc_release(plain, u.host, u.port, 1); /* HTTP/1.1 only: park it for the fetches below */
  for (int g = 0; g < ng; g++) {
    /* Only a 200 is usable; others are dropped and retried over h1.1 like subres_get. */
    if (!reqs[g].failed && reqs[g].status == 200 && reqs[g].body) {
      c->bodies[grp[g]] = reqs[g].body;
      c->lens[grp[g]] = reqs[g].blen;
      if (reqs[g].hdr && reqs[g].hdrlen > 0) { /* cache the fetched 200 */
        char vh[VARY_HDRS_MAX];
        vary_req_hdrs(c->urls[grp[g]], c->accept, 0, true, vh);
        cache_store("GET", c->urls[grp[g]], vh, reqs[g].status, now, reqs[g].hdr, "", reqs[g].body, reqs[g].blen);
      }
      {
        struct net_log e = {"GET", c->urls[grp[g]], reqs[g].status, "", reqs[g].blen, 0, 0};
        devtools_net_add(&e);
      }
    } else {
      if (reqs[g].body)
        free(reqs[g].body); /* h1.1 retry (logs itself) */
      c->bodies[grp[g]] = subres_get(c->urls[grp[g]], c->accept, &c->lens[grp[g]]);
    }
  }
  free(paths);
  free(hdrs);
}

/* Fetch n URLs into bodies[i]/lens[i] (malloc'd, NULL on failure); origin groups run in parallel. */
void subres_get_batch(char *const *urls, int n, const char *accept, unsigned char **bodies, int *lens)
{
  if (n <= 0)
    return;
  for (int i = 0; i < n; i++) {
    bodies[i] = 0;
    lens[i] = 0;
  }

  /* `handled` marks URLs already claimed by an origin group; sized to n, no fixed cap. */
  bool *handled = calloc((unsigned)n, sizeof *handled);
  if (!handled) { /* OOM: degrade to one fetch at a time */
    for (int i = 0; i < n; i++)
      bodies[i] = subres_get(urls[i], accept, &lens[i]);
    return;
  }

  /* data: and blob: URLs resolve inline (origin_of rejects them); mark handled even on failure. */
  for (int i = 0; i < n; i++) {
    if (starts_ci(urls[i], "data:") || starts_ci(urls[i], "blob:")) {
      char ct[64];
      bodies[i] = starts_ci(urls[i], "blob:") ? blobstore_get(urls[i], &lens[i], ct, sizeof ct)
                                              : decode_data_url(urls[i], &lens[i], ct, sizeof ct);
      handled[i] = true;
    }
  }

  /* Serve fresh cache hits first (single-threaded, before the pool) and mark them handled. */
  long now = (long)rtc_epoch();
  for (int i = 0; i < n; i++) {
    unsigned char *cb = 0;
    int cl = 0;
    char vh[VARY_HDRS_MAX];
    vary_req_hdrs(urls[i], accept, 0, true, vh);
    if (cache_lookup("GET", urls[i], vh, now, &cb, &cl, 0, 0, 0, 0, 0, 0) == CACHE_FRESH) {
      bodies[i] = cb;
      lens[i] = cl;
      handled[i] = true;
      struct net_log e = {"GET", urls[i], 200, "", cl, 0, 0};
      devtools_net_add(&e);
    }
  }

  struct subres_batch ctx = {urls, n, accept, bodies, lens, handled, MUTEX_INIT};
  par_fetch(n, subres_job, &ctx);
  free(handled);
}

/* Batch hook for html.c's external stylesheets. */
void css_batch(char *const *urls, int n, unsigned char **bodies, int *lens)
{
  subres_get_batch(urls, n, SUBRES_ACCEPT, bodies, lens);
}

/* --- parallel inline-image fetching --- */
/* Pool threads commit into img_pool; a short lock guards img_pool_len, copies go to disjoint regions. */
#define IMG_FETCHERS 4
#define IMG_FETCH_STACK (128 * 1024)

static char g_img_page[URL_MAX]; /* page URL the fetchers resolve against */
static volatile int g_img_next;  /* next image index to claim             */
static mutex_t g_img_lock;       /* guards g_img_next + the pool bump */

/* Claim the next eager (non-lazy) image index, or -1 when none remain. */
static int img_claim_next(void)
{
  int got = -1;
  mutex_lock(&g_img_lock);
  while (g_img_next < B->pg->n_imgs) {
    int i = g_img_next++;
    if (!B->pg->imgs[i].lazy && !B->pg->imgs[i].ok && !B->pg->imgs[i].fetched) {
      got = i;
      break;
    } /* skip h2-done */
  }
  mutex_unlock(&g_img_lock);
  return got;
}

/* Commit a fetched body into img_pool slot `idx`: SVG rasterized with `currentColor`, PNG stored verbatim. */
/* Sets im->ok last; does not free `body`. Shared by the h1.1 fetcher and h2 prefetch. */
static void commit_image(int idx, const unsigned char *body, int blen, unsigned current_color)
{
  struct inl_img *im = &B->pg->imgs[idx];

  /* Decoder-controlled ww/hh: size the copy in 64-bit and reject absurd dimensions (no 32-bit wrap). */
  if (image_sniff(body, blen) != IMG_FMT_PNG) {
    unsigned ww = 0, hh = 0, delay = 0;
    struct imganim *anim = 0;
    unsigned char *keep = 0;
    unsigned char *rgba = imganim_sniff(body, blen) != IMGANIM_NONE
                              ? image_decode_first_frame(body, blen, &anim, &keep, &ww, &hh, &delay)
                              : image_decode_rgba(body, blen, current_color, 256, &ww, &hh);
    if (rgba) {
      long long bytes = (long long)ww * hh * 4;
      int off = -1, uid = 0;
      mutex_lock(&g_img_lock);
      if (ww && hh && ww <= 16384 && hh <= 16384 && bytes <= (long long)(IMG_POOL - B->img_pool_len)) {
        off = B->img_pool_len;
        B->img_pool_len += (int)bytes;
        if (anim)
          uid = ++B->pg->img_uid_next;
      }
      mutex_unlock(&g_img_lock);
      if (off >= 0) {
        memcpy(B->img_pool + off, rgba, (unsigned)bytes);
        im->coff = off;
        im->clen = (int)bytes;
        im->iw = ww;
        im->ih = hh;
        im->is_argb = 1; /* raw RGBA */
        im->uid = uid;
        if (anim && animimg_add(uid, anim, keep, delay))
          anim = 0;
        im->ok = 1; /* set last: the UI draws once ok is seen */
        B->seq++;
      }
      free(rgba);
    }
    if (anim) {
      imganim_close(anim);
      free(keep);
    }
    return;
  }

  if (blen >= 8 && body[0] == 0x89 && body[1] == 'P' && body[2] == 'N' &&
      body[3] == 'G') { /* PNG stored verbatim (lazy decode) */
    struct memrd mr = {body, 0, blen};
    unsigned iw = 0, ih = 0;
    bool okdim = (png_dimensions(memrd_get, &mr, &iw, &ih) == 0 && iw && ih);

    int off = -1;
    mutex_lock(&g_img_lock);
    if (B->img_pool_len + blen <= IMG_POOL) {
      off = B->img_pool_len;
      B->img_pool_len += blen;
    }
    mutex_unlock(&g_img_lock);
    if (off >= 0) {
      memcpy(B->img_pool + off, body, (unsigned)blen); /* disjoint: no lock */
      im->coff = off;
      im->clen = blen;
      if (okdim) {
        im->iw = iw;
        im->ih = ih;
        im->uid = ++B->pg->img_uid_next;
      }
      im->ok = okdim; /* set last: the UI draws once ok is seen */
      B->seq++;       /* repaint as each image lands            */
    }
  }
}

/* Commit in-memory image bytes (e.g. an inline <svg> serialized by html.c) into slot `idx`; no fetch. */
void resource_commit_inline(int idx, const unsigned char *body, int len, unsigned current_color)
{
  commit_image(idx, body, len, current_color);
}

/* Commit raw RGBA pixels (a canvas bitmap) into slot `idx` as an unfetched image. Returns its uid, 0 if the pool is
 * full. */
int resource_commit_rgba(int idx, const unsigned char *rgba, unsigned w, unsigned h)
{
  struct inl_img *im = &B->pg->imgs[idx];
  long long bytes = (long long)w * h * 4;
  int off = -1, uid = 0;
  mutex_lock(&g_img_lock);
  if (w && h && bytes <= (long long)(IMG_POOL - B->img_pool_len)) {
    off = B->img_pool_len;
    B->img_pool_len += (int)bytes;
    uid = ++B->pg->img_uid_next;
  }
  mutex_unlock(&g_img_lock);
  if (off < 0)
    return 0;
  memcpy(B->img_pool + off, rgba, (unsigned)bytes);
  im->coff = off;
  im->clen = (int)bytes;
  im->iw = w;
  im->ih = h;
  im->is_argb = 1;
  im->uid = uid;
  im->ok = 1; /* set last: the UI draws once ok is seen */
  B->seq++;
  return uid;
}

/* Fetch inline image `i` over its own HTTP/1.1 socket and commit it; thread-safe. */
static void fetch_image(int i, const char *page_url)
{
  B->pg->imgs[i].ok = 0;
  char full[URL_MAX];
  resolve_url(page_url, B->pg->imgs[i].href, full, sizeof full);
  int blen = 0;
  unsigned char *body = subres_get(full, IMG_ACCEPT, &blen);
  if (!body)
    return;
  commit_image(i, body, blen, 0x000000);
  free(body);
}

static bool favicon_decode(const unsigned char *r, int n); /* defined below */

/* h2 batch on_done: map request index to its target (FAV_IDX = favicon, else image index). */
#define FAV_IDX (-1)
struct h2img_ctx {
  const int *idxs;
  struct h2_fetch *reqs;
  long now;
};
static void h2img_commit(void *ctx, int b, const unsigned char *body, int blen)
{
  const struct h2img_ctx *c = ctx;
  char full[URL_MAX];
  if (c->idxs[b] == FAV_IDX) {
    resolve_url(g_img_page, B->pg->favicon_href[0] ? B->pg->favicon_href : "/favicon.ico", full, sizeof full);
    {
      struct net_log e = {"GET", full, 200, "", blen, 0, 0};
      devtools_net_add(&e);
    }
    if (favicon_decode(body, blen)) {
      B->pg->favicon_ok = 1;
      B->seq++;
    }
  } else {
    resolve_url(g_img_page, B->pg->imgs[c->idxs[b]].href, full, sizeof full);
    {
      struct net_log e = {"GET", full, 200, "image/*", blen, 0, 0};
      devtools_net_add(&e);
    }
    commit_image(c->idxs[b], body, blen, 0x000000);
  }
  /* Cache the fetched 200 (the batch surfaced its headers into reqs[b].hdr). */
  if (c->reqs[b].hdr && c->reqs[b].hdrlen > 0) {
    char vh[VARY_HDRS_MAX];
    vary_req_hdrs(full, IMG_ACCEPT, 0, true, vh);
    cache_store("GET", full, vh, 200, c->now, c->reqs[b].hdr, "", body, blen);
  }
}

/* Multiplex same-origin HTTPS eager images over ONE h2 connection; the rest fall through to the pool. */
/* Single-threaded: h2 HPACK/frame state is process-global. */
static void h2_prefetch(const char *page_url)
{
  struct purl pu;
  if (!parse_url(page_url, &pu) || !pu.tls)
    return;

  char (*paths)[URL_MAX] = malloc((unsigned)H2_FETCH_MAX * URL_MAX);
  if (!paths)
    return;
  /* Per-stream header buffers so a fetched 200 can be cached (h2img_commit). */
  char (*hdrs)[RESP_HDR_MAX] = malloc((unsigned)H2_FETCH_MAX * RESP_HDR_MAX);
  long now = (long)rtc_epoch();
  struct h2_fetch reqs[H2_FETCH_MAX];
  int idxs[H2_FETCH_MAX], nb = 0;

  for (int i = 0; i < B->pg->n_imgs && nb < H2_FETCH_MAX; i++) {
    struct inl_img *im = &B->pg->imgs[i];
    if (im->lazy || im->ok || im->fetched)
      continue;
    char full[URL_MAX];
    resolve_url(page_url, im->href, full, sizeof full);
    struct purl u;
    if (!parse_url(full, &u))
      continue;
    if (!u.tls && hsts_has(u.host)) {
      u.tls = 1;
      if (u.port == 80)
        u.port = 443;
    }
    if (!u.tls || !streq_ci(u.host, pu.host) || u.port != pu.port)
      continue; /* same origin only */
    int k = 0;
    for (; u.path[k] && k < URL_MAX - 1; k++)
      paths[nb][k] = u.path[k];
    paths[nb][k] = '\0';
    reqs[nb].path = paths[nb];
    reqs[nb].status = 0;
    reqs[nb].body = 0;
    reqs[nb].blen = 0;
    reqs[nb].failed = 0;
    reqs[nb].hdr = hdrs ? hdrs[nb] : 0;
    reqs[nb].hdrcap = hdrs ? RESP_HDR_MAX : 0;
    reqs[nb].hdrlen = 0;
    idxs[nb] = i;
    nb++;
  }

  /* Fold a same-origin HTTPS favicon into this batch; others use fetch_favicon's h1.1 path. */
  if (nb < H2_FETCH_MAX && !B->pg->favicon_ok) {
    char fav[URL_MAX];
    resolve_url(page_url, B->pg->favicon_href[0] ? B->pg->favicon_href : "/favicon.ico", fav, sizeof fav);
    /* A fresh cached favicon skips the batch entirely. */
    unsigned char *fb = 0;
    int fl = 0;
    char vh[VARY_HDRS_MAX];
    vary_req_hdrs(fav, IMG_ACCEPT, 0, true, vh);
    if (cache_lookup("GET", fav, vh, now, &fb, &fl, 0, 0, 0, 0, 0, 0) == CACHE_FRESH) {
      if (favicon_decode(fb, fl)) {
        B->pg->favicon_ok = 1;
        B->seq++;
      }
      free(fb);
    }
    struct purl fu;
    if (!B->pg->favicon_ok && parse_url(fav, &fu) && fu.tls && streq_ci(fu.host, pu.host) && fu.port == pu.port) {
      int k = 0;
      for (; fu.path[k] && k < URL_MAX - 1; k++)
        paths[nb][k] = fu.path[k];
      paths[nb][k] = '\0';
      reqs[nb].path = paths[nb];
      reqs[nb].status = 0;
      reqs[nb].body = 0;
      reqs[nb].blen = 0;
      reqs[nb].failed = 0;
      reqs[nb].hdr = hdrs ? hdrs[nb] : 0;
      reqs[nb].hdrcap = hdrs ? RESP_HDR_MAX : 0;
      reqs[nb].hdrlen = 0;
      idxs[nb] = FAV_IDX;
      nb++;
    }
  }
  if (nb == 0) {
    free(paths);
    free(hdrs);
    return;
  }

  /* Commit each image as its stream finishes; a failed stream leaves ok=0 for the h1.1 pool. */
  struct h2img_ctx hc = {idxs, reqs, now};
  netconn *plain;
  origin_fetch_many(&pu, IMG_ACCEPT, reqs, nb, h2img_commit, &hc, &plain);
  if (plain)
    nc_release(plain, pu.host, pu.port, 1);
  free(paths);
  free(hdrs);
}

/* Per-thread loop: claim and fetch images until none remain. */
static void img_fetch_loop(void)
{
  int i;
  while ((i = img_claim_next()) >= 0)
    fetch_image(i, g_img_page);
}

static void img_fetcher_thread(void)
{
  img_fetch_loop();
  exit(0); /* a thread fn must not return */
}

/* Fetch eager images (loading != "lazy"): h2 prefetch for same-origin HTTPS, then an h1.1 thread pool. */
/* Lazy images wait for fetch_lazy_visible(). */
void fetch_inline_images(const char *page_url)
{
  int pending = 0;
  for (int i = 0; i < B->pg->n_imgs; i++)
    pending += !B->pg->imgs[i].lazy && !B->pg->imgs[i].ok && !B->pg->imgs[i].fetched;
  if (!pending)
    return;
  {
    int k = 0;
    g_img_page[0] = '\0';
    sapp(g_img_page, &k, sizeof g_img_page, page_url);
  }
  g_img_next = 0;

  /* Serve fresh cached images first (single-threaded); commit_image sets ok so fetchers skip them. */
  {
    long now = (long)rtc_epoch();
    for (int i = 0; i < B->pg->n_imgs; i++) {
      struct inl_img *im = &B->pg->imgs[i];
      if (im->lazy || im->ok || im->fetched)
        continue;
      char full[URL_MAX];
      resolve_url(page_url, im->href, full, sizeof full);
      unsigned char *cb = 0;
      int cl = 0;
      char vh[VARY_HDRS_MAX];
      vary_req_hdrs(full, IMG_ACCEPT, 0, true, vh);
      if (cache_lookup("GET", full, vh, now, &cb, &cl, 0, 0, 0, 0, 0, 0) == CACHE_FRESH) {
        commit_image(i, cb, cl, 0x000000);
        free(cb);
        struct net_log e = {"GET", full, 200, "image/*", cl, 0, 0};
        devtools_net_add(&e);
      }
    }
  }

  /* h2 for same-origin HTTPS images; the rest go through the h1.1 pool. */
  h2_prefetch(page_url);

  int tids[IMG_FETCHERS - 1];
  char *stks[IMG_FETCHERS - 1];
  void *tops[IMG_FETCHERS - 1];
  int nthr = 0;
  for (int t = 0; t < IMG_FETCHERS - 1; t++) {
    char *stk = 0; /* guarded stack preferred; malloc fallback */
    void *top = thread_stack_alloc(IMG_FETCH_STACK);
    if (!top) {
      stk = malloc(IMG_FETCH_STACK);
      if (!stk)
        break;
      top = stk + IMG_FETCH_STACK;
    }
    int tid = thread_create(img_fetcher_thread, top);
    if (tid < 0) {
      if (stk)
        free(stk);
      else
        thread_stack_free(top);
      break;
    }
    stks[nthr] = stk;
    tops[nthr] = top;
    tids[nthr] = tid;
    nthr++;
  }

  img_fetch_loop(); /* this thread fetches too */

  for (int t = 0; t < nthr; t++) { /* wait for the helpers, free their stacks */
    thread_join(tids[t]);
    if (stks[t])
      free(stks[t]);
    else
      thread_stack_free(tops[t]);
  }
}

/* Worker step: load one lazy <img> flagged as scrolled into view; returns 1 if fetched, else 0. */
int fetch_lazy_visible(void)
{
  if (!B->pg->cur_url[0])
    return 0;
  for (int i = 0; i < B->pg->n_imgs; i++) {
    struct inl_img *im = &B->pg->imgs[i];
    if (!im->lazy || im->fetched || !im->want)
      continue;
    fetch_image(i, B->pg->cur_url);
    im->fetched = 1;
    im->want = 0;
    B->seq++;      /* re-layout now that the image has landed   */
    cache_flush(); /* persist the lazily-fetched image (worker thread) */
    return 1;
  }
  return 0;
}

/* --- favicon decode --- */
/* Favicon -> 16x16 ARGB (B->pg->favicon): PNG via png, .ico parsed here; else the default globe. */

/* Nearest-neighbour scale of a decoded RGBA image into the 16x16 favicon, per png row callback. */
struct favscale {
  unsigned iw, ih, next_r;
  unsigned *dst;
};
static int fav_row(void *c, unsigned y, const unsigned char *rgba, unsigned w)
{
  (void)w;
  struct favscale *v = c;
  while (v->next_r < FAVICON_H && (v->next_r * v->ih / FAVICON_H) == y) {
    unsigned *drow = v->dst + v->next_r * FAVICON_W;
    for (unsigned x = 0; x < FAVICON_W; x++) {
      const unsigned char *p = rgba + (x * v->iw / FAVICON_W) * 4;
      drow[x] = ((unsigned)p[3] << 24) | ((unsigned)p[0] << 16) | ((unsigned)p[1] << 8) | (unsigned)p[2];
    }
    v->next_r++;
  }
  return 0;
}

static bool fav_decode_png(const unsigned char *buf, int len)
{
  struct memrd mr = {buf, 0, len};
  unsigned iw = 0, ih = 0;
  if (png_dimensions(memrd_get, &mr, &iw, &ih) != 0 || !iw || !ih)
    return false;
  struct favscale v = {iw, ih, 0, B->pg->favicon};
  struct memrd mr2 = {buf, 0, len};
  if (png_decode(memrd_get, &mr2, fav_row, &v, 0, 0) != 0)
    return false;
  return v.next_r > 0;
}

/* Little-endian readers for the .ico container (always little-endian). */
static unsigned le16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }
static unsigned le32(const unsigned char *p)
{
  return p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

/* Decode an ICO DIB image (no BMP file header): bottom-up, biHeight covers XOR rows plus AND mask. */
/* Supports 1/4/8/24/32 bpp BI_RGB, sampled directly at 16x16. */
static bool fav_decode_dib(const unsigned char *p, int len)
{
  if (len < 40)
    return false;
  unsigned hsz = le32(p);
  if (hsz < 40 || (int)hsz > len)
    return false;
  int W = (int)le32(p + 4);
  int Hf = (int)le32(p + 8);
  unsigned bpp = le16(p + 14);
  unsigned comp = le32(p + 16);
  if (comp != 0 && comp != 3)
    return false; /* BI_RGB / BI_BITFIELDS only */
  if (W <= 0 || Hf <= 0)
    return false;

  int H = Hf / 2;
  bool mask = true; /* XOR colour rows = half     */
  if (H < 1) {
    H = Hf;
    mask = false;
  } /* no AND mask present         */

  unsigned clr = le32(p + 32);
  if (bpp <= 8 && clr == 0)
    clr = 1u << bpp;
  const unsigned char *pal = p + hsz;
  int pal_sz = (bpp <= 8) ? (int)clr * 4 : 0;

  int xstride = ((W * (int)bpp + 31) / 32) * 4;
  const unsigned char *xor_ = pal + pal_sz;
  long xor_sz = (long)xstride * H;
  if ((xor_ - p) + xor_sz > len)
    return false;

  int astride = ((W + 31) / 32) * 4;
  const unsigned char *and_ = xor_ + xor_sz;
  bool and_present = mask && ((and_ - p) + (long)astride * H <= len);

  /* Trust 32bpp alpha only if some pixel is non-zero; else use the AND mask. */
  bool has_alpha = false;
  if (bpp == 32)
    for (long i = 3; i < xor_sz && !has_alpha; i += 4)
      if (xor_[i])
        has_alpha = true;

  for (int dy = 0; dy < FAVICON_H; dy++) {
    int sy = dy * H / FAVICON_H; /* dest row -> source row      */
    int br = H - 1 - sy;         /* DIB rows are bottom-up      */
    const unsigned char *xrow = xor_ + (long)br * xstride;
    const unsigned char *arow = and_ + (long)br * astride;
    for (int dx = 0; dx < FAVICON_W; dx++) {
      int sx = dx * W / FAVICON_W;
      unsigned r = 0, g = 0, b = 0, a = 255;
      if (bpp == 32) {
        const unsigned char *q = xrow + sx * 4;
        b = q[0];
        g = q[1];
        r = q[2];
        a = has_alpha ? q[3] : 255;
      } else if (bpp == 24) {
        const unsigned char *q = xrow + sx * 3;
        b = q[0];
        g = q[1];
        r = q[2];
      } else {
        unsigned idx;
        if (bpp == 8)
          idx = xrow[sx];
        else if (bpp == 4)
          idx = (sx & 1) ? (xrow[sx >> 1] & 0xf) : (xrow[sx >> 1] >> 4);
        else if (bpp == 1)
          idx = (xrow[sx >> 3] >> (7 - (sx & 7))) & 1;
        else
          return false; /* unsupported depth           */
        if (idx >= clr)
          idx = 0;
        const unsigned char *c = pal + idx * 4;
        b = c[0];
        g = c[1];
        r = c[2];
      }
      if (bpp != 32 && and_present) {
        unsigned mb = arow[sx >> 3];
        if ((mb >> (7 - (sx & 7))) & 1)
          a = 0; /* mask bit set = clear   */
      }
      B->pg->favicon[dy * FAVICON_W + dx] = (a << 24) | (r << 16) | (g << 8) | b;
    }
  }
  return true;
}

/* Pick the smallest ico entry >= 16px (exact 16 wins), else the largest; fills poff/plen. */
static bool ico_pick(const unsigned char *p, int len, int *poff, int *plen)
{
  if (len < 6 || le16(p) != 0 || le16(p + 2) != 1)
    return false; /* type 1 = icon */
  int count = le16(p + 4);
  if (count < 1 || 6 + count * 16 > len)
    return false;
  int best = -1, best_score = -1;
  for (int i = 0; i < count; i++) {
    const unsigned char *e = p + 6 + i * 16;
    int w = e[0] ? e[0] : 256;
    int boff = (int)le32(e + 12), blen = (int)le32(e + 8);
    if (boff < 0 || blen <= 0 || boff + blen > len)
      continue;
    int score = (w >= FAVICON_W) ? (1000 - w) : w;
    if (score > best_score) {
      best_score = score;
      best = i;
      *poff = boff;
      *plen = blen;
    }
  }
  return best >= 0;
}

/* Decode a fetched favicon into B->pg->favicon; returns 1 on success. */
static bool favicon_decode(const unsigned char *r, int n)
{
  if (n < 4)
    return false;
  if (r[0] == 0x89 && r[1] == 'P' && r[2] == 'N' && r[3] == 'G')
    return fav_decode_png(r, n);
  if (r[0] == 0 && r[1] == 0 && r[2] == 1 && r[3] == 0) { /* .ico        */
    int off = 0, l = 0;
    if (!ico_pick(r, n, &off, &l))
      return false;
    const unsigned char *img = r + off;
    if (l >= 8 && img[0] == 0x89 && img[1] == 'P' && img[2] == 'N' && img[3] == 'G') /* PNG in .ico */
      return fav_decode_png(img, l);
    return fav_decode_dib(img, l);
  }
  if (svg_probe((const char *)r, n)) /* SVG favicon */
    return svg_render((const char *)r, n, FAVICON_W, FAVICON_H, B->pg->favicon, 0x000000);
  return false; /* GIF/BMP favicons unsupported */
}

/* Fetch and decode the favicon (<link rel=icon> href, else /favicon.ico); leaves favicon_ok 0 on failure. */
void fetch_favicon(const char *page_url)
{
  if (B->pg->favicon_ok)
    return; /* already decoded on the image h2 batch */
  char full[URL_MAX];
  resolve_url(page_url, B->pg->favicon_href[0] ? B->pg->favicon_href : "/favicon.ico", full, sizeof full);
  int len = 0;
  unsigned char *body = subres_get(full, "image/png,image/x-icon,image/svg+xml,*/*", &len);
  if (!body)
    return;
  if (favicon_decode(body, len))
    B->pg->favicon_ok = 1;
  free(body);
}
