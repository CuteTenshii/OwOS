#ifndef CRC32_H
#define CRC32_H

#include <stdint.h>

/* CRC-32 (reflected, polynomial 0xEDB88820 — the zlib / gzip / PNG variant).
 *
 * Canonical zlib API: seed with crc32(0, 0, 0) and fold data in. Each call
 * brackets the accumulator with the pre/post inversion, so a one-shot
 * crc32(0, buf, len) and incremental chaining (crc = crc32(crc, buf, len))
 * both give the finished checksum directly — no external finalize.
 *
 * Not cryptographic: an accidental-corruption / protocol-demux checksum only
 * (gzip integrity, STUN FINGERPRINT). Authentication is HMAC's job. */
uint32_t crc32(uint32_t crc, const void *buf, uint32_t len);

#endif /* CRC32_H */
