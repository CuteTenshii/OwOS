/* Host unit test for the browser CSS selector matcher (css.c): :is()/:where() and paren-aware
 * selector splitting. An empty or unsupported :is() argument must match nothing.
 *
 * Build + run:
 *   cc -std=c11 -Itests -Iuser/apps/browser -Iuser/lib/math \
 *      tests/css_test.c user/apps/browser/css/engine.c user/apps/browser/css/engine_values.c
 * user/apps/browser/css/engine_props.c user/apps/browser/css/engine_decl.c user/apps/browser/css/engine_sel.c
 * user/apps/browser/css/engine_sheet.c user/apps/browser/css/sibling.c user/apps/browser/color.c \ -lm -o /tmp/css_test
 * && /tmp/css_test
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

/* Track every node/attr so the scratch DOM frees at exit. */
static struct dom_node *g_nodes[256];
static int g_nn;
static struct dom_attr *g_attrs[256];
static int g_na;

/* ---- minimal DOM the matcher needs (css.c only calls dom_get_attr + fields) ---- */
static struct dom_node *mk(enum dom_type t, const char *s)
{
  struct dom_node *n = calloc(1, sizeof *n);
  n->type = t;
  n->tag = strdup(t == DOM_ELEMENT ? s : "");
  if (t != DOM_ELEMENT)
    n->text = strdup(s);
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

/* Every element lives under a synthetic #document root so :root works. */
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

/* 1. A rule whose whole selector is :is(<complex>) must not collapse to the universal selector. */
static void test_complex_is_selectors(void)
{
  const char *rtl = ":is(:is(:lang(ar), :lang(he), :lang(fa)) svg.lucide-chevron-right,"
                    " :is(:lang(ar), :lang(he), :lang(fa)) svg.lucide-chevron-left)"
                    " { transform: rotateY(180deg) }";
  css_sheet *sh = sheet(rtl);
  struct dom_node *d = el("div");
  doc(d);
  expect(!matches_any(sh, d));

  /* No RTL ancestor and :lang is unsupported, so no match. */
  struct dom_node *body = el("body"), *svg = cls(el("svg"), "lucide-chevron-right");
  append(body, svg);
  doc(body);
  expect(!matches_any(sh, svg));
  css_free(sh);
}

/* 2. :is() with a class list, constrained by a tag. */
static void test_tag_constrained_is_selectors(void)
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

/* 3. A standalone :is(.x) matches only .x, never everything. */
static void test_standalone_is_selector(void)
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
static void test_selector_grouping(void)
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
static void test_descendant_selector_inside_is(void)
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

/* 6. :where() has zero specificity, so a later bare class rule wins the cascade. */
static void test_where_specificity(void)
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

/* 7. An unsupported selector inside :is() yields no match, not everything. */
static void test_unsupported_selector_handling(void)
{
  css_sheet *sh = sheet(":is(:lang(ar) .x) { color: red }");
  struct dom_node *d = cls(el("div"), "x");
  doc(d);
  expect(!matches_any(sh, d));
  css_free(sh);
}

/* 8. A rule filed under two class keys is reported once for an element with both. */
static void test_multi_key_selector_deduplication(void)
{
  css_sheet *sh = sheet(":is(.a, .b) { color: red }");
  struct dom_node *ab = cls(el("div"), "a b");
  doc(ab);
  struct css_match m[8];
  int n = css_matches(sh, ab, m, 8);
  expect(n == 1);
  css_free(sh);
}

/* 9. A subject :is() with an id/tag files under that key; non-matching branch keys don't leak. */
static void test_id_and_tag_selector(void)
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

/* 10. :popover-open / :modal follow the state attributes; closed elements don't match. */
static void test_popover_and_dialog_state(void)
{
  css_sheet *sh = sheet(":popover-open { color: red } dialog:modal { color: blue }");
  struct dom_node *closed = el("div"), *opened = el("div"), *plain = el("dialog"), *modal = el("dialog");
  set_attr(opened, "popover-open", "");
  set_attr(modal, "open", "");
  set_attr(modal, "dialog-modal", "");
  doc(closed);
  doc(opened);
  doc(plain);
  doc(modal);
  expect(!matches_any(sh, closed));
  expect(matches_any(sh, opened));
  expect(!matches_any(sh, plain));
  expect(matches_any(sh, modal));
  css_free(sh);
}

/* 11. dialogs and popovers get a UA box that author styles can override. */
static void test_dialog_and_popover_ua(void)
{
  struct css_style st;
  struct dom_node *dlg = el("dialog"), *pop = el("div"), *div = el("div");
  set_attr(pop, "popover", "");
  css_style_reset(&st);
  css_apply_attrs(dlg, &st);
  expect(st.bg == 0xffffff && st.border_w[0] == 2 && st.border_style[0] == CSS_BST_SOLID && st.margin[1] == -2);
  css_style_reset(&st);
  css_apply_attrs(pop, &st);
  expect(st.bg == 0xffffff && st.border_w[0] == 1);
  css_style_reset(&st);
  css_apply_attrs(div, &st);
  expect(st.bg == -1 && st.border_w[0] == -1);
}

/* :host styles the shadow host only from its own shadow sheet; inside the tree it works as an ancestor */
static void test_host_selectors(void)
{
  css_sheet *sh =
      sheet(":host { color: red } :host(.on) { color: blue } div { color: green } :host .a { color: pink }");
  struct dom_node *host = el("x-host"), *root = el("#shadow-root"), *inner = cls(el("div"), "a"), *plain = el("div");
  struct css_style st;
  root->host = host;
  host->shadow = root;
  append(root, inner);
  append(root, plain);
  doc(host);

  css_set_scope(host, true);
  css_style_reset(&st);
  css_apply_sheet(sh, host, &st);
  expect(st.color == 0xff0000);
  css_set_scope(0, false);

  css_style_reset(&st);
  css_apply_sheet(sh, host, &st);
  expect(st.color == -1); /* the page's own pass never sees :host */

  struct dom_node *host2 = cls(el("x-host"), "on");
  doc(host2);
  css_set_scope(host2, true);
  css_style_reset(&st);
  css_apply_sheet(sh, host2, &st);
  expect(st.color == 0x0000ff); /* :host(.on) comes later and matches */
  css_set_scope(host, true);
  css_style_reset(&st);
  css_apply_sheet(sh, plain, &st);
  expect(st.color == -1); /* host-only mode skips every other rule */

  css_set_scope(host, false);
  css_style_reset(&st);
  css_apply_sheet(sh, plain, &st);
  expect(st.color == 0x008000);
  css_style_reset(&st);
  css_apply_sheet(sh, inner, &st);
  expect(st.color == 0xffc0cb); /* :host .a reaches the host through the shadow root */
  css_set_scope(0, false);
  css_free(sh);
}

/* Attribute selectors and :not() with attribute or list arguments */
static void test_attribute_selectors(void)
{
  struct dom_node *a = el("a"), *b = el("div"), *c = el("span");
  set_attr(a, "href", "https://x.test/docs/page.html");
  set_attr(a, "rel", "nofollow external");
  set_attr(a, "lang", "en-GB");
  set_attr(b, "data-mode", "Dark");
  doc(a);
  doc(b);
  doc(c);
  const struct {
    const char *sel;
    int on_a, on_b, on_c;
  } cases[] = {
      {"a[href]", 1, 0, 0},
      {"[href]", 1, 0, 0},
      {"[href=\"https://x.test/docs/page.html\"]", 1, 0, 0},
      {"[href='nope']", 0, 0, 0},
      {"[href^=https]", 1, 0, 0},
      {"[href$='.html']", 1, 0, 0},
      {"[href*=docs]", 1, 0, 0},
      {"[rel~=external]", 1, 0, 0},
      {"[rel~=extern]", 0, 0, 0},
      {"[lang|=en]", 1, 0, 0},
      {"[data-mode=dark]", 0, 0, 0},
      {"[data-mode=dark i]", 0, 1, 0},
      {"[data-mode='Dark']", 0, 1, 0},
      {"[ href ]", 1, 0, 0},
      {"[href^='']", 0, 0, 0},
      {"div:not([data-mode])", 0, 0, 0},
      {":not([href])", 0, 1, 1},
      {":not(a, div)", 0, 0, 1},
      {"span:not([href]):not(.x)", 0, 0, 1},
  };
  for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    char rule[128];
    snprintf(rule, sizeof rule, "%s { color: red }", cases[i].sel);
    css_sheet *sh = sheet(rule);
    expect(matches_any(sh, a) == cases[i].on_a);
    expect(matches_any(sh, b) == cases[i].on_b);
    expect(matches_any(sh, c) == cases[i].on_c);
    css_free(sh);
  }
}

/* A shadow sheet's own custom properties win inside it and never reach the page or another shadow sheet */
static void test_scoped_custom_properties(void)
{
  css_sheet *page = sheet(":root { --c: red; --d: green } p { color: var(--c) } b { color: var(--d) }");
  css_sheet *one = sheet(":host { --c: blue } p { color: var(--c) } b { color: var(--d) }");
  css_sheet *two = sheet("p { color: var(--c, black) }");
  struct dom_node *p = el("p");
  struct dom_node *b = el("b");
  struct css_style st;
  doc(p);
  doc(b);
  css_set_primary(page);
  css_style_reset(&st);
  css_apply_sheet(page, p, &st);
  expect(st.color == 0xff0000);
  css_style_reset(&st);
  css_apply_sheet(one, p, &st);
  expect(st.color == 0x0000ff);
  css_style_reset(&st);
  css_apply_sheet(one, b, &st);
  expect(st.color == 0x008000); /* not declared locally: the page's value */
  css_style_reset(&st);
  css_apply_sheet(two, p, &st);
  expect(st.color == 0xff0000); /* the other shadow sheet's --c did not leak; the page's is seen */
  css_style_reset(&st);
  css_apply_sheet(page, p, &st);
  expect(st.color == 0xff0000);
  css_set_primary(0);
  css_free(page);
  css_free(one);
  css_free(two);
}

/* ::slotted() styles assigned light children from the shadow sheet; ::part() styles shadow elements from outside */
static void test_slotted_and_part(void)
{
  struct dom_node *host = el("x-h"), *root = el("#shadow-root"), *wrap = cls(el("div"), "w"), *slot = el("slot");
  struct dom_node *e1 = cls(el("span"), "k"), *e2 = el("span"), *btn = el("button"), *plain = el("button");
  struct css_style st;
  set_attr(slot, "name", "a");
  set_attr(e1, "slot", "a");
  set_attr(btn, "part", "btn big");
  root->host = host;
  host->shadow = root;
  append(root, wrap);
  append(wrap, slot);
  append(wrap, btn);
  append(wrap, plain);
  append(host, e1);
  append(host, e2);
  doc(host);

  css_sheet *inner =
      sheet("::slotted(span) { color: red } .w ::slotted(.k) { color: blue } slot::slotted(b) { color: lime }");
  css_set_scope(host, CSS_SCOPE_SLOTTED);
  css_style_reset(&st);
  css_apply_sheet(inner, e1, &st);
  expect(st.color == 0x0000ff); /* the more specific rule, whose ancestor matches the slot's */
  css_style_reset(&st);
  css_apply_sheet(inner, e2, &st);
  expect(st.color == -1); /* no slot takes an unnamed child here */
  css_set_scope(host, CSS_SCOPE_NORMAL);
  css_style_reset(&st);
  css_apply_sheet(inner, e1, &st);
  expect(st.color == -1); /* normal pass ignores ::slotted rules */

  css_sheet *outer =
      sheet("x-h::part(btn) { color: pink } x-h::part(btn big) { background: black } x-h::part(nope) { color: lime }");
  css_set_scope(host, CSS_SCOPE_PART);
  css_style_reset(&st);
  css_apply_sheet(outer, btn, &st);
  expect(st.color == 0xffc0cb && st.bg == 0);
  css_style_reset(&st);
  css_apply_sheet(outer, plain, &st);
  expect(st.color == -1 && st.bg == -1);
  css_set_scope(0, CSS_SCOPE_NORMAL);
  css_free(inner);
  css_free(outer);
}

/* 16. Escaped identifiers: Tailwind variants like .md\:flex and .w-\[80vh\] name classes "md:flex", "w-[80vh]". */
static int sel_hits(const char *src, const struct dom_node *e)
{
  css_sheet *sh = sheet(src);
  int hit = matches_any(sh, e);
  css_free(sh);
  return hit;
}
static void test_escaped_identifiers(void)
{
  struct dom_node *variant = cls(el("div"), "relative max-md:flex-col");
  struct dom_node *arbitrary = cls(el("div"), "h-[80vh]");
  struct dom_node *slash = cls(el("nav"), "bg-gray-700/50");
  struct dom_node *current = cls(el("a"), "aria-[current=page]:bg-white");
  struct dom_node *not_current = cls(el("a"), "aria-[current=page]:bg-white");
  struct dom_node *hex = cls(el("p"), "2xl:p-4");
  struct dom_node *comma = cls(el("p"), "a,b");
  struct dom_node *nul = cls(el("p"), "\xef\xbf\xbdx");
  struct dom_node *id = el("p");
  struct dom_node *raw = cls(el("p"), "caf\xc3\xa9");
  struct dom_node *prefix = cls(el("div"), "max-md");
  set_attr(current, "aria-current", "page");
  set_attr(id, "id", "caf\xc3\xa9");
  struct dom_node *all[] = {variant, arbitrary, slash, current, not_current, hex, comma, nul, id, raw, prefix};
  for (unsigned i = 0; i < sizeof all / sizeof *all; i++)
    doc(all[i]);

  expect(sel_hits(".max-md\\:flex-col { color: red }", variant));
  expect(!sel_hits(".max-md\\:flex-col { color: red }", prefix));
  expect(sel_hits(".h-\\[80vh\\] { color: red }", arbitrary));
  expect(!sel_hits(".h-\\[80vh\\] { color: red }", variant));
  expect(sel_hits(".bg-gray-700\\/50 { color: red }", slash));
  expect(sel_hits(".aria-\\[current\\=page\\]\\:bg-white[aria-current=page] { color: red }", current));
  expect(!sel_hits(".aria-\\[current\\=page\\]\\:bg-white[aria-current=page] { color: red }", not_current));
  expect(sel_hits(".\\32 xl\\:p-4 { color: red }", hex)); /* the space ends the hex escape, not a combinator */
  expect(!sel_hits(".\\32 xl\\:p-4 { color: red }", variant));
  expect(sel_hits(".a\\,b { color: red }", comma)); /* an escaped comma doesn't split the list */
  expect(!sel_hits(".a\\,b { color: red }", variant));
  expect(sel_hits(".\\0 x { color: red }", nul)); /* NUL decodes to U+FFFD */
  expect(sel_hits("#caf\\e9 { color: red }", id));
  expect(sel_hits(".caf\\\xc3\xa9 { color: red }", raw)); /* an escaped raw UTF-8 character */
  expect(sel_hits(".caf\xc3\xa9 { color: red }", raw));   /* a bare non-ASCII name */
}

/* 17. color/text-decoration inherit must survive the cascade so the walk can override a link's UA blue/underline. */
static void test_inherit_keywords(void)
{
  struct dom_node *a = el("a"), *p = el("p"), *q = el("q");
  doc(a);
  doc(p);
  doc(q);
  struct css_style st;

  css_sheet *sh = sheet("a { color: red; text-decoration: underline } a { color: inherit; text-decoration: inherit }");
  css_style_reset(&st);
  css_apply_sheet(sh, a, &st);
  expect_eq(st.color, CSS_COLOR_INHERIT);
  expect_eq(st.under, CSS_DECO_INHERIT);
  expect_eq(st.strike, CSS_DECO_INHERIT);
  css_free(sh);

  sh = sheet("p { color: currentColor; text-decoration: line-through } q { color: initial; text-decoration: unset }");
  css_style_reset(&st);
  css_apply_sheet(sh, p, &st);
  expect_eq(st.color, CSS_COLOR_INHERIT);
  expect_eq(st.strike, 1);
  css_style_reset(&st);
  css_apply_sheet(sh, q, &st);
  expect_eq(st.color, 0x000000);
  expect_eq(st.under, 0);
  expect_eq(st.strike, 0);
  css_free(sh);
}

/* 18. Box shorthands split on top-level spaces only: Tailwind's px-3 is padding-inline: calc(var(--spacing) * 3). */
static void test_box_shorthand_calc(void)
{
  struct dom_node *a = el("a"), *b = el("b");
  doc(a);
  doc(b);
  struct css_style st;
  css_sheet *sh = sheet(":root { --spacing: 4px } a { padding-inline: calc(var(--spacing) * 3) }"
                        " b { margin: calc(2px * 2) calc(3px + 1px) 5px }");
  css_style_reset(&st);
  css_apply_sheet(sh, a, &st);
  expect_eq(st.padding[3], 12);
  expect_eq(st.padding[1], 12);
  css_style_reset(&st);
  css_apply_sheet(sh, b, &st);
  expect_eq(st.margin[0], 4);
  expect_eq(st.margin[1], 4);
  expect_eq(st.margin[2], 5);
  expect_eq(st.margin[3], 4);
  css_free(sh);
}

/* 19. content: "" still generates a pseudo-element (a fixed backdrop, a clearfix); none and normal don't. */
static void test_empty_content_generates(void)
{
  struct dom_node *h = el("html"), *p = el("p"), *q = el("q");
  doc(h);
  doc(p);
  doc(q);
  struct css_style st;
  css_sheet *sh = sheet("html::before { content: \"\"; position: fixed } p::after { content: \"x\" }"
                        " p::after { content: none } q::before { content: \"a\" } q::before { content: \"\" }");
  expect(css_pseudo_content(sh, h, CSS_PSEUDO_BEFORE, &st));
  expect_eq(st.pos, CSS_POS_FIXED);
  expect(!css_pseudo_content(sh, p, CSS_PSEUDO_AFTER, &st));
  expect(css_pseudo_content(sh, q, CSS_PSEUDO_BEFORE, &st));
  expect_eq(st.content[0], 0); /* the later "" replaces "a" */
  css_free(sh);
}

/* 20. A translucent background keeps its alpha through the cascade, so the box paints it translucent. */
static void test_background_alpha(void)
{
  struct dom_node *a = el("a"), *b = el("b"), *i = el("i");
  doc(a);
  doc(b);
  doc(i);
  struct css_style st;
  css_sheet *sh =
      sheet("a { background-color: #36415380 } b { background: rgba(0, 0, 0, .25) } i { background: #123 }");
  css_style_reset(&st);
  css_apply_sheet(sh, a, &st);
  expect_eq(st.bg, 0x364153);
  expect_eq(st.bg_a, 0x80);
  css_style_reset(&st);
  css_apply_sheet(sh, b, &st);
  expect_eq(st.bg, 0);
  expect(st.bg_a >= 63 && st.bg_a <= 64);
  css_style_reset(&st);
  css_apply_sheet(sh, i, &st);
  expect_eq(st.bg_a, 255);
  css_free(sh);
}

int main(void)
{
  test("complex :is selectors", test_complex_is_selectors);
  test("tag-constrained :is selectors", test_tag_constrained_is_selectors);
  test("standalone :is selector", test_standalone_is_selector);
  test("selector grouping", test_selector_grouping);
  test("descendant selector inside :is", test_descendant_selector_inside_is);
  test(":where specificity", test_where_specificity);
  test("unsupported selector handling", test_unsupported_selector_handling);
  test("multi-key selector deduplication", test_multi_key_selector_deduplication);
  test("id and tag selector keys", test_id_and_tag_selector);
  test("popover and dialog state selectors", test_popover_and_dialog_state);
  test("dialog and popover UA box", test_dialog_and_popover_ua);
  test(":host selectors", test_host_selectors);
  test("attribute selectors", test_attribute_selectors);
  test("scoped custom properties", test_scoped_custom_properties);
  test("::slotted and ::part", test_slotted_and_part);
  test("escaped identifiers", test_escaped_identifiers);
  test("inherit keywords", test_inherit_keywords);
  test("empty content generates", test_empty_content_generates);
  test("box shorthand calc", test_box_shorthand_calc);
  test("background alpha", test_background_alpha);

  free_dom();
  return 0;
}
