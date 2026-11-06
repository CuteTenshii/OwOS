/* fetch.c — the browser's main page transaction: the buffered reader, response-
 * header parsing, the HTTP/1.1 + HTTP/2 fetch, downloads (an attachment
 * response streamed to disk instead of B->pg->raw — kept here because it shares
 * this file's body reader), the redirect follower, the file:/data: schemes, and
 * do_fetch. Separately-fetched sub-resources (inline images, favicon, external
 * stylesheets) live in resource.c; the cookie/HSTS jar in jar.c. Runs on the
 * worker thread (main.c), so its blocking reads never stall the UI. */
#include "brotli.h" /* RFC 7932 `br` content-coding (whole-buffer decode)   */
#include "browser.h"
#include "cache.h"       /* the HTTP response cache (RFC 7234, honours headers)  */
#include "clienthints.h" /* Accept-CH: per-origin Client Hints opt-in           */
#include "fetchint.h"    /* shared transport interface + BR_* request headers   */
#include "h3.h"          /* HTTP/3 over QUIC (libcurl engine)                    */
#include "headers.h"     /* libcurl: HTTP response header parsing               */
#include "jar.h"         /* scheme/path-aware cookie header (Secure/Path)        */

/* The machine token for the UA platform, from uname(2), normalized the way
 * browsers present it: i386/i686 collapse to "x86", x86_64/amd64 to "x86_64",
 * 64-bit ARM to "aarch64", 32-bit ARM to "arm". Cached — arch can't change at
 * runtime, and uname() is authoritative so there's no hardcoded fallback. */
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

/* navigator.appVersion — the UA minus "Mozilla/". Built once from ua_arch() +
 * OWOS_VERSION (the browser version), so neither arch nor version is hardcoded.
 */
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

/* The full User-Agent, "Mozilla/" + browser_ua_version(). Shared by the HTTP
 * transport and the JS navigator so wire and script never disagree. */
const char *browser_ua(void)
{
  static char buf[168];
  if (!buf[0])
    snprintf(buf, sizeof buf, "Mozilla/%s", browser_ua_version());
  return buf;
}

/* Content-encoding of a response body. */
enum { ENC_NONE = 0, ENC_ZLIB = 1, ENC_BR = 2 };

/* Hard ceiling on a single decompressed body. A small gzip/deflate/br response
 * can expand enormously (a decompression bomb); without a cap the sink grows
 * until it exhausts memory, and the old (len+n)*2 size math overflowed to a
 * negative int near ~1 GiB. RAW_MAX (browser.h) bounds the *compressed* body we
 * read; this bounds the decompressed output at a generous multiple of it. */
#define DECOMP_MAX (RAW_MAX * 2) /* 64 MiB */

/* Origins observed to advertise HTTP/3 via Alt-Svc; the next request to one is
 * tried over h3 first (how browsers bootstrap h3 without an extra round trip).
 */
#define H3_HOSTS_MAX 32
static char h3_hosts[H3_HOSTS_MAX][URL_HOST_MAX];
static int h3_nhosts;
static int h3_known(const char *host)
{
  for (int i = 0; i < h3_nhosts; i++)
    if (streq(h3_hosts[i], host))
      return 1;
  return 0;
}
static void h3_remember(const char *host)
{
  if (h3_known(host) || h3_nhosts >= H3_HOSTS_MAX)
    return;
  int j = 0;
  for (; host[j] && j < URL_HOST_MAX - 1; j++)
    h3_hosts[h3_nhosts][j] = host[j];
  h3_hosts[h3_nhosts][j] = '\0';
  h3_nhosts++;
}

/* One body pipeline for every transport: each fills B->pg->raw with the raw
 * (maybe compressed) body, then raw_decode() decodes Content-Encoding in place
 * and route_body() decides render vs download. (No per-transport decode paths.)
 *
 * Decompress an in-memory body per its content-encoding (ENC_BR / ENC_ZLIB).
 * Returns a malloc'd buffer of *out_len bytes (caller frees), or NULL. */
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
  int cap, len, oom;
};
static int memwr_fn(void *c, const unsigned char *p, unsigned n)
{
  struct memwr *w = c;
  /* Bomb guard: refuse a write that pushes the output past the cap. Unsigned
   * math throughout so the size can't wrap negative near the ceiling. */
  if (n > (unsigned)DECOMP_MAX || (unsigned)w->len + n > (unsigned)DECOMP_MAX) {
    w->oom = 1;
    return -1;
  }
  if (w->len + (int)n > w->cap) {
    unsigned want = (unsigned)w->len + n;
    unsigned nc = want + want / 2 + 4096; /* ~1.5x with headroom */
    if (nc > (unsigned)DECOMP_MAX)
      nc = (unsigned)DECOMP_MAX;
    unsigned char *nb = realloc(w->buf, nc);
    if (!nb) {
      w->oom = 1;
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
    /* brotli decodes whole-buffer inside the lib; the most we can enforce
     * here is to reject (and free) an over-cap result before it's adopted. */
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

/* Decode the body already collected in B->pg->raw per its content-encoding, in
 * place. A failed decode empties B->pg->raw (never show the compressed bytes).
 */
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
  /* Adopt the decoded buffer directly rather than copying it back into raw:
   * both come from the same malloc/realloc/free family (browser_ensure_raw and the
   * memwr/brotli sinks). browser_buf_lock guards the swap against the main thread's
   * concurrent reads of B->pg->raw, matching browser_ensure_raw's discipline. */
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

/* Show a one-line message as the page body. The browser has no status bar, so
 * errors (and other notices that used to live there) are rendered straight into
 * the content pane. Clears cur_url so the address bar keeps whatever the user
 * typed, ready to edit and retry. */
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

static int mime_is(const char *ctype, const char *want)
{
  int n = 0;
  while (ctype[n] && ctype[n] != ';' && ctype[n] != ' ' && ctype[n] != '\t')
    n++;
  int w = 0;
  while (want[w])
    w++;
  if (n != w)
    return 0;
  for (int i = 0; i < n; i++)
    if (lower(ctype[i]) != lower(want[i]))
      return 0;
  return 1;
}

static int is_html_type(const char *ctype)
{
  return mime_is(ctype, "text/html") || mime_is(ctype, "application/xhtml+xml");
}

/* --------------------------------------------------- body byte source/sink */

/* Yields the next *content* byte of the response body, transparently undoing
 * the transfer framing (chunked sizes, Content-Length limit, or read-to-close)
 * — so whatever consumes it (a plain copy, or the inflate decoder) sees only
 * the payload. Returns the byte (0..255) or -1 at end of body. */
struct bodysrc {
  int chunked, have_len, done, need_crlf;
  unsigned remaining; /* bytes left in this chunk / the content-length   */
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
          b->done = 1;
          return -1;
        }
        b->need_crlf = 0;
      }
      unsigned sz = 0;
      int c, any = 0;
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
        any = 1;
      }
      while (c >= 0 && c != '\n')
        c = rgetc();
      if (!any || sz == 0) {
        b->done = 1;
        return -1;
      }
      b->remaining = sz;
      b->need_crlf = 1;
    }
    int c = rgetc();
    if (c < 0) {
      b->done = 1;
      return -1;
    }
    b->remaining--;
    return c;
  }
  if (b->have_len) {
    if (b->remaining == 0) {
      b->done = 1;
      return -1;
    }
    int c = rgetc();
    if (c < 0) {
      b->done = 1;
      return -1;
    }
    b->remaining--;
    return c;
  }
  int c = rgetc();
  if (c < 0) {
    b->done = 1;
    return -1;
  }
  return c;
}

/* ------------------------------------------------------------- downloads */

#define DL_DIR "/home/root/Downloads"

/* A response is a download when the server flags it as an attachment, when the
 * URL points at a file type the browser never renders inline (audio/video/
 * archive/document/binary — matched by extension), or when the Content-Type is
 * something the pane can't show (anything that isn't HTML, plain text, or an
 * image — images get their own viewer/notice). The extension check catches a
 * download a sloppy server mislabels: an .mp3 served as text/plain or with no
 * Content-Type at all would otherwise dump raw bytes into the page. An empty
 * Content-Type with no telltale extension is treated as HTML, the common case.
 */
static int url_is_download_ext(const char *url);
static int is_download(const char *url)
{
  if (B->cdisp_attach)
    return 1;
  if (url_is_download_ext(url))
    return 1;
  if (B->pg->ctype[0] == '\0')
    return 0;
  if (is_html_type(B->pg->ctype))
    return 0;
  if (starts_ci(B->pg->ctype, "text/"))
    return 0;
  if (starts_ci(B->pg->ctype, "image/"))
    return 0;
  return 1;
}

/* Copy the last path component of `in` into `out`, dropping any directory
 * prefix and a trailing "?query" — so "/files/a/b.zip?x=1" -> "b.zip". */
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

/* True when the URL's last path segment ends in an extension the browser never
 * renders inline (audio, video, archives, documents, executables, fonts). Lets
 * is_download() save such a file even when the server omits or mislabels the
 * Content-Type — e.g. an .mp3 served as text/plain, which otherwise filled the
 * page with raw bytes. Image extensions are deliberately absent: PNGs are shown
 * inline, a decision left to the Content-Type. */
static int url_is_download_ext(const char *url)
{
  struct purl u;
  if (!parse_url(url, &u))
    return 0;
  char base[128];
  basename_of(u.path, base, sizeof base);
  const char *ext = 0;
  for (const char *q = base; *q; q++)
    if (*q == '.')
      ext = q + 1;
  if (!ext || !*ext)
    return 0;
  static const char *const bin_ext[] = {
      "mp3", "wav", "flac", "ogg", "oga",  "opus", "m4a",  "aac", "wma",  "mid",   "mp4",  "mkv", "webm", "avi",
      "mov", "wmv", "flv",  "m4v", "mpg",  "mpeg", "zip",  "gz",  "tgz",  "bz2",   "xz",   "7z",  "rar",  "tar",
      "iso", "img", "pdf",  "doc", "docx", "xls",  "xlsx", "ppt", "pptx", "odt",   "epub", "exe", "dll",  "bin",
      "dmg", "deb", "rpm",  "apk", "msi",  "so",   "ttf",  "otf", "woff", "woff2", 0};
  for (int i = 0; bin_ext[i]; i++)
    if (streq_ci(ext, bin_ext[i]))
      return 1;
  return 0;
}

/* Choose a filename for the download: the Content-Disposition name if any, else
 * the URL's last path segment, else a generic fallback. */
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

/* owofs caps each path component at 63 chars (FS_NAME_LEN - 1); a longer leaf
 * makes fs_stream_begin reject the whole path, which surfaced as a bogus
 * "disk full" download failure for any file whose name exceeds that. */
#define DL_LEAF_MAX 63

/* Build DL_DIR/<base> for attempt 0, or DL_DIR/<stem>-<n><ext> for n>=1, so a
 * repeated download lands beside the original instead of clobbering it. The
 * leaf is clamped to owofs's component limit, truncating the stem (never the
 * extension or the collision suffix) so e.g. "annual-report-2026-final.pdf"
 * still lands as a valid "annual-rep.pdf". */
static void build_path(const char *base, int n, char *out, int cap)
{
  /* Split base into stem and ".ext" at the last dot (a leading dot doesn't
   * count -- a dotfile has no extension to preserve). */
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

  /* A pathologically long extension can't fit either; cap it, leaving room
   * for the suffix and at least one stem char. */
  if (ext_len > DL_LEAF_MAX - sl - 1) {
    ext_len = DL_LEAF_MAX - sl - 1;
    if (ext_len < 0)
      ext_len = 0;
  }
  /* Truncate the stem so stem + suffix + ext fits the component limit. */
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

/* Pick a non-clobbering DL_DIR/<name> path for a download (the saved name comes
 * from Content-Disposition or the URL; a repeat gets a "-N" suffix). Ensures
 * DL_DIR exists first: the boot image creates it, but a disk.img persisted from
 * an older build may not have it, and a missing directory makes stream_begin
 * fail (which the h2 path used to mishandle by dumping the body into the page).
 */
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

/* Report a finished download: switch to a text pane on `url` and show the saved
 * path (and byte count) or a failure notice. Shared by the 1.1 and h2 paths. */
static void download_finished(const char *url, const char *path, int wrote, int fail)
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

/* External stylesheet URLs from the Link: response header (heap list in B->
 * ext_css). Reset each response, consumed by html_to_text before layout. */
static void ext_css_clear(void)
{
  struct css_link *c = B->ext_css;
  /* Defensive: the head is always NULL or a heap node (>= USER_BASE). A lower
   * value is a stray write from elsewhere — drop the list rather than fault. */
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

/* True if "stylesheet" appears as a whitespace-delimited token of the rel value
 * within [q,end) — so rel="not-a-stylesheet" or "stylesheet-alt" doesn't match.
 * A trailing '"' (from a quoted rel="stylesheet") is treated as a separator. */
static int rel_has_stylesheet(const char *q, const char *end)
{
  for (const char *s = q; s < end;) {
    while (s < end && (*s == ' ' || *s == '\t' || *s == '"'))
      s++;
    const char *tok = s;
    while (s < end && *s != ' ' && *s != '\t' && *s != '"')
      s++;
    if (s - tok == 10 && starts_ci(tok, "stylesheet"))
      return 1;
  }
  return 0;
}

/* Parse one Link: header value (RFC 8288): "<url>; rel=stylesheet, <url2>; …".
 * For each entry whose rel includes "stylesheet", resolve its URL against
 * `base` and add it to B->ext_css. Each entry's params are scanned only up to
 * the next top-level comma, so one entry's rel can't leak into another's. */
static void parse_link_header(const char *v, const char *base)
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

    int is_sheet = 0; /* scan params (after the URL) */
    for (const char *q = p; q < eend; q++) {
      if ((q == p || q[-1] == ';' || q[-1] == ' ') && starts_ci(q, "rel")) {
        const char *r = q + 3;
        while (r < eend && *r == ' ')
          r++;
        if (r < eend && *r == '=') {
          r++;
          while (r < eend && (*r == ' ' || *r == '"'))
            r++;
          const char *rv = r;
          while (rv < eend && *rv != ';')
            rv++;
          if (rel_has_stylesheet(r, rv))
            is_sheet = 1;
        }
      }
    }

    if (is_sheet && url[0]) {
      char abs[URL_MAX];
      resolve_url(base, url, abs, sizeof abs);
      ext_css_add(abs);
    }
    v = (*eend == ',') ? eend + 1 : eend;
  }
}

/* Status code of a raw header block: "HTTP/x.y NNN …" -> NNN (0 if
 * unparseable). */
static int hdr_status(const char *hdr)
{
  const char *p = hdr;
  while (*p && *p != ' ')
    p++; /* skip the "HTTP/x.y" token */
  while (*p == ' ')
    p++;
  int s = 0, any = 0;
  while (*p >= '0' && *p <= '9') {
    s = s * 10 + (*p++ - '0');
    any = 1;
  }
  return any ? s : 0;
}

/* 103 Early Hints: a 1xx interim response precedes the final one and may carry
 * Link: rel=stylesheet hints. Its Link values are stashed here (joined by '\n')
 * because the final response's parse_response clears ext_css; early_apply
 * merges them back afterwards. Reset per request in http_txn. */
static char g_early[2048];
static int g_early_len;

static void early_reset(void)
{
  g_early_len = 0;
  g_early[0] = '\0';
}

static void early_collect(const char *hdr)
{
  struct http_headers H;
  http_headers_parse(&H, hdr);
  int n = http_headers_count(&H, "link");
  for (int i = 0; i < n; i++) {
    const char *v = http_headers_at(&H, "link", i);
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
      parse_link_header(line, base);
  }
}

/* Parse a response header block (an HTTP/1.1 block, or the HTTP/2-synthesized
 * "HTTP/x NNN\r\nname: value\r\n…" equivalent) into the browser state: sets
 * B->pg->ctype / B->dl_name / B->cdisp_attach, stores cookies for `host`, notes
 * HSTS, collects Link: stylesheets, resolves any Location into `loc`, and
 * reports the body framing (chunked / content-length / gzip — moot over h2,
 * which sends none). Returns the status code. Shared by the HTTP/1.1 and HTTP/2
 * fetch paths.*/
/* The most recent document response's raw header block + the tick http_txn
 * began, captured here / in http_txn so do_fetch can log them to the devtools
 * Network tab (the transport functions return only the status, not the header
 * block or timing). Document-only: sub-resources are logged from resource.c's
 * own transport. */
static char g_doc_hdr[RESP_HDR_MAX];
static unsigned g_doc_t0;
/* Conditional-request header lines ("if-none-match"/"if-modified-since") for a
 * stale cached document, filled by http_txn's cache_lookup and appended by each
 * transport's request builder. Lowercased, so one string serves h1.1/h2/h3. */
static char g_doc_cond[512];

static int parse_response(const char *hdr, const char *host, const char *full_url, char *loc, int loc_cap, int *chunked,
                          int *have_len, unsigned *content_len, int *enc_gz)
{
  {
    int k = 0;
    for (; hdr[k] && k < (int)sizeof g_doc_hdr - 1; k++)
      g_doc_hdr[k] = hdr[k];
    g_doc_hdr[k] = '\0';
  }
  *chunked = 0;
  *have_len = 0;
  *enc_gz = 0;
  *content_len = 0;
  B->nosniff = false;
  ext_css_clear(); /* this response's Link: stylesheets replace the last */

  /* Parse the raw block once into a k/v object, then just query it. */
  struct http_headers H;
  http_headers_parse(&H, hdr);
  int status = H.status;

  const char *te = http_headers_get(&H, "transfer-encoding");
  if (te && http_header_has_token(te, "chunked"))
    *chunked = 1;

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
      *have_len = 1;
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
      int quoted = (*f == '"');
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

  /* Set-Cookie may appear several times — store every one. */
  int ncookie = http_headers_count(&H, "set-cookie");
  for (int i = 0; i < ncookie; i++)
    cookie_store(host, http_headers_at(&H, "set-cookie", i));

  if (http_headers_get(&H, "strict-transport-security"))
    hsts_add(host);

  /* Accept-CH (Client Hints opt-in): record which hints this origin wants so
   * later requests to it carry them. Honoured on https only (spec: secure). */
  const char *ach = http_headers_get(&H, "accept-ch");
  if (ach && starts_ci(full_url, "https://"))
    ch_accept(host, ach);

  /* Alt-Svc advertising HTTP/3 (h3=… or h3-NN=…) — remember the origin so the
   * next request to it goes over QUIC. */
  const char *as = http_headers_get(&H, "alt-svc");
  if (as)
    for (const char *p = as; *p; p++)
      if ((p[0] == 'h' || p[0] == 'H') && p[1] == '3' && (p[2] == '=' || p[2] == '-')) {
        h3_remember(host);
        break;
      }

  /* Link: <a.css>; rel=stylesheet — external sheets declared in the header. */
  int nlink = http_headers_count(&H, "link");
  for (int i = 0; i < nlink; i++)
    parse_link_header(http_headers_at(&H, "link", i), full_url);

  /* nosniff: the server forbids MIME-sniffing — honour the declared
   * Content-Type and never guess the body's type (see render_raw). */
  const char *xcto = http_headers_get(&H, "x-content-type-options");
  if (xcto && http_header_has_token(xcto, "nosniff"))
    B->nosniff = true;

  /* Content-Security-Policy: build the page's enforced policy set — each header
   * is one policy and a load must satisfy them all. Report-Only is deliberately
   * not enforced, so it is left out. (The final hop after redirects wins.) */
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

  /* Permissions-Policy (and the legacy Feature-Policy): a feature the site's
   * own policy disallows for this document is blocked with no prompt. Multiple
   * headers combine (OR the blocked bitmaps); the final hop after redirects
   * defines the page, so overwrite rather than accumulate across responses. */
  B->pg->perm_policy = 0;
  for (int i = 0, np = http_headers_count(&H, "permissions-policy"); i < np; i++)
    B->pg->perm_policy |= perm_policy_parse(http_headers_at(&H, "permissions-policy", i), host);
  for (int i = 0, nf = http_headers_count(&H, "feature-policy"); i < nf; i++)
    B->pg->perm_policy |= perm_policy_parse(http_headers_at(&H, "feature-policy", i), host);

  http_headers_free(&H);
  jar_flush(); /* persist cookies/HSTS once, if this response changed them */
  return status;
}

/* Route the fully-received, already-content-decoded body in B->pg->raw: a
 * download (attachment / non-renderable type on a 2xx) is written to
 * ~/Downloads and B->pg->raw cleared; anything else stays in B->pg->raw for
 * rendering. The single place the download decision lives, shared by every
 * transport. Returns `status`. */
static int route_body(const char *full_url, int status, int allow_dl)
{
  if (allow_dl && status >= 200 && status < 300 && is_download(full_url)) {
    char path[256];
    pick_dl_path(full_url, path, sizeof path);
    int wrote = -1, fail = 0;
    if (stream_begin(path) == 0) {
      if (B->pg->raw_len > 0 && stream_write((const unsigned char *)B->pg->raw, B->pg->raw_len) < 0)
        fail = 1;
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

/* Headers-ready hook (fired before any DATA): parse the response (status, enc,
 * redirect Location, cookies). */
static int h2_headers_cb(void *ctx, const char *block, int len)
{
  (void)len;
  struct h2tx *h = ctx;
  if (hdr_status(block) < 200) {
    early_collect(block);
    return 0;
  } /* 103 etc. */
  int chunked, have_len;
  unsigned clen;
  h->status = parse_response(block, h->host, h->full_url, h->loc, h->loc_cap, &chunked, &have_len, &clen, &h->enc);
  return 0;
}

/* Body sink: append the raw DATA payload to B->pg->raw (decoded once, after).
 */
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

/* Run the request over HTTP/2 (sock already open + ALPN-confirmed h2). Fills
 * B->pg->raw with the raw body; raw_decode + route_body finish it the common
 * way. */
static int h2_txn(const struct purl *u, const char *full_url, char *loc, int loc_cap, int allow_dl, const char *method,
                  const unsigned char *body, int blen, const char *bctype)
{
  char auth[URL_HOST_MAX + 8];
  int an = 0; /* :authority = host[:port] */
  sapp(auth, &an, sizeof auth, u->host);
  if (u->port != 443) {
    sapp(auth, &an, sizeof auth, ":");
    uapp(auth, &an, sizeof auth, (unsigned)u->port);
  }

  /* cookies + accept + accept-encoding. No Connection/Transfer-Encoding
   * (forbidden in HTTP/2). The User-Agent rides in h2_opts. A POST body carries
   * its own content-type header (h2 has no separate request line for it). */
  char xh[1408];
  int xn = 0;
  char ck[1024];
  if (cookie_header_tls(u->host, u->path, u->tls, ck, sizeof ck) > 0) {
    sapp(xh, &xn, sizeof xh, "cookie: ");
    sapp(xh, &xn, sizeof xh, ck);
    sapp(xh, &xn, sizeof xh, "\r\n");
  }
  sapp(xh, &xn, sizeof xh,
       "accept: " BR_ACCEPT "\r\naccept-language: " BR_ACCLNG "\r\n"
       "accept-encoding: gzip, deflate, br\r\n");
  {
    char ch[512];
    if (ch_request_headers(u->host, ch, sizeof ch) > 0)
      sapp(xh, &xn, sizeof xh, ch);
  }
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
  if (body && blen > 0)
    h2_run_body(B->sock, 1, auth, method, u->path, &opts, body, blen, hdrbuf, sizeof hdrbuf, &hlen, h2_body_cb, &h);
  else
    h2_run(B->sock, 1, auth, method, u->path, &opts, hdrbuf, sizeof hdrbuf, &hlen, h2_body_cb, &h);

  early_apply(full_url); /* merge any 103 Early-Hints stylesheets into ext_css */
  raw_decode(h.enc);
  return route_body(full_url, h.status > 0 ? h.status : -5, allow_dl);
}

/* ---- HTTP/3 transaction (QUIC; used for Alt-Svc-advertised origins) -------
 */

/* Body sink: append to B->pg->raw (decoded once, after — same as h2/h1.1). */
static int h3_body_cb(void *ctx, const unsigned char *p, int n)
{
  (void)ctx;
  if (B->abort_load)
    return 1; /* Stop: tell h3_run to bail */
  if (!browser_ensure_raw(B->pg->raw_len + n))
    return 1;
  for (int i = 0; i < n; i++)
    B->pg->raw[B->pg->raw_len++] = (char)p[i];
  return 0;
}

/* Run the request over HTTP/3. Mirrors http_txn's contract; returns the status,
 * or a negative HTTP_E* (the caller then falls back to TLS h2/h1.1). */
static int h3_txn(const struct purl *u, const char *full_url, char *loc, int loc_cap, int allow_dl)
{
  loc[0] = '\0';
  B->pg->ctype[0] = '\0';
  B->dl_name[0] = '\0';
  B->cdisp_attach = 0;
  B->pg->raw_len = 0;

  char auth[URL_HOST_MAX + 8];
  int an = 0;
  sapp(auth, &an, sizeof auth, u->host);
  if (u->port != 443) {
    sapp(auth, &an, sizeof auth, ":");
    uapp(auth, &an, sizeof auth, (unsigned)u->port);
  }

  char xh[1408];
  int xn = 0;
  sapp(xh, &xn, sizeof xh, "user-agent: ");
  sapp(xh, &xn, sizeof xh, browser_ua());
  sapp(xh, &xn, sizeof xh,
       "\r\naccept: " BR_ACCEPT "\r\naccept-language: " BR_ACCLNG "\r\naccept-encoding: gzip, deflate, br\r\n");
  {
    char ch[512];
    if (ch_request_headers(u->host, ch, sizeof ch) > 0)
      sapp(xh, &xn, sizeof xh, ch);
  }
  char ck[1024];
  if (cookie_header_tls(u->host, u->path, u->tls, ck, sizeof ck) > 0) {
    sapp(xh, &xn, sizeof xh, "cookie: ");
    sapp(xh, &xn, sizeof xh, ck);
    sapp(xh, &xn, sizeof xh, "\r\n");
  }
  if (g_doc_cond[0])
    sapp(xh, &xn, sizeof xh, g_doc_cond); /* revalidation */

  char hdrbuf[RESP_HDR_MAX];
  int hlen = 0;
  int st = h3_run(u->host, u->port, auth, "GET", u->path, xh, hdrbuf, sizeof hdrbuf, &hlen, h3_body_cb, 0);
  if (st <= 0)
    return st; /* QUIC failed -> caller falls back */

  int chunked, have_len, enc;
  unsigned clen;
  int status = parse_response(hdrbuf, u->host, full_url, loc, loc_cap, &chunked, &have_len, &clen, &enc);
  raw_decode(enc);
  return route_body(full_url, status, allow_dl);
}

/* Apply the HTTP cache to a finished document response (any transport): a
 * revalidation that came back 304 reuses the stored body (rendered as 200); a
 * fresh, non-download 200 is stored (already content-decoded, in B->pg->raw).
 * Returns the status do_fetch should act on. */
static int doc_cache_finish(const char *full_url, long now, int revalidating, int status)
{
  if (revalidating && status == 304) {
    unsigned char *rb = 0;
    int rl = 0;
    if (cache_revalidated(full_url, now, g_doc_hdr, &rb, &rl, B->pg->ctype, sizeof B->pg->ctype, 0, 0)) {
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
    cache_store("GET", full_url, 200, now, g_doc_hdr, B->pg->ctype, (const unsigned char *)B->pg->raw, B->pg->raw_len);
  return status;
}

static int http_txn(const struct purl *u, const char *full_url, char *loc, int loc_cap, int allow_dl,
                    const char *method, const unsigned char *body, int blen, const char *bctype)
{
  int is_get = streq(method, "GET");
  g_doc_t0 = msec(); /* for the devtools Network duration of this response */
  loc[0] = '\0';
  B->pg->ctype[0] = '\0';
  B->dl_name[0] = '\0';
  B->cdisp_attach = 0;
  B->rpos = B->rlen = B->reof = 0;
  B->pg->raw_len = 0;
  early_reset();

  /* HTTP cache (RFC 7234): a fresh copy of this document skips the wire; a
   * stale one with a validator revalidates via g_doc_cond and reuses the body
   * on 304. The body pipeline stores already-decoded bytes + Content-Type, so a
   * hit needs no re-decode. Downloads (route_body empties raw) are never
   * cached. */
  long now = (long)rtc_epoch();
  int revalidating = 0;
  g_doc_cond[0] = '\0';
  if (is_get) { /* only GET is cacheable / revalidatable */
    unsigned char *cbody = 0;
    int clen = 0;
    int cv = cache_lookup("GET", full_url, now, &cbody, &clen, B->pg->ctype, sizeof B->pg->ctype, g_doc_hdr,
                          sizeof g_doc_hdr, g_doc_cond, sizeof g_doc_cond);
    if (cv == CACHE_FRESH) {
      B->pg->raw_len = 0;
      if (browser_ensure_raw(clen)) {
        memcpy(B->pg->raw, cbody, (unsigned)clen);
        B->pg->raw_len = clen;
      }
      free(cbody);
      return 200;
    }
    revalidating = (cv == CACHE_REVALIDATE);
  }

  /* If this https origin previously advertised HTTP/3 (Alt-Svc), try QUIC
   * first; on any failure fall through to the TLS h2/h1.1 path below. HTTP/3
   * here has no request-body path, so a POST skips it straight to h2/h1.1. */
  if (is_get && u->tls && u->port == 443 && h3_known(u->host)) {
    int st = h3_txn(u, full_url, loc, loc_cap, allow_dl);
    if (st > 0)
      return doc_cache_finish(full_url, now, revalidating, st);
    loc[0] = '\0';
    B->pg->ctype[0] = '\0';
    B->dl_name[0] = '\0';
    B->cdisp_attach = 0;
    B->pg->raw_len = 0;
  }

  /* Over TLS, offer ALPN "h2"; if the server selects it, run the request over
   * HTTP/2 instead of the HTTP/1.1 code below. The netconn owns a user-space
   * TLS connection (or a plain TCP socket). */
  B->sock = nc_open(u->host, u->port, u->tls, /*offer_h2=*/1);
  if (!B->sock) {
    if (nc_last_error() == NC_OPEN_RESOLVE)
      return -2;
    if (nc_last_error() == NC_OPEN_TLS)
      return -6;
    return -3;
  }

  if (u->tls && nc_alpn(B->sock) == ALPN_H2) {
    int st = h2_txn(u, full_url, loc, loc_cap, allow_dl, method, body, blen, bctype);
    nc_close(B->sock);
    B->sock = 0;
    return doc_cache_finish(full_url, now, revalidating, st);
  }

  char req[2048];
  int n = 0;
  sapp(req, &n, sizeof req, method);
  sapp(req, &n, sizeof req, " ");
  sapp(req, &n, sizeof req, u->path);
  sapp(req, &n, sizeof req, " HTTP/1.1\r\nHost: ");
  sapp(req, &n, sizeof req, u->host);
  if (!((u->tls && u->port == 443) || (!u->tls && u->port == 80))) {
    sapp(req, &n, sizeof req, ":");
    uapp(req, &n, sizeof req, (unsigned)u->port);
  }
  {
    char ck[1024];
    if (cookie_header_tls(u->host, u->path, u->tls, ck, sizeof ck) > 0) {
      sapp(req, &n, sizeof req, "\r\nCookie: ");
      sapp(req, &n, sizeof req, ck);
    }
  }
  sapp(req, &n, sizeof req, "\r\nUser-Agent: ");
  sapp(req, &n, sizeof req, browser_ua());
  sapp(req, &n, sizeof req,
       "\r\nAccept: " BR_ACCEPT "\r\n"
       "Accept-Language: " BR_ACCLNG "\r\n"
       "Accept-Encoding: gzip, deflate, br\r\n");
  {
    char ch[512];
    if (ch_request_headers(u->host, ch, sizeof ch) > 0)
      sapp(req, &n, sizeof req, ch);
  }
  sapp(req, &n, sizeof req, "Connection: close\r\n");
  if (g_doc_cond[0])
    sapp(req, &n, sizeof req, g_doc_cond); /* revalidation */
  if (body && blen > 0) {                  /* POST/PUT body */
    sapp(req, &n, sizeof req, "Content-Type: ");
    sapp(req, &n, sizeof req, bctype ? bctype : "application/x-www-form-urlencoded");
    sapp(req, &n, sizeof req, "\r\nContent-Length: ");
    uapp(req, &n, sizeof req, (unsigned)blen);
    sapp(req, &n, sizeof req, "\r\n");
  }
  sapp(req, &n, sizeof req, "\r\n");

  if (nc_write(B->sock, req, n) < 0) {
    nc_close(B->sock);
    B->sock = 0;
    return -4;
  }
  if (body && blen > 0 && nc_write(B->sock, (const char *)body, blen) < 0) {
    nc_close(B->sock);
    B->sock = 0;
    return -4;
  }

  /* Read the response header block, skipping any 1xx interim response. A 103
   * Early Hints response carries Link: rel=stylesheet hints (stashed now,
   * merged after the final response is parsed); other 1xx are just skipped. */
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
      early_collect(hdr);
    if (++interim >= 16) {
      nc_close(B->sock);
      B->sock = 0;
      return -5;
    }
  }

  int chunked, have_len, enc_gz;
  unsigned content_len;
  int status = parse_response(hdr, u->host, full_url, loc, loc_cap, &chunked, &have_len, &content_len, &enc_gz);
  early_apply(full_url); /* merge any 103 Early-Hints stylesheets into ext_css */

  struct bodysrc bs = {chunked, have_len, 0, 0, have_len ? content_len : 0};

  /* Read the whole (framed) body into B->pg->raw, then the common pipeline
   * decodes Content-Encoding and routes render-vs-download. */
  int c;
  while ((c = body_src(&bs)) >= 0) {
    if (!browser_ensure_raw(B->pg->raw_len + 1))
      break; /* out of memory: stop */
    B->pg->raw[B->pg->raw_len++] = (char)c;
  }
  nc_close(B->sock);
  B->sock = 0;

  raw_decode(enc_gz);
  return doc_cache_finish(full_url, now, revalidating, route_body(full_url, status, allow_dl));
}

/* ----------------------------------------------- local & inline URL schemes */

/* Render whatever is in B->pg->raw (with B->pg->ctype already set) as the page
 * body — HTML, plain text, or a PNG. Shared by the network, file:, and data:
 * paths so all three pick the display mode the same way. HTML pulls external
 * stylesheets (via css_batch) before layout; other sub-resources are fetched by
 * do_fetch. */
static void render_raw(const char *url)
{
  /* HTML rendering normally also covers an empty/missing Content-Type (sloppy
   * servers). But X-Content-Type-Options: nosniff forbids that guess: with it
   * set, render as HTML ONLY when the server explicitly declared an HTML type,
   * otherwise fall through to plain text. */
  int as_html = is_html_type(B->pg->ctype) || (B->pg->ctype[0] == '\0' && !B->nosniff);
  if (as_html) {
    B->pg->mode = MODE_HTML;
    html_to_text(url, css_batch);
  } else if (starts_ci(B->pg->ctype, "image/png")) {
    B->pg->mode = MODE_IMAGE;
    B->pg->text_len = 0;
    B->pg->nlinks = 0;
    B->pg->doc_title[0] = '\0';
  } else if (starts_ci(B->pg->ctype, "image/")) {
    B->pg->mode = MODE_TEXT;
    doc_message("[unsupported image type - only PNG can be shown]");
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

/* Pick a Content-Type for a local file from its extension — only the
 * distinctions render_raw cares about (HTML vs PNG vs everything-else-as-text);
 * an unknown or missing extension renders verbatim as text. */
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

/* file:// — read a file from the local FS (owofs or a /media mount) into
 * B->pg->raw, streamed and grown on demand so a file of any size loads (bounded
 * only by memory). Accepts file:/path, file://host/path (host ignored),
 * file:///path; a trailing ?query or #fragment is dropped. Returns 0, or -1 if
 * it can't open. */
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

/* data:[<mediatype>][;base64],<data> — decode an inline document into
 * B->pg->raw. The media type sets B->pg->ctype so render_raw picks the display
 * mode. Uses the shared decode_data_url (resource.c) so the base64/percent
 * logic lives once, then copies the payload into the streaming raw buffer.
 * Returns 0, or -1 if malformed. */
static int load_data_url(const char *url)
{
  int len = 0;
  B->pg->ctype[0] = '\0';
  unsigned char *buf = decode_data_url(url, &len, B->pg->ctype, sizeof B->pg->ctype);
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
  /* Capture the page we're leaving as the referrer for the page we're about to
   * load (cur_url still holds the old URL until render_raw overwrites it). */
  {
    int k = 0;
    B->referrer[0] = '\0';
    sapp(B->referrer, &k, sizeof B->referrer, B->pg->cur_url);
  }
  /* Free the previous page's live DOM/CSS/JS before building the new one. This
   * covers every navigation (incl. plain-text/image/error paths that don't go
   * through html_to_text); html_to_text repeats it harmlessly for HTML pages.
   */
  html_page_teardown();
  if (B->pg->csp) {
    csp_free(B->pg->csp);
    B->pg->csp = 0;
  } /* drop the old page's CSP; parse_response/meta rebuild it */
  B->pg->perm_policy = 0; /* drop the old Permissions-Policy; parse_response rebuilds it */
  devtools_net_reset();   /* a new top-level navigation starts a fresh net log */
  sapp(url, &n, sizeof url, B->req_url);
  B->dl_saved = 0;
  B->scripts_pending = 0; /* only an HTML render (html_to_text) re-arms it */
  B->pg->favicon_ok = 0;  /* cleared per navigation; refilled on success */
  ext_css_clear();        /* Link: stylesheets refilled by parse_response  */

  /* Capture this navigation's HTTP method + body (a form POST fills them).
   * Point at B->req_body rather than copying to a big stack buffer — the bytes
   * stay put through the fetch, and render_raw's deep HTML parse runs on this
   * same frame. Clear req_method so the NEXT navigation defaults back to GET.
   */
  char method[8];
  strcpy(method, B->req_method[0] ? B->req_method : "GET");
  const unsigned char *body = (const unsigned char *)B->req_body;
  int blen = B->req_body_len;
  const char *bctype = B->req_ctype[0] ? B->req_ctype : 0;
  B->req_method[0] = '\0';
  B->req_body_len = 0;

  /* Local / inline schemes never touch the network: file:// reads the local
   * FS, data: decodes an embedded document. Both fill B->pg->raw + B->pg->ctype
   * and render through the same path as a fetched page (no sub-resource fetch).
   */
  if (starts_ci(url, "file:") || starts_ci(url, "data:")) {
    int data = starts_ci(url, "data:");
    B->pg->ctype[0] = '\0';
    B->dl_name[0] = '\0';
    B->cdisp_attach = 0;
    B->rpos = B->rlen = B->reof = 0;
    B->pg->n_imgs = 0;
    int rc = data ? load_data_url(url) : load_file_url(url);
    if (rc < 0) {
      const char *msg = data ? "Malformed data: URL" : "Cannot open file (not found or not absolute)";
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

  /* Auto-add a scheme when the user typed a bare host ("miwa.lol/..."): default
   * to https like a modern browser. Remember it was implicit so a host that
   * can't do TLS (or a plain dev server on a custom port) falls back to http.
   */
  int implicit_scheme = 0;
  if (!starts_ci(url, "http://") && !starts_ci(url, "https://")) {
    char tmp[URL_MAX];
    int t = 0;
    sapp(tmp, &t, sizeof tmp, "https://");
    sapp(tmp, &t, sizeof tmp, url);
    n = 0;
    sapp(url, &n, sizeof url, tmp);
    implicit_scheme = 1;
  }

  /* Log the document as in-flight up front, so a slow or failing navigation
   * shows in the Network panel while it loads — not only once (if) it
   * completes. Seed the duration clock here too (http_txn re-stamps it per hop)
   * so a bail before the first hop reports a sane elapsed time. */
  g_doc_t0 = msec();
  int docnid = devtools_net_begin(method, url);

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

    /* HSTS: a host that previously sent Strict-Transport-Security is only
     * ever contacted over HTTPS, even if the URL says http://. */
    if (!u.tls && hsts_has(u.host)) {
      u.tls = 1;
      if (u.port == 80)
        u.port = 443;
    }

    char loc[URL_MAX];
    int st = http_txn(&u, url, loc, sizeof loc, 1, method, body, blen, bctype);
    if (st < 0) {
      if (B->abort_load) { /* user stopped: no error page */
        struct net_log e = {method, url, 0, 0, 0, (int)(msec() - g_doc_t0), 0, NET_CANCELLED, 0};
        devtools_net_update(docnid, &e);
        return;
      }
      /* Secure connect failed for an auto-https URL (host has no TLS, or a
       * plain dev server) — retry the same URL over http once. */
      if (implicit_scheme && st == -3 && starts_ci(url, "https://")) {
        char tmp[URL_MAX];
        int t = 0;
        sapp(tmp, &t, sizeof tmp, "http://");
        sapp(tmp, &t, sizeof tmp, url + 8); /* past "https://" */
        n = 0;
        sapp(url, &n, sizeof url, tmp);
        implicit_scheme = 0;
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
    if ((st == 301 || st == 302 || st == 303 || st == 307 || st == 308) && loc[0]) {
      if (hop == MAX_REDIR) {
        show_error("Too many redirects");
        struct net_log e = {
            method, url, st, 0, 0, (int)(msec() - g_doc_t0), g_doc_hdr, NET_FAILED, "Too many redirects"};
        devtools_net_update(docnid, &e);
        return;
      }
      /* A POST redirected by 301/302/303 becomes a bodyless GET (browser
       * convention; 303 mandates it). 307/308 preserve the method + body. */
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

    /* Resolve the in-flight document row with the final response (after any
     * redirects; a streamed download has raw_len 0). */
    {
      struct net_log e = {
          method,    url,      st, B->pg->ctype, B->dl_saved ? 0 : B->pg->raw_len, (int)(msec() - g_doc_t0),
          g_doc_hdr, NET_DONE, 0};
      devtools_net_update(docnid, &e);
    }

    /* A download was streamed straight to ~/Downloads inside http_txn (it
     * never touched B->pg->raw), so there is nothing to render. */
    if (B->dl_saved)
      return;

    /* Render according to the response type. An empty Content-Type is
     * treated as HTML (the common case for sloppy servers); a plain text
     * type is shown verbatim so line breaks survive; image/png is decoded. */
    resource_set_page(url); /* attribute this page's sub-resources for CORP */
    render_raw(url);
    /* The title bar is updated by the main thread (only the window's owner
     * task may call win_settitle); it reads B->pg->doc_title / B->pg->cur_url.
     */

    /* HTML pages may carry inline <img>: the parallel fetchers (resource.c)
     * pull each over its own socket into img_pool — B->pg->raw is untouched.
     * Each landed image bumps B->seq so the UI re-lays-out as they arrive. */
    if (B->pg->mode == MODE_HTML && B->pg->n_imgs > 0 && !B->abort_load)
      fetch_inline_images(url);

    /* Pull the site favicon for the tab strip. Skipped for a directly-rendered
     * image page, whose pixels are in B->pg->raw (the favicon fetch uses its
     * own buffer, but the page is already the image, so there's nothing to
     * fetch). */
    if (B->pg->mode != MODE_IMAGE && !B->abort_load)
      fetch_favicon(url);

    /* Persist everything this navigation cached (document + stylesheets +
     * images + favicon). Deferred to here — the single worker thread, with the
     * sub-resource pools already joined — because owofs allows one open write
     * stream at a time. */
    cache_flush();
    return;
  }
}
