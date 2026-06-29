/* notify.c — client side of the notifyd protocol (see notify.h). Each call opens
 * its own short-lived connection to the "notify" port, sends one request, and
 * closes — never holding the single connection slot open. */
#include "owos.h"
#include "notify.h"

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

static int slen(const char *s) { return s ? strlen(s) : 0; }

/* u8-length-prefixed string, truncated to `cap` (the byte count sent, never the
 * NUL). */
static int wr_str8(int fd, const char *s, int cap)
{
	int n = slen(s);
	if (n > cap) n = cap;
	if (n > 255) n = 255;
	unsigned char b = (unsigned char)n;
	return write_full(fd, &b, 1) && (n == 0 || write_full(fd, s, n));
}

static int wr_str16(int fd, const char *s, int cap)
{
	int n = slen(s);
	if (n > cap) n = cap;
	if (n > 65535) n = 65535;
	unsigned char b[2] = { (unsigned char)n, (unsigned char)(n >> 8) };
	return write_full(fd, b, 2) && (n == 0 || write_full(fd, s, n));
}

static int read_ack(int fd)
{
	char r = 0;
	return read(fd, &r, 1) == 1 && r == NOTIFY_ACK;
}

int notify_send(const struct notification *n)
{
	if (!n || !n->title || !n->title[0])
		return -1;
	int ch = port_open(NOTIFY_PORT, 0);
	if (ch < 0)
		return -1;

	int nb = n->nbuttons;
	if (nb < 0) nb = 0;
	if (nb > NOTIFY_MAX_BUTTONS) nb = NOTIFY_MAX_BUTTONS;

	char op = NOTIFY_OP_NOTIFY;
	int ok = write_full(ch, &op, 1) &&
	         wr_str8(ch, n->app, NOTIFY_APP_MAX - 1) &&
	         wr_str16(ch, n->title, NOTIFY_TITLE_MAX - 1) &&
	         wr_str16(ch, n->subtitle, NOTIFY_SUB_MAX - 1) &&
	         wr_str16(ch, n->body, NOTIFY_BODY_MAX - 1) &&
	         wr_str16(ch, n->image, NOTIFY_IMG_MAX - 1);
	if (ok) {
		unsigned char c = (unsigned char)nb;
		ok = write_full(ch, &c, 1);
	}
	for (int i = 0; ok && i < nb; i++)
		ok = wr_str8(ch, n->buttons[i].label, NOTIFY_BLABEL_MAX - 1) &&
		     wr_str16(ch, n->buttons[i].action, NOTIFY_BCMD_MAX - 1);
	if (ok)
		ok = read_ack(ch);

	close(ch);
	return ok ? 0 : -1;
}

int notify_simple(const char *title, const char *body)
{
	struct notification n = { 0 };
	n.title = title;
	n.body = body;
	return notify_send(&n);
}

int notify_show(void)
{
	int ch = port_open(NOTIFY_PORT, 0);
	if (ch < 0)
		return -1;
	char op = NOTIFY_OP_SHOW;
	int ok = write_full(ch, &op, 1) && read_ack(ch);
	close(ch);
	return ok ? 0 : -1;
}
