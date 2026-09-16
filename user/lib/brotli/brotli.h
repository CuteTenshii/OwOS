/* brotli.h — a from-scratch Brotli codec (RFC 7932), for HTTP `br`
 * content-coding and WOFF2 font decompression.
 *
 * Whole-buffer API matching OwOS's style: the decoder grows a heap output
 * buffer (the compressed stream doesn't carry its size); the encoder returns a
 * heap buffer too. Both return NULL on error; the caller frees the result. */
#ifndef OWOS_BROTLI_H
#define OWOS_BROTLI_H

/* Decompress a Brotli stream. Returns a malloc'd buffer of *out_len bytes, or
 * NULL on malformed input / OOM. */
unsigned char *brotli_decompress(const unsigned char *in, int in_len, int *out_len);

/* Compress `in` into a Brotli stream. Returns a malloc'd buffer of *out_len
 * bytes, or NULL on OOM. Correct (any decoder accepts it) but a pragmatic
 * ratio, not Google-encoder quality. */
unsigned char *brotli_compress(const unsigned char *in, int in_len, int *out_len);

#endif /* OWOS_BROTLI_H */
