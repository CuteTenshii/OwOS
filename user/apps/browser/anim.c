/* anim.c — CSS animation & transition engine. See anim.h.
 *
 * Model: a per-element record (keyed by dom_node*) tracks a running keyframe
 * animation and/or a running transition. Each layout walk, anim_apply() is
 * handed the element's freshly-cascaded style; it interpolates the animatable
 * properties and writes the results back over that style, so the rest of the
 * renderer draws the animated frame with no special-casing. A transition starts
 * when a watched property's cascaded value changes between walks (the usual
 * trigger: a script toggled a class or inline style). Animations take
 * precedence over transitions, per spec, so they overlay last. */
#include "anim.h"
#include "owos.h"
#include <stdbool.h>

#define ANIM_RECS 128

struct anim_rec {
  struct dom_node *node; /* key; 0 = free slot */
  unsigned last_seen;    /* frame counter, for LRU reclamation */

  /* running @keyframes animation */
  bool kf_running;
  char kf_name[CSS_ANIM_NAME];
  int kf_dur, kf_delay, kf_iter, kf_dir, kf_fill, kf_timing;
  unsigned kf_start;

  /* transition */
  bool has_base; /* base holds the previous steady cascade */
  struct css_style base;
  bool tr_running;
  unsigned tr_mask; /* which ANP_* are transitioning */
  struct css_style tr_from, tr_to;
  unsigned tr_start;
  int tr_dur, tr_delay, tr_timing;
};

static struct anim_rec g_recs[ANIM_RECS];
static unsigned g_frame; /* bumped each walk */
static bool g_more;      /* a frame is still due */

void anim_reset(void)
{
  for (int i = 0; i < ANIM_RECS; i++)
    g_recs[i].node = 0;
  g_frame = 0;
  g_more = false;
}

void anim_frame_begin(unsigned now)
{
  (void)now;
  g_frame++;
  g_more = false;
}

int anim_active(void) { return g_more; }

/* Find `node`'s record, or claim a slot for it (a free one, else the least
 * recently seen). A reclaimed slot starts blank so its stale timers don't
 * apply. */
static struct anim_rec *rec_for(struct dom_node *node)
{
  struct anim_rec *free_slot = 0, *lru = &g_recs[0];
  for (int i = 0; i < ANIM_RECS; i++) {
    struct anim_rec *r = &g_recs[i];
    if (r->node == node)
      return r;
    if (!r->node && !free_slot)
      free_slot = r;
    if (r->node && r->last_seen < lru->last_seen)
      lru = r;
  }
  struct anim_rec *r = free_slot ? free_slot : lru;
  struct anim_rec blank = {0};
  *r = blank;
  r->node = node;
  return r;
}

/* ------------------------------------------------ easing -------------------
 */
/* Cubic Bézier value on one axis for control points (0,a1)->(a2,1) at parameter
 * t (endpoints fixed at 0 and 1). */
static double bez_axis(double t, double a1, double a2)
{
  double u = 1.0 - t;
  return 3.0 * u * u * t * a1 + 3.0 * u * t * t * a2 + t * t * t;
}

/* Map linear progress x in [0,1] through an easing function to an eased value.
 */
static double ease_curve(int kind, double x)
{
  if (x <= 0)
    return 0;
  if (x >= 1)
    return 1;
  switch (kind) {
  case CSS_EASE_LINEAR:
    return x;
  case CSS_EASE_STEP_START:
    return 1; /* jumps to end immediately */
  case CSS_EASE_STEP_END:
    return 0; /* holds start until the very end */
  default:
    break;
  }
  double p1x, p1y, p2x, p2y;
  switch (kind) {
  case CSS_EASE:
    p1x = 0.25;
    p1y = 0.10;
    p2x = 0.25;
    p2y = 1.00;
    break;
  case CSS_EASE_IN:
    p1x = 0.42;
    p1y = 0.00;
    p2x = 1.00;
    p2y = 1.00;
    break;
  case CSS_EASE_OUT:
    p1x = 0.00;
    p1y = 0.00;
    p2x = 0.58;
    p2y = 1.00;
    break;
  case CSS_EASE_IN_OUT:
    p1x = 0.42;
    p1y = 0.00;
    p2x = 0.58;
    p2y = 1.00;
    break;
  default:
    return x;
  }
  /* invert x = bez_axis(t) for t by bisection, then evaluate the y axis */
  double lo = 0, hi = 1, t = x;
  for (int i = 0; i < 24; i++) {
    double xe = bez_axis(t, p1x, p2x);
    if (xe < x)
      lo = t;
    else
      hi = t;
    t = (lo + hi) * 0.5;
  }
  return bez_axis(t, p1y, p2y);
}

/* ------------------------------------------------ property access ----------
 */
static bool ap_is_color(int p)
{
  return p == ANP_COLOR || p == ANP_BG || p == ANP_BORDER_COLOR || p == ANP_OUTLINE_COLOR;
}

/* Read animatable property `p` from `s`; *set = 1 if the style specifies it
 * (not the unset sentinel). Colours come back as 0xRRGGBB. */
static long ap_get(const struct css_style *s, int p, int *set)
{
  *set = 1;
  switch (p) {
  case ANP_COLOR:
    *set = s->color >= 0;
    return s->color;
  case ANP_BG:
    *set = s->bg >= 0;
    return s->bg;
  case ANP_BORDER_COLOR:
    *set = s->border_color >= 0;
    return s->border_color;
  case ANP_OUTLINE_COLOR:
    *set = s->outline_color >= 0;
    return s->outline_color;
  case ANP_OPACITY:
    *set = s->opacity >= 0;
    return s->opacity;
  case ANP_WIDTH:
    *set = s->width >= 0;
    return s->width;
  case ANP_HEIGHT:
    *set = s->height >= 0;
    return s->height;
  case ANP_MIN_W:
    *set = s->min_w >= 0;
    return s->min_w;
  case ANP_MIN_H:
    *set = s->min_h >= 0;
    return s->min_h;
  case ANP_MAX_W:
    *set = s->max_w >= 0;
    return s->max_w;
  case ANP_MAX_H:
    *set = s->max_h >= 0;
    return s->max_h;
  case ANP_MARGIN_T:
    *set = s->margin[0] != -1;
    return s->margin[0];
  case ANP_MARGIN_R:
    *set = s->margin[1] != -1;
    return s->margin[1];
  case ANP_MARGIN_B:
    *set = s->margin[2] != -1;
    return s->margin[2];
  case ANP_MARGIN_L:
    *set = s->margin[3] != -1;
    return s->margin[3];
  case ANP_PADDING_T:
    *set = s->padding[0] != -1;
    return s->padding[0];
  case ANP_PADDING_R:
    *set = s->padding[1] != -1;
    return s->padding[1];
  case ANP_PADDING_B:
    *set = s->padding[2] != -1;
    return s->padding[2];
  case ANP_PADDING_L:
    *set = s->padding[3] != -1;
    return s->padding[3];
  case ANP_TOP:
    *set = s->top != -1;
    return s->top;
  case ANP_RIGHT:
    *set = s->right != -1;
    return s->right;
  case ANP_BOTTOM:
    *set = s->bottom != -1;
    return s->bottom;
  case ANP_LEFT:
    *set = s->left != -1;
    return s->left;
  case ANP_RADIUS:
    *set = s->radius >= 0;
    return s->radius;
  case ANP_OUTLINE_W:
    *set = s->outline_w >= 0;
    return s->outline_w;
  case ANP_FONT_SIZE:
    *set = s->font_px >= 0;
    return s->font_px;
  }
  *set = 0;
  return 0;
}

static void ap_set(struct css_style *s, int p, long v)
{
  switch (p) {
  case ANP_COLOR:
    s->color = (int)v;
    break;
  case ANP_BG:
    s->bg = (int)v;
    break;
  case ANP_BORDER_COLOR:
    s->border_color = (int)v;
    break;
  case ANP_OUTLINE_COLOR:
    s->outline_color = (int)v;
    break;
  case ANP_OPACITY:
    s->opacity = (signed char)(v < 0 ? 0 : v > 100 ? 100 : v);
    break;
  case ANP_WIDTH:
    s->width = (int)v;
    s->pct_w = -1;
    break;
  case ANP_HEIGHT:
    s->height = (int)v;
    s->pct_h = -1;
    break;
  case ANP_MIN_W:
    s->min_w = (int)v;
    break;
  case ANP_MIN_H:
    s->min_h = (int)v;
    break;
  case ANP_MAX_W:
    s->max_w = (int)v;
    break;
  case ANP_MAX_H:
    s->max_h = (int)v;
    break;
  case ANP_MARGIN_T:
    s->margin[0] = (short)v;
    break;
  case ANP_MARGIN_R:
    s->margin[1] = (short)v;
    break;
  case ANP_MARGIN_B:
    s->margin[2] = (short)v;
    break;
  case ANP_MARGIN_L:
    s->margin[3] = (short)v;
    break;
  case ANP_PADDING_T:
    s->padding[0] = (short)v;
    break;
  case ANP_PADDING_R:
    s->padding[1] = (short)v;
    break;
  case ANP_PADDING_B:
    s->padding[2] = (short)v;
    break;
  case ANP_PADDING_L:
    s->padding[3] = (short)v;
    break;
  case ANP_TOP:
    s->top = (int)v;
    break;
  case ANP_RIGHT:
    s->right = (int)v;
    break;
  case ANP_BOTTOM:
    s->bottom = (int)v;
    break;
  case ANP_LEFT:
    s->left = (int)v;
    break;
  case ANP_RADIUS:
    s->radius = (int)v;
    break;
  case ANP_OUTLINE_W:
    s->outline_w = (short)v;
    break;
  case ANP_FONT_SIZE:
    s->font_px = (int)v;
    s->font_pct = -1;
    break;
  }
}

static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static long lerp_color(int a, int b, double f)
{
  int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
  int br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
  int r = clamp8((int)(ar + (br - ar) * f + 0.5));
  int g = clamp8((int)(ag + (bg - ag) * f + 0.5));
  int bl = clamp8((int)(ab + (bb - ab) * f + 0.5));
  return (r << 16) | (g << 8) | bl;
}

static long lerp_prop(int p, long a, long b, double f)
{
  if (ap_is_color(p))
    return lerp_color((int)a, (int)b, f);
  double v = a + (b - a) * f;
  return (long)(v >= 0 ? v + 0.5 : v - 0.5);
}

/* True if transition property `p` is in mask (TRANS_ALL selects all). */
static bool prop_in_mask(unsigned mask, int p) { return (mask & TRANS_ALL) ? true : (mask & (1u << p)) != 0; }

/* ------------------------------------------------ keyframe sampling --------
 */
/* Write the keyframe styles at eased progress `p` (0..1) over `cs`,
 * interpolating between the two stops bracketing p for each animatable property
 * that both define (snapping to whichever one defines it otherwise). */
static void sample_keyframes(const struct css_keyframes *kf, double p, struct css_style *cs)
{
  int pm = (int)(p * 1000 + 0.5);
  if (pm < 0)
    pm = 0;
  if (pm > 1000)
    pm = 1000;
  int n = css_keyframes_count(kf);

  int loo = -1, hio = 1001;
  const struct css_style *lod = 0, *hid = 0;
  for (int i = 0; i < n; i++) {
    int off;
    const struct css_style *d;
    if (!css_keyframes_stop(kf, i, &off, &d))
      continue;
    if (off <= pm && off > loo) {
      loo = off;
      lod = d;
    }
    if (off >= pm && off < hio) {
      hio = off;
      hid = d;
    }
  }
  if (!lod) {
    lod = hid;
    loo = hio;
  }
  if (!hid) {
    hid = lod;
    hio = loo;
  }
  if (!lod)
    return;
  double f = (hio > loo) ? (double)(pm - loo) / (double)(hio - loo) : 0;

  for (int prop = 0; prop < ANP_COUNT; prop++) {
    int sa = 0, sb = 0;
    long a = ap_get(lod, prop, &sa), b = ap_get(hid, prop, &sb);
    if (sa && sb)
      ap_set(cs, prop, lerp_prop(prop, a, b, f));
    else if (sb)
      ap_set(cs, prop, b);
    else if (sa)
      ap_set(cs, prop, a);
  }
}

/* Overlay the running keyframe animation onto cs. Returns 1 when the animation
 * is finished (no more frames needed from it), 0 while it is still running. */
static bool apply_keyframes(css_sheet *sheet, struct anim_rec *r, struct css_style *cs, unsigned now)
{
  const struct css_keyframes *kf = css_keyframes_find(sheet, r->kf_name);
  if (!kf || css_keyframes_count(kf) == 0)
    return true;
  int dur = r->kf_dur;
  if (dur <= 0)
    return true;
  int delay = r->kf_delay > 0 ? r->kf_delay : 0;
  /* unsigned subtraction first (small, fits): (long)now would overflow 32-bit
   * long */
  long el = (long)(now - r->kf_start) - delay; /* ms into the active phase */

  bool finished = false;
  double frac; /* 0..1 within the current iteration */
  long iter_index;

  if (el < 0) {
    if (r->kf_fill == CSS_AFILL_BACKWARDS || r->kf_fill == CSS_AFILL_BOTH) {
      frac = 0;
      iter_index = 0; /* hold the first frame during delay */
    } else {
      return false; /* waiting out the delay, no overlay */
    }
  } else {
    iter_index = el / dur;
    frac = (double)(el % dur) / (double)dur;
    if (r->kf_iter >= 0 && iter_index >= r->kf_iter) {
      finished = true;
      iter_index = r->kf_iter - 1;
      frac = 1.0;
      if (r->kf_fill != CSS_AFILL_FORWARDS && r->kf_fill != CSS_AFILL_BOTH)
        return true; /* done, no fill: leave cs untouched */
    }
  }

  bool rev;
  switch (r->kf_dir) {
  case CSS_ADIR_REVERSE:
    rev = true;
    break;
  case CSS_ADIR_ALTERNATE:
    rev = (iter_index & 1) != 0;
    break;
  case CSS_ADIR_ALTERNATE_REVERSE:
    rev = !(iter_index & 1);
    break;
  default:
    rev = false;
  }
  double prog = rev ? 1.0 - frac : frac;
  sample_keyframes(kf, ease_curve(r->kf_timing, prog), cs);
  return finished;
}

/* ------------------------------------------------ transitions --------------
 */
/* Overlay the running transition onto cs at time `now`. Returns 1 when
 * finished. */
static bool apply_transition(struct anim_rec *r, unsigned now, struct css_style *cs)
{
  long el = (long)(now - r->tr_start) - r->tr_delay;
  if (el < 0)
    el = 0; /* delay: hold the `from` value */
  bool done = false;
  double f;
  if (r->tr_dur <= 0 || el >= r->tr_dur) {
    f = 1.0;
    done = true;
  } else
    f = (double)el / (double)r->tr_dur;
  double fe = ease_curve(r->tr_timing, f);
  for (int p = 0; p < ANP_COUNT; p++) {
    if (!(r->tr_mask & (1u << p)))
      continue;
    int sa = 0, sb = 0;
    long a = ap_get(&r->tr_from, p, &sa), b = ap_get(&r->tr_to, p, &sb);
    if (!sa || !sb)
      continue;
    ap_set(cs, p, lerp_prop(p, a, b, fe));
  }
  return done;
}

void anim_apply(css_sheet *sheet, struct dom_node *el, struct css_style *cs, unsigned now)
{
  if (!el)
    return;
  int wants_anim = cs->anim_name[0] && cs->anim_dur > 0;
  int wants_trans = cs->trans_mask && cs->trans_dur > 0;

  /* No animation, no transition, and nothing already running for this node:
   * skip the bookkeeping entirely (the common case for most elements). */
  struct anim_rec *r = 0;
  if (wants_anim || wants_trans)
    r = rec_for(el);
  else {
    for (int i = 0; i < ANIM_RECS; i++)
      if (g_recs[i].node == el) {
        r = &g_recs[i];
        break;
      }
    if (!r)
      return;
  }
  r->last_seen = g_frame;

  /* The freshly-cascaded animatable values are the transition baseline. */
  struct css_style newbase = *cs;

  /* ---- start a transition when a watched property changed ---- */
  if (wants_trans && r->has_base) {
    unsigned changed = 0;
    for (int p = 0; p < ANP_COUNT; p++) {
      if (!prop_in_mask(cs->trans_mask, p))
        continue;
      int sa = 0, sb = 0;
      long a = ap_get(&r->base, p, &sa), b = ap_get(&newbase, p, &sb);
      if (sa && sb && a != b)
        changed |= (1u << p);
    }
    if (changed) {
      /* from = what is currently displayed: the old steady value, or the
       * in-flight sample if a transition was already running (avoids a jump
       * when a transition is reversed mid-way). */
      struct css_style from = r->base;
      if (r->tr_running)
        apply_transition(r, now, &from);
      r->tr_from = from;
      r->tr_to = newbase;
      r->tr_mask = changed;
      r->tr_start = now;
      r->tr_dur = cs->trans_dur;
      r->tr_delay = cs->trans_delay > 0 ? cs->trans_delay : 0;
      r->tr_timing = cs->trans_timing >= 0 ? cs->trans_timing : CSS_EASE;
      r->tr_running = true;
    }
  }
  r->base = newbase;
  r->has_base = true;

  /* ---- apply the running transition ---- */
  if (r->tr_running) {
    if (apply_transition(r, now, cs))
      r->tr_running = false;
    else
      g_more = true;
  }

  /* ---- keyframe animation (wins over transitions) ---- */
  if (wants_anim) {
    if (!r->kf_running || strcmp(r->kf_name, cs->anim_name) != 0) {
      int n = 0;
      for (; cs->anim_name[n] && n < CSS_ANIM_NAME - 1; n++)
        r->kf_name[n] = cs->anim_name[n];
      r->kf_name[n] = '\0';
      r->kf_start = now;
      r->kf_running = true;
    }
    r->kf_dur = cs->anim_dur;
    r->kf_delay = cs->anim_delay > 0 ? cs->anim_delay : 0;
    r->kf_iter = cs->anim_iter >= -1 ? cs->anim_iter : 1;
    r->kf_dir = cs->anim_dir >= 0 ? cs->anim_dir : CSS_ADIR_NORMAL;
    r->kf_fill = cs->anim_fill >= 0 ? cs->anim_fill : CSS_AFILL_NONE;
    r->kf_timing = cs->anim_timing >= 0 ? cs->anim_timing : CSS_EASE;
    if (!apply_keyframes(sheet, r, cs, now))
      g_more = true;
  } else if (r->kf_running) {
    r->kf_running = false;
  }
}
