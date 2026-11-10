/* Host test for user/lib/plist: parses reference XML and binary plists written by Python's plistlib, round-trips
 * both writers, and rejects malformed input. */
#define OWOS_SUITE_TEST
#include "plist.h"
#include "plist_vectors.h"
#include "test.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Check a parsed tree against the structure plist_vectors.h encodes */
static void check_reference(const plist *d)
{
  const plist *v;
  double n;
  expect(d && d->type == PLIST_DICT);
  expect_eq(plist_count(d), 14);
  expect_string(plist_get_string(plist_dict_get(d, "name")), "Wi-Fi");
  v = plist_dict_get(d, "int");
  expect(v && v->type == PLIST_INT);
  expect_eq(v->u.integer, 42);
  expect_eq(plist_dict_get(d, "neg")->u.integer, -7);
  expect_eq(plist_dict_get(d, "big")->u.integer, 1099511627776LL);
  expect(plist_get_number(plist_dict_get(d, "real"), &n) && n == 1.5);
  expect_eq(plist_dict_get(d, "real")->type, PLIST_REAL);
  expect_eq(plist_get_bool(plist_dict_get(d, "t"), 0), 1);
  expect(plist_dict_get(d, "f")->type == PLIST_BOOL && !plist_dict_get(d, "f")->u.boolean);
  v = plist_dict_get(d, "date");
  expect(v && v->type == PLIST_DATE);
  expect(v->u.real == 599627045.0); /* 2020-01-02T03:04:05Z in seconds since 2001 */
  v = plist_dict_get(d, "data");
  expect(v && v->type == PLIST_DATA && v->u.str.len == 3 && !memcmp(v->u.str.data, "\x00\x01\xff", 3));
  expect_string(plist_get_string(plist_dict_get(d, "uni")), "h\xc3\xa9llo \xe2\x98\x83 \xf0\x9d\x84\x9e & <tag>");
  v = plist_dict_get(d, "list");
  expect(v && v->type == PLIST_ARRAY && plist_count(v) == 3);
  expect_eq(plist_array_get(v, 0)->u.integer, 1);
  expect_string(plist_get_string(plist_array_get(v, 1)), "two");
  expect(plist_array_get(plist_array_get(v, 2), 0)->u.real == 3.25);
  expect(plist_dict_get(d, "empty")->type == PLIST_DICT && plist_count(plist_dict_get(d, "empty")) == 0);
  expect(plist_dict_get(d, "emptyl")->type == PLIST_ARRAY && plist_count(plist_dict_get(d, "emptyl")) == 0);
  expect_eq((int)strlen(plist_get_string(plist_dict_get(d, "long"))), 40);
  expect(plist_dict_get(d, "missing") == 0);
}

static void test_parse_reference_xml(void)
{
  plist *d = plist_parse(plist_ref_xml, sizeof plist_ref_xml, 0);
  check_reference(d);
  plist_free(d);
}

static void test_parse_reference_binary(void)
{
  plist *d = plist_parse(plist_ref_bin, sizeof plist_ref_bin, 0);
  check_reference(d);
  plist_free(d);
}

static void test_xml_round_trip(void)
{
  plist *a = plist_parse(plist_ref_bin, sizeof plist_ref_bin, 0), *b;
  int len;
  char *xml = plist_to_xml(a, &len);
  expect(xml != 0 && (int)strlen(xml) == len);
  b = plist_parse(xml, len, 0);
  check_reference(b);
  free(xml);
  plist_free(a);
  plist_free(b);
}

static void test_binary_round_trip(void)
{
  plist *a = plist_parse(plist_ref_xml, sizeof plist_ref_xml, 0), *b;
  int len;
  unsigned char *bin = plist_to_bin(a, &len);
  expect(bin != 0 && len > 40 && !memcmp(bin, "bplist00", 8));
  b = plist_parse(bin, len, 0);
  check_reference(b);
  free(bin);
  plist_free(a);
  plist_free(b);
}

static void test_builders_and_entities(void)
{
  plist *d = plist_new_dict(), *back;
  char *xml;
  int len;
  expect(plist_dict_set(d, "k", plist_new_string("a<b>&\"c\"")) == 0);
  expect(plist_dict_set(d, "k", plist_new_int(5)) == 0); /* replaces */
  expect(plist_dict_set(d, "s", plist_new_string("a<b>&\"c\"")) == 0);
  expect_eq(plist_count(d), 2);
  expect_eq(plist_dict_get(d, "k")->u.integer, 5);
  xml = plist_to_xml(d, &len);
  expect(strstr(xml, "a&lt;b&gt;&amp;\"c\"") != 0);
  back = plist_parse(xml, len, 0);
  expect_string(plist_get_string(plist_dict_get(back, "s")), "a<b>&\"c\"");
  free(xml);
  plist_free(back);
  expect(plist_dict_set(0, "x", plist_new_int(1)) == -1); /* frees the value */
  plist_free(d);
}

static void test_xml_extras(void)
{
  static const char doc[] = "<?xml version=\"1.0\"?><!-- c --><plist version=\"1.0\"><dict><key>a</key><string/>"
                            "<key>b</key><array><true/><false/><integer>0x10</integer><real>-2.5e1</real></array>"
                            "<key>c</key><string>&#65;&#x42;<![CDATA[<raw>]]></string></dict></plist>";
  plist *d = plist_parse(doc, sizeof doc - 1, 0);
  plist *arr = plist_dict_get(d, "b");
  expect_string(plist_get_string(plist_dict_get(d, "a")), "");
  expect_eq(plist_array_get(arr, 2)->u.integer, 16);
  expect(plist_array_get(arr, 3)->u.real == -25.0);
  expect_string(plist_get_string(plist_dict_get(d, "c")), "AB<raw>");
  plist_free(d);
}

static void test_rejects_malformed(void)
{
  plist_error err;
  unsigned char bad[sizeof plist_ref_bin];
  expect(plist_parse("", 0, &err) == 0);
  expect(plist_parse("<plist><dict><key>a</key></dict></plist>", 41, &err) == 0);
  expect(plist_parse("<plist><integer>1</string></plist>", 34, &err) == 0);
  expect(plist_parse("<plist><wat/></plist>", 21, &err) == 0 && err.msg[0]);
  expect(plist_parse("bplist00", 8, &err) == 0);
  memcpy(bad, plist_ref_bin, sizeof bad);
  bad[sizeof bad - 9] = 0xff; /* top object number out of range */
  expect(plist_parse(bad, sizeof bad, &err) == 0);
  for (int cut = 9; cut < (int)sizeof plist_ref_bin; cut += 7) /* truncation must never crash */
    plist_free(plist_parse(plist_ref_bin, cut, 0));
  for (int i = 8; i < (int)sizeof plist_ref_bin - 32; i++) { /* nor must single-byte corruption */
    memcpy(bad, plist_ref_bin, sizeof bad);
    bad[i] ^= 0xa5;
    plist_free(plist_parse(bad, sizeof bad, 0));
  }
}

int main(void)
{
  test("parse reference XML", test_parse_reference_xml);
  test("parse reference binary", test_parse_reference_binary);
  test("XML writer round-trips", test_xml_round_trip);
  test("binary writer round-trips", test_binary_round_trip);
  test("builders and escaping", test_builders_and_entities);
  test("XML entities, CDATA, empty elements", test_xml_extras);
  test("malformed input is rejected, not crashed on", test_rejects_malformed);
  return 0;
}
