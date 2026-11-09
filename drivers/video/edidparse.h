/* edidparse.h: EDID block validation and preferred-timing extraction across the base block, CTA-861 and DisplayID
 * 1.x extension blocks, plus HDMI capability flags. Pure functions, no hardware access. */
#ifndef OWOS_EDIDPARSE_H
#define OWOS_EDIDPARSE_H

#include <stdint.h>

#define EDID_BLOCK 128
#define EDID_MAX_BLOCKS 4

struct edid_timing {
  int hz, w, h;
  int mm; /* physical width in mm, 0 if unknown */
};

struct edid_cta {
  int hdmi;         /* an HDMI vendor-specific data block is present */
  int audio;        /* basic audio flag of a CTA-861 block */
  int max_tmds_mhz; /* highest advertised TMDS clock/character rate, 0 if none */
};

/* True if the 128 bytes of a block sum to 0 mod 256. */
int edid_checksum_ok(const uint8_t *block);

/* Preferred timing of an EDID of nblocks blocks. The base block's first detailed timing wins; when it is a
 * placeholder (zero pixel clock, as on panels beyond its 16-bit limits) the DisplayID preferred timing is used, then
 * the first CTA-861 detailed timing. Extension blocks with a bad checksum are ignored. 0 on success, -1 if none. */
int edid_preferred_timing(const uint8_t *edid, int nblocks, struct edid_timing *t);

/* HDMI capabilities from the CTA-861 extension blocks (revision 3+, valid checksum). All zero for a DVI-style sink. */
void edid_cta_info(const uint8_t *edid, int nblocks, struct edid_cta *c);

#endif /* OWOS_EDIDPARSE_H */
