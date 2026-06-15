/* upng.h — a streaming PNG decoder for user programs.
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

/* ---------------------------------------------------------- image metadata */
/*
 * A read-only properties view of a PNG, for an "image info" panel: the IHDR
 * geometry/colour fields plus the ancillary chunks a PNG carries metadata in
 * (tRNS transparency, pHYs resolution, gAMA gamma, tIME timestamp, the tEXt /
 * iTXt text records, and the eXIf EXIF block). upng_probe reads only the chunks
 * up to the first IDAT — where these all conventionally live — so it touches a
 * few KiB of header, never the (possibly multi-MiB) pixel data; a rare metadata
 * chunk placed after IDAT is not reported. The eXIf bytes are buffered verbatim;
 * decode them with upng_exif_emit, which walks the TIFF/EXIF structure and
 * reports one human-readable tag at a time.
 */
#define UPNG_TEXT_MAX 16     /* most tEXt/iTXt records captured                 */
#define UPNG_KEY_MAX  40     /* per-record keyword cap (incl. NUL)              */
#define UPNG_VAL_MAX  128    /* per-record value cap (incl. NUL)               */
#define UPNG_EXIF_MAX 16384  /* eXIf bytes we'll buffer for upng_exif_emit      */

struct upng_text {
	char key[UPNG_KEY_MAX];
	char val[UPNG_VAL_MAX];
};

struct upng_meta {
	unsigned w, h;
	unsigned depth;            /* IHDR bit depth per sample (1/2/4/8/16)        */
	unsigned color;            /* IHDR colour type: 0/2/3/4/6                    */
	unsigned channels;         /* samples per pixel implied by `color`          */
	int      interlaced;       /* 1 if Adam7-interlaced                          */
	int      has_alpha;        /* colour type carries an alpha channel (4/6)    */
	int      has_trns;         /* a tRNS chunk is present                        */
	unsigned palette_n;        /* PLTE entries (0 if not a palette image)       */
	int      have_phys;        /* pHYs present                                   */
	unsigned ppu_x, ppu_y;     /* pHYs pixels per unit, X and Y                  */
	int      phys_metre;       /* pHYs unit == 1 (metre) -> ppu is per metre     */
	int      have_gamma;       /* gAMA present                                   */
	unsigned gamma;            /* gAMA value, file-encoded (gamma * 100000)      */
	int      have_time;        /* tIME present                                   */
	unsigned year, mon, day, hour, min, sec;
	int      n_text;           /* captured tEXt/iTXt records                     */
	struct upng_text text[UPNG_TEXT_MAX];
	int      exif_len;         /* eXIf bytes in `exif` (0 none, -1 over-cap)     */
	unsigned char exif[UPNG_EXIF_MAX];
};

/* Fill *m from the stream. Returns UPNG_OK once IHDR is read (later chunks are
 * best-effort and never fail the call), or a UPNG_E* code if the signature/IHDR
 * is bad. *m is fully zeroed first. Large — give it a static, not a stack slot. */
int upng_probe(upng_reader rd, void *rctx, struct upng_meta *m);

/* One decoded EXIF tag: a human label and a formatted value string. */
typedef void (*upng_kv)(void *ctx, const char *key, const char *val);

/* Walk the TIFF/EXIF block captured in upng_meta.exif (`len` = meta.exif_len),
 * invoking emit() for each recognised IFD0 / Exif-sub-IFD tag. All offsets are
 * bounds-checked; an unparsable block simply emits nothing. */
void upng_exif_emit(const unsigned char *exif, int len, upng_kv emit, void *ctx);

/* Stream-decode, invoking row() per scanline. Fills out_w / out_h (the width and
 * height pointers) if non-NULL. Returns UPNG_OK (incl. caller early-stop) or a
 * UPNG_E* code. */
int upng_decode(upng_reader rd, void *rctx, upng_row row, void *row_ctx,
                unsigned *out_w, unsigned *out_h);

#endif /* OWOS_UPNG_H */
