/* webp.h — a WebP decoder (lossless VP8L and lossy VP8) + lossless encoder for user programs.
 *
 * Untrusted-input parsing belongs in user space: a malformed or hostile WebP
 * can only corrupt this process (which then faults and is reaped), never the
 * kernel. Unlike the streaming PNG decoder (png), VP8L is not a streaming
 * format — backward references and the spatial transforms need the whole image
 * in memory — so the API works on a complete in-memory buffer and produces a
 * whole RGBA image.
 *
 * Scope: the full lossless VP8L bitstream — the four transforms (predictor,
 * colour, subtract-green, colour-indexing), the colour cache, LZ77 backward
 * references, and meta-Huffman groups — and lossy VP8 key frames (see vp8.h)
 * with their ALPH alpha plane. Animation is not decoded (reported as
 * WEBP_EUNSUP). The encoder emits a
 * standards-conformant lossless VP8L stream (subtract-green transform + LZ77 +
 * dynamic Huffman) that any conformant decoder — including libwebp's dwebp —
 * reads back.
 */
#ifndef OWOS_WEBP_H
#define OWOS_WEBP_H

enum {
  WEBP_OK = 0,
  WEBP_EBADSIG = -1, /* not a RIFF/WEBP container                          */
  WEBP_EUNSUP = -2,  /* valid WebP we don't decode (animation)             */
  WEBP_ETRUNC = -3,  /* stream ended early                                 */
  WEBP_EFORMAT = -4, /* corrupt VP8L bitstream                             */
  WEBP_ENOMEM = -5,  /* allocation failed                                  */
};

/* Read just the canvas dimensions from the container (VP8X canvas size if the
 * file is extended, else the VP8L / VP8 frame header). Returns WEBP_OK or a
 * WEBP_E* code. `len` need only cover the file header (~32 bytes). */
int webp_dimensions(const unsigned char *buf, int len, unsigned *w, unsigned *h);

/* A read-only properties view for an "image info" panel. */
struct webp_info {
  unsigned w, h;
  int lossless;  /* 1 = VP8L (lossless), 0 = VP8 (lossy)                  */
  int has_alpha; /* an alpha channel is present                           */
  int animation; /* 1 = animated (ANIM/ANMF)                              */
  int extended;  /* 1 = VP8X extended container                           */
};

/* Fill *info from the container header (touches only a few header bytes). */
int webp_probe(const unsigned char *buf, int len, struct webp_info *info);

/* Decode a complete lossless WebP in `buf`/`len` into a freshly malloc'd RGBA
 * buffer (4*w*h bytes, byte order R,G,B,A), returned via *out (caller frees).
 * Fills *w and *h. Returns WEBP_OK, or a WEBP_E* code (WEBP_EUNSUP for an
 * animated file). On any error *out is left NULL. */
int webp_decode(const unsigned char *buf, int len, unsigned char **out, unsigned *w, unsigned *h);

/* Encode an RGBA image (`rgba`, 4*w*h bytes, byte order R,G,B,A) as a lossless
 * VP8L WebP into a freshly malloc'd buffer (*out, caller frees; *outlen bytes).
 * Returns WEBP_OK or a WEBP_E* code. The output round-trips exactly (lossless)
 * and is decodable by any conformant WebP decoder. */
int webp_encode(const unsigned char *rgba, unsigned w, unsigned h, unsigned char **out, int *outlen);

#endif /* OWOS_WEBP_H */
