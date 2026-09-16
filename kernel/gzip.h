/* gzip.h — a freestanding gzip (RFC 1952) codec over DEFLATE (RFC 1951).
 *
 * Interoperable with the standard gzip/gunzip tools: the encoder emits a real
 * gzip member (10-byte header, a DEFLATE body, CRC32 + ISIZE trailer) and the
 * decoder handles any DEFLATE stream — stored, fixed-Huffman, and
 * dynamic-Huffman blocks — so it can decompress server "Content-Encoding: gzip"
 * responses as well as files written by host gzip. The encoder runs an LZ77
 * match finder, builds dynamic Huffman codes, and emits whichever of the
 * dynamic, fixed-Huffman, or stored encodings is smallest for the input.
 */
#ifndef OWOS_GZIP_H
#define OWOS_GZIP_H

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

/* The original size recorded in a gzip trailer (ISIZE, the decompressed length
 * mod 2^32), or -1 if src is not a gzip member. Lets a caller size its output
 * buffer before decompressing. */
int gzip_orig_size(const uint8_t *src, uint32_t slen);

/* Decompress a raw zlib stream (RFC 1950: 2-byte header + DEFLATE body +
 * adler32 trailer) into dst (capacity dcap). This is the same DEFLATE engine
 * gzip uses, minus the gzip envelope — it's what PNG's IDAT data is wrapped in.
 * Returns the decompressed length, or -1 if src isn't a valid zlib stream, uses
 * a preset dictionary (FDICT — unsupported, and unused by PNG), the stream is
 * corrupt, or dst is too small. The adler32 trailer is not verified. */
int zlib_inflate(const uint8_t *src, uint32_t slen, uint8_t *dst, uint32_t dcap);

/* Compress slen bytes of src into a raw zlib stream (the inverse of
 * zlib_inflate — 2-byte header + DEFLATE body + adler32 trailer). This is the
 * wrapper a PNG encoder needs for its IDAT data. Returns the stream length, or
 * -1 if dst is too small. gzip_bound() is a safe upper bound on dcap. */
int zlib_compress(const uint8_t *src, uint32_t slen, uint8_t *dst, uint32_t dcap);

/* Wrap src in a raw zlib stream using only *stored* (uncompressed) DEFLATE
 * blocks: a valid zlib/PNG stream produced by an O(n) copy, with none of the
 * LZ77 match-search cost. The output is ~the input size (no compression), but
 * it's produced in well under a second even at full-screen sizes — the right
 * trade for a screenshot key that must not stall the UI. Same envelope as
 * zlib_compress (decodable by zlib_inflate / any PNG reader). Returns the
 * stream length, or -1 if dst is too small. gzip_bound() is a safe upper bound.
 */
int zlib_store(const uint8_t *src, uint32_t slen, uint8_t *dst, uint32_t dcap);

#endif /* OWOS_GZIP_H */
