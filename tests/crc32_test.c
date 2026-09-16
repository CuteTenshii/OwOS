/* crc32_test.c — host unit test for the shared zlib/gzip/PNG CRC-32.
 *
 * Validates the canonical value, the incremental-chaining property the PNG
 * encoder relies on, and that the new zlib-style chain reproduces the old
 * "init 0xFFFFFFFF, finalize ^all-ones" png_crc/crc_acc scheme byte-for-byte.
 *
 *   cc -std=c11 -Iuser/lib/zlib tests/crc32_test.c user/lib/zlib/crc32.c \
 *      -o /tmp/crc32_test && /tmp/crc32_test
 */
#include <stdint.h>
#define OWOS_SUITE_TEST
#include <stdio.h>
#include <string.h>
#include "crc32.h"

/* The pre-refactor png.c helpers, verbatim, to cross-check the new chain. */
static uint32_t old_table[256];
static void old_build(void)
{ for (uint32_t i = 0; i < 256; i++) { uint32_t c = i;
	for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
	old_table[i] = c; } }
static uint32_t old_acc(uint32_t c, const uint8_t *d, uint32_t n)
{ for (uint32_t i = 0; i < n; i++) c = old_table[(c ^ d[i]) & 0xFF] ^ (c >> 8); return c; }

int main(void)
{
	old_build();

	/* 1. The universal CRC-32 check value. */
	test("canonical check value");
	expect(crc32(0, "123456789", 9) == 0xCBF43926u);

	/* 2. Empty input is the seed. */
	test("empty input");
	expect(crc32(0, 0, 0) == 0u);

	/* 3. Incremental chaining == one-shot over the concatenation. */
	test("incremental chaining");
	const char *A = "hello, ", *B = "world";
	char cat[32]; strcpy(cat, A); strcat(cat, B);
	uint32_t chained = crc32(crc32(0, A, (uint32_t)strlen(A)), B, (uint32_t)strlen(B));
	uint32_t oneshot = crc32(0, cat, (uint32_t)strlen(cat));
	expect(chained == oneshot);

	/* 4. The new png chain reproduces the old init/finalize scheme. A PNG IHDR:
	 *    CRC over chunk type "IHDR" + the 13 data bytes. */
	test("PNG IHDR compatibility");
	uint8_t type[4] = { 'I','H','D','R' };
	uint8_t data[13] = { 0,0,1,0, 0,0,0,1, 8,2,0,0,0 };
	uint32_t oldv = old_acc(old_acc(0xFFFFFFFFu, type, 4), data, 13) ^ 0xFFFFFFFFu;
	uint32_t newv = crc32(crc32(0, type, 4), data, 13);
	expect(newv == oldv);

	/* 5. And a multi-segment accumulation matches a single pass. */
	test("multi-segment accumulation");
	uint32_t seg = crc32(crc32(crc32(0, type, 4), data, 8), data + 8, 5);
	uint8_t all[17]; memcpy(all, type, 4); memcpy(all + 4, data, 13);
	expect(seg == crc32(0, all, 17));

	return 0;
}
