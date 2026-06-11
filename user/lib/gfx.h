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
#define GUI_MENU_W 96    /* dropdown width           */

/* A status line that owns its short message. */
struct status_bar { char msg[64]; };
void status_set(struct status_bar *s, const char *msg);
void status_draw(const struct status_bar *s, int x, int y, int w,
                 unsigned fg, unsigned bg);   /* fills the bar and shows msg */

/* A single drop-down menu (e.g. "File" with New/Open/Save/...). The owning app
 * draws the full-width bar background itself, then calls menu_draw() last so the
 * popup overlays the content. */
struct menu { const char *title; const char *const *items; int n; int open; };
void menu_draw(const struct menu *m, int x, int y);
/* Route a click at (mx,my) given the menu title's top-left (x,y). Toggles/closes
 * m->open as needed; returns the chosen item index, or -1 if none was chosen. */
int  menu_click(struct menu *m, int mx, int my, int x, int y);

#endif /* OWOS_GFX_H */
