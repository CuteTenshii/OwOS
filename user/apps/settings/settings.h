/* settings.h, shared state and page interfaces for the Settings app */
#ifndef SETTINGS_H
#define SETTINGS_H

#include "gfx.h"
#include "owos.h"
#include <stdbool.h>

#define HEADER_H 22 /* sidebar title strip */
#define ROW_H 26    /* sidebar category row height */
#define SIDEBAR_W 150
#define SIDEBAR_MIN 120
#define SIDEBAR_MAX 220

/* Theme (matches the Files explorer) */
#define BG_PANEL 0x12161eu
#define BG_SIDE 0x0e1218u
#define BG_HEADER 0x161d28u
#define BG_DIVIDER 0x05070au
#define BG_SEL 0x24405cu
#define FG_TITLE 0x8aa0b8u
#define FG_TEXT 0xc8d0dcu
#define FG_DIM 0x8aa0b8u
#define FG_WHITE 0xffffffu

enum { CAT_MOUSE = 0, CAT_KEYBOARD, CAT_SOUND, CAT_DISPLAY, CAT_FONTS, CAT_UPDATES, CAT_ABOUT, NCATS };

extern int W, H;     /* client size; changes on resize */
extern int sel;      /* selected category */
extern unsigned seq; /* bump to request a repaint */

/* util.c */
void uitoa(unsigned v, char *out);
int patoi(const char *s);
int read_proc(const char *name, char *buf, int max);
bool field(const char *buf, const char *key, char *out, int max);

/* devices.c: mouse, keyboard, sound, display and font state */
void dev_load(int cat);
void dev_controls(int cat);

/* controls.c: dropdown controls and the level slider of the active category */
#define CONTROLS_Y0 44
void controls_add(const char *label, const char *hint, struct select_box *box, void (*apply)(int idx));
void controls_build(int cat);
void controls_draw(int cx, int cw);
void controls_click(int cx, int cw, int mx, int my);
void controls_move(int cx, int cw, int mx, int my);
bool controls_key(int k);
bool controls_drag(int mx, int cx, int cw);

/* updates.c */
void updates_draw(int cx, int cw);
void updates_click(int cx, int mx, int my);
void updates_poll(void);

/* about.c */
void about_load(void);
void about_draw(int cx, int cw);

#endif /* SETTINGS_H */
