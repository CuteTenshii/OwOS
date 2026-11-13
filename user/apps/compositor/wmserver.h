/* wmserver.h: the compositor's window table, served to apps over the "wm" port (wmserver.c, wmproto.h) */
#ifndef OWOS_WMSERVER_H
#define OWOS_WMSERVER_H

#define WM_TITLE_H 22
#define WM_BORDER 2
#define WM_BAR_H 28
#define WM_TITLE_MAX 64
#define WM_MIN_W 140
#define WM_MIN_H 120
#define WM_LIST_MAX 64

/* The compositor's view of a window (wmserver.c) */
struct winrec {
  int id, x, y, w, h;
  unsigned flags; /* UWIN_F_* */
  unsigned present_seq;
  int dirty_x, dirty_y, dirty_w, dirty_h; /* client damage since last enum; w < 0 = whole frame */
  int owner_pid, z;
  int buf_w, buf_h; /* client buffer size (stride = buf_w) */
  char title[WM_TITLE_MAX];
  int parent; /* transient parent window id, or -1 */
  int cursor; /* client-area cursor shape, WIN_CURSOR_* */
};
#define UWIN_F_VISIBLE 0x01
#define UWIN_F_FOCUSED 0x02
#define UWIN_F_MINIMIZED 0x04
#define UWIN_F_MAXIMIZED 0x08
#define UWIN_F_RESIZABLE 0x10
#define UWIN_F_MODAL 0x20      /* transient child that blocks its parent family */
#define UWIN_F_FULLSCREEN 0x40 /* covers the screen, no chrome */
struct winmap {
  unsigned vaddr;
  int w, h, stride;
};
struct winevent {
  unsigned type;
  int a, b;
}; /* type = WEV_*, client-relative */

/* wmserver.c */
void wms_poll(void);  /* accept clients and run their requests */
void wms_flush(void); /* send queued events and resizes */
int win_enum(struct winrec *out, int max);
int win_map(int id, struct winmap *m);
int win_set_geom(int id, int x, int y, int w, int h);
int win_deliver(int id, const struct winevent *ev);
int win_key_space(int id); /* events that still fit the owner's queue; huge for a bad id */

/* The screen new windows are placed on, and the framebuffer that caps a buffer's size (screen.c) */
extern int screen_w, screen_h;
extern int hw_w, hw_h;

#endif /* OWOS_WMSERVER_H */
