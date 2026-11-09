/* tray.h: client API for the compositor's system tray.
 *
 * One request per short-lived connection to the "tray" port, like notify. Icons are keyed by
 * app-chosen id. Both calls return -1 when the compositor isn't running. */
#ifndef OWOS_TRAY_H
#define OWOS_TRAY_H

#define TRAY_PORT "tray"
#define TRAY_OP_SET 'S'    /* add or replace an icon -> 'K' */
#define TRAY_OP_REMOVE 'R' /* remove an icon -> 'K' */
#define TRAY_ACK 'K'

#define TRAY_ID_MAX 32
#define TRAY_ICON_MAX 96
#define TRAY_CMD_MAX 192

struct tray_icon {
  int pid;            /* owner: the icon goes when it exits; 0 = stays until removed */
  const char *id;     /* unique per app, replaces an existing icon with the same id */
  const char *icon;   /* PNG path, or a name under /usr/share/icons */
  const char *action; /* command run on left click, or NULL */
  const char *menu;   /* command run on right click, or NULL */
};

int tray_set(const struct tray_icon *t);
int tray_remove(const char *id);

#endif /* OWOS_TRAY_H */
