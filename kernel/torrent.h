/* torrent.h — parse a .torrent metainfo file on top of the bencode parser.
 *
 * Pulls the fields a client actually needs out of the bencoded dictionary and
 * computes the 20-byte infohash (SHA-1 of the info dictionary's exact on-disk
 * bytes) — the identifier used in tracker announces and the peer handshake.
 *
 * Spans (announce, name, pieces) point into the source buffer passed to
 * torrent_parse(), which must stay alive while the struct is in use. The
 * bencode node pool is heap-allocated; release it with torrent_free().
 */
#ifndef OWOS_TORRENT_H
#define OWOS_TORRENT_H

#include <stdint.h>
#include "bencode.h"

struct torrent {
	struct bencode   ben;         /* parse tree (nodes are heap-allocated)    */

	const char      *announce;    /* primary tracker URL (NUL-terminated copy) */
	char             announce_buf[256];

	const uint8_t   *name;        /* suggested file/dir name (into source)    */
	uint32_t         name_len;

	int64_t          piece_length;
	const uint8_t   *pieces;      /* 20*n_pieces concatenated SHA-1 hashes    */
	uint32_t         n_pieces;

	int64_t          total_length;/* sum of all file lengths, in bytes        */
	int              file_count;  /* 1 for single-file torrents               */
	int              multi_file;  /* non-zero if the info dict has a file list */

	uint8_t          infohash[20];
};

/* Parse buf[0..len) as a .torrent. Returns 0 on success, -1 on malformed
 * input / missing required fields / out of memory. On success, call
 * torrent_free() when done to release the node pool. */
int  torrent_parse(struct torrent *t, const uint8_t *buf, uint32_t len);
void torrent_free(struct torrent *t);

/* Human-readable dump for the `torrent info` shell command. */
void torrent_print(const struct torrent *t);

/* Parse an embedded known-good torrent and check every parsed field plus the
 * infohash against expected values; prints PASS/FAIL and returns 0 iff all
 * passed. Wired to `torrent test`. */
int  torrent_selftest(void);

#endif /* OWOS_TORRENT_H */
