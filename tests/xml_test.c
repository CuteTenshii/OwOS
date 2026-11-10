/* Host test for the XML element reader and escaper (user/lib/xml/xml.c). */
#define OWOS_SUITE_TEST
#include "test.h"
#include "xml.h"
#include <string.h>

static void test_text_ignores_namespace_prefix(void)
{
  char out[32];
  expect_eq(xml_text(xml_all("<s:Body><u:Ret xmlns:u=\"x\">42</u:Ret></s:Body>"), "Ret", out, sizeof out), 2);
  expect_string(out, "42");
}

static void test_text_decodes_entities_and_cdata(void)
{
  char out[64];
  expect_eq(xml_text(xml_all("<a>x &amp; &lt;y&gt; &quot;z&apos;</a>"), "a", out, sizeof out), 11);
  expect_string(out, "x & <y> \"z'");
  expect(xml_text(xml_all("<a>1<![CDATA[<2>&]]>3</a>"), "a", out, sizeof out) == 6);
  expect_string(out, "1<2>&3");
}

static void test_text_stops_at_first_child_and_handles_empty(void)
{
  char out[32];
  expect_eq(xml_text(xml_all("<a>head<b>inner</b>tail</a>"), "a", out, sizeof out), 4);
  expect_string(out, "head");
  expect_eq(xml_text(xml_all("<a/>"), "a", out, sizeof out), 0);
  expect_eq(xml_text(xml_all("<a></a>"), "a", out, sizeof out), 0);
}

static void test_text_reports_missing_and_overflow(void)
{
  char out[4];
  expect_eq(xml_text(xml_all("<a>x</a>"), "b", out, sizeof out), -1);
  expect_eq(xml_text(xml_all("<a>toolong</a>"), "a", out, sizeof out), -1);
}

static void test_next_walks_siblings_and_balances_nesting(void)
{
  struct xml_span scan = xml_all("<l><i>1</i><i>2<i>x</i></i><!-- <i>c</i> --><i/></l>"), in;
  expect(xml_next(&scan, "i", &in));
  expect_eq(in.e - in.s, 1);
  expect_eq(*in.s, '1');
  expect(xml_next(&scan, "i", &in));
  expect_eq(in.e - in.s, 9);
  expect(strncmp(in.s, "2<i>x</i>", 9) == 0);
  expect(xml_next(&scan, "i", &in));
  expect_eq(in.e - in.s, 0);
  expect(!xml_next(&scan, "i", &in));
}

static void test_attribute_values_may_contain_gt(void)
{
  char out[8];
  expect_eq(xml_text(xml_all("<a t=\"1>2\">ok</a>"), "a", out, sizeof out), 2);
  expect_string(out, "ok");
}

static void test_name_must_match_whole_local_name(void)
{
  char out[8];
  expect_eq(xml_text(xml_all("<serviceType>x</serviceType>"), "service", out, sizeof out), -1);
}

static void test_escape(void)
{
  char out[32];
  expect_eq(xml_escape(out, 0, sizeof out, "a&b<c>\"d'"), 24);
  expect_string(out, "a&amp;b&lt;c&gt;&quot;d'");
  expect_eq(xml_escape(out, 0, 5, "&&"), -1);
}

int main(void)
{
  test("text ignores the namespace prefix", test_text_ignores_namespace_prefix);
  test("text decodes entities and CDATA", test_text_decodes_entities_and_cdata);
  test("text stops at the first child and handles empty elements", test_text_stops_at_first_child_and_handles_empty);
  test("text reports a missing element and overflow", test_text_reports_missing_and_overflow);
  test("next walks siblings and balances same-name nesting", test_next_walks_siblings_and_balances_nesting);
  test("attribute values may contain >", test_attribute_values_may_contain_gt);
  test("names match whole", test_name_must_match_whole_local_name);
  test("escape covers & < > and quotes, and reports overflow", test_escape);
  return 0;
}
