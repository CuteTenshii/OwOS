/* tracker.h — BitTorrent HTTP tracker client.
 *
 * Announces to an HTTP tracker (the GET-with-query "BEP 3" protocol) and
 * parses the bencoded reply into a peer list. Requests compact peers (BEP 23)
 * but also understands the legacy list-of-dicts form. UDP and HTTPS trackers
 * are out of scope; the announce goes over the existing HTTP client.
 */
#ifndef OWOS_TRACKER_H
#define OWOS_TRACKER_H

#include <stdint.h>

struct tracker_peer {
	uint8_t  ip[4];
	uint16_t port;
};

/* Announce `infohash` (20 bytes) to `announce` (an http:// tracker URL) and
 * fill peers[0..max). `left` is the number of bytes still needed (sent as the
 * "left=" query param). On success returns the number of peers written (>=0)
 * and, if interval_out is non-NULL, stores the tracker's re-announce interval
 * in seconds. Returns -1 on a transport/parse error or a tracker failure
 * (whose reason, if any, is printed). */
int tracker_announce(const char *announce, const uint8_t infohash[20],
                     int64_t left, struct tracker_peer *peers, int max,
                     int *interval_out);

/* Print a peer list (ip:port per line) the way `torrent peers` wants it. */
void tracker_print(const struct tracker_peer *peers, int count, int interval);

/* Parse an embedded known tracker reply and verify peer extraction + the
 * infohash percent-encoding, with no network. Prints PASS/FAIL; returns 0 iff
 * all passed. Wired to `torrent peers test`. */
int tracker_selftest(void);

#endif /* OWOS_TRACKER_H */
