/* devtools.c: in-window developer tools (F12 / Ctrl+Shift+I) with Elements, Styles and Console views.
 * The worker thread owns the live DOM and builds snapshots; the main thread only reads them under dt_lk. */
#include "devtools_int.h"

/* Snapshot/console interlock (worker writes, main reads) */
mutex_t dt_lk;

/* Panel state */
bool dt_open;
int dt_tab;
int dt_scroll[N_TABS];
volatile bool dt_dirty; /* main asks the worker to rebuild snapshots  */
/* Set from worker/fetch threads: repaint only the dock, drained by devtools_take_paint. */
volatile bool dt_paint_only;
static unsigned dt_seen_mut;          /* last dom_mutation_seq the snapshot was built for */
static unsigned dt_seen_layout;       /* last B->layout_seq (reflow generation) */
static unsigned dt_seen_app_sig;      /* last cookie/HSTS jar signature (app tab) */
static struct dom_node *dt_seen_root; /* live root the snapshot was built for (see gate) */

/* Growable text buffer with a per-line offset index (O(1) line lookup). */

struct textblk dt_dom; /* Elements view text                          */
int *dt_dom_node;      /* per-line: the pre-order index of its node   */
int dt_dom_node_cap;
struct textblk dt_sty; /* Styles view text                            */
struct textblk dt_app; /* Application view text (cookies + HSTS)       */
int dt_sel = -1;       /* selected node's pre-order index (-1 = none)  */

/* Selected-element highlight in document px; computed outside dt_lk (lock order browser_buf->dt). */
static volatile bool dt_hl_active;
static int dt_hl_x, dt_hl_y, dt_hl_w, dt_hl_h;
static char dt_hl_label[80];

/* Inspect-element picker: main thread feeds the hovered/clicked pane offset, worker resolves it. */
volatile bool dt_pick;                   /* picker armed                              */
static volatile int dt_pick_hover = -1;  /* main->worker: pane offset hovered (-1 none) */
static volatile int dt_pick_commit = -1; /* main->worker: pane offset clicked (-1 none) */
static int dt_pick_last = -2;            /* worker: last hovered node idx (dedupe repaints) */
static bool dt_pick_reveal;              /* worker: scroll Elements to the new selection */

/* Collapsed subtrees by pre-order index; per-tab, swapped in cap_swap. */
int *dt_collapsed, dt_collapsed_n, dt_collapsed_cap;
bool dt_is_collapsed(int idx)
{
  for (int i = 0; i < dt_collapsed_n; i++)
    if (dt_collapsed[i] == idx)
      return true;
  return false;
}
void dt_toggle_collapsed(int idx)
{
  for (int i = 0; i < dt_collapsed_n; i++)
    if (dt_collapsed[i] == idx) {
      dt_collapsed[i] = dt_collapsed[--dt_collapsed_n];
      return;
    }
  if (dt_collapsed_n >= dt_collapsed_cap) {
    int nc = dt_collapsed_cap ? dt_collapsed_cap * 2 : 16;
    int *p = realloc(dt_collapsed, (unsigned)nc * sizeof *p);
    if (!p)
      return;
    dt_collapsed = p;
    dt_collapsed_cap = nc;
  }
  dt_collapsed[dt_collapsed_n++] = idx;
}

/* Elements find box: substring search over the rendered DOM lines. */
char dt_find[128];
struct textfield dt_find_tf;
bool dt_find_init, dt_find_focused;

/* Breadcrumb: ancestor chain of the selected node, rebuilt by the worker under dt_lk. */
struct dt_crumb dt_crumbs[DT_CRUMB_MAX];
int dt_ncrumbs;

void dt_tb_reset(struct textblk *t)
{
  t->len = 0;
  t->nlines = 0;
  t->line_open = false;
}

void dt_tb_putc(struct textblk *t, char c)
{
  if (!t->line_open) { /* opening a fresh line */
    if (t->nlines >= t->off_cap) {
      int nc = t->off_cap ? t->off_cap * 2 : 64;
      int *p = realloc(t->off, (unsigned)nc * sizeof *p);
      if (!p)
        return;
      t->off = p;
      t->off_cap = nc;
    }
    t->off[t->nlines++] = t->len;
    t->line_open = true;
  }
  if (t->len + 1 >= t->cap) {
    int nc = t->cap ? t->cap * 2 : 1024;
    char *p = realloc(t->buf, (unsigned)nc);
    if (!p)
      return;
    t->buf = p;
    t->cap = nc;
  }
  /* Terminate the line and consume the NUL slot so the next line starts past it. */
  if (c == '\n') {
    t->buf[t->len++] = '\0';
    t->line_open = false;
    return;
  }
  t->buf[t->len++] = c;
}

void dt_tb_puts(struct textblk *t, const char *s)
{
  while (*s)
    dt_tb_putc(t, *s++);
}
void dt_tb_newline(struct textblk *t) { dt_tb_putc(t, '\n'); }

/* Copy line `i` of `t` (truncated to `max` bytes) into `out`. Returns length. */
int dt_tb_line(const struct textblk *t, int i, char *out, int max)
{
  if (i < 0 || i >= t->nlines) {
    out[0] = '\0';
    return 0;
  }
  const char *s = t->buf + t->off[i];
  int n = 0;
  while (s[n] && n < max - 1) {
    out[n] = s[n];
    n++;
  }
  out[n] = '\0';
  return n;
}

/* --- per-tab view state (main thread, on tab switch) ---------------------- */

/* Drop the current page's capture. Caller holds dt_lk. */
static void dt_reset_content(void)
{
  for (int i = 0; i < dt_con_count; i++) {
    int s = (dt_con_start + i) % CON_MAX;
    free(dt_con[s].text);
    dt_con[s].text = 0;
    dt_con_free_runs(s);
  }
  dt_con_start = dt_con_count = 0;
  free(dt_con_pend);
  dt_con_pend = 0;
  dt_con_pend_len = 0;
  dt_con_pend_cap = 0;
  for (int i = 0; i < dt_net_count; i++) {
    int s = (dt_net_start + i) % NET_MAX;
    free(dt_net[s].text);
    dt_net[s].text = 0;
    dt_netinfo_clear(&dt_netinfo[s]);
  }
  dt_net_start = dt_net_count = 0;
  dt_net_sel = -1;
  dt_collapsed_n = 0; /* pre-order indices are page-specific */
  dt_tb_reset(&dt_dom);
  dt_tb_reset(&dt_sty);
}

/* A tab's frozen devtools capture, swapped with the live one on tab switch. */
struct dt_cap {
  struct conline dt_con[CON_MAX];
  int dt_con_start, dt_con_count;
  char *dt_con_pend;
  int dt_con_pend_len, dt_con_pend_cap, dt_con_pend_level;
  struct conline dt_net[NET_MAX];
  int dt_net_start, dt_net_count;
  struct netinfo dt_netinfo[NET_MAX];
  int net_sel;
  struct textblk dom, sty;
  int *dom_node, dom_node_cap, sel;
  int *collapsed, collapsed_n, collapsed_cap;
};

/* Swap the live capture (the statics above) with *c, field for field. */
static void cap_swap(struct dt_cap *c)
{
  for (int i = 0; i < CON_MAX; i++) {
    struct conline t = dt_con[i];
    dt_con[i] = c->dt_con[i];
    c->dt_con[i] = t;
  }
  {
    int t = dt_con_start;
    dt_con_start = c->dt_con_start;
    c->dt_con_start = t;
  }
  {
    int t = dt_con_count;
    dt_con_count = c->dt_con_count;
    c->dt_con_count = t;
  }
  {
    char *t = dt_con_pend;
    dt_con_pend = c->dt_con_pend;
    c->dt_con_pend = t;
  }
  {
    int t = dt_con_pend_len;
    dt_con_pend_len = c->dt_con_pend_len;
    c->dt_con_pend_len = t;
  }
  {
    int t = dt_con_pend_cap;
    dt_con_pend_cap = c->dt_con_pend_cap;
    c->dt_con_pend_cap = t;
  }
  {
    int t = dt_con_pend_level;
    dt_con_pend_level = c->dt_con_pend_level;
    c->dt_con_pend_level = t;
  }
  for (int i = 0; i < NET_MAX; i++) {
    struct conline t = dt_net[i];
    dt_net[i] = c->dt_net[i];
    c->dt_net[i] = t;
  }
  for (int i = 0; i < NET_MAX; i++) {
    struct netinfo t = dt_netinfo[i];
    dt_netinfo[i] = c->dt_netinfo[i];
    c->dt_netinfo[i] = t;
  }
  {
    int t = dt_net_start;
    dt_net_start = c->dt_net_start;
    c->dt_net_start = t;
  }
  {
    int t = dt_net_count;
    dt_net_count = c->dt_net_count;
    c->dt_net_count = t;
  }
  {
    int t = dt_net_sel;
    dt_net_sel = c->net_sel;
    c->net_sel = t;
  }
  {
    struct textblk t = dt_dom;
    dt_dom = c->dom;
    c->dom = t;
  }
  {
    struct textblk t = dt_sty;
    dt_sty = c->sty;
    c->sty = t;
  }
  {
    int *t = dt_dom_node;
    dt_dom_node = c->dom_node;
    c->dom_node = t;
  }
  {
    int t = dt_dom_node_cap;
    dt_dom_node_cap = c->dom_node_cap;
    c->dom_node_cap = t;
  }
  {
    int t = dt_sel;
    dt_sel = c->sel;
    c->sel = t;
  }
  {
    int *t = dt_collapsed;
    dt_collapsed = c->collapsed;
    c->collapsed = t;
  }
  {
    int t = dt_collapsed_n;
    dt_collapsed_n = c->collapsed_n;
    c->collapsed_n = t;
  }
  {
    int t = dt_collapsed_cap;
    dt_collapsed_cap = c->collapsed_cap;
    c->collapsed_cap = t;
  }
}

void devtools_view_save(struct dt_view *v)
{
  v->open = dt_open;
  v->tab = dt_tab;
  for (int i = 0; i < N_TABS; i++)
    v->scroll[i] = dt_scroll[i];
  struct dt_cap *c = v->cap;
  if (!c) {
    c = calloc(1, sizeof *c);
    v->cap = c;
    if (!c)
      return;
    c->sel = -1;
    c->net_sel = -1;
  }
  mutex_lock(&dt_lk);
  cap_swap(c); /* freeze the live capture into this tab's handle */
  mutex_unlock(&dt_lk);
}

void devtools_view_load(const struct dt_view *v)
{
  dt_open = v->open;
  dt_tab = v->tab;
  dt_pick = false;
  dt_pick_hover = -1;
  dt_pick_last = -2; /* don't carry a pick across tabs */
  for (int i = 0; i < N_TABS; i++)
    dt_scroll[i] = v->scroll[i];
  mutex_lock(&dt_lk);
  if (v->cap)
    cap_swap((struct dt_cap *)v->cap); /* restore this tab's frozen capture */
  else {
    dt_reset_content();
    dt_sel = -1;
  } /* never captured: empty devtools     */
  mutex_unlock(&dt_lk);
  dt_dirty = true; /* re-resolve crumbs/highlight against the restored tab's live DOM */
  B->seq++;
}

/* Release a closed tab's frozen capture. */
void devtools_view_free(struct dt_view *v)
{
  struct dt_cap *c = v->cap;
  if (!c)
    return;
  for (int i = 0; i < CON_MAX; i++) {
    free(c->dt_con[i].text);
    free(c->dt_con[i].runs);
  }
  free(c->dt_con_pend);
  for (int i = 0; i < NET_MAX; i++) {
    free(c->dt_net[i].text);
    free(c->dt_netinfo[i].url);
    free(c->dt_netinfo[i].resp_hdr);
  }
  free(c->dom.buf);
  free(c->dom.off);
  free(c->sty.buf);
  free(c->sty.off);
  free(c->dom_node);
  free(c->collapsed);
  free(c);
  v->cap = 0;
}

/* Free the LIVE capture; used when the active tab closes. */
void devtools_reset_live(void)
{
  mutex_lock(&dt_lk);
  dt_reset_content();
  dt_sel = -1;
  mutex_unlock(&dt_lk);
}

/* Fill the page highlight (dt_hl_*) for element `sn`. Worker only; must run outside dt_lk. */
static void dt_set_highlight(struct dom_node *sn)
{
  dt_hl_active = false;
  if (!sn || sn->type == DOM_TEXT)
    return;
  int s0, e0, bx, by, bw, bh;
  if (!html_node_span(sn, &s0, &e0) || !browser_range_box(s0, e0, &bx, &by, &bw, &bh))
    return;
  dt_hl_x = bx;
  dt_hl_y = by;
  dt_hl_w = bw;
  dt_hl_h = bh;
  int k = 0;
  sapp(dt_hl_label, &k, sizeof dt_hl_label, sn->tag[0] ? sn->tag : "?");
  const char *id = dom_get_attr(sn, "id");
  if (id && id[0]) {
    sapp(dt_hl_label, &k, sizeof dt_hl_label, "#");
    sapp(dt_hl_label, &k, sizeof dt_hl_label, id);
  }
  const char *cls = dom_get_attr(sn, "class");
  if (cls && cls[0]) { /* first class only, for brevity */
    sapp(dt_hl_label, &k, sizeof dt_hl_label, ".");
    for (const char *p = cls; *p && *p != ' ' && k < (int)sizeof dt_hl_label - 1; p++)
      dt_hl_label[k++] = *p;
    dt_hl_label[k] = '\0';
  }
  sapp(dt_hl_label, &k, sizeof dt_hl_label, "  ");
  uapp(dt_hl_label, &k, sizeof dt_hl_label, (unsigned)(bw < 0 ? 0 : bw));
  sapp(dt_hl_label, &k, sizeof dt_hl_label, "x");
  uapp(dt_hl_label, &k, sizeof dt_hl_label, (unsigned)(bh < 0 ? 0 : bh));
  dt_hl_active = true; /* set last: coords are ready */
}

/* "tag", "tag#id" or "tag.firstclass" label for the breadcrumb. */
static void crumb_label(struct dom_node *n, char *out, int cap)
{
  int k = 0;
  sapp(out, &k, cap, n->tag[0] ? n->tag : "?");
  const char *id = dom_get_attr(n, "id");
  if (id && id[0]) {
    sapp(out, &k, cap, "#");
    sapp(out, &k, cap, id);
    return;
  }
  const char *cls = dom_get_attr(n, "class");
  if (cls && cls[0]) {
    sapp(out, &k, cap, ".");
    for (const char *p = cls; *p && *p != ' ' && k < cap - 1; p++)
      out[k++] = *p;
    out[k] = '\0';
  }
}

/* Rebuild the breadcrumb root-first from the selected node. Caller holds dt_lk. */
static void build_crumbs(struct dom_node *root, struct dom_node *sel)
{
  dt_ncrumbs = 0;
  struct dom_node *chain[DT_CRUMB_MAX];
  int nc = 0;
  for (struct dom_node *p = sel; p && nc < DT_CRUMB_MAX; p = p->parent)
    if (p->type == DOM_ELEMENT)
      chain[nc++] = p;
  for (int i = nc - 1; i >= 0 && dt_ncrumbs < DT_CRUMB_MAX; i--) {
    int idx = 0, found = dt_dom_index_of(root, chain[i], &idx);
    if (found < 0)
      continue;
    dt_crumbs[dt_ncrumbs].node_idx = found;
    crumb_label(chain[i], dt_crumbs[dt_ncrumbs].label, sizeof dt_crumbs[dt_ncrumbs].label);
    dt_ncrumbs++;
  }
}

/* Element under pane offset `off`; a text node resolves to its parent. Worker only. */
static struct dom_node *pick_resolve(int off)
{
  struct dom_node *n = html_node_at_offset(off);
  if (n && n->type == DOM_TEXT && n->parent)
    n = n->parent;
  return n;
}

/* Picker step: a click selects and reveals the node, otherwise highlight the hovered element. */
static void devtools_pick_tick(struct dom_node *root)
{
  if (!root)
    return;
  int commit = dt_pick_commit;
  if (commit >= 0) {
    dt_pick_commit = -1;
    struct dom_node *n = pick_resolve(commit);
    if (n) {
      int idx = 0, found = dt_dom_index_of(root, n, &idx);
      if (found >= 0) {
        dt_sel = found;
        dt_tab = TAB_ELEMENTS;
        dt_dirty = true;
        dt_pick_reveal = true;
      }
    }
    dt_pick = false;
    dt_pick_hover = -1;
    dt_pick_last = -2;
    B->seq++;
    return;
  }
  int off = dt_pick_hover;
  struct dom_node *n = off >= 0 ? pick_resolve(off) : 0;
  int idx = 0, ni = n ? dt_dom_index_of(root, n, &idx) : -1;
  if (ni == dt_pick_last)
    return; /* same target: no repaint, no rebuild */
  dt_pick_last = ni;
  dt_set_highlight(n);
  B->seq++;
}

void devtools_worker_tick(void)
{
  if (!dt_open)
    return;
  if (dt_eval_pending) { /* a REPL command: run it in the page ctx */
    dt_eval_pending = false;
    js_repl_run(dt_eval_buf); /* appends its echo + result to the console */
    dt_dirty = true;          /* the script may have mutated the DOM      */
  }

  struct dom_node *root = html_dom_root();

  /* Picker runs before the snapshot early-out so it ticks while armed. */
  if (dt_pick || dt_pick_commit >= 0)
    devtools_pick_tick(root);

  /* Rebuild snapshots only on DOM/layout change, dt_dirty or a new root (node_append bumps no seq). */
  unsigned mut = dom_mutation_seq, lay = B->layout_seq, asig = dt_app_sig();
  bool rebuild = dt_dirty || root != dt_seen_root || mut != dt_seen_mut || lay != dt_seen_layout;
  bool app_changed = asig != dt_seen_app_sig;
  if (!rebuild && !app_changed)
    return;

  mutex_lock(&dt_lk);
  if (rebuild) {
    if (root) { /* live page: rebuild Elements + Styles */
      dt_tb_reset(&dt_dom);
      int idx = 0;
      dt_dom_emit(&dt_dom, root, 0, &idx, 0);
      /* Compare with the node count, not the per-line capacity (hidden nodes get no line). */
      if (dt_sel >= idx)
        dt_sel = -1;
      dt_style_emit(root);
      dt_ncrumbs = 0;
      if (dt_sel >= 0) {
        int i2 = 0;
        struct dom_node *sn = dt_dom_node_at(root, dt_sel, &i2);
        if (sn)
          build_crumbs(root, sn);
      }
    } else if (dt_dom.nlines == 0) { /* no doc and nothing frozen: placeholder */
      dt_tb_reset(&dt_dom);
      dt_tb_puts(&dt_dom, "(no document loaded)");
      dt_tb_newline(&dt_dom);
      dt_ncrumbs = 0;
    } /* else: keep the frozen snapshot of a backgrounded tab */
  }
  if (app_changed)
    dt_app_emit(); /* cookies/HSTS, no document needed */
  /* Reveal a freshly picked node: centre its row in the Elements view. */
  if (rebuild && dt_pick_reveal && root && dt_sel >= 0) {
    dt_pick_reveal = false;
    int rows = dt_content_rows(devtools_top());
    for (int i = 0; i < dt_dom.nlines && i < dt_dom_node_cap; i++)
      if (dt_dom_node[i] == dt_sel) {
        int sc = i - rows / 2;
        if (sc < 0)
          sc = 0;
        dt_scroll[TAB_ELEMENTS] = sc;
        break;
      }
  }
  dt_dirty = false;
  mutex_unlock(&dt_lk);

  /* Highlight box, outside dt_lk; the hover highlight owns dt_hl_* while picking. */
  if (rebuild && !dt_pick) {
    dt_hl_active = false;
    if (root && dt_sel >= 0 && (dt_tab == TAB_ELEMENTS || dt_tab == TAB_STYLES)) {
      int idx = 0;
      struct dom_node *sn = dt_dom_node_at(root, dt_sel, &idx);
      dt_set_highlight(sn);
    }
  }

  dt_seen_mut = mut;
  dt_seen_layout = lay;
  dt_seen_app_sig = asig;
  dt_seen_root = root;
  B->seq++; /* repaint with the refreshed snapshot */
}

/* Selected element's page-overlay box (document px) and label; returns 0 when inactive. */
int devtools_highlight(int *x, int *y, int *w, int *h, const char **label)
{
  if (!dt_hl_active)
    return 0;
  if (x)
    *x = dt_hl_x;
  if (y)
    *y = dt_hl_y;
  if (w)
    *w = dt_hl_w;
  if (h)
    *h = dt_hl_h;
  if (label)
    *label = dt_hl_label;
  return 1;
}

/* --- main-thread layout, render, input ------------------------------------ */

void devtools_toggle(void)
{
  dt_open = !dt_open;
  if (!dt_open)
    dt_pick = false; /* closing the panel disarms the picker */
  dt_dirty = true;
  B->seq++;
}

int devtools_visible(void) { return dt_open; }

/* Consume a pending dock-only repaint request (see dt_paint_only). */
int devtools_take_paint(void)
{
  int v = dt_paint_only;
  dt_paint_only = false;
  return v;
}

/* Inspect-element picker: arming opens the panel. */
void devtools_pick_toggle(void)
{
  if (!dt_open)
    dt_open = true;
  dt_pick = !dt_pick;
  dt_pick_hover = -1;
  dt_pick_last = -2;
  dt_dirty = true; /* re-derive the highlight on disarm */
  B->seq++;
}
int devtools_picking(void) { return dt_pick; }
void devtools_pick_hover(int off)
{
  if (dt_pick)
    dt_pick_hover = off;
}
void devtools_pick_commit(int off)
{
  if (dt_pick)
    dt_pick_commit = off;
}
