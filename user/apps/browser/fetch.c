/* fetch.c: page transaction (HTTP/1.1, h2, h3), downloads, redirects, file:/data: schemes; runs on the worker thread.
 */
#include "brotli.h" /* RFC 7932 `br` content-coding (whole-buffer decode)   */
#include "browser.h"
#include "features/cache/cache.h"             /* the HTTP response cache (RFC 7234, honours headers)  */
#include "features/clienthints/clienthints.h" /* Accept-CH: per-origin Client Hints opt-in           */
#include "features/jar/jar.h"                 /* scheme/path-aware cookie header (Secure/Path)        */
#include "fetchint.h"                         /* shared transport interface + BR_* request headers   */
#include "h3.h"                               /* HTTP/3 over QUIC (libcurl engine)                    */
#include "headers.h"                          /* libcurl: HTTP response header parsing               */
#include "hints.h"                            /* preconnect / preload resource hints                  */

/* UA platform token from uname(2): x86, x86_64, aarch64 or arm; cached. */
static const char *ua_arch(void)
{
  static char tok[12];
  if (!tok[0]) {
    struct utsname u;
    const char *m = (uname(&u) == 0) ? u.machine : "";
    if (starts_ci(m, "x86_64") || starts_ci(m, "amd64"))
      strlcpy(tok, "x86_64", sizeof tok);
    else if (starts_ci(m, "aarch64") || starts_ci(m, "arm64"))
      strlcpy(tok, "aarch64", sizeof tok);
    else if (starts_ci(m, "arm"))
      strlcpy(tok, "arm", sizeof tok);
    else
      strlcpy(tok, "x86", sizeof tok);
  }
  return tok;
}

/* navigator.appVersion: the UA minus "Mozilla/". */
const char *browser_ua_version(void)
{
  static char buf[160];
  if (!buf[0])
    snprintf(buf, sizeof buf,
             "5.0 (OwOS; %s) AppleWebKit/537.36 (KHTML, like Gecko) "
             "Chrome/131.0.0.0 OwOSBrowser/%s Safari/537.36",
             ua_arch(), OWOS_VERSION);
  return buf;
}

/* The full User-Agent, shared by the HTTP transport and the JS navigator. */
const char *browser_ua(void)
{
  static char buf[168];
  if (!buf[0])
    snprintf(buf, sizeof buf, "Mozilla/%s", browser_ua_version());
  return buf;
}

/* Content-encoding of a response body. */
enum { ENC_NONE = 0, ENC_ZLIB = 1, ENC_BR = 2 };

/* Ceiling on a decompressed body (decompression-bomb guard); RAW_MAX bounds the compressed body. */
#define DECOMP_MAX (RAW_MAX * 2) /* 64 MiB */

/* Decompress an in-memory body per ENC_BR / ENC_ZLIB; returns a malloc'd buffer (caller frees) or NULL. */
struct memrd {
  const unsigned char *p;
  int len, pos;
};
static int memrd_fn(void *c)
{
  struct memrd *m = c;
  return m->pos < m->len ? m->p[m->pos++] : -1;
}
struct memwr {
  unsigned char *buf;
  int cap, len;
  bool oom;
};
static int memwr_fn(void *c, const unsigned char *p, unsigned n)
{
  struct memwr *w = c;
  /* Bomb guard: refuse writes past the cap; unsigned math so sizes can't wrap. */
  if (n > (unsigned)DECOMP_MAX || (unsigned)w->len + n > (unsigned)DECOMP_MAX) {
    w->oom = true;
    return -1;
  }
  if (w->len + (int)n > w->cap) {
    unsigned want = (unsigned)w->len + n;
    unsigned nc = want + want / 2 + 4096; /* ~1.5x with headroom */
    if (nc > (unsigned)DECOMP_MAX)
      nc = (unsigned)DECOMP_MAX;
    unsigned char *nb = realloc(w->buf, nc);
    if (!nb) {
      w->oom = true;
      return -1;
    }
    w->buf = nb;
    w->cap = (int)nc;
  }
  for (unsigned i = 0; i < n; i++)
    w->buf[w->len++] = p[i];
  return 0;
}
static unsigned char *body_decompress(const unsigned char *in, int in_len, int enc, int *out_len)
{
  if (enc == ENC_BR) {
    /* brotli decodes whole-buffer in the lib; only an over-cap result can be rejected here. */
    unsigned char *o = brotli_decompress(in, in_len, out_len);
    if (o && *out_len > DECOMP_MAX) {
      free(o);
      return 0;
    }
    return o;
  }
  struct memrd r = {in, in_len, 0};
  struct memwr w = {0, 0, 0, 0};
  if (inflate_http(memrd_fn, &r, memwr_fn, &w) < 0 || w.oom) {
    free(w.buf);
    return 0;
  }
  *out_len = w.len;
  return w.buf ? w.buf : malloc(1);
}

/* Decode B->pg->raw in place per its content-encoding; a failed decode empties it. */
static void raw_decode(int enc)
{
  if (!enc || B->pg->raw_len <= 0)
    return;
  int ol = 0;
  unsigned char *o = body_decompress((unsigned char *)B->pg->raw, B->pg->raw_len, enc, &ol);
  if (!o) {
    B->pg->raw_len = 0;
    return;
  } /* failed decode: never show the compressed bytes */
  /* Adopt the decoded buffer directly (same allocator family); browser_buf_lock guards the swap. */
  browser_buf_lock();
  free(B->pg->raw);
  B->pg->raw = (char *)o;
  B->pg->raw_len = ol;
  B->pg->raw_cap = ol;
  browser_buf_unlock();
}

static int rgetc(void)
{
  if (B->abort_load)
    return -2; /* Stop / tab-close: bail the read */
  if (B->rpos >= B->rlen) {
    if (B->reof)
      return -1;
    int n = nc_read(B->sock, B->rbuf, (int)sizeof B->rbuf);
    if (n < 0)
      return -2;
    if (n == 0) {
      B->reof = 1;
      return -1;
    }
    B->rpos = 0;
    B->rlen = n;
  }
  return B->rbuf[B->rpos++];
}

/* Show a one-line message as the page body; clears cur_url so the address bar keeps the typed text. */
void show_error(const char *m)
{
  B->pg->mode = MODE_TEXT;
  B->pg->scroll = 0;
  B->pg->cur_url[0] = '\0';
  doc_message(m);
  B->seq++;
}

static const char *fetch_error_message(int status)
{
  switch (status) {
  case -1:
    return "No network device / out of sockets";
  case -2:
    return "DNS lookup failed";
  case -3:
    return "TCP connection failed";
  case -4:
    return "Failed to send request";
  case -6:
    return "TLS handshake failed";
  default:
    return "No response from server";
  }
}

static bool mime_is(const char *ctype, const char *want)
{
  int n = 0;
  while (ctype[n] && ctype[n] != ';' && ctype[n] != ' ' && ctype[n] != '\t')
    n++;
  int w = 0;
  while (want[w])
    w++;
  if (n != w)
    return false;
  for (int i = 0; i < n; i++)
    if (lower(ctype[i]) != lower(want[i]))
      return false;
  return true;
}

static bool is_html_type(const char *ctype)
{
  return mime_is(ctype, "text/html") || mime_is(ctype, "application/xhtml+xml");
}

/* --------------------------------------------------- body byte source/sink */

/* Next content byte of the body with transfer framing removed, or -1 at end of body. */
struct bodysrc {
  bool chunked, have_len, done, need_crlf;
  unsigned remaining; /* bytes left in this chunk / the content-length   */
  bool clean;         /* ended where the framing says, nothing left to read */
};

static int body_src(void *ctx)
{
  struct bodysrc *b = ctx;
  if (b->done)
    return -1;
  if (b->chunked) {
    while (b->remaining == 0) {
      if (b->need_crlf) { /* CRLF trailing the prev chunk    */
        int c = rgetc();
        if (c == '\r')
          rgetc(); /* the \n                          */
        else if (c < 0) {
          b->done = true;
          return -1;
        }
        b->need_crlf = false;
      }
      unsigned sz = 0;
      int c;
      bool any = false;
      while ((c = rgetc()) >= 0 && c != '\r' && c != ';') {
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
        any = true;
      }
      while (c >= 0 && c != '\n')
        c = rgetc();
      if (any && sz == 0) { /* last chunk: the trailer section ends at a blank line */
        for (;;) {
          c = rgetc();
          bool empty = true;
          while (c >= 0 && c != '\n') {
            empty &= c == '\r';
            c = rgetc();
          }
          if (c < 0 || empty) {
            b->clean = c >= 0;
            break;
          }
        }
      }
      if (!any || sz == 0) {
        b->done = true;
        return -1;
      }
      b->remaining = sz;
      b->need_crlf = true;
    }
    int c = rgetc();
    if (c < 0) {
      b->done = true;
      return -1;
    }
    b->remaining--;
    return c;
  }
  if (b->have_len) {
    if (b->remaining == 0) {
      b->done = b->clean = true;
      return -1;
    }
    int c = rgetc();
    if (c < 0) {
      b->done = true;
      return -1;
    }
    b->remaining--;
    return c;
  }
  int c = rgetc();
  if (c < 0) {
    b->done = true;
    return -1;
  }
  return c;
}

/* ------------------------------------------------------------- downloads */

#define DL_DIR "/home/root/Downloads"

/* A response is a download if flagged as an attachment, or by file extension, or by a non-renderable Content-Type. */
static bool url_is_download_ext(const char *url);
static bool is_download(const char *url)
{
  if (B->cdisp_attach)
    return true;
  if (url_is_download_ext(url))
    return true;
  if (B->pg->ctype[0] == '\0')
    return false;
  if (is_html_type(B->pg->ctype))
    return false;
  if (starts_ci(B->pg->ctype, "text/"))
    return false;
  if (starts_ci(B->pg->ctype, "image/"))
    return false;
  return true;
}

/* Copy the last path component of `in` into `out`, dropping directory and "?query". */
static void basename_of(const char *in, char *out, int cap)
{
  const char *last = in;
  for (const char *p = in; *p; p++)
    if (*p == '/' || *p == '\\')
      last = p + 1;
  int k = 0;
  for (; last[k] && last[k] != '?' && k < cap - 1; k++)
    out[k] = last[k];
  out[k] = '\0';
}

/* True if the last path segment has an extension never rendered inline (images excluded). */
static bool url_is_download_ext(const char *url)
{
  struct purl u;
  if (!parse_url(url, &u))
    return false;
  char base[128];
  basename_of(u.path, base, sizeof base);
  const char *ext = 0;
  for (const char *q = base; *q; q++)
    if (*q == '.')
      ext = q + 1;
  if (!ext || !*ext)
    return false;
  static const char *const bin_ext[] = {
      "mp3", "wav", "flac", "ogg", "oga",  "opus", "m4a",  "aac", "wma",  "mid",   "mp4",  "mkv", "webm", "avi",
      "mov", "wmv", "flv",  "m4v", "mpg",  "mpeg", "zip",  "gz",  "tgz",  "bz2",   "xz",   "7z",  "rar",  "tar",
      "iso", "img", "pdf",  "doc", "docx", "xls",  "xlsx", "ppt", "pptx", "odt",   "epub", "exe", "dll",  "bin",
      "dmg", "deb", "rpm",  "apk", "msi",  "so",   "ttf",  "otf", "woff", "woff2", 0};
  for (int i = 0; bin_ext[i]; i++)
    if (streq_ci(ext, bin_ext[i]))
      return true;
  return false;
}

/* Choose a download filename: Content-Disposition, else the URL's last segment, else a fallback. */
static void derive_filename(const char *url, char *out, int cap)
{
  if (B->dl_name[0]) {
    basename_of(B->dl_name, out, cap);
    if (out[0])
      return;
  }
  struct purl u;
  if (parse_url(url, &u)) {
    basename_of(u.path, out, cap);
    if (out[0])
      return;
  }
  int k = 0;
  sapp(out, &k, cap, "download.bin");
}

/* owofs caps path components at 63 chars; longer leaves make fs_stream_begin reject the path. */
#define DL_LEAF_MAX 63

/* Build DL_DIR/<base> (attempt 0) or DL_DIR/<stem>-<n><ext), truncating the stem to the component limit. */
static void build_path(const char *base, int n, char *out, int cap)
{
  /* Split base into stem and ".ext" at the last dot (a leading dot is not an extension). */
  int dot = -1;
  for (int i = 0; base[i]; i++)
    if (base[i] == '.')
      dot = i;
  int stem_len = (dot > 0) ? dot : (int)strlen(base);
  const char *ext = (dot > 0) ? base + dot : "";
  int ext_len = (int)strlen(ext);

  /* Collision suffix "-N" (empty for the first attempt). */
  char sfx[8];
  int sl = 0;
  if (n > 0) {
    sfx[sl++] = '-';
    uapp(sfx, &sl, (int)sizeof sfx, (unsigned)n);
  }
  sfx[sl] = '\0';

  /* Cap a pathologically long extension, leaving room for the suffix and one stem char. */
  if (ext_len > DL_LEAF_MAX - sl - 1) {
    ext_len = DL_LEAF_MAX - sl - 1;
    if (ext_len < 0)
      ext_len = 0;
  }
  /* Truncate the stem so stem + suffix + ext fits. */
  int budget = DL_LEAF_MAX - sl - ext_len;
  if (budget < 1)
    budget = 1;
  if (stem_len > budget)
    stem_len = budget;

  int p = 0;
  sapp(out, &p, cap, DL_DIR "/");
  for (int i = 0; i < stem_len && p < cap - 1; i++)
    out[p++] = base[i];
  out[p] = '\0';
  sapp(out, &p, cap, sfx);
  for (int i = 0; i < ext_len && p < cap - 1; i++)
    out[p++] = ext[i];
  out[p] = '\0';
}

/* Pick a non-clobbering DL_DIR/<name> path; ensures DL_DIR exists (older disk images lack it). */
static void pick_dl_path(const char *url, char *path, int cap)
{
  mkdir(DL_DIR); /* no-op if it already exists */
  char base[128];
  derive_filename(url, base, sizeof base);
  char probe[1];
  for (int n = 0; n < 100; n++) {
    build_path(base, n, path, cap);
    if (readfile(path, probe, 0) < 0)
      break; /* first free name           */
  }
}

/* Report a finished download: show the saved path and size, or a failure notice. */
static void download_finished(const char *url, const char *path, int wrote, bool fail)
{
  B->pg->mode = MODE_TEXT;
  {
    int ci = 0;
    sapp(B->pg->cur_url, &ci, sizeof B->pg->cur_url, url);
  }
  B->pg->scroll = 0;

  char msg[256];
  int m = 0;
  if (!fail && wrote >= 0) {
    sapp(msg, &m, sizeof msg, "Saved to ");
    sapp(msg, &m, sizeof msg, path);
    sapp(msg, &m, sizeof msg, "  (");
    uapp(msg, &m, sizeof msg, (unsigned)wrote);
    sapp(msg, &m, sizeof msg, " bytes)");
  } else {
    sapp(msg, &m, sizeof msg, "Download failed (disk full or cannot write to " DL_DIR ")");
  }
  doc_message(msg);
  B->dl_saved = 1;
  B->seq++;
}

/* ----------------------------------------------------------- transaction */

/* External stylesheet URLs from the Link: header (B->ext_css); reset per response. */
static void ext_css_clear(void)
{
  struct css_link *c = B->ext_css;
  /* The head must be NULL or a heap node; a lower value is a stray write, so drop the list. */
  if ((unsigned)c && (unsigned)c < 0x40000000u) {
    B->ext_css = 0;
    return;
  }
  while (c) {
    struct css_link *n = c->next;
    free(c->url);
    free(c);
    c = n;
  }
  B->ext_css = 0;
}

static void ext_css_add(const char *url)
{
  struct css_link *c = malloc(sizeof *c);
  if (!c)
    return;
  int len = strlen(url);
  c->url = malloc((unsigned)len + 1);
  if (!c->url) {
    free(c);
    return;
  }
  for (int i = 0; i <= len; i++)
    c->url[i] = url[i];
  c->next = B->ext_css;
  B->ext_css = c;
}

/* Parse a Link: header value (RFC 8288), resolved against `base`: rel=stylesheet goes to B->ext_css (when `sheets`),
 * rel=preconnect and rel=preload to the resource hints. */
static void parse_link_header(const char *v, const char *base, bool sheets)
{
  while (*v) {
    while (*v == ' ' || *v == ',' || *v == '\t')
      v++;
    if (!*v)
      break;

    const char *entry = v, *eend = v; /* this entry: [entry, eend) */
    while (*eend && *eend != ',')
      eend++;

    char url[URL_MAX];
    url[0] = '\0';
    const char *p = entry;
    if (*p == '<') {
      p++;
      int k = 0;
      while (p < eend && *p != '>' && k < (int)sizeof url - 1)
        url[k++] = *p++;
      url[k] = '\0';
      if (p < eend && *p == '>')
        p++;
    }

    bool is_sheet = false, is_preload = false, is_preconnect = false; /* scan params (after the URL) */
    char as[16] = "";
    for (const char *q = p; q < eend; q++) {
      bool rel = starts_ci(q, "rel");
      if (!(q == p || q[-1] == ';' || q[-1] == ' ') || !(rel || starts_ci(q, "as")))
        continue;
      const char *r = q + (rel ? 3 : 2);
      while (r < eend && *r == ' ')
        r++;
      if (r >= eend || *r != '=')
        continue;
      r++;
      while (r < eend && (*r == ' ' || *r == '"'))
        r++;
      const char *rv = r;
      while (rv < eend && *rv != ';')
        rv++;
      if (rel) {
        is_sheet = hint_rel_has(r, rv, "stylesheet");
        is_preload = hint_rel_has(r, rv, "preload");
        is_preconnect = hint_rel_has(r, rv, "preconnect");
      } else {
        int k = 0;
        for (; r < rv && *r != '"' && *r != ' ' && k < (int)sizeof as - 1; r++)
          as[k++] = *r;
        as[k] = '\0';
      }
    }

    if (url[0] && (is_sheet || is_preload || is_preconnect)) {
      char abs[URL_MAX];
      resolve_url(base, url, abs, sizeof abs);
      if (is_sheet && sheets)
        ext_css_add(abs);
      if (is_preconnect)
        hint_preconnect(abs);
      if (is_preload)
        hint_preload(abs, as, base);
    }
    v = (*eend == ',') ? eend + 1 : eend;
  }
}

/* Status code of a raw header block ("HTTP/x.y NNN"), 0 if unparseable. */
static int hdr_status(const char *hdr)
{
  const char *p = hdr;
  while (*p && *p != ' ')
    p++; /* skip the "HTTP/x.y" token */
  while (*p == ' ')
    p++;
  int s = 0;
  bool any = false;
  while (*p >= '0' && *p <= '9') {
    s = s * 10 + (*p++ - '0');
    any = true;
  }
  return any ? s : 0;
}

/* 103 Early Hints: preconnect/preload start at once; Link values are stashed because the final parse_response clears
 * ext_css, and early_apply merges their stylesheets. */
static char g_early[2048];
static int g_early_len;

static void early_reset(void)
{
  g_early_len = 0;
  g_early[0] = '\0';
}

static void early_collect(const char *hdr, const char *base)
{
  struct http_headers H;
  http_headers_parse(&H, hdr);
  int n = http_headers_count(&H, "link");
  for (int i = 0; i < n; i++) {
    const char *v = http_headers_at(&H, "link", i);
    parse_link_header(v, base, false);
    for (; *v && g_early_len < (int)sizeof g_early - 2; v++)
      g_early[g_early_len++] = *v;
    if (g_early_len < (int)sizeof g_early - 1)
      g_early[g_early_len++] = '\n';
  }
  g_early[g_early_len] = '\0';
  http_headers_free(&H);
}

static void early_apply(const char *base)
{
  char line[1024];
  for (const char *p = g_early; *p;) {
    int k = 0;
    while (*p && *p != '\n' && k < (int)sizeof line - 1)
      line[k++] = *p++;
    line[k] = '\0';
    if (*p == '\n')
      p++;
    if (k)
      parse_link_header(line, base, true);
  }
}

/* All values of a repeated header joined with ", " into a heap string; NULL if absent or OOM. */
static char *join_headers(const struct http_headers *H, const char *name)
{
  int cnt = http_headers_count(H, name), len = 0;
  if (cnt == 0)
    return 0;
  for (int i = 0; i < cnt; i++)
    len += (int)strlen(http_headers_at(H, name, i)) + 2;
  char *s = malloc((unsigned)len + 1);
  if (!s)
    return 0;
  int n = 0;
  for (int i = 0; i < cnt; i++) {
    if (i)
      sapp(s, &n, len + 1, ", ");
    sapp(s, &n, len + 1, http_headers_at(H, name, i));
  }
  return s;
}

/* Parse an HTTP/1.1 or h2-synthesized header block into browser state; returns the status code. */
/* Last document response's raw headers and start tick, for the devtools Network log. */
static char g_doc_hdr[RESP_HDR_MAX];
static bool g_doc_from_cache; /* the last document came from the HTTP cache, with no request sent */
static unsigned g_doc_t0;
/* Lowercased conditional-request header lines for a stale cached document, shared by h1.1/h2/h3. */
static char g_doc_cond[512];

static int parse_response(const char *hdr, const char *host, const char *full_url, char *loc, int loc_cap,
                          bool *chunked, bool *have_len, unsigned *content_len, int *enc_gz)
{
  {
    int k = 0;
    for (; hdr[k] && k < (int)sizeof g_doc_hdr - 1; k++)
      g_doc_hdr[k] = hdr[k];
    g_doc_hdr[k] = '\0';
  }
  *chunked = false;
  *have_len = false;
  *enc_gz = 0;
  *content_len = 0;
  B->nosniff = false;
  ext_css_clear(); /* this response's Link: stylesheets replace the last */

  /* Parse the raw block once into a k/v object, then query it. */
  struct http_headers H;
  http_headers_parse(&H, hdr);
  int status = H.status;

  const char *te = http_headers_get(&H, "transfer-encoding");
  if (te && http_header_has_token(te, "chunked"))
    *chunked = true;

  const char *ce = http_headers_get(&H, "content-encoding");
  if (ce && http_header_has_token(ce, "br"))
    *enc_gz = ENC_BR;
  else if (ce && (http_header_has_token(ce, "gzip") || http_header_has_token(ce, "deflate")))
    *enc_gz = ENC_ZLIB;

  const char *cl = http_headers_get(&H, "content-length");
  if (cl) {
    bool ok;
    unsigned x = http_parse_uint(cl, &ok);
    if (ok) {
      *content_len = x;
      *have_len = true;
    }
  }

  const char *loc_v = http_headers_get(&H, "location");
  if (loc_v)
    resolve_url(full_url, loc_v, loc, loc_cap);

  const char *ct = http_headers_get(&H, "content-type");
  if (ct) {
    int k = 0;
    for (const char *v = ct; *v && *v != ';' && k < (int)sizeof B->pg->ctype - 1; v++)
      B->pg->ctype[k++] = lower(*v);
    B->pg->ctype[k] = '\0';
  }
  html_set_charset(ct); /* charset param drives html.c transcoding (NULL = reset) */

  const char *cd = http_headers_get(&H, "content-disposition");
  if (cd) {
    if (http_header_has_token(cd, "attachment"))
      B->cdisp_attach = 1;
    for (const char *q = cd; *q; q++) {
      if (!starts_ci(q, "filename"))
        continue;
      const char *f = q + 8;
      if (*f == '*')
        continue; /* skip the filename*= form */
      while (*f == ' ')
        f++;
      if (*f != '=')
        continue;
      f++;
      while (*f == ' ')
        f++;
      bool quoted = (*f == '"');
      if (quoted)
        f++;
      int k = 0;
      while (*f && k < (int)sizeof B->dl_name - 1) {
        if (quoted ? (*f == '"') : (*f == ';' || *f == ' '))
          break;
        B->dl_name[k++] = *f++;
      }
      B->dl_name[k] = '\0';
      break;
    }
  }

  /* Set-Cookie may repeat; store every one. */
  int ncookie = http_headers_count(&H, "set-cookie");
  for (int i = 0; i < ncookie; i++)
    cookie_store(host, http_headers_at(&H, "set-cookie", i));

  if (http_headers_get(&H, "strict-transport-security"))
    hsts_add(host);

  /* Accept-CH: record the hints this origin wants (https only). */
  const char *ach = http_headers_get(&H, "accept-ch");
  if (ach && starts_ci(full_url, "https://"))
    ch_accept(host, ach);

  /* Alt-Svc advertising h3: remember the origin so the next request uses QUIC. */
  const char *as = http_headers_get(&H, "alt-svc");
  if (as)
    for (const char *p = as; *p; p++)
      if ((p[0] == 'h' || p[0] == 'H') && p[1] == '3' && (p[2] == '=' || p[2] == '-')) {
        h3_remember(host);
        break;
      }

  /* nosniff: honour the declared Content-Type and never guess (see render_raw). */
  const char *xcto = http_headers_get(&H, "x-content-type-options");
  if (xcto && http_header_has_token(xcto, "nosniff"))
    B->nosniff = true;

  /* Content-Security-Policy: one policy per header, all must pass; Report-Only is not enforced. */
  if (B->pg->csp) {
    csp_free(B->pg->csp);
    B->pg->csp = 0;
  }
  int ncsp = http_headers_count(&H, "content-security-policy");
  if (ncsp > 0) {
    B->pg->csp = csp_new();
    for (int i = 0; i < ncsp; i++)
      csp_add(B->pg->csp, http_headers_at(&H, "content-security-policy", i));
  }

  /* Link: rel=stylesheet sheets, and rel=preconnect/preload hints (checked against the CSP just set). */
  for (int i = 0, nlink = http_headers_count(&H, "link"); i < nlink; i++)
    parse_link_header(http_headers_at(&H, "link", i), full_url, true);

  /* Permissions-Policy (and Feature-Policy): blocked bitmaps OR together; the final redirect hop wins. */
  B->pg->perm_policy = 0;
  for (int i = 0, np = http_headers_count(&H, "permissions-policy"); i < np; i++)
    B->pg->perm_policy |= perm_policy_parse(http_headers_at(&H, "permissions-policy", i), host);
  for (int i = 0, nf = http_headers_count(&H, "feature-policy"); i < nf; i++)
    B->pg->perm_policy |= perm_policy_parse(http_headers_at(&H, "feature-policy", i), host);
  free(B->pg->perm_policy_hdr);
  B->pg->perm_policy_hdr = join_headers(&H, "permissions-policy");

  http_headers_free(&H);
  jar_flush(); /* persist cookies/HSTS once, if this response changed them */
  return status;
}

/* Route the decoded body in B->pg->raw: downloads go to ~/Downloads (raw cleared), the rest stays for rendering. */
static int route_body(const char *full_url, int status, int allow_dl)
{
  if (allow_dl && status >= 200 && status < 300 && is_download(full_url)) {
    char path[256];
    pick_dl_path(full_url, path, sizeof path);
    int wrote = -1;
    bool fail = false;
    if (stream_begin(path) == 0) {
      if (B->pg->raw_len > 0 && stream_write((const unsigned char *)B->pg->raw, B->pg->raw_len) < 0)
        fail = true;
      wrote = stream_end();
    }
    download_finished(full_url, path, wrote, fail);
    B->pg->raw_len = 0;
  }
  return status;
}

/* ---- HTTP/2 transaction (used when ALPN negotiates h2 over TLS) --------- */

struct h2tx {
  const char *full_url, *host;
  char *loc;
  int loc_cap;
  int status, enc;
};

/* Headers-ready hook: parse the response before any DATA. */
static int h2_headers_cb(void *ctx, const char *block, int len)
{
  (void)len;
  struct h2tx *h = ctx;
  if (hdr_status(block) < 200) {
    early_collect(block, h->full_url);
    return 0;
  } /* 103 etc. */
  bool chunked, have_len;
  unsigned clen;
  h->status = parse_response(block, h->host, h->full_url, h->loc, h->loc_cap, &chunked, &have_len, &clen, &h->enc);
  return 0;
}

/* Body sink: append the raw DATA payload to B->pg->raw. */
static int h2_body_cb(void *ctx, const unsigned char *p, int n)
{
  (void)ctx;
  if (B->abort_load)
    return 1; /* Stop: tell h2_run to bail */
  if (!browser_ensure_raw(B->pg->raw_len + n))
    return 1; /* out of memory: stop */
  for (int i = 0; i < n; i++)
    B->pg->raw[B->pg->raw_len++] = (char)p[i];
  return 0;
}

/* Append the lowercased headers a document GET sends beyond the UA: cookies, accept*, client hints. */
static void doc_req_hdrs(char *xh, int *xn, int cap, const struct purl *u)
{
  char ck[1024];
  if (cookie_header_tls(u->host, u->path, u->tls, ck, sizeof ck) > 0) {
    sapp(xh, xn, cap, "cookie: ");
    sapp(xh, xn, cap, ck);
    sapp(xh, xn, cap, "\r\n");
  }
  sapp(xh, xn, cap,
       "accept: " BR_ACCEPT "\r\naccept-language: " BR_ACCLNG "\r\n"
       "accept-encoding: gzip, deflate, br\r\n");
  char ch[512];
  if (ch_headers_for(u, false, ch, sizeof ch) > 0)
    sapp(xh, xn, cap, ch);
}

/* Run the request over HTTP/2 (ALPN-confirmed); fills B->pg->raw with the raw body. */
static int h2_txn(h2_session *hs, const struct purl *u, const char *full_url, char *loc, int loc_cap, int allow_dl,
                  const char *method, const unsigned char *body, int blen, const char *bctype, bool *stale)
{
  char auth[URL_HOST_MAX + 8], hb[URL_HOST_MAX + 3];
  int an = 0; /* :authority = host[:port] */
  sapp(auth, &an, sizeof auth, url_bracket_host(u->host, hb, sizeof hb));
  if (u->port != 443) {
    sapp(auth, &an, sizeof auth, ":");
    uapp(auth, &an, sizeof auth, (unsigned)u->port);
  }

  /* cookies + accept + accept-encoding; no Connection/Transfer-Encoding in HTTP/2, UA rides in h2_opts. */
  char xh[1408];
  int xn = 0;
  doc_req_hdrs(xh, &xn, sizeof xh, u);
  if (body && blen > 0) {
    sapp(xh, &xn, sizeof xh, "content-type: ");
    sapp(xh, &xn, sizeof xh, bctype ? bctype : "application/x-www-form-urlencoded");
    sapp(xh, &xn, sizeof xh, "\r\n");
  }
  if (g_doc_cond[0])
    sapp(xh, &xn, sizeof xh, g_doc_cond); /* revalidation */

  struct h2tx h = {full_url, u->host, loc, loc_cap, 0, 0};
  struct h2_opts opts = {browser_ua(), xh, h2_headers_cb};
  char hdrbuf[RESP_HDR_MAX];
  int hlen = 0;
  bool reused = h2_session_used(hs);
  int rc =
      h2_session_run(hs, 1, auth, method, u->path, &opts, body, blen, hdrbuf, sizeof hdrbuf, &hlen, h2_body_cb, &h);
  h2_session_release(hs, u->host, u->port);
  *stale = reused && h.status == 0 && (rc == HTTP_ESEND || rc == HTTP_ENORESP);
  if (*stale)
    return -5;

  early_apply(full_url); /* merge any 103 Early-Hints stylesheets into ext_css */
  raw_decode(h.enc);
  return route_body(full_url, h.status > 0 ? h.status : -5, allow_dl);
}

/* ---- HTTP/3 transaction (QUIC; used for Alt-Svc-advertised origins) ---- */

/* Run the request over HTTP/3 on an idle or new QUIC connection. Returns the status; 0 when HTTP/3 can't be used and
 * the caller should fall back; -5 when a request with a body went out and no response came back. *stale is set when
 * a reused connection failed before any response, so retrying on a new one is safe. */
static int h3_txn(const struct purl *u, const char *full_url, char *loc, int loc_cap, int allow_dl, const char *method,
                  const unsigned char *body, int blen, const char *bctype, bool fresh, bool *stale)
{
  *stale = false;
  loc[0] = '\0';
  B->pg->ctype[0] = '\0';
  B->dl_name[0] = '\0';
  B->cdisp_attach = 0;
  B->pg->raw_len = 0;

  h3_push_init();
  h3_session *s = fresh ? 0 : h3_session_acquire(u->host, u->port);
  bool reused = s != 0;
  if (!s && h3_session_open(&s, u->host, u->port) != 0) {
    h3_forget(u->host);
    return 0;
  }

  char auth[URL_HOST_MAX + 8], hb[URL_HOST_MAX + 3];
  int an = 0;
  sapp(auth, &an, sizeof auth, url_bracket_host(u->host, hb, sizeof hb));
  if (u->port != 443) {
    sapp(auth, &an, sizeof auth, ":");
    uapp(auth, &an, sizeof auth, (unsigned)u->port);
  }

  char xh[1408];
  int xn = 0;
  sapp(xh, &xn, sizeof xh, "user-agent: ");
  sapp(xh, &xn, sizeof xh, browser_ua());
  sapp(xh, &xn, sizeof xh, "\r\n");
  doc_req_hdrs(xh, &xn, sizeof xh, u);
  if (body && blen > 0) {
    sapp(xh, &xn, sizeof xh, "content-type: ");
    sapp(xh, &xn, sizeof xh, bctype ? bctype : "application/x-www-form-urlencoded");
    sapp(xh, &xn, sizeof xh, "\r\n");
  }
  if (g_doc_cond[0])
    sapp(xh, &xn, sizeof xh, g_doc_cond); /* revalidation */

  struct h2tx h = {full_url, u->host, loc, loc_cap, 0, 0};
  struct h3_opts opts = {h2_headers_cb, &h};
  char hdrbuf[RESP_HDR_MAX];
  int hlen = 0;
  int rc =
      h3_session_run(s, auth, method, u->path, xh, body, blen, &opts, hdrbuf, sizeof hdrbuf, &hlen, h2_body_cb, &h);
  h3_session_release(s, u->host, u->port);

  if (h.status == 0) { /* no response */
    bool has_body = body && blen > 0;
    *stale = reused && !has_body && (rc == HTTP_ESEND || rc == HTTP_ENORESP);
    if (*stale)
      return 0;
    if (!reused)
      h3_forget(u->host);
    return has_body && rc != HTTP_ESEND ? -5 : 0;
  }
  early_apply(full_url); /* merge any 103 Early-Hints stylesheets into ext_css */
  raw_decode(h.enc);
  return route_body(full_url, h.status, allow_dl);
}

#define DOC_VARY_HDRS_MAX 2560

/* The request headers a document GET to `u` sends, as the cache's Vary input. */
static void doc_vary_hdrs(const struct purl *u, char *out)
{
  int n = 0;
  out[0] = '\0';
  sapp(out, &n, DOC_VARY_HDRS_MAX, "user-agent: ");
  sapp(out, &n, DOC_VARY_HDRS_MAX, browser_ua());
  sapp(out, &n, DOC_VARY_HDRS_MAX, "\r\n");
  doc_req_hdrs(out, &n, DOC_VARY_HDRS_MAX, u);
}

/* Apply the HTTP cache to a finished document: 304 reuses the stored body, a fresh non-download 200 is stored. */
static int doc_cache_finish(const struct purl *u, const char *full_url, long now, bool revalidating, int status)
{
  char vh[DOC_VARY_HDRS_MAX];
  doc_vary_hdrs(u, vh);
  if (revalidating && status == 304) {
    unsigned char *rb = 0;
    int rl = 0;
    /* The page is the cached response, so its headers (refreshed by the 304) replace the 304's own */
    static char stored[RESP_HDR_MAX];
    if (cache_revalidated(full_url, vh, now, g_doc_hdr, &rb, &rl, B->pg->ctype, sizeof B->pg->ctype, stored,
                          sizeof stored)) {
      strlcpy(g_doc_hdr, stored, sizeof g_doc_hdr);
      B->pg->raw_len = 0;
      if (browser_ensure_raw(rl)) {
        memcpy(B->pg->raw, rb, (unsigned)rl);
        B->pg->raw_len = rl;
      }
      free(rb);
      return 200;
    }
    return status; /* entry evicted mid-flight: render the (empty) 304 */
  }
  if (status == 200 && !B->dl_saved && B->pg->raw_len > 0)
    cache_store("GET", full_url, vh, 200, now, g_doc_hdr, B->pg->ctype, (const unsigned char *)B->pg->raw,
                B->pg->raw_len);
  return status;
}

/* The connection half of a document request, on an idle or new connection (h2 session or HTTP/1.1). *stale is set
 * when a reused connection failed before any response, so retrying on a new one is safe. */
/* The document request line and header block (the HTTP/1.1 wire form) into req; returns its length */
static int doc_request(char *req, int cap, const struct purl *u, const char *method, const unsigned char *body,
                       int blen, const char *bctype)
{
  int n = 0;
  sapp(req, &n, cap, method);
  sapp(req, &n, cap, " ");
  sapp(req, &n, cap, u->path);
  sapp(req, &n, cap, " HTTP/1.1\r\nHost: ");
  {
    char hb[URL_HOST_MAX + 3];
    sapp(req, &n, cap, url_bracket_host(u->host, hb, sizeof hb));
  }
  if (!((u->tls && u->port == 443) || (!u->tls && u->port == 80))) {
    sapp(req, &n, cap, ":");
    uapp(req, &n, cap, (unsigned)u->port);
  }
  {
    char ck[1024];
    if (cookie_header_tls(u->host, u->path, u->tls, ck, sizeof ck) > 0) {
      sapp(req, &n, cap, "\r\nCookie: ");
      sapp(req, &n, cap, ck);
    }
  }
  sapp(req, &n, cap, "\r\nUser-Agent: ");
  sapp(req, &n, cap, browser_ua());
  sapp(req, &n, cap,
       "\r\nAccept: " BR_ACCEPT "\r\n"
       "Accept-Language: " BR_ACCLNG "\r\n"
       "Accept-Encoding: gzip, deflate, br\r\n");
  {
    char ch[512];
    if (ch_headers_for(u, false, ch, sizeof ch) > 0)
      sapp(req, &n, cap, ch);
  }
  if (g_doc_cond[0])
    sapp(req, &n, cap, g_doc_cond); /* revalidation */
  if (body && blen > 0) {           /* POST/PUT body */
    sapp(req, &n, cap, "Content-Type: ");
    sapp(req, &n, cap, bctype ? bctype : "application/x-www-form-urlencoded");
    sapp(req, &n, cap, "\r\nContent-Length: ");
    uapp(req, &n, cap, (unsigned)blen);
    sapp(req, &n, cap, "\r\n");
  }
  sapp(req, &n, cap, "\r\n");
  return n;
}

static int http_txn_conn(const struct purl *u, const char *full_url, char *loc, int loc_cap, int allow_dl,
                         const char *method, const unsigned char *body, int blen, const char *bctype, long now,
                         bool revalidating, bool fresh, bool *stale)
{
  *stale = false;
  loc[0] = '\0';
  B->pg->ctype[0] = '\0';
  B->dl_name[0] = '\0';
  B->cdisp_attach = 0;
  B->rpos = B->rlen = B->reof = 0;
  B->pg->raw_len = 0;

  /* Over TLS, offer ALPN "h2"; if selected, run over HTTP/2 instead of HTTP/1.1. */
  h2_session *hs = h2_open(u, fresh, &B->sock);
  if (hs) {
    int st = h2_txn(hs, u, full_url, loc, loc_cap, allow_dl, method, body, blen, bctype, stale);
    return *stale ? st : doc_cache_finish(u, full_url, now, revalidating, st);
  }
  if (!B->sock) {
    if (nc_last_error() == NC_OPEN_RESOLVE)
      return -2;
    if (nc_last_error() == NC_OPEN_TLS)
      return -6;
    return -3;
  }
  bool pooled = nc_pooled(B->sock);

  char req[2048];
  int n = doc_request(req, sizeof req, u, method, body, blen, bctype);

  if (nc_write(B->sock, req, n) < 0 || (body && blen > 0 && nc_write(B->sock, (const char *)body, blen) < 0)) {
    nc_close(B->sock);
    B->sock = 0;
    *stale = pooled;
    return -4;
  }

  /* Read the header block, skipping 1xx interim responses; 103 Link hints are stashed for the final response. */
  char hdr[RESP_HDR_MAX];
  int hlen = 0;
  for (int interim = 0;;) {
    hlen = 0;
    int match = 0;
    for (;;) {
      int ci = rgetc();
      if (ci < 0) {
        nc_close(B->sock);
        B->sock = 0;
        *stale = pooled && ci == -1 && interim == 0;
        return -5;
      }
      char c = (char)ci;
      if (hlen < (int)sizeof hdr - 1)
        hdr[hlen++] = c;
      if (c == '\r')
        match = (match == 2) ? 3 : 1;
      else if (c == '\n')
        match = (match == 1) ? 2 : (match == 3 ? 4 : 0);
      else
        match = 0;
      if (match == 4)
        break;
    }
    hdr[hlen] = '\0';
    int s = hdr_status(hdr);
    if (s < 100 || s >= 200)
      break; /* the final response */
    if (s == 103)
      early_collect(hdr, full_url);
    if (++interim >= 16) {
      nc_close(B->sock);
      B->sock = 0;
      return -5;
    }
  }

  bool chunked, have_len;
  int enc_gz;
  unsigned content_len;
  int status = parse_response(hdr, u->host, full_url, loc, loc_cap, &chunked, &have_len, &content_len, &enc_gz);
  early_apply(full_url); /* merge any 103 Early-Hints stylesheets into ext_css */

  struct bodysrc bs = {chunked, have_len, false, false, have_len ? content_len : 0, false};
  if (status == 204 || status == 304 || streq(method, "HEAD"))
    bs.done = bs.clean = true; /* no body follows the header block */

  /* Read the whole framed body into B->pg->raw; the common pipeline decodes and routes it. */
  int c;
  while ((c = body_src(&bs)) >= 0) {
    if (!browser_ensure_raw(B->pg->raw_len + 1))
      break; /* out of memory: stop */
    B->pg->raw[B->pg->raw_len++] = (char)c;
  }
  /* A connection left at a response boundary stays open for the next request to this origin. */
  bool keep = false;
  if (bs.clean && !B->abort_load && !B->reof && B->rpos == B->rlen) {
    struct http_headers H;
    http_headers_parse(&H, g_doc_hdr);
    const char *cn = http_headers_get(&H, "connection");
    keep = starts_ci(g_doc_hdr, "HTTP/1.1") && !(cn && http_header_has_token(cn, "close"));
    http_headers_free(&H);
  }
  if (keep)
    nc_release(B->sock, u->host, u->port, u->tls);
  else
    nc_close(B->sock);
  B->sock = 0;

  raw_decode(enc_gz);
  return doc_cache_finish(u, full_url, now, revalidating, route_body(full_url, status, allow_dl));
}

static int http_txn(const struct purl *u, const char *full_url, char *loc, int loc_cap, int allow_dl,
                    const char *method, const unsigned char *body, int blen, const char *bctype)
{
  bool is_get = streq(method, "GET");
  g_doc_t0 = msec(); /* for the devtools Network duration of this response */
  loc[0] = '\0';
  B->pg->ctype[0] = '\0';
  B->dl_name[0] = '\0';
  B->cdisp_attach = 0;
  B->rpos = B->rlen = B->reof = 0;
  B->pg->raw_len = 0;
  early_reset();

  /* HTTP cache (RFC 7234): fresh hits skip the wire, stale ones revalidate via g_doc_cond; downloads are never cached.
   */
  long now = (long)rtc_epoch();
  bool revalidating = false;
  g_doc_cond[0] = '\0';
  g_doc_from_cache = false;
  if (is_get) { /* only GET is cacheable / revalidatable */
    unsigned char *cbody = 0;
    int clen = 0;
    char vh[DOC_VARY_HDRS_MAX];
    doc_vary_hdrs(u, vh);
    int cv = cache_lookup("GET", full_url, vh, now, &cbody, &clen, B->pg->ctype, sizeof B->pg->ctype, g_doc_hdr,
                          sizeof g_doc_hdr, g_doc_cond, sizeof g_doc_cond);
    if (cv == CACHE_FRESH) {
      B->pg->raw_len = 0;
      if (browser_ensure_raw(clen)) {
        memcpy(B->pg->raw, cbody, (unsigned)clen);
        B->pg->raw_len = clen;
      }
      free(cbody);
      g_doc_from_cache = true;
      return 200;
    }
    revalidating = (cv == CACHE_REVALIDATE);
  }

  /* If this origin advertised HTTP/3, try QUIC first, else fall through to TLS h2/h1.1. */
  if (u->tls && u->port == 443 && h3_known(u->host)) {
    bool h3_stale;
    int st = h3_txn(u, full_url, loc, loc_cap, allow_dl, method, body, blen, bctype, false, &h3_stale);
    if (h3_stale)
      st = h3_txn(u, full_url, loc, loc_cap, allow_dl, method, body, blen, bctype, true, &h3_stale);
    if (st > 0)
      return doc_cache_finish(u, full_url, now, revalidating, st);
    if (st < 0)
      return st;
    loc[0] = '\0';
    B->pg->ctype[0] = '\0';
    B->dl_name[0] = '\0';
    B->cdisp_attach = 0;
    B->pg->raw_len = 0;
  }

  bool stale;
  int st =
      http_txn_conn(u, full_url, loc, loc_cap, allow_dl, method, body, blen, bctype, now, revalidating, false, &stale);
  if (st < 0 && stale && !(body && blen > 0))
    st =
        http_txn_conn(u, full_url, loc, loc_cap, allow_dl, method, body, blen, bctype, now, revalidating, true, &stale);
  return st;
}

/* ----------------------------------------------- local & inline URL schemes */

/* Render B->pg->raw (ctype set) as HTML, text or an image; shared by network, file: and data: paths. */
static void render_raw(const char *url)
{
  /* Empty Content-Type renders as HTML, except under nosniff which requires an explicit HTML type. */
  bool as_html = is_html_type(B->pg->ctype) || (B->pg->ctype[0] == '\0' && !B->nosniff);
  if (as_html) {
    B->pg->mode = MODE_HTML;
    html_to_text(url, css_batch);
  } else if (starts_ci(B->pg->ctype, "image/png") || starts_ci(B->pg->ctype, "image/gif") ||
             starts_ci(B->pg->ctype, "image/webp")) {
    B->pg->mode = MODE_IMAGE;
    B->pg->text_len = 0;
    B->pg->nlinks = 0;
    B->pg->nregs = 0;
    B->pg->doc_title[0] = '\0';
  } else if (starts_ci(B->pg->ctype, "image/")) {
    B->pg->mode = MODE_TEXT;
    doc_message("[unsupported image type - only PNG, GIF and WebP can be shown]");
  } else {
    B->pg->mode = MODE_TEXT;
    plain_to_text();
  }
  B->pg->scroll = 0;
  {
    int ci = 0;
    sapp(B->pg->cur_url, &ci, sizeof B->pg->cur_url, url);
  }
}

/* Content-Type for a local file from its extension (HTML, PNG, else text). */
static void ctype_from_ext(const char *path)
{
  const char *ext = 0;
  for (const char *q = path; *q; q++) {
    if (*q == '/' || *q == '\\')
      ext = 0;
    else if (*q == '.')
      ext = q + 1;
  }
  const char *t = "text/plain";
  if (ext) {
    if (streq_ci(ext, "html") || streq_ci(ext, "htm"))
      t = "text/html";
    else if (streq_ci(ext, "png"))
      t = "image/png";
  }
  int k = 0;
  sapp(B->pg->ctype, &k, sizeof B->pg->ctype, t);
}

/* file://: read a local file into B->pg->raw, grown on demand; host ignored, ?query and #fragment dropped. */
static int load_file_url(const char *url)
{
  const char *p = url + 5;          /* past "file:" */
  if (p[0] == '/' && p[1] == '/') { /* "//" then an (ignored) host       */
    p += 2;
    while (*p && *p != '/')
      p++;
  }
  if (*p != '/')
    return -1; /* need an absolute local path       */

  char path[URL_MAX];
  int pn = 0;
  for (const char *q = p; *q && *q != '?' && *q != '#' && pn < (int)sizeof path - 1; q++)
    path[pn++] = *q;
  path[pn] = '\0';

  B->pg->raw_len = 0;
  unsigned off = 0;
  for (;;) {
    if (!browser_ensure_raw(B->pg->raw_len + 4096))
      break; /* out of memory: stop */
    int got = readfile_at(path, off, B->pg->raw + B->pg->raw_len, 4096);
    if (got < 0)
      return -1;
    if (got == 0)
      break; /* end of file                       */
    B->pg->raw_len += got;
    off += (unsigned)got;
  }
  ctype_from_ext(path);
  return 0;
}

/* data:[<mediatype>][;base64],<data> or blob: URL: load into B->pg->raw; sets ctype. */
static int load_data_url(const char *url)
{
  int len = 0;
  B->pg->ctype[0] = '\0';
  unsigned char *buf = starts_ci(url, "blob:") ? blobstore_get(url, &len, B->pg->ctype, sizeof B->pg->ctype)
                                               : decode_data_url(url, &len, B->pg->ctype, sizeof B->pg->ctype);
  if (!buf)
    return -1;
  B->pg->raw_len = 0;
  for (int i = 0; i < len; i++) {
    if (!browser_ensure_raw(B->pg->raw_len + 1))
      break;
    B->pg->raw[B->pg->raw_len++] = (char)buf[i];
  }
  free(buf);
  return 0;
}

void do_fetch(void)
{
  char url[URL_MAX];
  int n = 0;
  /* Keep the page being left as the referrer (cur_url still holds the old URL). */
  {
    int k = 0;
    B->referrer[0] = '\0';
    sapp(B->referrer, &k, sizeof B->referrer, B->pg->cur_url);
  }
  hints_join();
  /* Free the previous page's DOM/CSS/JS on every navigation. */
  html_page_teardown();
  if (B->pg->csp) {
    csp_free(B->pg->csp);
    B->pg->csp = 0;
  } /* drop the old page's CSP; parse_response/meta rebuild it */
  B->pg->perm_policy = 0; /* drop the old Permissions-Policy; parse_response rebuilds it */
  free(B->pg->perm_policy_hdr);
  B->pg->perm_policy_hdr = 0;
  devtools_nav_reset(); /* a new top-level navigation starts a fresh net log + console */
  sapp(url, &n, sizeof url, B->req_url);
  B->dl_saved = 0;
  B->scripts_pending = 0; /* only an HTML render (html_to_text) re-arms it */
  B->pg->favicon_ok = 0;  /* cleared per navigation; refilled on success */
  ext_css_clear();        /* Link: stylesheets refilled by parse_response  */

  /* Capture this navigation's method + body without copying; req_method is cleared so the next one is GET. */
  char method[8];
  strcpy(method, B->req_method[0] ? B->req_method : "GET");
  const unsigned char *body = (const unsigned char *)B->req_body;
  int blen = B->req_body_len;
  const char *bctype = B->req_ctype[0] ? B->req_ctype : 0;
  B->req_method[0] = '\0';
  B->req_body_len = 0;

  /* file: and data: never touch the network; they fill raw + ctype and render like a fetched page. */
  if (starts_ci(url, "file:") || starts_ci(url, "data:") || starts_ci(url, "blob:")) {
    bool data = !starts_ci(url, "file:");
    B->pg->ctype[0] = '\0';
    B->dl_name[0] = '\0';
    B->cdisp_attach = 0;
    B->rpos = B->rlen = B->reof = 0;
    B->pg->n_imgs = 0;
    int rc = data ? load_data_url(url) : load_file_url(url);
    if (rc < 0) {
      const char *msg = data ? "Malformed data: or blob: URL" : "Cannot open file (not found or not absolute)";
      show_error(msg);
      {
        struct net_log e = {"GET", url, 0, 0, 0, 0, 0, NET_FAILED, msg};
        devtools_net_add(&e);
      }
      return;
    }
    resource_set_page(url);
    render_raw(url);
    {
      struct net_log e = {"GET", url, 200, B->pg->ctype, B->pg->raw_len, 0, 0};
      devtools_net_add(&e);
    }
    return;
  }

  /* Add https:// to a bare host; remember it was implicit so a TLS failure falls back to http. */
  bool implicit_scheme = false;
  if (!starts_ci(url, "http://") && !starts_ci(url, "https://")) {
    char tmp[URL_MAX];
    int t = 0;
    sapp(tmp, &t, sizeof tmp, "https://");
    sapp(tmp, &t, sizeof tmp, url);
    n = 0;
    sapp(url, &n, sizeof url, tmp);
    implicit_scheme = true;
  }

  /* Log the document as in-flight up front so slow loads show in the Network panel. */
  g_doc_t0 = msec();
  int docnid = devtools_net_begin(method, url);
  bool ch_retried = false;

  for (int hop = 0; hop <= MAX_REDIR; hop++) {
    if (B->abort_load) { /* stopped before this hop: leave as-is */
      struct net_log e = {method, url, 0, 0, 0, (int)(msec() - g_doc_t0), 0, NET_CANCELLED, 0};
      devtools_net_update(docnid, &e);
      return;
    }
    struct purl u;
    if (!parse_url(url, &u)) {
      show_error("Bad URL");
      struct net_log e = {method, url, 0, 0, 0, (int)(msec() - g_doc_t0), 0, NET_FAILED, "Bad URL"};
      devtools_net_update(docnid, &e);
      return;
    }

    /* HSTS: a host with Strict-Transport-Security is only contacted over HTTPS. */
    if (!u.tls && hsts_has(u.host)) {
      u.tls = 1;
      if (u.port == 80)
        u.port = 443;
    }

    char loc[URL_MAX], ch_sent[512];
    ch_headers_for(&u, false, ch_sent, sizeof ch_sent);
    int st = http_txn(&u, url, loc, sizeof loc, 1, method, body, blen, bctype);
    if (st < 0) {
      if (B->abort_load) { /* user stopped: no error page */
        struct net_log e = {method, url, 0, 0, 0, (int)(msec() - g_doc_t0), 0, NET_CANCELLED, 0};
        devtools_net_update(docnid, &e);
        return;
      }
      /* TLS connect failed for an auto-https URL: retry over http once. */
      if (implicit_scheme && st == -3 && starts_ci(url, "https://")) {
        char tmp[URL_MAX];
        int t = 0;
        sapp(tmp, &t, sizeof tmp, "http://");
        sapp(tmp, &t, sizeof tmp, url + 8); /* past "https://" */
        n = 0;
        sapp(url, &n, sizeof url, tmp);
        implicit_scheme = false;
        continue;
      }
      const char *msg = fetch_error_message(st);
      show_error(msg);
      {
        struct net_log e = {method, url, 0, 0, 0, (int)(msec() - g_doc_t0), 0, NET_FAILED, msg};
        devtools_net_update(docnid, &e);
      }
      return;
    }
    if (!ch_retried && streq(method, "GET") && !B->dl_saved) {
      struct http_headers H;
      http_headers_parse(&H, g_doc_hdr);
      char *critical = join_headers(&H, "critical-ch");
      http_headers_free(&H);
      char ch_now[512];
      ch_headers_for(&u, false, ch_now, sizeof ch_now);
      ch_retried = critical && ch_critical_retry(critical, ch_sent, ch_now);
      free(critical);
      if (ch_retried) {
        hop--;
        continue;
      }
    }
    if ((st == 301 || st == 302 || st == 303 || st == 307 || st == 308) && loc[0]) {
      if (hop == MAX_REDIR) {
        show_error("Too many redirects");
        struct net_log e = {
            method, url, st, 0, 0, (int)(msec() - g_doc_t0), g_doc_hdr, NET_FAILED, "Too many redirects"};
        devtools_net_update(docnid, &e);
        return;
      }
      /* A POST redirected by 301/302/303 becomes a bodyless GET; 307/308 keep method and body. */
      if (!streq(method, "GET") && st != 307 && st != 308) {
        strcpy(method, "GET");
        body = 0;
        blen = 0;
        bctype = 0;
      }
      n = 0;
      sapp(url, &n, sizeof url, loc);
      continue;
    }

    /* Resolve the in-flight document row with the final response. */
    {
      char reqh[2048];
      reqh[0] = '\0';
      if (!g_doc_from_cache)
        doc_request(reqh, sizeof reqh, &u, method, body, blen, bctype);
      struct net_log e = {method,
                          url,
                          st,
                          B->pg->ctype,
                          B->dl_saved ? 0 : B->pg->raw_len,
                          (int)(msec() - g_doc_t0),
                          g_doc_hdr,
                          NET_DONE,
                          0,
                          reqh,
                          B->dl_saved ? 0 : B->pg->raw,
                          B->dl_saved ? 0 : B->pg->raw_len,
                          g_doc_from_cache};
      devtools_net_update(docnid, &e);
    }

    /* A download was streamed to ~/Downloads inside http_txn, so there is nothing to render. */
    if (B->dl_saved)
      return;

    /* Render by response type: empty Content-Type is HTML, text is verbatim, image/png is decoded. */
    resource_set_page(url); /* attribute this page's sub-resources for CORP */
    render_raw(url);
    /* The title bar is updated by the main thread (win_settitle is owner-only). */

    /* Inline <img> are fetched by the parallel fetchers in resource.c; each landed image bumps B->seq. */
    if (B->pg->mode == MODE_HTML && B->pg->n_imgs > 0 && !B->abort_load)
      fetch_inline_images(url);

    /* Pull the site favicon for the tab strip, unless the page is itself an image. */
    if (B->pg->mode != MODE_IMAGE && !B->abort_load)
      fetch_favicon(url);

    /* Persist cached documents, stylesheets, images and favicon here (owofs allows one write stream at a time). */
    hints_join();
    cache_flush();
    return;
  }
}
