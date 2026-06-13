/* fetch.c — HTTP/HTTPS transport for the browser: buffered reader, cookie jar,
 * one transaction, and the redirect-following fetch. Ported from the kernel
 * app_browser.c with net_sock_* -> the net_* user syscalls. Runs on the worker
 * thread (see main.c), so its blocking reads never stall the UI. */
#include "browser.h"

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

void set_status(const char *s)
{
	int i = 0;
	while (s[i] && i < (int)sizeof B->status - 1) { B->status[i] = s[i]; i++; }
	B->status[i] = '\0';
	B->seq++;
}

/* -------------------------------------------------------------- cookie jar */

static int streq_ci(const char *a, const char *b)
{ while (*a && lower(*a) == lower(*b)) { a++; b++; } return lower(*a) == lower(*b); }

/* Case-insensitive substring test (used to spot "html" inside a Content-Type). */
static int has_sub(const char *s, const char *sub)
{
	for (; *s; s++) {
		const char *a = s, *b = sub;
		while (*b && lower(*a) == lower(*b)) { a++; b++; }
		if (!*b) return 1;
	}
	return 0;
}

static void cookie_store(const char *host, const char *v)
{
	while (*v == ' ') v++;
	char name[64]; int ni = 0;
	while (*v && *v != '=' && *v != ';' && ni < (int)sizeof name - 1) name[ni++] = *v++;
	name[ni] = '\0';
	if (*v != '=' || ni == 0) return;
	v++;
	char val[256]; int vi = 0;
	while (*v && *v != ';' && *v != '\r' && *v != '\n' && vi < (int)sizeof val - 1)
		val[vi++] = *v++;
	val[vi] = '\0';

	int free_slot = -1;
	for (int i = 0; i < MAX_COOKIES; i++) {
		if (!B->cookies[i].used) { if (free_slot < 0) free_slot = i; continue; }
		if (streq_ci(B->cookies[i].host, host) && streq(B->cookies[i].name, name)) {
			int k = 0; for (; val[k]; k++) B->cookies[i].val[k] = val[k];
			B->cookies[i].val[k] = '\0';
			return;
		}
	}
	if (free_slot < 0) return;
	struct cookie *c = &B->cookies[free_slot];
	c->used = 1;
	int k;
	for (k = 0; host[k] && k < (int)sizeof c->host - 1; k++) c->host[k] = host[k];
	c->host[k] = '\0';
	for (k = 0; name[k]; k++) c->name[k] = name[k];
	c->name[k] = '\0';
	for (k = 0; val[k]; k++) c->val[k] = val[k];
	c->val[k] = '\0';
}

static int cookie_header(const char *host, char *out, int cap)
{
	int n = 0;
	for (int i = 0; i < MAX_COOKIES; i++) {
		if (!B->cookies[i].used || !streq_ci(B->cookies[i].host, host)) continue;
		if (n) sapp(out, &n, cap, "; ");
		sapp(out, &n, cap, B->cookies[i].name);
		sapp(out, &n, cap, "=");
		sapp(out, &n, cap, B->cookies[i].val);
	}
	return n;
}

static int cookie_count(const char *host)
{
	int n = 0;
	for (int i = 0; i < MAX_COOKIES; i++)
		if (B->cookies[i].used && streq_ci(B->cookies[i].host, host)) n++;
	return n;
}

/* ------------------------------------------------- HSTS (force-HTTPS hosts) */

static int hsts_has(const char *host)
{
	for (int i = 0; i < B->n_hsts; i++)
		if (streq_ci(B->hsts[i], host)) return 1;
	return 0;
}

static void hsts_add(const char *host)
{
	if (hsts_has(host) || B->n_hsts >= MAX_HSTS) return;
	int k = 0;
	for (; host[k] && k < 127; k++) B->hsts[B->n_hsts][k] = host[k];
	B->hsts[B->n_hsts][k] = '\0';
	B->n_hsts++;
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

/* Appends decompressed (or copied) bytes to the render buffer, stopping once it
 * is full — returning <0 then makes the streaming inflater halt cleanly, so an
 * over-large page is truncated to RAW_MAX rather than failing outright. */
static int raw_sink(void *ctx, const unsigned char *p, unsigned n)
{
	(void)ctx;
	for (unsigned i = 0; i < n; i++) {
		if (B->raw_len >= RAW_MAX)
			return -1;
		B->raw[B->raw_len++] = (char)p[i];
	}
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

/* A response is a download when the server flags it as an attachment, or its
 * Content-Type is something the browser can't render in the pane (anything that
 * isn't HTML, plain text, or an image — images get their own viewer/notice). An
 * empty Content-Type is treated as HTML, the common sloppy-server case. */
static int is_download(void)
{
	if (B->cdisp_attach) return 1;
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

/* Build DL_DIR/<base> for attempt 0, or DL_DIR/<stem>-<n><ext> for n>=1, so a
 * repeated download lands beside the original instead of clobbering it. */
static void build_path(const char *base, int n, char *out, int cap)
{
	int p = 0;
	sapp(out, &p, cap, DL_DIR "/");
	if (n == 0) { sapp(out, &p, cap, base); return; }
	int dot = -1;
	for (int i = 0; base[i]; i++) if (base[i] == '.') dot = i;
	if (dot <= 0) {                                /* no usable extension      */
		sapp(out, &p, cap, base);
		sapp(out, &p, cap, "-"); uapp(out, &p, cap, (unsigned)n);
	} else {
		for (int i = 0; i < dot && p < cap - 1; i++) out[p++] = base[i];
		out[p] = '\0';
		sapp(out, &p, cap, "-"); uapp(out, &p, cap, (unsigned)n);
		sapp(out, &p, cap, base + dot);            /* ".ext"                   */
	}
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

/* Stream the response body straight to ~/Downloads rather than buffering it, so
 * a download is bounded only by the disk, never by RAW_MAX. Picks a
 * non-clobbering name, inflates on the fly when the body is compressed, and
 * reports the outcome through the status line and content pane. */
static void stream_download(const char *url, struct bodysrc *bs, int enc_gz)
{
	char base[128];
	derive_filename(url, base, sizeof base);

	char path[256], probe[1];
	for (int n = 0; n < 100; n++) {
		build_path(base, n, path, sizeof path);
		if (readfile(path, probe, 0) < 0) break;   /* first free name           */
	}

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

	B->mode = MODE_TEXT;
	{ int ci = 0; sapp(B->cur_url, &ci, sizeof B->cur_url, url); }
	B->scroll = 0;

	char msg[256]; int m = 0;
	if (!d.fail && wrote >= 0) {
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
	set_status(msg);
	B->dl_saved = 1;
}

/* ----------------------------------------------------------- transaction */

static int http_txn(const struct purl *u, const char *full_url, char *loc,
                    int loc_cap, int allow_dl)
{
	loc[0] = '\0';
	B->ctype[0] = '\0';
	B->dl_name[0] = '\0';
	B->cdisp_attach = 0;
	B->rpos = B->rlen = B->reof = 0;
	B->raw_len = 0;

	B->sock = net_connect(u->host, u->port, u->tls);
	if (B->sock < 0) return B->sock;

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
	     "\r\nUser-Agent: Mozilla/5.0 (OwOS; x86) OwOS-Browser/1.0\r\n"
	     "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n"
	     "Accept-Language: en-US,en;q=0.9\r\n"
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

	int status = 0;
	{
		const char *s = hdr;
		while (*s && *s != ' ') s++;
		while (*s == ' ') s++;
		while (*s >= '0' && *s <= '9') status = status * 10 + (*s++ - '0');
	}

	int chunked = 0, have_len = 0, enc_gz = 0; unsigned content_len = 0;
	for (char *line = hdr; *line; ) {
		if (starts_ci(line, "transfer-encoding:")) {
			for (char *q = line; *q && *q != '\n'; q++)
				if (starts_ci(q, "chunked")) { chunked = 1; break; }
		} else if (starts_ci(line, "content-encoding:")) {
			/* gzip / deflate -> the body is a compressed stream we inflate.
			 * inflate_http auto-detects gzip vs zlib(deflate) from its bytes. */
			for (char *q = line; *q && *q != '\n'; q++)
				if (starts_ci(q, "gzip") || starts_ci(q, "deflate")) {
					enc_gz = 1; break;
				}
		} else if (starts_ci(line, "content-length:")) {
			const char *v = line + 15;
			while (*v == ' ') v++;
			unsigned x = 0;
			while (*v >= '0' && *v <= '9') x = x * 10 + (unsigned)(*v++ - '0');
			content_len = x; have_len = 1;
		} else if (starts_ci(line, "location:")) {
			const char *v = line + 9;
			while (*v == ' ') v++;
			char raw[URL_MAX]; int r = 0;
			while (*v && *v != '\r' && *v != '\n' && r < (int)sizeof raw - 1)
				raw[r++] = *v++;
			raw[r] = '\0';
			resolve_url(full_url, raw, loc, loc_cap);
		} else if (starts_ci(line, "content-type:")) {
			const char *v = line + 13;
			while (*v == ' ') v++;
			int k = 0;     /* keep the type/subtype, drop any "; charset=..." */
			while (*v && *v != '\r' && *v != '\n' && *v != ';' &&
			       k < (int)sizeof B->ctype - 1)
				B->ctype[k++] = lower(*v++);
			B->ctype[k] = '\0';
		} else if (starts_ci(line, "content-disposition:")) {
			/* "attachment" forces a download regardless of type; a filename=
			 * param (quoted or bare token) names the saved file. filename*=
			 * (RFC 5987 extended form) is skipped — we prefer the plain one. */
			const char *v = line + 20;
			for (const char *q = v; *q && *q != '\n'; q++)
				if (starts_ci(q, "attachment")) { B->cdisp_attach = 1; break; }
			for (const char *q = v; *q && *q != '\n'; q++) {
				if (!starts_ci(q, "filename")) continue;
				const char *f = q + 8;
				if (*f == '*') continue;           /* filename*= : skip       */
				while (*f == ' ') f++;
				if (*f != '=') continue;
				f++;
				while (*f == ' ') f++;
				int quoted = (*f == '"'); if (quoted) f++;
				int k = 0;
				while (*f && *f != '\r' && *f != '\n' &&
				       k < (int)sizeof B->dl_name - 1) {
					if (quoted ? (*f == '"') : (*f == ';' || *f == ' ')) break;
					B->dl_name[k++] = *f++;
				}
				B->dl_name[k] = '\0';
				break;
			}
		} else if (starts_ci(line, "set-cookie:")) {
			const char *v = line + 11;
			while (*v == ' ') v++;
			cookie_store(u->host, v);
		} else if (starts_ci(line, "strict-transport-security:")) {
			hsts_add(u->host);     /* future visits to this host force HTTPS  */
		}
		char *eol = line;
		while (*eol && *eol != '\n') eol++;
		if (!*eol) break;
		line = eol + 1;
	}

	struct bodysrc bs = { chunked, have_len, 0, 0,
	                      have_len ? content_len : 0 };

	/* A download (attachment, or a non-renderable Content-Type) on a successful
	 * response streams straight to ~/Downloads — decided here, before the body is
	 * read, so binary content never has to fit in B->raw. Redirects (3xx) and
	 * sub-resource fetches (allow_dl == 0) take the buffered path below. */
	if (allow_dl && status >= 200 && status < 300 && is_download()) {
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
		while ((c = body_src(&bs)) >= 0 && B->raw_len < RAW_MAX)
			B->raw[B->raw_len++] = (char)c;
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

/* Fetch every inline <img> the parser found as a sub-resource. Each is fetched
 * into B->raw (one at a time), and PNGs that fit are copied into img_pool with
 * their dimensions peeked; the rest are left ok=0 so the layout shows an alt
 * box. Single-hop only (redirects on an image just drop it). */
static void fetch_inline_images(const char *page_url)
{
	B->img_pool_len = 0;
	for (int i = 0; i < B->n_imgs; i++) {
		struct inl_img *im = &B->imgs[i];
		im->ok = 0;

		char full[URL_MAX];
		resolve_url(page_url, im->href, full, sizeof full);
		struct purl u;
		if (!parse_url(full, &u)) continue;
		if (!u.tls && hsts_has(u.host)) { u.tls = 1; if (u.port == 80) u.port = 443; }

		char msg[200]; int m = 0;
		sapp(msg, &m, sizeof msg, "Loading image ");
		uapp(msg, &m, sizeof msg, (unsigned)(i + 1));
		sapp(msg, &m, sizeof msg, "/");
		uapp(msg, &m, sizeof msg, (unsigned)B->n_imgs);
		sapp(msg, &m, sizeof msg, " ...");
		set_status(msg);

		char loc[URL_MAX];
		int st = http_txn(&u, full, loc, sizeof loc, 0);
		if (st != 200 || B->raw_len < 8) continue;
		if ((unsigned char)B->raw[0] != 0x89 || B->raw[1] != 'P' ||
		    B->raw[2] != 'N' || B->raw[3] != 'G') continue;          /* PNG only */
		if (B->img_pool_len + B->raw_len > IMG_POOL) continue;       /* no room  */

		im->coff = B->img_pool_len;
		im->clen = B->raw_len;
		for (int k = 0; k < B->raw_len; k++)
			B->img_pool[im->coff + k] = (unsigned char)B->raw[k];
		B->img_pool_len += B->raw_len;

		struct memrd mr = { B->img_pool + im->coff, 0, im->clen };
		unsigned iw = 0, ih = 0;
		if (upng_dimensions(memrd_get, &mr, &iw, &ih) == 0 && iw && ih) {
			im->iw = iw; im->ih = ih; im->ok = 1;
		}
		B->seq++;                  /* let the UI re-layout as each image lands  */
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
	if (B->ctype[0] == '\0' || has_sub(B->ctype, "html")) {
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
 * bounded by RAW_MAX and streamed so a file larger than the read buffer still
 * loads. Accepts file:/path, file://host/path (host ignored), file:///path; a
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
		int want = RAW_MAX - B->raw_len;
		if (want <= 0) break;
		if (want > 4096) want = 4096;
		int got = readfile_at(path, off, B->raw + B->raw_len, (unsigned)want);
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
				if (B->raw_len >= RAW_MAX) break;
				B->raw[B->raw_len++] = (char)((acc >> nb) & 0xff);
			}
		}
	} else {
		for (; *d && B->raw_len < RAW_MAX; d++) {
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
			set_status(data ? "Malformed data: URL"
			                : "Cannot open file (not found or not absolute)");
			return;
		}
		render_raw(url);
		char done[256]; int dn = 0;
		sapp(done, &dn, sizeof done, data ? "data:  -  " : "file:  -  ");
		uapp(done, &dn, sizeof done, (unsigned)B->raw_len);
		sapp(done, &dn, sizeof done, " bytes  -  ");
		sapp(done, &dn, sizeof done, url);
		set_status(done);
		return;
	}

	for (int hop = 0; hop <= MAX_REDIR; hop++) {
		struct purl u;
		if (!parse_url(url, &u)) { set_status("Bad URL"); return; }

		/* HSTS: a host that previously sent Strict-Transport-Security is only
		 * ever contacted over HTTPS, even if the URL says http://. */
		if (!u.tls && hsts_has(u.host)) {
			u.tls = 1;
			if (u.port == 80) u.port = 443;
		}

		char msg[200]; int m = 0;
		sapp(msg, &m, sizeof msg, u.tls ? "Connecting (https) to " : "Connecting to ");
		sapp(msg, &m, sizeof msg, u.host);
		sapp(msg, &m, sizeof msg, " ...");
		set_status(msg);

		char loc[URL_MAX];
		int st = http_txn(&u, url, loc, sizeof loc, 1);
		if (st < 0) {
			if      (st == -2) set_status("Could not resolve host");
			else if (st == -1) set_status("No network device / out of sockets");
			else if (st == -3) set_status("Connection failed");
			else if (st == -4) set_status("Failed to send request");
			else               set_status("No response from server");
			return;
		}
		if ((st == 301 || st == 302 || st == 303 || st == 307 || st == 308) && loc[0]) {
			if (hop == MAX_REDIR) { set_status("Too many redirects"); return; }
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

		unsigned page_bytes = (unsigned)B->raw_len;

		/* HTML pages may carry inline <img>: fetch each as a sub-resource and
		 * stash its PNG bytes (this reuses B->raw, now that html_to_text has
		 * already extracted the page text). Done before the final status line so
		 * the per-image "Loading image N/M" notices don't linger afterwards. */
		if (B->mode == MODE_HTML && B->n_imgs > 0)
			fetch_inline_images(url);

		char done[200]; int d = 0;
		sapp(done, &d, sizeof done, "HTTP ");
		uapp(done, &d, sizeof done, (unsigned)st);
		sapp(done, &d, sizeof done, "  -  ");
		uapp(done, &d, sizeof done, page_bytes);
		sapp(done, &d, sizeof done, " bytes");
		int nck = cookie_count(u.host);
		if (nck) {
			sapp(done, &d, sizeof done, "  -  ");
			uapp(done, &d, sizeof done, (unsigned)nck);
			sapp(done, &d, sizeof done, " cookie");
			if (nck != 1) sapp(done, &d, sizeof done, "s");
		}
		sapp(done, &d, sizeof done, "  -  ");
		sapp(done, &d, sizeof done, url);
		set_status(done);
		return;
	}
}
