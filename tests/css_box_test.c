/* Host tests for CSS border styles and text-decoration lines. */
#include <stdio.h>
#define OWOS_SUITE_TEST
#include "css.h"
#include "test.h"

static void check_border(const char *decls, int style)
{
  struct css_style s;
  css_style_reset(&s);
  css_apply_inline(decls, &s);
  expect(s.border_style[0] == style && s.border_style[1] == style && s.border_style[2] == style &&
         s.border_style[3] == style);
}

int main(void)
{
  struct css_style s;
  test("border-style: dashed");
  check_border("border-style: dashed", CSS_BST_DASHED);
  test("border shorthand style");
  check_border("border: 2px dotted red", CSS_BST_DOTTED);
  test("border-style none");
  check_border("border: 1px solid red; border-style: none", CSS_BST_NONE);
  test("border-style four-value shorthand");
  css_style_reset(&s);
  css_apply_inline("border-style: solid dashed dotted none", &s);
  expect(s.border_style[0] == CSS_BST_SOLID && s.border_style[1] == CSS_BST_DASHED &&
         s.border_style[2] == CSS_BST_DOTTED && s.border_style[3] == CSS_BST_NONE);

  test("text-decoration line-through");
  css_style_reset(&s);
  css_apply_inline("text-decoration: underline line-through", &s);
  expect(s.under == 1 && s.strike == 1);

  test("text-align right and justify");
  css_style_reset(&s);
  css_apply_inline("text-align: right", &s);
  expect(s.center == 2);
  css_apply_inline("text-align: justify", &s);
  expect(s.center == 3);

  return 0;
}
