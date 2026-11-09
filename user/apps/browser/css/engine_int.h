#ifndef BROWSER_CSS_ENGINE_INT_H
#define BROWSER_CSS_ENGINE_INT_H

/* Private to css/engine*.c. */
#include "color.h"
#include "css.h"
#include "owos.h"
#include "sibling.h"
#include <stdbool.h>

/* :not() argument: one simple selector (tag, #id or .class). */
struct simple_sel {
  char tag[16];
  char **cls; /* heap class names */
  int ncls;
  char *id; /* heap, NULL = none */
};

/* Pseudo-classes; dynamic and unrecognised ones map to PC_NEVER (never match). */
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
  PC_POPOVER_OPEN,
  PC_MODAL,
  PC_ATTR,         /* [name], [name=value], ~= |= ^= $= *= with an optional i flag */
  PC_NOT_LIST,     /* :not() with a complex or comma-separated argument */
  PC_HOST,         /* :host / :host(sel): the shadow host the sheet belongs to */
  PC_HOST_CONTEXT, /* :host-context(sel): the host or one of its ancestors matches sel */
  PC_HOVER,        /* :hover, an ancestor of the pointer node */
  PC_ACTIVE,       /* :active */
  PC_FOCUS,        /* :focus and :focus-visible */
  PC_FOCUS_WITHIN, /* :focus-within */
  PC_NEVER,
};

/* :is()/:where() sub-selector list, held by pointer so struct pseudo stays fixed-size. */
struct sel_list;

struct pseudo {
  int kind;
  int a, b;              /* an+b for the nth-* kinds; a=1 marks :where (spec 0) */
  struct simple_sel arg; /* :not() argument (kind == PC_NOT) */
  struct sel_list *list; /* :is()/:where() sub-selectors (kind == PC_MATCHES), else 0 */
  char *an, *av;         /* attribute selector name and value (kind == PC_ATTR) */
  char aop;              /* 0 exists, '=', '~', '|', '^', '$', '*' */
  bool aci;              /* the i flag: compare case-insensitively */
};

struct sel_step {
  char tag[16];
  char **cls; /* heap class names */
  int ncls;
  char *id; /* heap, NULL = none */
  struct pseudo *pc;
  int npc;
  int pseudo_el;            /* PE_* ::pseudo-element; never matches a normal box */
  struct sel_list *pe_list; /* ::slotted(sel) argument */
  char *pe_names;           /* ::part(a b) space-separated names */
  bool child;               /* combinator to the step on the right is '>' */
};
enum {
  PE_NONE = 0,
  PE_BEFORE = CSS_PSEUDO_BEFORE,
  PE_AFTER = CSS_PSEUDO_AFTER,
  PE_OTHER = 3,
  PE_SLOTTED = 4,
  PE_PART = 5,
  PE_SELECTION = 6,
  PE_BACKDROP = 7,
  PE_MARKER = 8
};

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

/* Parsed @media query: OR of comma-separated conditions; n == 0 means always applies. */
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
  MF_SCHEME_DARK,
  MF_TRUE /* a feature the browser always satisfies, e.g. (hover: hover) */
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

/* Rule index key: id, else class, else tag, else universal; :is()/:where() may add several. */
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

/* One index entry: a rule filed in a bucket; `key` points into the rule's selector tree. */
struct css_idx {
  struct css_rule *rule;
  const char *key; /* the id/class/tag string, or 0 for universal */
  struct css_idx *next;
};

/* @font-face: family, raw src url, and the stylesheet base URL a relative src resolves against. */
struct css_face {
  char family[CSS_FAM_MAX];
  char *url;
  char *base;
  struct css_face *next;
};

/* @keyframes: name plus stops sorted by offset (permille), each with its declaration delta. */
struct css_keyframes {
  char name[CSS_ANIM_NAME];
  int nstops, cap;
  short *off; /* permille 0..1000, ascending */
  struct css_style *stop;
  struct css_keyframes *next;
};

struct css_var {
  char *name, *val;
};
struct var_table {
  struct css_var *v;
  int n, cap;
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
  bool has_selection;              /* set once any ::selection rule is pushed */
  bool has_marker;                 /* set once any ::marker / ::-webkit-details-marker rule is pushed */
  const char *cur_base;            /* base URL for faces added by the in-progress css_add */
  /* Rule index, rebuilt lazily when `gen` changes; each entry lives in exactly one bucket. */
  struct css_idx *id_b[CSS_NBUCK], *cls_b[CSS_NBUCK], *tag_b[CSS_NBUCK], *uni;
  int idx_gen;
  bool idx_failed;
  struct css_rule **cand;
  int cand_cap;
  struct css_rule **match;
  int match_cap;
  struct var_table lvars; /* this sheet's own custom properties, used when it is not the primary sheet */
  int lvars_gen;
};

/* Root font size: px basis for rem and em; keep in sync with the renderer's default text size. */
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
signed char parse_justify(const char *v);
int parse_len(const char *v);
int parse_len2(const char *v, int *pct);
bool parse_slen(const char *v, int *px, int *pct);
bool parse_ext_decl(const char *p, const char *pe, const char *val, struct css_style *out);
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
bool selector_subject_is_host(const struct selector *sel);
bool selector_match_slotted(const struct selector *sel, const struct dom_node *el);
bool selector_match_part(const struct selector *sel, const struct dom_node *el);
extern const struct dom_node *g_state_hover, *g_state_active, *g_state_focus;
extern bool g_state_used;
extern int g_css_mode;
extern const struct dom_node *g_css_scope_host; /* the shadow host :host refers to while a shadow sheet applies */

void vars_forget_sheet(css_sheet *sh);
void sheet_vars_free(css_sheet *sh);
bool selector_match_pseudo(const struct selector *sel, const struct dom_node *el, int which);
int selector_spec(const struct selector *sel);
bool span_is(const char *a, const char *ae, const char *lit);
bool str_uses_var(const char *s);
void vars_clear(void);

#endif
