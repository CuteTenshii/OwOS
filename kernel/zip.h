/* zip.h — LZSS compression behind the zip/unzip shell commands.
 *
 * A self-contained LZ77-family codec: no allocations of its own beyond the
 * caller's buffers (the compressor borrows a little heap for its hash tables),
 * freestanding, and byte-stream in / byte-stream out. The container is an
 * 8-byte header ("OWOZ" + the original size) followed by the LZSS token stream.
 */
#ifndef OWOS_ZIP_H
#define OWOS_ZIP_H

#include <stdint.h>

#define ZIP_HEADER 8            /* magic (4) + original size (4)               */

/* A safe upper bound on the compressed size of src_len bytes, so callers can
 * size the destination buffer before compressing. */
uint32_t zip_bound(uint32_t src_len);

/* Compress slen bytes of src into dst (capacity dcap). Returns the compressed
 * length (>= ZIP_HEADER), or -1 if dst is too small. */
int zip_compress(const uint8_t *src, uint32_t slen, uint8_t *dst, uint32_t dcap);

/* Decompress an OWOZ buffer into dst (capacity dcap). Returns the original
 * length, or -1 if the input is not a valid/intact archive or dst is too
 * small. */
int zip_decompress(const uint8_t *src, uint32_t slen, uint8_t *dst, uint32_t dcap);

/* The original (uncompressed) size recorded in an OWOZ header, or -1 if src is
 * not an OwOS zip archive. Lets a caller size the output buffer up front. */
int zip_orig_size(const uint8_t *src, uint32_t slen);

#endif /* OWOS_ZIP_H */
