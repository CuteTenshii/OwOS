/* Host test for the XML reader (user/lib/xml/xml.c). */
#define OWOS_SUITE_TEST
#include "test.h"
#include "xml.h"
#include <string.h>

#define A "urn:a"
#define B "urn:b"

static const char *DOC = "<?xml version=\"1.0\"?><r xmlns=\"" A "\" xmlns:b=\"" B "\">"
                         "<v>def</v><b:v>pfx</b:v><x xmlns=\"" B "\"><v>inner</v></x>"
                         "<c:v xmlns:c=\"" A "\">cpfx</c:v><u:v>unbound</u:v></r>";

static void test_unqualified_match_ignores_namespaces(void)
{
  struct xml_span d = xml_all(DOC), el;
  int n = 0;
  while (xml_next(&d, "v", &el))
    n++;
  expect_eq(n, 5);
}

static void test_default_namespace_and_prefix_resolve(void)
{
  char out[16];
  expect_eq(xml_text_ns(xml_all(DOC), A, "v", out, sizeof out), 3);
  expect(!strcmp(out, "def"));
  expect_eq(xml_text_ns(xml_all(DOC), B, "v", out, sizeof out), 3);
  expect(!strcmp(out, "pfx"));
}

static void test_nested_redeclaration_and_sibling_walk(void)
{
  struct xml_span d = xml_all(DOC), el;
  char out[16];
  int n = 0;
  while (xml_next_ns(&d, B, "v", &el))
    n++;
  expect_eq(n, 2);
  d = xml_all(DOC);
  expect(xml_next_ns(&d, A, "v", &el));
  expect(xml_next_ns(&d, A, "v", &el));
  expect_eq(xml_text_ns(el, A, "v", out, sizeof out), -1);
  expect(!strncmp(el.s, "cpfx", 4));
}

static void test_declaration_on_ancestor_outside_the_span(void)
{
  struct xml_span d = xml_all(DOC), x, scan, el;
  char out[16];
  expect(xml_next_ns(&d, B, "x", &x));
  scan = x;
  expect(xml_next_ns(&scan, B, "v", &el));
  expect_eq(el.e - el.s, 5);
  expect_eq(xml_text_ns(x, B, "v", out, sizeof out), 5);
  expect(!strcmp(out, "inner"));
  expect_eq(xml_text_ns(x, A, "v", out, sizeof out), -1);
}

static void test_unbound_prefix_and_no_namespace(void)
{
  struct xml_span d = xml_all(DOC), el;
  expect(xml_next_ns(&d, "", "v", &el));
  expect(!strncmp(el.s, "unbound", 7));
  expect(!xml_next_ns(&d, "", "v", &el));
  d = xml_all("<a><v>1</v></a>");
  expect(xml_next_ns(&d, "", "v", &el));
  d = xml_all("<a><v>1</v></a>");
  expect(!xml_next_ns(&d, A, "v", &el));
}

static void test_same_name_nesting_and_self_closing(void)
{
  struct xml_span d = xml_all("<a xmlns=\"" A "\"><a><a/></a><a>z</a></a>"), el;
  expect(xml_next_ns(&d, A, "a", &el));
  expect_eq(el.e - el.s, 19);
  d = xml_all("<a xmlns='" A "'><i/></a>");
  expect(xml_next_ns(&d, A, "i", &el));
  expect_eq(el.e - el.s, 0);
}

int main(void)
{
  test("unqualified match ignores namespaces", test_unqualified_match_ignores_namespaces);
  test("default namespace and prefixes resolve", test_default_namespace_and_prefix_resolve);
  test("nested redeclaration and sibling walk", test_nested_redeclaration_and_sibling_walk);
  test("declaration on an ancestor outside the span", test_declaration_on_ancestor_outside_the_span);
  test("unbound prefix and no namespace", test_unbound_prefix_and_no_namespace);
  test("same-name nesting and self-closing", test_same_name_nesting_and_self_closing);
  return 0;
}
