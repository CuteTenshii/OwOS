/* fetch.c — HTTP/HTTPS transport for the browser: buffered reader, one
 * transaction, and the redirect-following fetch. Ported from the kernel
 * app_browser.c with net_sock_* -> the net_* user syscalls. Runs on the worker
 * thread (see main.c), so its blocking reads never stall the UI. The cookie +
 * HSTS jar and its SQLite persistence live in jar.c; this file only calls
 * cookie_store/cookie_header/hsts_add/hsts_has (declared in browser.h). */
#include "browser.h"
#include "headers.h"   /* libcurl: HTTP response header parsing */

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
 * streq_ci lives in browser.h; the cookie/HSTS jar lives in jar.c. */
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

/* Request-header values, shared by the HTTP/1.1 and HTTP/2 request builders so
 * the browser presents itself identically on either protocol. */
#define BR_UA     "Mozilla/5.0 (OwOS; x86) OwOS-Browser/1.0"
#define BR_ACCEPT "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"
#define BR_ACCLNG "en-US,en;q=0.9"

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
		if (enc_gz) {
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

/* Parse a response header block (an HTTP/1.1 block, or the HTTP/2-synthesized
 * "HTTP/x NNN\r\nname: value\r\n…" equivalent) into the browser state: sets
 * B->ctype / B->dl_name / B->cdisp_attach, stores cookies for `host`, notes
 * HSTS, resolves any Location into `loc`, and reports the body framing
 * (chunked / content-length / gzip — moot over h2, which sends none). Returns
 * the status code. Shared by the HTTP/1.1 and HTTP/2 fetch paths. */
static int parse_response(const char *hdr, const char *host, const char *full_url,
                          char *loc, int loc_cap, int *chunked, int *have_len,
                          unsigned *content_len, int *enc_gz)
{
	*chunked = 0; *have_len = 0; *enc_gz = 0; *content_len = 0;
	B->nosniff = false;

	/* Parse the raw block once into a k/v object, then just query it. */
	struct http_headers H;
	http_headers_parse(&H, hdr);
	int status = H.status;

	const char *te = http_headers_get(&H, "transfer-encoding");
	if (te && http_header_has_token(te, "chunked")) *chunked = 1;

	const char *ce = http_headers_get(&H, "content-encoding");
	if (ce && (http_header_has_token(ce, "gzip") || http_header_has_token(ce, "deflate")))
		*enc_gz = 1;

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
	char dl_path[256];
};

/* Headers-ready hook (fired before any DATA): parse the response, and if it is
 * a download, open the destination file now so the body streams to disk. */
static int h2_headers_cb(void *ctx, const char *block, int len)
{
	(void)len;
	struct h2tx *h = ctx;
	int chunked, have_len, enc_gz; unsigned clen;
	h->status = parse_response(block, h->host, h->full_url, h->loc, h->loc_cap,
	                           &chunked, &have_len, &clen, &enc_gz);
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

	/* Request headers: cookies + accept. No Accept-Encoding (so the body is
	 * uncompressed — h2 DATA needs no inflate) and no Connection/Transfer-
	 * Encoding (forbidden in HTTP/2). The User-Agent rides in h2_opts. */
	char xh[1408]; int xn = 0;
	char ck[1024];
	if (cookie_header(u->host, ck, sizeof ck) > 0) {
		sapp(xh, &xn, sizeof xh, "cookie: ");
		sapp(xh, &xn, sizeof xh, ck);
		sapp(xh, &xn, sizeof xh, "\r\n");
	}
	sapp(xh, &xn, sizeof xh,
	     "accept: " BR_ACCEPT "\r\naccept-language: " BR_ACCLNG "\r\n");

	struct h2tx h = { full_url, u->host, loc, loc_cap, allow_dl, 0, 0, 0, 0, 0, {0} };
	struct h2_opts opts = { BR_UA, xh, h2_headers_cb };
	char hdrbuf[4096]; int hlen = 0;
	h2_run(B->sock, 1, auth, "GET", u->path, &opts,
	       hdrbuf, sizeof hdrbuf, &hlen, h2_body_cb, &h);

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
	     "Accept-Encoding: gzip, deflate\r\n"
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

	if (enc_gz) {
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

/* --------------------------------------------------- inline image fetching */

/* A byte reader over a slice of img_pool, for upng_dimensions. */
struct memrd { const unsigned char *p; int pos, len; };
static int memrd_get(void *c)
{
	struct memrd *m = c;
	return m->pos < m->len ? m->p[m->pos++] : -1;
}

/* Self-contained per-thread HTTP reader: several images fetch in parallel, each
 * over its OWN socket/buffer, so they never touch B->sock/B->raw (which only the
 * page fetch uses). The kernel TLS is multi-flow, so the HTTPS handshakes run
 * concurrently. */
struct ireader { int sock; unsigned char buf[2048]; int pos, len, eof; };
static int ir_getc(struct ireader *r)
{
	if (r->pos >= r->len) {
		if (r->eof) return -1;
		int n = net_recv(r->sock, r->buf, (int)sizeof r->buf);
		if (n <= 0) { r->eof = 1; return -1; }
		r->pos = 0; r->len = n;
	}
	return r->buf[r->pos++];
}

/* Append one byte to a malloc'd, doubling body buffer (capped at RAW_MAX).
 * Returns 0, or -1 if full / out of memory (the caller stops). */
static int body_put(unsigned char **body, int *cap, int *blen, int c)
{
	if (*blen >= *cap) {
		int ncap = *cap * 2;
		if (ncap > (int)RAW_MAX) ncap = RAW_MAX;
		if (*blen >= ncap) return -1;             /* hit the cap */
		unsigned char *nb = realloc(*body, (unsigned)ncap);
		if (!nb) return -1;
		*body = nb; *cap = ncap;
	}
	(*body)[(*blen)++] = (unsigned char)c;
	return 0;
}

/* Read a 200 response's body (chunked / content-length / read-to-close) off an
 * open image request into a freshly malloc'd buffer. Returns it (caller frees)
 * with *outlen set, or NULL on a non-200 / error. Bounded by RAW_MAX. */
static unsigned char *read_http_body(struct ireader *r, int *outlen)
{
	char hdr[2048]; int hl = 0, m = 0;
	for (;;) {
		int c = ir_getc(r);
		if (c < 0) return 0;
		if (hl < (int)sizeof hdr - 1) hdr[hl++] = (char)c;
		if      (c == '\r') m = (m == 2) ? 3 : 1;
		else if (c == '\n') m = (m == 1) ? 2 : (m == 3 ? 4 : 0);
		else                m = 0;
		if (m == 4) break;
	}
	hdr[hl] = '\0';

	int status = 0;
	{ const char *s = hdr; while (*s && *s != ' ') s++; while (*s == ' ') s++;
	  while (*s >= '0' && *s <= '9') status = status * 10 + (*s++ - '0'); }
	if (status != 200) return 0;

	int chunked = 0; long clen = -1;
	for (const char *line = hdr; *line; ) {
		if (starts_ci(line, "content-length:")) {
			const char *v = line + 15; while (*v == ' ') v++;
			clen = 0; while (*v >= '0' && *v <= '9') clen = clen * 10 + (*v++ - '0');
		} else if (starts_ci(line, "transfer-encoding:")) {
			for (const char *q = line; *q && *q != '\n'; q++)
				if (starts_ci(q, "chunked")) { chunked = 1; break; }
		}
		const char *e = line; while (*e && *e != '\n') e++;
		if (!*e) break;
		line = e + 1;
	}

	int cap = (clen > 0 && clen < (long)RAW_MAX) ? (int)clen + 1 : 65536;
	unsigned char *body = malloc((unsigned)cap);
	if (!body) return 0;
	int blen = 0;

	if (chunked) {
		for (;;) {
			unsigned sz = 0; int c, any = 0;
			while ((c = ir_getc(r)) >= 0 && c != '\r' && c != ';') {
				int d;
				if      (c >= '0' && c <= '9') d = c - '0';
				else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
				else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
				else break;
				sz = sz * 16 + (unsigned)d; any = 1;
			}
			while (c >= 0 && c != '\n') c = ir_getc(r);
			if (!any || sz == 0) break;
			for (unsigned k = 0; k < sz; k++) {
				c = ir_getc(r);
				if (c < 0 || body_put(&body, &cap, &blen, c) < 0) goto done;
			}
			c = ir_getc(r); if (c == '\r') ir_getc(r);   /* trailing CRLF */
		}
	} else {
		long limit = (clen >= 0) ? clen : (long)RAW_MAX;
		while (blen < limit) {
			int c = ir_getc(r);
			if (c < 0 || body_put(&body, &cap, &blen, c) < 0) break;
		}
	}
done:
	*outlen = blen;
	return body;
}

/* --- parallel inline-image fetching ---------------------------------------
 * A small pool of threads fetches the page's images concurrently. Each thread
 * reads an image into a malloc'd buffer, then commits it into the shared
 * img_pool: a short lock guards only the bump of img_pool_len; the byte copy and
 * the per-image metadata go to disjoint regions, so they need no lock. */
#define IMG_FETCHERS    4
#define IMG_FETCH_STACK (32 * 1024)

static char         g_img_page[URL_MAX];   /* page URL the fetchers resolve against */
static volatile int g_img_next;            /* next image index to claim             */
static volatile int g_img_lock;            /* guards g_img_next + the pool bump      */

static int img_xchg(volatile int *p, int v)
{ __asm__ volatile ("xchgl %0, %1" : "+r"(v), "+m"(*p) : : "memory"); return v; }
static void img_lock(void)   { while (img_xchg(&g_img_lock, 1)) msleep(0); }
static void img_unlock(void) { __asm__ volatile ("" ::: "memory"); g_img_lock = 0; }

/* Claim the next eager (non-lazy) image index, or -1 when none remain. */
static int img_claim_next(void)
{
	int got = -1;
	img_lock();
	while (g_img_next < B->n_imgs) {
		int i = g_img_next++;
		if (!B->imgs[i].lazy) { got = i; break; }
	}
	img_unlock();
	return got;
}

/* Fetch inline image `i` over its own socket and commit it to img_pool. Safe to
 * run on several threads at once. */
static void fetch_image(int i, const char *page_url)
{
	struct inl_img *im = &B->imgs[i];
	im->ok = 0;

	char full[URL_MAX];
	resolve_url(page_url, im->href, full, sizeof full);
	struct purl u;
	if (!parse_url(full, &u)) return;
	if (!u.tls && hsts_has(u.host)) { u.tls = 1; if (u.port == 80) u.port = 443; }

	int sock = net_connect(u.host, u.port, u.tls ? SK_TLS : SK_TCP);
	if (sock < 0) return;

	char req[1024]; int n = 0;
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
	sapp(req, &n, sizeof req,
	     "\r\nUser-Agent: " BR_UA "\r\nAccept: image/png,*/*\r\nConnection: close\r\n\r\n");
	if (net_send(sock, req, n) < 0) { net_close(sock); return; }

	struct ireader r = { sock, { 0 }, 0, 0, 0 };
	int blen = 0;
	unsigned char *body = read_http_body(&r, &blen);
	net_close(sock);
	if (!body) return;

	if (blen >= 8 && body[0] == 0x89 && body[1] == 'P' &&
	    body[2] == 'N' && body[3] == 'G') {            /* PNG only */
		struct memrd mr = { body, 0, blen };
		unsigned iw = 0, ih = 0;
		int okdim = (upng_dimensions(memrd_get, &mr, &iw, &ih) == 0 && iw && ih);

		int off = -1;
		img_lock();
		if (B->img_pool_len + blen <= IMG_POOL) {
			off = B->img_pool_len;
			B->img_pool_len += blen;
		}
		img_unlock();
		if (off >= 0) {
			memcpy(B->img_pool + off, body, (unsigned)blen);  /* disjoint: no lock */
			im->coff = off; im->clen = blen;
			if (okdim) { im->iw = iw; im->ih = ih; }
			im->ok = okdim;            /* set last: the UI draws once ok is seen */
			B->seq++;                  /* repaint as each image lands            */
		}
	}
	free(body);
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
	exit(0);                           /* a thread fn must not return */
}

/* Fetch the page's *eager* inline images (loading != "lazy") in PARALLEL: spawn
 * a few helper threads, fetch alongside them on this (worker) thread, then join.
 * loading="lazy" images are left for fetch_lazy_visible() when scrolled in. */
static void fetch_inline_images(const char *page_url)
{
	B->img_pool_len = 0;
	{ int k = 0; g_img_page[0] = '\0'; sapp(g_img_page, &k, sizeof g_img_page, page_url); }
	g_img_next = 0;

	int   tids[IMG_FETCHERS - 1];
	char *stks[IMG_FETCHERS - 1];
	int   nthr = 0;
	for (int t = 0; t < IMG_FETCHERS - 1; t++) {
		char *stk = malloc(IMG_FETCH_STACK);
		if (!stk) break;
		int tid = thread_create(img_fetcher_thread, stk + IMG_FETCH_STACK);
		if (tid < 0) { free(stk); break; }
		stks[nthr] = stk; tids[nthr] = tid; nthr++;
	}

	img_fetch_loop();                  /* this thread fetches too */

	for (int t = 0; t < nthr; t++) {   /* wait for the helpers, free their stacks */
		thread_join(tids[t]);
		free(stks[t]);
	}
}

/* Worker step (called when no page fetch is pending): load at most one lazy
 * <img> the UI has flagged as scrolled into view, against the current page URL.
 * Returns 1 if it fetched one (so the caller can loop), 0 if there was nothing to
 * do — which keeps the worker mostly idle until the user actually scrolls. */
int fetch_lazy_visible(void)
{
	if (!B->cur_url[0])
		return 0;
	for (int i = 0; i < B->n_imgs; i++) {
		struct inl_img *im = &B->imgs[i];
		if (!im->lazy || im->fetched || !im->want)
			continue;
		fetch_image(i, B->cur_url);
		im->fetched = 1; im->want = 0;
		B->seq++;                  /* re-layout now that the image has landed   */
		return 1;
	}
	return 0;
}

/* ------------------------------------------------------------ favicon decode */
/* A favicon is fetched into B->raw and decoded down to a fixed 16x16 ARGB
 * bitmap (B->favicon) for the tab strip. PNG favicons go through upng; .ico
 * files are parsed here (best directory entry -> embedded PNG or a raw DIB).
 * Anything we can't decode (GIF/BMP/SVG/empty) just leaves favicon_ok 0 and the
 * tab shows the default globe. */

/* Nearest-neighbour scale of a decoded RGBA image into the 16x16 favicon. The
 * row callback fills every destination row whose source maps to the row `y`
 * upng just produced, so it works whatever the source dimensions are. */
struct favscale { unsigned iw, ih, next_r; unsigned *dst; };
static int fav_row(void *c, unsigned y, const unsigned char *rgba, unsigned w)
{
	(void)w;
	struct favscale *v = c;
	while (v->next_r < FAVICON_H && (v->next_r * v->ih / FAVICON_H) == y) {
		unsigned *drow = v->dst + v->next_r * FAVICON_W;
		for (unsigned x = 0; x < FAVICON_W; x++) {
			const unsigned char *p = rgba + (x * v->iw / FAVICON_W) * 4;
			drow[x] = ((unsigned)p[3] << 24) | ((unsigned)p[0] << 16) |
			          ((unsigned)p[1] << 8) | (unsigned)p[2];
		}
		v->next_r++;
	}
	return 0;
}

static int fav_decode_png(const unsigned char *buf, int len)
{
	struct memrd mr = { buf, 0, len };
	unsigned iw = 0, ih = 0;
	if (upng_dimensions(memrd_get, &mr, &iw, &ih) != 0 || !iw || !ih)
		return 0;
	struct favscale v = { iw, ih, 0, B->favicon };
	struct memrd mr2 = { buf, 0, len };
	if (upng_decode(memrd_get, &mr2, fav_row, &v, 0, 0) != 0)
		return 0;
	return v.next_r > 0;
}

/* Little-endian readers for the .ico container (always little-endian). */
static unsigned le16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }
static unsigned le32(const unsigned char *p)
{ return p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24); }

/* Decode one ICO image stored as a Windows DIB (BITMAPINFOHEADER + pixels, no
 * BMP file header). Bottom-up rows; biHeight counts the XOR colour rows plus an
 * equal AND (1bpp transparency) mask, so the real height is half. Supports
 * 1/4/8/24/32 bpp, BI_RGB. Sampled directly at 16x16 to avoid a full-size
 * intermediate buffer. */
static int fav_decode_dib(const unsigned char *p, int len)
{
	if (len < 40) return 0;
	unsigned hsz = le32(p);
	if (hsz < 40 || (int)hsz > len) return 0;
	int W   = (int)le32(p + 4);
	int Hf  = (int)le32(p + 8);
	unsigned bpp  = le16(p + 14);
	unsigned comp = le32(p + 16);
	if (comp != 0 && comp != 3) return 0;          /* BI_RGB / BI_BITFIELDS only */
	if (W <= 0 || Hf <= 0) return 0;

	int H = Hf / 2, mask = 1;                       /* XOR colour rows = half     */
	if (H < 1) { H = Hf; mask = 0; }                /* no AND mask present         */

	unsigned clr = le32(p + 32);
	if (bpp <= 8 && clr == 0) clr = 1u << bpp;
	const unsigned char *pal = p + hsz;
	int pal_sz = (bpp <= 8) ? (int)clr * 4 : 0;

	int xstride = ((W * (int)bpp + 31) / 32) * 4;
	const unsigned char *xor_ = pal + pal_sz;
	long xor_sz = (long)xstride * H;
	if ((xor_ - p) + xor_sz > len) return 0;

	int astride = ((W + 31) / 32) * 4;
	const unsigned char *and_ = xor_ + xor_sz;
	int and_present = mask && ((and_ - p) + (long)astride * H <= len);

	/* Trust a 32bpp alpha channel only if at least one pixel is non-zero;
	 * many icons leave it all-zero and rely on the AND mask instead. */
	int has_alpha = 0;
	if (bpp == 32)
		for (long i = 3; i < xor_sz && !has_alpha; i += 4)
			if (xor_[i]) has_alpha = 1;

	for (int dy = 0; dy < FAVICON_H; dy++) {
		int sy = dy * H / FAVICON_H;                /* dest row -> source row      */
		int br = H - 1 - sy;                        /* DIB rows are bottom-up      */
		const unsigned char *xrow = xor_ + (long)br * xstride;
		const unsigned char *arow = and_ + (long)br * astride;
		for (int dx = 0; dx < FAVICON_W; dx++) {
			int sx = dx * W / FAVICON_W;
			unsigned r = 0, g = 0, b = 0, a = 255;
			if (bpp == 32) {
				const unsigned char *q = xrow + sx * 4;
				b = q[0]; g = q[1]; r = q[2]; a = has_alpha ? q[3] : 255;
			} else if (bpp == 24) {
				const unsigned char *q = xrow + sx * 3;
				b = q[0]; g = q[1]; r = q[2];
			} else {
				unsigned idx;
				if      (bpp == 8) idx = xrow[sx];
				else if (bpp == 4) idx = (sx & 1) ? (xrow[sx >> 1] & 0xf)
				                                  : (xrow[sx >> 1] >> 4);
				else if (bpp == 1) idx = (xrow[sx >> 3] >> (7 - (sx & 7))) & 1;
				else return 0;                      /* unsupported depth           */
				if (idx >= clr) idx = 0;
				const unsigned char *c = pal + idx * 4;
				b = c[0]; g = c[1]; r = c[2];
			}
			if (bpp != 32 && and_present) {
				unsigned mb = arow[sx >> 3];
				if ((mb >> (7 - (sx & 7))) & 1) a = 0;   /* mask bit set = clear   */
			}
			B->favicon[dy * FAVICON_W + dx] =
				(a << 24) | (r << 16) | (g << 8) | b;
		}
	}
	return 1;
}

/* Pick the best image in an .ico directory: the smallest entry that is still
 * >= 16px (an exact 16 wins), else the largest available. Fills poff/plen with
 * its byte range within the file. */
static int ico_pick(const unsigned char *p, int len, int *poff, int *plen)
{
	if (len < 6 || le16(p) != 0 || le16(p + 2) != 1) return 0;   /* type 1 = icon */
	int count = le16(p + 4);
	if (count < 1 || 6 + count * 16 > len) return 0;
	int best = -1, best_score = -1;
	for (int i = 0; i < count; i++) {
		const unsigned char *e = p + 6 + i * 16;
		int w = e[0] ? e[0] : 256;
		int boff = (int)le32(e + 12), blen = (int)le32(e + 8);
		if (boff < 0 || blen <= 0 || boff + blen > len) continue;
		int score = (w >= FAVICON_W) ? (1000 - w) : w;
		if (score > best_score) { best_score = score; best = i; *poff = boff; *plen = blen; }
	}
	return best >= 0;
}

/* Decode whatever sits in B->raw into B->favicon; returns 1 on success. */
static int favicon_decode(void)
{
	int n = B->raw_len;
	if (n < 4) return 0;
	const unsigned char *r = (const unsigned char *)B->raw;
	if (r[0] == 0x89 && r[1] == 'P' && r[2] == 'N' && r[3] == 'G')
		return fav_decode_png(r, n);
	if (r[0] == 0 && r[1] == 0 && r[2] == 1 && r[3] == 0) {        /* .ico        */
		int off = 0, l = 0;
		if (!ico_pick(r, n, &off, &l)) return 0;
		const unsigned char *img = r + off;
		if (l >= 8 && img[0] == 0x89 && img[1] == 'P' &&
		    img[2] == 'N' && img[3] == 'G')                        /* PNG in .ico */
			return fav_decode_png(img, l);
		return fav_decode_dib(img, l);
	}
	return 0;                                  /* GIF/BMP/SVG favicons unsupported */
}

/* Fetch and decode the page favicon into B->favicon. Uses the <link rel=icon>
 * href the parser found, else the conventional /favicon.ico at the site root.
 * Follows redirects (CDNs often 30x a favicon) up to MAX_REDIR, one socket per
 * hop, into B->raw — safe to clobber here since the page body is already
 * rendered. Leaves favicon_ok 0 on any failure (the tab falls back to the
 * globe). */
static void fetch_favicon(const char *page_url)
{
	char full[URL_MAX];
	resolve_url(page_url, B->favicon_href[0] ? B->favicon_href : "/favicon.ico",
	            full, sizeof full);

	for (int hop = 0; hop <= MAX_REDIR; hop++) {
		struct purl u;
		if (!parse_url(full, &u)) return;
		if (!u.tls && hsts_has(u.host)) { u.tls = 1; if (u.port == 80) u.port = 443; }

		char loc[URL_MAX]; loc[0] = '\0';
		int st = http_txn(&u, full, loc, sizeof loc, 0);
		if ((st == 301 || st == 302 || st == 303 || st == 307 || st == 308) && loc[0]) {
			char next[URL_MAX];
			resolve_url(full, loc, next, sizeof next);
			int k = 0;
			for (; next[k] && k < URL_MAX - 1; k++) full[k] = next[k];
			full[k] = '\0';
			continue;
		}
		if (st != 200 || B->raw_len < 4) return;
		if (favicon_decode()) B->favicon_ok = 1;
		return;
	}
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
 * all three pick the display mode the same way. Does not fetch sub-resources. */
static void render_raw(const char *url)
{
	/* HTML rendering normally also covers an empty/missing Content-Type (sloppy
	 * servers). But X-Content-Type-Options: nosniff forbids that guess: with it
	 * set, render as HTML ONLY when the server explicitly declared an HTML type,
	 * otherwise fall through to plain text. */
	int as_html = has_sub(B->ctype, "html") ||
	              (B->ctype[0] == '\0' && !B->nosniff);
	if (as_html) {
		B->mode = MODE_HTML; html_to_text();
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

		/* HTML pages may carry inline <img>: fetch each as a sub-resource and
		 * stash its PNG bytes (this reuses B->raw, now that html_to_text has
		 * already extracted the page text). Each fetched image bumps B->seq so
		 * the UI re-lays-out as they land. */
		if (B->mode == MODE_HTML && B->n_imgs > 0)
			fetch_inline_images(url);

		/* Pull the site favicon for the tab strip (after inline images, since it
		 * also reuses B->raw). Skipped for a directly-rendered image page, whose
		 * pixels still live in B->raw and would be clobbered. */
		if (B->mode != MODE_IMAGE)
			fetch_favicon(url);
		return;
	}
}
