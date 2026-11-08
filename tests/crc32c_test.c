/* Host test for CRC-32C (kernel/crc32c.c). */
#define OWOS_SUITE_TEST
#include "../kernel/crc32c.h"
#include "test.h"
#include <string.h>

static void check_canonical(void) { expect((crc32c_raw(0xFFFFFFFFu, "123456789", 9) ^ 0xFFFFFFFFu) == 0xE3069283u); }

static void check_iscsi_vectors(void)
{
  unsigned char z[32], f[32];
  memset(z, 0, sizeof z);
  memset(f, 0xFF, sizeof f);
  expect((crc32c_raw(0xFFFFFFFFu, z, 32) ^ 0xFFFFFFFFu) == 0x8A9136AAu);
  expect((crc32c_raw(0xFFFFFFFFu, f, 32) ^ 0xFFFFFFFFu) == 0x62A8AB43u);
}

static void check_chaining(void)
{
  unsigned char b[100];
  for (int i = 0; i < 100; i++)
    b[i] = (unsigned char)(i * 7 + 3);
  uint32_t whole = crc32c_raw(0xFFFFFFFFu, b, 100);
  for (int cut = 0; cut <= 100; cut += 9) {
    uint32_t c = crc32c_raw(0xFFFFFFFFu, b, (uint32_t)cut);
    expect(crc32c_raw(c, b + cut, (uint32_t)(100 - cut)) == whole);
  }
}

int main(void)
{
  test("check value of the canonical string", check_canonical);
  test("32 zero bytes and 32 0xFF bytes (iSCSI vectors)", check_iscsi_vectors);
  test("chaining equals one shot across the 8-byte fast path boundary", check_chaining);
  return 0;
}
