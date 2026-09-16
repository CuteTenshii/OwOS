/* vp8.h — a VP8 key-frame (intra) decoder for lossy WebP.
 *
 * WebP's lossy codec is a single VP8 key frame (RFC 6386): a boolean entropy
 * coder over intra-predicted 16x16 macroblocks, a 4x4 integer DCT + Walsh-
 * Hadamard DC transform, and an in-loop deblocking filter, decoded to YUV 4:2:0
 * and converted to RGB. Inter frames, which never appear in a WebP, are not
 * handled. The decoder is a self-contained port of libwebp's reference C paths;
 * the probability, quantizer and mode tables are copied verbatim from it, the
 * way the h264/mpeg codecs mirror FFmpeg's tables.
 *
 * Like the VP8L path, this parses untrusted input in user space: a hostile
 * frame can only fault this process, never the kernel.
 */
#ifndef OWOS_VP8_H
#define OWOS_VP8_H

enum {
  VP8_OK = 0,
  VP8_EFORMAT = -1, /* not a valid VP8 key frame / corrupt bitstream */
  VP8_ENOMEM = -2,  /* allocation failed                            */
};

/* Decode a raw VP8 key-frame payload (the bytes of a "VP8 " chunk) into a fresh
 * malloc'd RGBA buffer (4*w*h, byte order R,G,B,A; alpha set to 255). Fills *w
 * and *h. Returns VP8_OK or a VP8_E* code; on error *out is left NULL. */
int vp8_decode(const unsigned char *data, int size, unsigned char **out, unsigned *w, unsigned *h);

#endif /* OWOS_VP8_H */
