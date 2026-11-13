/* wmproto.h: the window protocol between apps (user/lib/win/win.c) and the compositor, over the "wm" IPC port.
 *
 * Every message starts with struct wm_msg; WM_CREATE and WM_TITLE append a NUL-padded title. Pixel buffers are shm
 * objects the compositor creates and grants to the client's pid. On a resize the compositor names the buffer to draw
 * into next; it keeps showing the previous one until the client's WM_ACK says it now draws at the new size, which
 * the client sends just before its next present.
 */
#ifndef OWOS_WMPROTO_H
#define OWOS_WMPROTO_H

#include <stdint.h>

#define WM_PORT "wm"
#define WM_TITLE_LEN 64

enum {
  /* client -> compositor */
  WM_CREATE = 1, /* a = w, b = h, + title */
  WM_PRESENT,    /* id; a, b, c, d = damage x, y, w, h (c or d <= 0: all) */
  WM_CLOSE,      /* id */
  WM_TITLE,      /* id, + title */
  WM_RESIZABLE,  /* id; a = on */
  WM_FULLSCREEN, /* id; a = on */
  WM_CURSOR,     /* id; a = WIN_CURSOR_* */
  WM_PARENT,     /* id; a = parent id or -1, b = modal */
  WM_ACK,        /* id; a = buffer handle, b, c = the size the client now draws at */
  WM_OWNS,       /* a = pid */
  /* compositor -> client */
  WM_CREATED = 32, /* id (-1 on failure); a = buffer handle, b, c = w, h */
  WM_EVENT,        /* id; a = WEV_*, b, c = its arguments */
  WM_RESIZE,       /* id; a = buffer handle to draw into next, b, c = new client w, h */
  WM_OWNS_REPLY,   /* a = pid, b = 1 if it owns a window */
};

struct wm_msg {
  uint16_t type;
  uint16_t len; /* whole message, header included */
  int32_t id;
  int32_t a, b, c, d;
};

struct wm_msg_title {
  struct wm_msg m;
  char title[WM_TITLE_LEN];
};

#define WM_MSG_MAX ((int)sizeof(struct wm_msg_title))

#endif /* OWOS_WMPROTO_H */
