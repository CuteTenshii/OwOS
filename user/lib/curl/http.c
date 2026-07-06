/* http.c — libcurl: HTTP/1.1 over the net_* syscalls (see http.h). */

#include "owos.h"
#include "url.h"
#include "http.h"
#include "headers.h"
#include "h2.h"
#include "h3.h"
#include "netconn.h"

/* ---- buffered transport reader ----------------------------------------- */

struct rdr {
	netconn      *conn;
	unsigned char buf[2048];
	int           pos, len, eof;
};

/* Next response byte; -1 at EOF, -2 transport error, -3 interrupted. */
static int rgetc(struct rdr *r)
{
	if (r->pos >= r->len) {
		if (r->eof)
			return -1;
		int n = nc_read(r->conn, r->buf, (int)sizeof r->buf);
		if (n == NET_EINTR)
			return -3;
		if (n < 0)
			return -2;
		if (n == 0) {
			r->eof = 1;
			return -1;
		}
		r->pos = 0;
		r->len = n;
	}
	return r->buf[r->pos++];
}

/* ---- tiny string helpers ----------------------------------------------- */

static void sapp(char *dst, int *n, int cap, const char *s)
{
	while (*s && *n < cap - 1)
		dst[(*n)++] = *s++;
	dst[*n] = '\0';
}

/* ---- body delivery: batch bytes, flush to the sink --------------------- */

struct emitter {
	http_body_fn  fn;
	void         *ctx;
	unsigned char buf[1024];
	int           len;
	int           aborted;
};

static void emit_flush(struct emitter *e)
{
	if (e->len && e->fn && !e->aborted) {
		if (e->fn(e->ctx, e->buf, e->len) != 0)
			e->aborted = 1;
	}
	e->len = 0;
}

static void emit_byte(struct emitter *e, int b)
{
	if (e->len == (int)sizeof e->buf)
		emit_flush(e);
	e->buf[e->len++] = (unsigned char)b;
}

/* ---- request ----------------------------------------------------------- */

/* ---- HTTP/1.1 exchange over an already-open socket --------------------- */

/* Send the request line + headers and stream the response over `conn` (which
 * the caller opened and will close). Identical framing for plaintext and TLS.
 * Returns the HTTP status or a negative HTTP_E* code. */
static int http1_exchange(netconn *conn, const char *method, const struct purl *u,
                          const char *extra_headers, char *hdr, int hdrcap,
                          int *hdrlen, http_body_fn body, void *ctx)
{
	if (!method)
		method = "GET";

	/* Build and send the request line + headers. */
	char req[1024];
	int n = 0;
	sapp(req, &n, (int)sizeof req, method);
	sapp(req, &n, (int)sizeof req, " ");
	sapp(req, &n, (int)sizeof req, u->path);
	sapp(req, &n, (int)sizeof req, " HTTP/1.1\r\nHost: ");
	sapp(req, &n, (int)sizeof req, u->host);
	sapp(req, &n, (int)sizeof req, "\r\nConnection: close\r\n");
	if (extra_headers)                              /* curl -H lines, incl. User-Agent */
		sapp(req, &n, (int)sizeof req, extra_headers);
	sapp(req, &n, (int)sizeof req, "\r\n");
	if (nc_write(conn, req, n) < 0)
		return HTTP_ESEND;

	struct rdr r;
	r.conn = conn; r.pos = r.len = r.eof = 0;

	/* Read the header block through the blank line ("\r\n\r\n"). */
	char hbuf[4096];
	int hl = 0, match = 0;
	for (;;) {
		int ci = rgetc(&r);
		if (ci == -3) return HTTP_EINTR;
		if (ci < 0)   return HTTP_ENORESP;
		char c = (char)ci;
		if (hl < (int)sizeof hbuf - 1)
			hbuf[hl++] = c;
		if (c == '\r')
			match = (match == 0) ? 1 : (match == 2 ? 3 : 1);
		else if (c == '\n')
			match = (match == 1) ? 2 : (match == 3 ? 4 : 0);
		else
			match = 0;
		if (match == 4)
			break;
	}
	hbuf[hl] = '\0';

	if (hdr && hdrcap > 0) {
		int i = 0;
		for (; i < hl && i < hdrcap - 1; i++)
			hdr[i] = hbuf[i];
		hdr[i] = '\0';
		if (hdrlen) *hdrlen = i;
	}

	/* Status code + body framing (chunked > Content-Length > read-until-close),
	 * via the shared header object. */
	struct http_headers H;
	http_headers_parse(&H, hbuf);
	int status = H.status;
	const char *te = http_headers_get(&H, "transfer-encoding");
	const char *cl = http_headers_get(&H, "content-length");
	int chunked = te && http_header_has_token(te, "chunked");
	int have_len = 0;
	unsigned content_len = 0;
	if (cl) { bool ok; content_len = http_parse_uint(cl, &ok); have_len = ok; }
	http_headers_free(&H);

	struct emitter e;
	e.fn = body; e.ctx = ctx; e.len = 0; e.aborted = 0;
	int rc = status;

	if (chunked) {
		for (;;) {
			unsigned sz = 0;
			int c, any = 0;
			while ((c = rgetc(&r)) >= 0 && c != '\r' && c != ';') {
				int d;
				if (c >= '0' && c <= '9')      d = c - '0';
				else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
				else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
				else break;
				sz = sz * 16 + (unsigned)d;
				any = 1;
			}
			if (c == -3) { rc = HTTP_EINTR; goto done; }
			while (c >= 0 && c != '\n') c = rgetc(&r);   /* rest of size line */
			if (!any || sz == 0) break;
			for (unsigned i = 0; i < sz; i++) {
				int b = rgetc(&r);
				if (b == -3) { rc = HTTP_EINTR; goto done; }
				if (b < 0) goto done;
				emit_byte(&e, b);
				if (e.aborted) { rc = HTTP_EABORT; goto done; }
			}
			rgetc(&r);                                   /* trailing CR */
			rgetc(&r);                                   /* trailing LF */
		}
	} else if (have_len) {
		for (unsigned i = 0; i < content_len; i++) {
			int b = rgetc(&r);
			if (b == -3) { rc = HTTP_EINTR; goto done; }
			if (b < 0) break;
			emit_byte(&e, b);
			if (e.aborted) { rc = HTTP_EABORT; goto done; }
		}
	} else {
		int b;
		while ((b = rgetc(&r)) >= 0) {
			emit_byte(&e, b);
			if (e.aborted) { rc = HTTP_EABORT; goto done; }
		}
		if (b == -3) { rc = HTTP_EINTR; goto done; }
	}

done:
	emit_flush(&e);
	return rc;
}

/* ---- public requests --------------------------------------------------- */

/* host[:port] for the HTTP/2 :authority (port only when non-default). */
static void build_authority(char *out, int cap, const struct purl *u)
{
	int n = 0;
	sapp(out, &n, cap, u->host);
	if (u->port != (u->tls ? 443 : 80)) {
		char p[12]; int k = 0;
		unsigned v = (unsigned)u->port;
		if (v == 0) p[k++] = '0';
		else { char t[12]; int j = 0; while (v) { t[j++] = (char)('0' + v % 10); v /= 10; }
		       while (j) p[k++] = t[--j]; }
		p[k] = '\0';
		sapp(out, &n, cap, ":");
		sapp(out, &n, cap, p);
	}
}

/* The one request core: connect (offering ALPN "h2" when want_h2), then run the
 * exchange over HTTP/2 (h2c when plaintext, or when ALPN picked h2 over TLS) or
 * HTTP/1.1. `extra_headers` is an optional "name: value\r\n…" block appended to
 * the request (curl -H). http_request / http_request2 are thin wrappers. */
int http_request_ex(const char *method, const char *url, int want_h2, int want_h3,
                    const char *extra_headers, char *hdr, int hdrcap, int *hdrlen,
                    http_body_fn body, void *ctx)
{
	struct purl u;
	if (!parse_url(url, &u))
		return HTTP_EPARSE;

	/* HTTP/3 is https-only (QUIC carries its own TLS). It manages its own UDP
	 * socket, so dispatch before the TCP/TLS connect below. */
	if (want_h3 && u.tls) {
		char auth[URL_HOST_MAX + 8];
		build_authority(auth, (int)sizeof auth, &u);
		return h3_run(u.host, u.port, auth, method ? method : "GET", u.path,
		              extra_headers, hdr, hdrcap, hdrlen, body, ctx);
	}

	/* HTTP/2 runs over a netconn (user-space TLS via user/lib/tls, or plain TCP
	 * for h2c); offering ALPN "h2" when the origin is https. When ALPN falls back
	 * to HTTP/1.1, run the h1.1 exchange over the SAME netconn. */
	if (want_h2) {
		netconn *conn = nc_open(u.host, u.port, u.tls, /*offer_h2=*/1);
		if (!conn)
			return HTTP_ECONNECT;
		/* h2 for h2c (plaintext), or when ALPN selected it over TLS. */
		int use_h2 = !u.tls || nc_alpn(conn) == ALPN_H2;
		int rc;
		if (use_h2) {
			char auth[URL_HOST_MAX + 8];
			build_authority(auth, (int)sizeof auth, &u);
			struct h2_opts opts = { 0, extra_headers, 0 };
			rc = h2_run(conn, u.tls, auth, method ? method : "GET", u.path, &opts,
			            hdr, hdrcap, hdrlen, body, ctx);
		} else {
			rc = http1_exchange(conn, method, &u, extra_headers,
			                    hdr, hdrcap, hdrlen, body, ctx);
		}
		nc_close(conn);
		return rc;
	}

	netconn *conn = nc_open(u.host, u.port, u.tls, 0);
	if (!conn)
		return HTTP_ECONNECT;
	int rc = http1_exchange(conn, method, &u, extra_headers,
	                        hdr, hdrcap, hdrlen, body, ctx);
	nc_close(conn);
	return rc;
}

int http_request(const char *method, const char *url,
                 char *hdr, int hdrcap, int *hdrlen,
                 http_body_fn body, void *ctx)
{
	return http_request_ex(method, url, 0, 0, 0, hdr, hdrcap, hdrlen, body, ctx);
}

int http_request2(const char *method, const char *url,
                  char *hdr, int hdrcap, int *hdrlen,
                  http_body_fn body, void *ctx)
{
	return http_request_ex(method, url, 1, 0, 0, hdr, hdrcap, hdrlen, body, ctx);
}

int http_request3(const char *method, const char *url,
                  char *hdr, int hdrcap, int *hdrlen,
                  http_body_fn body, void *ctx)
{
	return http_request_ex(method, url, 0, 1, 0, hdr, hdrcap, hdrlen, body, ctx);
}

int http_get(const char *url, http_body_fn body, void *ctx)
{
	return http_request("GET", url, 0, 0, 0, body, ctx);
}
