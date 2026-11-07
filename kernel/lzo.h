/* lzo.h: bounds-checked LZO1X decompressor (the format btrfs uses for compress=lzo). */
#ifndef OWOS_LZO_H
#define OWOS_LZO_H

#include <stdint.h>

/* Decode one LZO1X stream (ending in the EOS marker, consuming all of src) into dst (capacity dcap). Returns 0 and
 * sets *dlen, or -1 on any corrupt, truncated or oversized stream. */
int lzo1x_unpack(const uint8_t *src, uint32_t slen, uint8_t *dst, uint32_t dcap, uint32_t *dlen);

#endif
