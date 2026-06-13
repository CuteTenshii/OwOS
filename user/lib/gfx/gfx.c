/* gfx.c — user-space drawing into a window's shared pixel buffer. See gfx.h. */
#include "gfx.h"
#include "font_aa.h"
#include "font_ui.h"
#include "owos.h"   /* KEY_* editing codes delivered via WEV_KEY */

/* The GUI keeps the legacy 8x16 cell (app layouts depend on it); only the glyph
 * pixels gained antialiasing. font_aa is an 8-bit coverage atlas baked offline
 * by tools/genfont.c — change these and re-bake to match. */
#define GFX_FONT_W 8
#define GFX_FONT_H 16

static unsigned *g_fb;
static int       g_w, g_h;

void gfx_init(unsigned *fb, int w, int h) { g_fb = fb; g_w = w; g_h = h; }

void gfx_pixel(int x, int y, unsigned color)
{
	if ((unsigned)x < (unsigned)g_w && (unsigned)y < (unsigned)g_h)
		g_fb[y * g_w + x] = color;
}

void gfx_fill_rect(int x, int y, int w, int h, unsigned color)
{
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > g_w) w = g_w - x;
	if (y + h > g_h) h = g_h - y;
	for (int r = 0; r < h; r++) {
		unsigned *p = g_fb + (long)(y + r) * g_w + x;
		for (int c = 0; c < w; c++)
			p[c] = color;
	}
}

void gfx_clear(unsigned color) { gfx_fill_rect(0, 0, g_w, g_h, color); }

void gfx_rect(int x, int y, int w, int h, unsigned color)
{
	gfx_fill_rect(x, y, w, 1, color);
	gfx_fill_rect(x, y + h - 1, w, 1, color);
	gfx_fill_rect(x, y, 1, h, color);
	gfx_fill_rect(x + w - 1, y, 1, h, color);
}

/* Blend fg over bg by coverage a (0..255), per channel. */
static unsigned gfx_blend(unsigned fg, unsigned bg, unsigned a)
{
	if (a == 0)   return bg;
	if (a == 255) return fg;
	unsigned ia = 255 - a;
	unsigned r = (((fg >> 16) & 0xff) * a + ((bg >> 16) & 0xff) * ia) / 255;
	unsigned g = (((fg >>  8) & 0xff) * a + ((bg >>  8) & 0xff) * ia) / 255;
	unsigned b = ((fg & 0xff) * a + (bg & 0xff) * ia) / 255;
	return (r << 16) | (g << 8) | b;
}

void gfx_glyph(int x, int y, unsigned char ch, unsigned fg, unsigned bg)
{
	int transparent = (bg & GFX_TRANSPARENT) != 0;
	for (int row = 0; row < GFX_FONT_H; row++) {
		int sy = y + row;
		if ((unsigned)sy >= (unsigned)g_h)
			continue;
		const unsigned char *cov = font_aa[ch][row];
		for (int col = 0; col < GFX_FONT_W; col++) {
			int sx = x + col;
			if ((unsigned)sx >= (unsigned)g_w)
				continue;
			unsigned a = cov[col];
			if (a == 0 && transparent)
				continue;
			unsigned *px = &g_fb[sy * g_w + sx];
			*px = transparent ? gfx_blend(fg, *px, a) : gfx_blend(fg, bg, a);
		}
	}
}

void gfx_text(int x, int y, const char *s, unsigned fg, unsigned bg)
{
	for (; *s; s++, x += GFX_FONT_W)
		gfx_glyph(x, y, (unsigned char)*s, fg, bg);
}

/* Proportional sans-serif UI font (chrome only). Blends fg over the existing
 * pixels — fill the background first — and advances per glyph. See gfx.h. */
static void gfx_ui_glyph(int x, int y, unsigned char ch, unsigned fg)
{
	for (int row = 0; row < UIFONT_H; row++) {
		int sy = y + row;
		if ((unsigned)sy >= (unsigned)g_h)
			continue;
		const unsigned char *cov = font_ui[ch][row];
		for (int col = 0; col < UIFONT_W; col++) {
			int sx = x + col;
			if ((unsigned)sx >= (unsigned)g_w)
				continue;
			unsigned a = cov[col];
			if (a) {
				unsigned *px = &g_fb[sy * g_w + sx];
				*px = gfx_blend(fg, *px, a);
			}
		}
	}
}

int gfx_ui_width(const char *s)
{
	int w = 0;
	for (; *s; s++)
		w += font_ui_adv[(unsigned char)*s];
	return w;
}

int gfx_ui_cw(unsigned char c) { return font_ui_adv[c]; }

void gfx_ui_text(int x, int y, const char *s, unsigned fg)
{
	for (; *s; s++) {
		gfx_ui_glyph(x, y, (unsigned char)*s, fg);
		x += font_ui_adv[(unsigned char)*s];
	}
}

/* --------------------------------------------------------------- GUI widgets */

void status_set(struct status_bar *s, const char *msg)
{
	int i = 0;
	for (; msg[i] && i < (int)sizeof(s->msg) - 1; i++)
		s->msg[i] = msg[i];
	s->msg[i] = '\0';
}

void status_draw(const struct status_bar *s, int x, int y, int w,
                 unsigned fg, unsigned bg)
{
	gfx_fill_rect(x, y, w, GUI_BAR_H, bg);
	gfx_ui_text(x + 6, y + (GUI_BAR_H - UIFONT_H) / 2, s->msg, fg);
}

#define MENU_BG    0x2a3342u    /* dropdown background                         */
#define MENU_FG    0xe0e6eeu    /* enabled item text                          */
#define MENU_DIM   0x636b78u    /* disabled item text / dimmed hint           */

/* A small checkmark drawn in a row's left gutter (the cell is GUI_ROW_H tall). */
static void menu_check(int x, int y, unsigned c)
{
	gfx_fill_rect(x + 0, y + 8, 2, 2, c);
	gfx_fill_rect(x + 1, y + 9, 2, 2, c);
	gfx_fill_rect(x + 2, y + 10, 2, 2, c);   /* vertex */
	gfx_fill_rect(x + 3, y + 8, 2, 2, c);
	gfx_fill_rect(x + 4, y + 6, 2, 2, c);
	gfx_fill_rect(x + 5, y + 4, 2, 2, c);
	gfx_fill_rect(x + 6, y + 2, 2, 2, c);
}

/* Draw a popup list (no title bar) at (px,py): one row per item, with the check
 * gutter, disabled dimming, accelerator hints, and submenu arrows. Open child
 * submenus are then drawn on top, to the right of their parent row. */
static void menu_popup(const struct menu *m, int px, int py)
{
	int gutter = m->flags ? 10 : 0;          /* reserve a check column         */
	gfx_fill_rect(px, py, GUI_MENU_W, m->n * GUI_ROW_H, MENU_BG);
	for (int i = 0; i < m->n; i++) {
		int ry = py + i * GUI_ROW_H;
		unsigned char fl = m->flags ? m->flags[i] : 0;
		int dis = (fl & MENU_DISABLED) != 0;
		unsigned fg = dis ? MENU_DIM : MENU_FG;
		if (fl & MENU_CHECKED)
			menu_check(px + 2, ry, dis ? MENU_DIM : 0x8fd98f);
		gfx_ui_text(px + 6 + gutter, ry, m->items[i], fg);
		if (m->subs && m->subs[i])               /* submenu: a right arrow (►)  */
			gfx_glyph(px + GUI_MENU_W - 12, ry, 0x10, fg, MENU_BG);
		else if (m->hints && m->hints[i] && m->hints[i][0]) {
			int hw = gfx_ui_width(m->hints[i]);
			gfx_ui_text(px + GUI_MENU_W - 6 - hw, ry, m->hints[i],
			            dis ? MENU_DIM : 0x8a93a0);
		}
	}
	gfx_rect(px, py, GUI_MENU_W, m->n * GUI_ROW_H, 0x05070a);
	for (int i = 0; i < m->n; i++)
		if (m->subs && m->subs[i] && m->subs[i]->open)
			menu_popup(m->subs[i], px + GUI_MENU_W - 2, py + i * GUI_ROW_H);
}

void menu_draw(const struct menu *m, int x, int y)
{
	unsigned bg = m->open ? 0x3a4a60 : 0x232a36;
	int tw = gfx_ui_width(m->title) + 6;
	if (m->open)
		gfx_fill_rect(x, y, tw, GUI_BAR_H, bg);
	gfx_ui_text(x + 3, y + (GUI_BAR_H - UIFONT_H) / 2, m->title, 0xe0e6ee);
	if (m->open)
		menu_popup(m, x, y + GUI_BAR_H);
}

/* Recursively close a menu's submenus (used when the menu itself closes). */
static void menu_subs_close(struct menu *m)
{
	if (!m->subs)
		return;
	for (int i = 0; i < m->n; i++)
		if (m->subs[i]) { m->subs[i]->open = 0; menu_subs_close(m->subs[i]); }
}

/* Route a click in m's popup at (px,py), recursing through open submenus (drawn
 * on top, so tested first). Returns 2 = a leaf item fired (sets *fm and *fi),
 * 1 = handled but nothing fired (opened/closed a submenu, or a disabled item),
 * 0 = the click missed this subtree. */
static int menu_popup_hit(struct menu *m, int px, int py, int mx, int my,
                          struct menu **fm, int *fi)
{
	for (int i = 0; i < m->n; i++)
		if (m->subs && m->subs[i] && m->subs[i]->open) {
			int r = menu_popup_hit(m->subs[i], px + GUI_MENU_W - 2,
			                       py + i * GUI_ROW_H, mx, my, fm, fi);
			if (r)
				return r;
		}
	if (mx < px || mx >= px + GUI_MENU_W ||
	    my < py || my >= py + m->n * GUI_ROW_H)
		return 0;
	int i = (my - py) / GUI_ROW_H;
	unsigned char fl = m->flags ? m->flags[i] : 0;
	if (fl & MENU_DISABLED)
		return 1;                                /* inert: swallow, stay open   */
	if (m->subs && m->subs[i]) {                     /* parent: toggle its child */
		int wasopen = m->subs[i]->open;
		for (int k = 0; k < m->n; k++)
			if (m->subs[k]) m->subs[k]->open = 0;
		m->subs[i]->open = !wasopen;
		return 1;
	}
	*fm = m; *fi = i;
	return 2;
}

int menu_click_ex(struct menu *m, int mx, int my, int x, int y, struct menu **leaf)
{
	if (leaf)
		*leaf = 0;
	int tw = gfx_ui_width(m->title) + 6;
	if (my >= y && my < y + GUI_BAR_H) {        /* the title row */
		m->open = (mx >= x && mx < x + tw) ? !m->open : 0;
		if (!m->open)
			menu_subs_close(m);
		return -1;
	}
	if (m->open) {
		struct menu *fm = 0;
		int fi = -1;
		int r = menu_popup_hit(m, x, y + GUI_BAR_H, mx, my, &fm, &fi);
		if (r == 2) {                       /* a leaf fired -> close everything */
			m->open = 0;
			menu_subs_close(m);
			if (leaf)
				*leaf = fm;
			return fi;
		}
		if (r == 1)                         /* opened submenu / disabled: stay  */
			return -1;
		m->open = 0;                        /* clicked outside -> dismiss       */
		menu_subs_close(m);
	}
	return -1;
}

int menu_click(struct menu *m, int mx, int my, int x, int y)
{
	struct menu *leaf;
	int it = menu_click_ex(m, mx, my, x, y, &leaf);
	return (leaf == m) ? it : -1;       /* back-compat: only this menu's items */
}

/* Search m (and its submenus) for an enabled item whose accelerator is `key`. */
static int menu_shortcut_walk(const struct menu *m, int key, struct menu **leaf)
{
	if (m->keys)
		for (int i = 0; i < m->n; i++) {
			unsigned char fl = m->flags ? m->flags[i] : 0;
			if (m->keys[i] == key && !(fl & MENU_DISABLED) &&
			    !(m->subs && m->subs[i])) {
				if (leaf) *leaf = (struct menu *)m;
				return i;
			}
		}
	if (m->subs)
		for (int i = 0; i < m->n; i++)
			if (m->subs[i]) {
				int it = menu_shortcut_walk(m->subs[i], key, leaf);
				if (it >= 0)
					return it;
			}
	return -1;
}

int menu_shortcut(const struct menu *m, int key)
{
	if (!key)
		return -1;
	struct menu *leaf = 0;
	int it = menu_shortcut_walk(m, key, &leaf);
	return (leaf == m) ? it : -1;       /* back-compat: only this menu's items */
}

/* ------------------------------------------------------------- menu bar */

#define MENUBAR_GAP 4   /* px between adjacent titles on the bar */

/* x of menu i's title: the bar origin plus the widths of the titles before it. */
static int menubar_title_x(const struct menubar *mb, int i)
{
	int x = mb->x;
	for (int k = 0; k < i; k++)
		x += gfx_ui_width(mb->menus[k]->title) + 6 + MENUBAR_GAP;
	return x;
}

void menubar_close(struct menubar *mb)
{
	for (int i = 0; i < mb->n; i++) {
		mb->menus[i]->open = 0;
		menu_subs_close(mb->menus[i]);
	}
}

void menubar_draw(const struct menubar *mb)
{
	/* Closed titles first; the open one (if any) last so its dropdown overlays
	 * a neighbouring title. */
	int openi = -1;
	for (int i = 0; i < mb->n; i++) {
		if (mb->menus[i]->open) { openi = i; continue; }
		menu_draw(mb->menus[i], menubar_title_x(mb, i), mb->y);
	}
	if (openi >= 0)
		menu_draw(mb->menus[openi], menubar_title_x(mb, openi), mb->y);
}

int menubar_click(struct menubar *mb, int mx, int my, struct menu **leaf, int *item)
{
	*leaf = 0;
	*item = -1;

	/* On the title row: toggle the clicked title and close the others, so only
	 * one dropdown is ever open. */
	if (my >= mb->y && my < mb->y + GUI_BAR_H) {
		for (int i = 0; i < mb->n; i++) {
			int x = menubar_title_x(mb, i);
			int tw = gfx_ui_width(mb->menus[i]->title) + 6;
			if (mx >= x && mx < x + tw) {
				int wasopen = mb->menus[i]->open;
				menubar_close(mb);
				mb->menus[i]->open = !wasopen;
				return 1;
			}
		}
		menubar_close(mb);        /* empty bar area: dismiss any open menu */
		return 1;
	}

	/* Below the bar: hand the click to whichever menu is open (menu_click_ex
	 * closes it and reports the chosen leaf item, or -1 if the click missed). */
	for (int i = 0; i < mb->n; i++) {
		if (!mb->menus[i]->open)
			continue;
		struct menu *lf = 0;
		int it = menu_click_ex(mb->menus[i], mx, my,
		                       menubar_title_x(mb, i), mb->y, &lf);
		if (it >= 0) { *leaf = lf; *item = it; }
		return 1;
	}
	return 0;
}

int menubar_shortcut(const struct menubar *mb, int key, struct menu **leaf)
{
	if (!key) { if (leaf) *leaf = 0; return -1; }
	for (int i = 0; i < mb->n; i++) {
		struct menu *lf = 0;
		int it = menu_shortcut_walk(mb->menus[i], key, &lf);
		if (it >= 0) { if (leaf) *leaf = lf; return it; }
	}
	if (leaf)
		*leaf = 0;
	return -1;
}

/* ------------------------------------------------------------- text field */

#define TF_BG     0xffffffu   /* field background                 */
#define TF_FG     0x101418u   /* text colour                      */
#define TF_BORDER 0x3a3f48u   /* idle border                      */
#define TF_FOCUS  0x2f6bd8u   /* focused border accent + caret    */
#define TF_PAD    4           /* inner horizontal padding (px)    */

void textfield_init(struct textfield *tf, char *buf, int cap)
{
	tf->buf = buf;
	tf->cap = cap;
	tf->len = 0;
	tf->caret = 0;
	tf->scroll = 0;
	if (cap > 0)
		buf[0] = '\0';
}

void textfield_set(struct textfield *tf, const char *s)
{
	int i = 0;
	for (; s[i] && i < tf->cap - 1; i++)
		tf->buf[i] = s[i];
	tf->buf[i] = '\0';
	tf->len = i;
	tf->caret = i;
	tf->scroll = 0;
}

/* Pixel width of buf[a..b) in the proportional UI font. */
static int tf_span(const struct textfield *tf, int a, int b)
{
	int w = 0;
	for (int i = a; i < b; i++)
		w += gfx_ui_cw((unsigned char)tf->buf[i]);
	return w;
}

void textfield_draw(struct textfield *tf, int x, int y, int w, int h, int focused)
{
	gfx_fill_rect(x, y, w, h, TF_BG);
	gfx_rect(x, y, w, h, focused ? TF_FOCUS : TF_BORDER);

	int inner = x + TF_PAD;
	int avail = w - 2 * TF_PAD;
	if (avail < 1) avail = 1;
	int ty = y + (h - UIFONT_H) / 2;

	/* Scroll so the caret stays visible: never left of `scroll`, and the run
	 * from `scroll` to the caret must fit `avail` (leave 2px for the caret). */
	if (tf->scroll > tf->len) tf->scroll = tf->len;
	if (tf->caret < tf->scroll) tf->scroll = tf->caret;
	while (tf->scroll < tf->caret && tf_span(tf, tf->scroll, tf->caret) > avail - 2)
		tf->scroll++;

	/* Draw visible chars; place the caret as we pass it. */
	int px = inner;
	for (int i = tf->scroll; i < tf->len; i++) {
		int cw = gfx_ui_cw((unsigned char)tf->buf[i]);
		if (px - inner + cw > avail)
			break;
		char one[2] = { tf->buf[i], '\0' };
		gfx_ui_text(px, ty, one, TF_FG);
		if (focused && i == tf->caret)
			gfx_fill_rect(px, ty, 1, UIFONT_H, TF_FOCUS);
		px += cw;
	}
	if (focused && tf->caret >= tf->len) {           /* caret at end of text */
		if (px > inner + avail - 1) px = inner + avail - 1;
		gfx_fill_rect(px, ty, 1, UIFONT_H, TF_FOCUS);
	}
}

int textfield_click(struct textfield *tf, int mx, int my,
                    int x, int y, int w, int h)
{
	if (mx < x || mx >= x + w || my < y || my >= y + h)
		return 0;
	int inner = x + TF_PAD;
	int avail = w - 2 * TF_PAD;
	int px = inner;
	tf->caret = tf->len;                             /* default: past the text */
	for (int i = tf->scroll; i < tf->len; i++) {
		int cw = gfx_ui_cw((unsigned char)tf->buf[i]);
		if (px - inner + cw > avail) { tf->caret = i; break; }
		if (mx < px + cw / 2) { tf->caret = i; break; }
		px += cw;
	}
	return 1;
}

int textfield_key(struct textfield *tf, char c)
{
	unsigned char u = (unsigned char)c;
	switch (u) {
	case KEY_LEFT:
		if (tf->caret > 0) tf->caret--;
		return 1;
	case KEY_RIGHT:
		if (tf->caret < tf->len) tf->caret++;
		return 1;
	case KEY_HOME:
		tf->caret = 0;
		return 1;
	case KEY_END:
		tf->caret = tf->len;
		return 1;
	case '\b':                                       /* delete left of caret */
		if (tf->caret > 0) {
			for (int i = tf->caret - 1; i < tf->len; i++)
				tf->buf[i] = tf->buf[i + 1];
			tf->caret--;
			tf->len--;
		}
		return 1;
	case KEY_DELETE:                                 /* delete right of caret */
		if (tf->caret < tf->len) {
			for (int i = tf->caret; i < tf->len; i++)
				tf->buf[i] = tf->buf[i + 1];
			tf->len--;
		}
		return 1;
	default:
		if (u >= 0x20 && u < 0x7f && tf->len < tf->cap - 1) {
			for (int i = tf->len; i > tf->caret; i--)  /* open a gap */
				tf->buf[i] = tf->buf[i - 1];
			tf->buf[tf->caret] = c;
			tf->caret++;
			tf->len++;
			tf->buf[tf->len] = '\0';
			return 1;
		}
		return 0;                                    /* Enter/Esc/arrows: app handles */
	}
}

/* ------------------------------------------------------------- tab strip */

#define TAB_NEW_W   26   /* width of the trailing "+" button            */
#define TAB_CLOSE_W 16   /* right-edge close hit/draw zone within a tab */
#define TAB_MAX_W  168   /* a tab never grows wider than this           */
#define TAB_MIN_W   46   /* nor shrinks below this (then they overflow) */

#define TAB_STRIP_BG 0x1c2330u   /* strip backdrop (behind/under tabs)  */
#define TAB_ON_BG    0xf4f5f7u   /* active tab: matches the content pane */
#define TAB_OFF_BG   0x2a3342u   /* inactive tab                        */
#define TAB_ON_FG    0x101418u
#define TAB_OFF_FG   0xb8c0ccu

/* Even per-tab width: the strip minus the "+" button split across the tabs,
 * clamped so labels stay legible (and overflow rather than vanish if cramped). */
static int tabbar_each_w(int w, int n)
{
	if (n < 1) n = 1;
	int avail = w - TAB_NEW_W;
	int tw = avail / n;
	if (tw > TAB_MAX_W) tw = TAB_MAX_W;
	if (tw < TAB_MIN_W) tw = TAB_MIN_W;
	return tw;
}

/* x of the "+" button: just past the last tab, but never off the right edge. */
static int tabbar_new_x(int x, int w, int n, int tw)
{
	int nx = x + n * tw;
	if (nx > x + w - TAB_NEW_W)
		nx = x + w - TAB_NEW_W;
	return nx;
}

/* Draw `s` (proportional UI font) clipped to maxpx pixels, appending ".." when
 * it doesn't fit. Truncation measures real pixel widths, not a char count. */
static void tab_label(int x, int y, const char *s, int maxpx, unsigned fg)
{
	if (gfx_ui_width(s) <= maxpx) { gfx_ui_text(x, y, s, fg); return; }
	char buf[40];
	int k = 0;
	int dots = font_ui_adv['.'] * 2;
	int w = 0;
	for (int i = 0; s[i] && k < (int)sizeof buf - 3; i++) {
		int aw = font_ui_adv[(unsigned char)s[i]];
		if (w + aw + dots > maxpx)
			break;
		buf[k++] = s[i];
		w += aw;
	}
	buf[k++] = '.'; buf[k++] = '.'; buf[k] = '\0';
	gfx_ui_text(x, y, buf, fg);
}

/* A small 7x7 "×" centred on (cx,cy). */
static void tab_closebox(int cx, int cy, unsigned c)
{
	for (int i = 0; i < 7; i++) {
		gfx_pixel(cx + i,     cy + i, c);
		gfx_pixel(cx + 6 - i, cy + i, c);
	}
}

void tabbar_draw(int x, int y, int w, const char *const *labels, int n, int active)
{
	gfx_fill_rect(x, y, w, GUI_TAB_H, TAB_STRIP_BG);
	int tw = tabbar_each_w(w, n);
	int ty = y + (GUI_TAB_H - UIFONT_H) / 2;
	for (int i = 0; i < n; i++) {
		int tx = x + i * tw;
		int on = (i == active);
		unsigned bg = on ? TAB_ON_BG : TAB_OFF_BG;
		gfx_fill_rect(tx, y, tw - 1, GUI_TAB_H, bg);
		if (on)                                      /* accent bar on top */
			gfx_fill_rect(tx, y, tw - 1, 2, 0x2f6bd8);
		gfx_fill_rect(tx + tw - 1, y, 1, GUI_TAB_H, TAB_STRIP_BG);  /* divider */
		int has_x = n > 1;
		int textpx = tw - 8 - (has_x ? TAB_CLOSE_W : 4);
		tab_label(tx + 8, ty, labels[i], textpx, on ? TAB_ON_FG : TAB_OFF_FG);
		if (has_x)
			tab_closebox(tx + tw - TAB_CLOSE_W + 4, y + (GUI_TAB_H - 7) / 2,
			             on ? 0x707880 : 0x8a93a0);
	}
	int nx = tabbar_new_x(x, w, n, tw);
	gfx_ui_text(nx + (TAB_NEW_W - gfx_ui_width("+")) / 2, ty, "+", 0xc8d0dc);
}

int tabbar_hit(int x, int y, int w, int n, int mx, int my, int *index)
{
	if (my < y || my >= y + GUI_TAB_H || mx < x)
		return TABBAR_NONE;
	int tw = tabbar_each_w(w, n);
	int nx = tabbar_new_x(x, w, n, tw);
	if (mx >= nx && mx < nx + TAB_NEW_W)
		return TABBAR_NEW;
	int i = (mx - x) / tw;
	if (i < 0 || i >= n)
		return TABBAR_NONE;
	int tx = x + i * tw;
	if (n > 1 && mx >= tx + tw - TAB_CLOSE_W) {
		if (index) *index = i;
		return TABBAR_CLOSE;
	}
	if (index) *index = i;
	return TABBAR_SELECT;
}
