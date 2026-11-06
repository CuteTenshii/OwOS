/* mediactl.h: client API for the system media session service (/bin/mediad).
 * A player registers a session, publishes metadata and state, and polls for transport commands
 * (media keys, the Now Playing control, `mediactl`). All calls return -1 when mediad is not running. */
#ifndef OWOS_MEDIACTL_H
#define OWOS_MEDIACTL_H

#define MEDIA_PORT "media"
#define MEDIA_STR_APP 32
#define MEDIA_STR_TEXT 256
#define MEDIA_STR_ART 512

enum media_state { MEDIA_STOPPED = 0, MEDIA_PLAYING, MEDIA_PAUSED };

enum media_cmd {
  MEDIA_CMD_NONE = 0,
  MEDIA_CMD_PLAY,
  MEDIA_CMD_PAUSE,
  MEDIA_CMD_TOGGLE,
  MEDIA_CMD_STOP,
  MEDIA_CMD_NEXT,
  MEDIA_CMD_PREV,
  MEDIA_CMD_SEEK_FWD,  /* arg: milliseconds, 0 = default step  */
  MEDIA_CMD_SEEK_BACK, /* arg: milliseconds, 0 = default step  */
  MEDIA_CMD_SEEK_TO,   /* arg: absolute position in ms         */
};

/* Actions the session can handle (bit per media_cmd, 1u << cmd). */
#define MEDIA_CAP(cmd) (1u << (cmd))

struct media_info {
  char app[MEDIA_STR_APP];
  char title[MEDIA_STR_TEXT];
  char artist[MEDIA_STR_TEXT];
  char album[MEDIA_STR_TEXT];
  char artwork[MEDIA_STR_ART]; /* image file path, or empty */
  int state;                   /* enum media_state */
  unsigned pos_ms, dur_ms;     /* dur_ms 0 = unknown/live */
  unsigned caps;               /* MEDIA_CAP bits */
};

int mediactl_open(const char *app);                         /* session id >= 0, or -1 */
int mediactl_update(int id, const struct media_info *info); /* publish full state; 0/-1 */
int mediactl_poll(int id, unsigned *arg);    /* next pending command (enum media_cmd) or MEDIA_CMD_NONE */
int mediactl_close(int id);                  /* 0/-1 */
int mediactl_status(struct media_info *out); /* the active session; 1 = have one, 0 = none, -1 = no service */
int mediactl_send(int cmd, unsigned arg);    /* command to the active session; 0 delivered/queued, -1 */

#endif
