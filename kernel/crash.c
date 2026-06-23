/* crash.c — crash reporter (see crash.h). */
#include <stdint.h>
#include "crash.h"
#include "creg.h"
#include "idt.h"
#include "wm.h"
#include "fb.h"
#include "paging.h"
#include "loader.h"

#define MAXCRASH    4
#define CRASH_TRACE 10

struct crash_info {
	int      state;            /* 0 free, 1 pending (needs window), 2 shown   */
	char     name[24];
	uint32_t vector, err, eip, cr2;
	uint32_t eax, ebx, ecx, edx, esi, edi, ebp, esp;
	uint32_t trace[CRASH_TRACE];
	int      ntrace;
};

static struct crash_info reports[MAXCRASH];

/* Format `v` as 8-digit hex into out (no "0x"); returns out for chaining. */
static const char *hex8(uint32_t v, char *out)
{
	static const char d[] = "0123456789abcdef";
	for (int i = 0; i < 8; i++)
		out[i] = d[(v >> ((7 - i) * 4)) & 0xF];
	out[8] = '\0';
	return out;
}

void crash_record(const char *name, struct registers *r)
{
	int slot = -1;
	for (int i = 0; i < MAXCRASH; i++)
		if (!reports[i].state) { slot = i; break; }
	if (slot < 0)
		return;                       /* report buffer full: drop it          */
	struct crash_info *c = &reports[slot];

	int i = 0;
	for (; name && name[i] && i < (int)sizeof c->name - 1; i++)
		c->name[i] = name[i];
	c->name[i] = '\0';

	c->vector = r->int_no;
	c->err    = r->err_code;
	c->eip    = r->eip;
	c->cr2    = 0;
	if (r->int_no == 14)
		c->cr2 = (uint32_t)read_cr2();
	c->eax = r->eax; c->ebx = r->ebx; c->ecx = r->ecx; c->edx = r->edx;
	c->esi = r->esi; c->edi = r->edi; c->ebp = r->ebp; c->esp = r->esp;

	/* Backtrace: the faulting EIP, then the saved return address up each frame.
	 * Probe every read so a corrupt frame pointer can't fault us. */
	c->ntrace = 0;
	c->trace[c->ntrace++] = r->eip;
	uint32_t bp = r->ebp;
	while (c->ntrace < CRASH_TRACE &&
	       bp >= USER_BASE && bp + 8 <= USER_REGION_END &&
	       paging_user_mapped(bp) && paging_user_mapped(bp + 4)) {
		uint32_t ret = *(volatile uint32_t *)(bp + 4);
		c->trace[c->ntrace++] = ret;
		uint32_t nbp = *(volatile uint32_t *)bp;
		if (nbp <= bp)
			break;                    /* frames must grow walking up           */
		bp = nbp;
	}
	c->state = 1;
}

/* ------------------------------------------------------------- the window */

static void crash_draw(struct window *w, int cx, int cy, int cw, int ch,
                       int focused)
{
	(void)focused; (void)cw; (void)ch;
	struct crash_info *c = w->state;
	if (!c) return;
	uint32_t bg = 0x1a1020;
	fb_fill_rect(cx, cy, cw, ch, bg);

	char hx[12];
	int x = cx + 10, y = cy + 8, lh = 16;

	char title[48];
	int t = 0;
	for (; c->name[t] && t < 23; t++) title[t] = c->name[t];
	const char *suf = " has crashed.";
	for (int k = 0; suf[k] && t < 47; k++) title[t++] = suf[k];
	title[t] = '\0';
	fb_text(x, y, title, 0xff8080, bg); y += lh * 2;

	fb_text(x, y, "fault: ", 0x9098a0, bg);
	fb_text(x + 7 * 8, y, c->vector == 14 ? "page fault" :
	        c->vector == 13 ? "general protection" :
	        c->vector == 6  ? "invalid opcode" :
	        c->vector == 0  ? "divide error" : "exception", 0xe0e6ee, bg);
	y += lh;

	fb_text(x, y, "eip=", 0x9098a0, bg);  fb_text(x + 4 * 8, y, hex8(c->eip, hx), 0x66ddff, bg);
	if (c->vector == 14) {
		fb_text(x + 16 * 8, y, "cr2=", 0x9098a0, bg);
		fb_text(x + 20 * 8, y, hex8(c->cr2, hx), 0xffe08a, bg);
	}
	y += lh;
	fb_text(x, y, "err=", 0x9098a0, bg);  fb_text(x + 4 * 8, y, hex8(c->err, hx), 0xc8d0dc, bg);
	y += lh + 6;

	fb_text(x, y, "eax", 0x707880, bg); fb_text(x + 4 * 8, y, hex8(c->eax, hx), 0xc8d0dc, bg);
	fb_text(x + 14 * 8, y, "ebx", 0x707880, bg); fb_text(x + 18 * 8, y, hex8(c->ebx, hx), 0xc8d0dc, bg);
	y += lh;
	fb_text(x, y, "ecx", 0x707880, bg); fb_text(x + 4 * 8, y, hex8(c->ecx, hx), 0xc8d0dc, bg);
	fb_text(x + 14 * 8, y, "edx", 0x707880, bg); fb_text(x + 18 * 8, y, hex8(c->edx, hx), 0xc8d0dc, bg);
	y += lh;
	fb_text(x, y, "esp", 0x707880, bg); fb_text(x + 4 * 8, y, hex8(c->esp, hx), 0xc8d0dc, bg);
	fb_text(x + 14 * 8, y, "ebp", 0x707880, bg); fb_text(x + 18 * 8, y, hex8(c->ebp, hx), 0xc8d0dc, bg);
	y += lh + 6;

	fb_text(x, y, "stack trace:", 0x9098a0, bg); y += lh;
	for (int i = 0; i < c->ntrace; i++) {
		fb_text(x + 12, y, hex8(c->trace[i], hx), 0xb0c0d0, bg);
		if (i == 0) fb_text(x + 12 + 9 * 8, y, "<- fault", 0x707880, bg);
		y += lh;
	}
}

static void crash_close(struct window *w)
{
	struct crash_info *c = w->state;
	if (c) c->state = 0;              /* free the slot */
	wm_destroy_window(w);
}

void crash_pump(void)
{
	for (int i = 0; i < MAXCRASH; i++) {
		if (reports[i].state != 1)
			continue;
		struct window *win = wm_create_window(140 + i * 22, 120 + i * 22,
		                                      420, 300, "crash", 1);
		if (!win)
			return;                  /* no slot now; try again next frame     */
		reports[i].state = 2;
		win->draw = crash_draw;
		win->on_close = crash_close;
		win->state = &reports[i];
	}
}
