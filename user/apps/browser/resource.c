/* resource.c — sub-resource fetching for the browser: a self-contained HTTP/1.1
 * GET (own socket, follows redirects, sends cookies, HSTS-upgrade) shared by
 * the parallel inline-image fetcher, the favicon, and external stylesheets.
 * These run while the page renders, each on its own socket, so the multi-flow
 * kernel TLS lets several proceed at once. The main page transaction lives in
 * fetch.c. */
#include "base64.h" /* shared base64 for data: URL decoding */
#include "browser.h"
#include "cache.h"       /* the HTTP response cache (RFC 7234, honours headers)   */
#include "clienthints.h" /* Accept-CH: per-origin Client Hints opt-in        */
#include "cors.h"        /* CORS request orchestration + CORP sub-resource guard  */
#include "fetchint.h"
#include "headers.h" /* HTTP response headers (iframe X-Frame-Options / CSP)  */
#include "imgdec.h"  /* shared image decoder (SVG/WebP/PNG) for <img> */
#include "svg.h"     /* SVG -> ARGB rasterizer (favicon path) */

/* The origin that owns the sub-resources currently being fetched (the page, set
 * by do_fetch). Cross-origin no-cors loads are checked against it for CORP. */
static char g_embedder[URL_MAX];
void resource_set_page(const char *page_url)
{
  int k = 0;
  g_embedder[0] = '\0';
  if (page_url)
    sapp(g_embedder, &k, sizeof g_embedder, page_url);
}

/* --------------------------------------------------- inline image fetching */

/* A byte reader over a slice of img_pool, for upng_dimensions. */
struct memrd {
  const unsigned char *p;
  int pos, len;
};
static int memrd_get(void *c)
{
  struct memrd *m = c;
  return m->pos < m->len ? m->p[m->pos++] : -1;
}

/* Self-contained per-thread HTTP reader: several images fetch in parallel, each
 * over its OWN netconn/buffer, so they never touch B->sock/B->pg->raw (which
 * only the page fetch uses). The user-space TLS is per-connection, so the HTTPS
 * handshakes run concurrently. */
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

/* Append one byte to a malloc'd, doubling body buffer (capped at RAW_MAX).
 * Returns 0, or -1 if full / out of memory (the caller stops). */
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

/* Grow *body (doubling, capped at RAW_MAX) so it holds at least `need` bytes.
 * Returns the resulting capacity, which is < need only when the cap or OOM
 * stops the growth. */
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

/* Bulk-copy up to `want` body bytes from the reader into *body, taking whole
 * buffered runs (memcpy) instead of a call per byte. Refills from the socket as
 * needed. Returns the count copied; short of `want` means EOF or the RAW_MAX
 * cap. */
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

/* Read a response header block (up to the blank line) into `hdr`,
 * NUL-terminated. Returns its length, or -1 if the connection closed before the
 * block completed. */
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

/* Pump up to `want` body bytes from the reader to `sink` (whole buffered runs
 * at a time), refilling from the socket as needed. `want < 0` means read to
 * close. Returns the count pumped (short of `want` = EOF), or -1 if the sink
 * aborts. */
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

/* Stream a response's framed body (chunked / content-length / read-to-close) to
 * `sink`, undoing the transfer framing. Returns the content byte count, or -1
 * if the sink aborts (a disk error). `clen < 0` with !chunked means read to
 * close. */
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

/* Response line/header facts a sub-resource fetch needs: the status code and a
 * Location (for redirects). */
struct resp_meta {
  int status;
  char location[URL_MAX];
  char ctype[160];
};

/* Parse a response header block (an HTTP/1.1 block, or the HTTP/2-synthesized
 * "HTTP/2 NNN\r\nname: value\r\n…"): status + Location + Content-Type into
 * `meta`, and the framing (chunked / Content-Length) for the body reader.
 * Shared by the h1.1 socket reader and the h2 path. */
static void parse_resp_headers(const char *hdr, struct resp_meta *meta, bool *chunked, long *clen)
{
  meta->status = 0;
  meta->location[0] = '\0';
  meta->ctype[0] = '\0';
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

/* Read a response's body (chunked / content-length / read-to-close) off an open
 * sub-resource request into a freshly malloc'd buffer. Fills *meta (status +
 * Location) and returns the body (caller frees) with *outlen set, for ANY
 * status — the caller decides what to do with a redirect or an error. NULL only
 * on a connection error / OOM. Bounded by RAW_MAX. */
static unsigned char *read_http_body(struct ireader *r, int *outlen, struct resp_meta *meta, char *hdr_out, int hdr_cap)
{
  char hdr[RESP_HDR_MAX];
  if (ir_read_header_block(r, hdr, (int)sizeof hdr) < 0)
    return 0;
  if (hdr_out && hdr_cap > 0) { /* hand the raw block to the caller */
    int k = 0;
    for (; hdr[k] && k < hdr_cap - 1; k++)
      hdr_out[k] = hdr[k];
    hdr_out[k] = '\0';
  }

  bool chunked;
  long clen;
  parse_resp_headers(hdr, meta, &chunked, &clen);

  int cap = (clen > 0 && clen < (long)RAW_MAX) ? (int)clen + 1 : 65536;
  unsigned char *body = malloc((unsigned)cap);
  if (!body)
    return 0;
  int blen = 0;

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
      if (ir_drain(r, &body, &cap, &blen, (long)sz) != (long)sz)
        goto done;
      c = ir_getc(r);
      if (c == '\r')
        ir_getc(r); /* trailing CRLF */
    }
  } else {
    long limit = (clen >= 0) ? clen : (long)RAW_MAX;
    ir_drain(r, &body, &cap, &blen, limit);
  }
done:
  *outlen = blen;
  return body;
}

static bool is_redirect(int s) { return s == 301 || s == 302 || s == 303 || s == 307 || s == 308; }

/* Fetch an absolute URL as a sub-resource (GET, follow redirects): the 200 body
 * (caller frees) + *outlen, or NULL. A thin wrapper over browser_fetch (the one
 * single-request transport: h2 when the server offers it, else h1.1), which
 * also gives sub-resources HTTP/2. Sends cookies, HSTS-upgrades, runs
 * concurrently (the kernel TLS is multi-flow), and drops a cross-origin
 * response the server's Cross-Origin-Resource-Policy forbids this page from
 * using. */
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

  /* CORP: a cross-origin no-cors resource may forbid this document embedding
   * it. */
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

/* Build an arbitrary-method request (for the JS fetch()): like build_get but
 * with the method, an optional Content-Type, and a Content-Length when a body
 * follows. */
/* Append a caller's "Name: value\r\n" header blob to an HTTP/2 header buffer,
 * lowercasing each field name (HTTP/2 requires lowercase header names). */
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
  sapp(req, n, cap, "\r\nConnection: close\r\n");
  {
    char ch[512];
    if (ch_request_headers(u->host, ch, sizeof ch) > 0)
      sapp(req, n, cap, ch);
  }
  if (rq->extra_hdrs && rq->extra_hdrs[0])
    sapp(req, n, cap, rq->extra_hdrs); /* caller-supplied, each "Name: value\r\n" */
  sapp(req, n, cap, "\r\n");
}

/* h2 response body sink for browser_fetch: append DATA into a growable buffer.
 */
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

/* The single-request transport core: one GET/POST (h2 when offered, else h1.1),
 * following redirects, returning the body for ANY final status. The optional
 * out fields of `rs` (final URL after redirects, the raw header block) let a
 * caller that needs more than status+ctype (the iframe path reads
 * X-Frame-Options/CSP, the CORS path reads Access-Control-*) get them without a
 * second request. NULL on a connection/OOM error. Worker-thread only. */
/* Log an unfinished request as cancelled (the user stopped the load) or failed
 * (a connectivity / transport error), resolving the in-flight row `id`. */
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

  /* HTTP cache: only a bodyless GET is cacheable. A fresh copy skips the wire
   * entirely; a stale one rides a conditional GET (the `cond` headers) and a
   * 304 reuses the stored body. Keyed on the requested URL (origurl), so a
   * repeat hits the cache before re-walking any redirect chain. */
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
  if (cacheable) {
    unsigned char *cbody = 0;
    int clen = 0;
    int cv = cache_lookup(method, origurl, now, &cbody, &clen, rs->ctype, sizeof rs->ctype, rs->hdr, rs->hdr_cap, cond,
                          sizeof cond);
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

    netconn *conn = nc_open(u.host, u.port, u.tls, /*offer_h2=*/1);
    if (!conn)
      return 0;

    if (u.tls && nc_alpn(conn) == ALPN_H2) {
      /* HTTP/2. :authority + lowercased headers; no accept-encoding so the
       * body comes back plain (no decompression needed here). */
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
      if (!rq->no_cookies && cookie_header(u.host, ck, sizeof ck) > 0) {
        sapp(xh, &xn, sizeof xh, "cookie: ");
        sapp(xh, &xn, sizeof xh, ck);
        sapp(xh, &xn, sizeof xh, "\r\n");
      }
      sapp(xh, &xn, sizeof xh, "accept: ");
      sapp(xh, &xn, sizeof xh, accept);
      sapp(xh, &xn, sizeof xh, "\r\n");
      {
        char ch[512];
        if (ch_request_headers(u.host, ch, sizeof ch) > 0)
          sapp(xh, &xn, sizeof xh, ch);
      }
      if (rq->ctype && rq->ctype[0]) {
        sapp(xh, &xn, sizeof xh, "content-type: ");
        sapp(xh, &xn, sizeof xh, rq->ctype);
        sapp(xh, &xn, sizeof xh, "\r\n");
      }
      if (rq->extra_hdrs && rq->extra_hdrs[0])
        h2_append_headers(xh, &xn, sizeof xh, rq->extra_hdrs);
      struct h2_opts opts = {browser_ua(), xh, 0};
      struct h2buf hb;
      hb.cap = 65536;
      hb.blen = 0;
      hb.body = malloc((unsigned)hb.cap);
      if (!hb.body) {
        nc_close(conn);
        return 0;
      }
      int hlen = 0;
      int rc = h2_run_body(conn, 1, auth, method, u.path, &opts, rq->body, rq->body_len, rawhdr, sizeof rawhdr, &hlen,
                           h2buf_cb, &hb);
      nc_close(conn);
      if (rc < 0) {
        if (hb.body)
          free(hb.body);
        return 0;
      }
      bool dchunk;
      long dclen;
      parse_resp_headers(rawhdr, &meta, &dchunk, &dclen);
      body = hb.body ? hb.body : malloc(1);
      blen = hb.blen;
    } else {
      char req[2048];
      int n = 0;
      build_request(req, &n, sizeof req, &u, method, accept, rq);
      if (nc_write(conn, req, n) < 0) {
        nc_close(conn);
        return 0;
      }
      if (rq->body && rq->body_len > 0 && nc_write(conn, (const char *)rq->body, rq->body_len) < 0) {
        nc_close(conn);
        return 0;
      }
      struct ireader r = {conn, {0}, 0, 0, false};
      body = read_http_body(&r, &blen, &meta, rawhdr, sizeof rawhdr);
      nc_close(conn);
    }
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

    /* A revalidation that came back 304: reuse the stored body (its freshness
     * refreshed from the 304's headers). */
    if (revalidating && meta.status == 304) {
      unsigned char *rb = 0;
      int rl = 0;
      if (cache_revalidated(origurl, now, rawhdr, &rb, &rl, rs->ctype, sizeof rs->ctype, rs->hdr, rs->hdr_cap)) {
        free(body);
        rs->status = 200;
        rs->len = rl;
        if (rs->final_url && rs->final_cap) {
          int k = 0;
          sapp(rs->final_url, &k, rs->final_cap, origurl);
        }
        return rb;
      }
      /* Entry vanished (evicted mid-flight): fall through and hand back the
       * 304 as-is; the caller treats a non-200 as a miss and refetches. */
    }
    /* Store a fresh 200 under the requested URL (cache_store vets the headers).
     */
    if (cacheable && meta.status == 200)
      cache_store(method, origurl, 200, now, rawhdr, meta.ctype, body, blen);

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

/* Public transport entry. browser_fetch_raw does the HTTP work; this frames
 * every request in the devtools Network panel: an in-flight (pending) row
 * logged the moment it starts, resolved to done / failed / cancelled when raw
 * returns — so a request shows while loading and however it ends, not only on
 * success. */
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

/* A body sink over the open owofs write stream: append each run, tracking the
 * total and latching a disk error (which aborts the pump). */
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

/* HTTP/2 media streaming: the h2 engine delivers the response headers (once,
 * before any DATA) to mstream_headers, then each DATA payload to mstream_body.
 * On a 2xx the header hook opens the disk stream so DATA writes straight to it;
 * a redirect is noted (its target resolved) and its body discarded. */
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

    /* Offer h2 like every other sub-resource load: whichever the server picks,
     * the body streams straight to disk (h2 via the DATA callback, h1.1 via the
     * framed body pump). h3 is document-only in this browser, so media matches
     * the h2/h1.1 sub-resource path. */
    netconn *conn = nc_open(u.host, u.port, u.tls, /*offer_h2=*/1);
    if (!conn) {
      net_fail(nid, "GET", full);
      return -1;
    }

    if (u.tls && nc_alpn(conn) == ALPN_H2) {
      /* HTTP/2: :authority + lowercased headers, DATA streamed to disk by the
       * mstream callbacks. No Connection/Transfer-Encoding (forbidden in h2).
       */
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
        if (ch_request_headers(u.host, ch, sizeof ch) > 0)
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

    /* GET with an open-ended Range so a partial-content server answers 206;
     * identity coding so the body writes to disk verbatim (media is already
     * compressed and this path does not decode Content-Encoding). */
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
      if (ch_request_headers(u.host, ch, sizeof ch) > 0)
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
      /* A real HTTP response arrived (4xx/5xx): record it as completed with its
       * status, not a connectivity failure. */
      struct net_log e = {"GET", full, meta.status, meta.ctype, 0, (int)(msec() - t0), rawhdr, NET_DONE, 0};
      devtools_net_update(nid, &e);
      return -1;
    }

    /* 2xx (200 whole-body or 206 partial): stream the body straight to disk.
     * stream_begin is deferred to here so a redirect chain reserves no disk. */
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

/* The CORS transport for fetch()/XMLHttpRequest. A same-origin request behaves
 * like browser_fetch (cookies sent, no extra checks). A cross-origin request
 * carries an Origin header; a non-simple one is gated by a preflight OPTIONS,
 * and the response is only handed back if its Access-Control-Allow-* headers
 * admit `page_url`'s origin. Cookies ride only when `with_credentials` (fetch
 * credentials:"include" / XHR withCredentials), per the CORS credentials rules.
 *
 * Returns the body (caller frees) with `rs` filled; NULL on a transport error
 * OR a CORS denial — the JS layer surfaces both as a network error (TypeError /
 * xhr "error"), so they need not be distinguished. The page-set "Name:
 * value\r\n" headers in `rq->extra_hdrs` classify the request and seed the
 * preflight's Access-Control-Request-Headers. */
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

  /* Non-simple request: a preflight must clear it first. The preflight is never
   * credentialed and carries no author headers of its own — just the intended
   * method and the list of non-simple header names. */
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

  /* The actual cross-origin request: same base, but Origin'd headers and
   * cookies gated on the credentials mode. */
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

/* --- generic parallel sub-resource fetch pool -----------------------------
 * A small fan-out helper: run fn(ctx, i) for every i in [0,n) across a few
 * worker threads plus this one, joining before return. Each job must fetch over
 * its own socket(s) and write disjoint state (or lock its own) — the transport
 * engines (h1.1, and the now per-connection h2) are re-entrant, so the jobs run
 * truly concurrently. Worker-thread only and non-nested: the spawned threads
 * find the active job through g_par (thread_create takes no argument), and
 * par_fetch joins them before returning, so the pointer stays live throughout.
 */
#define PAR_FETCHERS 4
#define PAR_FETCH_STACK (128 * 1024)

struct par_job {
  volatile int next; /* next index to claim                       */
  volatile int lock; /* guards `next`                             */
  int n;
  void (*fn)(void *ctx, int i);
  void *ctx;
};
static struct par_job *g_par;

static int par_xchg(volatile int *p, int v)
{
  __asm__ volatile("xchgl %0, %1" : "+r"(v), "+m"(*p) : : "memory");
  return v;
}
static void par_spin_lock(volatile int *l)
{
  while (par_xchg(l, 1))
    msleep(0);
}
static void par_spin_unlock(volatile int *l)
{
  __asm__ volatile("" ::: "memory");
  *l = 0;
}

static int par_claim(struct par_job *j)
{
  int got = -1;
  par_spin_lock(&j->lock);
  if (j->next < j->n)
    got = j->next++;
  par_spin_unlock(&j->lock);
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
  struct par_job job = {0, 0, n, fn, ctx};
  g_par = &job;

  int want = n - 1;
  if (want > PAR_FETCHERS - 1)
    want = PAR_FETCHERS - 1;
  int tids[PAR_FETCHERS - 1];
  char *stks[PAR_FETCHERS - 1];
  void *tops[PAR_FETCHERS - 1];
  int nthr = 0;
  for (int t = 0; t < want; t++) {
    /* Prefer a kernel-guarded stack (unmapped guard page below, so an overflow
     * in the user-space TLS/h2 path faults cleanly instead of scribbling the
     * heap); fall back to a plain malloc'd stack if none are free. */
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

/* --- iframe fetching + embedding policy ------------------------------------ *
 * Whether a framed document may be displayed is the framed document's own call,
 * expressed by X-Frame-Options or the CSP `frame-ancestors` directive; the
 * embedder (the page, or an outer frame) is matched against it by origin. */

/* Two URLs share an origin when scheme (tls), host and port all match. */
static bool origin_eq(const char *a, const char *b)
{
  struct purl ua, ub;
  if (!parse_url(a, &ua) || !parse_url(b, &ub))
    return false;
  return ua.tls == ub.tls && ua.port == ub.port && streq_ci(ua.host, ub.host);
}

/* The value of CSP directive `name` (case-insensitive, at a ';'/start
 * boundary), or NULL. Returns a pointer into `csp` past the name + any spaces.
 */
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

/* Does one frame-ancestors source token [tok,tend) admit the embedder `parent`
 * (`framed` is the framed doc's own URL, used by 'self')? Handles 'self', '*',
 * a scheme://host[:port] origin, a "*.suffix" host, and a bare host. */
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

/* Decide whether `framed` may be embedded by `parent` given its X-Frame-Options
 * and Content-Security-Policy. frame-ancestors, when present, supersedes XFO. A
 * short human reason is written to `reason` when embedding is refused. */
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

/* Decode a data: URL into a fresh buffer (caller frees) + its media type, so a
 * `data:` iframe needs no network. base64 and percent-encoded bodies both work.
 */
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

/* One pending iframe's fetched result, filled by a pool thread and consumed on
 * the worker thread (only it may mutate the DOM via html_iframe_resolve). */
struct iframe_res {
  char url[URL_MAX];
  char parent[URL_MAX];
  int kind; /* FRAME_OK / FRAME_ERROR / FRAME_BLOCKED   */
  char reason[96];
  unsigned char *body; /* malloc'd (freed after resolve), or NULL  */
  int len;
  char ctype[160];
};

/* Fetch + run the embedding policy for one pending iframe (network + pure
 * header checks only, no DOM mutation), storing the verdict in res[i]. Safe to
 * run on several threads at once: each touches only its own res entry and own
 * sockets. */
static void iframe_job(void *vctx, int i)
{
  struct iframe_res *r = &((struct iframe_res *)vctx)[i];
  r->body = 0;
  r->len = 0;
  r->ctype[0] = '\0';
  r->reason[0] = '\0';

  if (starts_ci(r->url, "data:")) {
    int len = 0;
    unsigned char *body = decode_data_url(r->url, &len, r->ctype, sizeof r->ctype);
    if (body) {
      r->kind = FRAME_OK;
      r->body = body;
      r->len = len;
    } else {
      r->kind = FRAME_ERROR;
      int k = 0;
      sapp(r->reason, &k, sizeof r->reason, "bad data: URL");
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

/* Drain a batch of the page's pending iframes and fetch them in PARALLEL (data:
 * inline, else GET + X-Frame-Options / CSP frame-ancestors / CORP policy), then
 * hand each verdict back to the cache on this (worker) thread, which alone may
 * touch the DOM (html.c re-flattens to show the allowed ones). More than
 * IFRAME_BATCH pending, or nested frames, surface on the worker's next pass. */
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

/* Normalise a URL's origin (HSTS-upgrades http->https) into *u; returns 0 on a
 * bad URL. */
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

/* Shared state for a parallel subres batch: the URL list + its output arrays,
 * the per-URL claim bitmap and the lock guarding the origin-group claim. */
struct subres_batch {
  char *const *urls;
  int n;
  const char *accept;
  unsigned char **bodies;
  int *lens;
  bool *handled;
  volatile int lock;
};

/* Fetch one origin-group, starting from the URL `i` a pool thread just claimed:
 * gather every still-unclaimed same-origin HTTPS URL (up to H2_FETCH_MAX) and
 * multiplex them over one h2 connection (h1.1 per failed stream / no-h2
 * origin); a plain-http URL is fetched alone over h1.1. Only the group claim is
 * locked; the network I/O and the disjoint bodies[]/lens[] writes need none.
 * Several groups run at once — each opens its own connection and the engines
 * are re-entrant — so distinct origins fetch concurrently. */
static void subres_job(void *vctx, int i)
{
  struct subres_batch *c = vctx;
  struct purl u;
  int grp[H2_FETCH_MAX], ng = 0;

  par_spin_lock(&c->lock);
  if (c->handled[i]) {
    par_spin_unlock(&c->lock);
    return;
  } /* claimed by another group */
  if (!origin_of(c->urls[i], &u)) {
    c->handled[i] = true;
    par_spin_unlock(&c->lock);
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
  par_spin_unlock(&c->lock);

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
  /* Per-stream response-header buffers, so a fetched 200 can be cached (the h2
   * batch otherwise surfaces no headers). NULL on OOM = fetch without caching.
   */
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

  netconn *conn = nc_open(u.host, u.port, /*tls=*/1, /*offer_h2=*/1);
  if (conn && nc_alpn(conn) == ALPN_H2) {
    char auth[URL_HOST_MAX + 8];
    int an = 0;
    sapp(auth, &an, sizeof auth, u.host);
    if (u.port != 443) {
      sapp(auth, &an, sizeof auth, ":");
      uapp(auth, &an, sizeof auth, (unsigned)u.port);
    }
    char xh[1280];
    int xn = 0;
    char ck[1024];
    if (cookie_header(u.host, ck, sizeof ck) > 0) {
      sapp(xh, &xn, sizeof xh, "cookie: ");
      sapp(xh, &xn, sizeof xh, ck);
      sapp(xh, &xn, sizeof xh, "\r\n");
    }
    sapp(xh, &xn, sizeof xh, "accept: ");
    sapp(xh, &xn, sizeof xh, c->accept);
    sapp(xh, &xn, sizeof xh, "\r\n");
    {
      char ch[512];
      if (ch_request_headers(u.host, ch, sizeof ch) > 0)
        sapp(xh, &xn, sizeof xh, ch);
    }
    struct h2_opts o = {browser_ua(), xh, 0};
    h2_fetch_many(conn, 1, auth, &o, reqs, ng, 0, 0); /* accumulate */
    nc_close(conn);
    for (int g = 0; g < ng; g++) {
      /* Only a 200 body is usable; a non-200 (redirect/error) is dropped
       * and retried over h1.1, exactly as the single-request subres_get
       * path treats it (subres_get returns NULL for any non-200). */
      if (!reqs[g].failed && reqs[g].status == 200 && reqs[g].body) {
        c->bodies[grp[g]] = reqs[g].body;
        c->lens[grp[g]] = reqs[g].blen;
        if (reqs[g].hdr && reqs[g].hdrlen > 0) /* cache the fetched 200 */
          cache_store("GET", c->urls[grp[g]], reqs[g].status, now, reqs[g].hdr, "", reqs[g].body, reqs[g].blen);
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
  } else {
    if (conn)
      nc_close(conn);
    for (int g = 0; g < ng; g++) /* no h2: h1.1 each */
      c->bodies[grp[g]] = subres_get(c->urls[grp[g]], c->accept, &c->lens[grp[g]]);
  }
  free(paths);
  free(hdrs);
}

/* Fetch n absolute URLs into bodies[i] (malloc'd, caller frees, NULL on
 * failure)
 * + lens[i], in PARALLEL across a thread pool: same-origin HTTPS URLs are still
 * multiplexed over one h2 connection (h1.1 fallback per failed stream / no-h2
 * origin) and plain-http URLs go over h1.1, but distinct origin-groups now
 * fetch concurrently — each on its own connection, the engines being
 * re-entrant. */
void subres_get_batch(char *const *urls, int n, const char *accept, unsigned char **bodies, int *lens)
{
  if (n <= 0)
    return;
  for (int i = 0; i < n; i++) {
    bodies[i] = 0;
    lens[i] = 0;
  }

  /* `handled` tracks which URLs an origin-group already claimed. Sized to n (no
   * fixed cap): a page may reference any number of sub-resources. Each h2 group
   * still gathers at most H2_FETCH_MAX same-origin streams per connection and
   * leaves the rest for another claimed group, so this stays unbounded. */
  bool *handled = calloc((unsigned)n, sizeof *handled);
  if (!handled) { /* OOM: degrade to one fetch at a time */
    for (int i = 0; i < n; i++)
      bodies[i] = subres_get(urls[i], accept, &lens[i]);
    return;
  }

  /* data: URLs decode inline (no network, no origin) — the fetch groups below
   * key on origin_of, which rejects data:, so a data: sub-resource (e.g. a
   * background image) would otherwise never produce a body. Mark handled even
   * on failure. */
  for (int i = 0; i < n; i++) {
    if (starts_ci(urls[i], "data:")) {
      char ct[64];
      bodies[i] = decode_data_url(urls[i], &lens[i], ct, sizeof ct);
      handled[i] = true;
    }
  }

  /* Serve fresh cache hits up front (single-threaded here, before the pool):
   * marking a URL `handled` excludes it from every fetch group below. Runs on
   * this worker thread, so no fetch thread is racing `handled` yet. */
  long now = (long)rtc_epoch();
  for (int i = 0; i < n; i++) {
    unsigned char *cb = 0;
    int cl = 0;
    if (cache_lookup("GET", urls[i], now, &cb, &cl, 0, 0, 0, 0, 0, 0) == CACHE_FRESH) {
      bodies[i] = cb;
      lens[i] = cl;
      handled[i] = true;
      struct net_log e = {"GET", urls[i], 200, "", cl, 0, 0};
      devtools_net_add(&e);
    }
  }

  struct subres_batch ctx = {urls, n, accept, bodies, lens, handled, 0};
  par_fetch(n, subres_job, &ctx);
  free(handled);
}

/* Batch hook for html.c's external stylesheets. */
void css_batch(char *const *urls, int n, unsigned char **bodies, int *lens)
{
  subres_get_batch(urls, n, "text/css,*/*", bodies, lens);
}

/* --- parallel inline-image fetching ---------------------------------------
 * A small pool of threads fetches the page's images concurrently. Each thread
 * reads an image into a malloc'd buffer, then commits it into the shared
 * img_pool: a short lock guards only the bump of img_pool_len; the byte copy
 * and the per-image metadata go to disjoint regions, so they need no lock. */
#define IMG_FETCHERS 4
#define IMG_FETCH_STACK (128 * 1024)

static char g_img_page[URL_MAX]; /* page URL the fetchers resolve against */
static volatile int g_img_next;  /* next image index to claim             */
static volatile int g_img_lock;  /* guards g_img_next + the pool bump      */

static int img_xchg(volatile int *p, int v)
{
  __asm__ volatile("xchgl %0, %1" : "+r"(v), "+m"(*p) : : "memory");
  return v;
}
static void img_lock(void)
{
  while (img_xchg(&g_img_lock, 1))
    msleep(0);
}
static void img_unlock(void)
{
  __asm__ volatile("" ::: "memory");
  g_img_lock = 0;
}

/* Claim the next eager (non-lazy) image index, or -1 when none remain. */
static int img_claim_next(void)
{
  int got = -1;
  img_lock();
  while (g_img_next < B->pg->n_imgs) {
    int i = g_img_next++;
    if (!B->pg->imgs[i].lazy && !B->pg->imgs[i].ok) {
      got = i;
      break;
    } /* skip h2-done */
  }
  img_unlock();
  return got;
}

/* Commit a fetched image body into img_pool for image `idx`: an SVG is
 * rasterized to RGBA (its `currentColor` resolving to `current_color`), a PNG
 * stored verbatim. Sets im->ok last so the UI only draws a fully-committed
 * image. Does NOT free `body` (the caller owns it). Shared by the HTTP/1.1
 * fetcher and the h2 multiplexed prefetch. The short img_lock guards only the
 * img_pool_len bump; the copy is to a disjoint region. */
static void commit_image(int idx, const unsigned char *body, int blen, unsigned current_color)
{
  struct inl_img *im = &B->pg->imgs[idx];

  /* SVG / WebP -> decode to RGBA in the shared pool. The shared decoder
   * produces RGBA bytes; ww/hh are decoder-controlled, so size the copy in
   * 64-bit and reject absurd dimensions before touching the pool (no 32-bit
   * wrap past the bound). */
  if (image_sniff(body, blen) != IMG_FMT_PNG) {
    unsigned ww = 0, hh = 0;
    unsigned char *rgba = image_decode_rgba(body, blen, current_color, 256, &ww, &hh);
    if (rgba) {
      long long bytes = (long long)ww * hh * 4;
      int off = -1;
      img_lock();
      if (ww && hh && ww <= 16384 && hh <= 16384 && bytes <= (long long)(IMG_POOL - B->img_pool_len)) {
        off = B->img_pool_len;
        B->img_pool_len += (int)bytes;
      }
      img_unlock();
      if (off >= 0) {
        memcpy(B->img_pool + off, rgba, (unsigned)bytes);
        im->coff = off;
        im->clen = (int)bytes;
        im->iw = ww;
        im->ih = hh;
        im->is_argb = 1; /* raw RGBA */
        im->ok = 1;      /* set last: the UI draws once ok is seen */
        B->seq++;
      }
      free(rgba);
    }
    return;
  }

  if (blen >= 8 && body[0] == 0x89 && body[1] == 'P' && body[2] == 'N' &&
      body[3] == 'G') { /* PNG stored verbatim (lazy decode) */
    struct memrd mr = {body, 0, blen};
    unsigned iw = 0, ih = 0;
    bool okdim = (upng_dimensions(memrd_get, &mr, &iw, &ih) == 0 && iw && ih);

    int off = -1;
    img_lock();
    if (B->img_pool_len + blen <= IMG_POOL) {
      off = B->img_pool_len;
      B->img_pool_len += blen;
    }
    img_unlock();
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

/* Commit image bytes we already hold in memory (e.g. an inline <svg> element
 * serialized back to SVG text by html.c) into image slot `idx` — no network
 * fetch. Reuses the same committer as the HTTP path; `current_color` is what
 * the SVG's `currentColor` resolves to (the element's CSS color). */
void resource_commit_inline(int idx, const unsigned char *body, int len, unsigned current_color)
{
  commit_image(idx, body, len, current_color);
}

/* Fetch inline image `i` over its own HTTP/1.1 socket and commit it. Safe to
 * run on several threads at once (the h1.1 pool path). */
static void fetch_image(int i, const char *page_url)
{
  B->pg->imgs[i].ok = 0;
  char full[URL_MAX];
  resolve_url(page_url, B->pg->imgs[i].href, full, sizeof full);
  int blen = 0;
  unsigned char *body = subres_get(full, "image/png,image/webp,image/svg+xml,*/*", &blen);
  if (!body)
    return;
  commit_image(i, body, blen, 0x000000);
  free(body);
}

static bool favicon_decode(const unsigned char *r, int n); /* defined below */

/* on_done bridge for the h2 batch: map a batch request index back to its target
 * (idxs[]) and consume that resource as its stream lands. An index of FAV_IDX
 * is the page favicon (decoded into B->pg->favicon); anything else is an image
 * index. */
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
  if (c->reqs[b].hdr && c->reqs[b].hdrlen > 0)
    cache_store("GET", full, 200, c->now, c->reqs[b].hdr, "", body, blen);
}

/* Multiplex the page's same-origin HTTPS eager images over ONE h2 connection
 * (up to H2_FETCH_MAX). Committed images get ok=1, so the h1.1 pool below skips
 * them; cross-origin / http / h2-unavailable / overflow / per-stream-failed
 * images fall through to the pool. Single-threaded by necessity: the h2
 * engine's HPACK + frame state is process-global, so only one h2 connection may
 * be live. */
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
    if (im->lazy || im->ok)
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

  /* Fold the favicon onto the same connection when it's same-origin HTTPS, so
   * it rides the images' h2 stream batch instead of opening its own.
   * Cross-origin / http favicons (and a failed stream) are left for
   * fetch_favicon's h1.1 path, which runs only when favicon_ok is still 0. */
  if (nb < H2_FETCH_MAX && !B->pg->favicon_ok) {
    char fav[URL_MAX];
    resolve_url(page_url, B->pg->favicon_href[0] ? B->pg->favicon_href : "/favicon.ico", fav, sizeof fav);
    /* A fresh cached favicon skips the batch entirely. */
    unsigned char *fb = 0;
    int fl = 0;
    if (cache_lookup("GET", fav, now, &fb, &fl, 0, 0, 0, 0, 0, 0) == CACHE_FRESH) {
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

  netconn *conn = nc_open(pu.host, pu.port, /*tls=*/1, /*offer_h2=*/1);
  if (!conn) {
    free(paths);
    free(hdrs);
    return;
  }
  if (nc_alpn(conn) != ALPN_H2) {
    nc_close(conn);
    free(paths);
    free(hdrs);
    return;
  } /* pool handles */

  char auth[URL_HOST_MAX + 8];
  int an = 0;
  sapp(auth, &an, sizeof auth, pu.host);
  if (pu.port != 443) {
    sapp(auth, &an, sizeof auth, ":");
    uapp(auth, &an, sizeof auth, (unsigned)pu.port);
  }

  char xh[1280];
  int xn = 0;
  char ck[1024];
  if (cookie_header(pu.host, ck, sizeof ck) > 0) {
    sapp(xh, &xn, sizeof xh, "cookie: ");
    sapp(xh, &xn, sizeof xh, ck);
    sapp(xh, &xn, sizeof xh, "\r\n");
  }
  sapp(xh, &xn, sizeof xh, "accept: image/png,image/webp,image/svg+xml,*/*\r\n");
  {
    char ch[512];
    if (ch_request_headers(pu.host, ch, sizeof ch) > 0)
      sapp(xh, &xn, sizeof xh, ch);
  }
  struct h2_opts opts = {browser_ua(), xh, 0};

  /* Commit each image the moment its stream finishes (h2_fetch_many's on_done),
   * so they pop in progressively rather than all at once after the batch. Each
   * commit sets ok=1 + bumps B->seq, so the UI re-lays-out as they land; a
   * failed/empty stream leaves ok=0 and the h1.1 pool retries it. */
  struct h2img_ctx hc = {idxs, reqs, now};
  h2_fetch_many(conn, 1, auth, &opts, reqs, nb, h2img_commit, &hc);
  nc_close(conn);
  free(paths);
  free(hdrs);
}

/* The fetch loop each pool thread (and the worker itself) runs: claim images
 * until none remain. */
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

/* Fetch the page's *eager* inline images (loading != "lazy"): first multiplex
 * the same-origin HTTPS ones over a single h2 connection (h2_prefetch), then
 * fetch whatever is left over the HTTP/1.1 thread pool — spawn a few helper
 * threads, fetch alongside them on this (worker) thread, then join.
 * loading="lazy" images wait for fetch_lazy_visible() when scrolled in. */
void fetch_inline_images(const char *page_url)
{
  B->img_pool_len = 0;
  {
    int k = 0;
    g_img_page[0] = '\0';
    sapp(g_img_page, &k, sizeof g_img_page, page_url);
  }
  g_img_next = 0;

  /* Serve fresh cached eager images up front (single-threaded, before the
   * pool): commit_image sets im->ok, so both h2_prefetch and the h1.1 pool skip
   * them. */
  {
    long now = (long)rtc_epoch();
    for (int i = 0; i < B->pg->n_imgs; i++) {
      struct inl_img *im = &B->pg->imgs[i];
      if (im->lazy || im->ok)
        continue;
      char full[URL_MAX];
      resolve_url(page_url, im->href, full, sizeof full);
      unsigned char *cb = 0;
      int cl = 0;
      if (cache_lookup("GET", full, now, &cb, &cl, 0, 0, 0, 0, 0, 0) == CACHE_FRESH) {
        commit_image(i, cb, cl, 0x000000);
        free(cb);
        struct net_log e = {"GET", full, 200, "image/*", cl, 0, 0};
        devtools_net_add(&e);
      }
    }
  }

  /* First multiplex the same-origin HTTPS images over one h2 connection; the
   * rest (cross-origin, http, or h2-unavailable) go through the h1.1 pool. */
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

/* Worker step (called when no page fetch is pending): load at most one lazy
 * <img> the UI has flagged as scrolled into view, against the current page URL.
 * Returns 1 if it fetched one (so the caller can loop), 0 if there was nothing
 * to do — which keeps the worker mostly idle until the user actually scrolls.
 */
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

/* ------------------------------------------------------------ favicon decode
 */
/* A favicon is fetched into B->pg->raw and decoded down to a fixed 16x16 ARGB
 * bitmap (B->pg->favicon) for the tab strip. PNG favicons go through upng; .ico
 * files are parsed here (best directory entry -> embedded PNG or a raw DIB).
 * Anything we can't decode (GIF/BMP/SVG/empty) just leaves favicon_ok 0 and the
 * tab shows the default globe. */

/* Nearest-neighbour scale of a decoded RGBA image into the 16x16 favicon. The
 * row callback fills every destination row whose source maps to the row `y`
 * upng just produced, so it works whatever the source dimensions are. */
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
  if (upng_dimensions(memrd_get, &mr, &iw, &ih) != 0 || !iw || !ih)
    return false;
  struct favscale v = {iw, ih, 0, B->pg->favicon};
  struct memrd mr2 = {buf, 0, len};
  if (upng_decode(memrd_get, &mr2, fav_row, &v, 0, 0) != 0)
    return false;
  return v.next_r > 0;
}

/* Little-endian readers for the .ico container (always little-endian). */
static unsigned le16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }
static unsigned le32(const unsigned char *p)
{
  return p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

/* Decode one ICO image stored as a Windows DIB (BITMAPINFOHEADER + pixels, no
 * BMP file header). Bottom-up rows; biHeight counts the XOR colour rows plus an
 * equal AND (1bpp transparency) mask, so the real height is half. Supports
 * 1/4/8/24/32 bpp, BI_RGB. Sampled directly at 16x16 to avoid a full-size
 * intermediate buffer. */
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

  /* Trust a 32bpp alpha channel only if at least one pixel is non-zero;
   * many icons leave it all-zero and rely on the AND mask instead. */
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

/* Pick the best image in an .ico directory: the smallest entry that is still
 * >= 16px (an exact 16 wins), else the largest available. Fills poff/plen with
 * its byte range within the file. */
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

/* Decode `buf` (a fetched favicon) into B->pg->favicon; returns 1 on success.
 */
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

/* Fetch and decode the page favicon into B->pg->favicon, from the <link
 * rel=icon> href the parser found, else /favicon.ico. subres_get follows CDN
 * redirects and uses its own socket/buffer, so this no longer clobbers
 * B->pg->raw. Leaves favicon_ok 0 on any failure (the tab falls back to the
 * globe). */
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
