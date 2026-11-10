/* Host test for the XML tree decoder and encoder (user/lib/xml/xmltree.c). */
#define OWOS_SUITE_TEST
#include "test.h"
#include "xmltree.h"
#include <stdlib.h>
#include <string.h>

static struct xml_node *parse(const char *s)
{
  struct xml_error e;
  struct xml_node *d = xml_parse(s, (int)strlen(s), &e);
  expect(d != 0);
  return d;
}

static void bad(const char *s, const char *msg)
{
  struct xml_error e;
  struct xml_node *d = xml_parse(s, (int)strlen(s), &e);
  expect(d == 0);
  if (!d)
    expect_string(e.msg, msg);
}

static void test_elements_attributes_and_text(void)
{
  struct xml_node *d = parse(
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!-- c -->\n<Root a='1' B=\"x &amp; y\">hi <b/>there</Root>\n");
  struct xml_node *r = xml_find(d, 0, "Root");
  expect(r != 0);
  expect_string(xml_attr(r, "a"), "1");
  expect_string(xml_attr(r, "B"), "x & y");
  expect_eq(d->first->type, XML_COMMENT);
  char *t = xml_text_content(r);
  expect_string(t, "hi there");
  free(t);
  expect(xml_find(r, 0, "b") != 0);
  xml_free(d);
}

static void test_references_cdata_and_line_ends(void)
{
  struct xml_node *d = parse("<a v=\"1\t2\r\n3\">&lt;&#65;&#x42;&#x1F600;<![CDATA[<&>]]>\r\nx</a>");
  struct xml_node *a = xml_find(d, 0, "a");
  expect_string(xml_attr(a, "v"), "1 2 3");
  expect_eq(a->first->type, XML_TEXT);
  expect_string(a->first->value, "<AB\xF0\x9F\x98\x80");
  expect_eq(a->first->next->type, XML_CDATA);
  expect_string(a->first->next->value, "<&>");
  expect_string(a->last->value, "\nx");
  xml_free(d);
}

static void test_namespaces_resolve(void)
{
  struct xml_node *d = parse(
      "<r xmlns=\"urn:a\" xmlns:p=\"urn:p\"><p:x p:k=\"1\" k=\"2\"/><y xmlns=\"\"/><z xmlns=\"urn:z\"><w/></z></r>");
  struct xml_node *r = xml_find(d, "urn:a", "r");
  expect(r != 0);
  struct xml_node *x = xml_find(r, "urn:p", "x");
  expect(x != 0);
  expect_string(x->name, "p:x");
  expect_string(x->local, "x");
  expect_string(xml_attr_ns(x, "urn:p", "k"), "1");
  expect_string(xml_attr_ns(x, "", "k"), "2");
  expect(xml_find(r, "", "y") != 0);
  struct xml_node *z = xml_find(r, "urn:z", "z");
  expect(z != 0);
  expect(xml_find(z, "urn:z", "w") != 0);
  expect_string(xml_attr_ns(r, "http://www.w3.org/2000/xmlns/", "p"), "urn:p");
  xml_free(d);
}

static void test_rejects_malformed_input(void)
{
  bad("<a><b></a></b>", "mismatched end tag");
  bad("<a>", "unclosed element");
  bad("", "no root element");
  bad("<a/><b/>", "more than one root element");
  bad("<a x=\"1\" x=\"2\"/>", "duplicate attribute");
  bad("<a x=1/>", "expected a quoted attribute value");
  bad("<a>&nope;</a>", "undefined entity");
  bad("<a>&#0;</a>", "bad character reference");
  bad("<a x=\"<\"/>", "'<' in an attribute value");
  bad("<p:a/>", "unbound namespace prefix");
  bad("<a><!-- x -- y --></a>", "'--' in a comment");
  bad("<a>]]></a>", "']]>' in character data");
  bad("x<a/>", "text outside the root element");
  bad("<?xml version=\"1.0\" encoding=\"UTF-16\"?><a/>", "unsupported encoding");
  bad("<!DOCTYPE a [<!ENTITY e \"x\">]><a/>", "entity declarations are not supported");
  bad("<a><?xml x?></a>", "reserved processing instruction target");
  bad("<a xmlns:q=\"\"/>", "empty namespace declaration");
}

static void test_error_position(void)
{
  struct xml_error e;
  const char *s = "<a>\n <b></c>\n</a>";
  expect(xml_parse(s, (int)strlen(s), &e) == 0);
  expect_eq(e.line, 2);
  expect_eq(e.col, 9);
}

static void test_doctype_and_nesting_limit(void)
{
  struct xml_node *d = parse("<!DOCTYPE html PUBLIC \"-//W3C//DTD>\" \"x\" [ <!-- > --> <!ELEMENT a ANY> ]><a/>");
  expect(xml_find(d, 0, "a") != 0);
  xml_free(d);
  char deep[2048] = "";
  for (int i = 0; i < 300; i++)
    strcat(deep, "<a>");
  bad(deep, "nesting too deep");
}

static void test_roundtrip_and_escaping(void)
{
  const char *src =
      "<a x=\"&lt;&quot;&#10;&#9;&amp;\"><b>t &amp; &lt; &gt;</b><![CDATA[a]]]]><![CDATA[>b]]><!--c--><?pi d?></a>";
  struct xml_node *d = parse(src);
  int n;
  char *out = xml_write(d, 0, &n);
  expect(out != 0);
  expect_string(out, src);
  expect_eq(n, (int)strlen(src));
  free(out);
  xml_free(d);
}

static void test_build_and_pretty_print(void)
{
  struct xml_node *d = xml_new(XML_DOC, 0, 0);
  struct xml_node *r = xml_new_element("feed", "urn:f");
  expect_eq(xml_set_attr(r, "xmlns", 0, "urn:f"), 0);
  expect_eq(xml_set_attr(r, "id", 0, "a\"b"), 0);
  expect_eq(xml_set_attr(r, "id", 0, "2"), 0);
  struct xml_node *e = xml_new_element("entry", "urn:f");
  expect_eq(xml_append(e, xml_new(XML_TEXT, 0, "x<y")), 0);
  expect_eq(xml_append(r, e), 0);
  expect_eq(xml_append(r, xml_new_element("empty", "urn:f")), 0);
  expect_eq(xml_append(d, r), 0);
  char *out = xml_write(d, XML_PRETTY | XML_DECL, 0);
  expect_string(out, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<feed xmlns=\"urn:f\" id=\"2\">\n  "
                     "<entry>x&lt;y</entry>\n  <empty/>\n</feed>\n");
  free(out);
  struct xml_node *back = parse("<feed xmlns=\"urn:f\"><entry/></feed>");
  expect(xml_find(back, "urn:f", "feed") != 0);
  xml_free(back);
  xml_free(d);
}

static void test_append_and_write_refusals(void)
{
  struct xml_node *d = xml_new(XML_DOC, 0, 0), *a = xml_new_element("a", 0), *b = xml_new_element("b", 0);
  expect_eq(xml_append(d, xml_new(XML_TEXT, 0, "t")), -1);
  expect_eq(xml_append(d, a), 0);
  expect_eq(xml_append(d, b), -1);
  expect_eq(xml_append(a, d), -1);
  expect(xml_new_element("1bad", 0) == 0);
  expect_eq(xml_append(a, xml_new(XML_COMMENT, 0, "x--y")), 0);
  expect(xml_write(d, 0, 0) == 0);
  xml_free(d);
  struct xml_node *t = xml_new(XML_TEXT, 0, "a\001b");
  expect(xml_write(t, 0, 0) == 0);
  xml_free(t);
}

int main(void)
{
  test("elements, attributes and text", test_elements_attributes_and_text);
  test("references, CDATA and line ends", test_references_cdata_and_line_ends);
  test("namespaces resolve", test_namespaces_resolve);
  test("malformed input is rejected", test_rejects_malformed_input);
  test("error position", test_error_position);
  test("DOCTYPE skipped, nesting limited", test_doctype_and_nesting_limit);
  test("round trip and escaping", test_roundtrip_and_escaping);
  test("build and pretty print", test_build_and_pretty_print);
  test("append and write refusals", test_append_and_write_refusals);
  return 0;
}
