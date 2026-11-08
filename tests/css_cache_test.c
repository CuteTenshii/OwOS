/* Host unit test for the CSS engine's memoised and unbounded paths: var() cache, ancestor bloom,
 * selector/media/keyframe limits, style invalidation, anim store.
 *
 * Build + run: scripts/run-tests.sh (or `make test`). */
#include <stdarg.h>
#include <stdio.h>
#define OWOS_SUITE_TEST
#include "css.h"
#include "css/sibling.h"
#include "dom.h"
#include "features/anim/anim.c"

/* snprintf into base at offset `at`; returns the new length. */
static size_t fmt_at(char *base, size_t cap, size_t at, const char *f, ...)
{
  va_list ap;
  va_start(ap, f);
  vsnprintf(base + at, cap - at, f, ap);
  va_end(ap);
  return at + strlen(base + at);
}

static struct dom_node *parse(const char *html) { return dom_parse(html, (int)strlen(html)); }

static css_sheet *sheet(const char *src)
{
  css_sheet *sh = css_new();
  css_add(sh, src, (int)strlen(src), "http://t/");
  return sh;
}

static struct dom_node *find(struct dom_node *n, const char *id)
{
  const char *v = dom_get_attr(n, "id");
  if (v && !strcmp(v, id))
    return n;
  for (struct dom_node *c = n->first_child; c; c = c->next) {
    struct dom_node *r = find(c, id);
    if (r)
      return r;
  }
  return 0;
}

static void apply(css_sheet *sh, struct dom_node *el, struct css_style *cs)
{
  css_style_reset(cs);
  css_apply_sheet(sh, el, cs);
}

/* ---- reference matcher: same greedy ancestor semantics, no index, no bloom --- */
#define RS_MAX 10
struct rstep {
  char tag[16];
  char cls[8][48];
  int ncls;
  char id[16];
  int child; /* combinator to the next step is '>' */
};
struct rsel {
  struct rstep st[RS_MAX];
  int n;
  char text[512];
};

static int has_class(const struct dom_node *n, const char *c)
{
  const char *l = dom_get_attr(n, "class");
  size_t cl = strlen(c);
  for (const char *p = l ? l : ""; *p;) {
    while (*p == ' ')
      p++;
    const char *q = p;
    while (*q && *q != ' ')
      q++;
    if ((size_t)(q - p) == cl && !strncmp(p, c, cl))
      return 1;
    p = q;
  }
  return 0;
}

static int rstep_match(const struct rstep *s, const struct dom_node *n)
{
  if (!n || n->type != DOM_ELEMENT)
    return 0;
  if (s->tag[0] && strcmp(s->tag, n->tag))
    return 0;
  if (s->id[0]) {
    const char *id = dom_get_attr(n, "id");
    if (!id || strcmp(id, s->id))
      return 0;
  }
  for (int i = 0; i < s->ncls; i++)
    if (!has_class(n, s->cls[i]))
      return 0;
  return 1;
}

static int rsel_match(const struct rsel *sel, const struct dom_node *el)
{
  if (!rstep_match(&sel->st[sel->n - 1], el))
    return 0;
  const struct dom_node *cur = el;
  for (int i = sel->n - 2; i >= 0; i--) {
    cur = cur ? cur->parent : 0;
    if (sel->st[i].child) {
      if (!rstep_match(&sel->st[i], cur))
        return 0;
    } else {
      while (cur && !rstep_match(&sel->st[i], cur))
        cur = cur->parent;
      if (!cur)
        return 0;
    }
  }
  return 1;
}

static unsigned g_seed = 12345;
static unsigned rnd(unsigned n)
{
  g_seed = g_seed * 1103515245u + 12345u;
  return (g_seed >> 16) % n;
}

static const char *const g_tags[] = {"div", "p", "span", "ul", "li", "a"};
static const char *const g_cls[] = {"a", "b", "c", "d", "e", "long-class-name-beyond-twenty-three-chars"};
static const char *const g_ids[] = {"i1", "i2", "i3"};

static void gen_sel(struct rsel *s)
{
  s->n = 1 + (int)rnd(5);
  s->text[0] = '\0';
  for (int i = 0; i < s->n; i++) {
    struct rstep *st = &s->st[i];
    memset(st, 0, sizeof *st);
    if (rnd(2))
      strcpy(st->tag, g_tags[rnd(6)]);
    if (rnd(5) == 0)
      strcpy(st->id, g_ids[rnd(3)]);
    int nc = (int)rnd(3);
    for (int c = 0; c < nc; c++)
      strcpy(st->cls[st->ncls++], g_cls[rnd(6)]);
    if (!st->tag[0] && !st->id[0] && !st->ncls)
      strcpy(st->tag, "div");
    st->child = i + 1 < s->n && rnd(3) == 0;
    if (i)
      strcat(s->text, s->st[i - 1].child ? " > " : " ");
    strcat(s->text, st->tag);
    if (st->id[0]) {
      strcat(s->text, "#");
      strcat(s->text, st->id);
    }
    for (int c = 0; c < st->ncls; c++) {
      strcat(s->text, ".");
      strcat(s->text, st->cls[c]);
    }
  }
}

static void gen_html(char *out, size_t cap, int budget)
{
  int depth = 0, n = 0, tags[64];
  out[0] = '\0';
  for (int i = 0; i < budget; i++) {
    if (depth > 0 && (rnd(3) == 0 || depth > 7)) {
      fmt_at(out, cap, strlen(out), "</%s>", g_tags[tags[--depth]]);
      continue;
    }
    int t = (int)rnd(6);
    tags[depth++] = t;
    fmt_at(out, cap, strlen(out), "<%s", g_tags[t]);
    if (rnd(4) == 0)
      fmt_at(out, cap, strlen(out), " id=\"%s\"", g_ids[rnd(3)]);
    int nc = (int)rnd(4);
    if (nc) {
      strcat(out, " class=\"");
      for (int c = 0; c < nc; c++)
        fmt_at(out, cap, strlen(out), "%s%s", c ? " " : "", g_cls[rnd(6)]);
      strcat(out, "\"");
    }
    strcat(out, ">");
    n++;
  }
  while (depth > 0)
    fmt_at(out, cap, strlen(out), "</%s>", g_tags[tags[--depth]]);
}

static int collect(struct dom_node *n, struct dom_node **out, int max, int k)
{
  if (n->type == DOM_ELEMENT && k < max)
    out[k++] = n;
  for (struct dom_node *c = n->first_child; c; c = c->next)
    k = collect(c, out, max, k);
  return k;
}

/* Compare the engine's matched rule set against the reference for every element. */
static int check_all(css_sheet *sh, struct rsel *rules, int nrules, struct dom_node *root)
{
  struct dom_node *els[512];
  int ne = collect(root, els, 512, 0), bad = 0;
  for (int e = 0; e < ne; e++) {
    struct css_match m[256];
    int nm = css_matches(sh, els[e], m, 256);
    char seen[256] = {0};
    for (int i = 0; i < nm; i++)
      if (m[i].decl.width >= 0 && m[i].decl.width < 256)
        seen[m[i].decl.width] = 1;
    for (int r = 0; r < nrules; r++)
      if (seen[r] != rsel_match(&rules[r], els[e]))
        bad++;
  }
  return bad;
}

static void var_rule_result_is_memoised_and_follows_the_variable_table(void)
{
  css_sheet *sh = sheet(":root{--c:#ff0000} .a{color:var(--c)} .b{color:var(--nope,#00ff00)}");
  struct dom_node *root = parse("<div id=x class=a></div><div id=y class=b></div>");
  struct css_style cs;
  apply(sh, find(root, "x"), &cs);
  expect(cs.color == 0xff0000);
  apply(sh, find(root, "x"), &cs);
  expect(cs.color == 0xff0000);
  apply(sh, find(root, "y"), &cs);
  expect(cs.color == 0x00ff00);
  const char *more = ":root{--c:#0000ff}";
  css_add(sh, more, (int)strlen(more), "http://t/");
  apply(sh, find(root, "x"), &cs);
  expect(cs.color == 0x0000ff);
  css_free(sh);
  dom_free(root);
}

static void var_rules_with_long_declarations_and_many_variables(void)
{
  char src[16384];
  size_t pn = 0;
  pn = fmt_at(src, sizeof src, pn, ":root{");
  for (int i = 0; i < 300; i++)
    pn = fmt_at(src, sizeof src, pn, "--v%d:%dpx;", i, i + 1);
  pn = fmt_at(src, sizeof src, pn, "} .big{width:var(--v299);");
  for (int i = 0; i < 40; i++)
    pn = fmt_at(src, sizeof src, pn, "unknown-prop-%d:1px;", i);
  pn = fmt_at(src, sizeof src, pn, "margin-left:7px}");
  css_sheet *sh = sheet(src);
  struct dom_node *root = parse("<div id=x class=big></div>");
  struct css_style cs;
  apply(sh, find(root, "x"), &cs);
  expect(cs.width == 300);
  expect(cs.margin[3] == 7);
  css_free(sh);
  dom_free(root);
}

static void var_value_longer_than_220_characters_survives(void)
{
  char src[2048];
  size_t pn = 0;
  pn = fmt_at(src, sizeof src, pn, ":root{--f:");
  for (int i = 0; i < 40; i++)
    pn = fmt_at(src, sizeof src, pn, "'Fam%d', ", i);
  pn = fmt_at(src, sizeof src, pn, "sans-serif} .t{font-family:var(--f)}");
  css_sheet *sh = sheet(src);
  struct dom_node *root = parse("<div id=x class=t></div>");
  struct css_style cs;
  apply(sh, find(root, "x"), &cs);
  expect(cs.fam[0] != '\0');
  css_free(sh);
  dom_free(root);
}

static void bloom_filter_and_index_agree_with_a_naive_matcher_across_mutations(void)
{
  int bad = 0;
  for (int round = 0; round < 40; round++) {
    static struct rsel rules[200];
    char css[40000] = "", html[8000];
    int nr = 60 + (int)rnd(100);
    for (int r = 0; r < nr; r++) {
      gen_sel(&rules[r]);
      fmt_at(css, sizeof css, strlen(css), "%s{width:%dpx}", rules[r].text, r);
    }
    gen_html(html, sizeof html, 90);
    css_sheet *sh = sheet(css);
    struct dom_node *root = parse(html);
    bad += check_all(sh, rules, nr, root);
    struct dom_node *els[512];
    int ne = collect(root, els, 512, 0);
    for (int m = 0; m < 6 && ne; m++) {
      char cl[96];
      snprintf(cl, sizeof cl, "%s %s", g_cls[rnd(6)], g_cls[rnd(6)]);
      dom_set_attr(els[rnd((unsigned)ne)], "class", cl);
      bad += check_all(sh, rules, nr, root);
    }
    css_free(sh);
    dom_free(root);
  }
  expect(bad == 0);
}

static void selector_limits_many_classes_many_steps_many_pseudo_classes_long_names(void)
{
  css_sheet *sh = sheet(".c1.c2.c3.c4.c5.c6.c7{width:1px} "
                        ".s1 .s2 .s3 .s4 .s5 .s6 .s7 .s8{height:2px} "
                        "li:first-child:not(.x):not(.y):not(.z):not(.w){min-width:3px} "
                        ".abcdefghijklmnopqrstuvwxyz0123456789{min-height:4px}");
  struct dom_node *root = parse("<div id=e6 class='c1 c2 c3 c4 c5 c6'></div>"
                                "<div id=e7 class='c1 c2 c3 c4 c5 c6 c7'></div>"
                                "<div class=s1><div class=s2><div class=s3><div class=s4><div class=s5>"
                                "<div class=s6><div class=s7><div id=deep class=s8></div></div></div>"
                                "</div></div></div></div></div>"
                                "<div id=shallow class=s8></div>"
                                "<ul><li id=li1></li><li id=li2></li></ul><ul><li id=li3 class=y></li></ul>"
                                "<div id=long1 class=abcdefghijklmnopqrstuvwxyz0123456789></div>"
                                "<div id=long2 class=abcdefghijklmnopqrstuvw></div>");
  struct css_style cs;
  apply(sh, find(root, "e6"), &cs);
  expect(cs.width < 0);
  apply(sh, find(root, "e7"), &cs);
  expect(cs.width == 1);
  apply(sh, find(root, "deep"), &cs);
  expect(cs.height == 2);
  apply(sh, find(root, "shallow"), &cs);
  expect(cs.height < 0);
  apply(sh, find(root, "li1"), &cs);
  expect(cs.min_w == 3);
  apply(sh, find(root, "li2"), &cs);
  expect(cs.min_w < 0);
  apply(sh, find(root, "li3"), &cs);
  expect(cs.min_w < 0);
  apply(sh, find(root, "long1"), &cs);
  expect(cs.min_h == 4);
  apply(sh, find(root, "long2"), &cs);
  expect(cs.min_h < 0);
  css_free(sh);
  dom_free(root);
}

static void media_queries_beyond_four_features_or_conditions(void)
{
  css_set_viewport(800, 600);
  css_sheet *sh = sheet("@media (min-width:1px) and (max-width:5000px) and (min-height:1px) and (max-height:5000px) "
                        "and (orientation:landscape) and (min-width:900px){.m{width:11px}}"
                        "@media (max-width:10px),(max-width:20px),(max-width:30px),(max-width:40px),"
                        "(max-width:50px),(min-width:700px){.n{width:12px}}");
  struct dom_node *root = parse("<div id=m class=m></div><div id=n class=n></div>");
  struct css_style cs;
  apply(sh, find(root, "m"), &cs);
  expect(cs.width < 0);
  apply(sh, find(root, "n"), &cs);
  expect(cs.width == 12);
  css_set_viewport(1000, 600);
  apply(sh, find(root, "m"), &cs);
  expect(cs.width == 11);
  css_free(sh);
  dom_free(root);
}

static void keyframes_beyond_sixteen_stops(void)
{
  char src[2048];
  size_t pn = 0;
  pn = fmt_at(src, sizeof src, pn, "@keyframes k{");
  for (int i = 0; i <= 20; i++)
    pn = fmt_at(src, sizeof src, pn, "%d%%{opacity:0.%02d}", i * 5, i);
  fmt_at(src, sizeof src, pn, "}");
  css_sheet *sh = sheet(src);
  const struct css_keyframes *kf = css_keyframes_find(sh, "k");
  expect(kf && css_keyframes_count(kf) == 21);
  int pm = -1;
  const struct css_style *d = 0;
  expect(css_keyframes_stop(kf, 20, &pm, &d) && pm == 1000 && d);
  css_free(sh);
}

static void sibling_index_with_more_than_64_distinct_tag_types(void)
{
  char html[2048];
  size_t pn = 0;
  for (int i = 0; i < 70; i++)
    pn = fmt_at(html, sizeof html, pn, "<t%d id=n%d></t%d>", i, i, i);
  struct dom_node *root = parse(html);
  struct dom_node *last = find(root, "n69");
  expect(css_elem_index(last) == 70 && css_type_index(last) == 1 && css_type_count(last) == 1);
  dom_free(root);
}

static void style_invalidation_follows_the_mutation_kind(void)
{
  struct dom_node *root = parse("<div id=a><p id=b><span id=c></span></p><p id=d></p></div>");
  struct dom_node *ids[4] = {find(root, "a"), find(root, "b"), find(root, "c"), find(root, "d")};
  struct dom_node *all[8];
  int n = collect(root, all, 8, 0);
#define FRESH()                                                                                                        \
  for (int i = 0; i < n; i++)                                                                                          \
  all[i]->computed_style_seq = 5, all[i]->pseudo_seq = 5
  dom_structural_styles = false;
  FRESH();
  dom_set_attr(ids[1], "data-x", "1");
  dom_set_attr(ids[1], "value", "v");
  expect(ids[1]->computed_style_seq == 5 && ids[2]->computed_style_seq == 5);
  dom_set_attr(ids[1], "class", "k");
  expect(ids[1]->computed_style_seq == DOM_STYLE_STALE && ids[1]->pseudo_seq == DOM_STYLE_STALE);
  expect(ids[2]->computed_style_seq == DOM_STYLE_STALE);
  expect(ids[0]->computed_style_seq == 5 && ids[3]->computed_style_seq == 5);
  FRESH();
  dom_remove_attr(ids[3], "id");
  expect(ids[3]->computed_style_seq == DOM_STYLE_STALE && ids[1]->computed_style_seq == 5);
  FRESH();
  struct dom_node *t = dom_new_text("hi");
  dom_append_child(ids[2], t);
  expect(ids[0]->computed_style_seq == 5 && ids[1]->computed_style_seq == 5 && ids[2]->computed_style_seq == 5);
  struct dom_node *e = dom_new_element("b");
  dom_append_child(ids[1], e);
  expect(e->computed_style_seq == DOM_STYLE_STALE);
  expect(ids[1]->computed_style_seq == 5 && ids[2]->computed_style_seq == 5 && ids[3]->computed_style_seq == 5);
  dom_structural_styles = true;
  FRESH();
  struct dom_node *e2 = dom_new_element("b");
  dom_append_child(ids[1], e2);
  expect(ids[1]->computed_style_seq == DOM_STYLE_STALE && ids[2]->computed_style_seq == DOM_STYLE_STALE);
  expect(ids[0]->computed_style_seq == 5 && ids[3]->computed_style_seq == 5);
  FRESH();
  dom_remove_child(ids[0], ids[3]);
  expect(ids[1]->computed_style_seq == DOM_STYLE_STALE && ids[0]->computed_style_seq == DOM_STYLE_STALE);
  dom_structural_styles = false;
  dom_free(root);
  dom_free(ids[3]);
}

static void a_structural_pseudo_class_in_a_sheet_turns_on_structural_invalidation(void)
{
  dom_structural_styles = false;
  css_sheet *sh = sheet(".a .b{color:red}");
  expect(!dom_structural_styles);
  css_free(sh);
  sh = sheet("li:nth-child(2){color:red}");
  expect(dom_structural_styles);
  css_free(sh);
  dom_structural_styles = false;
}

static void animation_records_are_unbounded_per_node_and_dropped_with_the_node(void)
{
  css_sheet *sh = sheet("@keyframes fade{0%{opacity:0}100%{opacity:1}}");
  enum { N = 300 };
  struct dom_node *root = dom_new_element("div"), *kids[N];
  for (int i = 0; i < N; i++) {
    kids[i] = dom_new_element("p");
    dom_append_child(root, kids[i]);
  }
  struct css_style cs;
  anim_frame_begin(0);
  for (int i = 0; i < N; i++) {
    css_style_reset(&cs);
    strcpy(cs.anim_name, "fade");
    cs.anim_dur = 1000;
    cs.anim_timing = CSS_EASE_LINEAR;
    anim_apply(sh, kids[i], &cs, 0);
  }
  expect(g_nrecs == N && anim_active());
  anim_frame_begin(500);
  for (int i = 0; i < N; i++) {
    css_style_reset(&cs);
    strcpy(cs.anim_name, "fade");
    cs.anim_dur = 1000;
    cs.anim_timing = CSS_EASE_LINEAR;
    anim_apply(sh, kids[i], &cs, 500);
    expect(cs.opacity >= 45 && cs.opacity <= 55);
  }
  struct dom_node *plain = dom_new_element("i");
  css_style_reset(&cs);
  anim_apply(sh, plain, &cs, 500);
  expect(!plain->anim_rec && cs.opacity < 0);
  dom_free(plain);
  dom_free(root);
  expect(g_nrecs == 0);
  anim_reset();
  css_free(sh);
}

int main(void)
{
  test("var() rule result is memoised and follows the variable table",
       var_rule_result_is_memoised_and_follows_the_variable_table);
  test("var() rules with long declarations and many variables", var_rules_with_long_declarations_and_many_variables);
  test("var() value longer than 220 characters survives", var_value_longer_than_220_characters_survives);
  test("bloom filter and index agree with a naive matcher, across mutations",
       bloom_filter_and_index_agree_with_a_naive_matcher_across_mutations);
  test("selector limits: many classes, many steps, many pseudo-classes, long names",
       selector_limits_many_classes_many_steps_many_pseudo_classes_long_names);
  test("media queries beyond four features or conditions", media_queries_beyond_four_features_or_conditions);
  test("keyframes beyond sixteen stops", keyframes_beyond_sixteen_stops);
  test("sibling index with more than 64 distinct tag types", sibling_index_with_more_than_64_distinct_tag_types);
  test("style invalidation follows the mutation kind", style_invalidation_follows_the_mutation_kind);
  test("a structural pseudo-class in a sheet turns on structural invalidation",
       a_structural_pseudo_class_in_a_sheet_turns_on_structural_invalidation);
  test("animation records are unbounded, per node, and dropped with the node",
       animation_records_are_unbounded_per_node_and_dropped_with_the_node);
  return 0;
}
