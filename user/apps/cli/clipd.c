/* clipd.c — the clipboard server.
 *
 * A windowless resident daemon: it owns the "clipboard" IPC port and keeps an
 * in-RAM history of clips so the system has a real multi-item, any-type
 * clipboard instead of a single /proc text node. A clip is an opaque blob
 * tagged with a MIME type ("text/plain", "image/png", "application/pdf",
 * "video/mp4", ...), so anything — text, an image, a document, a video — can be
 * held and round-tripped byte-for-byte. Clients (editor/term/browser, and the
 * Super+V history viewer) link user/lib/clipboard.
 *
 * One request per connection (the IPC port is one-connection-at-a-time, which is
 * exactly right for short clipboard ops). Multi-byte fields are little-endian;
 * the MIME type is a length(1)-prefixed string; payloads larger than the
 * channel's ring stream in pieces, so a big blob isn't bounded by the pipe size.
 *
 *   'P' put    mime(1+) datalen(4) data(datalen)        -> 'K'
 *   'G' get    index(4, 0 = newest)  -> mime(1+) datalen(4) data
 *   'N' count                        -> count(4)
 *   'L' list   index(4)              -> mime(1+) datalen(4) plen(2) preview
 *   (an out-of-range get/list answers with a zero-length mime and datalen 0.)
 */
#include "owos.h"
#include "mime.h"

#define MAX_CLIPS   64        /* history depth; oldest is evicted past this   */
#define MIME_MAX    64        /* longest MIME type we keep (NUL-terminated)    */
#define PREVIEW_MAX 64        /* bytes of a text clip the list op returns      */

struct clip { char mime[MIME_MAX]; int len; void *data; };
static struct clip hist[MAX_CLIPS];
static int nclips;

/* ---- framed I/O over a channel fd (read/write may transfer partial) ------- */
static int read_full(int fd, void *buf, int n)
{
	char *p = (char *)buf;
	int got = 0;
	while (got < n) {
		int r = read(fd, p + got, n - got);
		if (r <= 0) return 0;          /* peer closed / error */
		got += r;
	}
	return 1;
}

static int write_full(int fd, const void *buf, int n)
{
	const char *p = (const char *)buf;
	int put = 0;
	while (put < n) {
		int w = write(fd, p + put, n - put);
		if (w <= 0) return 0;
		put += w;
	}
	return 1;
}

static int rd_u32(int fd, unsigned *v)
{
	unsigned char b[4];
	if (!read_full(fd, b, 4)) return 0;
	*v = (unsigned)b[0] | ((unsigned)b[1] << 8) | ((unsigned)b[2] << 16) | ((unsigned)b[3] << 24);
	return 1;
}

static int wr_u32(int fd, unsigned v)
{
	unsigned char b[4] = { (unsigned char)v, (unsigned char)(v >> 8),
	                       (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
	return write_full(fd, b, 4);
}

/* Read a length(1)-prefixed MIME string into `out` (<= MIME_MAX, NUL-term). */
static int rd_mime(int fd, char *out)
{
	unsigned char n;
	if (!read_full(fd, &n, 1)) return 0;
	if (n >= MIME_MAX) n = MIME_MAX - 1;
	if (n && !read_full(fd, out, n)) return 0;
	out[n] = '\0';
	return 1;
}

/* Write a length(1)-prefixed MIME string. */
static int wr_mime(int fd, const char *mime)
{
	int n = 0;
	while (mime[n] && n < MIME_MAX - 1) n++;
	unsigned char nb = (unsigned char)n;
	return write_full(fd, &nb, 1) && (n == 0 || write_full(fd, mime, n));
}

/* ---- history ------------------------------------------------------------- */
/* Prepend a clip; evict (and free) the oldest once the ring is full. Takes
 * ownership of `data`. */
static void store(const char *mime, void *data, int len)
{
	if (nclips == MAX_CLIPS) {
		free(hist[MAX_CLIPS - 1].data);
		nclips--;
	}
	for (int i = nclips; i > 0; i--)
		hist[i] = hist[i - 1];
	int j = 0;
	for (; mime[j] && j < MIME_MAX - 1; j++) hist[0].mime[j] = mime[j];
	hist[0].mime[j] = '\0';
	hist[0].data = data;
	hist[0].len  = len;
	nclips++;
}

/* ---- one request -------------------------------------------------------- */
static void serve(int ch)
{
	char op;
	if (read(ch, &op, 1) != 1)
		return;

	if (op == 'P') {
		char mime[MIME_MAX];
		unsigned len;
		if (!rd_mime(ch, mime) || !rd_u32(ch, &len))
			return;
		void *data = len ? malloc(len) : 0;
		if (len && !data)
			return;
		if (len && !read_full(ch, data, (int)len)) {
			free(data);
			return;
		}
		store(mime, data, (int)len);
		write_full(ch, "K", 1);
		return;
	}

	if (op == 'G') {
		unsigned idx;
		if (!rd_u32(ch, &idx))
			return;
		if (idx >= (unsigned)nclips) {
			wr_mime(ch, "");
			wr_u32(ch, 0);
			return;
		}
		struct clip *c = &hist[idx];
		wr_mime(ch, c->mime);
		wr_u32(ch, (unsigned)c->len);
		if (c->len)
			write_full(ch, c->data, c->len);
		return;
	}

	if (op == 'N') {
		wr_u32(ch, (unsigned)nclips);
		return;
	}

	if (op == 'L') {
		unsigned idx;
		if (!rd_u32(ch, &idx))
			return;
		if (idx >= (unsigned)nclips) {
			unsigned char z2[2] = { 0, 0 };
			wr_mime(ch, "");
			wr_u32(ch, 0);
			write_full(ch, z2, 2);
			return;
		}
		struct clip *c = &hist[idx];
		int plen = mime_is_text(c->mime) ? (c->len < PREVIEW_MAX ? c->len : PREVIEW_MAX) : 0;
		unsigned char pb[2] = { (unsigned char)plen, (unsigned char)(plen >> 8) };
		wr_mime(ch, c->mime);
		wr_u32(ch, (unsigned)c->len);
		write_full(ch, pb, 2);
		if (plen)
			write_full(ch, c->data, plen);
		return;
	}
}

int main(void)
{
	int lst = port_open("clipboard", 1);
	if (lst < 0)
		return 1;                  /* a clipd is already running */

	for (;;) {
		int ch = port_accept(lst);
		if (ch < 0)
			continue;
		serve(ch);
		close(ch);
	}
}
