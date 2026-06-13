/* endian.h — explicit little/big-endian byte loads, shared across the kernel.
 *
 * Every driver, filesystem and wire protocol used to carry its own copy of
 * these (le32/be32/get32/rd32/get_u32/u32 …). They were byte-identical within
 * an endianness but collided by name across files — peer.c's get32() was
 * big-endian while smb.c's was little-endian. Centralising them here kills the
 * duplication *and* the name collision: the endianness is now spelled out in
 * the call. Store helpers (store_le32 …) stay local where they're needed; this
 * header is loads only. */
#ifndef OWOS_ENDIAN_H
#define OWOS_ENDIAN_H

#include <stdint.h>

static inline uint16_t load_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t load_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t load_le64(const uint8_t *p)
{
	return (uint64_t)load_le32(p) | ((uint64_t)load_le32(p + 4) << 32);
}

static inline uint16_t load_be16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t load_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline uint64_t load_be64(const uint8_t *p)
{
	return ((uint64_t)load_be32(p) << 32) | load_be32(p + 4);
}

#endif /* OWOS_ENDIAN_H */
