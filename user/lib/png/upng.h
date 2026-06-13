/* upng.h — a streaming PNG decoder for ring-3 programs.
 *
 * Untrusted-input parsing belongs in user space: a malformed or hostile PNG can
 * only corrupt this process (which then faults and is reaped), never the
 * kernel. The decoder is fully streaming — it PULLS file bytes through a reader
 * callback (so the caller feeds straight from readfile_at, never buffering the
 * whole file) and emits one reconstructed scanline at a time through a row
 * callback. Working set is ~2 scanlines + the inflate window, whatever the
 * image size — so even a multi-megabyte screenshot decodes in the 1.5 MiB user
 * window.
 *
 * Scope: all five colour types — grayscale(0), RGB(2), palette(3), gray+alpha
 * (4), RGBA(6) — at bit depths 1/2/4/8/16, interlaced or not. Transparency is
 * preserved (alpha channel, palette tRNS, or colour-key) and the row is RGBA;
 * interlaced images are the one exception (emitted opaque). Non-interlaced
 * images stream with the ~2-scanline working
 * set above, whatever the height; interlaced (Adam7) images need a full RGB
 * frame buffer, so they are capped at 512x512 px and otherwise return
 * UPNG_EUNSUP. Built on the streaming inflate library.
 */
#ifndef OWOS_UPNG_H
#define OWOS_UPNG_H

/* Next PNG file byte (0..255), or < 0 at end of file. */
typedef int (*upng_reader)(void *ctx);

/* One reconstructed row: y is its index, rgba is 4*w bytes (R,G,B,A). Alpha is
 * 255 for opaque images; for images with transparency (alpha channel, palette
 * tRNS, or a colour-key) it carries the real value, so the caller can blend.
 * Return 0 to continue, < 0 to stop decoding early. */
typedef int (*upng_row)(void *ctx, unsigned y, const unsigned char *rgba, unsigned w);

enum {
	UPNG_OK       =  0,
	UPNG_EBADSIG  = -1,   /* not a PNG                                         */
	UPNG_EBADHDR  = -2,   /* missing/zero IHDR                                 */
	UPNG_EUNSUP   = -3,   /* valid PNG, unsupported form (palette/16-bit/…)    */
	UPNG_EINFLATE = -4,   /* corrupt IDAT / bad filter                         */
	UPNG_ETRUNC   = -5,   /* stream ended early                                */
};

/* Peek the image dimensions from IHDR (reads only the start of the stream). */
int upng_dimensions(upng_reader rd, void *rctx, unsigned *w, unsigned *h);

/* Stream-decode, invoking row() per scanline. Fills *out_w/*out_h if non-NULL.
 * Returns UPNG_OK (incl. caller early-stop) or a UPNG_E* code. */
int upng_decode(upng_reader rd, void *rctx, upng_row row, void *row_ctx,
                unsigned *out_w, unsigned *out_h);

#endif /* OWOS_UPNG_H */
