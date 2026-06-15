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

/* Blit a w*h ARGB (0xAARRGGBB) bitmap at (x,y), alpha-blending each pixel over
 * the framebuffer. Fully transparent pixels are skipped; clipped to bounds. */
void gfx_blit_argb(int x, int y, int w, int h, const unsigned *argb)
{
	for (int r = 0; r < h; r++) {
		int sy = y + r;
		if ((unsigned)sy >= (unsigned)g_h) continue;
		const unsigned *src = argb + (long)r * w;
		unsigned *drow = g_fb + (long)sy * g_w;
		for (int c = 0; c < w; c++) {
			int sx = x + c;
			if ((unsigned)sx >= (unsigned)g_w) continue;
			unsigned px = src[c], a = px >> 24;
			if (a == 0) continue;
			drow[sx] = (a == 255) ? (px & 0xffffff)
			                      : gfx_blend(px & 0xffffff, drow[sx], a);
		}
	}
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

#define TAB_ICON     16  /* favicon slot drawn at the left of each tab  */
#define TAB_ICON_PAD 6   /* left margin before the icon                 */
#define TAB_ICON_GAP 5   /* gap between the icon and the label          */

static int isqrt_i(int v)
{
	int r = 0;
	if (v < 0) return 0;
	while ((r + 1) * (r + 1) <= v) r++;
	return r;
}

/* The default 16x16 globe shown when a tab has no decoded favicon: a blue disc
 * with an equator, two parallels, and three meridians (one straight, two
 * curved along the disc edge). Drawn directly so no bitmap asset is needed. */
static void tab_globe(int x, int y)
{
	const int R = 7, cx = 8, cy = 8;
	for (int py = 0; py < 16; py++) {
		int dy = py - cy;
		int e = isqrt_i(R * R - dy * dy);            /* disc half-width this row */
		for (int px = 0; px < 16; px++) {
			int dx = px - cx, d2 = dx * dx + dy * dy;
			if (d2 > R * R) continue;
			unsigned col;
			if      (d2 >= (R - 1) * (R - 1))          col = 0x2f5fb0u;  /* rim    */
			else if (dy == 0 || dy == 3 || dy == -3)   col = 0xcfe0ffu;  /* lats   */
			else if (dx == 0 || dx == e / 2 || dx == -e / 2)
			                                           col = 0xcfe0ffu;  /* longs  */
			else                                       col = 0x5b8defu;  /* ocean  */
			gfx_pixel(x + px, y + py, col);
		}
	}
}

/* icons[i] points to a 16x16 ARGB favicon for tab i (NULL -> draw the globe);
 * pass a NULL icons array to give every tab the globe. */
void tabbar_draw(int x, int y, int w, const char *const *labels,
                 const unsigned *const *icons, int n, int active)
{
	gfx_fill_rect(x, y, w, GUI_TAB_H, TAB_STRIP_BG);
	int tw = tabbar_each_w(w, n);
	int ty = y + (GUI_TAB_H - UIFONT_H) / 2;
	int iy = y + (GUI_TAB_H - TAB_ICON) / 2;
	for (int i = 0; i < n; i++) {
		int tx = x + i * tw;
		int on = (i == active);
		unsigned bg = on ? TAB_ON_BG : TAB_OFF_BG;
		gfx_fill_rect(tx, y, tw - 1, GUI_TAB_H, bg);
		if (on)                                      /* accent bar on top */
			gfx_fill_rect(tx, y, tw - 1, 2, 0x2f6bd8);
		gfx_fill_rect(tx + tw - 1, y, 1, GUI_TAB_H, TAB_STRIP_BG);  /* divider */

		int ix = tx + TAB_ICON_PAD;
		if (icons && icons[i]) gfx_blit_argb(ix, iy, TAB_ICON, TAB_ICON, icons[i]);
		else                   tab_globe(ix, iy);

		int lx = ix + TAB_ICON + TAB_ICON_GAP;
		int has_x = n > 1;
		int textpx = tw - (lx - tx) - (has_x ? TAB_CLOSE_W : 4);
		tab_label(lx, ty, labels[i], textpx, on ? TAB_ON_FG : TAB_OFF_FG);
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

/* ------------------------------------------------------------- button */

/* Per-state face: fill, a 1px top highlight, label colour. The border is the
 * same near-black as the menu/tab chrome so buttons sit on any background. */
struct btn_face { unsigned bg, top, fg; };
static const struct btn_face btn_faces[] = {
	[BTN_NORMAL]   = { 0x3a4658u, 0x4a5a70u, 0xe6ebf2u },
	[BTN_HOVER]    = { 0x46546au, 0x586a84u, 0xffffffu },
	[BTN_PRESSED]  = { 0x2a3342u, 0x2a3342u, 0xc8d0dcu },
	[BTN_DISABLED] = { 0x2a3038u, 0x2a3038u, 0x636b78u },
};

void button_draw(int x, int y, int w, int h, const char *label, int state)
{
	if (state < 0 || state > BTN_DISABLED)
		state = BTN_NORMAL;
	const struct btn_face *f = &btn_faces[state];
	gfx_fill_rect(x, y, w, h, f->bg);
	if (f->top != f->bg)                          /* raised: lit top edge */
		gfx_fill_rect(x + 1, y + 1, w - 2, 1, f->top);
	gfx_rect(x, y, w, h, 0x05070a);

	/* Centre the label; nudge it down a pixel while pressed so it "sinks". */
	int tw = gfx_ui_width(label);
	int tx = x + (w - tw) / 2;
	int ty = y + (h - UIFONT_H) / 2 + (state == BTN_PRESSED ? 1 : 0);
	gfx_ui_text(tx, ty, label, f->fg);
}

int button_hit(int x, int y, int w, int h, int mx, int my)
{
	return mx >= x && mx < x + w && my >= y && my < y + h;
}

/* ------------------------------------------------------------- progress bar */

#define PROG_TROUGH 0x1c2330u   /* sunken background        */
#define PROG_FILL   0x2f6bd8u   /* accent-blue done portion */
#define PROG_TOP    0x5a8fe0u   /* lit top edge of the fill */
#define PROG_BORDER 0x05070au

void progress_draw(int x, int y, int w, int h, int value, int max, int show_pct)
{
	if (max < 0) max = 0;
	if (value < 0) value = 0;
	if (value > max) value = max;
	int pct = max > 0 ? value * 100 / max : 0;

	gfx_fill_rect(x, y, w, h, PROG_TROUGH);

	/* Fill the inner area (inside the 1px border) proportionally. */
	int inner = w - 2;
	int fw = max > 0 && inner > 0 ? (int)((long)inner * value / max) : 0;
	if (fw > 0) {
		gfx_fill_rect(x + 1, y + 1, fw, h - 2, PROG_FILL);
		if (h > 3)
			gfx_fill_rect(x + 1, y + 1, fw, 1, PROG_TOP);
	}
	gfx_rect(x, y, w, h, PROG_BORDER);

	if (show_pct) {
		char buf[5];                              /* "100%" + NUL */
		int p = pct, k = 0;
		if (p >= 100) { buf[k++] = '1'; buf[k++] = '0'; buf[k++] = '0'; }
		else {
			if (p >= 10) buf[k++] = '0' + p / 10;
			buf[k++] = '0' + p % 10;
		}
		buf[k++] = '%';
		buf[k] = '\0';
		int tw = gfx_ui_width(buf);
		int tx = x + (w - tw) / 2;
		int ty = y + (h - UIFONT_H) / 2;
		/* White over the blue done-portion, light grey over the trough; pick by
		 * whether the label's centre sits on the filled side. */
		unsigned fg = (w / 2 < 1 + fw) ? 0xffffffu : 0xc8d0dcu;
		gfx_ui_text(tx, ty, buf, fg);
	}
}

/* ------------------------------------------------------------- scrollbar */

#define SCROLL_TRACK     0xc8ccd2u   /* light track groove        */
#define SCROLL_THUMB     0x6b7686u   /* draggable slate thumb     */
#define SCROLL_ARROW_BG  0xb4b9c2u   /* arrow-button face         */
#define SCROLL_ARROW_FG  0x4a5564u   /* arrowhead                 */
#define SCROLL_MIN_THUMB 12          /* keep the thumb grabbable  */

/* Square arrow-button height for a w-wide, h-tall bar, or 0 when the bar is too
 * short to hold both arrows and a grabbable thumb (then it's a plain track). */
static int scrollbar_arrow_h(int w, int h)
{
	int a = w;
	if (a < 4 || h < 2 * a + SCROLL_MIN_THUMB) return 0;
	return a;
}

/* Thumb height + top offset within a track of height `tr` for the current counts,
 * shared by scrollbar_draw and scrollbar_pos so the visual and the hit-test
 * agree. Assumes total > visible (the only case with a thumb). */
static void scrollbar_thumb(int tr, int scroll, int total, int visible,
                            int *th, int *ty)
{
	int t = (int)((long)tr * visible / total);
	if (t < SCROLL_MIN_THUMB) t = SCROLL_MIN_THUMB;
	if (t > tr) t = tr;
	int maxs = total - visible;
	if (scroll < 0) scroll = 0;
	if (scroll > maxs) scroll = maxs;
	int span = tr - t;
	*th = t;
	*ty = (maxs > 0) ? (int)((long)span * scroll / maxs) : 0;
}

/* A filled triangle centred in the arrow box [bx,by,w,a], pointing up or down. */
static void scrollbar_tri(int bx, int by, int w, int a, int up, unsigned color)
{
	int sz = (w - 2) / 2;
	if (sz < 3) sz = 3;
	if (sz > a / 2) sz = a / 2;
	if (sz < 1) sz = 1;
	int cx = bx + w / 2;
	int top = by + (a - sz) / 2;
	for (int r = 0; r < sz; r++) {
		int half = up ? r : (sz - 1 - r);
		gfx_fill_rect(cx - half, top + r, half * 2 + 1, 1, color);
	}
}

void scrollbar_draw(int x, int y, int w, int h, int scroll, int total, int visible)
{
	int a = scrollbar_arrow_h(w, h);
	int tr = h - 2 * a;                    /* track region height */

	gfx_fill_rect(x, y, w, h, SCROLL_TRACK);
	if (a > 0) {
		gfx_fill_rect(x, y, w, a, SCROLL_ARROW_BG);            /* up box   */
		gfx_fill_rect(x, y + h - a, w, a, SCROLL_ARROW_BG);    /* down box */
		scrollbar_tri(x, y, w, a, 1, SCROLL_ARROW_FG);
		scrollbar_tri(x, y + h - a, w, a, 0, SCROLL_ARROW_FG);
	}
	if (total > visible && visible > 0 && tr > 0 && w > 2) {
		int th, ty;
		scrollbar_thumb(tr, scroll, total, visible, &th, &ty);
		gfx_fill_rect(x + 1, y + a + ty, w - 2, th, SCROLL_THUMB);
	}
}

int scrollbar_hit(int x, int y, int w, int h, int mx, int my)
{
	if (mx < x || mx >= x + w || my < y || my >= y + h)
		return SCROLLBAR_NONE;
	int a = scrollbar_arrow_h(w, h);
	if (a > 0) {
		if (my < y + a)        return SCROLLBAR_UP;
		if (my >= y + h - a)   return SCROLLBAR_DOWN;
	}
	return SCROLLBAR_TRACK;
}

int scrollbar_pos(int x, int y, int w, int h, int mx, int my,
                  int scroll, int total, int visible)
{
	if (scrollbar_hit(x, y, w, h, mx, my) != SCROLLBAR_TRACK) return scroll;
	int maxs = total - visible;
	if (maxs <= 0) return 0;
	int a = scrollbar_arrow_h(w, h);
	int tr = h - 2 * a;                    /* track region height       */
	if (tr <= 0) return scroll;
	int th, ty;
	scrollbar_thumb(tr, scroll, total, visible, &th, &ty);
	int span = tr - th;
	int rel = my - (y + a) - th / 2;       /* centre the thumb on the cursor */
	int s = (span > 0) ? (int)((long)rel * maxs / span) : 0;
	if (s < 0) s = 0;
	if (s > maxs) s = maxs;
	return s;
}

/* ------------------------------------------------------------- sidebar */

#define SB_GRIP_DOT 0x33415au   /* grip dimple colour, picked off the divider */

static int sb_clamp(const struct sidebar *sb, int w)
{
	if (w < sb->min_w) w = sb->min_w;
	if (w > sb->max_w) w = sb->max_w;
	return w;
}

void sidebar_init(struct sidebar *sb, int w, int min_w, int max_w)
{
	if (max_w < min_w) max_w = min_w;
	sb->min_w = min_w;
	sb->max_w = max_w;
	sb->resizable = 1;
	sb->dragging = 0;
	sb->w = sb_clamp(sb, w);
}

void sidebar_set_width(struct sidebar *sb, int w)
{
	sb->w = sb_clamp(sb, w);
}

void sidebar_draw(struct sidebar *sb, int x, int y, int w, int h, unsigned bg,
                  unsigned divider)
{
	gfx_fill_rect(x, y, w, h, bg);
	gfx_fill_rect(x + w, y, 1, h, divider);

	/* A short centred column of dimples marks the divider as a drag handle. */
	if (sb->resizable && h > 24) {
		int cy = y + h / 2 - 9;
		for (int i = 0; i < 4; i++)
			gfx_fill_rect(x + w - 1, cy + i * 5, 2, 2, SB_GRIP_DOT);
	}
}

int sidebar_press(struct sidebar *sb, int x, int y, int w, int h,
                  int mx, int my)
{
	sb->dragging = 0;
	if (!sb->resizable)
		return 0;
	int gx = x + w;                              /* the divider line            */
	if (mx >= gx - SIDEBAR_GRIP / 2 && mx <= gx + SIDEBAR_GRIP / 2 &&
	    my >= y && my < y + h) {
		sb->dragging = 1;
		return 1;
	}
	return 0;
}

int sidebar_drag(struct sidebar *sb, int x, int mx)
{
	if (!sb->dragging)
		return 0;
	int nw = sb_clamp(sb, mx - x);
	if (nw == sb->w)
		return 0;
	sb->w = nw;
	return 1;
}
