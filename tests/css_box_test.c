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

static void check_border_four_value(void)
{
  struct css_style s;
  css_style_reset(&s);
  css_apply_inline("border-style: solid dashed dotted none", &s);
  expect(s.border_style[0] == CSS_BST_SOLID && s.border_style[1] == CSS_BST_DASHED &&
         s.border_style[2] == CSS_BST_DOTTED && s.border_style[3] == CSS_BST_NONE);
}

static void check_line_through(void)
{
  struct css_style s;
  css_style_reset(&s);
  css_apply_inline("text-decoration: underline line-through", &s);
  expect(s.under == 1 && s.strike == 1);
}

static void check_text_align(void)
{
  struct css_style s;
  css_style_reset(&s);
  css_apply_inline("text-align: right", &s);
  expect(s.center == 2);
  css_apply_inline("text-align: justify", &s);
  expect(s.center == 3);
}

static void check_border_dashed(void) { check_border("border-style: dashed", CSS_BST_DASHED); }

static void check_border_shorthand(void) { check_border("border: 2px dotted red", CSS_BST_DOTTED); }

static void check_border_none(void) { check_border("border: 1px solid red; border-style: none", CSS_BST_NONE); }

int main(void)
{
  test("border-style: dashed", check_border_dashed);
  test("border shorthand style", check_border_shorthand);
  test("border-style none", check_border_none);
  test("border-style four-value shorthand", check_border_four_value);
  test("text-decoration line-through", check_line_through);
  test("text-align right and justify", check_text_align);
  return 0;
}
