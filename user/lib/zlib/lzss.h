/* lzss.h — OwOS-native LZSS codec for the zip/unzip commands.
 *
 * A port of kernel/zip.c: a self-contained LZ77-family codec, byte-stream
 * in / byte-stream out, with an 8-byte "OWOZ" + original-size header. Its hash
 * tables are file-static scratch (the codec runs single-threaded from the
 * terminal), so it needs no allocator.
 */
#ifndef OWOS_USER_LZSS_H
#define OWOS_USER_LZSS_H

#include <stdint.h>

#define ZIP_HEADER 8 /* magic (4) + original size (4) */

uint32_t zip_bound(uint32_t src_len);
int zip_compress(const uint8_t *src, uint32_t slen, uint8_t *dst, uint32_t dcap);
int zip_decompress(const uint8_t *src, uint32_t slen, uint8_t *dst, uint32_t dcap);
int zip_orig_size(const uint8_t *src, uint32_t slen);

#endif /* OWOS_USER_LZSS_H */
