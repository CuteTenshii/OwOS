/* zstd.h: bounds-checked Zstandard (RFC 8878) frame decoder, enough for btrfs compress=zstd extents. */
#ifndef OWOS_ZSTD_H
#define OWOS_ZSTD_H

#include <stdint.h>

/* Decode the first zstd frame of src (skippable frames before it are skipped) into dst (capacity dcap). Returns the
 * decoded length, or -1 on a corrupt or truncated frame, a dictionary frame, an oversized result or an allocation
 * failure. The content checksum is parsed but not verified. */
int zstd_decompress(const uint8_t *src, uint32_t slen, uint8_t *dst, uint32_t dcap);

#endif
