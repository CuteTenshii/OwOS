/* css_test.c — host unit test for the browser CSS selector matcher (css.c).
 *
 * Builds tiny DOM trees by hand and asserts which sheet rules css_matches()
 * reports for an element. Focus: :is()/:where() support and the paren-aware
 * selector-list / compound splitting — i.e. that a selector wrapped in :is()
 * matches its real targets and, crucially, NOTHING (never the universal *)
 * when its argument is empty or unsupported.
 *
 * Build + run (see CLAUDE.md "pure-lib logic gets host unit tests"):
 *   cc -std=c11 -Iscripts -Iuser/apps/gui/browser -Iuser/lib/math \
 *      scripts/css_test.c user/apps/gui/browser/css.c user/apps/gui/browser/color.c \
 *      -lm -o /tmp/css_test && /tmp/css_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dom.h"
#include "css.h"

static char *dup_str(const char *s) { char *p = malloc(strlen(s) + 1); strcpy(p, s); return p; }
#define strdup dup_str

/* Track every node/attr so the whole scratch DOM frees at exit — a leak that
 * survives free_dom() then belongs to css.c, not the harness. */
static struct dom_node *g_nodes[256]; static int g_nn;
static struct dom_attr *g_attrs[256]; static int g_na;

/* ---- minimal DOM the matcher needs (css.c only calls dom_get_attr + fields) ---- */
static struct dom_node *mk(enum dom_type t, const char *s)
{
	struct dom_node *n = calloc(1, sizeof *n);
	n->type = t;
	if (t == DOM_ELEMENT) { strncpy(n->tag, s, sizeof n->tag - 1); }
	else                  { n->text = strdup(s); }
	g_nodes[g_nn++] = n;
	return n;
}
static struct dom_node *el(const char *tag) { return mk(DOM_ELEMENT, tag); }

static void set_attr(struct dom_node *n, const char *name, const char *val)
{
	struct dom_attr *a = calloc(1, sizeof *a);
	a->name = strdup(name); a->value = strdup(val ? val : "");
	if (n->last_attr) n->last_attr->next = a; else n->attrs = a;
	n->last_attr = a;
	g_attrs[g_na++] = a;
}

static void free_dom(void)
{
	for (int i = 0; i < g_na; i++) { free(g_attrs[i]->name); free(g_attrs[i]->value); free(g_attrs[i]); }
	for (int i = 0; i < g_nn; i++) { free(g_nodes[i]->text); free(g_nodes[i]); }
	g_na = g_nn = 0;
}
static struct dom_node *cls(struct dom_node *n, const char *c) { set_attr(n, "class", c); return n; }

static void append(struct dom_node *p, struct dom_node *c)
{
	c->parent = p;
	if (p->last_child) p->last_child->next = c; else p->first_child = c;
	p->last_child = c;
}

const char *dom_get_attr(const struct dom_node *n, const char *name)
{
	for (struct dom_attr *a = n ? n->attrs : 0; a; a = a->next)
		if (strcmp(a->name, name) == 0) return a->value;
	return 0;
}

/* Every element lives under a synthetic #document root so :root and the ancestor
 * walk behave as in the real tree. */
static struct dom_node *doc(struct dom_node *child)
{
	struct dom_node *root = el("#document");
	append(root, child);
	return root;
}

/* ---- assertions --------------------------------------------------------- */
static int g_fail;
static void ok(int cond, const char *msg)
{
	printf(cond ? "  ok   %s\n" : "  FAIL %s\n", msg);
	if (!cond) g_fail = 1;
}

/* Does `sheet` have any rule matching `e`? */
static int matches_any(css_sheet *sh, const struct dom_node *e)
{
	struct css_match m[32];
	return css_matches(sh, e, m, 32) > 0;
}

static css_sheet *sheet(const char *src)
{
	css_sheet *sh = css_new();
	css_add(sh, src, (int)strlen(src), "http://t/");
	return sh;
}

int main(void)
{
	/* 1. The reported bug: a rule whose whole selector is :is(<complex>) must not
	 *    collapse to the universal selector. A plain <div> matches nothing. */
	{
		const char *rtl =
		  ":is(:is(:lang(ar), :lang(he), :lang(fa)) svg.lucide-chevron-right,"
		  " :is(:lang(ar), :lang(he), :lang(fa)) svg.lucide-chevron-left)"
		  " { transform: rotateY(180deg) }";
		css_sheet *sh = sheet(rtl);
		struct dom_node *d = el("div"); doc(d);
		ok(!matches_any(sh, d), ":is()-wrapped RTL rule does NOT match a plain <div> (was universal *)");

		/* An svg with the class but no RTL ancestor also matches nothing (:lang
		 * is unsupported -> the sub-selector can never match, so the rule can't). */
		struct dom_node *body = el("body"), *svg = cls(el("svg"), "lucide-chevron-right");
		append(body, svg); doc(body);
		ok(!matches_any(sh, svg), ":is()-wrapped RTL rule matches nothing without a :lang match");
		css_free(sh);
	}

	/* 2. :is() with a class list, constrained by a tag. */
	{
		css_sheet *sh = sheet("div:is(.x, .y) { color: red }");
		struct dom_node *dx = cls(el("div"), "x"), *dy = cls(el("div"), "y"),
		                *dz = cls(el("div"), "z"), *sx = cls(el("span"), "x");
		doc(dx); doc(dy); doc(dz); doc(sx);
		ok( matches_any(sh, dx), "div:is(.x,.y) matches div.x");
		ok( matches_any(sh, dy), "div:is(.x,.y) matches div.y");
		ok(!matches_any(sh, dz), "div:is(.x,.y) does not match div.z");
		ok(!matches_any(sh, sx), "div:is(.x,.y) does not match span.x");
		css_free(sh);
	}

	/* 3. A standalone :is(.x) matches only .x — never everything. */
	{
		css_sheet *sh = sheet(":is(.x) { color: red }");
		struct dom_node *px = cls(el("p"), "x"), *pz = cls(el("p"), "z");
		doc(px); doc(pz);
		ok( matches_any(sh, px), ":is(.x) matches p.x");
		ok(!matches_any(sh, pz), ":is(.x) does not match p.z (no universal fallback)");
		css_free(sh);
	}

	/* 4. A top-level comma still groups; a comma INSIDE :is() must not. */
	{
		css_sheet *sh = sheet(".keep, :is(.a, .b) { color: red }");
		struct dom_node *k = cls(el("p"), "keep"), *a = cls(el("p"), "a"),
		                *b = cls(el("p"), "b"), *c = cls(el("p"), "c");
		doc(k); doc(a); doc(b); doc(c);
		ok( matches_any(sh, k), "grouping: .keep matches");
		ok( matches_any(sh, a), "grouping: :is(.a,.b) matches .a");
		ok( matches_any(sh, b), "grouping: :is(.a,.b) matches .b");
		ok(!matches_any(sh, c), "grouping: .c matches neither");
		css_free(sh);
	}

	/* 5. :is() may hold a full descendant selector (space inside the parens). */
	{
		css_sheet *sh = sheet(":is(section p) { color: red }");
		struct dom_node *sec = el("section"), *pin = el("p");
		append(sec, pin); doc(sec);
		struct dom_node *loose = el("p"); doc(loose);
		ok( matches_any(sh, pin),   ":is(section p) matches a <p> inside <section>");
		ok(!matches_any(sh, loose), ":is(section p) does not match a <p> elsewhere");
		css_free(sh);
	}

	/* 6. :where() matches functionally but carries zero specificity, so a bare
	 *    class rule declared later still wins the cascade (reported winner-first). */
	{
		css_sheet *sh = sheet("p:where(.w) { color: red } .w { color: green }");
		struct dom_node *pw = cls(el("p"), "w"); doc(pw);
		struct css_match m[8];
		int n = css_matches(sh, pw, m, 8);
		ok(n == 2, ":where(.w) + .w both match p.w");
		/* winner (index 0) is the higher-specificity .w, not the :where rule */
		ok(n >= 1 && strstr(m[0].selector, "where") == 0, ":where() specificity is below a plain class");
		css_free(sh);
	}

	/* 7. An unsupported selector inside :is() yields no match — not everything. */
	{
		css_sheet *sh = sheet(":is(:lang(ar) .x) { color: red }");
		struct dom_node *d = cls(el("div"), "x"); doc(d);
		ok(!matches_any(sh, d), ":is() with an unsupported inner selector matches nothing");
		css_free(sh);
	}

	/* 8. Multi-key indexing de-dup: a rule filed under two class keys must be
	 *    reported exactly once for an element that carries both classes. */
	{
		css_sheet *sh = sheet(":is(.a, .b) { color: red }");
		struct dom_node *ab = cls(el("div"), "a b"); doc(ab);
		struct css_match m[8];
		int n = css_matches(sh, ab, m, 8);
		ok(n == 1, "multi-key :is(.a,.b) reported once for class=\"a b\" (dedup)");
		css_free(sh);
	}

	/* 9. A subject :is() constrained by an id/tag files under that single key and
	 *    still matches; a non-matching branch key doesn't leak the rule elsewhere. */
	{
		css_sheet *sh = sheet(":is(#hero, aside) .item { color: red }");
		struct dom_node *hero = el("div"), *item = cls(el("span"), "item");
		set_attr(hero, "id", "hero"); append(hero, item); doc(hero);
		struct dom_node *aside = el("aside"), *item2 = cls(el("span"), "item");
		append(aside, item2); doc(aside);
		struct dom_node *plain = el("div"), *item3 = cls(el("span"), "item");
		append(plain, item3); doc(plain);
		ok( matches_any(sh, item),  ":is(#hero, aside) .item matches under #hero");
		ok( matches_any(sh, item2), ":is(#hero, aside) .item matches under <aside>");
		ok(!matches_any(sh, item3), ":is(#hero, aside) .item does not match under a plain div");
		css_free(sh);
	}

	free_dom();
	printf(g_fail ? "\nFAILED\n" : "\nall passed\n");
	return g_fail;
}
