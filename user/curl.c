/* curl.c — a ring-3 HTTP/1.1 client.
 *
 * Unlike the old kernel built-in, the protocol lives entirely in user space:
 * this program parses the URL, builds the request, and frames the response
 * (status line, headers, chunked / Content-Length / read-until-close). The
 * kernel only lends it a transport through four syscalls — net_connect (DNS +
 * TCP + optional TLS), net_send, net_recv, net_close.
 *
 * Flags: -i show response headers, -X METHOD set the method, -o FILE save the
 * body to a file instead of printing it. It does not advertise gzip, so the
 * server replies with identity encoding (no in-kernel inflate needed).
 *
 *   run curl http://example.com
 *   run curl -i https://example.com
 *   run curl -o page.html http://example.com/index.html
 */

#include "owos.h"

/* ---- tiny string helpers (freestanding, no libc) ----------------------- */

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static char lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* Does `s` begin with `pre`, case-insensitively? */
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

/* Append `s` to dst[*n], capped at cap-1; updates *n. */
static void sapp(char *dst, int *n, int cap, const char *s)
{
	while (*s && *n < cap - 1)
		dst[(*n)++] = *s++;
	dst[*n] = '\0';
}

/* ---- URL parsing ------------------------------------------------------- */

struct purl {
	int  tls;
	int  port;
	char host[128];
	char path[300];
};

/* Split scheme://host[:port][/path]. Lenient: a missing scheme means http.
 * No IPv6-bracket support (the kernel resolver takes a name or dotted-quad).
 * Returns 1 on success, 0 if the host is empty. */
static int parse_url(const char *url, struct purl *u)
{
	const char *p = url;
	u->tls = 0;
	u->port = 0;

	if (starts_ci(p, "http://"))
		p += 7;
	else if (starts_ci(p, "https://")) {
		p += 8;
		u->tls = 1;
	}

	int i = 0;
	while (*p && *p != '/' && *p != ':' && i < (int)sizeof(u->host) - 1)
		u->host[i++] = *p++;
	u->host[i] = '\0';
	if (i == 0)
		return 0;

	if (*p == ':') {
		p++;
		int port = 0;
		while (*p >= '0' && *p <= '9')
			port = port * 10 + (*p++ - '0');
		u->port = port;
	}
	if (u->port == 0)
		u->port = u->tls ? 443 : 80;

	int j = 0;
	if (*p != '/')
		u->path[j++] = '/';
	while (*p && j < (int)sizeof(u->path) - 1)
		u->path[j++] = *p++;
	u->path[j] = '\0';
	if (u->path[0] == '\0') {
		u->path[0] = '/';
		u->path[1] = '\0';
	}
	return 1;
}

/* ---- buffered transport reader ----------------------------------------- */

static int sock;
static unsigned char rbuf[2048];
static int rpos, rlen, reof;

/* Next response byte; -1 at EOF, -2 on transport error. */
static int rgetc(void)
{
	if (rpos >= rlen) {
		if (reof)
			return -1;
		int n = net_recv(sock, rbuf, (int)sizeof rbuf);
		if (n < 0)
			return -2;
		if (n == 0) {
			reof = 1;
			return -1;
		}
		rpos = 0;
		rlen = n;
	}
	return rbuf[rpos++];
}

/* ---- output: stream to console, or accumulate for -o ------------------- */

static int  save;                       /* -o mode                          */
static char obuf[256 * 1024];           /* -o accumulation buffer (.bss)   */
static int  olen, oflow;
static char fbuf[1024];                 /* console write batching           */
static int  flen;

static void out_flush(void)
{
	if (flen) {
		write(1, fbuf, flen);
		flen = 0;
	}
}

static void out_byte(int b)
{
	if (save) {
		if (olen < (int)sizeof obuf)
			obuf[olen++] = (char)b;
		else
			oflow = 1;
		return;
	}
	if (flen == (int)sizeof fbuf)
		out_flush();
	fbuf[flen++] = (char)b;
}

/* ---- main -------------------------------------------------------------- */

int main(int argc, char **argv)
{
	int show_headers = 0;
	const char *method = "GET";
	const char *outname = 0;
	const char *url = 0;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (streq(a, "-i")) {
			show_headers = 1;
		} else if (streq(a, "-X") && i + 1 < argc) {
			method = argv[++i];
		} else if (streq(a, "-o") && i + 1 < argc) {
			outname = argv[++i];
			save = 1;
		} else if (a[0] != '-') {
			url = a;
		}
	}

	if (!url) {
		puts("usage: curl [-i] [-X method] [-o file] <url>\n");
		return 1;
	}

	struct purl u;
	if (!parse_url(url, &u)) {
		puts("curl: could not parse URL\n");
		return 1;
	}

	sock = net_connect(u.host, u.port, u.tls);
	if (sock < 0) {
		if (sock == -2) {
			puts("curl: could not resolve host: ");
			puts(u.host);
			puts("\n");
		} else if (sock == -1) {
			puts("curl: no network device or out of connections\n");
		} else {
			puts("curl: failed to connect to ");
			puts(u.host);
			puts("\n");
		}
		return 1;
	}

	/* Build and send the request. */
	char req[640];
	int n = 0;
	sapp(req, &n, (int)sizeof req, method);
	sapp(req, &n, (int)sizeof req, " ");
	sapp(req, &n, (int)sizeof req, u.path);
	sapp(req, &n, (int)sizeof req, " HTTP/1.1\r\nHost: ");
	sapp(req, &n, (int)sizeof req, u.host);
	sapp(req, &n, (int)sizeof req,
	     "\r\nUser-Agent: owos-curl\r\nConnection: close\r\n\r\n");
	if (net_send(sock, req, n) < 0) {
		puts("curl: failed to send request\n");
		net_close(sock);
		return 1;
	}

	/* Read the header block through the blank line ("\r\n\r\n"). */
	char hdr[4096];
	int hlen = 0, match = 0;
	for (;;) {
		int ci = rgetc();
		if (ci < 0) {
			puts("curl: no response from server\n");
			net_close(sock);
			return 1;
		}
		char c = (char)ci;
		if (hlen < (int)sizeof hdr - 1)
			hdr[hlen++] = c;
		if (c == '\r')
			match = (match == 0) ? 1 : (match == 2 ? 3 : 1);
		else if (c == '\n')
			match = (match == 1) ? 2 : (match == 3 ? 4 : 0);
		else
			match = 0;
		if (match == 4)
			break;
	}
	hdr[hlen] = '\0';

	if (show_headers)
		write(1, hdr, hlen);

	/* Body framing: chunked > Content-Length > read-until-close. */
	int chunked = 0, have_len = 0;
	unsigned content_len = 0;
	for (char *line = hdr; *line; ) {
		if (starts_ci(line, "transfer-encoding:")) {
			/* good enough: "chunked" anywhere on the line */
			for (char *q = line; *q && *q != '\n'; q++)
				if (starts_ci(q, "chunked")) {
					chunked = 1;
					break;
				}
		} else if (starts_ci(line, "content-length:")) {
			const char *v = line + 15;
			while (*v == ' ')
				v++;
			unsigned x = 0;
			while (*v >= '0' && *v <= '9')
				x = x * 10 + (unsigned)(*v++ - '0');
			content_len = x;
			have_len = 1;
		}
		char *eol = line;
		while (*eol && *eol != '\n')
			eol++;
		if (!*eol)
			break;
		line = eol + 1;
	}

	if (chunked) {
		for (;;) {
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
			while (c >= 0 && c != '\n')          /* rest of size line */
				c = rgetc();
			if (!any || sz == 0)
				break;
			for (unsigned i = 0; i < sz; i++) {
				int b = rgetc();
				if (b < 0)
					goto done;
				out_byte(b);
			}
			rgetc();                            /* trailing CR */
			rgetc();                            /* trailing LF */
		}
	} else if (have_len) {
		for (unsigned i = 0; i < content_len; i++) {
			int b = rgetc();
			if (b < 0)
				break;
			out_byte(b);
		}
	} else {
		int b;
		while ((b = rgetc()) >= 0)
			out_byte(b);
	}
done:
	net_close(sock);

	if (save) {
		if (oflow)
			puts("curl: response exceeds 256 KiB buffer, truncated\n");
		if (writefile(outname, obuf, olen) != 0) {
			puts("curl: failed to save ");
			puts(outname);
			puts("\n");
			return 1;
		}
		puts("curl: saved ");
		print_uint((unsigned)olen);
		puts(" bytes to ");
		puts(outname);
		puts("\n");
	} else {
		out_flush();
		puts("\n");
	}
	return 0;
}
