/* gfx.h — tiny user-space drawing helpers over a window's pixel buffer.
 *
 * Call gfx_init() once with the buffer from win_fb() and the window's client
 * width/height, then draw with the rect/glyph/text primitives. Colors are
 * XRGB8888 (0x00RRGGBB). Pass GFX_TRANSPARENT as a glyph's bg to skip its
 * background pixels. All primitives clip to the buffer bounds.
 */
#ifndef OWOS_GFX_H
#define OWOS_GFX_H

#define GFX_TRANSPARENT 0x01000000u

void gfx_init(unsigned *fb, int w, int h);
void gfx_clear(unsigned color);
void gfx_pixel(int x, int y, unsigned color);
void gfx_fill_rect(int x, int y, int w, int h, unsigned color);
void gfx_rect(int x, int y, int w, int h, unsigned color);   /* 1px outline */
void gfx_glyph(int x, int y, unsigned char ch, unsigned fg, unsigned bg);
void gfx_text(int x, int y, const char *s, unsigned fg, unsigned bg);

/* --------------------------------------------------------------- GUI widgets */
/*
 * Reusable pieces every windowed app tends to want, so they aren't reinvented
 * per app: a status line that owns its message, and a one-menu dropdown bar.
 * Standard metrics (8px glyphs):
 */
#define GUI_BAR_H  18    /* menu / status bar height */
#define GUI_ROW_H  16    /* dropdown item height     */
#define GUI_MENU_W 140   /* dropdown width (wide enough for a "Ctrl+S" hint)  */

/* A status line that owns its short message. */
struct status_bar { char msg[64]; };
void status_set(struct status_bar *s, const char *msg);
void status_draw(const struct status_bar *s, int x, int y, int w,
                 unsigned fg, unsigned bg);   /* fills the bar and shows msg */

/* A single drop-down menu (e.g. "File" with New/Open/Save/...). The owning app
 * draws the full-width bar background itself, then calls menu_draw() last so the
 * popup overlays the content.
 *
 * `hints`/`keys` are optional parallel arrays (NULL = none) giving each item a
 * keyboard accelerator: `hints[i]` is the label shown right-aligned in the
 * dropdown ("Ctrl+S"), `keys[i]` the KEY_* byte (KEY_SAVE...) the app's key
 * handler feeds to menu_shortcut() to fire that item without opening the menu. */
struct menu {
	const char *title;
	const char *const *items;
	int n;
	int open;
	const char *const  *hints;   /* optional: per-item accelerator label */
	const unsigned char *keys;   /* optional: per-item KEY_* shortcut byte */
};
void menu_draw(const struct menu *m, int x, int y);
/* Route a click at (mx,my) given the menu title's top-left (x,y). Toggles/closes
 * m->open as needed; returns the chosen item index, or -1 if none was chosen. */
int  menu_click(struct menu *m, int mx, int my, int x, int y);
/* Map a pressed key to a menu item via the `keys` table: returns the item index
 * whose accelerator equals `key`, or -1 if none (or no `keys` table). */
int  menu_shortcut(const struct menu *m, int key);

/* A row of dropdown menus sharing one bar (File / Edit / View ...). Holds an
 * array of `struct menu` pointers laid out left-to-right from (x,y); the helpers
 * keep at most one dropdown open at a time, so the owning app gets a real menu
 * bar without re-implementing the open/close bookkeeping per app. */
struct menubar {
	struct menu *const *menus;
	int n;
	int x, y;          /* bar origin: the title row's top-left */
};
/* Draw every title on the bar, then the open menu's dropdown last (on top). The
 * app still fills the full-width bar background itself, then calls this last. */
void menubar_draw(const struct menubar *mb);
/* Route a click. Returns 1 if it hit the bar or an open dropdown (the app should
 * repaint and not process the click further), 0 if it was elsewhere. When an
 * item is chosen, *menu and *item give its menu and item index; otherwise *item
 * is -1 (only open/close state changed). Clicking off an open menu closes it. */
int  menubar_click(struct menubar *mb, int mx, int my, int *menu, int *item);
/* Map a pressed key to any menu's accelerator: returns the item index and sets
 * *menu to which menu owns it, or -1 (with *menu = -1) if none matches. */
int  menubar_shortcut(const struct menubar *mb, int key, int *menu);
/* Close every dropdown on the bar (e.g. after firing an accelerator or Esc). */
void menubar_close(struct menubar *mb);

#endif /* OWOS_GFX_H */
