/* zstd.h: a from-scratch Zstandard codec (RFC 8878).
 *
 * Whole-buffer API in OwOS's style (mirrors brotli.h): the decoder grows a heap
 * output buffer; the encoder returns a heap buffer. Both return NULL on error;
 * the caller frees the result.
 *
 * The decoder is complete (FSE + Huffman entropy stages, every literals and
 * sequence mode, the three repeat offsets, and optional XXH64 content-checksum
 * verification), so it reads streams from the reference `zstd` encoder. The
 * encoder produces valid frames (LZ77 matching + predefined-FSE sequences,
 * Huffman-or-raw literals) that the reference decoder accepts. */
#ifndef OWOS_ZSTD_H
#define OWOS_ZSTD_H

/* Decompress a single-or-multi-frame zstd stream. Returns a malloc'd buffer of
 * *out_len bytes (0-length input decodes to a 0-length, non-NULL buffer), or
 * NULL on malformed input / OOM / checksum mismatch. */
unsigned char *zstd_decompress(const unsigned char *in, int in_len, int *out_len);

/* Compress `in` into one zstd frame. `level` selects match-search effort
 * (1..19; clamped); higher searches harder for a better ratio. Returns a
 * malloc'd buffer of *out_len bytes, or NULL on OOM. The output is always a
 * valid frame the reference decoder accepts, and never much larger than the
 * input (incompressible data falls back to stored blocks). */
unsigned char *zstd_compress(const unsigned char *in, int in_len, int *out_len, int level);

#endif /* OWOS_ZSTD_H */
