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

/* Proportional sans-serif UI font for chrome (menus, titles, tabs, status).
 * Variable per-glyph advance; cell is UIFONT_W x UIFONT_H. gfx_ui_text blends
 * fg over the existing pixels (fill the background first), so it has no bg
 * argument. gfx_ui_width gives a string's pixel width for layout/centring. */
#define UIFONT_W 16
#define UIFONT_H 18
void gfx_ui_text(int x, int y, const char *s, unsigned fg);
int  gfx_ui_width(const char *s);
int  gfx_ui_cw(unsigned char c);   /* one glyph's pen advance (for fitting text) */

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

/* Per-item state bits for the optional `flags` array (mutable — apps toggle
 * them at runtime). DISABLED items are dimmed and inert (no click, no
 * accelerator); CHECKED items show a checkmark in a left gutter. */
#define MENU_DISABLED 0x01
#define MENU_CHECKED  0x02

/* A drop-down menu (e.g. "File" with New/Open/Save/...). The owning app draws the
 * full-width bar background itself, then calls menu_draw() last so the popup
 * overlays the content. The same struct doubles as a submenu (see `subs`): a
 * submenu has no title bar of its own, just the popup, opened by its parent item.
 *
 * Optional parallel arrays (NULL = absent), each indexed by item:
 *  - `hints`/`keys`: a keyboard accelerator — `hints[i]` is the right-aligned
 *    label ("Ctrl+S"), `keys[i]` the KEY_* byte fed to menu_shortcut().
 *  - `flags`: MENU_DISABLED|MENU_CHECKED state bits (mutable; reserves a check
 *    gutter for the whole menu when present).
 *  - `subs`: a child submenu per item (subs[i] != NULL makes item i a parent that
 *    opens subs[i] to its right instead of firing an action). */
struct menu {
	const char *title;
	const char *const *items;
	int n;
	int open;
	const char *const  *hints;   /* optional: per-item accelerator label */
	const unsigned char *keys;   /* optional: per-item KEY_* shortcut byte */
	unsigned char       *flags;  /* optional: per-item MENU_DISABLED|MENU_CHECKED */
	struct menu *const  *subs;   /* optional: per-item submenu (NULL = none)      */
};
void menu_draw(const struct menu *m, int x, int y);
/* Route a click at (mx,my) given the menu title's top-left (x,y). Toggles/closes
 * m->open (and any submenus) as needed; returns the chosen item index of `m`, or
 * -1 if nothing in `m` fired (a disabled item, an opened submenu, a chosen
 * *submenu* item, or a miss). Use menu_click_ex to dispatch submenu items. */
int  menu_click(struct menu *m, int mx, int my, int x, int y);
/* Like menu_click but reports the leaf menu that owns the fired item via *leaf
 * (which may be a submenu, not `m`). Returns that menu's item index, or -1. */
int  menu_click_ex(struct menu *m, int mx, int my, int x, int y,
                   struct menu **leaf);
/* Map a pressed key to a menu item via the `keys` tables (searches submenus too,
 * skips disabled items): returns the item index, or -1 if none. */
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
 * item is chosen, *leaf and *item give the leaf menu (a bar menu OR one of its
 * submenus — dispatch by pointer) and its item index; otherwise *item is -1
 * (only open/close state changed). Clicking off an open menu closes it. */
int  menubar_click(struct menubar *mb, int mx, int my, struct menu **leaf, int *item);
/* Map a pressed key to any menu's accelerator (submenus included): returns the
 * item index and sets *leaf to the owning menu, or -1 (with *leaf = NULL). */
int  menubar_shortcut(const struct menubar *mb, int key, struct menu **leaf);
/* Close every dropdown on the bar and its submenus (after an accelerator/Esc). */
void menubar_close(struct menubar *mb);

/* --------------------------------------------------------------- tab strip */
/*
 * A horizontal strip of tabs with per-tab close boxes and a trailing "+" button,
 * the way a tabbed browser/editor lays out documents. The widget is stateless:
 * the app owns the tab list and its bookkeeping; this only draws `labels[0..n)`
 * (highlighting `active`) and maps a click back to an action. Tabs share the
 * available width evenly (clamped), labels are truncated to fit, and the close
 * box appears only when more than one tab is open.
 */
#define GUI_TAB_H 24    /* tab strip height */

/* tabbar_hit() outcomes. SELECT/CLOSE fill *index with the tab; NEW is the "+". */
enum { TABBAR_NONE = 0, TABBAR_SELECT, TABBAR_CLOSE, TABBAR_NEW };

/* Draw the strip filling [x, x+w) at height GUI_TAB_H from y. The app fills no
 * background itself — the widget paints the whole strip. */
void tabbar_draw(int x, int y, int w, const char *const *labels, int n, int active);
/* Hit-test a click at (mx,my) against the same geometry. Returns a TABBAR_*
 * code; for SELECT/CLOSE sets *index to the tab index. */
int  tabbar_hit(int x, int y, int w, int n, int mx, int my, int *index);

#endif /* OWOS_GFX_H */
