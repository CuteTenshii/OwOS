#ifndef BROWSER_CSS_ENGINE_INT_H
#define BROWSER_CSS_ENGINE_INT_H

/* Private to css/engine*.c. */
#include "color.h"
#include "css.h"
#include "owos.h"
#include "sibling.h"
#include <stdbool.h>

/* The argument of :not() — a single simple selector (tag / #id / .class, no
 * combinators or nested pseudos): enough for :not(.x), :not(div), :not(div.x).
 */
struct simple_sel {
  char tag[16];
  char **cls; /* heap class names */
  int ncls;
  char *id; /* heap, NULL = none */
};

/* Pseudo-classes we model. The structural/state ones are matched against the
 * DOM; the dynamic group lands on PC_NEVER (no live interaction state in a
 * static render, so it never matches), as does any pseudo-class we don't
 * recognise (its rule then can't apply, which is what an invalid selector
 * should do). */
enum {
  PC_FIRST_CHILD,
  PC_LAST_CHILD,
  PC_ONLY_CHILD,
  PC_FIRST_OF_TYPE,
  PC_LAST_OF_TYPE,
  PC_ONLY_OF_TYPE,
  PC_NTH_CHILD,
  PC_NTH_LAST_CHILD,
  PC_NTH_OF_TYPE,
  PC_NTH_LAST_OF_TYPE,
  PC_ROOT,
  PC_EMPTY,
  PC_NOT,
  PC_CHECKED,
  PC_DISABLED,
  PC_ENABLED,
  PC_REQUIRED,
  PC_OPTIONAL,
  PC_LINK,
  PC_MATCHES, /* :is()/:where(): matches if any sub-selector does  */
  PC_NEVER,
};

/* :is()/:where() argument: a heap list of full sub-selectors (each may carry
 * its own combinators and nested :is()). Held by pointer so struct pseudo —
 * embedded in every rule's struct selector — stays fixed-size and can nest
 * arbitrarily. */
struct sel_list;

struct pseudo {
  int kind;
  int a, b;              /* an+b for the nth-* kinds; a=1 marks :where (spec 0) */
  struct simple_sel arg; /* :not() argument (kind == PC_NOT) */
  struct sel_list *list; /* :is()/:where() sub-selectors (kind == PC_MATCHES), else 0 */
};

struct sel_step {
  char tag[16];
  char **cls; /* heap class names */
  int ncls;
  char *id; /* heap, NULL = none */
  struct pseudo *pc;
  int npc;
  int pseudo_el; /* PE_* — a ::pseudo-element; never matches a normal box */
  bool child;    /* combinator to the step on the right is '>' */
};
enum { PE_NONE = 0, PE_BEFORE = CSS_PSEUDO_BEFORE, PE_AFTER = CSS_PSEUDO_AFTER, PE_OTHER = 3 };

struct selector {
  struct sel_step *step;
  int n;
  unsigned anc_req[4]; /* ancestor-bloom bits the steps left of the subject need */
  bool anc_never;      /* an ancestor step can never match (pseudo-element) */
};

/* The concrete :is()/:where() argument list (forward-declared above). */
struct sel_list {
  int n;
  struct selector *sels;
};

/* A parsed @media query the rule lives under (n == 0 → no query, always
 * applies). A query is an OR of comma-separated conditions; each condition is a
 * media type plus ANDed features, optionally `not`-negated. See
 * media_matches(). */
enum { MT_ALL, MT_SCREEN, MT_PRINT, MT_OTHER };
enum {
  MF_UNKNOWN,
  MF_MIN_W,
  MF_MAX_W,
  MF_W,
  MF_MIN_H,
  MF_MAX_H,
  MF_H,
  MF_ORIENT_PORTRAIT,
  MF_ORIENT_LANDSCAPE,
  MF_SCHEME_LIGHT,
  MF_SCHEME_DARK
};

struct media_feature {
  int kind;
  int val;
}; /* val = px (width/height only) */
struct media_cond {
  bool negate; /* a leading `not` */
  int mtype;
  struct media_feature *feat;
  int nfeat;
};
struct media_query {
  struct media_cond *cond;
  int n;
  struct media_query *next; /* the owning sheet's list */
};

/* Rule index key: a rule is bucketed by its rightmost compound's most specific
 * simple selector (id, else a class, else tag, else universal), so an element
 * only ever tests rules that can match it — without this, Tailwind's thousands
 * of utility rules made the cascade O(elements × all-rules) and pegged the CPU.
 *
 * A rule is filed under one or more keys (see rule_keys): a plain compound
 * gives a single id/class/tag key; a subject-position :is()/:where() with no
 * key of its own gives one key per branch (e.g. :is(a.x, b.y) -> classes x and
 * y), so it leaves the universal bucket and is tested only by elements those
 * branches can match. */
enum { KK_UNIV = 0, KK_ID, KK_CLS, KK_TAG };
struct css_rule {
  struct selector sel;
  struct css_style decl;
  const struct media_query *media; /* NULL = unconditional; owned by the sheet */
  int spec, order;
  bool important;
  unsigned char has_var; /* a declaration uses var() -> resolve at fold */
  char *raw;             /* authored declaration text, whitespace-collapsed */
  struct css_style *vc;  /* decl with var() resolved, valid while vc_serial == g_vars_serial */
  unsigned vc_serial;
  unsigned visit;        /* collect_candidates de-dup stamp (multi-key) */
  struct css_rule *next; /* document order (build + fallback iteration)  */
};

/* One index entry: a rule filed in a bucket under a specific key. `key` points
 * into the rule's own selector tree (stable for the rule's lifetime), so a rule
 * keyed under several selectors needs no string copies. */
struct css_idx {
  struct css_rule *rule;
  const char *key; /* the id/class/tag string, or 0 for universal */
  struct css_idx *next;
};

/* An @font-face: a declared family name, the raw TTF/OTF src url, and the base
 * URL of the stylesheet that declared it (a relative src resolves against this,
 * not the page — an external sheet's fonts live next to the sheet). */
struct css_face {
  char family[CSS_FAM_MAX];
  char *url;
  char *base;
  struct css_face *next;
};

/* A parsed @keyframes block: a name plus a set of stops, each an offset
 * (permille, 0..1000) and the declaration delta that applies at it, kept sorted
 * by offset. The animation engine (anim.c) finds the two stops bracketing a
 * progress and interpolates their animatable properties. */
struct css_keyframes {
  char name[CSS_ANIM_NAME];
  int nstops, cap;
  short *off; /* permille 0..1000, ascending */
  struct css_style *stop;
  struct css_keyframes *next;
};

#define CSS_NBUCK 256
struct css_sheet {
  struct css_rule *head, *tail;
  int order;
  int gen;
  struct css_face *faces;
  struct css_keyframes *keyframes; /* @keyframes blocks (name -> stops) */
  struct media_query *medias;      /* every @media query parsed for this sheet */
  bool has_pseudo_el;              /* set once any ::before/::after rule is pushed */
  const char *cur_base;            /* base URL for faces added by the in-progress css_add */
  /* Rule index (rebuilt lazily when `gen` changes; see ensure_index). Buckets
   * hold css_idx entries so one rule can be filed under several keys; every
   * entry lives in exactly one bucket chain, so teardown just walks them. */
  struct css_idx *id_b[CSS_NBUCK], *cls_b[CSS_NBUCK], *tag_b[CSS_NBUCK], *uni;
  int idx_gen;
  bool idx_failed;
  struct css_rule **cand;
  int cand_cap;
  struct css_rule **match;
  int match_cap;
};

/* Root font size: the px basis for font-relative units (rem; em too — we don't
 * track an element-local font-size, so em uses the same basis). Keep in sync
 * with the renderer's default text size. */
#define CSS_ROOT_FONT_PX 16

extern int g_vw, g_vh;
extern css_sheet *g_vars_sheet;
extern int g_vars_gen;

int bg_pos_single(const char *t, int axis);
int bg_value_color(const char *v, int *alpha);
bool calc_number(const char **pp, double *out);
void copy_family_stack(const char *v, char *out, int cap);
bool ends_ci(const char *s, const char *suf);
void first_family(const char *v, char *out, int cap);
bool has_ci(const char *hay, const char *needle);
void index_clear(css_sheet *sh);
bool is_sel_name(char c);
bool media_matches(const struct media_query *mq);
short one_len(const char *v, int allow_auto);
signed char parse_align(const char *v);
void parse_bg_image(const char *v, struct css_bg_image *b);
void parse_bg_position(const char *v, short *px, short *py);
int parse_bg_repeat(const char *v);
void parse_bg_shorthand(const char *v, struct css_style *out);
void parse_bg_size(const char *v, signed char *kind, short *sw, short *sh);
int parse_border_style(const char *v);
void parse_border_styles(const char *v, signed char out[4]);
void parse_border_val(const char *v, int *w, int *col);
void parse_box4(const char *v, short out[4], int allow_auto);
void parse_box_shadow(const char *v, struct css_style *o);
int parse_color(const char *v);
void parse_content(const char *v, char *out, int cap);
void parse_decls(const char *s, const char *end, struct css_style *out, bool *important);
int parse_disp(const char *v);
void parse_flex_shorthand(const char *v, struct css_style *o);
void parse_font_shorthand(const char *v, struct css_style *out);
void parse_font_size(const char *v, struct css_style *o);
void parse_gap(const char *v, struct css_style *o);
signed char parse_grid_span(const char *v);
signed char parse_justify(const char *v);
int parse_len(const char *v);
int parse_len2(const char *v, int *pct);
void parse_line_height(const char *v, struct css_style *o);
signed char parse_list_position(const char *v);
signed char parse_list_style(const char *v);
signed char parse_opacity(const char *v);
int parse_paint_val(const char *v);
bool parse_selector(const char *s, const char *e, struct selector *sel);
int parse_tracks(const char *v, struct css_track *out, int max);
int parse_weight(const char *v);
char *resolve_vars_dup(const char *in, int inlen);
const char *scan_top(const char *s, const char *e, const char *stop);
void selector_free(struct selector *sel);
bool selector_match(const struct selector *sel, const struct dom_node *el);
bool selector_match_pseudo(const struct selector *sel, const struct dom_node *el, int which);
int selector_spec(const struct selector *sel);
bool span_is(const char *a, const char *ae, const char *lit);
bool str_uses_var(const char *s);
void vars_clear(void);

#endif
