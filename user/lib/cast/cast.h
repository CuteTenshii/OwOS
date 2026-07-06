/* cast.h — Google Cast (Chromecast) sender protocol, consumer-agnostic.
 *
 * Two layers, usable apart:
 *   - discovery: find Cast receivers on the LAN over DNS-SD (mdns_browse of
 *     "_googlecast._tcp.local"), one-shot (cast_discover) for a CLI, or a
 *     background scanner (cast_scan_*) a UI polls for live availability.
 *   - a session: a TLS connection to a device's port 8009 speaking the Cast
 *     channel protocol (length-prefixed CastMessage protobufs carrying JSON
 *     payloads), which launches the Default Media Receiver and drives it
 *     (LOAD/PLAY/PAUSE/SEEK/STOP/volume). Playback status is published as plain
 *     fields the consumer polls (cast_get_status) — the session never calls back
 *     into a scripting engine, exactly like the media mixer.
 *
 * The transport is user-space TLS (user/lib/tls via netconn); the receiver's
 * certificate is NOT verified, matching the rest of the stack. Frames are the
 * general protobuf wire codec (user/lib/protobuf); payloads are the general JSON
 * value tree (user/lib/json). Link with $(CAST) plus $(PROTOBUF) $(JSON) $(MATH)
 * $(NETCONN) $(TLS) $(CRYPTO) $(RESOLVE) (which bundles $(MDNS)).
 *
 * A session owns two threads (a blocking reader + a keepalive pinger); it is
 * real-hardware territory — QEMU/SLIRP has no Cast device, so cast_discover
 * simply returns 0 there and every entry point degrades gracefully.
 */
#ifndef OWOS_CAST_H
#define OWOS_CAST_H

#include <stdint.h>

/* A discovered Cast receiver. The strings come from the DNS-SD TXT record. */
struct cast_device {
	char     id[64];     /* TXT id= : the device's stable UUID          */
	char     name[128];  /* TXT fn= : friendly name ("Living Room TV")  */
	char     model[64];  /* TXT md= : model name                        */
	uint8_t  ip[4];      /* resolved A record                           */
	uint16_t port;       /* SRV port, normally 8009                     */
};

/* One-shot synchronous discovery. Fills up to `max` devices, returns the count
 * (>=0) or a negative error (NET_EINTR on a cooperative Ctrl+C). Blocks up to
 * `timeout_ms`. */
int cast_discover(struct cast_device *out, int max, int timeout_ms);

/* Background scanner: rediscovers periodically on its own thread so a caller can
 * read a fresh device list without blocking. cast_scan_start is idempotent.
 * cast_scan_gen() bumps whenever the discovered set changes (so a poller knows
 * when to re-read). cast_scan_count()/cast_scan_get() snapshot the current set. */
void     cast_scan_start(void);
unsigned cast_scan_gen(void);
int      cast_scan_count(void);
int      cast_scan_get(int i, struct cast_device *out);   /* 1 filled / 0 out of range */

/* An active session with one receiver's Default Media Receiver app. */
typedef struct cast_session cast_session;

/* Connection lifecycle (cast_status.conn). */
enum { CAST_DISCONNECTED, CAST_CONNECTING, CAST_CONNECTED };

/* Player state (cast_status.state), mirroring MEDIA_STATUS playerState. */
enum { CAST_IDLE, CAST_BUFFERING, CAST_PLAYING, CAST_PAUSED };

struct cast_status {
	int    conn;          /* CAST_DISCONNECTED / CONNECTING / CONNECTED */
	int    state;         /* CAST_IDLE / BUFFERING / PLAYING / PAUSED   */
	double current_time;  /* playback position, seconds                 */
	double duration;      /* media length, seconds (0 if unknown)       */
	double volume;        /* receiver volume, 0.0 .. 1.0                 */
	int    muted;
};

/* Open a session: TLS-connect to dev->ip:port, launch the Default Media
 * Receiver, and wait up to `timeout_ms` for its transport to come up. Returns a
 * connected session, or NULL if the connection or launch failed. */
cast_session *cast_connect(const struct cast_device *dev, int timeout_ms);

/* Load `url` into the receiver and begin playback. `content_type` is a MIME hint
 * (e.g. "video/mp4"; NULL lets the receiver sniff); `title` is optional display
 * metadata. Waits up to `timeout_ms` for the media session to establish.
 * Returns 0 on success, -1 on failure. */
int cast_load(cast_session *s, const char *url, const char *content_type,
              const char *title, int timeout_ms);

int cast_play(cast_session *s);
int cast_pause(cast_session *s);
int cast_stop(cast_session *s);                    /* stop media, keep the session */
int cast_seek(cast_session *s, double seconds);
int cast_set_volume(cast_session *s, double level); /* 0.0 .. 1.0 */
int cast_set_muted(cast_session *s, int muted);

/* Snapshot the published status (safe from any thread). */
void cast_get_status(cast_session *s, struct cast_status *out);

/* Tear down: stop the receiver app, close the channel, join the threads and
 * free. May take up to ~5 s (the reader unblocks on the kernel's socket idle
 * timeout once the keepalive stops). Safe to pass NULL. */
void cast_close(cast_session *s);

#endif /* OWOS_CAST_H */
