/* Host unit test for the browser's CSS list-* support (css.c).
 *
 * Exercises list-style-type / list-style-position parsing (longhands + the
 * `list-style` shorthand) through css_apply_inline, and the cascade merge of the
 * new list_pos field through css_apply_sheet against a real <li> DOM node.
 *
 *   cc -std=c11 -Iscripts -Iuser/apps/gui/browser \
 *      tests/css_list_test.c user/apps/gui/browser/css.c \
 *      user/apps/gui/browser/color.c user/apps/gui/browser/dom.c \
 *      -lm -o /tmp/css_list_test && /tmp/css_list_test
 */
#include <stdio.h>
#define OWOS_SUITE_TEST
#include "css.h"

static const char *lst_name(int v)
{
	static const char *n[] = { "default", "none", "disc", "circle", "square",
		"decimal", "decimal-zero", "lower-alpha", "upper-alpha",
		"lower-roman", "upper-roman" };
	return (v >= 0 && v <= CSS_LST_UPPER_ROMAN) ? n[v] : "?";
}
static const char *pos_name(int v)
{
	static const char *n[] = { "default", "outside", "inside" };
	return (v >= 0 && v <= CSS_LSP_INSIDE) ? n[v] : "?";
}

static void chk_inline(const char *decls, int want_type, int want_pos)
{
	struct css_style s;
	test(decls);
	css_style_reset(&s);
	css_apply_inline(decls, &s);
	expect(s.list_style == want_type && s.list_pos == want_pos);
}

int main(void)
{
	/* Longhands. */
	chk_inline("list-style-type: decimal",              CSS_LST_DECIMAL,      CSS_LSP_DEFAULT);
	chk_inline("list-style-type: decimal-leading-zero", CSS_LST_DECIMAL_ZERO, CSS_LSP_DEFAULT);
	chk_inline("list-style-type: upper-alpha",          CSS_LST_UPPER_ALPHA,  CSS_LSP_DEFAULT);
	chk_inline("list-style-position: inside",           CSS_LST_DEFAULT,      CSS_LSP_INSIDE);
	chk_inline("list-style-position: outside",          CSS_LST_DEFAULT,      CSS_LSP_OUTSIDE);

	/* Shorthand, both components, either order. */
	chk_inline("list-style: lower-roman inside",        CSS_LST_LOWER_ROMAN,  CSS_LSP_INSIDE);
	chk_inline("list-style: inside square",             CSS_LST_SQUARE,       CSS_LSP_INSIDE);
	chk_inline("list-style: none",                      CSS_LST_NONE,         CSS_LSP_DEFAULT);
	chk_inline("list-style: disc",                      CSS_LST_DISC,         CSS_LSP_DEFAULT);
	/* A bare url() names no type keyword: type stays default (ul/ol fallback). */
	chk_inline("list-style: url(bullet.png)",           CSS_LST_DEFAULT,      CSS_LSP_DEFAULT);

	/* Cascade: two matching rules contribute type and position separately; both
	 * must survive the merge (regression guard for the new list_pos field). */
	css_sheet *sheet = css_new();
	const char *src = "li { list-style-type: upper-roman }"
	                  "li { list-style-position: inside }";
	css_add(sheet, src, (int)__builtin_strlen(src), "http://x/");
	struct dom_node *li = dom_new_element("li");
	struct css_style s;
	css_style_reset(&s);
	css_apply_sheet(sheet, li, &s);
	expect(s.list_style == CSS_LST_UPPER_ROMAN && s.list_pos == CSS_LSP_INSIDE);
	dom_free(li);
	css_free(sheet);

	return 0;
}
