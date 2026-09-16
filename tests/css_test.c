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
 *      tests/css_test.c user/apps/gui/browser/css/engine.c user/apps/gui/browser/css/sibling.c
 * user/apps/gui/browser/color.c \ -lm -o /tmp/css_test && /tmp/css_test
 */
#include <stdio.h>
#define OWOS_SUITE_TEST
#include "css.h"
#include "dom.h"
#include <stdlib.h>
#include <string.h>

static char *dup_str(const char *s)
{
  char *p = malloc(strlen(s) + 1);
  strcpy(p, s);
  return p;
}
#define strdup dup_str

/* Track every node/attr so the whole scratch DOM frees at exit — a leak that
 * survives free_dom() then belongs to css.c, not the harness. */
static struct dom_node *g_nodes[256];
static int g_nn;
static struct dom_attr *g_attrs[256];
static int g_na;

/* ---- minimal DOM the matcher needs (css.c only calls dom_get_attr + fields) ---- */
static struct dom_node *mk(enum dom_type t, const char *s)
{
  struct dom_node *n = calloc(1, sizeof *n);
  n->type = t;
  if (t == DOM_ELEMENT) {
    strncpy(n->tag, s, sizeof n->tag - 1);
  } else {
    n->text = strdup(s);
  }
  g_nodes[g_nn++] = n;
  return n;
}
static struct dom_node *el(const char *tag) { return mk(DOM_ELEMENT, tag); }

static void set_attr(struct dom_node *n, const char *name, const char *val)
{
  struct dom_attr *a = calloc(1, sizeof *a);
  a->name = strdup(name);
  a->value = strdup(val ? val : "");
  if (n->last_attr)
    n->last_attr->next = a;
  else
    n->attrs = a;
  n->last_attr = a;
  g_attrs[g_na++] = a;
}

static void free_dom(void)
{
  for (int i = 0; i < g_na; i++) {
    free(g_attrs[i]->name);
    free(g_attrs[i]->value);
    free(g_attrs[i]);
  }
  for (int i = 0; i < g_nn; i++) {
    free(g_nodes[i]->text);
    free(g_nodes[i]);
  }
  g_na = g_nn = 0;
}
static struct dom_node *cls(struct dom_node *n, const char *c)
{
  set_attr(n, "class", c);
  return n;
}

static void append(struct dom_node *p, struct dom_node *c)
{
  c->parent = p;
  if (p->last_child)
    p->last_child->next = c;
  else
    p->first_child = c;
  p->last_child = c;
}

__attribute__((weak)) const char *dom_get_attr(const struct dom_node *n, const char *name)
{
  for (struct dom_attr *a = n ? n->attrs : 0; a; a = a->next)
    if (strcmp(a->name, name) == 0)
      return a->value;
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
  test("complex :is selectors");
  {
    const char *rtl = ":is(:is(:lang(ar), :lang(he), :lang(fa)) svg.lucide-chevron-right,"
                      " :is(:lang(ar), :lang(he), :lang(fa)) svg.lucide-chevron-left)"
                      " { transform: rotateY(180deg) }";
    css_sheet *sh = sheet(rtl);
    struct dom_node *d = el("div");
    doc(d);
    expect(!matches_any(sh, d));

    /* An svg with the class but no RTL ancestor also matches nothing (:lang
     * is unsupported -> the sub-selector can never match, so the rule can't). */
    struct dom_node *body = el("body"), *svg = cls(el("svg"), "lucide-chevron-right");
    append(body, svg);
    doc(body);
    expect(!matches_any(sh, svg));
    css_free(sh);
  }

  /* 2. :is() with a class list, constrained by a tag. */
  test("tag-constrained :is selectors");
  {
    css_sheet *sh = sheet("div:is(.x, .y) { color: red }");
    struct dom_node *dx = cls(el("div"), "x"), *dy = cls(el("div"), "y"), *dz = cls(el("div"), "z"),
                    *sx = cls(el("span"), "x");
    doc(dx);
    doc(dy);
    doc(dz);
    doc(sx);
    expect(matches_any(sh, dx));
    expect(matches_any(sh, dy));
    expect(!matches_any(sh, dz));
    expect(!matches_any(sh, sx));
    css_free(sh);
  }

  /* 3. A standalone :is(.x) matches only .x — never everything. */
  test("standalone :is selector");
  {
    css_sheet *sh = sheet(":is(.x) { color: red }");
    struct dom_node *px = cls(el("p"), "x"), *pz = cls(el("p"), "z");
    doc(px);
    doc(pz);
    expect(matches_any(sh, px));
    expect(!matches_any(sh, pz));
    css_free(sh);
  }

  /* 4. A top-level comma still groups; a comma INSIDE :is() must not. */
  test("selector grouping");
  {
    css_sheet *sh = sheet(".keep, :is(.a, .b) { color: red }");
    struct dom_node *k = cls(el("p"), "keep"), *a = cls(el("p"), "a"), *b = cls(el("p"), "b"), *c = cls(el("p"), "c");
    doc(k);
    doc(a);
    doc(b);
    doc(c);
    expect(matches_any(sh, k));
    expect(matches_any(sh, a));
    expect(matches_any(sh, b));
    expect(!matches_any(sh, c));
    css_free(sh);
  }

  /* 5. :is() may hold a full descendant selector (space inside the parens). */
  test("descendant selector inside :is");
  {
    css_sheet *sh = sheet(":is(section p) { color: red }");
    struct dom_node *sec = el("section"), *pin = el("p");
    append(sec, pin);
    doc(sec);
    struct dom_node *loose = el("p");
    doc(loose);
    expect(matches_any(sh, pin));
    expect(!matches_any(sh, loose));
    css_free(sh);
  }

  /* 6. :where() matches functionally but carries zero specificity, so a bare
   *    class rule declared later still wins the cascade (reported winner-first). */
  test(":where specificity");
  {
    css_sheet *sh = sheet("p:where(.w) { color: red } .w { color: green }");
    struct dom_node *pw = cls(el("p"), "w");
    doc(pw);
    struct css_match m[8];
    int n = css_matches(sh, pw, m, 8);
    expect(n == 2);
    /* winner (index 0) is the higher-specificity .w, not the :where rule */
    expect(n >= 1 && strstr(m[0].selector, "where") == 0);
    css_free(sh);
  }

  /* 7. An unsupported selector inside :is() yields no match — not everything. */
  test("unsupported selector handling");
  {
    css_sheet *sh = sheet(":is(:lang(ar) .x) { color: red }");
    struct dom_node *d = cls(el("div"), "x");
    doc(d);
    expect(!matches_any(sh, d));
    css_free(sh);
  }

  /* 8. Multi-key indexing de-dup: a rule filed under two class keys must be
   *    reported exactly once for an element that carries both classes. */
  test("multi-key selector deduplication");
  {
    css_sheet *sh = sheet(":is(.a, .b) { color: red }");
    struct dom_node *ab = cls(el("div"), "a b");
    doc(ab);
    struct css_match m[8];
    int n = css_matches(sh, ab, m, 8);
    expect(n == 1);
    css_free(sh);
  }

  /* 9. A subject :is() constrained by an id/tag files under that single key and
   *    still matches; a non-matching branch key doesn't leak the rule elsewhere. */
  test("id and tag selector keys");
  {
    css_sheet *sh = sheet(":is(#hero, aside) .item { color: red }");
    struct dom_node *hero = el("div"), *item = cls(el("span"), "item");
    set_attr(hero, "id", "hero");
    append(hero, item);
    doc(hero);
    struct dom_node *aside = el("aside"), *item2 = cls(el("span"), "item");
    append(aside, item2);
    doc(aside);
    struct dom_node *plain = el("div"), *item3 = cls(el("span"), "item");
    append(plain, item3);
    doc(plain);
    expect(matches_any(sh, item));
    expect(matches_any(sh, item2));
    expect(!matches_any(sh, item3));
    css_free(sh);
  }

  free_dom();
  return 0;
}
