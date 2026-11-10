/* airplay.h: AirPlay 1 video sender (HTTP on port 7000), consumer-agnostic.
 *
 *   - discovery: AirPlay receivers on the LAN over DNS-SD ("_airplay._tcp.local"), one-shot (airplay_discover)
 *     or a background scanner (airplay_scan_*) a UI polls.
 *   - a session: a plain-HTTP connection to the receiver that hands it a media URL (POST /play), which the
 *     receiver then fetches itself, and drives it (/rate, /scrub, /stop). Playback status comes from polling
 *     /playback-info on a thread and is published as plain fields (airplay_get_status).
 *
 * This is the unauthenticated legacy protocol: it works with receivers that accept "anyone on the network"
 * (Apple TV 2/3, AirPort Express, most AirPlay smart TVs). A receiver that demands pairing (a tvOS device set to
 * require a code, or any FairPlay-only target) refuses /play and airplay_load returns -1. There is no receiver
 * volume control in this protocol. Link with $(AIRPLAY) $(PLIST) $(BASE64) $(MATH) plus $(NETCONN) $(TLS) $(CRYPTO) $(RESOLVE). */
#ifndef OWOS_AIRPLAY_H
#define OWOS_AIRPLAY_H

#include <stdint.h>

struct airplay_device {
  char id[32];    /* TXT deviceid=: the receiver's MAC address */
  char name[128]; /* DNS-SD instance name                      */
  char model[64]; /* TXT model=                                */
  uint8_t ip[4];
  uint16_t port; /* SRV port, normally 7000 */
};

/* Find receivers that can play video; blocks up to timeout_ms. Count (>=0) or a negative error. */
int airplay_discover(struct airplay_device *out, int max, int timeout_ms);

void airplay_scan_start(void); /* idempotent; rediscovers on its own thread */
unsigned airplay_scan_gen(void);
int airplay_scan_count(void);
int airplay_scan_get(int i, struct airplay_device *out); /* 1 filled / 0 out of range */

typedef struct airplay_session airplay_session;

enum { AIRPLAY_DISCONNECTED, AIRPLAY_CONNECTED };
enum { AIRPLAY_IDLE, AIRPLAY_BUFFERING, AIRPLAY_PLAYING, AIRPLAY_PAUSED };

struct airplay_status {
  int conn;            /* AIRPLAY_DISCONNECTED / CONNECTED */
  int state;           /* AIRPLAY_IDLE / BUFFERING / PLAYING / PAUSED */
  double current_time; /* seconds */
  double duration;     /* seconds, 0 if unknown */
};

/* Reach the receiver (GET /server-info) and start polling it. NULL on failure. */
airplay_session *airplay_connect(const struct airplay_device *dev, int timeout_ms);

/* Have the receiver fetch and play `url` from `start` seconds. 0 / -1. */
int airplay_load(airplay_session *s, const char *url, double start);
int airplay_play(airplay_session *s);
int airplay_pause(airplay_session *s);
int airplay_seek(airplay_session *s, double seconds);
int airplay_stop(airplay_session *s);

void airplay_get_status(airplay_session *s, struct airplay_status *out);

/* Stop playback, join the poller and free. Safe on NULL. */
void airplay_close(airplay_session *s);

#endif
