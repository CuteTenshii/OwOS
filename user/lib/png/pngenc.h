/* pngenc.h — encode a 32-bit XRGB framebuffer to a PNG file, in user space.
 *
 * Uncompressed: the IDAT is a zlib stream of *stored* DEFLATE blocks (no
 * compression), so encoding is a single fast pass and the only dependency is the
 * shared CRC-32 (crc32.h) — no deflate engine. This is the screenshot path; the
 * compositor owns the framebuffer, so the capture lives there, not in the kernel.
 * Decoding is the separate upng.h. */
#ifndef PNGENC_H
#define PNGENC_H

/* Write `pixels` (w x h, XRGB8888, `pitch` pixels per row) to `path` as a PNG.
 * Returns 0 on success, -1 on a bad dimension / allocation / write failure. */
int png_write(const char *path, const unsigned *pixels,
              unsigned w, unsigned h, unsigned pitch);

#endif /* PNGENC_H */
