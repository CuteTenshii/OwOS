/* Host test for the HTML parser's document-level state (user/apps/browser/dom.c). */
#define OWOS_SUITE_TEST
#include "dom.h"
#include "test.h"
#include <string.h>

/* The doctype name dom_parse records for `html`, copied so the tree can be freed; "(none)" when absent */
static void doctype_of(const char *html, char *out, int cap)
{
  struct dom_node *doc = dom_parse(html, (int)strlen(html));
  const char *dt = dom_doctype(doc);
  strncpy(out, dt ? dt : "(none)", (size_t)cap - 1);
  out[cap - 1] = '\0';
  dom_free(doc);
}

static void check_doctype(void)
{
  char dt[64];
  doctype_of("<!DOCTYPE html><html><body>x</body></html>", dt, sizeof dt);
  expect_string(dt, "html");
  doctype_of("<!doctype HTML PUBLIC \"-//W3C//DTD HTML 4.01//EN\">\n<p>x", dt, sizeof dt);
  expect_string(dt, "html");
  doctype_of("  \n<!DOCTYPE\tsvg >", dt, sizeof dt);
  expect_string(dt, "svg");
  doctype_of("<!DOCTYPE>", dt, sizeof dt);
  expect_string(dt, "");
  doctype_of("<html><body>no doctype</body></html>", dt, sizeof dt);
  expect_string(dt, "(none)");
  doctype_of("<!-- comment --><p>x</p>", dt, sizeof dt);
  expect_string(dt, "(none)");
  doctype_of("<html><!DOCTYPE late><p>x</p></html>", dt, sizeof dt);
  expect_string(dt, "(none)");
  doctype_of("<!DOCTYPE html><!DOCTYPE other>", dt, sizeof dt);
  expect_string(dt, "html");
}

static void check_doctype_only_on_documents(void)
{
  struct dom_node *doc = dom_parse("<!DOCTYPE html><p>x</p>", 23);
  expect(doc && doc->first_child);
  expect(doc && dom_doctype(doc->first_child) == 0);
  expect(dom_doctype(0) == 0);
  dom_free(doc);
}

int main(void)
{
  test("doctype names", check_doctype);
  test("doctype only on the document", check_doctype_only_on_documents);
  return 0;
}
