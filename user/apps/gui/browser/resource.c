/* resource.c — sub-resource fetching for the browser: a self-contained HTTP/1.1
 * GET (own socket, follows redirects, sends cookies, HSTS-upgrade) shared by the
 * parallel inline-image fetcher, the favicon, and external stylesheets. These run
 * while the page renders, each on its own socket, so the multi-flow kernel TLS
 * lets several proceed at once. The main page transaction lives in fetch.c. */
#include "browser.h"
#include "fetchint.h"
#include "headers.h"   /* HTTP response headers (iframe X-Frame-Options / CSP)  */
#include "svg.h"       /* SVG -> ARGB rasterizer (favicons, inline images) */

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

/* Response line/header facts a sub-resource fetch needs: the status code and a
 * Location (for redirects). */
struct resp_meta { int status; char location[URL_MAX]; char ctype[160]; };

/* Parse a response header block (an HTTP/1.1 block, or the HTTP/2-synthesized
 * "HTTP/2 NNN\r\nname: value\r\n…"): status + Location + Content-Type into `meta`,
 * and the framing (chunked / Content-Length) for the body reader. Shared by the
 * h1.1 socket reader and the h2 path. */
static void parse_resp_headers(const char *hdr, struct resp_meta *meta, int *chunked, long *clen)
{
	meta->status = 0; meta->location[0] = '\0'; meta->ctype[0] = '\0';
	*chunked = 0; *clen = -1;

	{ const char *s = hdr; while (*s && *s != ' ') s++; while (*s == ' ') s++;
	  while (*s >= '0' && *s <= '9') meta->status = meta->status * 10 + (*s++ - '0'); }

	for (const char *line = hdr; *line; ) {
		if (starts_ci(line, "content-length:")) {
			const char *v = line + 15; while (*v == ' ') v++;
			*clen = 0; while (*v >= '0' && *v <= '9') *clen = *clen * 10 + (*v++ - '0');
		} else if (starts_ci(line, "transfer-encoding:")) {
			for (const char *q = line; *q && *q != '\n'; q++)
				if (starts_ci(q, "chunked")) { *chunked = 1; break; }
		} else if (starts_ci(line, "location:")) {
			const char *v = line + 9; while (*v == ' ') v++;
			int k = 0;
			while (*v && *v != '\r' && *v != '\n' && k < (int)sizeof meta->location - 1)
				meta->location[k++] = *v++;
			meta->location[k] = '\0';
		} else if (starts_ci(line, "content-type:")) {
			const char *v = line + 13; while (*v == ' ') v++;
			int k = 0;
			while (*v && *v != '\r' && *v != '\n' && k < (int)sizeof meta->ctype - 1)
				meta->ctype[k++] = *v++;
			meta->ctype[k] = '\0';
		}
		const char *e = line; while (*e && *e != '\n') e++;
		if (!*e) break;
		line = e + 1;
	}
}

/* Read a response's body (chunked / content-length / read-to-close) off an open
 * sub-resource request into a freshly malloc'd buffer. Fills *meta (status +
 * Location) and returns the body (caller frees) with *outlen set, for ANY status
 * — the caller decides what to do with a redirect or an error. NULL only on a
 * connection error / OOM. Bounded by RAW_MAX. */
static unsigned char *read_http_body(struct ireader *r, int *outlen, struct resp_meta *meta,
                                     char *hdr_out, int hdr_cap)
{
	char hdr[4096]; int hl = 0, m = 0;
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
	if (hdr_out && hdr_cap > 0) {           /* hand the raw block to the caller */
		int k = 0; for (; hdr[k] && k < hdr_cap - 1; k++) hdr_out[k] = hdr[k]; hdr_out[k] = '\0';
	}

	int chunked; long clen;
	parse_resp_headers(hdr, meta, &chunked, &clen);

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

static int is_redirect(int s)
{ return s == 301 || s == 302 || s == 303 || s == 307 || s == 308; }

/* Fetch an absolute URL as a sub-resource (GET, follow redirects): the 200 body
 * (caller frees) + *outlen, or NULL. A thin wrapper over browser_fetch_url (the
 * one single-request transport: h2 when the server offers it, else h1.1), which
 * also gives sub-resources HTTP/2. Sends cookies, HSTS-upgrades, runs concurrently
 * (the kernel TLS is multi-flow). */
static unsigned char *subres_get(const char *absurl, const char *accept, int *outlen)
{
	int status = 0;
	unsigned char *body = browser_fetch_url(absurl, "GET", 0, 0, 0, accept,
	                                        &status, 0, 0, outlen);
	if (body && status != 200) { free(body); return 0; }
	return body;
}

/* Build an arbitrary-method request (for the JS fetch()): like build_get but with
 * the method, an optional Content-Type, and a Content-Length when a body follows. */
static void build_request(char *req, int *n, int cap, const struct purl *u,
                          const char *method, const char *accept, const char *ctype, int blen)
{
	sapp(req, n, cap, method); sapp(req, n, cap, " ");
	sapp(req, n, cap, u->path);
	sapp(req, n, cap, " HTTP/1.1\r\nHost: ");
	sapp(req, n, cap, u->host);
	if (!((u->tls && u->port == 443) || (!u->tls && u->port == 80))) {
		sapp(req, n, cap, ":"); uapp(req, n, cap, (unsigned)u->port);
	}
	{ char ck[1024]; if (cookie_header(u->host, ck, sizeof ck) > 0) { sapp(req, n, cap, "\r\nCookie: "); sapp(req, n, cap, ck); } }
	sapp(req, n, cap, "\r\nUser-Agent: " BR_UA "\r\nAccept: ");
	sapp(req, n, cap, accept && accept[0] ? accept : "*/*");
	if (ctype && ctype[0]) { sapp(req, n, cap, "\r\nContent-Type: "); sapp(req, n, cap, ctype); }
	if (blen > 0) { sapp(req, n, cap, "\r\nContent-Length: "); uapp(req, n, cap, (unsigned)blen); }
	sapp(req, n, cap, "\r\nConnection: close\r\n\r\n");
}

/* h2 response body sink for browser_fetch_url: append DATA into a growable buffer. */
struct h2buf { unsigned char *body; int cap, blen; };
static int h2buf_cb(void *ctx, const unsigned char *p, int n)
{
	struct h2buf *h = ctx;
	for (int i = 0; i < n; i++) if (body_put(&h->body, &h->cap, &h->blen, p[i]) < 0) return 1;
	return 0;
}

/* The single-request transport core: one GET/POST (h2 when offered, else h1.1),
 * following redirects, returning the body for ANY final status. The final hop's
 * response metadata, absolute URL and raw header block are reported out so a
 * caller that needs more than status+ctype (the iframe path, which reads
 * X-Frame-Options / CSP) gets them without a second request. NULL on a
 * connection/OOM error. Worker-thread only. */
static unsigned char *fetch_loop(const char *url, const char *method,
                                 const unsigned char *reqbody, int reqlen, const char *req_ctype,
                                 const char *accept, struct resp_meta *meta_out,
                                 char *final_url, int fu_cap, char *hdr_out, int hdr_cap, int *outlen)
{
	char full[URL_MAX]; { int k = 0; full[0] = '\0'; sapp(full, &k, sizeof full, url); }
	int is_get = !method || streq_ci(method, "GET") || streq_ci(method, "HEAD");
	if (!method) method = "GET";
	if (!accept || !accept[0]) accept = "*/*";
	*outlen = 0;

	for (int hop = 0; hop <= MAX_REDIR; hop++) {
		struct purl u;
		struct resp_meta meta;
		unsigned char *body = 0;
		int blen = 0;
		char rawhdr[4096]; rawhdr[0] = '\0';
		if (!parse_url(full, &u)) return 0;
		if (!u.tls && hsts_has(u.host)) { u.tls = 1; if (u.port == 80) u.port = 443; }

		int sock = net_connect(u.host, u.port, u.tls ? SK_TLS_H2 : SK_TCP);
		if (sock < 0) return 0;

		if (u.tls && net_alpn(sock) == ALPN_H2) {
			/* HTTP/2. :authority + lowercased headers; no accept-encoding so the
			 * body comes back plain (no decompression needed here). */
			char auth[URL_HOST_MAX + 8]; int an = 0;
			sapp(auth, &an, sizeof auth, u.host);
			if (u.port != 443) { sapp(auth, &an, sizeof auth, ":"); uapp(auth, &an, sizeof auth, (unsigned)u.port); }
			char xh[1408]; int xn = 0; char ck[1024];
			if (cookie_header(u.host, ck, sizeof ck) > 0) { sapp(xh, &xn, sizeof xh, "cookie: "); sapp(xh, &xn, sizeof xh, ck); sapp(xh, &xn, sizeof xh, "\r\n"); }
			sapp(xh, &xn, sizeof xh, "accept: "); sapp(xh, &xn, sizeof xh, accept); sapp(xh, &xn, sizeof xh, "\r\n");
			if (req_ctype && req_ctype[0]) { sapp(xh, &xn, sizeof xh, "content-type: "); sapp(xh, &xn, sizeof xh, req_ctype); sapp(xh, &xn, sizeof xh, "\r\n"); }
			struct h2_opts opts = { BR_UA, xh, 0 };
			struct h2buf hb; hb.cap = 65536; hb.blen = 0; hb.body = malloc((unsigned)hb.cap);
			if (!hb.body) { net_close(sock); return 0; }
			int hlen = 0;
			int rc = h2_run_body(sock, 1, auth, method, u.path, &opts, reqbody, reqlen,
			                     rawhdr, sizeof rawhdr, &hlen, h2buf_cb, &hb);
			net_close(sock);
			if (rc < 0) { if (hb.body) free(hb.body); return 0; }
			int dchunk; long dclen;
			parse_resp_headers(rawhdr, &meta, &dchunk, &dclen);
			body = hb.body ? hb.body : malloc(1);
			blen = hb.blen;
		} else {
			char req[2048]; int n = 0;
			build_request(req, &n, sizeof req, &u, method, accept, req_ctype, reqlen);
			if (net_send(sock, req, n) < 0) { net_close(sock); return 0; }
			if (reqbody && reqlen > 0 && net_send(sock, (const char *)reqbody, reqlen) < 0) { net_close(sock); return 0; }
			struct ireader r = { sock, { 0 }, 0, 0, 0 };
			body = read_http_body(&r, &blen, &meta, rawhdr, sizeof rawhdr);
			net_close(sock);
		}
		if (!body) return 0;

		if (is_get && is_redirect(meta.status) && meta.location[0]) {
			char next[URL_MAX];
			resolve_url(full, meta.location, next, sizeof next);
			int k = 0; full[0] = '\0'; sapp(full, &k, sizeof full, next);
			free(body);
			continue;
		}
		if (meta_out) *meta_out = meta;
		if (final_url && fu_cap) { int k = 0; final_url[0] = '\0'; sapp(final_url, &k, fu_cap, full); }
		if (hdr_out && hdr_cap) { int k = 0; for (; rawhdr[k] && k < hdr_cap - 1; k++) hdr_out[k] = rawhdr[k]; hdr_out[k] = '\0'; }
		*outlen = blen;
		devtools_net_add(method, full, meta.status, meta.ctype, blen);
		return body;
	}
	return 0;
}

/* The browser's single-request transport (sub-resources via subres_get, and the JS
 * fetch()): one request with `method` (+ optional body), returning the response
 * body (caller frees) for ANY status, with the status and Content-Type reported so
 * fetch()'s Response is real. A thin wrapper over fetch_loop. Worker-thread only. */
unsigned char *browser_fetch_url(const char *url, const char *method,
                                 const unsigned char *reqbody, int reqlen, const char *req_ctype,
                                 const char *accept, int *status, char *out_ctype, int oc_cap, int *outlen)
{
	struct resp_meta meta;
	*status = 0; *outlen = 0; if (out_ctype && oc_cap) out_ctype[0] = '\0';
	unsigned char *body = fetch_loop(url, method, reqbody, reqlen, req_ctype, accept,
	                                 &meta, 0, 0, 0, 0, outlen);
	if (!body) return 0;
	*status = meta.status;
	if (out_ctype && oc_cap) { int k = 0; for (; meta.ctype[k] && k < oc_cap - 1; k++) out_ctype[k] = meta.ctype[k]; out_ctype[k] = '\0'; }
	return body;
}

/* --- iframe fetching + embedding policy ------------------------------------ *
 * Whether a framed document may be displayed is the framed document's own call,
 * expressed by X-Frame-Options or the CSP `frame-ancestors` directive; the
 * embedder (the page, or an outer frame) is matched against it by origin. */

/* Two URLs share an origin when scheme (tls), host and port all match. */
static int origin_eq(const char *a, const char *b)
{
	struct purl ua, ub;
	if (!parse_url(a, &ua) || !parse_url(b, &ub)) return 0;
	return ua.tls == ub.tls && ua.port == ub.port && streq_ci(ua.host, ub.host);
}

static int contains_seq(const char *hay, const char *needle)
{
	for (; *hay; hay++) if (starts_ci(hay, needle)) return 1;
	return 0;
}

/* The value of CSP directive `name` (case-insensitive, at a ';'/start boundary),
 * or NULL. Returns a pointer into `csp` past the name + any spaces. */
static const char *csp_directive(const char *csp, const char *name)
{
	if (!csp) return 0;
	int nl = (int)strlen(name);
	for (const char *p = csp; *p; p++) {
		if (p != csp && p[-1] != ';' && p[-1] != ' ') continue;
		if (!starts_ci(p, name)) continue;
		const char *q = p + nl;
		if (*q != ' ' && *q != ';' && *q != '\0') continue;        /* whole token */
		while (*q == ' ') q++;
		return q;
	}
	return 0;
}

/* Does one frame-ancestors source token [tok,tend) admit the embedder `parent`
 * (`framed` is the framed doc's own URL, used by 'self')? Handles 'self', '*', a
 * scheme://host[:port] origin, a "*.suffix" host, and a bare host. */
static int csp_src_admits(const char *tok, const char *tend, const char *parent, const char *framed)
{
	int n = (int)(tend - tok);
	if (n == 6 && starts_ci(tok, "'self'")) return origin_eq(parent, framed);
	if (n == 1 && tok[0] == '*') return 1;

	char s[256]; int k = 0;
	for (; k < n && k < (int)sizeof s - 1; k++) s[k] = tok[k];
	s[k] = '\0';
	struct purl pp;
	if (!parse_url(parent, &pp)) return 0;
	if (contains_seq(s, "://")) return origin_eq(parent, s);   /* full origin */
	if (s[0] == '*' && s[1] == '.') {                          /* host suffix */
		const char *suf = s + 1;
		int hl = (int)strlen(pp.host), sl = (int)strlen(suf);
		return hl >= sl && streq_ci(pp.host + (hl - sl), suf);
	}
	return streq_ci(pp.host, s);                               /* bare host */
}

/* Decide whether `framed` may be embedded by `parent` given its X-Frame-Options
 * and Content-Security-Policy. frame-ancestors, when present, supersedes XFO. A
 * short human reason is written to `reason` when embedding is refused. */
static int frame_embedding_allowed(const char *parent, const char *framed,
                                   const char *xfo, const char *csp, char *reason, int rcap)
{
	if (reason && rcap) reason[0] = '\0';

	const char *fa = csp_directive(csp, "frame-ancestors");
	if (fa) {
		const char *p = fa;
		int any = 0, ok = 0;
		while (*p && *p != ';') {
			while (*p == ' ') p++;
			const char *t = p;
			while (*p && *p != ' ' && *p != ';') p++;
			if (p == t) break;
			any = 1;
			if ((p - t) == 6 && starts_ci(t, "'none'")) {
				if (reason && rcap) { int k = 0; sapp(reason, &k, rcap, "CSP frame-ancestors 'none'"); }
				return 0;
			}
			if (csp_src_admits(t, p, parent, framed)) ok = 1;
		}
		if (!any || ok) return 1;
		if (reason && rcap) { int k = 0; sapp(reason, &k, rcap, "CSP frame-ancestors"); }
		return 0;
	}

	if (xfo && xfo[0]) {
		const char *v = xfo; while (*v == ' ') v++;
		if (starts_ci(v, "deny")) {
			if (reason && rcap) { int k = 0; sapp(reason, &k, rcap, "X-Frame-Options: DENY"); }
			return 0;
		}
		if (starts_ci(v, "sameorigin")) {
			if (origin_eq(parent, framed)) return 1;
			if (reason && rcap) { int k = 0; sapp(reason, &k, rcap, "X-Frame-Options: SAMEORIGIN"); }
			return 0;
		}
		if (starts_ci(v, "allow-from")) {
			const char *u = v + 10; while (*u == ' ' || *u == '=') u++;
			char uri[URL_MAX]; int k = 0;
			for (; u[k] && u[k] != ' ' && u[k] != ';' && k < (int)sizeof uri - 1; k++) uri[k] = u[k];
			uri[k] = '\0';
			if (uri[0] && origin_eq(parent, uri)) return 1;
			if (reason && rcap) { int kk = 0; sapp(reason, &kk, rcap, "X-Frame-Options: ALLOW-FROM"); }
			return 0;
		}
		/* Unknown / malformed value: browsers ignore it, so do we. */
	}
	return 1;
}

/* Decode a data: URL into a fresh buffer (caller frees) + its media type, so a
 * `data:` iframe needs no network. base64 and percent-encoded bodies both work. */
static int rhex(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}
static int rb64(int c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}
static unsigned char *decode_data_url(const char *url, int *outlen, char *ctype, int cap)
{
	const char *p = url + 5;                          /* past "data:" */
	const char *comma = p;
	while (*comma && *comma != ',') comma++;
	if (*comma != ',') return 0;

	char mt[80]; int mi = 0, base64 = 0;
	const char *h = p;
	while (h < comma && *h != ';') { if (mi < (int)sizeof mt - 1) mt[mi++] = lower(*h); h++; }
	mt[mi] = '\0';
	while (h < comma) {
		h++;
		const char *tok = h;
		while (h < comma && *h != ';') h++;
		if (h - tok == 6 && starts_ci(tok, "base64")) base64 = 1;
	}
	{ int k = 0; ctype[0] = '\0'; sapp(ctype, &k, cap, mt[0] ? mt : "text/plain"); }

	const char *d = comma + 1;
	int cap2 = 4096, len = 0;
	unsigned char *out = malloc((unsigned)cap2);
	if (!out) return 0;
	if (base64) {
		int acc = 0, nb = 0;
		for (; *d && *d != '='; d++) {
			int v = rb64((unsigned char)*d);
			if (v < 0) continue;
			acc = (acc << 6) | v; nb += 6;
			if (nb >= 8) { nb -= 8; if (body_put(&out, &cap2, &len, (acc >> nb) & 0xff) < 0) break; }
		}
	} else {
		for (; *d; d++) {
			int c = (unsigned char)*d;
			if (c == '%') {
				int hi = rhex((unsigned char)d[1]);
				int lo = hi < 0 ? -1 : rhex((unsigned char)d[2]);
				if (lo >= 0) { c = hi * 16 + lo; d += 2; }
			}
			if (body_put(&out, &cap2, &len, c) < 0) break;
		}
	}
	*outlen = len;
	return out;
}

/* Drain the page's pending iframes: fetch each (data: inline, else GET), apply
 * the X-Frame-Options / CSP frame-ancestors policy against its embedder, and hand
 * the result back to the cache (html.c re-flattens the page to show it). Runs on
 * the worker thread, one frame at a time; nested frames surface on the next pass. */
void fetch_iframes(void)
{
	char url[URL_MAX], parent[URL_MAX];
	while (html_iframe_next_pending(url, sizeof url, parent, sizeof parent)) {
		if (starts_ci(url, "data:")) {
			char ctype[160]; int len = 0;
			unsigned char *body = decode_data_url(url, &len, ctype, sizeof ctype);
			if (body) { html_iframe_resolve(url, parent, FRAME_OK, "", body, len, ctype); free(body); }
			else        html_iframe_resolve(url, parent, FRAME_ERROR, "bad data: URL", 0, 0, 0);
			continue;
		}
		if (!starts_ci(url, "http://") && !starts_ci(url, "https://")) {
			html_iframe_resolve(url, parent, FRAME_ERROR, "unsupported scheme", 0, 0, 0);
			continue;
		}

		struct resp_meta meta; char final[URL_MAX]; char hdr[4096]; int len = 0;
		unsigned char *body = fetch_loop(url, "GET", 0, 0, 0, "text/html,*/*",
		                                 &meta, final, sizeof final, hdr, sizeof hdr, &len);
		if (!body) { html_iframe_resolve(url, parent, FRAME_ERROR, "no response", 0, 0, 0); continue; }
		if (meta.status < 200 || meta.status >= 300) {
			char r[40]; int k = 0; sapp(r, &k, sizeof r, "HTTP "); uapp(r, &k, sizeof r, (unsigned)meta.status);
			html_iframe_resolve(url, parent, FRAME_ERROR, r, 0, 0, 0);
			free(body); continue;
		}

		struct http_headers H; http_headers_parse(&H, hdr);
		const char *xfo = http_headers_get(&H, "x-frame-options");
		const char *csp = http_headers_get(&H, "content-security-policy");
		char reason[96];
		int ok = frame_embedding_allowed(parent, final[0] ? final : url, xfo, csp, reason, sizeof reason);
		http_headers_free(&H);

		html_iframe_resolve(url, parent, ok ? FRAME_OK : FRAME_BLOCKED, reason, body, len, meta.ctype);
		free(body);
	}
}

/* Normalise a URL's origin (HSTS-upgrades http->https) into *u; returns 0 on a
 * bad URL. */
static int origin_of(const char *url, struct purl *u)
{
	if (!parse_url(url, u)) return 0;
	if (!u->tls && hsts_has(u->host)) { u->tls = 1; if (u->port == 80) u->port = 443; }
	return 1;
}

/* Fetch n absolute URLs into bodies[i] (malloc'd, caller frees, NULL on failure)
 * + lens[i]. Same-origin HTTPS URLs are multiplexed over one h2 connection each
 * (h1.1 fallback per failed stream / no-h2 origin); plain-http URLs go over
 * HTTP/1.1 one at a time. Single-threaded — the h2 engine's state is global — so
 * it suits a small set of already-sequential sub-resources (stylesheets). */
void subres_get_batch(char *const *urls, int n, const char *accept,
                      unsigned char **bodies, int *lens)
{
	if (n <= 0) return;
	for (int i = 0; i < n; i++) { bodies[i] = 0; lens[i] = 0; }

	/* `handled` tracks which URLs a same-origin h2 batch already pulled. Sized to
	 * n (no fixed cap): a page may reference any number of sub-resources. Each h2
	 * round still gathers at most H2_FETCH_MAX same-origin streams per connection
	 * and opens another connection for the rest, so this stays unbounded. */
	char *handled = calloc((unsigned)n, 1);
	if (!handled) {                          /* OOM: degrade to one fetch at a time */
		for (int i = 0; i < n; i++) bodies[i] = subres_get(urls[i], accept, &lens[i]);
		return;
	}

	for (int i = 0; i < n; i++) {
		if (handled[i]) continue;
		struct purl u;
		if (!origin_of(urls[i], &u)) { handled[i] = 1; continue; }
		if (!u.tls) {                                  /* plain http: one at a time */
			bodies[i] = subres_get(urls[i], accept, &lens[i]);
			handled[i] = 1;
			continue;
		}

		/* Gather every same-origin HTTPS URL into one h2 batch. */
		char (*paths)[URL_MAX] = malloc((unsigned)H2_FETCH_MAX * URL_MAX);
		if (!paths) { bodies[i] = subres_get(urls[i], accept, &lens[i]); handled[i] = 1; continue; }
		struct h2_fetch reqs[H2_FETCH_MAX];
		int bi[H2_FETCH_MAX], nb = 0;
		for (int j = i; j < n && nb < H2_FETCH_MAX; j++) {
			if (handled[j]) continue;
			struct purl v;
			if (!origin_of(urls[j], &v)) { handled[j] = 1; continue; }
			if (!v.tls || !streq_ci(v.host, u.host) || v.port != u.port) continue;
			int k = 0;
			for (; v.path[k] && k < URL_MAX - 1; k++) paths[nb][k] = v.path[k];
			paths[nb][k] = '\0';
			reqs[nb].path = paths[nb];
			reqs[nb].status = 0; reqs[nb].body = 0; reqs[nb].blen = 0; reqs[nb].failed = 0;
			bi[nb] = j; handled[j] = 1; nb++;
		}

		int sock = net_connect(u.host, u.port, SK_TLS_H2);
		if (sock >= 0 && net_alpn(sock) == ALPN_H2) {
			char auth[URL_HOST_MAX + 8]; int an = 0;
			sapp(auth, &an, sizeof auth, u.host);
			if (u.port != 443) { sapp(auth, &an, sizeof auth, ":"); uapp(auth, &an, sizeof auth, (unsigned)u.port); }
			char xh[1280]; int xn = 0; char ck[1024];
			if (cookie_header(u.host, ck, sizeof ck) > 0) {
				sapp(xh, &xn, sizeof xh, "cookie: "); sapp(xh, &xn, sizeof xh, ck); sapp(xh, &xn, sizeof xh, "\r\n");
			}
			sapp(xh, &xn, sizeof xh, "accept: "); sapp(xh, &xn, sizeof xh, accept); sapp(xh, &xn, sizeof xh, "\r\n");
			struct h2_opts o = { BR_UA, xh, 0 };
			h2_fetch_many(sock, 1, auth, &o, reqs, nb, 0, 0);   /* accumulate */
			net_close(sock);
			for (int b = 0; b < nb; b++) {
				if (!reqs[b].failed && reqs[b].body) {
					bodies[bi[b]] = reqs[b].body; lens[bi[b]] = reqs[b].blen;
					devtools_net_add("GET", urls[bi[b]], reqs[b].status, "", reqs[b].blen);
				} else { if (reqs[b].body) free(reqs[b].body);   /* h1.1 retry (logs itself) */
				       bodies[bi[b]] = subres_get(urls[bi[b]], accept, &lens[bi[b]]); }
			}
		} else {
			if (sock >= 0) net_close(sock);
			for (int b = 0; b < nb; b++)                       /* no h2: h1.1 each */
				bodies[bi[b]] = subres_get(urls[bi[b]], accept, &lens[bi[b]]);
		}
		free(paths);
	}
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
		if (!B->imgs[i].lazy && !B->imgs[i].ok) { got = i; break; }  /* skip h2-done */
	}
	img_unlock();
	return got;
}

/* Commit a fetched image body into img_pool for image `idx`: an SVG is
 * rasterized to RGBA, a PNG stored verbatim. Sets im->ok last so the UI only
 * draws a fully-committed image. Does NOT free `body` (the caller owns it).
 * Shared by the HTTP/1.1 fetcher and the h2 multiplexed prefetch. The short
 * img_lock guards only the img_pool_len bump; the copy is to a disjoint region. */
static void commit_image(int idx, const unsigned char *body, int blen)
{
	struct inl_img *im = &B->imgs[idx];

	if (svg_probe((const char *)body, blen)) {         /* SVG -> rasterize to RGBA */
		int sw = 0, sh = 0;
		if (!svg_size((const char *)body, blen, &sw, &sh) || sw <= 0 || sh <= 0) { sw = 128; sh = 128; }
		int maxe = sw > sh ? sw : sh;                  /* bound source resolution  */
		if (maxe > 256) { sw = sw * 256 / maxe; sh = sh * 256 / maxe; }
		if (sw < 1) sw = 1;
		if (sh < 1) sh = 1;
		unsigned *argb = malloc((unsigned)sw * sh * 4);
		if (argb && svg_render((const char *)body, blen, sw, sh, argb)) {
			int bytes = sw * sh * 4;
			int off = -1;
			img_lock();
			if (B->img_pool_len + bytes <= IMG_POOL) { off = B->img_pool_len; B->img_pool_len += bytes; }
			img_unlock();
			if (off >= 0) {
				unsigned char *dst = B->img_pool + off;   /* ARGB int -> RGBA bytes */
				for (int i = 0; i < sw * sh; i++) {
					unsigned p = argb[i];
					dst[i*4+0] = (p >> 16) & 0xff;
					dst[i*4+1] = (p >> 8) & 0xff;
					dst[i*4+2] = p & 0xff;
					dst[i*4+3] = (p >> 24) & 0xff;
				}
				im->coff = off; im->clen = bytes;
				im->iw = (unsigned)sw; im->ih = (unsigned)sh;
				im->is_argb = 1;
				im->ok = 1;                /* set last: the UI draws once ok is seen */
				B->seq++;
			}
		}
		free(argb);
		return;
	}

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
			if (okdim) { im->iw = iw; im->ih = ih; im->uid = ++B->img_uid_next; }
			im->ok = okdim;            /* set last: the UI draws once ok is seen */
			B->seq++;                  /* repaint as each image lands            */
		}
	}
}

/* Fetch inline image `i` over its own HTTP/1.1 socket and commit it. Safe to run
 * on several threads at once (the h1.1 pool path). */
static void fetch_image(int i, const char *page_url)
{
	B->imgs[i].ok = 0;
	char full[URL_MAX];
	resolve_url(page_url, B->imgs[i].href, full, sizeof full);
	int blen = 0;
	unsigned char *body = subres_get(full, "image/png,image/svg+xml,*/*", &blen);
	if (!body) return;
	commit_image(i, body, blen);
	free(body);
}

static int favicon_decode(const unsigned char *r, int n);   /* defined below */

/* on_done bridge for the h2 batch: map a batch request index back to its target
 * (idxs[]) and consume that resource as its stream lands. An index of FAV_IDX is
 * the page favicon (decoded into B->favicon); anything else is an image index. */
#define FAV_IDX (-1)
struct h2img_ctx { const int *idxs; };
static void h2img_commit(void *ctx, int b, const unsigned char *body, int blen)
{
	const struct h2img_ctx *c = ctx;
	char full[URL_MAX];
	if (c->idxs[b] == FAV_IDX) {
		resolve_url(g_img_page, B->favicon_href[0] ? B->favicon_href : "/favicon.ico", full, sizeof full);
		devtools_net_add("GET", full, 200, "", blen);
		if (favicon_decode(body, blen)) { B->favicon_ok = 1; B->seq++; }
	} else {
		resolve_url(g_img_page, B->imgs[c->idxs[b]].href, full, sizeof full);
		devtools_net_add("GET", full, 200, "image/*", blen);
		commit_image(c->idxs[b], body, blen);
	}
}

/* Multiplex the page's same-origin HTTPS eager images over ONE h2 connection
 * (up to H2_FETCH_MAX). Committed images get ok=1, so the h1.1 pool below skips
 * them; cross-origin / http / h2-unavailable / overflow / per-stream-failed
 * images fall through to the pool. Single-threaded by necessity: the h2 engine's
 * HPACK + frame state is process-global, so only one h2 connection may be live. */
static void h2_prefetch(const char *page_url)
{
	struct purl pu;
	if (!parse_url(page_url, &pu) || !pu.tls) return;

	char (*paths)[URL_MAX] = malloc((unsigned)H2_FETCH_MAX * URL_MAX);
	if (!paths) return;
	struct h2_fetch reqs[H2_FETCH_MAX];
	int idxs[H2_FETCH_MAX], nb = 0;

	for (int i = 0; i < B->n_imgs && nb < H2_FETCH_MAX; i++) {
		struct inl_img *im = &B->imgs[i];
		if (im->lazy || im->ok) continue;
		char full[URL_MAX];
		resolve_url(page_url, im->href, full, sizeof full);
		struct purl u;
		if (!parse_url(full, &u)) continue;
		if (!u.tls && hsts_has(u.host)) { u.tls = 1; if (u.port == 80) u.port = 443; }
		if (!u.tls || !streq_ci(u.host, pu.host) || u.port != pu.port)
			continue;                                  /* same origin only */
		int k = 0;
		for (; u.path[k] && k < URL_MAX - 1; k++) paths[nb][k] = u.path[k];
		paths[nb][k] = '\0';
		reqs[nb].path = paths[nb];
		reqs[nb].status = 0; reqs[nb].body = 0; reqs[nb].blen = 0; reqs[nb].failed = 0;
		idxs[nb] = i;
		nb++;
	}

	/* Fold the favicon onto the same connection when it's same-origin HTTPS, so it
	 * rides the images' h2 stream batch instead of opening its own. Cross-origin /
	 * http favicons (and a failed stream) are left for fetch_favicon's h1.1 path,
	 * which runs only when favicon_ok is still 0. */
	if (nb < H2_FETCH_MAX) {
		char fav[URL_MAX];
		resolve_url(page_url, B->favicon_href[0] ? B->favicon_href : "/favicon.ico",
		            fav, sizeof fav);
		struct purl fu;
		if (parse_url(fav, &fu) && fu.tls &&
		    streq_ci(fu.host, pu.host) && fu.port == pu.port) {
			int k = 0;
			for (; fu.path[k] && k < URL_MAX - 1; k++) paths[nb][k] = fu.path[k];
			paths[nb][k] = '\0';
			reqs[nb].path = paths[nb];
			reqs[nb].status = 0; reqs[nb].body = 0; reqs[nb].blen = 0; reqs[nb].failed = 0;
			idxs[nb] = FAV_IDX;
			nb++;
		}
	}
	if (nb == 0) { free(paths); return; }

	int sock = net_connect(pu.host, pu.port, SK_TLS_H2);
	if (sock < 0) { free(paths); return; }
	if (net_alpn(sock) != ALPN_H2) { net_close(sock); free(paths); return; }   /* pool handles */

	char auth[URL_HOST_MAX + 8]; int an = 0;
	sapp(auth, &an, sizeof auth, pu.host);
	if (pu.port != 443) { sapp(auth, &an, sizeof auth, ":"); uapp(auth, &an, sizeof auth, (unsigned)pu.port); }

	char xh[1280]; int xn = 0; char ck[1024];
	if (cookie_header(pu.host, ck, sizeof ck) > 0) {
		sapp(xh, &xn, sizeof xh, "cookie: ");
		sapp(xh, &xn, sizeof xh, ck);
		sapp(xh, &xn, sizeof xh, "\r\n");
	}
	sapp(xh, &xn, sizeof xh, "accept: image/png,image/svg+xml,*/*\r\n");
	struct h2_opts opts = { BR_UA, xh, 0 };

	/* Commit each image the moment its stream finishes (h2_fetch_many's on_done),
	 * so they pop in progressively rather than all at once after the batch. Each
	 * commit sets ok=1 + bumps B->seq, so the UI re-lays-out as they land; a
	 * failed/empty stream leaves ok=0 and the h1.1 pool retries it. */
	struct h2img_ctx hc = { idxs };
	h2_fetch_many(sock, 1, auth, &opts, reqs, nb, h2img_commit, &hc);
	net_close(sock);
	free(paths);
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

/* Fetch the page's *eager* inline images (loading != "lazy"): first multiplex the
 * same-origin HTTPS ones over a single h2 connection (h2_prefetch), then fetch
 * whatever is left over the HTTP/1.1 thread pool — spawn a few helper threads,
 * fetch alongside them on this (worker) thread, then join. loading="lazy" images
 * wait for fetch_lazy_visible() when scrolled in. */
void fetch_inline_images(const char *page_url)
{
	B->img_pool_len = 0;
	{ int k = 0; g_img_page[0] = '\0'; sapp(g_img_page, &k, sizeof g_img_page, page_url); }
	g_img_next = 0;

	/* First multiplex the same-origin HTTPS images over one h2 connection; the
	 * rest (cross-origin, http, or h2-unavailable) go through the h1.1 pool. */
	h2_prefetch(page_url);

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

/* Decode `buf` (a fetched favicon) into B->favicon; returns 1 on success. */
static int favicon_decode(const unsigned char *r, int n)
{
	if (n < 4) return 0;
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
	if (svg_probe((const char *)r, n))                            /* SVG favicon */
		return svg_render((const char *)r, n, FAVICON_W, FAVICON_H, B->favicon);
	return 0;                                  /* GIF/BMP favicons unsupported */
}

/* Fetch and decode the page favicon into B->favicon, from the <link rel=icon>
 * href the parser found, else /favicon.ico. subres_get follows CDN redirects and
 * uses its own socket/buffer, so this no longer clobbers B->raw. Leaves
 * favicon_ok 0 on any failure (the tab falls back to the globe). */
void fetch_favicon(const char *page_url)
{
	if (B->favicon_ok) return;       /* already decoded on the image h2 batch */
	char full[URL_MAX];
	resolve_url(page_url, B->favicon_href[0] ? B->favicon_href : "/favicon.ico",
	            full, sizeof full);
	int len = 0;
	unsigned char *body = subres_get(full, "image/png,image/x-icon,image/svg+xml,*/*", &len);
	if (!body) return;
	if (favicon_decode(body, len)) B->favicon_ok = 1;
	free(body);
}
