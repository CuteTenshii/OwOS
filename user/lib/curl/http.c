/* http.c — libcurl: HTTP/1.1 over the net_* syscalls (see http.h). */

#include "owos.h"
#include "url.h"
#include "http.h"
#include "h2.h"

/* ---- buffered transport reader ----------------------------------------- */

struct rdr {
	int           sock;
	unsigned char buf[2048];
	int           pos, len, eof;
};

/* Next response byte; -1 at EOF, -2 transport error, -3 interrupted. */
static int rgetc(struct rdr *r)
{
	if (r->pos >= r->len) {
		if (r->eof)
			return -1;
		int n = net_recv(r->sock, r->buf, (int)sizeof r->buf);
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

static char lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int starts_ci(const char *s, const char *pre)
{
	while (*pre) {
		if (lower(*s) != lower(*pre))
			return 0;
		s++;
		pre++;
	}
	return 1;
}

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

/* Send the request line + headers and stream the response over `sock` (which
 * the caller opened and will close). Identical framing for plaintext and TLS.
 * Returns the HTTP status or a negative HTTP_E* code. */
static int http1_exchange(int sock, const char *method, const struct purl *u,
                          char *hdr, int hdrcap, int *hdrlen,
                          http_body_fn body, void *ctx)
{
	if (!method)
		method = "GET";

	/* Build and send the request line + headers. */
	char req[640];
	int n = 0;
	sapp(req, &n, (int)sizeof req, method);
	sapp(req, &n, (int)sizeof req, " ");
	sapp(req, &n, (int)sizeof req, u->path);
	sapp(req, &n, (int)sizeof req, " HTTP/1.1\r\nHost: ");
	sapp(req, &n, (int)sizeof req, u->host);
	sapp(req, &n, (int)sizeof req,
	     "\r\nUser-Agent: owos-curl\r\nConnection: close\r\n\r\n");
	if (net_send(sock, req, n) < 0)
		return HTTP_ESEND;

	struct rdr r;
	r.sock = sock; r.pos = r.len = r.eof = 0;

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

	/* Status code from "HTTP/1.1 NNN ...". */
	int status = 0;
	{
		const char *p = hbuf;
		while (*p && *p != ' ') p++;          /* skip "HTTP/1.1"            */
		while (*p == ' ') p++;
		for (int k = 0; k < 3 && p[k] >= '0' && p[k] <= '9'; k++)
			status = status * 10 + (p[k] - '0');
	}

	/* Body framing: chunked > Content-Length > read-until-close. */
	int chunked = 0, have_len = 0;
	unsigned content_len = 0;
	for (char *line = hbuf; *line; ) {
		if (starts_ci(line, "transfer-encoding:")) {
			for (char *q = line; *q && *q != '\n'; q++)
				if (starts_ci(q, "chunked")) { chunked = 1; break; }
		} else if (starts_ci(line, "content-length:")) {
			const char *v = line + 15;
			while (*v == ' ') v++;
			unsigned x = 0;
			while (*v >= '0' && *v <= '9')
				x = x * 10 + (unsigned)(*v++ - '0');
			content_len = x;
			have_len = 1;
		}
		char *eol = line;
		while (*eol && *eol != '\n') eol++;
		if (!*eol) break;
		line = eol + 1;
	}

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

static int connect_url(const struct purl *u)
{
	int sock = net_connect(u->host, u->port, u->tls);   /* u->tls: 0=TCP,1=TLS */
	if (sock < 0) {
		if (sock == NET_EINTR) return HTTP_EINTR;
		if (sock == -2)        return HTTP_ERESOLVE;
		return HTTP_ECONNECT;
	}
	return sock;
}

int http_request(const char *method, const char *url,
                 char *hdr, int hdrcap, int *hdrlen,
                 http_body_fn body, void *ctx)
{
	struct purl u;
	if (!parse_url(url, &u))
		return HTTP_EPARSE;
	int sock = connect_url(&u);
	if (sock < 0)
		return sock;
	int rc = http1_exchange(sock, method, &u, hdr, hdrcap, hdrlen, body, ctx);
	net_close(sock);
	return rc;
}

int http_request2(const char *method, const char *url,
                  char *hdr, int hdrcap, int *hdrlen,
                  http_body_fn body, void *ctx)
{
	struct purl u;
	if (!parse_url(url, &u))
		return HTTP_EPARSE;
	int sock = connect_url(&u);
	if (sock < 0)
		return sock;

	int rc;
	if (u.tls) {
		/* HTTP/2 over TLS is selected by ALPN ("h2"); until that's wired into
		 * the kernel TLS, speak HTTP/1.1 on the secure connection. */
		rc = http1_exchange(sock, method, &u, hdr, hdrcap, hdrlen, body, ctx);
	} else {
		/* h2c with prior knowledge: no Upgrade dance, just the h2 preface. */
		char auth[URL_HOST_MAX + 8];
		build_authority(auth, (int)sizeof auth, &u);
		rc = h2_request(sock, 0, auth, method ? method : "GET", u.path,
		                hdr, hdrcap, hdrlen, body, ctx);
	}
	net_close(sock);
	return rc;
}

int http_get(const char *url, http_body_fn body, void *ctx)
{
	return http_request("GET", url, 0, 0, 0, body, ctx);
}
