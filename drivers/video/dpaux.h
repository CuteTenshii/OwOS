/* dpaux.h: DisplayPort AUX channel protocol (DPCD reads, EDID over I2C-over-AUX), independent of the transport. */
#ifndef OWOS_DPAUX_H
#define OWOS_DPAUX_H

#include <stdint.h>

#define DP_EDID_LEN 128

/* Send one AUX request (header + payload) and store the reply in rx. Returns the reply length, or < 0 on failure. */
typedef int (*dp_aux_fn)(void *ctx, const uint8_t *tx, int txlen, uint8_t *rx, int rxmax);

/* Read len bytes of DPCD starting at addr. 0 on success, < 0 on failure. */
int dp_read_dpcd(dp_aux_fn xfer, void *ctx, uint32_t addr, uint8_t *buf, int len);

/* True if the DPCD receiver capabilities (revision, max lane count) describe a real sink. */
int dp_dpcd_valid(const uint8_t caps[3]);

/* Read EDID block number block (128 bytes) from the sink's DDC at I2C 0x50, tunneled over AUX. Blocks 2 and up are
 * reached through the E-DDC segment pointer. 0 on success, < 0 on failure. */
int dp_read_edid(dp_aux_fn xfer, void *ctx, int block, uint8_t *edid);

#endif /* OWOS_DPAUX_H */
