/* Host test for user/lib/uuid: RFC 9562 test vectors, parsing forms, and the generators' version bits. */
#define OWOS_SUITE_TEST
#define rng_bytes uuid_test_rng_bytes /* this suite supplies its own clock and entropy */
#define now_ms uuid_test_now_ms
void rng_bytes(void *out, int n);
long long now_ms(void);
#include "../user/lib/uuid/gen.c"
#include "test.h"
#include "uuid.h"
#include <string.h>

long long now_ms(void) { return 0x0123456789ABLL; }
void rng_bytes(void *out, int n) { memset(out, 0xFF, (size_t)n); }

static void text(const uuid_t u, char *out) { uuid_to_string(u, out, 0); }

static void test_v5_matches_rfc_vectors(void)
{
  uuid_t u;
  char s[UUID_STR_LEN];
  uuid_v5(u, UUID_NS_DNS, "www.example.com", 15);
  text(u, s);
  expect_string(s, "2ed6657d-e927-568b-95e1-2665a8aea6a2"); /* RFC 9562 appendix A.4 */
  expect_eq(uuid_version(u), 5);
  expect_eq(uuid_variant(u), 2);
  uuid_v5(u, UUID_NS_URL, "http://www.example.com", 22);
  text(u, s);
  expect_string(s, "2d32a3f1-fac2-52a6-ac52-fe9e87e628ea"); /* python3 uuid.uuid5(NAMESPACE_URL, ...) */
}

static void test_v4_and_v7_set_version_and_variant(void)
{
  uuid_t u;
  char s[UUID_STR_LEN];
  uuid_generate_v4(u);
  text(u, s);
  expect_string(s, "ffffffff-ffff-4fff-bfff-ffffffffffff");
  uuid_generate_v7(u);
  text(u, s);
  expect_string(s, "01234567-89ab-7fff-bfff-ffffffffffff");
  expect_eq(uuid_version(u), 7);
  expect(uuid_v7_time(u) == 0x0123456789ABULL);
}

static void test_v7_orders_by_time(void)
{
  unsigned char r[10] = {0};
  uuid_t a, b;
  uuid_v7_from(a, 1000, r);
  uuid_v7_from(b, 1001, r);
  expect(uuid_compare(a, b) < 0);
  expect(uuid_compare(b, a) > 0);
  expect_eq(uuid_compare(a, a), 0);
}

static void test_parse_forms(void)
{
  uuid_t want, got;
  char s[UUID_STR_LEN];
  static const char *const ok[] = {"2ED6657D-E927-568B-95E1-2665A8AEA6A2", "2ed6657de927568b95e12665a8aea6a2",
                                   "{2ed6657d-e927-568b-95e1-2665a8aea6a2}",
                                   "urn:uuid:2ed6657d-e927-568b-95e1-2665a8aea6a2"};
  uuid_v5(want, UUID_NS_DNS, "www.example.com", 15);
  for (unsigned i = 0; i < sizeof ok / sizeof ok[0]; i++) {
    memset(got, 0, 16);
    expect_eq(uuid_parse(ok[i], got), 0);
    expect_eq(uuid_compare(got, want), 0);
  }
  uuid_to_string(want, s, 1);
  expect_string(s, "2ED6657D-E927-568B-95E1-2665A8AEA6A2");
}

static void test_parse_rejects_garbage(void)
{
  static const char *const bad[] = {"",
                                    "2ed6657d-e927-568b-95e1-2665a8aea6a",   /* short */
                                    "2ed6657d-e927-568b-95e1-2665a8aea6a22", /* long */
                                    "2ed6657de927-568b-95e1-2665a8aea6a2",   /* some dashes missing */
                                    "2ed6657d_e927-568b-95e1-2665a8aea6a2",
                                    "2ed6657d-e927-568b-95e1-2665a8aea6zz",
                                    "{2ed6657d-e927-568b-95e1-2665a8aea6a2",
                                    "2ed6657d-e927-568b-95e1-2665a8aea6a2}"};
  uuid_t u;
  memset(u, 7, 16);
  for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++)
    expect_eq(uuid_parse(bad[i], u), -1);
  expect(u[0] == 7 && u[15] == 7); /* untouched on failure */
  expect_eq(uuid_parse(0, u), -1);
}

static void test_nil(void)
{
  uuid_t u;
  memset(u, 0, 16);
  expect(uuid_is_nil(u));
  u[15] = 1;
  expect(!uuid_is_nil(u));
}

int main(void)
{
  test("v5 matches the RFC vectors", test_v5_matches_rfc_vectors);
  test("v4 and v7 set version and variant", test_v4_and_v7_set_version_and_variant);
  test("v7 orders by time", test_v7_orders_by_time);
  test("parse accepts every common form", test_parse_forms);
  test("parse rejects malformed text", test_parse_rejects_garbage);
  test("nil", test_nil);
  return 0;
}
