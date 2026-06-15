/* torrent.h — .torrent metainfo parsing + infohash (userspace libtorrent).
 *
 * Ported from kernel/torrent.h. Pulls the fields a client needs out of the
 * bencoded dict and computes the 20-byte infohash (SHA-1 of the info dict's
 * exact on-disk bytes). Spans (announce, name, pieces) point into the source
 * buffer passed to torrent_parse(), which must outlive the struct. The node
 * pool is a static buffer (no malloc in user space) — one parse in flight at a time.
 */
#ifndef OWOS_USER_TORRENT_H
#define OWOS_USER_TORRENT_H

#include <stdint.h>
#include "bencode.h"

struct torrent {
	struct bencode   ben;
	const char      *announce;
	char             announce_buf[256];
	const uint8_t   *name;
	uint32_t         name_len;
	int64_t          piece_length;
	const uint8_t   *pieces;       /* 20*n_pieces concatenated SHA-1 hashes    */
	uint32_t         n_pieces;
	int64_t          total_length;
	int              file_count;
	int              multi_file;
	uint8_t          infohash[20];
};

int  torrent_parse(struct torrent *t, const uint8_t *buf, uint32_t len);
void torrent_free(struct torrent *t);
void torrent_print(const struct torrent *t);
int  torrent_selftest(void);

#endif /* OWOS_USER_TORRENT_H */
