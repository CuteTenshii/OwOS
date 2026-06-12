/* png.h — a minimal PNG decoder built on the kernel's DEFLATE engine.
 *
 * PNG is a chunk container (IHDR/IDAT/IEND/...) wrapping a single zlib stream of
 * filtered scanlines. The heavy lifting — inflate — is already in gzip.c; this
 * decoder only walks the chunks, inflates the concatenated IDAT data via
 * zlib_inflate(), reverses the per-scanline filters, and packs the result into
 * the framebuffer's XRGB8888 format ready for fb_blit().
 *
 * SCOPE (skeleton): 8-bit, non-interlaced truecolour — colour type 2 (RGB) and
 * 6 (RGBA). Palette (type 3), grayscale (0/4), sub-8-bit depths, 16-bit, and
 * Adam7 interlacing are rejected with a distinct error for now (see TODOs in
 * png.c). This covers the majority of real-world PNGs.
 */
#ifndef OWOS_PNG_H
#define OWOS_PNG_H

#include <stdint.h>

typedef struct {
	uint32_t  width;
	uint32_t  height;
	uint32_t *pixels;   /* width*height XRGB8888, heap-allocated (kmalloc).    */
	                    /* On success the caller owns it and must kfree() it.  */
} png_image;

enum {
	PNG_OK              =  0,
	PNG_EBADSIG         = -1,   /* not a PNG (signature mismatch)              */
	PNG_ETRUNC          = -2,   /* stream ends mid-chunk                       */
	PNG_EBADHDR         = -3,   /* missing/short IHDR, or zero dimensions      */
	PNG_EUNSUPPORTED    = -4,   /* valid PNG, but a feature we don't decode yet*/
	PNG_EINFLATE        = -5,   /* zlib/DEFLATE error in the IDAT data         */
	PNG_EBADFILTER      = -6,   /* unknown scanline filter byte                */
	PNG_ENOMEM          = -7,   /* kmalloc failed                              */
	PNG_ETOOBIG         = -8,   /* dimensions overflow / exceed sane limits    */
};

/* Decode the PNG in src[0..slen). On PNG_OK, *out is filled and out->pixels is
 * a freshly kmalloc'd XRGB8888 buffer the caller must kfree(). On any error the
 * return value is one of the PNG_E* codes and *out is left untouched (nothing
 * to free). */
int png_decode(const uint8_t *src, uint32_t slen, png_image *out);

/* Human-readable form of a png_decode() return code (for diagnostics). */
const char *png_strerror(int err);

/* Encode a w*h XRGB8888 image into a PNG file written to dst (capacity dcap).
 * `pixels` is read with a row stride of `src_pitch` pixels (pass w for a packed
 * buffer, or a framebuffer's pitch to encode straight from the back buffer).
 * Produces an 8-bit truecolour (RGB, no alpha) PNG using filter 0 and the
 * kernel's DEFLATE compressor. Returns the file length, or a PNG_E* code (<0).
 * png_encode_bound() gives a safe dcap. */
int png_encode(const uint32_t *pixels, uint32_t w, uint32_t h,
               uint32_t src_pitch, uint8_t *dst, uint32_t dcap);

/* A safe upper bound on the encoded size of a w*h image. */
uint32_t png_encode_bound(uint32_t w, uint32_t h);

/* Encode a w*h XRGB8888 image (read with row stride `src_pitch` pixels) and
 * stream it straight to the owofs file `path`, with only a small fixed buffer in
 * RAM — no multi-megabyte allocation. Same format as png_encode (8-bit RGB,
 * stored zlib blocks). Pre-allocates the file, then fills it with fs_write_at.
 * Returns PNG_OK or a PNG_E* code (<0). */
int png_write_stream(const char *path, const uint32_t *pixels, uint32_t w,
                     uint32_t h, uint32_t src_pitch);

#endif /* OWOS_PNG_H */
