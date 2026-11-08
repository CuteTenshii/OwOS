/* deflate.h — a freestanding gzip (RFC 1952) codec over DEFLATE (RFC 1951),
 * for OwOS user programs. A near-verbatim port of the kernel's gzip
 * codec (kernel/gzip.c) with kmalloc/endian dependencies removed.
 *
 * Interoperable with the standard gzip/gunzip tools: the encoder emits a real
 * gzip member (10-byte header, a DEFLATE body, CRC32 + ISIZE trailer) and the
 * decoder handles any DEFLATE stream — stored, fixed-Huffman, and
 * dynamic-Huffman blocks.
 */
#ifndef OWOS_USER_DEFLATE_H
#define OWOS_USER_DEFLATE_H

#include <stdint.h>

/* A safe upper bound on the compressed size of src_len bytes. */
uint32_t gzip_bound(uint32_t src_len);

/* Compress slen bytes of src into dst (capacity dcap). Returns the compressed
 * length, or -1 if dst is too small. */
int gzip_compress(const uint8_t *src, uint32_t slen, uint8_t *dst, uint32_t dcap);

/* Decompress a gzip member into dst (capacity dcap). Returns the original
 * length, or -1 if src is not valid gzip, the stream is corrupt, dst is too
 * small, or the CRC32/length check fails. */
int gzip_decompress(const uint8_t *src, uint32_t slen, uint8_t *dst, uint32_t dcap);

/* Containers for deflate_compress. dst needs gzip_bound(slen) bytes for any of them. */
enum { DEFLATE_GZIP, DEFLATE_ZLIB, DEFLATE_RAW };
/* Compress into a gzip member, a zlib stream (RFC 1950) or a bare DEFLATE body.
 * Returns the compressed length, or -1 if dst is too small. */
int deflate_compress(int fmt, const uint8_t *src, uint32_t slen, uint8_t *dst, uint32_t dcap);

/* The Adler-32 checksum that ends a zlib stream. */
uint32_t zlib_adler32(const uint8_t *p, uint32_t n);

/* The original size recorded in a gzip trailer (ISIZE, the decompressed length
 * mod 2^32), or -1 if src is not a gzip member. Lets a caller size its output
 * buffer before decompressing. */
int gzip_orig_size(const uint8_t *src, uint32_t slen);

#endif /* OWOS_USER_DEFLATE_H */
