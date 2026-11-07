#ifndef OWOS_CRC32C_H
#define OWOS_CRC32C_H

#include <stdint.h>

/* CRC-32C (poly 0x82F63B78), raw: no pre/post inversion. */
uint32_t crc32c_raw(uint32_t crc, const void *buf, uint32_t len);

#endif
