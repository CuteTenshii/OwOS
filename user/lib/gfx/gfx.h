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
/* Read back the current surface (buffer + dimensions), so a helper that borrows
 * gfx for its own window (e.g. a modal dialog) can gfx_init back to it after.
 */
void gfx_get(unsigned **fb, int *w, int *h);
void gfx_clear(unsigned color);
/* Confine all subsequent primitives to a rectangle (intersected with the
 * buffer), or gfx_clip_none() to restore the full buffer. A compositor uses
 * this to repaint one damage rect at a time without touching pixels outside it.
 * gfx_init resets it to the whole buffer. */
void gfx_set_clip(int x, int y, int w, int h);
void gfx_clip_none(void);
void gfx_get_clip(int *x, int *y, int *w, int *h); /* the active clip rect */
void gfx_pixel(int x, int y, unsigned color);
void gfx_fill_rect(int x, int y, int w, int h, unsigned color);
void gfx_rect(int x, int y, int w, int h, unsigned color); /* 1px outline */
void gfx_glyph(int x, int y, unsigned char ch, unsigned fg, unsigned bg);
void gfx_text(int x, int y, const char *s, unsigned fg, unsigned bg);
/* Monospace text that decodes UTF-8 (one CP437 cell per codepoint, columns stay
 * aligned; accents via Unicode->CP437). Returns the pen x after the string. */
int gfx_text_utf8(int x, int y, const char *s, unsigned fg, unsigned bg);

/* Blit a w*h ARGB (0xAARRGGBB) bitmap at (x,y), alpha-blending over the buffer
 * (fully transparent pixels skipped). Used for tab favicons. */
void gfx_blit_argb(int x, int y, int w, int h, const unsigned *argb);

/* Proportional sans-serif UI font for chrome (menus, titles, tabs, status).
 * Variable per-glyph advance; cell is UIFONT_W x UIFONT_H. gfx_ui_text blends
 * fg over the existing pixels (fill the background first), so it has no bg
 * argument. gfx_ui_width gives a string's pixel width for layout/centring. */
#define UIFONT_W 16
#define UIFONT_H 18
void gfx_ui_text(int x, int y, const char *s, unsigned fg); /* UTF-8 */
int gfx_ui_width(const char *s);                            /* UTF-8 string pixel width */
int gfx_ui_cw(unsigned char c);                             /* one byte/ASCII glyph's pen advance */
int gfx_ui_cw_cp(unsigned cp);                              /* one Unicode codepoint's pen advance         */
/* Like gfx_ui_text, but a string wider than maxw pixels is cut and ends in "..";
 * nothing is drawn for maxw <= 0. */
void gfx_ui_text_clip(int x, int y, const char *s, unsigned fg, int maxw);

/* Sized UI text — the sans face at an arbitrary px, uncached (for headings). y
 * is the line top; the baseline is derived from the px ascent. */
void gfx_ui_text_px(int x, int y, const char *s, unsigned fg, int px);
void gfx_ui_text_px_clip(int x, int y, const char *s, unsigned fg, int px, int ct,
                         int cb); /* rows clipped to [ct,cb); ct>=cb = none */
int gfx_ui_width_px(const char *s, int px);
int gfx_ui_cw_cp_px(unsigned cp, int px);
int gfx_ui_ascent_px(int px);

/* --------------------------------------------------------------- GUI widgets
 */
/*
 * Reusable pieces every windowed app tends to want, so they aren't reinvented
 * per app: a status line that owns its message, and a one-menu dropdown bar.
 * Standard metrics (8px glyphs):
 */
#define GUI_BAR_H 18   /* menu / status bar height */
#define GUI_ROW_H 16   /* dropdown item height     */
#define GUI_MENU_W 140 /* dropdown width (wide enough for a "Ctrl+S" hint)  */

/* A status line that owns its short message. */
struct status_bar {
  char msg[64];
};
void status_set(struct status_bar *s, const char *msg);
void status_draw(const struct status_bar *s, int x, int y, int w, unsigned fg,
                 unsigned bg); /* fills the bar and shows msg */

/* Per-item state bits for the optional `flags` array (mutable — apps toggle
 * them at runtime). DISABLED items are dimmed and inert (no click, no
 * accelerator); CHECKED items show a checkmark in a left gutter. */
#define MENU_DISABLED 0x01
#define MENU_CHECKED 0x02

/* A drop-down menu (e.g. "File" with New/Open/Save/...). The owning app draws
 * the full-width bar background itself, then calls menu_draw() last so the
 * popup overlays the content. The same struct doubles as a submenu (see
 * `subs`): a submenu has no title bar of its own, just the popup, opened by its
 * parent item.
 *
 * Optional parallel arrays (NULL = absent), each indexed by item:
 *  - `hints`/`keys`: a keyboard accelerator — `hints[i]` is the right-aligned
 *    label ("Ctrl+S"), `keys[i]` the KEY_* byte fed to menu_shortcut().
 *  - `flags`: MENU_DISABLED|MENU_CHECKED state bits (mutable; reserves a check
 *    gutter for the whole menu when present).
 *  - `subs`: a child submenu per item (subs[i] != NULL makes item i a parent
 * that opens subs[i] to its right instead of firing an action). */
struct menu {
  const char *title;
  const char *const *items;
  int n;
  int open;
  const char *const *hints;  /* optional: per-item accelerator label */
  const unsigned char *keys; /* optional: per-item KEY_* shortcut byte */
  unsigned char *flags;      /* optional: per-item MENU_DISABLED|MENU_CHECKED */
  struct menu *const *subs;  /* optional: per-item submenu (NULL = none)      */
};
void menu_draw(const struct menu *m, int x, int y);
/* Route a click at (mx,my) given the menu title's top-left (x,y).
 * Toggles/closes m->open (and any submenus) as needed; returns the chosen item
 * index of `m`, or -1 if nothing in `m` fired (a disabled item, an opened
 * submenu, a chosen *submenu* item, or a miss). Use menu_click_ex to dispatch
 * submenu items. */
int menu_click(struct menu *m, int mx, int my, int x, int y);
/* Like menu_click but reports the leaf menu that owns the fired item via *leaf
 * (which may be a submenu, not `m`). Returns that menu's item index, or -1. */
int menu_click_ex(struct menu *m, int mx, int my, int x, int y, struct menu **leaf);
/* Map a pressed key to a menu item via the `keys` tables (searches submenus
 * too, skips disabled items): returns the item index, or -1 if none. */
int menu_shortcut(const struct menu *m, int key);
/* A free-standing popup list (right-click menu) whose top-left is (x,y); m->title
 * is unused and submenus are not supported. menu_popup_click returns the enabled
 * item under (mx,my), or -1 for a miss or a disabled item. The app owns when it is
 * open and closes it after any click. */
void menu_popup_draw(const struct menu *m, int x, int y);
int menu_popup_click(const struct menu *m, int mx, int my, int x, int y);

/* A row of dropdown menus sharing one bar (File / Edit / View ...). Holds an
 * array of `struct menu` pointers laid out left-to-right from (x,y); the
 * helpers keep at most one dropdown open at a time, so the owning app gets a
 * real menu bar without re-implementing the open/close bookkeeping per app. */
struct menubar {
  struct menu *const *menus;
  int n;
  int x, y; /* bar origin: the title row's top-left */
};
/* Draw every title on the bar, then the open menu's dropdown last (on top). The
 * app still fills the full-width bar background itself, then calls this last.
 */
void menubar_draw(const struct menubar *mb);
/* Route a click. Returns 1 if it hit the bar or an open dropdown (the app
 * should repaint and not process the click further), 0 if it was elsewhere.
 * When an item is chosen, *leaf and *item give the leaf menu (a bar menu OR one
 * of its submenus — dispatch by pointer) and its item index; otherwise *item is
 * -1 (only open/close state changed). Clicking off an open menu closes it. */
int menubar_click(struct menubar *mb, int mx, int my, struct menu **leaf, int *item);
/* Map a pressed key to any menu's accelerator (submenus included): returns the
 * item index and sets *leaf to the owning menu, or -1 (with *leaf = NULL). */
int menubar_shortcut(const struct menubar *mb, int key, struct menu **leaf);
/* Close every dropdown on the bar and its submenus (after an accelerator/Esc).
 */
void menubar_close(struct menubar *mb);

/* --------------------------------------------------------------- text field */
/*
 * A single-line editable text field — an address bar, a rename box, any "type a
 * string and press Enter" input — so apps don't re-roll caret/scroll/editing by
 * hand. The app owns the character buffer (NUL-terminated); the widget tracks
 * the caret and horizontal scroll, drawing the proportional UI font in a white
 * field that scrolls to keep the caret in view.
 *
 * Bind once with textfield_init(&tf, buf, cap), set initial text with
 * textfield_set, then each frame draw it and feed WEV_KEY chars to
 * textfield_key. The current text is just tf.buf (NUL-terminated, tf.len
 * chars).
 */
#define TEXTFIELD_H 26 /* a comfortable single-line field height */

#define TEXTFIELD_UNDO 32 /* undo steps kept per field */

struct tf_snap {
  char *text; /* heap copy of the buffer */
  int caret;
};

struct textfield {
  char *buf;  /* app-owned buffer, NUL-terminated         */
  int cap;    /* buffer capacity in bytes (incl. the NUL) */
  int len;    /* current text length                      */
  int caret;  /* insertion point, 0..len                  */
  int anchor; /* other end of the selection; == caret when empty */
  int scroll; /* leftmost visible char index (maintained by textfield_draw) */
  struct tf_snap undo[TEXTFIELD_UNDO], redo[TEXTFIELD_UNDO];
  int nundo, nredo;
  int last_edit; /* kind of the previous edit, to merge a run of typing into one undo step */
};

void textfield_init(struct textfield *tf, char *buf, int cap);
void textfield_set(struct textfield *tf, const char *s); /* replace text; caret to end; clears undo */
/* Draw the field box filling [x,y,w,h]; `focused` shows a caret and a bright
 * border. Updates tf->scroll to keep the caret visible. */
void textfield_draw(struct textfield *tf, int x, int y, int w, int h, int focused);
/* Place the caret from a click and report focus: returns 1 if (mx,my) fell
 * inside [x,y,w,h] (the app should treat the field as focused), 0 otherwise.
 * Uses the scroll left by the last textfield_draw, so call draw first each
 * frame. */
int textfield_click(struct textfield *tf, int mx, int my, int x, int y, int w, int h);
/* Extend the selection while the button is held after a textfield_click; the
 * caret follows mx and the view scrolls past the field's edges. */
void textfield_drag(struct textfield *tf, int mx, int x, int w);
/* Selected range [*start,*end); returns 1 if non-empty. */
int textfield_selection(const struct textfield *tf, int *start, int *end);
void textfield_select_all(struct textfield *tf);
/* Insert s[0..n) at the caret over the selection (paste): printable ASCII only,
 * newlines and tabs become spaces. One undo step. */
void textfield_insert(struct textfield *tf, const char *s, int n);
/* Feed a WEV_KEY char with its KMOD_* flags. Handles printable insert (replacing
 * the selection), Backspace, Delete, Left/Right/Home/End (Shift extends the
 * selection), Ctrl+A, Ctrl+Z (undo) and Ctrl+Y or Ctrl+Shift+Z (redo). Returns 1 if it consumed the key (repaint), 0 if
 * not, e.g. Enter/Esc/arrows the field doesn't use, which the app handles. */
int textfield_key(struct textfield *tf, char c, int mods);

/* --------------------------------------------------------------- tab strip */
/*
 * A horizontal strip of tabs with per-tab close boxes and a trailing "+"
 * button, the way a tabbed browser/editor lays out documents. The app owns the
 * tab list and its bookkeeping; this only draws `labels[0..n)` (highlighting
 * `active`) and maps a click back to an action. Tabs share the available width
 * evenly (clamped), labels are truncated to fit, and every tab has a close box.
 * When the tabs don't fit at their minimum width, the strip shows a window of
 * them between scroll arrows.
 */
#define GUI_TAB_H 24 /* tab strip height */

/* Per-strip state the app keeps between frames (zero-initialize it) */
struct tabbar {
  int first;       /* first visible tab while the strip overflows */
  int last_active; /* scroll to reveal `active` only when it changes */
};

/* tabbar_hit() outcomes. SELECT/CLOSE fill *index with the tab; NEW is the "+";
 * SCROLL means an arrow moved the strip and the app should repaint. */
enum { TABBAR_NONE = 0, TABBAR_SELECT, TABBAR_CLOSE, TABBAR_NEW, TABBAR_SCROLL };

/* Draw the strip filling [x, x+w) at height GUI_TAB_H from y. The app fills no
 * background itself; the widget paints the whole strip. icons[i] is an
 * optional 16x16 ARGB favicon for tab i (NULL -> a default globe); pass a NULL
 * icons array to give every tab the globe. */
void tabbar_draw(struct tabbar *tb, int x, int y, int w, const char *const *labels, const unsigned *const *icons, int n,
                 int active);
/* Hit-test a click at (mx,my) against the geometry of the last draw. Returns a
 * TABBAR_* code; for SELECT/CLOSE sets *index to the tab index. */
int tabbar_hit(struct tabbar *tb, int x, int y, int w, int n, int mx, int my, int *index);
/* Scroll an overflowing strip by `delta` tabs (positive = right) */
void tabbar_scroll(struct tabbar *tb, int w, int n, int delta);

/* --------------------------------------------------------------- button */
/*
 * A clickable push button with a centred label — an "OK"/"Cancel"/"Connect"
 * the way a dialog or toolbar wants one. Stateless like the tab strip: the app
 * owns no widget struct, it just draws the button each frame with the visual
 * state it's tracking and hit-tests clicks against the same rectangle.
 *
 * `state` selects the look. The app decides it: BTN_HOVER while the pointer is
 * over the button (track the last mouse-move), BTN_PRESSED between mouse-down
 * and mouse-up on it, BTN_DISABLED for an inert/dimmed button (button_hit still
 * returns 0 for it so it can't fire), else BTN_NORMAL.
 */
#define GUI_BTN_H 26 /* a comfortable button height */

enum { BTN_NORMAL = 0, BTN_HOVER, BTN_PRESSED, BTN_DISABLED };

/* Draw the button filling [x,y,w,h] with `label` centred, in the given state.
 */
void button_draw(int x, int y, int w, int h, const char *label, int state);
/* Returns 1 if (mx,my) falls inside [x,y,w,h], 0 otherwise (geometry only — the
 * app gates on its own disabled state before treating a hit as a press). */
int button_hit(int x, int y, int w, int h, int mx, int my);

/* --------------------------------------------------------------- checkbox, radio, switch */
/*
 * Two-state controls with an optional trailing label (NULL or "" for none).
 * Stateless like the button: the app keeps `checked` / `on`, draws each frame
 * with the BTN_* visual state it tracks (hover, pressed, disabled), and
 * flips its value when control_hit() reports a click on an enabled control.
 * A radio group is the app's business: set the clicked one, clear the rest.
 * `focused` draws a focus ring for keyboard operation (Space toggles).
 * The control occupies [x, x + control_width()) by GUI_CTRL_H from y.
 */
#define GUI_CTRL_H 18     /* row height, label included */
#define GUI_CHECK_SIZE 14 /* checkbox and radio box */
#define GUI_SWITCH_W 28
#define GUI_SWITCH_H 16

enum { GFX_CTRL_CHECKBOX = 0, GFX_CTRL_RADIO, GFX_CTRL_SWITCH };

void checkbox_draw(int x, int y, const char *label, int checked, int state, int focused);
void radio_draw(int x, int y, const char *label, int checked, int state, int focused);
void switch_draw(int x, int y, const char *label, int on, int state, int focused);
/* Total width of a GFX_CTRL_* kind with its label. */
int control_width(int kind, const char *label);
/* 1 if (mx,my) is on the box or its label (geometry only, like button_hit). */
int control_hit(int kind, int x, int y, const char *label, int mx, int my);

/* --------------------------------------------------------------- progress bar
 */
/*
 * A horizontal progress bar: a sunken trough with an accent-blue fill showing
 * `value`/`max` (clamped to 0..max), for a download/copy/load. Stateless — the
 * app passes the current counts each frame. With show_pct != 0 it overlays a
 * centred "NN%" label; a value/max of N/0 reads as 0%.
 */
#define GUI_PROGRESS_H 16 /* a comfortable bar height */

void progress_draw(int x, int y, int w, int h, int value, int max, int show_pct);

/* --------------------------------------------------------------- slider */
/*
 * A horizontal value slider: a sunken track filled up to a white thumb, for a
 * volume or brightness level. Stateless like the progress bar: the app keeps
 * `value` (0..max) and a "grabbed" flag. On a press inside slider_hit(), set the
 * flag; while it is set, feed each mouse x through slider_value() to get the new
 * value. The box is [x,y,w,GUI_SLIDER_H]; the thumb stays inside it.
 */
#define GUI_SLIDER_H 14 /* thumb height, and the box height */

void slider_draw(int x, int y, int w, int value, int max);
/* 1 if (mx,my) is on the box or within a few pixels of it (a forgiving grab). */
int slider_hit(int x, int y, int w, int mx, int my);
/* The value 0..max for a pointer at mx, clamped. */
int slider_value(int x, int w, int mx, int max);

/* --------------------------------------------------------------- scrollbar */
/*
 * A vertical scrollbar: an up arrow, a track groove with a draggable thumb
 * sized to the visible fraction of the content, and a down arrow — the way a
 * scrolling document pane (the browser, a long listing) wants one. Stateless
 * like the button/progress bar: the app owns the scroll position and passes the
 * current counts each frame; the widget draws the chrome and maps a click back
 * to either an arrow step or a new thumb position.
 *
 * Units are caller-defined — whatever `scroll` counts in (lines, rows, pixels)
 * — as long as the three agree:
 *  - `total`    total content extent
 *  - `visible`  how much of it fits in the pane at once
 *  - `scroll`   current top position, 0..(total - visible)
 * When total <= visible the bar draws an empty (inert) track and scrollbar_pos
 * returns 0 (nothing to scroll). The arrows are drawn only when [x,y,w,h] is
 * tall enough to hold them plus a grabbable thumb; in a very short bar they
 * collapse to a plain track, and scrollbar_hit then reports the whole bar as
 * the track.
 */
#define GUI_SCROLL_W 10 /* a comfortable vertical scrollbar width */

/* scrollbar_hit() outcomes: which part of the bar a click landed on. The app
 * applies its own step for an arrow (one line/row), and uses scrollbar_pos for
 * a track grab. */
enum { SCROLLBAR_NONE = 0, SCROLLBAR_UP, SCROLLBAR_DOWN, SCROLLBAR_TRACK };

/* Look of a scrollbar: track and thumb colours (0xRRGGBB, -1 = the default), thumb corner radius, and whether the
 * arrow buttons are shown. The plain functions below use SCROLLBAR_DEFAULT; the _ex forms take a style and must be
 * given the same one for drawing and hit-testing. */
struct scrollbar_style {
  int track, thumb;
  int radius;
  int arrows;
};
extern const struct scrollbar_style SCROLLBAR_DEFAULT;
void scrollbar_draw_ex(int x, int y, int w, int h, int scroll, int total, int visible,
                       const struct scrollbar_style *st);
int scrollbar_hit_ex(int x, int y, int w, int h, int mx, int my, const struct scrollbar_style *st);
int scrollbar_pos_ex(int x, int y, int w, int h, int mx, int my, int scroll, int total, int visible,
                     const struct scrollbar_style *st);

/* Draw the scrollbar filling [x,y,w,h]: arrows top/bottom, then the track and a
 * thumb reflecting scroll/total/visible (no thumb when everything fits). */
void scrollbar_draw(int x, int y, int w, int h, int scroll, int total, int visible);
/* Hit-test a click at (mx,my) against the same geometry: returns a SCROLLBAR_*
 * region code (SCROLLBAR_NONE when the point is off the bar). */
int scrollbar_hit(int x, int y, int w, int h, int mx, int my);
/* Map a track click/drag at (mx,my) to a new scroll position, centring the
 * thumb on the cursor, clamped to 0..(total - visible). Returns the unchanged
 * `scroll` when (mx,my) is outside the track region, so it can be called
 * unconditionally. */
int scrollbar_pos(int x, int y, int w, int h, int mx, int my, int scroll, int total, int visible);

/* --------------------------------------------------------------- cursor */
/* Set window `win`'s client-area cursor to a WIN_CURSOR_* shape (owos.h): arrow, hand, text, busy, the four
 * resize arrows, move, not-allowed, crosshair, help, grab, grabbing, progress. The shape is only sent to
 * the system when it differs from the last one set, so calling it on every mouse move is cheap. */
void gfx_cursor(int win, int shape);

/* --------------------------------------------------------------- scroll view */
/*
 * A vertically scrolling container: content taller than the viewport is clipped to it and moved with the
 * mouse wheel, the keyboard, or an attached scrollbar. The widget owns the scroll position; the app owns
 * the content and draws it each frame between scrollview_begin() and scrollview_end():
 *
 *   int top = scrollview_begin(&sv, x, y, w, h, content_h);
 *   draw content at (scrollview_content_x(&sv) + ..., top + row_y), width scrollview_content_w(&sv)
 *   scrollview_end(&sv);
 *
 * Content coordinates start at 0 at the top; a click at screen my is at content y = my - sv.y + sv.scroll.
 * The scrollbar appears only when the content overflows, and the content width shrinks to make room.
 * Route events: WEV_SCROLL -> scrollview_wheel, WEV_KEY -> scrollview_key, WEV_MOUSE -> scrollview_press
 * (returns 1 when the bar consumed the click), WEV_DRAG -> scrollview_drag. Each returns 1 when the
 * view changed or consumed the event, so the app repaints.
 */
#define SCROLLVIEW_STEP 24 /* default px per wheel notch / arrow / key */

struct scrollview {
  int x, y, w, h; /* viewport from the last scrollview_begin, scrollbar included */
  int content_h;  /* content height in px */
  int scroll;     /* px scrolled from the top, 0..content_h - h */
  int step;       /* px per wheel notch / arrow / key */
  int dragging;   /* a scrollbar thumb drag is in progress */
  int saved_clip[4];
};

void scrollview_init(struct scrollview *sv, int step);
/* Place the viewport, clamp the scroll to the new content height, and clip drawing to the content area.
 * Returns the screen y of the content's top edge (y - scroll). */
int scrollview_begin(struct scrollview *sv, int x, int y, int w, int h, int content_h);
/* Restore the previous clip and draw the scrollbar when the content overflows. */
void scrollview_end(struct scrollview *sv);
int scrollview_content_x(const struct scrollview *sv); /* left edge of the content area */
int scrollview_content_w(const struct scrollview *sv); /* content width, less the scrollbar when shown */
int scrollview_wheel(struct scrollview *sv, int dz);   /* WEV_SCROLL delta: + up / - down */
/* Up/Down step, Home/End jump; 1 if handled. */
int scrollview_key(struct scrollview *sv, int key);
int scrollview_press(struct scrollview *sv, int mx, int my);
int scrollview_drag(struct scrollview *sv, int mx, int my);
/* Scroll the least amount that brings [y, y+h) (content coordinates) into view; 1 if it moved. */
int scrollview_reveal(struct scrollview *sv, int y, int h);

/* --------------------------------------------------------------- sidebar */
/*
 * A resizable left navigation pane — the "Places / Devices" rail a file manager
 * (or any master/detail window) puts down the left edge. The widget owns the
 * pane's *width as a live property*: the app reads sb.w back to lay its content
 * out, sets it directly with sidebar_set_width() (so the width is
 * customizable), and — when `resizable` — the user drags the divider to change
 * it, clamped to [min_w, max_w]. The widget paints the panel background plus a
 * 1px divider and a grip dimple at the right edge; the app draws its own rows
 * into [x, x+w).
 *
 * Bind once with sidebar_init(). Each frame: sidebar_draw() paints the panel +
 * grip, then the app lays out its rows within the pane. Route mouse events:
 *  - WEV_MOUSE  -> sidebar_press(): returns 1 if the press grabbed the divider
 *                  grip (begin a resize; the app must NOT also treat it as a
 * row click). A press that misses the grip clears any drag state.
 *  - WEV_DRAG   -> sidebar_drag(): while a resize is live, sets the width from
 *                  the pointer x (clamped); returns 1 if the width changed.
 * The drag ends on its own when the button is released (no release event — the
 * stop of WEV_DRAG ends it, and the next WEV_MOUSE re-evaluates the grip).
 */
#define SIDEBAR_GRIP 6 /* width of the divider's draggable hit zone (px) */

struct sidebar {
  int w;         /* current width — the live, app-readable property         */
  int min_w;     /* lower clamp for a drag / set_width                       */
  int max_w;     /* upper clamp for a drag / set_width                       */
  int resizable; /* 0 = a fixed pane: no grip, no drag (still has a divider) */
  int dragging;  /* internal: a resize drag is in progress                   */
};

/* Initialise to width `w`, clamped into [min_w, max_w]; resizable by default.
 */
void sidebar_init(struct sidebar *sb, int w, int min_w, int max_w);
/* Set the width property directly (clamped to [min_w, max_w]). */
void sidebar_set_width(struct sidebar *sb, int w);
/* Paint the panel [x, x+w) x [y, y+h) in `bg`, the 1px divider at its right
 * edge in `divider`, and (when resizable) a centred grip dimple on the divider.
 * Pass the *effective* width the app is rendering (it may cap sb->w against the
 * window); the divider/grip land at x+w. */
void sidebar_draw(struct sidebar *sb, int x, int y, int w, int h, unsigned bg, unsigned divider);
/* Hit-test a left-press at (mx,my) against the divider grip drawn at x+w over
 * [y, y+h). On a hit, begin a resize and return 1; otherwise clear any stale
 * drag and return 0. A non-resizable pane always returns 0. */
int sidebar_press(struct sidebar *sb, int x, int y, int w, int h, int mx, int my);
/* Feed a WEV_DRAG x while a resize is live: set the width to (mx - x), clamped
 * to [min_w, max_w]. Returns 1 if the width changed (repaint), else 0. */
int sidebar_drag(struct sidebar *sb, int x, int mx);

/* --- separator ----------------------------------------------------------- *
 * A thin themed divider line. Horizontal spans [x, x+len) at row y; vertical
 * spans [y, y+len) at column x (pass vertical = 1). Stateless. */
void gfx_separator(int x, int y, int len, int vertical);

/* --- select (dropdown) --------------------------------------------------- *
 * A combobox: a closed box showing the current option + a chevron, and an
 * option list that drops below when open. The app owns the `options` array (one
 * NUL-terminated label per entry) for the widget's lifetime. Draw the select
 * LAST so its open list overlays the rest of the UI (like a menu). */
#define GUI_SELECT_H 24 /* a comfortable closed-box height */
struct select_box {
  const char *const *options;
  int count;
  int selected; /* index of the chosen option (0..count-1)        */
  int open;     /* 1 = list expanded                              */
  int hover;    /* hovered row while open, or -1                  */
};
void select_init(struct select_box *s, const char *const *options, int count, int selected);
void select_draw(struct select_box *s, int x, int y, int w, int h);
/* Click: returns 1 if the selection changed (s->selected updated, list closed),
 * -1 if consumed (opened/closed), 0 if the click missed the widget entirely. */
int select_click(struct select_box *s, int x, int y, int w, int h, int mx, int my);
/* Update the hovered row from pointer motion while open; returns 1 if it
 * changed. */
int select_motion(struct select_box *s, int x, int y, int w, int h, int mx, int my);
/* Keyboard control for a focused select: Up/Down (move, or change when closed),
 * Home/End (ends), Enter/Space (open / confirm), Esc (close). Same returns as
 * select_click: 1 = selection changed, -1 = consumed, 0 = key not handled. */
int select_key(struct select_box *s, char key);

/* --- text selection helper ------------------------------------------------ *
 * A reusable selection model in abstract (line, col) coordinates. The app maps
 * pixels <-> (line,col), draws the highlight, and does the clipboard copy; this
 * owns the anchor/extent, drag state, ordering and hit-testing. Both a wrapped
 * text view (line = display row, col = byte offset in that row) and a fixed
 * char grid (line = row, col = cell) fit the model. Coordinates compare
 * lexically. */
struct gfx_sel {
  int active, has;
  int al, ac, el, ec;
};
void gfx_sel_clear(struct gfx_sel *s);
void gfx_sel_start(struct gfx_sel *s, int line, int col);  /* mousedown: set anchor */
void gfx_sel_extend(struct gfx_sel *s, int line, int col); /* drag: move the extent */
void gfx_sel_end(struct gfx_sel *s);                       /* mouseup: stop dragging */
int gfx_sel_active(const struct gfx_sel *s);               /* currently dragging     */
int gfx_sel_has(const struct gfx_sel *s);                  /* a non-empty selection  */
/* Ordered range (l0,c0) <= (l1,c1); returns 1 if the selection is non-empty. */
int gfx_sel_range(const struct gfx_sel *s, int *l0, int *c0, int *l1, int *c1);
/* 1 if (line,col) lies within the selection (half-open at the end). */
int gfx_sel_contains(const struct gfx_sel *s, int line, int col);

/* --------------------------------------------------------------- file list + picker */
/*
 * filelist_*: the directory model behind the picker. filelist_read lists `dir` into a malloc'd array
 * (free it), directories first and each group sorted by name without regard to case; files are kept only
 * when they end in one of `exts` (NULL-terminated, e.g. ".mp3"; NULL = all) and dropped when dirs_only.
 * Returns the entry count, or -1 when out of memory.
 */
#define FILELIST_MAX 1024

struct filelist_entry {
  char name[64];
  int is_dir;
  unsigned size;
};

int filelist_read(const char *dir, const char *const *exts, int dirs_only, struct filelist_entry **out);
int filelist_match(const char *name, const char *const *exts);
/* dir + "/" + name into out; an absolute `name` replaces dir */
void filelist_join(char *out, int outsz, const char *dir, const char *name);
/* Cut the last path component in place ("/a/b" -> "/a", "/a" -> "/"); 0 at the root */
int filelist_parent(char *path);

/*
 * filepicker: a modal file/folder chooser, a transient child of window `parent` (or -1). Browses from
 * o->start (a directory; "/" when NULL or not one). FILEPICK_OPEN returns an existing file, FILEPICK_SAVE
 * any file name (o->name pre-fills it) and FILEPICK_FOLDER a directory. With o->exts set, a dropdown
 * toggles between o->filter_label (only those extensions) and all files. Returns 1 with the absolute path
 * in out, 0 if cancelled. With o->multiple (FILEPICK_OPEN only) files get a checkbox column (click it or press
 * Space; Ctrl+A ticks all) and `out` receives the chosen absolute paths, each NUL-terminated and the list ended
 * by an empty string; the return value is how many fit in outsz. It borrows gfx for its own window and restores the
 * caller's surface.
 */
enum { FILEPICK_OPEN = 0, FILEPICK_SAVE, FILEPICK_FOLDER };

struct filepicker_opts {
  const char *title;
  int mode;
  const char *start;
  const char *name;
  int multiple;
  const char *filter_label;
  const char *const *exts;
};

/* The 14x12 folder or file glyph the picker draws beside a name */
void filepicker_icon(int x, int y, int is_dir);
int filepicker(int parent, const struct filepicker_opts *o, char *out, int outsz);

#endif /* OWOS_GFX_H */
