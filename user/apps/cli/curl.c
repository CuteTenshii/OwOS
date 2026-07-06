/* curl.c — an HTTP/1.1 client, a thin CLI over libcurl (user/lib/http.c).
 *
 * The protocol lives in libcurl; this file just parses options, picks where the
 * body goes (console, or a buffer for -o), and reports the outcome. Ctrl+C is a
 * cooperative interrupt: a blocked transfer returns HTTP_EINTR and we print ^C.
 *
 *   run curl http://example.com
 *   run curl -i https://example.com
 *   run curl -o page.html http://example.com/index.html
 */

#include "owos.h"
#include "http.h"

/* ---- body sinks -------------------------------------------------------- */

/* Console mode: batch and write to fd 1. */
static char con_buf[1024];
static int  con_len;

static void con_flush(void)
{
	if (con_len) { write(1, con_buf, con_len); con_len = 0; }
}

static int sink_console(void *ctx, const unsigned char *buf, int len)
{
	(void)ctx;
	for (int i = 0; i < len; i++) {
		if (con_len == (int)sizeof con_buf)
			con_flush();
		con_buf[con_len++] = (char)buf[i];
	}
	return 0;
}

/* -o mode: accumulate into a buffer, then writefile. */
static char save_buf[256 * 1024];
static int  save_len, save_overflow;

static int sink_save(void *ctx, const unsigned char *buf, int len)
{
	(void)ctx;
	for (int i = 0; i < len; i++) {
		if (save_len < (int)sizeof save_buf)
			save_buf[save_len++] = (char)buf[i];
		else
			save_overflow = 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	int show_headers = 0;
	int http2 = 0;
	int http3 = 0;
	const char *method = "GET";
	const char *outname = 0;
	const char *url = 0;
	const char *user_agent = 0;            /* -A: overrides the default UA           */
	int         ua_in_headers = 0;         /* set if a -H line already gives one      */
	char xhdr[2048]; int xhlen = 0;        /* -H request headers ("name: value\r\n"…) */

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (streq(a, "-i"))                      show_headers = 1;
		else if (streq(a, "--http2"))            http2 = 1;
		else if (streq(a, "--http3") || streq(a, "-3")) http3 = 1;
		else if (streq(a, "-X") && i + 1 < argc) method = argv[++i];
		else if (streq(a, "-o") && i + 1 < argc) outname = argv[++i];
		else if ((streq(a, "-A") || streq(a, "--user-agent")) && i + 1 < argc)
			user_agent = argv[++i];
		else if (streq(a, "-H") && i + 1 < argc) {
			const char *h = argv[++i];
			if (starts_ci(h, "user-agent:")) ua_in_headers = 1;
			for (int k = 0; h[k] && xhlen < (int)sizeof xhdr - 3; k++)
				xhdr[xhlen++] = h[k];
			xhdr[xhlen++] = '\r'; xhdr[xhlen++] = '\n';
			xhdr[xhlen] = '\0';
		}
		else if (a[0] != '-')                    url = a;
	}

	if (!url) {
		puts("usage: curl [-i] [--http2] [--http3|-3] [-X method] [-A ua] "
		     "[-H \"Name: value\"] [-o file] <url>\n");
		return 1;
	}

	/* The User-Agent is the tool's to send, not libcurl's (as on a real system):
	 * add "-A" if given, else a default — unless a -H line already carries one. */
	if (!ua_in_headers) {
		const char *ua = user_agent ? user_agent : "owos-curl/1.0";
		for (const char *s = "User-Agent: "; *s && xhlen < (int)sizeof xhdr - 3; s++)
			xhdr[xhlen++] = *s;
		for (const char *s = ua; *s && xhlen < (int)sizeof xhdr - 3; s++)
			xhdr[xhlen++] = *s;
		xhdr[xhlen++] = '\r'; xhdr[xhlen++] = '\n';
		xhdr[xhlen] = '\0';
	}

	char hdr[4096];
	int  hlen = 0;
	int rc = http_request_ex(method, url, http2, http3, xhlen ? xhdr : 0,
	                      show_headers ? hdr : 0, (int)sizeof hdr, &hlen,
	                      outname ? sink_save : sink_console, 0);

	if (show_headers && hlen > 0)
		write(1, hdr, hlen);

	if (rc < 0) {
		con_flush();
		switch (rc) {
		case HTTP_EPARSE:   puts("curl: could not parse URL\n"); break;
		case HTTP_ERESOLVE: printf("curl: could not resolve host: %s\n", url); break;
		case HTTP_ECONNECT: puts("curl: failed to connect\n"); break;
		case HTTP_ESEND:    puts("curl: failed to send request\n"); break;
		case HTTP_ENORESP:  puts("curl: no response from server\n"); break;
		case HTTP_EINTR:    return EXIT_SIGINT;   /* silent; the shell echoed ^C */
		default:            puts("curl: transfer failed\n"); break;
		}
		return 1;
	}

	if (outname) {
		if (save_overflow)
			puts("curl: response exceeds 256 KiB buffer, truncated\n");
		if (writefile(outname, save_buf, save_len) != 0) {
			printf("curl: failed to save %s\n", outname);
			return 1;
		}
		printf("curl: saved %u bytes to %s\n", (unsigned)save_len, outname);
	} else {
		con_flush();
		puts("\n");
	}
	return 0;
}
