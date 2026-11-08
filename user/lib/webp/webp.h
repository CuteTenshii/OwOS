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
 * with their ALPH alpha plane, and animation (see webp_anim_open). The encoder emits a
 * standards-conformant lossless VP8L stream (subtract-green transform + LZ77 +
 * dynamic Huffman) that any conformant decoder — including libwebp's dwebp —
 * reads back.
 */
#ifndef OWOS_WEBP_H
#define OWOS_WEBP_H

enum {
  WEBP_OK = 0,
  WEBP_EBADSIG = -1, /* not a RIFF/WEBP container                          */
  WEBP_EUNSUP = -2,  /* valid WebP we don't decode (unknown VP8L version)  */
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
 * Fills *w and *h. Returns WEBP_OK, or a WEBP_E* code (an animated file yields its
 * first composed frame). On any error *out is left NULL. */
int webp_decode(const unsigned char *buf, int len, unsigned char **out, unsigned *w, unsigned *h);

/* ---- Animation (ANIM/ANMF) ---------------------------------------------
 * Frames are composed the way libwebp's WebPAnimDecoder and browsers do it: the
 * canvas starts transparent, each frame's disposal (clear its rectangle to
 * transparent black, never the ANIM background colour) is applied before the
 * next one is drawn, and a frame is blended or overwritten per its blend flag.
 * webp_decode() on an animated file returns the first composed frame. */
struct webp_anim;

/* Open an animated WebP. Returns WEBP_OK, WEBP_EUNSUP if the file is not
 * animated (use webp_decode), or another WEBP_E* code. `data` must outlive the
 * decoder. *w,*h receive the canvas size (at most 16384 per side); every frame
 * must lie inside it, else WEBP_EFORMAT. */
int webp_anim_open(struct webp_anim **out, const unsigned char *data, int len, unsigned *w, unsigned *h);

/* Compose the next frame into `canvas` (w*h*4 straight RGBA, caller-owned,
 * zeroed at start; it must persist between calls). *delay_ms gets the frame
 * duration. Returns 1 (frame composed), 0 (no more frames, canvas unchanged) or
 * a negative WEBP_E* code (canvas keeps what was composed so far). */
int webp_anim_next(struct webp_anim *a, unsigned char *canvas, int *delay_ms);

/* Restart at the first frame and zero `canvas`. */
void webp_anim_rewind(struct webp_anim *a, unsigned char *canvas);

/* ANIM loop count: 0 = forever, N = N plays in total. */
int webp_anim_loops(const struct webp_anim *a);

void webp_anim_close(struct webp_anim *a);

/* Encode an RGBA image (`rgba`, 4*w*h bytes, byte order R,G,B,A) as a lossless
 * VP8L WebP into a freshly malloc'd buffer (*out, caller frees; *outlen bytes).
 * Returns WEBP_OK or a WEBP_E* code. The output round-trips exactly (lossless)
 * and is decodable by any conformant WebP decoder. */
int webp_encode(const unsigned char *rgba, unsigned w, unsigned h, unsigned char **out, int *outlen);

#endif /* OWOS_WEBP_H */
