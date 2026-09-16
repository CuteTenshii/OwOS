/* notify.h — client API for the notification daemon (/bin/notifyd).
 *
 * Like the clipboard: one short request over an IPC port to a resident daemon
 * that keeps the history and draws the UI in its own window. Fails gracefully
 * (returns -1) when notifyd isn't running. */
#ifndef OWOS_NOTIFY_H
#define OWOS_NOTIFY_H

#define NOTIFY_MAX_BUTTONS 3

/* Wire protocol, shared with notifyd.c. Strings are length-prefixed: u8 for
 * short fields, u16 LE for long ones; oversized fields are truncated. */
#define NOTIFY_PORT "notify"
#define NOTIFY_OP_NOTIFY 'N' /* raise a notification -> 'K' */
#define NOTIFY_OP_SHOW 'S'   /* open the Notification Center -> 'K' */
#define NOTIFY_ACK 'K'

#define NOTIFY_APP_MAX 32
#define NOTIFY_TITLE_MAX 96
#define NOTIFY_SUB_MAX 96
#define NOTIFY_BODY_MAX 512
#define NOTIFY_IMG_MAX 256
#define NOTIFY_BLABEL_MAX 32
#define NOTIFY_BCMD_MAX 192

struct notify_button {
  const char *label;
  const char *action; /* command spawned on click, or NULL for none */
};

struct notification {
  const char *app;   /* sender name (NULL -> "App") */
  const char *title; /* required; empty title -> no-op */
  const char *subtitle;
  const char *body;
  const char *image; /* PNG path for the thumbnail, or NULL */
  struct notify_button buttons[NOTIFY_MAX_BUTTONS];
  int nbuttons;
};

int notify_send(const struct notification *n); /* 0, or -1 if notifyd is down */
int notify_simple(const char *title, const char *body);
int notify_show(void); /* open the Notification Center */

#endif /* OWOS_NOTIFY_H */
