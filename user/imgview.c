/* imgview.c — a ring-3 PNG image viewer.
 *
 * Usage: imgview <file.png>. Opens a window sized to fit the image, then
 * streams the PNG off disk through the user-space decoder (upng), scaling each
 * decoded scanline straight into the window's pixel buffer. Nothing about the
 * image — file bytes, decoded pixels — is ever held whole: the file is pulled in
 * 4 KiB reads via readfile_at, and the decoder emits one row at a time. A
 * malformed image can only crash this process; the kernel never parses it.
 */
#include "owos.h"
#include "upng.h"

/* ---- a buffered file reader over readfile_at (streams any-size files) ---- */
struct freader {
	const char   *name;
	unsigned      off;
	unsigned char buf[4096];
	unsigned      len, pos;
	int           eof;
};

static void fr_init(struct freader *f, const char *name)
{
	f->name = name; f->off = 0; f->len = 0; f->pos = 0; f->eof = 0;
}

static int fr_get(void *c)
{
	struct freader *f = c;
	if (f->pos >= f->len) {
		if (f->eof)
			return -1;
		int n = readfile_at(f->name, f->off, f->buf, sizeof f->buf);
		if (n <= 0) { f->eof = 1; return -1; }
		f->off += (unsigned)n; f->len = (unsigned)n; f->pos = 0;
	}
	return f->buf[f->pos++];
}

/* ---- scaled blit into the window buffer --------------------------------- */
struct view {
	unsigned cw, ch;        /* client (window) size = win_fb pitch            */
	unsigned iw, ih;        /* image size                                     */
	unsigned dw, dh, ox, oy;/* drawn size + top-left offset (letterboxed)     */
	unsigned next_r;        /* next destination row awaiting a source row     */
	int      id;            /* window, so we can present rows as they decode  */
	unsigned shown;         /* next_r at the last present (throttle presents) */
};

/* Called per source scanline: fill every destination row whose nearest source
 * row is this one (nearest-neighbour vertical scale), each scaled horizontally. */
static int v_row(void *c, unsigned y, const unsigned char *rgba, unsigned w)
{
	(void)w;
	struct view *v = c;
	unsigned *fb = win_fb();
	while (v->next_r < v->dh && (v->next_r * v->ih / v->dh) == y) {
		unsigned *drow = fb + (unsigned long)(v->oy + v->next_r) * v->cw + v->ox;
		for (unsigned x = 0; x < v->dw; x++) {
			const unsigned char *p = rgba + (x * v->iw / v->dw) * 4;
			unsigned a = p[3];
			if (a == 255) {
				drow[x] = ((unsigned)p[0] << 16) | ((unsigned)p[1] << 8) | p[2];
			} else if (a) {                       /* blend over the letterbox bg */
				unsigned d = drow[x], ia = 255 - a;
				unsigned r = (p[0] * a + ((d >> 16) & 0xff) * ia) / 255;
				unsigned g = (p[1] * a + ((d >> 8) & 0xff) * ia) / 255;
				unsigned b = (p[2] * a + (d & 0xff) * ia) / 255;
				drow[x] = (r << 16) | (g << 8) | b;
			}
		}
		v->next_r++;
	}
	/* Decoding a megabyte PNG off PIO disk takes seconds; present the rows
	 * filled so far every so often so the image paints in top-to-bottom
	 * instead of staying blank until the whole decode finishes. */
	if (v->next_r - v->shown >= 16) {
		v->shown = v->next_r;
		win_present(v->id);
	}
	return 0;
}

/* Decode `name` into the current cw*ch window, fit-to-window with aspect ratio
 * preserved and letterbox padding around it. */
static void render(int id, const char *name, unsigned cw, unsigned ch, unsigned iw, unsigned ih)
{
	struct view v;
	v.cw = cw; v.ch = ch; v.iw = iw; v.ih = ih; v.next_r = 0;
	v.id = id; v.shown = 0;
	if (iw <= cw && ih <= ch) {          /* fits: show 1:1 (no blurry upscale) */
		v.dw = iw; v.dh = ih;
	} else if ((unsigned long)iw * ch > (unsigned long)ih * cw) {
		v.dw = cw; v.dh = ih * cw / iw;  /* width-bound                        */
	} else {
		v.dh = ch; v.dw = iw * ch / ih;  /* height-bound                       */
	}
	if (v.dw == 0) v.dw = 1;
	if (v.dh == 0) v.dh = 1;
	v.ox = (cw - v.dw) / 2;
	v.oy = (ch - v.dh) / 2;

	unsigned *fb = win_fb();
	for (unsigned i = 0; i < cw * ch; i++)
		fb[i] = 0x141a24;                /* letterbox background               */

	struct freader fr;
	fr_init(&fr, name);
	upng_decode(fr_get, &fr, v_row, &v, 0, 0);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		puts("usage: imgview <file.png>\n");
		return 1;
	}
	const char *name = argv[1];

	/* Peek the dimensions to size the window. */
	struct freader fr;
	fr_init(&fr, name);
	unsigned iw = 0, ih = 0;
	if (upng_dimensions(fr_get, &fr, &iw, &ih) != 0 || iw == 0 || ih == 0) {
		puts("imgview: not a readable PNG (bad signature or header)\n");
		return 1;
	}

	/* Fit the image into a window that stays within the kernel's per-window
	 * buffer cap (WIN_MAX_BYTES = 896 KiB). 560*400*4 = 896000 B, and the
	 * fit only ever shrinks from there, so cw*ch*4 never exceeds the cap. */
	const unsigned MAXW = 560, MAXH = 400;
	unsigned cw = iw, ch = ih;
	if (cw > MAXW || ch > MAXH) {
		if ((unsigned long)iw * MAXH > (unsigned long)ih * MAXW) {
			cw = MAXW; ch = ih * MAXW / iw;
		} else {
			ch = MAXH; cw = iw * MAXH / ih;
		}
	}
	if (cw == 0) cw = 1;
	if (ch == 0) ch = 1;

	int id = win_create((int)cw, (int)ch, name);
	if (id < 0) {
		puts("imgview: could not open a window\n");
		return 1;
	}
	win_resizable(id, 0);          /* fixed size: the buffer matches cw*ch      */

	/* Decoding (PNG inflate) is CPU-bound and can take a moment on a big image;
	 * show the spinner while render() works, then go back to the arrow. */
	win_cursor(id, WIN_CURSOR_BUSY);
	render(id, name, cw, ch, iw, ih);
	win_present(id);
	win_cursor(id, WIN_CURSOR_ARROW);

	for (;;) {
		struct win_event ev;
		win_wait(id);
		while (win_poll(id, &ev)) {
			if (ev.type == WEV_CLOSE) {
				win_close(id);
				return 0;
			}
		}
	}
}
