/* inflate.h — a streaming DEFLATE/zlib/gzip decompressor for user programs.
 *
 * Pure user space: no syscall, no kernel involvement — decompression is just
 * computation, so it lives in the library the program links. Designed to stream
 * HTTP/HTTPS responses: it PULLS compressed bytes through a reader callback (so
 * the caller can feed straight from net_recv, never buffering the whole body)
 * and PUSHES decompressed bytes through a writer callback (so the caller renders
 * or stores as it arrives). Only the mandatory 32 KiB DEFLATE back-reference
 * window is held in memory, whatever the body size.
 *
 * Early stop: if the writer returns < 0 (e.g. the caller's render buffer is
 * full), inflation halts cleanly and returns 0 — a natural, lossless way to cap
 * output without an all-or-nothing failure.
 *
 * Reentrant: each call allocates its own back-reference window + flush buffer,
 * so concurrent decodes on separate threads don't interfere.
 */
#ifndef OWOS_INFLATE_H
#define OWOS_INFLATE_H

/* Return the next compressed byte (0..255), or < 0 at end of input. */
typedef int (*inf_reader)(void *ctx);

/* Consume n decompressed bytes. Return 0 to continue, < 0 to stop early. */
typedef int (*inf_writer)(void *ctx, const unsigned char *buf, unsigned n);

/* Inflate a raw DEFLATE stream. Returns 0 on success (or caller early-stop),
 * -1 on a malformed stream. */
int inflate_stream(inf_reader rd, void *rctx, inf_writer wr, void *wctx);

/* Inflate an HTTP Content-Encoding body: auto-detects a gzip (1f 8b) or zlib
 * (0x78) wrapper, skips its header, inflates the DEFLATE body, and ignores the
 * trailer. A stream that is neither is treated as raw DEFLATE. Returns 0 / -1. */
int inflate_http(inf_reader rd, void *rctx, inf_writer wr, void *wctx);

#endif /* OWOS_INFLATE_H */
