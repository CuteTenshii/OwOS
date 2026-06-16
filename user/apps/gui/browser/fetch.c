/* fetch.c — the browser's main page transaction: the buffered reader, response-
 * header parsing, the HTTP/1.1 + HTTP/2 fetch, downloads (an attachment response
 * streamed to disk instead of B->raw — kept here because it shares this file's
 * body reader), the redirect follower, the file:/data: schemes, and do_fetch.
 * Separately-fetched sub-resources (inline images, favicon, external stylesheets)
 * live in resource.c; the cookie/HSTS jar in jar.c. Runs on the worker thread
 * (main.c), so its blocking reads never stall the UI. */
#include "browser.h"
#include "fetchint.h"  /* shared transport interface + BR_* request headers   */
#include "headers.h"   /* libcurl: HTTP response header parsing               */
#include "brotli.h"    /* RFC 7932 `br` content-coding (whole-buffer decode)   */

/* Content-encoding of a response body. */
enum { ENC_NONE = 0, ENC_ZLIB = 1, ENC_BR = 2 };

/* Brotli equivalent of inflate_http, with the same pull/push contract: pull the
 * whole compressed body through `rd` (brotli has no streaming window — the
 * stream doesn't carry its decompressed size), decompress it, and push the
 * result through `wr`. Returns 0 on success or caller early-stop, -1 on a
 * malformed stream or OOM. */
static int brotli_http(inf_reader rd, void *rctx, inf_writer wr, void *wctx)
{
	unsigned char *buf = 0;
	int cap = 0, len = 0, c;
	while ((c = rd(rctx)) >= 0) {
		if (len + 1 > cap) {
			int ncap = cap * 2 + 8192;
			unsigned char *nb = realloc(buf, (unsigned)ncap);
			if (!nb) { free(buf); return -1; }
			buf = nb; cap = ncap;
		}
		buf[len++] = (unsigned char)c;
	}
	int outlen = 0;
	unsigned char *out = brotli_decompress(buf, len, &outlen);
	free(buf);
	if (!out) return -1;
	wr(wctx, out, (unsigned)outlen);        /* one push; a <0 (full) is a clean stop */
	free(out);
	return 0;
}

/* Decompress an in-memory body per its content-encoding (ENC_BR / ENC_ZLIB).
 * Returns a malloc'd buffer of *out_len bytes (caller frees), or NULL. Used by
 * the HTTP/2 path, whose body arrives already buffered (pushed in DATA chunks)
 * rather than pulled. */
struct memrd { const unsigned char *p; int len, pos; };
static int memrd_fn(void *c) { struct memrd *m = c; return m->pos < m->len ? m->p[m->pos++] : -1; }
struct memwr { unsigned char *buf; int cap, len, oom; };
static int memwr_fn(void *c, const unsigned char *p, unsigned n)
{
	struct memwr *w = c;
	if (w->len + (int)n > w->cap) {
		int nc = (w->len + (int)n) * 2 + 4096;
		unsigned char *nb = realloc(w->buf, (unsigned)nc);
		if (!nb) { w->oom = 1; return -1; }
		w->buf = nb; w->cap = nc;
	}
	for (unsigned i = 0; i < n; i++) w->buf[w->len++] = p[i];
	return 0;
}
static unsigned char *body_decompress(const unsigned char *in, int in_len, int enc, int *out_len)
{
	if (enc == ENC_BR) return brotli_decompress(in, in_len, out_len);
	struct memrd r = { in, in_len, 0 };
	struct memwr w = { 0, 0, 0, 0 };
	if (inflate_http(memrd_fn, &r, memwr_fn, &w) < 0 || w.oom) { free(w.buf); return 0; }
	*out_len = w.len;
	return w.buf ? w.buf : malloc(1);
}

static int rgetc(void)
{
	if (B->rpos >= B->rlen) {
		if (B->reof) return -1;
		int n = net_recv(B->sock, B->rbuf, (int)sizeof B->rbuf);
		if (n < 0) return -2;
		if (n == 0) { B->reof = 1; return -1; }
		B->rpos = 0; B->rlen = n;
	}
	return B->rbuf[B->rpos++];
}

/* Show a one-line message as the page body. The browser has no status bar, so
 * errors (and other notices that used to live there) are rendered straight into
 * the content pane. Clears cur_url so the address bar keeps whatever the user
 * typed, ready to edit and retry. */
void show_error(const char *m)
{
	B->mode = MODE_TEXT;
	B->scroll = 0;
	B->cur_url[0] = '\0';
	doc_message(m);
	B->seq++;
}

/* Case-insensitive substring test (used to spot "html" inside a Content-Type).
 * streq_ci lives in dom.h (via browser.h); the cookie/HSTS jar lives in jar.c. */
static int has_sub(const char *s, const char *sub)
{
	for (; *s; s++) {
		const char *a = s, *b = sub;
		while (*b && lower(*a) == lower(*b)) { a++; b++; }
		if (!*b) return 1;
	}
	return 0;
}

/* --------------------------------------------------- body byte source/sink */

/* Yields the next *content* byte of the response body, transparently undoing
 * the transfer framing (chunked sizes, Content-Length limit, or read-to-close)
 * — so whatever consumes it (a plain copy, or the inflate decoder) sees only
 * the payload. Returns the byte (0..255) or -1 at end of body. */
struct bodysrc {
	int      chunked, have_len, done, need_crlf;
	unsigned remaining;     /* bytes left in this chunk / the content-length   */
};

static int body_src(void *ctx)
{
	struct bodysrc *b = ctx;
	if (b->done)
		return -1;
	if (b->chunked) {
		while (b->remaining == 0) {
			if (b->need_crlf) {            /* CRLF trailing the prev chunk    */
				int c = rgetc();
				if (c == '\r') rgetc();    /* the \n                          */
				else if (c < 0) { b->done = 1; return -1; }
				b->need_crlf = 0;
			}
			unsigned sz = 0; int c, any = 0;
			while ((c = rgetc()) >= 0 && c != '\r' && c != ';') {
				int d;
				if      (c >= '0' && c <= '9') d = c - '0';
				else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
				else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
				else break;
				sz = sz * 16 + (unsigned)d; any = 1;
			}
			while (c >= 0 && c != '\n') c = rgetc();
			if (!any || sz == 0) { b->done = 1; return -1; }
			b->remaining = sz; b->need_crlf = 1;
		}
		int c = rgetc();
		if (c < 0) { b->done = 1; return -1; }
		b->remaining--;
		return c;
	}
	if (b->have_len) {
		if (b->remaining == 0) { b->done = 1; return -1; }
		int c = rgetc();
		if (c < 0) { b->done = 1; return -1; }
		b->remaining--;
		return c;
	}
	int c = rgetc();
	if (c < 0) { b->done = 1; return -1; }
	return c;
}

/* Appends decompressed (or copied) bytes to the page body buffer, growing it as
 * needed. Only an exhausted heap stops it (return <0), which makes the streaming
 * inflater halt cleanly rather than failing outright — no fixed size cap. */
static int raw_sink(void *ctx, const unsigned char *p, unsigned n)
{
	(void)ctx;
	if (!ensure_raw(B->raw_len + (int)n))
		return -1;
	for (unsigned i = 0; i < n; i++)
		B->raw[B->raw_len++] = (char)p[i];
	return 0;
}

/* Bulk counterpart to body_src for the uncompressed download path: copies whole
 * runs straight out of the socket buffer rather than a byte at a time, so a
 * multi-megabyte file isn't shuttled through one function call per byte. Returns
 * the number of body bytes placed in out[] (0 at end of body). The per-byte
 * body_src is reused only to step across chunk-size headers (once per chunk). */
static int body_fill(struct bodysrc *b, unsigned char *out, int max)
{
	int got = 0;
	while (got < max && !b->done) {
		if (b->chunked && b->remaining == 0) {
			int c = body_src(b);            /* crosses the chunk header */
			if (c < 0) break;
			out[got++] = (unsigned char)c;
			continue;
		}
		if (b->have_len && b->remaining == 0) { b->done = 1; break; }
		if (B->rpos >= B->rlen) {               /* refill from the socket */
			if (B->reof) { if (!b->chunked && !b->have_len) b->done = 1; break; }
			int rn = net_recv(B->sock, B->rbuf, (int)sizeof B->rbuf);
			if (rn < 0) { b->done = 1; break; }
			if (rn == 0) {
				B->reof = 1;
				if (!b->chunked && !b->have_len) b->done = 1;
				break;
			}
			B->rpos = 0; B->rlen = rn;
		}
		int avail = B->rlen - B->rpos;
		if (avail > max - got) avail = max - got;
		if ((b->chunked || b->have_len) && (unsigned)avail > b->remaining)
			avail = (int)b->remaining;
		for (int i = 0; i < avail; i++) out[got + i] = B->rbuf[B->rpos + i];
		B->rpos += avail;
		got += avail;
		if (b->chunked || b->have_len) b->remaining -= (unsigned)avail;
	}
	return got;
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
 * Content-Type with no telltale extension is treated as HTML, the common case. */
static int url_is_download_ext(const char *url);
static int is_download(const char *url)
{
	if (B->cdisp_attach) return 1;
	if (url_is_download_ext(url)) return 1;
	if (B->ctype[0] == '\0') return 0;
	if (has_sub(B->ctype, "html")) return 0;
	if (starts_ci(B->ctype, "text/")) return 0;
	if (starts_ci(B->ctype, "image/")) return 0;
	return 1;
}

/* Copy the last path component of `in` into `out`, dropping any directory
 * prefix and a trailing "?query" — so "/files/a/b.zip?x=1" -> "b.zip". */
static void basename_of(const char *in, char *out, int cap)
{
	const char *last = in;
	for (const char *p = in; *p; p++)
		if (*p == '/' || *p == '\\') last = p + 1;
	int k = 0;
	for (; last[k] && last[k] != '?' && k < cap - 1; k++) out[k] = last[k];
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
	if (!parse_url(url, &u)) return 0;
	char base[128];
	basename_of(u.path, base, sizeof base);
	const char *ext = 0;
	for (const char *q = base; *q; q++) if (*q == '.') ext = q + 1;
	if (!ext || !*ext) return 0;
	static const char *const bin_ext[] = {
		"mp3", "wav", "flac", "ogg", "oga", "opus", "m4a", "aac", "wma", "mid",
		"mp4", "mkv", "webm", "avi", "mov", "wmv", "flv", "m4v", "mpg", "mpeg",
		"zip", "gz", "tgz", "bz2", "xz", "7z", "rar", "tar", "iso", "img",
		"pdf", "doc", "docx", "xls", "xlsx", "ppt", "pptx", "odt", "epub",
		"exe", "dll", "bin", "dmg", "deb", "rpm", "apk", "msi", "so",
		"ttf", "otf", "woff", "woff2",
		0
	};
	for (int i = 0; bin_ext[i]; i++)
		if (streq_ci(ext, bin_ext[i])) return 1;
	return 0;
}

/* Choose a filename for the download: the Content-Disposition name if any, else
 * the URL's last path segment, else a generic fallback. */
static void derive_filename(const char *url, char *out, int cap)
{
	if (B->dl_name[0]) {
		basename_of(B->dl_name, out, cap);
		if (out[0]) return;
	}
	struct purl u;
	if (parse_url(url, &u)) {
		basename_of(u.path, out, cap);
		if (out[0]) return;
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
	for (int i = 0; base[i]; i++) if (base[i] == '.') dot = i;
	int stem_len = (dot > 0) ? dot : (int)strlen(base);
	const char *ext = (dot > 0) ? base + dot : "";
	int ext_len = (int)strlen(ext);

	/* Collision suffix "-N" (empty for the first attempt). */
	char sfx[8]; int sl = 0;
	if (n > 0) { sfx[sl++] = '-'; uapp(sfx, &sl, (int)sizeof sfx, (unsigned)n); }
	sfx[sl] = '\0';

	/* A pathologically long extension can't fit either; cap it, leaving room
	 * for the suffix and at least one stem char. */
	if (ext_len > DL_LEAF_MAX - sl - 1) {
		ext_len = DL_LEAF_MAX - sl - 1;
		if (ext_len < 0) ext_len = 0;
	}
	/* Truncate the stem so stem + suffix + ext fits the component limit. */
	int budget = DL_LEAF_MAX - sl - ext_len;
	if (budget < 1) budget = 1;
	if (stem_len > budget) stem_len = budget;

	int p = 0;
	sapp(out, &p, cap, DL_DIR "/");
	for (int i = 0; i < stem_len && p < cap - 1; i++) out[p++] = base[i];
	out[p] = '\0';
	sapp(out, &p, cap, sfx);
	for (int i = 0; i < ext_len && p < cap - 1; i++) out[p++] = ext[i];
	out[p] = '\0';
}

/* inflate sink: push decompressed bytes straight to the open stream, halting the
 * inflater (return -1) the moment a disk write fails. */
struct dlsink { int fail; };
static int file_sink(void *ctx, const unsigned char *p, unsigned n)
{
	struct dlsink *d = ctx;
	if (stream_write(p, (int)n) < 0) { d->fail = 1; return -1; }
	return 0;
}

/* Pick a non-clobbering DL_DIR/<name> path for a download (the saved name comes
 * from Content-Disposition or the URL; a repeat gets a "-N" suffix). Ensures
 * DL_DIR exists first: the boot image creates it, but a disk.img persisted from
 * an older build may not have it, and a missing directory makes stream_begin
 * fail (which the h2 path used to mishandle by dumping the body into the page). */
static void pick_dl_path(const char *url, char *path, int cap)
{
	mkdir(DL_DIR);                                  /* no-op if it already exists */
	char base[128];
	derive_filename(url, base, sizeof base);
	char probe[1];
	for (int n = 0; n < 100; n++) {
		build_path(base, n, path, cap);
		if (readfile(path, probe, 0) < 0) break;   /* first free name           */
	}
}

/* Report a finished download: switch to a text pane on `url` and show the saved
 * path (and byte count) or a failure notice. Shared by the 1.1 and h2 paths. */
static void download_finished(const char *url, const char *path, int wrote, int fail)
{
	B->mode = MODE_TEXT;
	{ int ci = 0; sapp(B->cur_url, &ci, sizeof B->cur_url, url); }
	B->scroll = 0;

	char msg[256]; int m = 0;
	if (!fail && wrote >= 0) {
		sapp(msg, &m, sizeof msg, "Saved to ");
		sapp(msg, &m, sizeof msg, path);
		sapp(msg, &m, sizeof msg, "  (");
		uapp(msg, &m, sizeof msg, (unsigned)wrote);
		sapp(msg, &m, sizeof msg, " bytes)");
	} else {
		sapp(msg, &m, sizeof msg,
		     "Download failed (disk full or cannot write to " DL_DIR ")");
	}
	doc_message(msg);
	B->dl_saved = 1;
	B->seq++;
}

/* Stream the (HTTP/1.1) response body straight to ~/Downloads rather than
 * buffering it, so a download is bounded only by the disk, never by RAW_MAX;
 * inflates on the fly when the body is compressed. */
static void stream_download(const char *url, struct bodysrc *bs, int enc_gz)
{
	char path[256];
	pick_dl_path(url, path, sizeof path);

	struct dlsink d = { 0 };
	int wrote = -1;
	if (stream_begin(path) == 0) {
		if (enc_gz == ENC_BR) {
			brotli_http(body_src, bs, file_sink, &d);
		} else if (enc_gz) {
			inflate_http(body_src, bs, file_sink, &d);
		} else {
			/* Bulk-read into a page-sized buffer, then one stream_write per fill. */
			unsigned char buf[4096]; int got;
			while ((got = body_fill(bs, buf, (int)sizeof buf)) > 0)
				if (stream_write(buf, got) < 0) { d.fail = 1; break; }
		}
		wrote = stream_end();   /* commits + returns the size; -1 after a failed write */
	}
	download_finished(url, path, wrote, d.fail);
}

/* ----------------------------------------------------------- transaction */

/* External stylesheet URLs from the Link: response header (heap list in B->
 * ext_css). Reset each response, consumed by html_to_text before layout. */
static void ext_css_clear(void)
{
	struct css_link *c = B->ext_css;
	while (c) { struct css_link *n = c->next; free(c->url); free(c); c = n; }
	B->ext_css = 0;
}

static void ext_css_add(const char *url)
{
	struct css_link *c = malloc(sizeof *c);
	if (!c) return;
	int len = strlen(url);
	c->url = malloc((unsigned)len + 1);
	if (!c->url) { free(c); return; }
	for (int i = 0; i <= len; i++) c->url[i] = url[i];
	c->next = B->ext_css;
	B->ext_css = c;
}

/* True if "stylesheet" appears as a token of the rel value within [q,end). */
static int rel_has_stylesheet(const char *q, const char *end)
{
	for (const char *s = q; s + 10 <= end; s++)
		if (starts_ci(s, "stylesheet")) return 1;
	return 0;
}

/* Parse one Link: header value (RFC 8288): "<url>; rel=stylesheet, <url2>; …".
 * For each entry whose rel includes "stylesheet", resolve its URL against `base`
 * and add it to B->ext_css. Each entry's params are scanned only up to the next
 * top-level comma, so one entry's rel can't leak into another's. */
static void parse_link_header(const char *v, const char *base)
{
	while (*v) {
		while (*v == ' ' || *v == ',' || *v == '\t') v++;
		if (!*v) break;

		const char *entry = v, *eend = v;        /* this entry: [entry, eend) */
		while (*eend && *eend != ',') eend++;

		char url[URL_MAX]; url[0] = '\0';
		const char *p = entry;
		if (*p == '<') {
			p++;
			int k = 0;
			while (p < eend && *p != '>' && k < (int)sizeof url - 1) url[k++] = *p++;
			url[k] = '\0';
			if (p < eend && *p == '>') p++;
		}

		int is_sheet = 0;                        /* scan params (after the URL) */
		for (const char *q = p; q < eend; q++) {
			if ((q == p || q[-1] == ';' || q[-1] == ' ') && starts_ci(q, "rel")) {
				const char *r = q + 3;
				while (r < eend && *r == ' ') r++;
				if (r < eend && *r == '=') {
					r++;
					while (r < eend && (*r == ' ' || *r == '"')) r++;
					const char *rv = r;
					while (rv < eend && *rv != ';') rv++;
					if (rel_has_stylesheet(r, rv)) is_sheet = 1;
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

/* Parse a response header block (an HTTP/1.1 block, or the HTTP/2-synthesized
 * "HTTP/x NNN\r\nname: value\r\n…" equivalent) into the browser state: sets
 * B->ctype / B->dl_name / B->cdisp_attach, stores cookies for `host`, notes
 * HSTS, collects Link: stylesheets, resolves any Location into `loc`, and reports
 * the body framing (chunked / content-length / gzip — moot over h2, which sends
 * none). Returns the status code. Shared by the HTTP/1.1 and HTTP/2 fetch paths.*/
static int parse_response(const char *hdr, const char *host, const char *full_url,
                          char *loc, int loc_cap, int *chunked, int *have_len,
                          unsigned *content_len, int *enc_gz)
{
	*chunked = 0; *have_len = 0; *enc_gz = 0; *content_len = 0;
	B->nosniff = false;
	ext_css_clear();        /* this response's Link: stylesheets replace the last */

	/* Parse the raw block once into a k/v object, then just query it. */
	struct http_headers H;
	http_headers_parse(&H, hdr);
	int status = H.status;

	const char *te = http_headers_get(&H, "transfer-encoding");
	if (te && http_header_has_token(te, "chunked")) *chunked = 1;

	const char *ce = http_headers_get(&H, "content-encoding");
	if (ce && http_header_has_token(ce, "br"))
		*enc_gz = ENC_BR;
	else if (ce && (http_header_has_token(ce, "gzip") || http_header_has_token(ce, "deflate")))
		*enc_gz = ENC_ZLIB;

	const char *cl = http_headers_get(&H, "content-length");
	if (cl) { bool ok; unsigned x = http_parse_uint(cl, &ok); if (ok) { *content_len = x; *have_len = 1; } }

	const char *loc_v = http_headers_get(&H, "location");
	if (loc_v) resolve_url(full_url, loc_v, loc, loc_cap);

	const char *ct = http_headers_get(&H, "content-type");
	if (ct) {
		int k = 0;
		for (const char *v = ct; *v && *v != ';' && k < (int)sizeof B->ctype - 1; v++)
			B->ctype[k++] = lower(*v);
		B->ctype[k] = '\0';
	}
	html_set_charset(ct);   /* charset param drives html.c transcoding (NULL = reset) */

	const char *cd = http_headers_get(&H, "content-disposition");
	if (cd) {
		if (http_header_has_token(cd, "attachment")) B->cdisp_attach = 1;
		for (const char *q = cd; *q; q++) {
			if (!starts_ci(q, "filename")) continue;
			const char *f = q + 8;
			if (*f == '*') continue;                /* skip the filename*= form */
			while (*f == ' ') f++;
			if (*f != '=') continue;
			f++;
			while (*f == ' ') f++;
			int quoted = (*f == '"'); if (quoted) f++;
			int k = 0;
			while (*f && k < (int)sizeof B->dl_name - 1) {
				if (quoted ? (*f == '"') : (*f == ';' || *f == ' ')) break;
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

	if (http_headers_get(&H, "strict-transport-security")) hsts_add(host);

	/* Link: <a.css>; rel=stylesheet — external sheets declared in the header. */
	int nlink = http_headers_count(&H, "link");
	for (int i = 0; i < nlink; i++)
		parse_link_header(http_headers_at(&H, "link", i), full_url);

	/* nosniff: the server forbids MIME-sniffing — honour the declared
	 * Content-Type and never guess the body's type (see render_raw). */
	const char *xcto = http_headers_get(&H, "x-content-type-options");
	if (xcto && http_header_has_token(xcto, "nosniff")) B->nosniff = true;

	http_headers_free(&H);
	jar_flush();   /* persist cookies/HSTS once, if this response changed them */
	return status;
}

/* ---- HTTP/2 transaction (used when ALPN negotiates h2 over TLS) --------- */

/* Shared state for the two h2 callbacks below and the download finalizer.
 *   is_dl    the response is a download — its body is never rendered.
 *   dl_open  the destination file opened, so body bytes go to disk.
 * Keeping these separate is load-bearing: a download whose file couldn't be
 * opened still must NOT fall through to B->raw (which would dump the binary into
 * the page as text); its body is discarded and the failure reported. */
struct h2tx {
	const char *full_url, *host;
	char *loc; int loc_cap;
	int  allow_dl;
	int  status;
	int  is_dl, dl_open, dl_started, dl_fail;
	int  enc;                         /* content-encoding (ENC_*) of the body    */
	unsigned char *body; int blen, bcap;   /* buffered compressed body (enc != 0) */
	char dl_path[256];
};

/* Append a DATA chunk to the h2 transaction's compressed-body buffer. */
static int h2buf(struct h2tx *h, const unsigned char *p, int n)
{
	if (h->blen + n > h->bcap) {
		int nc = (h->blen + n) * 2 + 4096;
		unsigned char *nb = realloc(h->body, (unsigned)nc);
		if (!nb) return -1;
		h->body = nb; h->bcap = nc;
	}
	for (int i = 0; i < n; i++) h->body[h->blen++] = p[i];
	return 0;
}

/* Headers-ready hook (fired before any DATA): parse the response, and if it is
 * a download, open the destination file now so the body streams to disk. */
static int h2_headers_cb(void *ctx, const char *block, int len)
{
	(void)len;
	struct h2tx *h = ctx;
	int chunked, have_len, enc_gz; unsigned clen;
	h->status = parse_response(block, h->host, h->full_url, h->loc, h->loc_cap,
	                           &chunked, &have_len, &clen, &enc_gz);
	h->enc = enc_gz;
	if (h->allow_dl && h->status >= 200 && h->status < 300 && is_download(h->full_url)) {
		h->is_dl = 1; h->dl_started = 1;
		pick_dl_path(h->full_url, h->dl_path, sizeof h->dl_path);
		if (stream_begin(h->dl_path) == 0) h->dl_open = 1;
		else                               h->dl_fail = 1;
	}
	return 0;
}

/* Body sink: stream to the download file (discarding if it failed to open — a
 * download is never rendered), or accumulate a renderable body into B->raw
 * (grown on demand, no size cap). Returns non-zero to stop the transfer. */
static int h2_body_cb(void *ctx, const unsigned char *p, int n)
{
	struct h2tx *h = ctx;
	/* Compressed body (gzip/deflate/br): buffer it; decompressed at transfer end
	 * since brotli has no streaming window and h2 DATA arrives in chunks. */
	if (h->enc) return h2buf(h, p, n) < 0 ? 1 : 0;
	if (h->is_dl) {
		if (h->dl_open && stream_write(p, n) < 0) { h->dl_fail = 1; return 1; }
		return 0;
	}
	if (!ensure_raw(B->raw_len + n)) return 1;      /* out of memory: stop      */
	for (int i = 0; i < n; i++)
		B->raw[B->raw_len++] = (char)p[i];
	return 0;
}

/* Run the request over HTTP/2 (sock already open + ALPN-confirmed h2). Mirrors
 * http_txn's contract: fills B->raw / streams a download, resolves redirects
 * into `loc`, and returns the status (or -5 if the transfer failed). */
static int h2_txn(const struct purl *u, const char *full_url, char *loc,
                  int loc_cap, int allow_dl)
{
	char auth[URL_HOST_MAX + 8]; int an = 0;        /* :authority = host[:port] */
	sapp(auth, &an, sizeof auth, u->host);
	if (u->port != 443) {
		sapp(auth, &an, sizeof auth, ":");
		uapp(auth, &an, sizeof auth, (unsigned)u->port);
	}

	/* Request headers: cookies + accept + accept-encoding (gzip/deflate/br — the
	 * body is buffered and decompressed below). No Connection/Transfer-Encoding
	 * (forbidden in HTTP/2). The User-Agent rides in h2_opts. */
	char xh[1408]; int xn = 0;
	char ck[1024];
	if (cookie_header(u->host, ck, sizeof ck) > 0) {
		sapp(xh, &xn, sizeof xh, "cookie: ");
		sapp(xh, &xn, sizeof xh, ck);
		sapp(xh, &xn, sizeof xh, "\r\n");
	}
	sapp(xh, &xn, sizeof xh,
	     "accept: " BR_ACCEPT "\r\naccept-language: " BR_ACCLNG "\r\n"
	     "accept-encoding: gzip, deflate, br\r\n");

	struct h2tx h = { full_url, u->host, loc, loc_cap, allow_dl, 0, 0, 0, 0, 0, 0, 0, 0, 0, {0} };
	struct h2_opts opts = { BR_UA, xh, h2_headers_cb };
	char hdrbuf[4096]; int hlen = 0;
	h2_run(B->sock, 1, auth, "GET", u->path, &opts,
	       hdrbuf, sizeof hdrbuf, &hlen, h2_body_cb, &h);

	/* Decompress the buffered body (if any) into its destination. */
	if (h.enc && h.blen > 0) {
		int ol = 0;
		unsigned char *o = body_decompress(h.body, h.blen, h.enc, &ol);
		if (!o) {
			if (h.is_dl) h.dl_fail = 1;
		} else if (h.is_dl) {
			if (h.dl_open && stream_write(o, ol) < 0) h.dl_fail = 1;
			free(o);
		} else {
			B->raw_len = 0;
			if (ensure_raw(ol)) { memcpy(B->raw, o, ol); B->raw_len = ol; }
			free(o);
		}
	}
	free(h.body);

	if (h.dl_started)                               /* finish a streamed download */
		download_finished(full_url, h.dl_path,
		                  h.dl_open ? stream_end() : -1, h.dl_fail || !h.dl_open);

	return h.status > 0 ? h.status : -5;
}

static int http_txn(const struct purl *u, const char *full_url, char *loc,
                    int loc_cap, int allow_dl)
{
	loc[0] = '\0';
	B->ctype[0] = '\0';
	B->dl_name[0] = '\0';
	B->cdisp_attach = 0;
	B->rpos = B->rlen = B->reof = 0;
	B->raw_len = 0;

	/* Over TLS, offer ALPN "h2" (SK_TLS_H2); if the server selects it, run the
	 * request over HTTP/2 instead of the HTTP/1.1 code below. */
	B->sock = net_connect(u->host, u->port, u->tls ? SK_TLS_H2 : SK_TCP);
	if (B->sock < 0) return B->sock;

	if (u->tls && net_alpn(B->sock) == ALPN_H2) {
		int st = h2_txn(u, full_url, loc, loc_cap, allow_dl);
		net_close(B->sock);
		return st;
	}

	char req[2048]; int n = 0;
	sapp(req, &n, sizeof req, "GET ");
	sapp(req, &n, sizeof req, u->path);
	sapp(req, &n, sizeof req, " HTTP/1.1\r\nHost: ");
	sapp(req, &n, sizeof req, u->host);
	if (!((u->tls && u->port == 443) || (!u->tls && u->port == 80))) {
		sapp(req, &n, sizeof req, ":");
		uapp(req, &n, sizeof req, (unsigned)u->port);
	}
	{
		char ck[1024];
		if (cookie_header(u->host, ck, sizeof ck) > 0) {
			sapp(req, &n, sizeof req, "\r\nCookie: ");
			sapp(req, &n, sizeof req, ck);
		}
	}
	sapp(req, &n, sizeof req,
	     "\r\nUser-Agent: " BR_UA "\r\nAccept: " BR_ACCEPT "\r\n"
	     "Accept-Language: " BR_ACCLNG "\r\n"
	     "Accept-Encoding: gzip, deflate, br\r\n"
	     "Connection: close\r\n\r\n");

	if (net_send(B->sock, req, n) < 0) { net_close(B->sock); return -4; }

	char hdr[4096]; int hlen = 0, match = 0;
	for (;;) {
		int ci = rgetc();
		if (ci < 0) { net_close(B->sock); return -5; }
		char c = (char)ci;
		if (hlen < (int)sizeof hdr - 1) hdr[hlen++] = c;
		if      (c == '\r') match = (match == 2) ? 3 : 1;
		else if (c == '\n') match = (match == 1) ? 2 : (match == 3 ? 4 : 0);
		else                match = 0;
		if (match == 4) break;
	}
	hdr[hlen] = '\0';

	int chunked, have_len, enc_gz; unsigned content_len;
	int status = parse_response(hdr, u->host, full_url, loc, loc_cap,
	                            &chunked, &have_len, &content_len, &enc_gz);

	struct bodysrc bs = { chunked, have_len, 0, 0,
	                      have_len ? content_len : 0 };

	/* A download (attachment, or a non-renderable Content-Type) on a successful
	 * response streams straight to ~/Downloads — decided here, before the body is
	 * read, so binary content never has to fit in B->raw. Redirects (3xx) and
	 * sub-resource fetches (allow_dl == 0) take the buffered path below. */
	if (allow_dl && status >= 200 && status < 300 && is_download(full_url)) {
		stream_download(full_url, &bs, enc_gz);
		net_close(B->sock);
		return status;
	}

	if (enc_gz == ENC_BR) {
		/* Brotli body: buffered whole then decompressed (no streaming window). */
		brotli_http(body_src, &bs, raw_sink, 0);
	} else if (enc_gz) {
		/* Compressed body: stream it through the inflater, which pulls framed
		 * body bytes via body_src and pushes decompressed bytes into raw_sink.
		 * Only the 32 KiB DEFLATE window lives in RAM, whatever the page size. */
		inflate_http(body_src, &bs, raw_sink, 0);
	} else {
		int c;
		while ((c = body_src(&bs)) >= 0) {
			if (!ensure_raw(B->raw_len + 1)) break;   /* out of memory: stop */
			B->raw[B->raw_len++] = (char)c;
		}
	}

	net_close(B->sock);
	return status;
}

/* ----------------------------------------------- local & inline URL schemes */

static int hexval(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static int b64val(int c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}


/* Render whatever is in B->raw (with B->ctype already set) as the page body —
 * HTML, plain text, or a PNG. Shared by the network, file:, and data: paths so
 * all three pick the display mode the same way. HTML pulls external stylesheets
 * (via css_batch) before layout; other sub-resources are fetched by do_fetch. */
static void render_raw(const char *url)
{
	/* HTML rendering normally also covers an empty/missing Content-Type (sloppy
	 * servers). But X-Content-Type-Options: nosniff forbids that guess: with it
	 * set, render as HTML ONLY when the server explicitly declared an HTML type,
	 * otherwise fall through to plain text. */
	int as_html = has_sub(B->ctype, "html") ||
	              (B->ctype[0] == '\0' && !B->nosniff);
	if (as_html) {
		B->mode = MODE_HTML; html_to_text(url, css_batch);
	} else if (starts_ci(B->ctype, "image/png")) {
		B->mode = MODE_IMAGE;
		B->text_len = 0; B->nlinks = 0; B->doc_title[0] = '\0';
	} else if (starts_ci(B->ctype, "image/")) {
		B->mode = MODE_TEXT;
		doc_message("[unsupported image type - only PNG can be shown]");
	} else {
		B->mode = MODE_TEXT; plain_to_text();
	}
	B->scroll = 0;
	{ int ci = 0; sapp(B->cur_url, &ci, sizeof B->cur_url, url); }
}

/* Pick a Content-Type for a local file from its extension — only the
 * distinctions render_raw cares about (HTML vs PNG vs everything-else-as-text);
 * an unknown or missing extension renders verbatim as text. */
static void ctype_from_ext(const char *path)
{
	const char *ext = 0;
	for (const char *q = path; *q; q++) {
		if (*q == '/' || *q == '\\') ext = 0;
		else if (*q == '.')          ext = q + 1;
	}
	const char *t = "text/plain";
	if (ext) {
		if      (streq_ci(ext, "html") || streq_ci(ext, "htm")) t = "text/html";
		else if (streq_ci(ext, "png"))                          t = "image/png";
	}
	int k = 0; sapp(B->ctype, &k, sizeof B->ctype, t);
}

/* file:// — read a file from the local FS (owofs or a /media mount) into B->raw,
 * streamed and grown on demand so a file of any size loads (bounded only by
 * memory). Accepts file:/path, file://host/path (host ignored), file:///path; a
 * trailing ?query or #fragment is dropped. Returns 0, or -1 if it can't open. */
static int load_file_url(const char *url)
{
	const char *p = url + 5;            /* past "file:" */
	if (p[0] == '/' && p[1] == '/') {   /* "//" then an (ignored) host       */
		p += 2;
		while (*p && *p != '/') p++;
	}
	if (*p != '/') return -1;           /* need an absolute local path       */

	char path[URL_MAX]; int pn = 0;
	for (const char *q = p; *q && *q != '?' && *q != '#' &&
	                        pn < (int)sizeof path - 1; q++)
		path[pn++] = *q;
	path[pn] = '\0';

	B->raw_len = 0;
	unsigned off = 0;
	for (;;) {
		if (!ensure_raw(B->raw_len + 4096)) break;   /* out of memory: stop */
		int got = readfile_at(path, off, B->raw + B->raw_len, 4096);
		if (got < 0) return -1;
		if (got == 0) break;            /* end of file                       */
		B->raw_len += got;
		off        += (unsigned)got;
	}
	ctype_from_ext(path);
	return 0;
}

/* data:[<mediatype>][;base64],<data> — decode an inline document into B->raw.
 * The media type (default text/plain) sets B->ctype so render_raw shows HTML/
 * text/PNG correctly; the payload is base64- or percent-decoded. A non-base64
 * body keeps '+' literal (unlike form-encoding). Returns 0, or -1 if malformed. */
static int load_data_url(const char *url)
{
	const char *p = url + 5;            /* past "data:" */
	const char *comma = p;
	while (*comma && *comma != ',') comma++;
	if (*comma != ',') return -1;       /* a data URL must have a comma       */

	char mt[80]; int mi = 0, base64 = 0;
	const char *h = p;
	while (h < comma && *h != ';') {    /* the media type "type/subtype"      */
		if (mi < (int)sizeof mt - 1) mt[mi++] = lower(*h);
		h++;
	}
	mt[mi] = '\0';
	while (h < comma) {                 /* ';'-separated params               */
		h++;
		const char *tok = h;
		while (h < comma && *h != ';') h++;
		if (h - tok == 6 && starts_ci(tok, "base64")) base64 = 1;
		/* charset= and other params are ignored                             */
	}

	int k = 0;
	B->ctype[0] = '\0';
	sapp(B->ctype, &k, sizeof B->ctype, mt[0] ? mt : "text/plain");

	const char *d = comma + 1;
	B->raw_len = 0;
	if (base64) {
		int acc = 0, nb = 0;
		for (; *d && *d != '='; d++) {
			int v = b64val((unsigned char)*d);
			if (v < 0) continue;        /* skip whitespace/newlines           */
			acc = (acc << 6) | v; nb += 6;
			if (nb >= 8) {
				nb -= 8;
				if (!ensure_raw(B->raw_len + 1)) break;
				B->raw[B->raw_len++] = (char)((acc >> nb) & 0xff);
			}
		}
	} else {
		for (; *d; d++) {
			if (!ensure_raw(B->raw_len + 1)) break;
			int c = (unsigned char)*d;
			if (c == '%') {
				int hi = hexval((unsigned char)d[1]);
				int lo = hi < 0 ? -1 : hexval((unsigned char)d[2]);
				if (lo >= 0) { c = hi * 16 + lo; d += 2; }
			}
			B->raw[B->raw_len++] = (char)c;
		}
	}
	return 0;
}

void do_fetch(void)
{
	char url[URL_MAX]; int n = 0;
	sapp(url, &n, sizeof url, B->req_url);
	B->dl_saved = 0;
	B->favicon_ok = 0;          /* cleared per navigation; refilled on success */
	ext_css_clear();            /* Link: stylesheets refilled by parse_response  */

	/* Local / inline schemes never touch the network: file:// reads the local
	 * FS, data: decodes an embedded document. Both fill B->raw + B->ctype and
	 * render through the same path as a fetched page (no sub-resource fetch). */
	if (starts_ci(url, "file:") || starts_ci(url, "data:")) {
		int data = starts_ci(url, "data:");
		B->ctype[0] = '\0'; B->dl_name[0] = '\0'; B->cdisp_attach = 0;
		B->rpos = B->rlen = B->reof = 0;
		B->n_imgs = 0;
		int rc = data ? load_data_url(url) : load_file_url(url);
		if (rc < 0) {
			show_error(data ? "Malformed data: URL"
			                : "Cannot open file (not found or not absolute)");
			return;
		}
		render_raw(url);
		return;
	}

	/* Auto-add a scheme when the user typed a bare host ("miwa.lol/..."): default
	 * to https like a modern browser. Remember it was implicit so a host that
	 * can't do TLS (or a plain dev server on a custom port) falls back to http. */
	int implicit_scheme = 0;
	if (!starts_ci(url, "http://") && !starts_ci(url, "https://")) {
		char tmp[URL_MAX]; int t = 0;
		sapp(tmp, &t, sizeof tmp, "https://");
		sapp(tmp, &t, sizeof tmp, url);
		n = 0; sapp(url, &n, sizeof url, tmp);
		implicit_scheme = 1;
	}

	for (int hop = 0; hop <= MAX_REDIR; hop++) {
		struct purl u;
		if (!parse_url(url, &u)) { show_error("Bad URL"); return; }

		/* HSTS: a host that previously sent Strict-Transport-Security is only
		 * ever contacted over HTTPS, even if the URL says http://. */
		if (!u.tls && hsts_has(u.host)) {
			u.tls = 1;
			if (u.port == 80) u.port = 443;
		}

		char loc[URL_MAX];
		int st = http_txn(&u, url, loc, sizeof loc, 1);
		if (st < 0) {
			/* Secure connect failed for an auto-https URL (host has no TLS, or a
			 * plain dev server) — retry the same URL over http once. */
			if (implicit_scheme && st == -3 && starts_ci(url, "https://")) {
				char tmp[URL_MAX]; int t = 0;
				sapp(tmp, &t, sizeof tmp, "http://");
				sapp(tmp, &t, sizeof tmp, url + 8);   /* past "https://" */
				n = 0; sapp(url, &n, sizeof url, tmp);
				implicit_scheme = 0;
				continue;
			}
			if      (st == -2) show_error("Could not resolve host");
			else if (st == -1) show_error("No network device / out of sockets");
			else if (st == -3) show_error("Connection failed");
			else if (st == -4) show_error("Failed to send request");
			else               show_error("No response from server");
			return;
		}
		if ((st == 301 || st == 302 || st == 303 || st == 307 || st == 308) && loc[0]) {
			if (hop == MAX_REDIR) { show_error("Too many redirects"); return; }
			n = 0; sapp(url, &n, sizeof url, loc);
			continue;
		}

		/* A download was streamed straight to ~/Downloads inside http_txn (it
		 * never touched B->raw), so there is nothing to render. */
		if (B->dl_saved)
			return;

		/* Render according to the response type. An empty Content-Type is
		 * treated as HTML (the common case for sloppy servers); a plain text
		 * type is shown verbatim so line breaks survive; image/png is decoded. */
		render_raw(url);
		/* The title bar is updated by the main thread (only the window's owner
		 * task may call win_settitle); it reads B->doc_title / B->cur_url. */

		/* HTML pages may carry inline <img>: the parallel fetchers (resource.c)
		 * pull each over its own socket into img_pool — B->raw is untouched. Each
		 * landed image bumps B->seq so the UI re-lays-out as they arrive. */
		if (B->mode == MODE_HTML && B->n_imgs > 0)
			fetch_inline_images(url);

		/* Pull the site favicon for the tab strip. Skipped for a directly-rendered
		 * image page, whose pixels are in B->raw (the favicon fetch uses its own
		 * buffer, but the page is already the image, so there's nothing to fetch). */
		if (B->mode != MODE_IMAGE)
			fetch_favicon(url);
		return;
	}
}
