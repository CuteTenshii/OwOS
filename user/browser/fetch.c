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

/* ----------------------------------------------------------- transaction */

static int http_txn(const struct purl *u, const char *full_url, char *loc, int loc_cap)
{
	loc[0] = '\0';
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
	     "Accept-Encoding: identity\r\n"
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

	int chunked = 0, have_len = 0; unsigned content_len = 0;
	for (char *line = hdr; *line; ) {
		if (starts_ci(line, "transfer-encoding:")) {
			for (char *q = line; *q && *q != '\n'; q++)
				if (starts_ci(q, "chunked")) { chunked = 1; break; }
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

	if (chunked) {
		for (;;) {
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
			if (!any || sz == 0) break;
			for (unsigned i = 0; i < sz; i++) {
				int b = rgetc();
				if (b < 0) goto body_done;
				if (B->raw_len < RAW_MAX) B->raw[B->raw_len++] = (char)b;
			}
			rgetc(); rgetc();
		}
	} else if (have_len) {
		for (unsigned i = 0; i < content_len && B->raw_len < RAW_MAX; i++) {
			int b = rgetc();
			if (b < 0) break;
			B->raw[B->raw_len++] = (char)b;
		}
	} else {
		int b;
		while ((b = rgetc()) >= 0 && B->raw_len < RAW_MAX)
			B->raw[B->raw_len++] = (char)b;
	}
body_done:
	net_close(B->sock);
	return status;
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

		html_to_text();
		B->scroll = 0;
		{ int ci = 0; sapp(B->cur_url, &ci, sizeof B->cur_url, url); }
		/* The title bar is updated by the main thread (only the window's owner
		 * task may call win_settitle); it reads B->doc_title / B->cur_url. */

		char done[200]; int d = 0;
		sapp(done, &d, sizeof done, "HTTP ");
		uapp(done, &d, sizeof done, (unsigned)st);
		sapp(done, &d, sizeof done, "  -  ");
		uapp(done, &d, sizeof done, (unsigned)B->raw_len);
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
