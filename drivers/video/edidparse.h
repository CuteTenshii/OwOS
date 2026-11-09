/* edidparse.h: EDID block validation and preferred-timing extraction across the base block, CTA-861 and DisplayID
 * 1.x extension blocks. Pure functions, no hardware access. */
#ifndef OWOS_EDIDPARSE_H
#define OWOS_EDIDPARSE_H

#include <stdint.h>

#define EDID_BLOCK 128
#define EDID_MAX_BLOCKS 4

struct edid_timing {
  int hz, w, h;
  int mm; /* physical width in mm, 0 if unknown */
};

/* True if the 128 bytes of a block sum to 0 mod 256. */
int edid_checksum_ok(const uint8_t *block);

/* Preferred timing of an EDID of nblocks blocks. The base block's first detailed timing wins; when it is a
 * placeholder (zero pixel clock, as on panels beyond its 16-bit limits) the DisplayID preferred timing is used, then
 * the first CTA-861 detailed timing. Extension blocks with a bad checksum are ignored. 0 on success, -1 if none. */
int edid_preferred_timing(const uint8_t *edid, int nblocks, struct edid_timing *t);

#endif /* OWOS_EDIDPARSE_H */
