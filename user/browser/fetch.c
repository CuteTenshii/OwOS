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

/* ----------------------------------------------------------- transaction */

static int http_txn(const struct purl *u, const char *full_url, char *loc, int loc_cap)
{
	loc[0] = '\0';
	B->ctype[0] = '\0';
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
		int st = http_txn(&u, full, loc, sizeof loc);
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

void do_fetch(void)
{
	char url[URL_MAX]; int n = 0;
	sapp(url, &n, sizeof url, B->req_url);

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
		int st = http_txn(&u, url, loc, sizeof loc);
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

		/* Render according to the response type. An empty Content-Type is
		 * treated as HTML (the common case for sloppy servers); a plain text
		 * type is shown verbatim so line breaks survive; image/png is decoded. */
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
