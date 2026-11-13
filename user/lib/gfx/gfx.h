/* gfx.h: tiny user-space drawing helpers over a window's pixel buffer.
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

/* Draw a filled rectangle with corners rounded to radius r (clamped to half the shorter side); r <= 0 is a plain
 * gfx_fill_rect. */
void gfx_fill_round(int x, int y, int w, int h, int r, unsigned color);
/* Outline [x,y,w,h] with a stroke t px thick inside it, corners rounded to radius r; r = 0, t = 1 is gfx_rect. */
void gfx_round_rect(int x, int y, int w, int h, int r, int t, unsigned color);

/*
 * Fonts. A struct gfx_font names a face by family, pixel size, weight and slant. Every text call takes one, and
 * NULL means the base font: the font of the base style, gfx_theme(GFX_W_BASE). A zero field also comes from the
 * base font (family NULL, px 0, weight 0), so a style can change just the size or just the weight.
 *
 * `family` is an installed family name ("Liberation Serif"), or "sans-serif" / "monospace" for the faces chosen in
 * Settings; an unknown family falls back to sans-serif. It is not copied and must stay valid while it is in use.
 * When no installed face of the family has the weight or slant asked for, the nearest one is drawn thicker (weight
 * 600 and up) or slanted (italic). Lighter weights need an installed face.
 */
#define GFX_WEIGHT_NORMAL 400
#define GFX_WEIGHT_BOLD 700

struct gfx_font {
  const char *family;
  int px;     /* em size in pixels */
  int weight; /* 100..900 */
  int italic;
};

/* Proportional UTF-8 text with the line's top at y. It blends fg over the existing pixels, so fill the background
 * first. */
void gfx_font_text(int x, int y, const char *s, unsigned fg, const struct gfx_font *f);
/* Like gfx_font_text, but text wider than maxw pixels is cut and ends in ".."; nothing is drawn for maxw <= 0. */
void gfx_font_text_clip(int x, int y, const char *s, unsigned fg, const struct gfx_font *f, int maxw);
int gfx_font_width(const char *s, const struct gfx_font *f); /* pixel width of a UTF-8 string */
int gfx_font_cw(unsigned cp, const struct gfx_font *f);      /* pen advance of one codepoint */
int gfx_font_ascent(const struct gfx_font *f);               /* line top to baseline */
int gfx_font_height(const struct gfx_font *f);               /* line height, 6/5 of the em */
/* *f with every unset field filled from the base font (f NULL: the base font itself) */
void gfx_font_resolve(const struct gfx_font *f, struct gfx_font *out);
struct font;
struct font *gfx_ui_font(void); /* the base font's face (owned by gfx), or NULL if it failed to load */

/* Shorthands for the base font. UIFONT_H is its line height under the built-in theme (gfx_font_height(NULL) in
 * general), UIFONT_W the widest cell. */
#define UIFONT_W 16
#define UIFONT_H 18
void gfx_ui_text(int x, int y, const char *s, unsigned fg);
void gfx_ui_text_clip(int x, int y, const char *s, unsigned fg, int maxw);
int gfx_ui_width(const char *s);
int gfx_ui_cw(unsigned char c);
int gfx_ui_cw_cp(unsigned cp);

/*
 * Styles. Every widget is drawn from a struct gfx_style: its box (margin, border, padding, size), its font, and its
 * colours in each BTN_* state. Each draw and hit-test call takes one as its last argument, and NULL means the
 * widget's theme style, gfx_theme(GFX_W_<kind>). Pass the same style to a widget's draw and hit-test calls so they
 * agree on its geometry.
 *
 * To restyle one widget, copy its theme style and change what you need:
 *
 *   struct gfx_style st = *gfx_theme(GFX_W_BUTTON);
 *   st.radius = 6;
 *   st.font.weight = GFX_WEIGHT_BOLD;
 *   st.color[BTN_NORMAL].bg = 0x2f6bd8;
 *   button_draw(x, y, w, h, "Save", state, &st);
 *
 * To restyle every widget of a kind in the app, gfx_theme_set() it. The base style (GFX_W_BASE) holds the font that
 * plain text and every widget font inherits from, and the focus ring colour for NULL styles.
 *
 * The box model follows CSS with box-sizing: border-box. A widget drawn at (x, y) with a size w x h fills that
 * margin box: its border box starts margin[GFX_LEFT] and margin[GFX_TOP] in from (x, y) and is `width` x `height`
 * when those are set, else what is left of w x h after the margins. Widgets that take no size from the caller (a
 * checkbox, a status bar's height) size themselves from their font, padding and parts unless `width` or `height`
 * is set. The border is drawn inside the border box and the content sits inside the border and padding. Hit tests
 * cover the border box, never the margin.
 */
enum { GFX_TOP, GFX_RIGHT, GFX_BOTTOM, GFX_LEFT };         /* margin[] and padding[] order, as in CSS */
enum { GFX_ALIGN_START, GFX_ALIGN_CENTER, GFX_ALIGN_END }; /* a label's place in the content box */
#define GFX_STATES 4                                       /* BTN_NORMAL .. BTN_DISABLED */

/* gfx_style.flags */
#define GFX_STYLE_NO_ARROWS 0x01 /* a scrollbar without its arrow buttons */
#define GFX_STYLE_NO_FOCUS 0x02  /* never draw the focus ring (the app shows focus itself) */

struct gfx_colors {
  unsigned bg, fg, border;
  /* The widget's secondary mark: a button's lit top edge (equal to bg for none), a menu item's shortcut hint, a
   * select's chevron, a tab's close box, a scrollbar's arrow buttons, a sidebar's grip, a separator's light line */
  unsigned detail;
};

struct gfx_style {
  int margin[4];      /* GFX_TOP, GFX_RIGHT, GFX_BOTTOM, GFX_LEFT; outside the border */
  int padding[4];     /* inside the border */
  int width, height;  /* border box; 0 = the caller's size, or the widget's natural size */
  int min_w, max_w;   /* clamp a width the widget computes itself (a tab); 0 = no bound */
  int part_w, part_h; /* the inner part: a check box, radio or switch, a slider thumb, a tab icon; 0 = default */
  int row_h;          /* a menu or dropdown row, or a scrollbar's shortest thumb; 0 = the line height */
  int track;          /* a slider track's thickness; 0 = default */
  int border;         /* border width */
  int radius;         /* corner radius */
  int gap;            /* between a part and its label, or between menu bar titles */
  int align;          /* GFX_ALIGN_* of the label */
  unsigned flags;     /* GFX_STYLE_* */
  struct gfx_font font;
  struct gfx_colors color[GFX_STATES];  /* the widget's body, by BTN_* state */
  struct gfx_colors accent[GFX_STATES]; /* what is on, filled or chosen: a tick, a bar's fill, a selection */
  unsigned focus;                       /* focus ring */
};

/* Widget kinds, the index into the theme */
enum {
  GFX_W_BASE = 0, /* plain text, and the font everything inherits */
  GFX_W_BUTTON,
  GFX_W_CHECKBOX,
  GFX_W_RADIO,
  GFX_W_SWITCH,
  GFX_W_TEXTFIELD,
  GFX_W_SELECT,
  GFX_W_PROGRESS,
  GFX_W_SLIDER,
  GFX_W_SCROLLBAR,
  GFX_W_MENUBAR, /* the title row of a menu or menu bar */
  GFX_W_MENU,    /* a dropdown, submenu or popup menu */
  GFX_W_TABBAR,
  GFX_W_STATUS,
  GFX_W_SIDEBAR,
  GFX_W_SEPARATOR,
  GFX_W_COUNT
};

/* The app's style for a kind (GFX_W_BASE for an unknown kind). The pointer stays valid; gfx_theme_set changes what
 * it points to. */
const struct gfx_style *gfx_theme(int kind);
/* Replace a kind's style with a copy of *st, or restore the built-in one when st is NULL. */
void gfx_theme_set(int kind, const struct gfx_style *st);
/* The built-in style for a kind, whatever the theme now holds */
const struct gfx_style *gfx_theme_default(int kind);
/* st itself, or the theme style of `kind` when st is NULL */
const struct gfx_style *gfx_style_of(int kind, const struct gfx_style *st);
/* The style's font with every unset field filled from the base font */
void gfx_style_font(const struct gfx_style *st, struct gfx_font *out);
/* The border-box height of a one-line widget: `height` if set, else its line height plus padding and border */
int gfx_style_text_h(const struct gfx_style *st);
/* Set all four sides of a margin[] or padding[] array */
void gfx_sides(int *sides, int top, int right, int bottom, int left);

struct gfx_box {
  int x, y, w, h;
};
/* The border box of a widget whose margin box is (x, y, w, h), per the rules above */
void gfx_style_box(const struct gfx_style *st, int x, int y, int w, int h, struct gfx_box *out);
/* The content box inside a border box: less the border and the padding */
void gfx_style_content(const struct gfx_style *st, const struct gfx_box *border, struct gfx_box *out);
/* The margin-box size of a widget whose border box is w x h (for laying out natural-size widgets) */
int gfx_style_outer_w(const struct gfx_style *st, int w);
int gfx_style_outer_h(const struct gfx_style *st, int h);
/* Paint a border box: its border in colors->border, its inside in colors->bg, both rounded by st->radius, and a
 * colors->detail line along the top of the inside when that differs from bg (a raised look). */
void gfx_style_paint(const struct gfx_style *st, const struct gfx_box *b, const struct gfx_colors *colors);
/* Draw a one-line label in the content box c, placed by st->align and centred vertically, in the style's font. A
 * label wider than c is cut with "..". */
void gfx_style_label(const struct gfx_style *st, const struct gfx_box *c, const char *s, unsigned fg);
/* Clamp a BTN_* state (GFX_FOCUSED cleared) into 0..GFX_STATES-1 */
int gfx_state_index(int state);

/*
 * Keyboard focus. OR GFX_FOCUSED into the BTN_* state a widget is drawn with (or set the widget's `focused`
 * field) and it shows a focus ring. Rings sit up to 3px outside the widget, so leave that margin around it.
 *
 * struct gfx_focus is a window's Tab order over its focusable controls, numbered 0..n-1 in visual order: Tab
 * moves forward and Shift+Tab back, both wrapping and skipping the entries `disabled` marks nonzero. Ctrl+Tab
 * and Alt+Tab are left to the app and the window manager.
 */
#define GFX_FOCUSED 0x100

/* The 2px ring around [x,y,w,h], 1px clear of it, in st->focus (NULL: the base style's), following st->radius. A
 * style with GFX_STYLE_NO_FOCUS draws nothing. */
void gfx_focus_ring(int x, int y, int w, int h, const struct gfx_style *st);

struct gfx_focus {
  int cur;                       /* focused entry, or -1 when every entry is disabled */
  int n;                         /* entry count */
  const unsigned char *disabled; /* optional, per entry: nonzero = skipped */
};
/* Focus entry `cur`, or the next enabled one after it. `disabled` may be NULL and stays owned by the app. */
void gfx_focus_init(struct gfx_focus *f, int n, int cur, const unsigned char *disabled);
/* Move to the next enabled entry in direction dir (+1 / -1), wrapping. Returns 1 if focus changed. */
int gfx_focus_move(struct gfx_focus *f, int dir);
/* Route a WEV_KEY (key, KMOD_* mods): Tab / Shift+Tab move focus. Returns 1 if focus changed. */
int gfx_focus_key(struct gfx_focus *f, int key, int mods);
int gfx_focus_is(const struct gfx_focus *f, int i);

/*
 * Accessibility. Widgets describe themselves as they draw: bind a tree with gfx_a11y_bind(), call
 * gfx_a11y_begin() at the start of each repaint, and every widget drawn after that appends a node with its role,
 * label, state and value. Nothing is recorded while no tree is bound. Add nodes for app-drawn content with
 * gfx_a11y_add(), and name a widget that draws no label (a slider, a text field) through gfx_a11y_last() right
 * after drawing it. The dialogs and the file picker bind their own tree and restore the caller's on return.
 */
enum {
  GFX_ROLE_NONE = 0,
  GFX_ROLE_DIALOG,
  GFX_ROLE_LABEL,
  GFX_ROLE_BUTTON,
  GFX_ROLE_CHECKBOX,
  GFX_ROLE_RADIO,
  GFX_ROLE_SWITCH,
  GFX_ROLE_SLIDER,
  GFX_ROLE_PROGRESS,
  GFX_ROLE_SCROLLBAR,
  GFX_ROLE_TEXTFIELD,
  GFX_ROLE_SELECT,
  GFX_ROLE_OPTION,
  GFX_ROLE_TAB,
  GFX_ROLE_MENU,
  GFX_ROLE_MENUITEM,
  GFX_ROLE_LIST,
  GFX_ROLE_LISTITEM,
  GFX_ROLE_COUNT
};

/* gfx_a11y.state bits */
#define GFX_A11Y_FOCUSED 0x001 /* has keyboard focus (a list's focused row, not the list) */
#define GFX_A11Y_DISABLED 0x002
#define GFX_A11Y_CHECKED 0x004 /* box ticked, radio chosen, switch on, menu item checked */
#define GFX_A11Y_PRESSED 0x008
#define GFX_A11Y_HOVER 0x010
#define GFX_A11Y_SELECTED 0x020 /* the active tab, the chosen option or list row */
#define GFX_A11Y_EXPANDED 0x040 /* its menu or list is open */
#define GFX_A11Y_HASPOPUP 0x080 /* opens a menu or list */
#define GFX_A11Y_DEFAULT 0x100  /* a dialog's primary button */

#define GFX_A11Y_TEXT 64
#define GFX_A11Y_DESC 160

struct gfx_a11y {
  int role;                 /* GFX_ROLE_* */
  unsigned state;           /* GFX_A11Y_* */
  int x, y, w, h;           /* bounds in client coordinates */
  int value, min, max;      /* slider, progress, scroll position, list row; unused when max <= min */
  char name[GFX_A11Y_TEXT]; /* label */
  char text[GFX_A11Y_TEXT]; /* contents: a field's text, a select's choice, a dialog's kind */
  char desc[GFX_A11Y_DESC]; /* longer help, or a dialog's message */
  char shortcut[16];        /* accelerator label, e.g. "Ctrl+S" */
};

struct gfx_a11y_tree {
  struct gfx_a11y *nodes; /* app-owned storage for cap nodes */
  int cap, n;
};

void gfx_a11y_tree_init(struct gfx_a11y_tree *t, struct gfx_a11y *nodes, int cap);
/* Record into t from now on (NULL stops recording). Returns the previously bound tree. */
struct gfx_a11y_tree *gfx_a11y_bind(struct gfx_a11y_tree *t);
/* Empty the bound tree; call at the start of every repaint. */
void gfx_a11y_begin(void);
/* Append a node. Returns NULL when no tree is bound or it is full; every setter accepts NULL. */
struct gfx_a11y *gfx_a11y_add(int role, const char *name, unsigned state, int x, int y, int w, int h);
/* The node added last since gfx_a11y_begin, or NULL */
struct gfx_a11y *gfx_a11y_last(void);
/* The setters copy the string, cutting it at a UTF-8 boundary to fit. */
void gfx_a11y_set_name(struct gfx_a11y *a, const char *s);
void gfx_a11y_set_text(struct gfx_a11y *a, const char *s);
void gfx_a11y_set_desc(struct gfx_a11y *a, const char *s);
void gfx_a11y_set_shortcut(struct gfx_a11y *a, const char *s);
void gfx_a11y_set_value(struct gfx_a11y *a, int value, int min, int max);
/* A BTN_* state, with GFX_FOCUSED, as GFX_A11Y_* bits */
unsigned gfx_a11y_state(int btn_state);
/* The first focused node in t, or NULL */
const struct gfx_a11y *gfx_a11y_focused(const struct gfx_a11y_tree *t);
/* "button", "check box", ...; "" for GFX_ROLE_NONE and GFX_ROLE_LABEL */
const char *gfx_a11y_role_name(int role);
/* The node as one spoken line, e.g. "Volume, slider, 40 of 100". Returns the length written to buf. */
int gfx_a11y_describe(const struct gfx_a11y *a, char *buf, int sz);

/* --------------------------------------------------------------- GUI widgets
 */
/*
 * Reusable pieces every windowed app tends to want, so they aren't reinvented per app. The GUI_* sizes below are
 * the built-in theme's; a style can change any of them, so lay out from the widget's own size helpers when the
 * style is not the built-in one.
 */
#define GUI_BAR_H 18   /* menu / status bar height */
#define GUI_ROW_H 16   /* dropdown item height     */
#define GUI_MENU_W 140 /* dropdown width (wide enough for a "Ctrl+S" hint)  */

/* A status line that owns its short message. */
struct status_bar {
  char msg[64];
};
void status_set(struct status_bar *s, const char *msg);
/* Fill the bar across [x, x+w) from y in color[BTN_NORMAL].bg and show msg in its fg. */
void status_draw(const struct status_bar *s, int x, int y, int w, const struct gfx_style *st);
int status_height(const struct gfx_style *st); /* the bar's margin-box height */

/* Per-item state bits for the optional `flags` array (mutable; apps toggle
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
 *  - `hints`/`keys`: a keyboard accelerator: `hints[i]` is the right-aligned
 *    label ("Ctrl+S"), `keys[i]` the KEY_* byte fed to menu_shortcut().
 *  - `flags`: MENU_DISABLED|MENU_CHECKED state bits (mutable; reserves a check
 *    gutter for the whole menu when present).
 *  - `subs`: a child submenu per item (subs[i] != NULL makes item i a parent
 * that opens subs[i] to its right instead of firing an action).
 *
 * Two styles shape a menu: `bar` (GFX_W_MENUBAR) its title, and `pop` (GFX_W_MENU) its dropdown and submenus. A
 * closed title is not filled, an open one is in color[BTN_PRESSED]. In the dropdown, color[BTN_NORMAL] is an item,
 * color[BTN_DISABLED] a disabled one (detail: the shortcut hint), accent[].fg the checkmark, and a pop->width of 0
 * fits the widest item. Every geometry call takes the same pair. */
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
void menu_draw(const struct menu *m, int x, int y, const struct gfx_style *bar, const struct gfx_style *pop);
/* Route a click at (mx,my) given the menu title's top-left (x,y).
 * Toggles/closes m->open (and any submenus) as needed; returns the chosen item
 * index of `m`, or -1 if nothing in `m` fired (a disabled item, an opened
 * submenu, a chosen *submenu* item, or a miss). Use menu_click_ex to dispatch
 * submenu items. */
int menu_click(struct menu *m, int mx, int my, int x, int y, const struct gfx_style *bar, const struct gfx_style *pop);
/* Like menu_click but reports the leaf menu that owns the fired item via *leaf
 * (which may be a submenu, not `m`). Returns that menu's item index, or -1. */
int menu_click_ex(struct menu *m, int mx, int my, int x, int y, struct menu **leaf, const struct gfx_style *bar,
                  const struct gfx_style *pop);
/* Map a pressed key to a menu item via the `keys` tables (searches submenus
 * too, skips disabled items): returns the item index, or -1 if none. */
int menu_shortcut(const struct menu *m, int key);
/* A free-standing popup list (right-click menu) whose top-left is (x,y); m->title
 * is unused and submenus are not supported. menu_popup_click returns the enabled
 * item under (mx,my), or -1 for a miss or a disabled item. The app owns when it is
 * open and closes it after any click. */
void menu_popup_draw(const struct menu *m, int x, int y, const struct gfx_style *pop);
int menu_popup_click(const struct menu *m, int mx, int my, int x, int y, const struct gfx_style *pop);
/* The popup's margin-box size, to keep it on screen */
int menu_popup_w(const struct menu *m, const struct gfx_style *pop);
int menu_popup_h(const struct menu *m, const struct gfx_style *pop);
/* A title's margin-box width and the title row's height */
int menu_title_w(const struct menu *m, const struct gfx_style *bar);
int menu_bar_h(const struct gfx_style *bar);

/* A row of dropdown menus sharing one bar (File / Edit / View ...). Holds an
 * array of `struct menu` pointers laid out left-to-right from (x,y), bar->gap
 * apart; the helpers keep at most one dropdown open at a time, so the owning app
 * gets a real menu bar without re-implementing the open/close bookkeeping per app. */
struct menubar {
  struct menu *const *menus;
  int n;
  int x, y; /* bar origin: the title row's top-left */
};
/* Draw every title on the bar, then the open menu's dropdown last (on top). The
 * app still fills the full-width bar background itself, then calls this last.
 */
void menubar_draw(const struct menubar *mb, const struct gfx_style *bar, const struct gfx_style *pop);
/* Route a click. Returns 1 if it hit the bar or an open dropdown (the app
 * should repaint and not process the click further), 0 if it was elsewhere.
 * When an item is chosen, *leaf and *item give the leaf menu (a bar menu OR one
 * of its submenus; dispatch by pointer) and its item index; otherwise *item is
 * -1 (only open/close state changed). Clicking off an open menu closes it. */
int menubar_click(struct menubar *mb, int mx, int my, struct menu **leaf, int *item, const struct gfx_style *bar,
                  const struct gfx_style *pop);
/* Map a pressed key to any menu's accelerator (submenus included): returns the
 * item index and sets *leaf to the owning menu, or -1 (with *leaf = NULL). */
int menubar_shortcut(const struct menubar *mb, int key, struct menu **leaf);
/* Close every dropdown on the bar and its submenus (after an accelerator/Esc).
 */
void menubar_close(struct menubar *mb);

/* --------------------------------------------------------------- text field */
/*
 * A single-line editable text field (an address bar, a rename box, any "type a
 * string and press Enter" input), so apps don't re-roll caret/scroll/editing by
 * hand. The app owns the character buffer (NUL-terminated); the widget tracks
 * the caret and horizontal scroll, drawing the style's font in a field that
 * scrolls to keep the caret in view.
 *
 * Bind once with textfield_init(&tf, buf, cap), set initial text with
 * textfield_set, then each frame draw it and feed WEV_KEY chars to
 * textfield_key. The current text is just tf.buf (NUL-terminated, tf.len
 * chars). Style: color[BTN_NORMAL] is the field, accent[BTN_NORMAL] the focused
 * border (border), the caret and selection (bg) and selected text (fg).
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
void textfield_draw(struct textfield *tf, int x, int y, int w, int h, int focused, const struct gfx_style *st);
/* Place the caret from a click and report focus: returns 1 if (mx,my) fell
 * inside [x,y,w,h] (the app should treat the field as focused), 0 otherwise.
 * Uses the scroll left by the last textfield_draw, so call draw first each
 * frame. */
int textfield_click(struct textfield *tf, int mx, int my, int x, int y, int w, int h, const struct gfx_style *st);
/* Extend the selection while the button is held after a textfield_click; the
 * caret follows mx and the view scrolls past the field's edges. */
void textfield_drag(struct textfield *tf, int mx, int x, int w, const struct gfx_style *st);
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
 * evenly (clamped to [min_w, max_w]), labels are truncated to fit, and every tab
 * has a close box. When the tabs don't fit at min_w, the strip shows a window of
 * them between scroll arrows.
 *
 * Style: color[BTN_NORMAL] is an inactive tab (border: the strip behind the tabs,
 * detail: the close box), accent[BTN_NORMAL] the active one (border: its top
 * stripe and focus outline), color[BTN_HOVER].fg the "+" and scroll arrows and
 * color[BTN_DISABLED].fg a spent arrow. part_w is the icon size, padding[GFX_LEFT]
 * its inset and gap the space after it.
 */
#define GUI_TAB_H 24 /* tab strip height */

/* Per-strip state the app keeps between frames (zero-initialize it) */
struct tabbar {
  int first;       /* first visible tab while the strip overflows */
  int last_active; /* scroll to reveal `active` only when it changes */
  int focused;     /* the strip has keyboard focus: outline the active tab */
};

/* tabbar_hit() outcomes. SELECT/CLOSE fill *index with the tab; NEW is the "+";
 * SCROLL means an arrow moved the strip and the app should repaint. */
enum { TABBAR_NONE = 0, TABBAR_SELECT, TABBAR_CLOSE, TABBAR_NEW, TABBAR_SCROLL };

/* Draw the strip filling [x, x+w) at tabbar_height() from y. The app fills no
 * background itself; the widget paints the whole strip. icons[i] is an
 * optional 16x16 ARGB favicon for tab i (NULL -> a default globe); pass a NULL
 * icons array to give every tab the globe. */
void tabbar_draw(struct tabbar *tb, int x, int y, int w, const char *const *labels, const unsigned *const *icons, int n,
                 int active, const struct gfx_style *st);
/* Hit-test a click at (mx,my) against the geometry of the last draw. Returns a
 * TABBAR_* code; for SELECT/CLOSE sets *index to the tab index. */
int tabbar_hit(struct tabbar *tb, int x, int y, int w, int n, int mx, int my, int *index, const struct gfx_style *st);
/* Scroll an overflowing strip by `delta` tabs (positive = right) */
void tabbar_scroll(struct tabbar *tb, int w, int n, int delta, const struct gfx_style *st);
/* Keys for a focused strip: Left/Right (wrapping) and Home/End change *active. Returns 1 if it changed. */
int tabbar_key(int key, int n, int *active);
int tabbar_height(const struct gfx_style *st); /* the strip's margin-box height */

/* --------------------------------------------------------------- button */
/*
 * A clickable push button with a label, an "OK"/"Cancel"/"Connect" the way a
 * dialog or toolbar wants one. Stateless like the tab strip: the app owns no
 * widget struct, it just draws the button each frame with the visual state it's
 * tracking and hit-tests clicks against the same rectangle.
 *
 * `state` selects the look, color[state]. The app decides it: BTN_HOVER while the
 * pointer is over the button (track the last mouse-move), BTN_PRESSED between
 * mouse-down and mouse-up on it, BTN_DISABLED for an inert/dimmed button
 * (button_hit still returns 0 for it so it can't fire), else BTN_NORMAL.
 */
#define GUI_BTN_H 26 /* a comfortable button height */

enum { BTN_NORMAL = 0, BTN_HOVER, BTN_PRESSED, BTN_DISABLED };

/* Draw the button in the margin box [x,y,w,h] with `label` placed by st->align, in the given state. */
void button_draw(int x, int y, int w, int h, const char *label, int state, const struct gfx_style *st);
/* Returns 1 if (mx,my) falls inside the border box of [x,y,w,h], 0 otherwise
 * (geometry only: the app gates on its own disabled state before treating a
 * hit as a press). */
int button_hit(int x, int y, int w, int h, int mx, int my, const struct gfx_style *st);
/* 1 if a WEV_KEY presses the focused button: Space or Enter */
int button_key(int key);

/* --------------------------------------------------------------- checkbox, radio, switch */
/*
 * Two-state controls with an optional trailing label (NULL or "" for none).
 * Stateless like the button: the app keeps `checked` / `on`, draws each frame
 * with the BTN_* visual state it tracks (hover, pressed, disabled), and
 * flips its value when control_hit() reports a click on an enabled control.
 * A radio group is the app's business: set the clicked one, clear the rest.
 * `focused` (or GFX_FOCUSED in `state`) draws a focus ring; feed keys to control_key().
 *
 * Style: part_w x part_h is the box, radio or switch and gap the space before the
 * label. color[state] is the box when off (bg), its edge (border) and the label
 * (fg); accent[state] is the box when on (bg, and border for its edge) and the
 * tick, dot or knob (fg). The control's margin box is control_width() x
 * control_height() from (x, y).
 */
#define GUI_CTRL_H 18     /* row height, label included */
#define GUI_CHECK_SIZE 14 /* checkbox and radio box */
#define GUI_SWITCH_W 28
#define GUI_SWITCH_H 16

enum { GFX_CTRL_CHECKBOX = 0, GFX_CTRL_RADIO, GFX_CTRL_SWITCH };

void checkbox_draw(int x, int y, const char *label, int checked, int state, int focused, const struct gfx_style *st);
void radio_draw(int x, int y, const char *label, int checked, int state, int focused, const struct gfx_style *st);
void switch_draw(int x, int y, const char *label, int on, int state, int focused, const struct gfx_style *st);
/* Margin-box width of a GFX_CTRL_* kind with its label, and its height */
int control_width(int kind, const char *label, const struct gfx_style *st);
int control_height(int kind, const struct gfx_style *st);
/* 1 if (mx,my) is on the box or its label (geometry only, like button_hit). */
int control_hit(int kind, int x, int y, const char *label, int mx, int my, const struct gfx_style *st);
/* Space on a focused control flips *value (a radio only turns on). Returns 1 if *value changed. */
int control_key(int kind, int key, int *value);

/* --------------------------------------------------------------- progress bar
 */
/*
 * A horizontal progress bar: a trough (color[BTN_NORMAL]) with a fill
 * (accent[BTN_NORMAL], detail its lit top edge) showing `value`/`max` (clamped
 * to 0..max), for a download/copy/load. Stateless: the app passes the current
 * counts each frame. With show_pct != 0 it overlays a centred "NN%" label, in
 * accent fg over the fill and color fg over the trough; a value/max of N/0 reads
 * as 0%.
 */
#define GUI_PROGRESS_H 16 /* a comfortable bar height */

void progress_draw(int x, int y, int w, int h, int value, int max, int show_pct, const struct gfx_style *st);

/* --------------------------------------------------------------- slider */
/*
 * A horizontal value slider: a track filled up to a thumb, for a volume or
 * brightness level. Stateless like the progress bar: the app keeps `value`
 * (0..max) and a "grabbed" flag. On a press inside slider_hit(), set the flag;
 * while it is set, feed each mouse x through slider_value() to get the new
 * value. The box is [x, y, w, slider_height()]; the thumb stays inside it.
 *
 * Style: part_w x part_h is the thumb (accent fg), `track` the track's
 * thickness; color[BTN_NORMAL] is the empty track and accent[BTN_NORMAL].bg its
 * filled part.
 */
#define GUI_SLIDER_H 14 /* thumb height, and the box height */

void slider_draw(int x, int y, int w, int value, int max, int focused, const struct gfx_style *st);
/* 1 if (mx,my) is on the box or within a few pixels of it (a forgiving grab). */
int slider_hit(int x, int y, int w, int mx, int my, const struct gfx_style *st);
/* The value 0..max for a pointer at mx, clamped. */
int slider_value(int x, int w, int mx, int max, const struct gfx_style *st);
/* Keys for a focused slider: Right/Up and Left/Down move by step (<= 0: a tenth of max), Home/End jump to the
 * ends. Returns 1 if *value changed. */
int slider_key(int key, int *value, int max, int step);
int slider_height(const struct gfx_style *st); /* the box's margin-box height */

/* --------------------------------------------------------------- scrollbar */
/*
 * A vertical scrollbar: an up arrow, a track groove with a draggable thumb
 * sized to the visible fraction of the content, and a down arrow, the way a
 * scrolling document pane (the browser, a long listing) wants one. Stateless
 * like the button/progress bar: the app owns the scroll position and passes the
 * current counts each frame; the widget draws the chrome and maps a click back
 * to either an arrow step or a new thumb position.
 *
 * Units are caller-defined (whatever `scroll` counts in: lines, rows, pixels)
 * as long as the three agree:
 *  - `total`    total content extent
 *  - `visible`  how much of it fits in the pane at once
 *  - `scroll`   current top position, 0..(total - visible)
 * When total <= visible the bar draws an empty (inert) track and scrollbar_pos
 * returns 0 (nothing to scroll). The arrows are drawn only when [x,y,w,h] is
 * tall enough to hold them plus a grabbable thumb; in a very short bar they
 * collapse to a plain track, and scrollbar_hit then reports the whole bar as
 * the track.
 *
 * Style: color[BTN_NORMAL] is the track (bg) and arrow buttons (detail: their
 * face, fg: the arrowhead); accent[BTN_NORMAL].bg is the thumb, rounded by
 * radius and inset by padding[GFX_LEFT] / padding[GFX_RIGHT]. row_h is the
 * shortest thumb; GFX_STYLE_NO_ARROWS drops the arrow buttons.
 */
#define GUI_SCROLL_W 10 /* a comfortable vertical scrollbar width */

/* scrollbar_hit() outcomes: which part of the bar a click landed on. The app
 * applies its own step for an arrow (one line/row), and uses scrollbar_pos for
 * a track grab. */
enum { SCROLLBAR_NONE = 0, SCROLLBAR_UP, SCROLLBAR_DOWN, SCROLLBAR_TRACK };

/* Draw the scrollbar filling [x,y,w,h]: arrows top/bottom, then the track and a
 * thumb reflecting scroll/total/visible (no thumb when everything fits). */
void scrollbar_draw(int x, int y, int w, int h, int scroll, int total, int visible, const struct gfx_style *st);
/* Hit-test a click at (mx,my) against the same geometry: returns a SCROLLBAR_*
 * region code (SCROLLBAR_NONE when the point is off the bar). */
int scrollbar_hit(int x, int y, int w, int h, int mx, int my, const struct gfx_style *st);
/* Map a track click/drag at (mx,my) to a new scroll position, centring the
 * thumb on the cursor, clamped to 0..(total - visible). Returns the unchanged
 * `scroll` when (mx,my) is outside the track region, so it can be called
 * unconditionally. */
int scrollbar_pos(int x, int y, int w, int h, int mx, int my, int scroll, int total, int visible,
                  const struct gfx_style *st);

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
  int x, y, w, h;              /* viewport from the last scrollview_begin, scrollbar included */
  int content_h;               /* content height in px */
  int scroll;                  /* px scrolled from the top, 0..content_h - h */
  int step;                    /* px per wheel notch / arrow / key */
  int dragging;                /* a scrollbar thumb drag is in progress */
  const struct gfx_style *bar; /* the scrollbar's style (NULL: the theme's); its width is the bar's */
  int saved_clip[4];
};

/* `bar` styles the scrollbar and must stay valid for the view's life; a bar width of 0 is GUI_SCROLL_W. */
void scrollview_init(struct scrollview *sv, int step, const struct gfx_style *bar);
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
 * A resizable left navigation pane: the "Places / Devices" rail a file manager
 * (or any master/detail window) puts down the left edge. The widget owns the
 * pane's *width as a live property*: the app reads sb.w back to lay its content
 * out, sets it directly with sidebar_set_width() (so the width is
 * customizable), and, when `resizable`, the user drags the divider to change
 * it, clamped to [min_w, max_w]. The widget paints the panel background plus a
 * divider and a grip dimple at the right edge; the app draws its own rows
 * into [x, x+w).
 *
 * Bind once with sidebar_init(). Each frame: sidebar_draw() paints the panel +
 * grip, then the app lays out its rows within the pane. Route mouse events:
 *  - WEV_MOUSE  -> sidebar_press(): returns 1 if the press grabbed the divider
 *                  grip (begin a resize; the app must NOT also treat it as a
 * row click). A press that misses the grip clears any drag state.
 *  - WEV_DRAG   -> sidebar_drag(): while a resize is live, sets the width from
 *                  the pointer x (clamped); returns 1 if the width changed.
 * The drag ends on its own when the button is released (no release event; the
 * stop of WEV_DRAG ends it, and the next WEV_MOUSE re-evaluates the grip).
 */
#define SIDEBAR_GRIP 6 /* width of the divider's draggable hit zone (px) */

struct sidebar {
  int w;         /* current width: the live, app-readable property          */
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
/* Paint the panel [x, x+w) x [y, y+h) in color[BTN_NORMAL].bg, a divider
 * st->border wide (at least 1px) at its right edge in .border, and (when
 * resizable) a centred grip dimple on the divider in .detail. Pass the
 * *effective* width the app is rendering (it may cap sb->w against the window);
 * the divider/grip land at x+w. */
void sidebar_draw(struct sidebar *sb, int x, int y, int w, int h, const struct gfx_style *st);
/* Hit-test a left-press at (mx,my) against the divider grip drawn at x+w over
 * [y, y+h). On a hit, begin a resize and return 1; otherwise clear any stale
 * drag and return 0. A non-resizable pane always returns 0. */
int sidebar_press(struct sidebar *sb, int x, int y, int w, int h, int mx, int my);
/* Feed a WEV_DRAG x while a resize is live: set the width to (mx - x), clamped
 * to [min_w, max_w]. Returns 1 if the width changed (repaint), else 0. */
int sidebar_drag(struct sidebar *sb, int x, int mx);

/* --- separator ----------------------------------------------------------- *
 * A thin themed divider: a color[BTN_NORMAL].border line st->border px thick
 * (at least 1) and a .detail line after it. Horizontal spans [x, x+len) from
 * row y; vertical spans [y, y+len) from column x (pass vertical = 1). Margins
 * offset it. Stateless. */
void gfx_separator(int x, int y, int len, int vertical, const struct gfx_style *st);

/* --- select (dropdown) --------------------------------------------------- *
 * A combobox: a closed box showing the current option + a chevron, and an
 * option list that drops below when open. The app owns the `options` array (one
 * NUL-terminated label per entry) for the widget's lifetime. Draw the select
 * LAST so its open list overlays the rest of the UI (like a menu).
 * Style: color[BTN_NORMAL] is the box and list (detail: the chevron),
 * accent[BTN_NORMAL] the open box's border and the hovered row (bg, fg); row_h
 * is a list row's height. */
#define GUI_SELECT_H 24 /* a comfortable closed-box height */
struct select_box {
  const char *const *options;
  int count;
  int selected; /* index of the chosen option (0..count-1)        */
  int open;     /* 1 = list expanded                              */
  int hover;    /* hovered row while open, or -1                  */
  int focused;  /* has keyboard focus: draws the focus ring       */
};
void select_init(struct select_box *s, const char *const *options, int count, int selected);
void select_draw(struct select_box *s, int x, int y, int w, int h, const struct gfx_style *st);
/* Click: returns 1 if the selection changed (s->selected updated, list closed),
 * -1 if consumed (opened/closed), 0 if the click missed the widget entirely. */
int select_click(struct select_box *s, int x, int y, int w, int h, int mx, int my, const struct gfx_style *st);
/* Update the hovered row from pointer motion while open; returns 1 if it
 * changed. */
int select_motion(struct select_box *s, int x, int y, int w, int h, int mx, int my, const struct gfx_style *st);
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
