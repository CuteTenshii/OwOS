/* kclip.c — kernel-side clipd client (see kclip.h). Speaks the clipd protocol
 * (user/clipd.c) over an ipc_connect() channel: the two pipe rings are driven
 * with pipe_read/pipe_write, same framing the user lib uses. Thread context
 * only — pipe_read blocks (yields) until clipd answers. */

#include "kclip.h"
#include "ipc.h"
#include "pipe.h"

/* ---- framed I/O over the raw channel pipe ids ---------------------------- */
static int pw(int id, const void *buf, int n)
{
	const char *p = (const char *)buf;
	int put = 0;
	while (put < n) {
		int w = pipe_write(id, p + put, n - put);
		if (w <= 0) return 0;
		put += w;
	}
	return 1;
}

static int pr(int id, void *buf, int n)
{
	char *p = (char *)buf;
	int got = 0;
	while (got < n) {
		int r = pipe_read(id, p + got, n - got);
		if (r <= 0) return 0;
		got += r;
	}
	return 1;
}

static int pw_u32(int id, unsigned v)
{
	unsigned char b[4] = { (unsigned char)v, (unsigned char)(v >> 8),
	                       (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
	return pw(id, b, 4);
}

static int pr_u32(int id, unsigned *v)
{
	unsigned char b[4];
	if (!pr(id, b, 4)) return 0;
	*v = (unsigned)b[0] | ((unsigned)b[1] << 8) | ((unsigned)b[2] << 16) | ((unsigned)b[3] << 24);
	return 1;
}

/* read a length(1)-prefixed MIME string into out[max] (NUL-terminated) */
static int pr_mime(int id, char *out, int max)
{
	unsigned char n;
	char tmp[64];
	if (!pr(id, &n, 1)) return 0;
	if (n && !pr(id, tmp, n)) return 0;
	int j = 0;
	for (; j < n && j < max - 1; j++) out[j] = tmp[j];
	out[j] = '\0';
	return 1;
}

static int is_text(const char *m)
{
	return m[0] == 't' && m[1] == 'e' && m[2] == 'x' && m[3] == 't' && m[4] == '/';
}

int kclip_get_text(char *out, int max)
{
	int rx, tx;
	if (max <= 0 || ipc_connect("clipboard", &rx, &tx) < 0)
		return 0;

	int got = 0;
	char mime[64];
	unsigned len = 0;
	if (pw(tx, "G", 1) && pw_u32(tx, 0) && pr_mime(rx, mime, sizeof mime) && pr_u32(rx, &len)) {
		if (is_text(mime) && len) {
			unsigned want = len < (unsigned)max ? len : (unsigned)max;
			if (pr(rx, out, (int)want))
				got = (int)want;
			/* drain any bytes past our buffer so the channel ends cleanly */
			char junk[64];
			for (unsigned r = want; r < len; ) {
				unsigned c = len - r;
				if (c > sizeof junk) c = sizeof junk;
				if (!pr(rx, junk, (int)c)) break;
				r += c;
			}
		}
	}
	pipe_close(rx, 0);     /* this end reads rx, writes tx */
	pipe_close(tx, 1);
	return got;
}

int kclip_set_text(const char *text, int len)
{
	int rx, tx;
	if (ipc_connect("clipboard", &rx, &tx) < 0)
		return -1;

	const char *mime = "text/plain";
	unsigned char ml = 10;                 /* strlen("text/plain") */
	char k = 0;
	int ok = pw(tx, "P", 1) && pw(tx, &ml, 1) && pw(tx, mime, ml) &&
	         pw_u32(tx, (unsigned)len) && (len == 0 || pw(tx, text, len)) &&
	         pr(rx, &k, 1) && k == 'K';

	pipe_close(rx, 0);
	pipe_close(tx, 1);
	return ok ? 0 : -1;
}
