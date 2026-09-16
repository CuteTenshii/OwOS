/* woff.h — WOFF 1.0 + WOFF2 web-font decoders.
 *
 * Both formats are SFNT (TrueType/OpenType) fonts in a compressed container:
 * WOFF1 zlib-compresses each table, WOFF2 brotli-compresses the whole table set
 * and additionally re-encodes the glyf/loca (and optionally hmtx) tables. These
 * functions undo the container and return a plain SFNT buffer the font parser
 * (user/lib/font) can load directly.
 *
 * Whole-buffer: take the full WOFF/WOFF2 file, return a malloc'd SFNT of
 * *out_len bytes (caller frees), or NULL on a malformed input / OOM. */
#ifndef OWOS_WOFF_H
#define OWOS_WOFF_H

/* 1 if `data` begins with a WOFF ('wOFF') or WOFF2 ('wOF2') signature. */
int woff_is_woff(const unsigned char *data, int len);

/* Decode a WOFF/WOFF2 font to a raw SFNT. Returns NULL if `data` is not a WOFF
 * container (the caller should then treat it as an SFNT already) or on error.
 */
unsigned char *woff_to_sfnt(const unsigned char *data, int len, int *out_len);

#endif /* OWOS_WOFF_H */
