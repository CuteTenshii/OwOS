/* devtools.c — the browser's in-window developer tools (F12 / Ctrl+Shift+I).
 *
 * A bottom-docked overlay with three views, switched by a tab strip:
 *   Elements  the live DOM tree, one node per line, click to select
 *   Styles    the computed CSS of the selected element (via the css.c engine)
 *   Console   the page's console.* output and uncaught JS errors
 *
 * Threading: the live DOM is owned by the worker thread (it parses, mutates and
 * frees it), so the DOM/Styles snapshots are built there in
 * devtools_worker_tick — never walked from the main thread. The console buffer
 * is appended from the worker (js.c routes console.* + errors here) and read by
 * the main thread's render. A single mutex (dt_lk) guards the snapshot text
 * and the console ring against the render thread; it is separate from
 * browser_buf_lock so console/DOM updates never
 * serialise against the page buffers. The main thread only reads the prepared
 * snapshots and draws them. */
#include "devtools_int.h"

/* --- the snapshot/console interlock (worker writes, main reads) ------------
 */
mutex_t dt_lk;

/* --- panel state -----------------------------------------------------------
 */
bool dt_open;
int dt_tab;
int dt_scroll[N_TABS];
volatile bool dt_dirty; /* main asks the worker to rebuild snapshots  */
/* A devtools-local change that repaints ONLY the dock, not the whole page: a
 * logged network request or console line. The dock is an opaque right-side
 * split, so redrawing just its region is a correct frame — a chatty page
 * logging hundreds of requests would otherwise re-lay-out and re-paint the
 * entire page per entry (via B->seq++), which is the lag. Set on the
 * worker/fetch threads, drained each frame by the main render loop
 * (devtools_take_paint). */
volatile bool dt_paint_only;
static unsigned dt_seen_mut;          /* last dom_mutation_seq the snapshot was built for */
static unsigned dt_seen_layout;       /* last B->layout_seq (reflow generation) */
static unsigned dt_seen_app_sig;      /* last cookie/HSTS jar signature (app tab) */
static struct dom_node *dt_seen_root; /* live root the snapshot was built for (see gate) */

/* --- a growable text buffer with a per-line offset index ------------------- *
 * Each snapshot (DOM tree, styles) is one char buffer holding '\n'-separated
 * lines, plus an offsets[] array so the renderer indexes a line in O(1). */

struct textblk dt_dom; /* Elements view text                          */
int *dt_dom_node;      /* per-line: the pre-order index of its node   */
int dt_dom_node_cap;
struct textblk dt_sty; /* Styles view text                            */
struct textblk dt_app; /* Application view text (cookies + HSTS)       */
int dt_sel = -1;       /* selected node's pre-order index (-1 = none)  */

/* Selected-element highlight box for the page overlay, in DOCUMENT px (the main
 * thread translates by scroll and draws it). Computed by the worker OUTSIDE
 * dt_lk — browser_range_box takes browser_buf_lock, and render() already holds
 * browser_buf_lock when it calls devtools_render() (lock order browser_buf->dt), so computing
 * it under dt_lk too would deadlock. Plain ints: a torn read just mis-paints
 * one frame of an overlay. dt_hl_active is written last (and read first) so
 * coords settle. */
static volatile bool dt_hl_active;
static int dt_hl_x, dt_hl_y, dt_hl_w, dt_hl_h;
static char dt_hl_label[80];

/* Inspect-element picker (Ctrl+Shift+C / the ↖ toolbar button): the page→tree
 * direction. The main thread feeds the pane text offset under the pointer; the
 * worker (it owns the DOM) resolves it to the enclosing element,
 * hover-highlights it via dt_hl_*, and on a click commits it to the selection
 * and reveals it in the Elements tree. Volatile handoff fields, no lock — torn
 * ints just mis-pick one frame, and the worker re-reads every 30 ms tick. */
volatile bool dt_pick;                   /* picker armed                              */
static volatile int dt_pick_hover = -1;  /* main->worker: pane offset hovered (-1 none) */
static volatile int dt_pick_commit = -1; /* main->worker: pane offset clicked (-1 none) */
static int dt_pick_last = -2;            /* worker: last hovered node idx (dedupe repaints) */
static bool dt_pick_reveal;              /* worker: scroll Elements to the new selection */

/* Collapsed subtrees, keyed by pre-order node index (stable while the tree is).
 * A collapsed node renders as "<tag…>…</tag>" on one line; the Elements walk
 * still advances the index through its hidden descendants so click->node
 * mapping holds. Per-tab: swapped with the tab's frozen capture in cap_swap
 * (pre-order indices are page-specific), and cleared whenever the page capture
 * resets. */
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

/* Elements "find" box: a substring search over the rendered DOM lines that
 * selects (and scrolls to) the next match, so it highlights on the page too. */
char dt_find[128];
struct textfield dt_find_tf;
bool dt_find_init, dt_find_focused;

/* Elements breadcrumb: the ancestor chain of the selected node (html › body ›
 * …), a clickable strip below the tree. The worker rebuilds it (DOM walk) under
 * dt_lk whenever the selection changes; the main thread draws it and maps a
 * click on a segment back to that ancestor's pre-order index. */
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
  /* Terminate the line AND consume the NUL's slot, so the next line starts past
   * it — otherwise the next char overwrites the terminator and dt_tb_line() reads
   * one run spanning every following line. */
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

/* Copy line `i` of `t` (truncated to `max` bytes) into `out`. Returns length.
 */
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

/* Drop the current page's capture. Caller holds dt_lk (the rings + snapshots
 * are shared with the worker/render threads). */
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

/* A tab's frozen devtools capture: the Elements/Styles snapshots plus the
 * console and network rings. When a tab is backgrounded its live capture is
 * swapped out into one of these (held by its dt_view), and swapped back in on
 * reveal — so the panel keeps its content across tab switches instead of
 * resetting. */
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

/* Release a closed tab's frozen capture (its rings' text + snapshot buffers).
 */
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

/* Free the LIVE capture (the statics). Used when the active tab closes: its
 * content lives in the statics, not its dt_view, so it must be dropped here
 * before the neighbour's capture is swapped in. */
void devtools_reset_live(void)
{
  mutex_lock(&dt_lk);
  dt_reset_content();
  dt_sel = -1;
  mutex_unlock(&dt_lk);
}

/* Fill the page-overlay highlight (dt_hl_*) for element `sn`: its document-px
 * box from the laid-out line table + a "tag#id.class  WxH" label. Clears the
 * highlight when the node has no rendered box. MUST run outside dt_lk —
 * browser_range_box takes browser_buf_lock and render() holds browser_buf_lock when it reads
 * dt_hl_* (lock order browser_buf->dt). Worker thread only. */
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

/* "tag", "tag#id" or "tag.firstclass" — a brief element label for the
 * breadcrumb. */
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

/* Rebuild the breadcrumb from the selected node up to the root (emitted
 * root-first), each segment carrying its pre-order index for click-to-select.
 * Caller holds dt_lk. */
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

/* The element under pane offset `off`; a text node resolves to its parent
 * element (the thing you mean to inspect). Worker thread — owns the span table.
 */
static struct dom_node *pick_resolve(int off)
{
  struct dom_node *n = html_node_at_offset(off);
  if (n && n->type == DOM_TEXT && n->parent)
    n = n->parent;
  return n;
}

/* Picker step, run before the snapshot early-out while the picker is armed. A
 * click (dt_pick_commit) selects the node, reveals it in the Elements tree and
 * ends pick mode; otherwise the hovered element is highlighted on the page.
 * Hover only bumps seq when the target element changes, so sweeping the pointer
 * costs one DOM walk per tick (not a snapshot rebuild every frame). Runs
 * outside dt_lk. */
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

  /* Inspect-element picker: cheap, before the snapshot early-out so it runs
   * every tick while armed. A commit sets dt_sel + dt_dirty, falling into the
   * rebuild. */
  if (dt_pick || dt_pick_commit >= 0)
    devtools_pick_tick(root);

  /* Rebuild the expensive Elements/Styles snapshot only when something relevant
   * changed — NOT on every global B->seq bump (cursor blink, network log,
   * scrolling the panel itself), which used to force a full DOM re-walk every
   * tick. The DOM's own mutation_seq covers structure + attributes; layout_seq
   * covers reflow (resize, late image, @font-face) so the box model and page
   * highlight track geometry; and dt_dirty covers
   * selection/tab/collapse/find/REPL. The cookie/HSTS jar has no counter, so a
   * cheap content signature gates the Application snapshot separately. The root
   * pointer catches a whole new document (navigation / first paint / a page
   * going non-NULL): the parser builds the tree with node_append, which does
   * NOT bump dom_mutation_seq, so without this a fresh page could leave the
   * stale "(no document loaded)" placeholder or a previous page's tree in the
   * panel. */
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
      /* idx is now the live pre-order node count; a selection at or past it no
       * longer exists (the tree shrank). Compare against the node count, NOT
       * the per-line array capacity — node indices count whitespace/collapsed
       * nodes that get no line, so they routinely exceed dt_dom_node_cap. */
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
    dt_app_emit(); /* cookies/HSTS — global, no document needed */
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

  /* Selected-element highlight box (outside dt_lk; browser_range_box locks
   * blk). While picking, the hover highlight owns dt_hl_* — don't clobber it
   * here. */
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

/* The selected element's page-overlay box (document px) + its label, for the
 * main thread's render. Returns 0 when no highlight is active. */
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

/* Consume a pending dock-only repaint request (see dt_paint_only). The main
 * render loop calls this each frame: a non-zero return means redraw just the
 * devtools dock — a network/console log line changed, but nothing on the page
 * did. */
int devtools_take_paint(void)
{
  int v = dt_paint_only;
  dt_paint_only = false;
  return v;
}

/* Inspect-element picker (page→tree). Arming it opens the panel; the main
 * thread feeds hovered/clicked pane offsets, the worker resolves + selects (see
 * above). */
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
