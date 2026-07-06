/* crc32.c — CRC-32 (reflected, poly 0xEDB88820), the zlib/gzip/PNG checksum.
 * One shared user-space copy for the DEFLATE codec (deflate.c, gzip trailer) and
 * the STUN codec (stun.c, FINGERPRINT), replacing the statics each used to
 * carry. Its real-world home is zlib, so it lives here. See crc32.h. */

#include "crc32.h"

static uint32_t crc_table[256];
static int      crc_ready;

static void crc_build(void)
{
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;
		for (int k = 0; k < 8; k++)
			c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
		crc_table[i] = c;
	}
	crc_ready = 1;
}

uint32_t crc32(uint32_t crc, const void *buf, uint32_t len)
{
	const uint8_t *d = buf;
	if (!crc_ready)
		crc_build();
	crc ^= 0xFFFFFFFFu;
	for (uint32_t i = 0; i < len; i++)
		crc = crc_table[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
	return crc ^ 0xFFFFFFFFu;
}
