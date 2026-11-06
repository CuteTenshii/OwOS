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
 * the main thread's render. A single spinlock (dt_lk) guards the snapshot text
 * and the console ring against the render thread; it is the same xchg pattern
 * as main.c's browser_buf_lock, but a separate lock so console/DOM updates never
 * serialise against the page buffers. The main thread only reads the prepared
 * snapshots and draws them. */
#include "browser.h"
#include "color.h" /* color_parse: %c styling maps CSS colours to console spans */
#include "css.h"
#include <stdbool.h>

/* The in-band `%c` style markers the JS engine embeds in console output. Kept
 * in sync with JS_STYLE_ON/OFF in user/lib/js/interp.h (that header pulls the
 * whole engine type graph, so it isn't included here). */
#define CON_STYLE_ON 0x01
#define CON_STYLE_OFF 0x02
#include "layout.h" /* struct lbox: computed box geometry for the Styles box model */

/* --- the snapshot/console interlock (worker writes, main reads) ------------
 */
static volatile int dt_lk;
static inline int dt_xchg(volatile int *p, int v)
{
  __asm__ volatile("xchgl %0, %1" : "+r"(v), "+m"(*p) : : "memory");
  return v;
}
static void dt_lock(void)
{
  while (dt_xchg(&dt_lk, 1))
    msleep(0);
}
static void dt_unlock(void)
{
  __asm__ volatile("" ::: "memory");
  dt_lk = 0;
}

/* --- panel state -----------------------------------------------------------
 */
enum { TAB_ELEMENTS, TAB_STYLES, TAB_CONSOLE, TAB_NETWORK, TAB_APP, N_TABS };
typedef char dt_panel_count_matches[N_TABS == DT_PANELS ? 1 : -1]; /* keep dt_view in sync */
static bool dt_open;
static int dt_tab;
static int dt_scroll[N_TABS];
static volatile bool dt_dirty; /* main asks the worker to rebuild snapshots  */
/* A devtools-local change that repaints ONLY the dock, not the whole page: a
 * logged network request or console line. The dock is an opaque right-side
 * split, so redrawing just its region is a correct frame — a chatty page
 * logging hundreds of requests would otherwise re-lay-out and re-paint the
 * entire page per entry (via B->seq++), which is the lag. Set on the
 * worker/fetch threads, drained each frame by the main render loop
 * (devtools_take_paint). */
static volatile bool dt_paint_only;
static unsigned dt_seen_mut;          /* last dom_mutation_seq the snapshot was built for */
static unsigned dt_seen_layout;       /* last B->layout_seq (reflow generation) */
static unsigned dt_seen_app_sig;      /* last cookie/HSTS jar signature (app tab) */
static struct dom_node *dt_seen_root; /* live root the snapshot was built for (see gate) */

#define DT_HDR 22 /* tab-strip height at the panel top           */
/* A code view is monospace: rows use the fixed cell font via gfx_text_utf8,
 * which decodes UTF-8 (so accents render) while keeping one column per
 * character. */
#define ROW_H GLYPH_H /* monospace cell height (16)                    */

/* --- a growable text buffer with a per-line offset index ------------------- *
 * Each snapshot (DOM tree, styles) is one char buffer holding '\n'-separated
 * lines, plus an offsets[] array so the renderer indexes a line in O(1). */
struct textblk {
  char *buf;
  int len, cap;
  int *off;
  int nlines, off_cap; /* off[i] = start of line i in buf */
  bool line_open;      /* a line has been started since the last '\n' */
};

static struct textblk dt_dom; /* Elements view text                          */
static int *dt_dom_node;      /* per-line: the pre-order index of its node   */
static int dt_dom_node_cap;
static struct textblk dt_sty; /* Styles view text                            */
static struct textblk dt_app; /* Application view text (cookies + HSTS)       */
static int dt_sel = -1;       /* selected node's pre-order index (-1 = none)  */

/* Selected-element highlight box for the page overlay, in DOCUMENT px (the main
 * thread translates by scroll and draws it). Computed by the worker OUTSIDE
 * dt_lock — browser_range_box takes browser_buf_lock, and render() already holds
 * browser_buf_lock when it calls devtools_render() (lock order browser_buf->dt), so computing
 * it under dt_lock too would deadlock. Plain ints: a torn read just mis-paints
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
static volatile bool dt_pick;            /* picker armed                              */
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
static int *dt_collapsed, dt_collapsed_n, dt_collapsed_cap;
static bool is_collapsed(int idx)
{
  for (int i = 0; i < dt_collapsed_n; i++)
    if (dt_collapsed[i] == idx)
      return true;
  return false;
}
static void toggle_collapsed(int idx)
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
static char dt_find[128];
static struct textfield dt_find_tf;
static bool dt_find_init, dt_find_focused;

/* Elements breadcrumb: the ancestor chain of the selected node (html › body ›
 * …), a clickable strip below the tree. The worker rebuilds it (DOM walk) under
 * dt_lock whenever the selection changes; the main thread draws it and maps a
 * click on a segment back to that ancestor's pre-order index. */
#define DT_CRUMB_MAX 24
struct dt_crumb {
  int node_idx;
  char label[40];
};
static struct dt_crumb dt_crumbs[DT_CRUMB_MAX];
static int dt_ncrumbs;

static void tb_reset(struct textblk *t)
{
  t->len = 0;
  t->nlines = 0;
  t->line_open = false;
}

static void tb_putc(struct textblk *t, char c)
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
   * it — otherwise the next char overwrites the terminator and tb_line() reads
   * one run spanning every following line. */
  if (c == '\n') {
    t->buf[t->len++] = '\0';
    t->line_open = false;
    return;
  }
  t->buf[t->len++] = c;
}

static void tb_puts(struct textblk *t, const char *s)
{
  while (*s)
    tb_putc(t, *s++);
}
static void tb_newline(struct textblk *t) { tb_putc(t, '\n'); }

/* Copy line `i` of `t` (truncated to `max` bytes) into `out`. Returns length.
 */
static int tb_line(const struct textblk *t, int i, char *out, int max)
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

/* --- console ring ---------------------------------------------------------- *
 * A fixed circular buffer of lines, each a heap string + a level. The worker
 * appends; the main thread reads under dt_lock. A pending accumulator collects
 * bytes between newlines (console.log sends a whole line, but write() chunks
 * are not guaranteed to land on a boundary). */
#define CON_MAX 400
/* A `%c` style span: from byte `off` in the line's (marker-stripped) text,
 * apply these overrides until the next run. Parsed out of the JS_STYLE_ON/OFF
 * markers the engine embeds (see interp.h) when con_push stores the line. */
struct crun {
  int off;
  unsigned fg, bg;
  unsigned char has_fg, has_bg, bold;
};
struct conline {
  char *text;
  unsigned char level;
  struct crun *runs;
  int nruns;
};
static struct conline con[CON_MAX];
static void con_free_runs(int slot);
static int con_start, con_count;
static char *con_pend;
static int con_pend_len, con_pend_cap;
static int con_pend_level;
static bool con_follow = true; /* keep the view pinned to the newest line      */
/* Level filter: 0 = all, 1 = warnings+errors, 2 = errors only. A line shows
 * when its level (DT_LOG=0 < DT_WARN=1 < DT_ERROR=2) is >= the filter, so the
 * value is the threshold. */
static int dt_con_filter;
static bool con_pass(int level) { return level >= dt_con_filter; }
static int con_visible_count(void)
{
  if (dt_con_filter == 0)
    return con_count;
  int c = 0;
  for (int i = 0; i < con_count; i++)
    if (con_pass(con[(con_start + i) % CON_MAX].level))
      c++;
  return c;
}
/* Drop every captured console line (the Clear button). Takes dt_lock itself. */
static void con_clear(void)
{
  dt_lock();
  for (int i = 0; i < con_count; i++) {
    int s = (con_start + i) % CON_MAX;
    free(con[s].text);
    con[s].text = 0;
    con_free_runs(s);
  }
  con_start = con_count = 0;
  free(con_pend);
  con_pend = 0;
  con_pend_len = con_pend_cap = 0;
  con_follow = true;
  dt_unlock();
}

/* --- console REPL input (main thread) -------------------------------------- *
 * A one-line editor at the bottom of the Console panel. Typed expressions are
 * handed to the worker (which owns the JS context) via the dt_eval_* handoff;
 * the worker evaluates them against the live page context and appends the
 * result to the console ring. A small history ring backs Up/Down recall. */
#define DT_INPUT_H 24
#define DT_FBTN_W 46 /* console filter / clear button width */
static char dt_input[256];
static struct textfield dt_input_tf;
static bool dt_input_init;
static bool dt_input_focused;

#define DT_HIST_MAX 32
static char *dt_hist[DT_HIST_MAX];
static int dt_hist_cnt; /* commands stored (<= DT_HIST_MAX)            */
static int dt_hist_nav; /* recall cursor; == dt_hist_cnt is the new line */

static volatile bool dt_eval_pending; /* main -> worker: a REPL command is queued    */
static char dt_eval_buf[256];         /* its source text                             */

static void dt_input_ensure(void)
{
  if (!dt_input_init) {
    textfield_init(&dt_input_tf, dt_input, sizeof dt_input);
    dt_input_init = true;
  }
}

static void dt_hist_add(const char *s)
{
  if (dt_hist_cnt == DT_HIST_MAX) { /* full: drop the oldest */
    free(dt_hist[0]);
    for (int i = 1; i < DT_HIST_MAX; i++)
      dt_hist[i - 1] = dt_hist[i];
    dt_hist_cnt--;
  }
  int n = strlen(s);
  char *d = malloc((unsigned)n + 1);
  if (d) {
    memcpy(d, s, (unsigned)n + 1);
    dt_hist[dt_hist_cnt++] = d;
  }
  dt_hist_nav = dt_hist_cnt;
}

static void dt_hist_recall(int dir)
{
  int p = dt_hist_nav + dir;
  if (p < 0)
    p = 0;
  if (p > dt_hist_cnt)
    p = dt_hist_cnt;
  dt_hist_nav = p;
  textfield_set(&dt_input_tf, p >= dt_hist_cnt ? "" : dt_hist[p]);
}

/* Parse one `%c` CSS block into the overrides of `r` (its off is preset). Only
 * the properties the console can actually render: color, background-color and a
 * bold/normal font-weight. An empty block leaves `r` a plain reset run. */
static void con_style_parse(struct crun *r, const char *css, int n)
{
  int i = 0;
  while (i < n) {
    int ns = i;
    while (i < n && css[i] != ':')
      i++;
    int ne = i;
    if (i < n)
      i++; /* skip ':' */
    int vs = i;
    while (i < n && css[i] != ';')
      i++;
    int ve = i;
    if (i < n)
      i++; /* skip ';' */
    while (ns < ne && (css[ns] == ' ' || css[ns] == '\t'))
      ns++;
    while (ne > ns && (css[ne - 1] == ' ' || css[ne - 1] == '\t'))
      ne--;
    while (vs < ve && (css[vs] == ' ' || css[vs] == '\t'))
      vs++;
    while (ve > vs && (css[ve - 1] == ' ' || css[ve - 1] == '\t'))
      ve--;
    if (ne <= ns || ve <= vs)
      continue;
    char name[24], val[80];
    int nl = ne - ns;
    if (nl > (int)sizeof name - 1)
      nl = (int)sizeof name - 1;
    for (int k = 0; k < nl; k++)
      name[k] = lower(css[ns + k]);
    name[nl] = '\0';
    int vl = ve - vs;
    if (vl > (int)sizeof val - 1)
      vl = (int)sizeof val - 1;
    memcpy(val, css + vs, (unsigned)vl);
    val[vl] = '\0';
    if (streq(name, "color")) {
      int c = color_parse(val);
      if (c >= 0) {
        r->fg = (unsigned)c;
        r->has_fg = 1;
      }
    } else if (streq(name, "background-color") || streq(name, "background")) {
      int c = color_parse(val);
      if (c >= 0) {
        r->bg = (unsigned)c;
        r->has_bg = 1;
      }
    } else if (streq(name, "font-weight")) {
      if (starts_ci(val, "bold"))
        r->bold = 1;
      else {
        int w = 0, k = 0;
        while (val[k] >= '0' && val[k] <= '9')
          w = w * 10 + (val[k++] - '0');
        if (w >= 600)
          r->bold = 1;
      }
    }
  }
}

static void con_free_runs(int slot)
{
  free(con[slot].runs);
  con[slot].runs = 0;
  con[slot].nruns = 0;
}

/* Store one console line: strip the in-band %c markers into plain display text,
 * recording each style directive as a run keyed by its offset in that text. */
static void con_push(int level, const char *s, int len)
{
  char *dup = malloc((unsigned)len + 1);
  if (!dup)
    return;
  struct crun *runs = 0;
  int nruns = 0, cap = 0, dn = 0, i = 0;
  while (i < len) {
    if (s[i] == CON_STYLE_ON) {
      int j = i + 1;
      while (j < len && s[j] != CON_STYLE_OFF)
        j++; /* css payload is [i+1, j) */
      if (nruns == cap) {
        int nc = cap ? cap * 2 : 4;
        struct crun *nr = realloc(runs, (unsigned)nc * sizeof *nr);
        if (nr) {
          runs = nr;
          cap = nc;
        }
      }
      if (nruns < cap) {
        struct crun *r = &runs[nruns++];
        r->off = dn;
        r->fg = r->bg = 0;
        r->has_fg = r->has_bg = r->bold = 0;
        con_style_parse(r, s + i + 1, j - (i + 1));
      }
      i = j < len ? j + 1 : j; /* skip the closing marker */
    } else {
      dup[dn++] = s[i++];
    }
  }
  dup[dn] = '\0';
  int slot;
  if (con_count < CON_MAX) {
    slot = (con_start + con_count) % CON_MAX;
    con_count++;
  } else { /* full: drop the oldest */
    slot = con_start;
    free(con[slot].text);
    con_free_runs(slot);
    con_start = (con_start + 1) % CON_MAX;
  }
  con[slot].text = dup;
  con[slot].level = (unsigned char)level;
  con[slot].runs = runs;
  con[slot].nruns = nruns;
}

/* Draw the wrapped row s[off,end) at (x,y) applying %c style runs: each run
 * overrides colour/background/weight from its offset onward; the default fg
 * (the level colour) fills gaps a run leaves unset. Bold is faked by a 1px
 * overstrike (the monospace face has no bold cut). Runs are in ascending-offset
 * order. */
static void con_draw_runs(int x, int y, const char *s, int off, int end, unsigned deffg, const struct crun *runs,
                          int nruns)
{
  char buf[256];
  int p = off;
  while (p < end) {
    const struct crun *r = 0;
    for (int k = 0; k < nruns; k++) {
      if (runs[k].off <= p)
        r = &runs[k];
      else
        break;
    }
    int seg = end;
    for (int k = 0; k < nruns; k++)
      if (runs[k].off > p && runs[k].off < seg)
        seg = runs[k].off;
    int n = 0;
    for (int q = p; q < seg && n < (int)sizeof buf - 1; q++)
      buf[n++] = s[q];
    buf[n] = '\0';
    unsigned fg = (r && r->has_fg) ? r->fg : deffg;
    if (r && r->has_bg)
      gfx_fill_rect(x, y, n * GLYPH_W, ROW_H, r->bg);
    int nx = gfx_text_utf8(x, y, buf, fg, GFX_TRANSPARENT);
    if (r && r->bold)
      gfx_text_utf8(x + 1, y, buf, fg, GFX_TRANSPARENT);
    x = nx;
    p = seg;
  }
}

void devtools_console_append(int level, const char *s, int len)
{
  /* Capture into the ring even while the panel is closed: errors thrown during
   * page load (before the user opens devtools) would otherwise be lost, leaving
   * an empty console. The ring is a bounded static, so this can't grow
   * unbounded; only the repaint is gated on the panel being open. */
  dt_lock();
  for (int i = 0; i < len; i++) {
    char c = s[i];
    if (c == '\n') {
      con_push(con_pend_level, con_pend ? con_pend : "", con_pend_len);
      con_pend_len = 0;
    } else {
      if (con_pend_len + 1 >= con_pend_cap) {
        int nc = con_pend_cap ? con_pend_cap * 2 : 128;
        char *p = realloc(con_pend, (unsigned)nc);
        if (!p)
          break;
        con_pend = p;
        con_pend_cap = nc;
      }
      con_pend[con_pend_len++] = c;
      con_pend_level = level; /* the level of the line being built */
    }
  }
  con_follow = true; /* a new line: jump to the bottom    */
  dt_unlock();
  if (dt_open && dt_tab == TAB_CONSOLE)
    dt_paint_only = true; /* repaint only if visible */
}

/* --- network ring ---------------------------------------------------------- *
 * One preformatted line per HTTP response the page made (logged from the fetch
 * paths on the worker / image-fetch threads), reset on each top-level
 * navigation (devtools_net_reset). Same circular-buffer shape as the console.
 */
#define NET_MAX 256
/* A conline `level` used only by net rows for the dim NET_PENDING colour —
 * never a console level (those are DT_LOG/WARN/ERROR), so the console filter is
 * unaffected. */
#define DT_NET_PENDING_LVL 3
static struct conline net[NET_MAX];
static int net_start, net_count;
/* Structured request fields, parallel to net[] (same slot/start/count), backing
 * the click-to-expand detail view. The summary string in net[] drives the list;
 * these drive the detail panel. url is heap (full, untruncated). */
struct netinfo {
  int id, state, status, size, ms;
  char method[8];
  char ctype[80];
  char err[64];
  char *url;
  char *resp_hdr;
};
static struct netinfo netinfo[NET_MAX];
static int dt_net_sel = -1; /* logical index of the expanded request, -1 none */
static int net_next_id = 1; /* monotonic id: the begin -> update handshake key */

static void netinfo_clear(struct netinfo *ni)
{
  free(ni->url);
  ni->url = 0;
  free(ni->resp_hdr);
  ni->resp_hdr = 0;
  ni->id = ni->state = ni->status = ni->size = ni->ms = 0;
  ni->method[0] = ni->ctype[0] = ni->err[0] = '\0';
}

/* Heap-dup a possibly-NULL string (the borrowed net_log fields), or NULL. */
static char *dt_strdup(const char *s)
{
  if (!s)
    return 0;
  int n = strlen(s);
  char *d = malloc((unsigned)n + 1);
  if (d)
    memcpy(d, s, (unsigned)n + 1);
  return d;
}

void devtools_net_reset(void)
{
  dt_lock();
  for (int i = 0; i < net_count; i++) {
    int s = (net_start + i) % NET_MAX;
    free(net[s].text);
    net[s].text = 0;
    netinfo_clear(&netinfo[s]);
  }
  net_start = 0;
  net_count = 0;
  dt_net_sel = -1;
  dt_unlock();
  if (dt_open)
    B->seq++;
}

/* Append a human-readable byte size: "923 B" / "12.3 KB" / "4.1 MB". */
static void net_putsize(char *b, int *n, int cap, int size)
{
  if (size < 1024) {
    uapp(b, n, cap, (unsigned)(size < 0 ? 0 : size));
    sapp(b, n, cap, " B");
    return;
  }
  int unit = size, div = 1024;
  const char *suf = "KB";
  if (size >= 1024 * 1024) {
    div = 1024 * 1024;
    suf = "MB";
  }
  uapp(b, n, cap, (unsigned)(unit / div));
  sapp(b, n, cap, ".");
  uapp(b, n, cap, (unsigned)((unit % div) * 10 / div)); /* one decimal */
  sapp(b, n, cap, " ");
  sapp(b, n, cap, suf);
}

/* One-line list summary for a request in state e->state. */
static void net_line(char *buf, int cap, int *np, const struct net_log *e)
{
  int n = 0;
  const char *method = e->method ? e->method : "GET";
  if (e->state == NET_PENDING) {
    sapp(buf, &n, cap, "...  ");
    sapp(buf, &n, cap, method);
    sapp(buf, &n, cap, "  ");
    sapp(buf, &n, cap, e->url ? e->url : "");
    *np = n;
    return;
  }
  if (e->state == NET_CANCELLED || e->state == NET_FAILED) {
    sapp(buf, &n, cap, e->state == NET_CANCELLED ? "(cancelled)  " : "(failed)  ");
    sapp(buf, &n, cap, method);
    sapp(buf, &n, cap, "  ");
    sapp(buf, &n, cap, e->url ? e->url : "");
    if (e->state == NET_FAILED && e->reason && e->reason[0]) {
      sapp(buf, &n, cap, "  - ");
      sapp(buf, &n, cap, e->reason);
    }
    *np = n;
    return;
  }
  if (e->status > 0)
    uapp(buf, &n, cap, (unsigned)e->status);
  else
    sapp(buf, &n, cap, "ERR");
  sapp(buf, &n, cap, "  ");
  sapp(buf, &n, cap, method);
  sapp(buf, &n, cap, "  ");
  net_putsize(buf, &n, cap, e->size);
  if (e->ms > 0) {
    sapp(buf, &n, cap, "  ");
    uapp(buf, &n, cap, (unsigned)e->ms);
    sapp(buf, &n, cap, " ms");
  }
  sapp(buf, &n, cap, "  ");
  sapp(buf, &n, cap, e->url ? e->url : "");
  if (e->ctype && e->ctype[0]) {
    sapp(buf, &n, cap, "  [");
    sapp(buf, &n, cap, e->ctype);
    sapp(buf, &n, cap, "]");
  }
  *np = n;
}

/* Colour bucket (a conline level) for a request in state e->state. */
static int net_level(const struct net_log *e)
{
  switch (e->state) {
  case NET_PENDING:
    return DT_NET_PENDING_LVL;
  case NET_FAILED:
    return DT_ERROR;
  case NET_CANCELLED:
    return DT_WARN;
  default:
    return (e->status == 0 || e->status >= 400) ? DT_ERROR : (e->status >= 300) ? DT_WARN : DT_LOG;
  }
}

/* (Re)write ring slot `slot` — its list line + structured detail — from e,
 * freeing any prior heap strings. The slot's id is preserved (begin assigns it
 * once; update keeps it), so a pending row resolves in place. Caller holds
 * dt_lock. */
static void net_fill(int slot, const struct net_log *e)
{
  char buf[URL_MAX + 200];
  int n = 0;
  net_line(buf, sizeof buf, &n, e);
  char *dup = malloc((unsigned)n + 1);
  if (dup) {
    memcpy(dup, buf, (unsigned)n);
    dup[n] = '\0';
  }
  free(net[slot].text);
  net[slot].text = dup;
  net[slot].level = (unsigned char)net_level(e);

  struct netinfo *ni = &netinfo[slot];
  int id = ni->id;
  netinfo_clear(ni);
  ni->id = id;
  ni->state = e->state;
  ni->status = e->status;
  ni->size = e->size;
  ni->ms = e->ms;
  {
    int k = 0;
    sapp(ni->method, &k, sizeof ni->method, e->method ? e->method : "GET");
  }
  {
    int k = 0;
    sapp(ni->ctype, &k, sizeof ni->ctype, e->ctype ? e->ctype : "");
  }
  if (e->reason) {
    int k = 0;
    sapp(ni->err, &k, sizeof ni->err, e->reason);
  }
  ni->url = dt_strdup(e->url);
  ni->resp_hdr = dt_strdup(e->resp_hdr);
}

/* Reserve the next ring slot, evicting the oldest when full. Caller holds
 * dt_lock. */
static int net_alloc_slot(void)
{
  int slot;
  if (net_count < NET_MAX) {
    slot = (net_start + net_count) % NET_MAX;
    net_count++;
  } else {
    slot = net_start;
    free(net[slot].text);
    net[slot].text = 0;
    netinfo_clear(&netinfo[slot]);
    net_start = (net_start + 1) % NET_MAX;
    /* dt_net_sel is a logical index off net_start: shift it as the oldest drops
     * so the expanded detail keeps pointing at the same request (or closes if
     * evicted). */
    if (dt_net_sel == 0)
      dt_net_sel = -1;
    else if (dt_net_sel > 0)
      dt_net_sel--;
  }
  return slot;
}

/* Append a fresh request row from e and return its id. Caller holds dt_lock. */
static int net_push_e(const struct net_log *e)
{
  int slot = net_alloc_slot();
  netinfo[slot].id = net_next_id++;
  int id = netinfo[slot].id;
  net_fill(slot, e); /* preserves the id just assigned */
  return id;
}

/* Capture into the ring even while the panel is closed (bounded static), so the
 * requests a page issues during load are still there when devtools is opened
 * afterwards; only the repaint is gated on the panel being visible. */
static void net_repaint(void)
{
  if (dt_open && dt_tab == TAB_NETWORK)
    dt_paint_only = true;
}

void devtools_net_add(const struct net_log *e)
{
  dt_lock();
  net_push_e(e);
  dt_unlock();
  net_repaint();
}

int devtools_net_begin(const char *method, const char *url)
{
  struct net_log e = {method, url, 0, 0, 0, 0, 0, NET_PENDING, 0};
  dt_lock();
  int id = net_push_e(&e);
  dt_unlock();
  net_repaint();
  return id;
}

/* Resolve the in-flight row `id` to its final state (e->state). If it was
 * already evicted from the bounded ring, the outcome is appended fresh so it is
 * never lost. */
void devtools_net_update(int id, const struct net_log *e)
{
  dt_lock();
  int slot = -1;
  for (int i = 0; i < net_count; i++) {
    int s = (net_start + i) % NET_MAX;
    if (netinfo[s].id == id) {
      slot = s;
      break;
    }
  }
  if (slot >= 0)
    net_fill(slot, e);
  else
    net_push_e(e);
  dt_unlock();
  net_repaint();
}

/* --- network type filter + response-header iteration -----------------------
 */
enum { NF_ALL, NF_DOC, NF_CSS, NF_JS, NF_IMG, NF_FONT, NF_OTHER, NF_N };
static const char *const net_filter_names[NF_N] = {"All", "Doc", "CSS", "JS", "Img", "Font", "Other"};
static int dt_net_filter; /* NF_* category shown; NF_ALL = everything    */
static int dt_nd_scroll;  /* response-header scroll in the request detail */

/* Case-insensitive: does `url` end with `suf`, ignoring a trailing
 * ?query/#frag? */
static bool url_ext_is(const char *url, const char *suf)
{
  if (!url)
    return false;
  int n = 0;
  while (url[n] && url[n] != '?' && url[n] != '#')
    n++;
  int m = strlen(suf);
  if (m > n)
    return false;
  for (int i = 0; i < m; i++)
    if (lower(url[n - m + i]) != lower(suf[i]))
      return false;
  return true;
}

/* Resource category for the type filter — Content-Type first, URL extension as
 * a fallback (image batches log an empty or wildcard image type). */
static int net_category(const struct netinfo *ni)
{
  const char *ct = ni->ctype;
  if (contains_ci(ct, "html"))
    return NF_DOC;
  if (contains_ci(ct, "css"))
    return NF_CSS;
  if (contains_ci(ct, "javascript") || contains_ci(ct, "ecmascript"))
    return NF_JS;
  if (starts_ci(ct, "image/"))
    return NF_IMG;
  if (contains_ci(ct, "font"))
    return NF_FONT;
  if (!ct[0]) { /* no type declared: guess from the URL */
    if (url_ext_is(ni->url, ".css"))
      return NF_CSS;
    if (url_ext_is(ni->url, ".js"))
      return NF_JS;
    if (url_ext_is(ni->url, ".png") || url_ext_is(ni->url, ".jpg") || url_ext_is(ni->url, ".jpeg") ||
        url_ext_is(ni->url, ".gif") || url_ext_is(ni->url, ".webp") || url_ext_is(ni->url, ".svg") ||
        url_ext_is(ni->url, ".ico"))
      return NF_IMG;
    if (url_ext_is(ni->url, ".woff") || url_ext_is(ni->url, ".woff2") || url_ext_is(ni->url, ".ttf") ||
        url_ext_is(ni->url, ".otf"))
      return NF_FONT;
    if (url_ext_is(ni->url, ".html") || url_ext_is(ni->url, ".htm"))
      return NF_DOC;
  }
  return NF_OTHER;
}
/* Does the raw ring slot pass the active type filter? */
static bool net_pass_slot(int slot) { return dt_net_filter == NF_ALL || net_category(&netinfo[slot]) == dt_net_filter; }
static int net_visible_count(void)
{
  if (dt_net_filter == NF_ALL)
    return net_count;
  int c = 0;
  for (int i = 0; i < net_count; i++)
    if (net_pass_slot((net_start + i) % NET_MAX))
      c++;
  return c;
}

/* Iterate the raw response-header block by line ('\n'-split, trailing '\r' and
 * the trailing blank terminator line dropped). Backs the Network detail headers
 * list. */
static int hdr_nlines(const char *h)
{
  if (!h)
    return 0;
  int n = 0;
  for (const char *p = h; *p;) {
    const char *e = p;
    while (*e && *e != '\n')
      e++;
    const char *t = e;
    if (t > p && t[-1] == '\r')
      t--;
    if (t > p)
      n++;
    p = *e ? e + 1 : e;
  }
  return n;
}
static int hdr_line(const char *h, int idx, char *out, int max)
{
  out[0] = '\0';
  if (!h)
    return 0;
  int n = 0;
  for (const char *p = h; *p;) {
    const char *e = p;
    while (*e && *e != '\n')
      e++;
    const char *t = e;
    if (t > p && t[-1] == '\r')
      t--;
    if (t > p) {
      if (n == idx) {
        int k = 0;
        for (const char *q = p; q < t && k < max - 1; q++)
          out[k++] = *q;
        out[k] = '\0';
        return k;
      }
      n++;
    }
    p = *e ? e + 1 : e;
  }
  return 0;
}

/* --- per-tab view state (main thread, on tab switch) ---------------------- */

/* Drop the current page's capture. Caller holds dt_lock (the rings + snapshots
 * are shared with the worker/render threads). */
static void dt_reset_content(void)
{
  for (int i = 0; i < con_count; i++) {
    int s = (con_start + i) % CON_MAX;
    free(con[s].text);
    con[s].text = 0;
    con_free_runs(s);
  }
  con_start = con_count = 0;
  free(con_pend);
  con_pend = 0;
  con_pend_len = 0;
  con_pend_cap = 0;
  for (int i = 0; i < net_count; i++) {
    int s = (net_start + i) % NET_MAX;
    free(net[s].text);
    net[s].text = 0;
    netinfo_clear(&netinfo[s]);
  }
  net_start = net_count = 0;
  dt_net_sel = -1;
  dt_collapsed_n = 0; /* pre-order indices are page-specific */
  tb_reset(&dt_dom);
  tb_reset(&dt_sty);
}

/* A tab's frozen devtools capture: the Elements/Styles snapshots plus the
 * console and network rings. When a tab is backgrounded its live capture is
 * swapped out into one of these (held by its dt_view), and swapped back in on
 * reveal — so the panel keeps its content across tab switches instead of
 * resetting. */
struct dt_cap {
  struct conline con[CON_MAX];
  int con_start, con_count;
  char *con_pend;
  int con_pend_len, con_pend_cap, con_pend_level;
  struct conline net[NET_MAX];
  int net_start, net_count;
  struct netinfo netinfo[NET_MAX];
  int net_sel;
  struct textblk dom, sty;
  int *dom_node, dom_node_cap, sel;
  int *collapsed, collapsed_n, collapsed_cap;
};

/* Swap the live capture (the statics above) with *c, field for field. */
static void cap_swap(struct dt_cap *c)
{
  for (int i = 0; i < CON_MAX; i++) {
    struct conline t = con[i];
    con[i] = c->con[i];
    c->con[i] = t;
  }
  {
    int t = con_start;
    con_start = c->con_start;
    c->con_start = t;
  }
  {
    int t = con_count;
    con_count = c->con_count;
    c->con_count = t;
  }
  {
    char *t = con_pend;
    con_pend = c->con_pend;
    c->con_pend = t;
  }
  {
    int t = con_pend_len;
    con_pend_len = c->con_pend_len;
    c->con_pend_len = t;
  }
  {
    int t = con_pend_cap;
    con_pend_cap = c->con_pend_cap;
    c->con_pend_cap = t;
  }
  {
    int t = con_pend_level;
    con_pend_level = c->con_pend_level;
    c->con_pend_level = t;
  }
  for (int i = 0; i < NET_MAX; i++) {
    struct conline t = net[i];
    net[i] = c->net[i];
    c->net[i] = t;
  }
  for (int i = 0; i < NET_MAX; i++) {
    struct netinfo t = netinfo[i];
    netinfo[i] = c->netinfo[i];
    c->netinfo[i] = t;
  }
  {
    int t = net_start;
    net_start = c->net_start;
    c->net_start = t;
  }
  {
    int t = net_count;
    net_count = c->net_count;
    c->net_count = t;
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
  dt_lock();
  cap_swap(c); /* freeze the live capture into this tab's handle */
  dt_unlock();
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
  dt_lock();
  if (v->cap)
    cap_swap((struct dt_cap *)v->cap); /* restore this tab's frozen capture */
  else {
    dt_reset_content();
    dt_sel = -1;
  } /* never captured: empty devtools     */
  dt_unlock();
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
    free(c->con[i].text);
    free(c->con[i].runs);
  }
  free(c->con_pend);
  for (int i = 0; i < NET_MAX; i++) {
    free(c->net[i].text);
    free(c->netinfo[i].url);
    free(c->netinfo[i].resp_hdr);
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
  dt_lock();
  dt_reset_content();
  dt_sel = -1;
  dt_unlock();
}

/* --- DOM snapshot (worker thread) ----------------------------------------- */

/* Append ` name="value"` for each attribute, values truncated so one element
 * line stays readable. */
static void dom_emit_attrs(struct textblk *t, struct dom_node *el)
{
  for (struct dom_attr *a = el->attrs; a; a = a->next) {
    tb_putc(t, ' ');
    tb_puts(t, a->name);
    if (a->value && a->value[0]) {
      tb_puts(t, "=\"");
      int n = 0;
      for (const char *v = a->value; *v && n < 40; v++, n++) {
        char c = *v;
        tb_putc(t, (c == '\n' || c == '\t') ? ' ' : c);
      }
      if (a->value[n])
        tb_puts(t, "...");
      tb_putc(t, '"');
    }
  }
}

/* Record the next-emitted line's owning node index in dt_dom_node (a
 * closing-tag line reuses the element's own index, so a click anywhere on the
 * element — open or close line — selects it). Call right before the line's
 * first character, when dt_dom.nlines still equals that line's number. */
static void dom_record(int my)
{
  if (dt_dom.nlines >= dt_dom_node_cap) {
    int nc = dt_dom_node_cap ? dt_dom_node_cap * 2 : 64;
    int *p = realloc(dt_dom_node, (unsigned)nc * sizeof *p);
    if (p) {
      dt_dom_node = p;
      dt_dom_node_cap = nc;
    }
  }
  if (dt_dom.nlines < dt_dom_node_cap)
    dt_dom_node[dt_dom.nlines] = my;
}

static void dom_indent(struct textblk *t, int depth)
{
  for (int d = 0; d < depth && d < 16; d++)
    tb_puts(t, "  ");
}

/* Collapsed text content (newlines/tabs → single spaces, truncated). */
static void dom_emit_text(struct textblk *t, const char *s, int maxc)
{
  int cnt = 0;
  for (; s && *s && cnt < maxc; s++) {
    char c = *s;
    if (c == '\n' || c == '\t' || c == '\r') {
      if (cnt && t->buf[t->len - 1] == ' ')
        continue; /* collapse runs */
      c = ' ';
    }
    tb_putc(t, c);
    cnt++;
  }
  if (s && *s)
    tb_puts(t, "...");
}

/* Void/self-closing elements that never get a closing tag in the tree. */
static bool dom_is_void(const char *tag)
{
  static const char *const v[] = {"area", "base", "br",    "col",    "embed", "hr",  "img", "input",
                                  "link", "meta", "param", "source", "track", "wbr", 0};
  for (int i = 0; v[i]; i++)
    if (streq(tag, v[i]))
      return true;
  return false;
}

/* True if `n`'s only child is a single (non-whitespace) text node — rendered
 * inline as `<tag>text</tag>` on one line, like a real devtools tree. */
static struct dom_node *dom_only_text(struct dom_node *n)
{
  struct dom_node *c = n->first_child;
  if (!c || c->next || c->type != DOM_TEXT)
    return 0;
  const char *s = c->text;
  while (s && *s && (*s == ' ' || *s == '\n' || *s == '\t' || *s == '\r'))
    s++;
  return (s && *s) ? c : 0;
}

/* Emit the DOM subtree rooted at `n`, indented by depth, with proper open/close
 * tags. Records each line's pre-order node index in dt_dom_node so a click maps
 * a row back to a node. Pure-whitespace text nodes are skipped, but `*idx`
 * still counts every node so the index space matches dom_node_at()'s pre-order
 * walk. */
/* Upper bounds on the snapshot walk: the tree builder caps depth at 256, but a
 * page that exhausts the 64 MiB window can leave a malformed tree, so cap here
 * too rather than recurse into garbage. The index keeps advancing past the cap
 * so click-to-node mapping stays aligned with dom_node_at()'s pre-order walk.
 */
#define DT_DOM_MAX_DEPTH 300
#define DT_DOM_MAX_NODES 50000

/* `suppress` walks a collapsed subtree to advance *idx WITHOUT emitting lines,
 * so the index space stays identical whether a node is expanded or collapsed
 * (the click->node and highlight mappings depend on that alignment). Every
 * output is guarded by !suppress; the control flow — and thus every (*idx)++ —
 * is the same. */
static void dom_emit(struct textblk *t, struct dom_node *n, int depth, int *idx, int suppress)
{
  int my = (*idx)++;
  if (!n || depth > DT_DOM_MAX_DEPTH || my > DT_DOM_MAX_NODES)
    return;

  if (n->type == DOM_TEXT) {
    const char *s = n->text;
    while (s && *s && (*s == ' ' || *s == '\n' || *s == '\t' || *s == '\r'))
      s++;
    if (s && *s && !suppress) {
      dom_record(my);
      dom_indent(t, depth);
      tb_putc(t, '"');
      dom_emit_text(t, n->text, 80);
      tb_putc(t, '"');
      tb_newline(t);
    }
    return;
  }

  const char *tag = n->tag[0] ? n->tag : "?";
  bool isvoid = dom_is_void(tag);
  /* ::before / ::after generated content shows as pseudo-element rows, like a
   * real devtools tree (not real DOM nodes, so they take no pre-order index).
   */
  bool hb = !isvoid && html_has_pseudo(n, CSS_PSEUDO_BEFORE);
  bool ha = !isvoid && html_has_pseudo(n, CSS_PSEUDO_AFTER);
  struct dom_node *only = (!isvoid && !hb && !ha) ? dom_only_text(n) : 0;
  bool empty = !n->first_child && !hb && !ha;
  bool general = !isvoid && !empty && !only; /* multi-line => collapsible */
  bool collapsed = general && is_collapsed(my);

  if (!suppress) { /* opening tag (+ a ▸/▾ on collapsible nodes) */
    dom_record(my);
    dom_indent(t, depth);
    if (general)
      tb_puts(t, collapsed ? "\xe2\x96\xba " : "\xe2\x96\xbc "); /* ► / ▼ (CP437 0x10/0x1F) */
    tb_putc(t, '<');
    tb_puts(t, tag);
    dom_emit_attrs(t, n);
    tb_putc(t, '>');
  }

  if (isvoid) { /* <br>, <img> — no close */
    if (!suppress)
      tb_newline(t);
    for (struct dom_node *c = n->first_child; c; c = c->next) /* normally none */
      dom_emit(t, c, depth + 1, idx, suppress);
    return;
  }
  if (empty) { /* <tag></tag> on one line */
    if (!suppress) {
      tb_puts(t, "</");
      tb_puts(t, tag);
      tb_putc(t, '>');
      tb_newline(t);
    }
    return;
  }
  if (only) { /* <tag>text</tag> inline */
    if (!suppress) {
      dom_emit_text(t, only->text, 80);
      tb_puts(t, "</");
      tb_puts(t, tag);
      tb_putc(t, '>');
      tb_newline(t);
    }
    (*idx)++; /* the inlined text node  */
    return;
  }
  if (collapsed) { /* "<tag…>…</tag>" + hidden subtree */
    if (!suppress) {
      tb_puts(t, "...");
      tb_puts(t, "</");
      tb_puts(t, tag);
      tb_putc(t, '>');
      tb_newline(t);
    }
    for (struct dom_node *c = n->first_child; c; c = c->next)
      dom_emit(t, c, depth + 1, idx, 1); /* advance the index only */
    return;
  }

  if (!suppress)
    tb_newline(t); /* open tag on its own line */
  if (hb && !suppress) {
    dom_record(my);
    dom_indent(t, depth + 1);
    tb_puts(t, "::before");
    tb_newline(t);
  }
  for (struct dom_node *c = n->first_child; c; c = c->next)
    dom_emit(t, c, depth + 1, idx, suppress);
  if (ha && !suppress) {
    dom_record(my);
    dom_indent(t, depth + 1);
    tb_puts(t, "::after");
    tb_newline(t);
  }
  if (!suppress) { /* closing line = same node */
    dom_record(my);
    dom_indent(t, depth);
    tb_puts(t, "</");
    tb_puts(t, tag);
    tb_putc(t, '>');
    tb_newline(t);
  }
}

/* The node at pre-order index `target` under `n`; *idx is the running counter.
 */
static struct dom_node *dom_node_at(struct dom_node *n, int target, int *idx)
{
  if ((*idx)++ == target)
    return n;
  for (struct dom_node *c = n->first_child; c; c = c->next) {
    struct dom_node *r = dom_node_at(c, target, idx);
    if (r)
      return r;
  }
  return 0;
}

/* Inverse of dom_node_at: the pre-order index of `target` under `n`, or -1.
 * Shares dom_node_at's index space exactly (count self, then children) so the
 * picker and breadcrumb map a node back to the row dom_emit() drew for it. */
static int dom_index_of(struct dom_node *n, struct dom_node *target, int *idx)
{
  int my = (*idx)++;
  if (n == target)
    return my;
  for (struct dom_node *c = n->first_child; c; c = c->next) {
    int r = dom_index_of(c, target, idx);
    if (r >= 0)
      return r;
  }
  return -1;
}

/* The laid-out box for `node`, or NULL (inline/anon elements have none). The
 * box pool has one writer — the worker — and style_emit runs on the worker, so
 * this lock-free read races only with the render thread's reads (no writer). */
static struct lbox *lbox_for(struct dom_node *node)
{
  int n = 0;
  struct lbox *p = layout_pool(&n);
  for (int i = 0; i < n; i++)
    if (p[i].node == node)
      return &p[i];
  return 0;
}

/* Append "  label: T R B L\n" for a resolved margin/border/padding edge. */
static void sty_edge(struct textblk *t, const char *label, struct ledge e)
{
  char b[48];
  int k = 0;
  sapp(b, &k, sizeof b, "  ");
  sapp(b, &k, sizeof b, label);
  sapp(b, &k, sizeof b, ": ");
  uapp(b, &k, sizeof b, (unsigned)(e.t < 0 ? 0 : e.t));
  sapp(b, &k, sizeof b, " ");
  uapp(b, &k, sizeof b, (unsigned)(e.r < 0 ? 0 : e.r));
  sapp(b, &k, sizeof b, " ");
  uapp(b, &k, sizeof b, (unsigned)(e.b < 0 ? 0 : e.b));
  sapp(b, &k, sizeof b, " ");
  uapp(b, &k, sizeof b, (unsigned)(e.l < 0 ? 0 : e.l));
  tb_puts(t, b);
  tb_newline(t);
}

/* Append "  label: #rrggbb\n", or nothing when the colour is unset (BG_NONE).
 */
static void sty_color(struct textblk *t, const char *label, unsigned c)
{
  if (c == BG_NONE)
    return;
  static const char hx[] = "0123456789abcdef";
  char b[40];
  int k = 0;
  sapp(b, &k, sizeof b, "  ");
  sapp(b, &k, sizeof b, label);
  sapp(b, &k, sizeof b, ": #");
  for (int sh = 20; sh >= 0; sh -= 4)
    b[k++] = hx[(c >> sh) & 15];
  b[k] = '\0';
  tb_puts(t, b);
  tb_newline(t);
}

/* The computed box model + a few resolved values for `n`, from its laid-out
 * box. */
static void style_emit_box(struct dom_node *n)
{
  struct lbox *lb = lbox_for(n);
  tb_newline(&dt_sty);
  tb_puts(&dt_sty, "Computed box");
  tb_newline(&dt_sty);
  if (!lb) {
    tb_puts(&dt_sty, "  (inline/anon element — no box geometry)");
    tb_newline(&dt_sty);
    return;
  }

  char b[64];
  int k = 0;
  sapp(b, &k, sizeof b, "  content: ");
  uapp(b, &k, sizeof b, (unsigned)(lb->cw < 0 ? 0 : lb->cw));
  sapp(b, &k, sizeof b, " x ");
  uapp(b, &k, sizeof b, (unsigned)(lb->ch < 0 ? 0 : lb->ch));
  tb_puts(&dt_sty, b);
  tb_newline(&dt_sty);
  sty_edge(&dt_sty, "padding", lb->padding);
  sty_edge(&dt_sty, "border", lb->border);
  sty_edge(&dt_sty, "margin", lb->margin);
  sty_color(&dt_sty, "background", lb->bg);
  if (lb->border.t || lb->border.r || lb->border.b || lb->border.l)
    sty_color(&dt_sty, "border-color", lb->border_color);
}

/* Append "  label: value\n" for a Computed-pane entry (px or free-text
 * variants). */
static void sty_kv_px(struct textblk *t, const char *label, int v)
{
  if (v < 0)
    return;
  char b[64];
  int k = 0;
  sapp(b, &k, sizeof b, "  ");
  sapp(b, &k, sizeof b, label);
  sapp(b, &k, sizeof b, ": ");
  uapp(b, &k, sizeof b, (unsigned)v);
  sapp(b, &k, sizeof b, "px");
  tb_puts(t, b);
  tb_newline(t);
}
static void sty_kv_str(struct textblk *t, const char *label, const char *val)
{
  char b[96];
  int k = 0;
  sapp(b, &k, sizeof b, "  ");
  sapp(b, &k, sizeof b, label);
  sapp(b, &k, sizeof b, ": ");
  sapp(b, &k, sizeof b, val);
  tb_puts(t, b);
  tb_newline(t);
}

/* Append "  label: <track> <track> ...\n" reconstructing a grid track template.
 */
static void sty_tracks(struct textblk *t, const char *label, const struct css_track *tr, int n)
{
  if (n <= 0)
    return;
  char b[112];
  int k = 0;
  sapp(b, &k, sizeof b, "  ");
  sapp(b, &k, sizeof b, label);
  sapp(b, &k, sizeof b, ": ");
  for (int i = 0; i < n && i < CSS_GRID_MAX; i++) {
    if (i)
      sapp(b, &k, sizeof b, " ");
    if (tr[i].kind == CSS_TRK_PX) {
      uapp(b, &k, sizeof b, (unsigned)tr[i].v);
      sapp(b, &k, sizeof b, "px");
    } else if (tr[i].kind == CSS_TRK_PCT) {
      uapp(b, &k, sizeof b, (unsigned)tr[i].v);
      sapp(b, &k, sizeof b, "%");
    } else if (tr[i].kind == CSS_TRK_FR) {
      uapp(b, &k, sizeof b, (unsigned)tr[i].v);
      sapp(b, &k, sizeof b, "fr");
    } else
      sapp(b, &k, sizeof b, "auto");
  }
  tb_puts(t, b);
  tb_newline(t);
}

/* A flat "Computed" list of the resolved values css.c models for `n` (the
 * cascade result from html_compute_style), shown after the box model like a
 * real devtools Computed pane. Only set properties are listed — most fields use
 * -1 for unset. */
static void style_emit_computed(struct dom_node *n)
{
  struct css_style s;
  html_compute_style(n, &s);
  tb_newline(&dt_sty);
  tb_puts(&dt_sty, "Computed");
  tb_newline(&dt_sty);

  if (s.fam[0])
    sty_kv_str(&dt_sty, "font-family", s.fam);
  sty_kv_px(&dt_sty, "font-size", s.font_px);
  if (s.color >= 0)
    sty_color(&dt_sty, "color", (unsigned)s.color);
  if (s.bg >= 0 && (unsigned)s.bg != BG_NONE)
    sty_color(&dt_sty, "background", (unsigned)s.bg);
  if (s.bold == 1)
    sty_kv_str(&dt_sty, "font-weight", "bold");
  if (s.under == 1)
    sty_kv_str(&dt_sty, "text-decoration", "underline");
  if (s.center == 1)
    sty_kv_str(&dt_sty, "text-align", "center");
  if (s.grid == 1) {
    sty_kv_str(&dt_sty, "display", "grid");
    sty_tracks(&dt_sty, "grid-template-columns", s.grid_cols, s.n_cols);
    sty_tracks(&dt_sty, "grid-template-rows", s.grid_rows, s.n_rows);
    if (s.grid_flow == 1)
      sty_kv_str(&dt_sty, "grid-auto-flow", "column");
  } else if (s.flex == 1) {
    sty_kv_str(&dt_sty, "display", "flex");
    if (s.flex_dir >= 0)
      sty_kv_str(&dt_sty, "flex-direction", s.flex_dir == 1 ? "column" : "row");
  } else if (s.disp == CSS_DISP_INLINE)
    sty_kv_str(&dt_sty, "display", "inline");
  else if (s.disp == CSS_DISP_NONE)
    sty_kv_str(&dt_sty, "display", "none");
  sty_kv_px(&dt_sty, "width", s.width);
  sty_kv_px(&dt_sty, "height", s.height);
  if (s.box_sizing >= 0)
    sty_kv_str(&dt_sty, "box-sizing", s.box_sizing == 1 ? "border-box" : "content-box");
  sty_kv_px(&dt_sty, "border-radius", s.radius);
  if (s.sh_color >= 0)
    sty_kv_str(&dt_sty, "box-shadow", "set");
  if (s.overflow >= 0)
    sty_kv_str(&dt_sty, "overflow", s.overflow == 1 ? "clipped" : "visible");
  if (s.gap_row >= 0)
    sty_kv_px(&dt_sty, "row-gap", s.gap_row);
  if (s.gap_col >= 0)
    sty_kv_px(&dt_sty, "column-gap", s.gap_col);
}

/* --- Application tab snapshot (worker thread; it owns cookie/HSTS writes) ----
 * * Cookies + HSTS-pinned hosts straight from the live jars
 * (B->cookies/B->hsts). Rebuilt in the worker tick like the DOM/Styles
 * snapshots, read under dt_lock. */

/* A cheap FNV-1a content signature of the live cookie/HSTS jars. The jars have
 * no mutation counter (unlike the DOM), so the worker uses this to rebuild the
 * Application snapshot only when it actually changes, not on every tick. */
static unsigned app_fnv(unsigned h, const char *s)
{
  if (s)
    for (; *s; s++) {
      h ^= (unsigned char)*s;
      h *= 16777619u;
    }
  return h;
}
static unsigned app_sig(void)
{
  unsigned h = 2166136261u;
  for (struct cookie *c = B->cookies; c; c = c->next) {
    h = app_fnv(h, c->name);
    h = app_fnv(h, c->val);
    h = app_fnv(h, c->host);
    h ^= 0x9e3779b9u;
  }
  for (struct hsts_host *hh = B->hsts; hh; hh = hh->next) {
    h = app_fnv(h, hh->host);
    h ^= 0x85ebca6bu;
  }
  return h;
}

static void app_emit(void)
{
  tb_reset(&dt_app);
  char b[16];
  int nc = 0;
  for (struct cookie *c = B->cookies; c; c = c->next)
    nc++;
  tb_puts(&dt_app, "Cookies (");
  {
    int k = 0;
    uapp(b, &k, sizeof b, (unsigned)nc);
    tb_puts(&dt_app, b);
  }
  tb_puts(&dt_app, ")");
  tb_newline(&dt_app);
  if (!nc) {
    tb_puts(&dt_app, "  (none)");
    tb_newline(&dt_app);
  }
  for (struct cookie *c = B->cookies; c; c = c->next) {
    tb_puts(&dt_app, "  ");
    tb_puts(&dt_app, c->name ? c->name : "(unnamed)");
    tb_putc(&dt_app, '=');
    dom_emit_text(&dt_app, c->val ? c->val : "", 64);
    tb_puts(&dt_app, "   @ ");
    tb_puts(&dt_app, c->host ? c->host : "");
    tb_newline(&dt_app);
  }
  tb_newline(&dt_app);
  int nh = 0;
  for (struct hsts_host *h = B->hsts; h; h = h->next)
    nh++;
  tb_puts(&dt_app, "HSTS (");
  {
    int k = 0;
    uapp(b, &k, sizeof b, (unsigned)nh);
    tb_puts(&dt_app, b);
  }
  tb_puts(&dt_app, ")");
  tb_newline(&dt_app);
  if (!nh) {
    tb_puts(&dt_app, "  (none)");
    tb_newline(&dt_app);
  }
  for (struct hsts_host *h = B->hsts; h; h = h->next) {
    tb_puts(&dt_app, "  ");
    tb_puts(&dt_app, h->host ? h->host : "");
    tb_newline(&dt_app);
  }
}

/* Build the Styles view for the selected node, like a real devtools pane: the
 * inline style="" block, then every matching rule (winner first) with its
 * source — the selector and, when the rule lives under an @media, that prelude
 * — so it's clear which class / media query contributed. css.c models a subset
 * of CSS, so a rule that set only unmodeled properties shows an explanatory
 * note. Ends with the computed box model from the laid-out box tree. */
static void style_emit(struct dom_node *root)
{
  tb_reset(&dt_sty);
  if (!root) {
    tb_puts(&dt_sty, "(no document loaded)");
    tb_newline(&dt_sty);
    return;
  }
  if (dt_sel < 0) {
    tb_puts(&dt_sty, "Select an element in the Elements tab.");
    tb_newline(&dt_sty);
    return;
  }
  int idx = 0;
  struct dom_node *n = dom_node_at(root, dt_sel, &idx);
  if (!n) {
    tb_puts(&dt_sty, "(selection no longer in the tree)");
    tb_newline(&dt_sty);
    return;
  }
  if (n->type == DOM_TEXT) {
    tb_puts(&dt_sty, "(text node — no styles)");
    tb_newline(&dt_sty);
    return;
  }

  tb_putc(&dt_sty, '<');
  tb_puts(&dt_sty, n->tag);
  const char *id = dom_get_attr(n, "id");
  if (id && id[0]) {
    tb_putc(&dt_sty, '#');
    tb_puts(&dt_sty, id);
  }
  const char *cls = dom_get_attr(n, "class");
  if (cls && cls[0]) {
    for (const char *p = cls; *p;) {
      while (*p == ' ')
        p++;
      if (!*p)
        break;
      tb_putc(&dt_sty, '.');
      while (*p && *p != ' ')
        tb_putc(&dt_sty, *p++);
    }
  }
  tb_putc(&dt_sty, '>');
  tb_newline(&dt_sty);
  tb_newline(&dt_sty);

  /* element.style {} — the inline style="" attribute, verbatim. */
  const char *inl = dom_get_attr(n, "style");
  if (inl && inl[0]) {
    tb_puts(&dt_sty, "element.style {");
    tb_newline(&dt_sty);
    tb_puts(&dt_sty, "  ");
    for (const char *p = inl; *p; p++) { /* one declaration per line */
      tb_putc(&dt_sty, *p);
      if (*p == ';' && p[1]) {
        tb_newline(&dt_sty);
        tb_puts(&dt_sty, "  ");
      }
    }
    tb_newline(&dt_sty);
    tb_puts(&dt_sty, "}");
    tb_newline(&dt_sty);
    tb_newline(&dt_sty);
  }

  /* Matching rules, winner first, grouped under their @media when any. */
  struct css_match m[64];
  int nm = html_match_styles(n, m, 64);
  for (int i = 0; i < nm; i++) {
    const char *base = "";
    if (m[i].media[0]) {
      tb_puts(&dt_sty, m[i].media);
      if (!m[i].active)
        tb_puts(&dt_sty, "   /* not currently matched */");
      tb_puts(&dt_sty, " {");
      tb_newline(&dt_sty);
      base = "  ";
    }
    tb_puts(&dt_sty, base);
    tb_puts(&dt_sty, m[i].selector);
    tb_puts(&dt_sty, " {");
    tb_newline(&dt_sty);

    char ind[8];
    int k = 0;
    while (base[k]) {
      ind[k] = base[k];
      k++;
    }
    ind[k++] = ' ';
    ind[k++] = ' ';
    ind[k] = '\0';
    /* The authored declarations, verbatim — including properties this engine
     * doesn't model (shown like a real devtools, not silently dropped). */
    if (m[i].raw && m[i].raw[0]) {
      const char *seg = m[i].raw;
      while (*seg) {
        const char *e = seg, *s2;
        while (*e && *e != ';')
          e++;
        s2 = seg;
        while (s2 < e && *s2 == ' ')
          s2++;
        if (e > s2) {
          tb_puts(&dt_sty, ind);
          while (s2 < e)
            tb_putc(&dt_sty, *s2++);
          tb_putc(&dt_sty, ';');
          tb_newline(&dt_sty);
        }
        seg = *e ? e + 1 : e;
      }
    } else {
      tb_puts(&dt_sty, ind);
      tb_puts(&dt_sty, "/* (empty rule) */");
      tb_newline(&dt_sty);
    }
    tb_puts(&dt_sty, base);
    tb_puts(&dt_sty, "}");
    tb_newline(&dt_sty);
    if (m[i].media[0]) {
      tb_puts(&dt_sty, "}");
      tb_newline(&dt_sty);
    }
  }
  if (nm == 0 && !(inl && inl[0])) {
    tb_puts(&dt_sty, "No matching CSS rules.");
    tb_newline(&dt_sty);
  }

  style_emit_box(n);
  style_emit_computed(n);
}

static int content_rows(int top); /* defined in the layout section below */

/* Fill the page-overlay highlight (dt_hl_*) for element `sn`: its document-px
 * box from the laid-out line table + a "tag#id.class  WxH" label. Clears the
 * highlight when the node has no rendered box. MUST run outside dt_lock —
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
 * Caller holds dt_lock. */
static void build_crumbs(struct dom_node *root, struct dom_node *sel)
{
  dt_ncrumbs = 0;
  struct dom_node *chain[DT_CRUMB_MAX];
  int nc = 0;
  for (struct dom_node *p = sel; p && nc < DT_CRUMB_MAX; p = p->parent)
    if (p->type == DOM_ELEMENT)
      chain[nc++] = p;
  for (int i = nc - 1; i >= 0 && dt_ncrumbs < DT_CRUMB_MAX; i--) {
    int idx = 0, found = dom_index_of(root, chain[i], &idx);
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
 * outside dt_lock. */
static void devtools_pick_tick(struct dom_node *root)
{
  if (!root)
    return;
  int commit = dt_pick_commit;
  if (commit >= 0) {
    dt_pick_commit = -1;
    struct dom_node *n = pick_resolve(commit);
    if (n) {
      int idx = 0, found = dom_index_of(root, n, &idx);
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
  int idx = 0, ni = n ? dom_index_of(root, n, &idx) : -1;
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
  unsigned mut = dom_mutation_seq, lay = B->layout_seq, asig = app_sig();
  bool rebuild = dt_dirty || root != dt_seen_root || mut != dt_seen_mut || lay != dt_seen_layout;
  bool app_changed = asig != dt_seen_app_sig;
  if (!rebuild && !app_changed)
    return;

  dt_lock();
  if (rebuild) {
    if (root) { /* live page: rebuild Elements + Styles */
      tb_reset(&dt_dom);
      int idx = 0;
      dom_emit(&dt_dom, root, 0, &idx, 0);
      /* idx is now the live pre-order node count; a selection at or past it no
       * longer exists (the tree shrank). Compare against the node count, NOT
       * the per-line array capacity — node indices count whitespace/collapsed
       * nodes that get no line, so they routinely exceed dt_dom_node_cap. */
      if (dt_sel >= idx)
        dt_sel = -1;
      style_emit(root);
      dt_ncrumbs = 0;
      if (dt_sel >= 0) {
        int i2 = 0;
        struct dom_node *sn = dom_node_at(root, dt_sel, &i2);
        if (sn)
          build_crumbs(root, sn);
      }
    } else if (dt_dom.nlines == 0) { /* no doc and nothing frozen: placeholder */
      tb_reset(&dt_dom);
      tb_puts(&dt_dom, "(no document loaded)");
      tb_newline(&dt_dom);
      dt_ncrumbs = 0;
    } /* else: keep the frozen snapshot of a backgrounded tab */
  }
  if (app_changed)
    app_emit(); /* cookies/HSTS — global, no document needed */
  /* Reveal a freshly picked node: centre its row in the Elements view. */
  if (rebuild && dt_pick_reveal && root && dt_sel >= 0) {
    dt_pick_reveal = false;
    int rows = content_rows(devtools_top());
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
  dt_unlock();

  /* Selected-element highlight box (outside dt_lock; browser_range_box locks
   * blk). While picking, the hover highlight owns dt_hl_* — don't clobber it
   * here. */
  if (rebuild && !dt_pick) {
    dt_hl_active = false;
    if (root && dt_sel >= 0 && (dt_tab == TAB_ELEMENTS || dt_tab == TAB_STYLES)) {
      int idx = 0;
      struct dom_node *sn = dom_node_at(root, dt_sel, &idx);
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

/* Right-side split (content | devtools), not an overlay: the panel docks on the
 * right for the full content height, and the page pane shrinks to its left. */
#define DT_MINW 240
static int dt_user_w = 0; /* 0 = auto (default fraction); >0 = user-dragged */
static int dt_width(void)
{
  int w = dt_user_w > 0 ? dt_user_w : BW * 5 / 12;
  if (w > BW - 220)
    w = BW - 220; /* always leave the page a usable width */
  if (w < DT_MINW)
    w = DT_MINW;
  if (w > BW - 80)
    w = BW - 80; /* tiny window: degrade gracefully      */
  if (w < 60)
    w = 60;
  return w;
}
/* Drag-to-resize: the panel docks right, so its width is (BW - pointer x). */
void devtools_set_width(int w)
{
  dt_user_w = w;
  B->seq++;
}
/* Is (mx,my) on the panel's left-edge splitter (the drag handle)? */
int devtools_on_splitter(int mx, int my)
{
  int x;
  if (!dt_open)
    return 0;
  x = devtools_left();
  return mx >= x - 3 && mx <= x + 3 && my >= devtools_top() && my < BH;
}

/* X of the panel's left edge — equals BW when closed, so the page gets the full
 * width and callers can use [0, devtools_left()) as the page region. */
int devtools_left(void) { return dt_open ? BW - dt_width() : BW; }

/* Y of the panel's top edge: just below the toolbar (tab strip + address bar).
 */
int devtools_top(void) { return MENU_H + TAB_H + ADDR_H; }

static const char *const tab_names[N_TABS] = {"Elements", "Styles", "Console", "Network", "Application"};

/* The header is a row of buttons (the shared gfx button widget): an
 * inspect-element toggle (↖) at the left, then one per view (the active one
 * drawn pressed), plus a trailing close button. Geometry is shared by render
 * and the click handler so the drawn and hit rects never drift. */
#define DT_CLOSE_W 24
#define DT_PICK_W 26                 /* inspect-element toggle, left of the tab strip */
#define DT_PICK_GLYPH "\xe2\x86\x96" /* ↖ — the universal "inspect" cursor icon */
static int tab_x(int i)
{
  int x = DT_PICK_W;
  for (int k = 0; k < i; k++)
    x += gfx_ui_width(tab_names[k]) + 18;
  return x;
}
static int tab_w(int i) { return gfx_ui_width(tab_names[i]) + 18; }

/* Elements breadcrumb strip, shown above the find bar when a node is selected.
 */
#define DT_CRUMB_H 20
#define DT_CRUMB_SEP " \xe2\x80\xba " /* " › " between segments */
static int crumb_h(void) { return (dt_tab == TAB_ELEMENTS && dt_ncrumbs > 0) ? DT_CRUMB_H : 0; }

#define DT_NETDETAIL_H 168 /* Network detail panel height when a request is expanded */
#define DT_NETBAR_H 20     /* Network summary + type-filter bar at the very bottom  */
static int content_top(int top) { return top + DT_HDR; }
static int net_detail_top(void) { return BH - DT_NETBAR_H - DT_NETDETAIL_H; }
/* The Console tab reserves a bottom row for the REPL input; the Elements tab a
 * find bar (plus the breadcrumb when a node is selected); the Network tab
 * always reserves the summary bar, plus the detail band when a request is
 * expanded above it. */
static int content_bottom(void)
{
  if (dt_tab == TAB_CONSOLE)
    return BH - DT_INPUT_H;
  if (dt_tab == TAB_ELEMENTS)
    return BH - DT_INPUT_H - crumb_h();
  if (dt_tab == TAB_NETWORK)
    return BH - DT_NETBAR_H - (dt_net_sel >= 0 ? DT_NETDETAIL_H : 0);
  return BH;
}
static int content_rows(int top)
{
  int h = content_bottom() - content_top(top);
  return h > 0 ? h / ROW_H : 0;
}
static int content_w(void) { return (BW - devtools_left()) - SCROLL_W; } /* panel minus scrollbar */

/* Total scrollable line count of the active tab (read under dt_lock by caller).
 */
static int active_lines(void)
{
  if (dt_tab == TAB_ELEMENTS)
    return dt_dom.nlines;
  if (dt_tab == TAB_STYLES)
    return dt_sty.nlines;
  if (dt_tab == TAB_APP)
    return dt_app.nlines;
  if (dt_tab == TAB_NETWORK)
    return net_visible_count();
  return con_visible_count();
}

static int clamp_scroll(int s, int total, int rows)
{
  int max = total - rows;
  if (max < 0)
    max = 0;
  if (s > max)
    s = max;
  if (s < 0)
    s = 0;
  return s;
}

/* Syntax-highlight palette (dark theme, à la a real devtools markup view). */
#define DT_C_TEXT 0xcdd3de
#define DT_C_PUNCT 0x9aa0a6 /* < > / = and whitespace             */
#define DT_C_TAG 0x7cb7e8   /* element tag name                   */
#define DT_C_ATTR 0xe0a060  /* attribute name                     */
#define DT_C_VALUE 0x9bce8a /* attribute value (incl. its quotes) */

/* Draw a run [run,run+rl) at x in colour col with the monospace UTF-8 font,
 * returning the advanced x (one cell per codepoint) so colour segments abut. */
static int flush_run(int x, int y, char *run, int *rl, unsigned col)
{
  if (*rl <= 0)
    return x;
  run[*rl] = '\0';
  x = gfx_text_utf8(x, y, run, col, GFX_TRANSPARENT);
  *rl = 0;
  return x;
}

/* Render one DOM-markup line with token colours: brackets/`=` punct, tag names,
 * attribute names, quoted values, and text content. A tiny scanner — `in_tag`
 * tracks `<…>`, `in_quote` a value, `tag_name` the first identifier after `<`.
 */
static void draw_dom_line(int x, int y, const char *s)
{
  char run[260];
  int rl = 0;
  unsigned cur = DT_C_TEXT;
  bool in_tag = false, in_quote = false, tag_name = false;
  for (int i = 0; s[i]; i++) {
    char c = s[i];
    unsigned col;
    if (in_quote) {
      col = DT_C_VALUE;
      if (c == '"')
        in_quote = false;
    } else if (c == '<') {
      in_tag = true;
      tag_name = true;
      col = DT_C_PUNCT;
    } else if (c == '>') {
      in_tag = false;
      tag_name = false;
      col = DT_C_PUNCT;
    } else if (in_tag) {
      if (c == '"') {
        in_quote = true;
        col = DT_C_VALUE;
      } else if (c == '/' || c == '=')
        col = DT_C_PUNCT;
      else if (c == ' ') {
        tag_name = false;
        col = DT_C_PUNCT;
      } else
        col = tag_name ? DT_C_TAG : DT_C_ATTR;
    } else
      col = DT_C_TEXT;

    if (rl && col != cur)
      x = flush_run(x, y, run, &rl, cur);
    cur = col;
    if (rl < (int)sizeof run - 1)
      run[rl++] = c;
  }
  flush_run(x, y, run, &rl, cur);
}

/* Render one Styles line: a `<tag#id.class>` header in tag colour; a
 * `name: value` declaration with the property dim and the value highlighted. */
static void draw_style_line(int x, int y, const char *s)
{
  if (s[0] == '<') {
    gfx_text_utf8(x, y, s, DT_C_TAG, GFX_TRANSPARENT);
    return;
  }
  int colon = -1;
  for (int i = 0; s[i]; i++)
    if (s[i] == ':') {
      colon = i;
      break;
    }
  if (colon < 0) {
    gfx_text_utf8(x, y, s, DT_C_TEXT, GFX_TRANSPARENT);
    return;
  }
  char run[260];
  int rl = 0;
  for (int i = 0; i <= colon && rl < (int)sizeof run - 1; i++)
    run[rl++] = s[i];
  x = flush_run(x, y, run, &rl, DT_C_ATTR);                        /* "name:"  */
  gfx_text_utf8(x, y, s + colon + 1, DT_C_VALUE, GFX_TRANSPARENT); /* " value" */
}

/* Render one Application-tab line: a section header (no indent) in tag colour;
 * an indented "name=value" entry with the name dim and the value highlighted.
 */
static void draw_app_line(int x, int y, const char *s)
{
  if (s[0] != ' ') {
    gfx_text_utf8(x, y, s, DT_C_TAG, GFX_TRANSPARENT);
    return;
  }
  int eq = -1;
  for (int i = 0; s[i]; i++)
    if (s[i] == '=') {
      eq = i;
      break;
    }
  if (eq < 0) {
    gfx_text_utf8(x, y, s, DT_C_TEXT, GFX_TRANSPARENT);
    return;
  }
  char run[260];
  int rl = 0;
  for (int i = 0; i <= eq && rl < (int)sizeof run - 1; i++)
    run[rl++] = s[i];
  x = flush_run(x, y, run, &rl, DT_C_ATTR);                     /* "  name=" */
  gfx_text_utf8(x, y, s + eq + 1, DT_C_VALUE, GFX_TRANSPARENT); /* value @ host */
}

/* Physical rows a console/network line occupies once wrapped at `max` columns.
 */
static int wrap_rows_for(const char *s, int max)
{
  int len = 0;
  while (s && s[len])
    len++;
  int r = (len + max - 1) / max;
  return r < 1 ? 1 : r;
}
/* The topmost VISIBLE console line (level-filtered) keeping the newest passing
 * content in view — i.e. the scroll position that shows the tail. */
static int con_ftail(int rows, int max)
{
  int used = 0, seen = 0, vis = con_visible_count();
  for (int i = con_count - 1; i >= 0; i--) {
    int slot = (con_start + i) % CON_MAX;
    if (!con_pass(con[slot].level))
      continue;
    int r = wrap_rows_for(con[slot].text, max);
    if (used + r > rows)
      break;
    used += r;
    seen++;
  }
  int start = vis - seen;
  if (start >= vis && vis > 0)
    start = vis - 1;
  if (start < 0)
    start = 0;
  return start;
}
/* con_tail_start over the type-filtered network log: the topmost VISIBLE
 * request index keeping the newest passing rows in view. */
static int net_ftail(int rows, int max)
{
  int used = 0, seen = 0, vis = net_visible_count();
  for (int i = net_count - 1; i >= 0; i--) {
    int slot = (net_start + i) % NET_MAX;
    if (!net_pass_slot(slot))
      continue;
    int r = wrap_rows_for(net[slot].text, max);
    if (used + r > rows)
      break;
    used += r;
    seen++;
  }
  int start = vis - seen;
  if (start >= vis && vis > 0)
    start = vis - 1;
  if (start < 0)
    start = 0;
  return start;
}

void devtools_render(void)
{
  if (!dt_open)
    return;
  int left = devtools_left(), top = devtools_top();
  int w = BW - left, h = BH - top;

  dt_lock();

  gfx_fill_rect(left, top, w, h, 0x1e1e1e);      /* panel body */
  gfx_fill_rect(left, top, w, DT_HDR, 0x14171c); /* header row */
  gfx_fill_rect(left, top, 1, h, 0x000000);      /* splitter line */

  button_draw(left, top, DT_PICK_W, DT_HDR, DT_PICK_GLYPH, dt_pick ? BTN_PRESSED : BTN_NORMAL);
  for (int i = 0; i < N_TABS; i++)
    button_draw(left + tab_x(i), top, tab_w(i), DT_HDR, tab_names[i], i == dt_tab ? BTN_PRESSED : BTN_NORMAL);
  button_draw(BW - DT_CLOSE_W, top, DT_CLOSE_W, DT_HDR, "\xc3\x97", BTN_NORMAL); /* × */

  int ct = content_top(top), rows = content_rows(top), cw = content_w();
  int tx = left + 6;
  int total = active_lines();
  int *sc = &dt_scroll[dt_tab];

  if (dt_tab == TAB_ELEMENTS || dt_tab == TAB_STYLES || dt_tab == TAB_APP)
    *sc = clamp_scroll(*sc, total, rows); /* console/network clamp below (wrap-aware) */

  int max = (cw - 8) / GLYPH_W;
  if (max > 255)
    max = 255;
  if (max < 1)
    max = 1;
  char line[256];
  if (dt_tab == TAB_CONSOLE || dt_tab == TAB_NETWORK) {
    /* Console/Network lines wrap: a long message spans as many rows as it needs
     * (so a full error/URL is readable), and scrolling steps by logical line.
     */
    bool isconsole = dt_tab == TAB_CONSOLE;
    struct conline *ring = isconsole ? con : net;
    int cap = isconsole ? CON_MAX : NET_MAX;
    int base = isconsole ? con_start : net_start;
    int raw = isconsole ? con_count : net_count; /* raw ring length (pre-filter) */
    int bottom = ct + rows * ROW_H, y;
    int maxstart = isconsole ? con_ftail(rows, max) : net_ftail(rows, max);
    if (isconsole && con_follow)
      *sc = maxstart; /* pinned to newest */
    if (*sc > maxstart)
      *sc = maxstart;
    if (*sc < 0)
      *sc = 0;
    if (isconsole)
      con_follow = (*sc >= maxstart);
    y = ct;
    int vi = 0; /* visible (filtered) index */
    for (int i = 0; i < raw && y + ROW_H <= bottom; i++) {
      int slot = (base + i) % cap;
      if (isconsole ? !con_pass(ring[slot].level) : !net_pass_slot(slot))
        continue;
      if (vi < *sc) {
        vi++;
        continue;
      } /* scrolled above the viewport */
      vi++;
      unsigned fg = ring[slot].level == DT_ERROR             ? 0xf08080
                    : ring[slot].level == DT_WARN            ? 0xe0c060
                    : ring[slot].level == DT_NET_PENDING_LVL ? 0x8a93a0
                                                             : 0xcdd3de;
      bool iserr = ring[slot].level == DT_ERROR;
      const char *s = ring[slot].text;
      struct crun *runs = isconsole ? ring[slot].runs : 0;
      int nruns = isconsole ? ring[slot].nruns : 0;
      int len = 0, off = 0;
      while (s && s[len])
        len++;
      bool netsel = (!isconsole && i == dt_net_sel);
      do {
        int n = 0;
        while (off + n < len && n < max - 1) {
          line[n] = s[off + n];
          n++;
        }
        line[n] = '\0';
        unsigned rowbg = netsel ? 0x09415f : iserr ? 0x3a2222 : 0;
        if (rowbg)
          gfx_fill_rect(left, y, cw, ROW_H, rowbg);
        if (nruns)
          con_draw_runs(tx, y, s, off, off + n, fg, runs, nruns);
        else
          gfx_text_utf8(tx, y, line, fg, GFX_TRANSPARENT);
        off += n;
        y += ROW_H;
      } while (off < len && y + ROW_H <= bottom);
    }
  } else
    for (int r = 0; r < rows; r++) {
      int idx = *sc + r;
      if (idx >= total)
        break;
      int y = ct + r * ROW_H;
      if (dt_tab == TAB_ELEMENTS) {
        bool sel = (idx < dt_dom_node_cap && dt_dom_node[idx] == dt_sel);
        if (sel)
          gfx_fill_rect(left, y, cw, ROW_H, 0x094771);
        tb_line(&dt_dom, idx, line, max);
        draw_dom_line(tx, y, line);
      } else if (dt_tab == TAB_STYLES) {
        tb_line(&dt_sty, idx, line, max);
        draw_style_line(tx, y, line);
      } else { /* TAB_APP */
        tb_line(&dt_app, idx, line, max);
        draw_app_line(tx, y, line);
      }
    }

  scrollbar_draw(BW - SCROLL_W, ct, SCROLL_W, content_bottom() - ct, *sc, total, rows);

  if (dt_tab == TAB_CONSOLE) { /* REPL input + filter/clear toolbar */
    int iy = BH - DT_INPUT_H;
    int ftx = BW - 2 * DT_FBTN_W, fbx = BW - DT_FBTN_W; /* filter / clear button x */
    dt_input_ensure();
    gfx_fill_rect(left, iy, w, DT_INPUT_H, 0x14171c);
    gfx_fill_rect(left, iy, w, 1, 0x000000);                             /* separator above the field */
    gfx_text_utf8(left + 5, iy + (DT_INPUT_H - GLYPH_H) / 2, "\xc2\xbb", /* » prompt */
                  0x7cb7e8, GFX_TRANSPARENT);
    textfield_draw(&dt_input_tf, left + 20, iy + 2, (ftx - 4) - (left + 20), DT_INPUT_H - 4, dt_input_focused);
    button_draw(ftx, iy + 1, DT_FBTN_W - 2, DT_INPUT_H - 2,
                dt_con_filter == 0   ? "All"
                : dt_con_filter == 1 ? "Warn"
                                     : "Err",
                dt_con_filter ? BTN_PRESSED : BTN_NORMAL);
    button_draw(fbx, iy + 1, DT_FBTN_W - 2, DT_INPUT_H - 2, "Clear", BTN_NORMAL);
  }

  if (dt_tab == TAB_ELEMENTS && dt_ncrumbs > 0) { /* ancestor breadcrumb */
    int cy = BH - DT_INPUT_H - DT_CRUMB_H;
    gfx_fill_rect(left, cy, w, DT_CRUMB_H, 0x14171c);
    gfx_fill_rect(left, cy, w, 1, 0x000000);
    int cx = left + 6, ty = cy + (DT_CRUMB_H - UIFONT_H) / 2;
    for (int i = 0; i < dt_ncrumbs && cx < BW - 8; i++) {
      if (i) {
        gfx_ui_text(cx, ty, DT_CRUMB_SEP, DT_C_PUNCT);
        cx += gfx_ui_width(DT_CRUMB_SEP);
      }
      unsigned col = dt_crumbs[i].node_idx == dt_sel ? DT_C_TAG : DT_C_TEXT;
      gfx_ui_text(cx, ty, dt_crumbs[i].label, col);
      cx += gfx_ui_width(dt_crumbs[i].label);
    }
  }

  if (dt_tab == TAB_ELEMENTS) { /* find bar at the bottom */
    int iy = BH - DT_INPUT_H;
    if (!dt_find_init) {
      textfield_init(&dt_find_tf, dt_find, sizeof dt_find);
      dt_find_init = true;
    }
    gfx_fill_rect(left, iy, w, DT_INPUT_H, 0x14171c);
    gfx_fill_rect(left, iy, w, 1, 0x000000);
    gfx_text_utf8(left + 5, iy + (DT_INPUT_H - GLYPH_H) / 2, "/", /* find prompt */
                  0x9aa0a6, GFX_TRANSPARENT);
    textfield_draw(&dt_find_tf, left + 20, iy + 2, w - 24, DT_INPUT_H - 4, dt_find_focused);
  }

  if (dt_tab == TAB_NETWORK) { /* request detail (above) + summary bar */
    if (dt_net_sel >= 0 && dt_net_sel < net_count) {
      int dy = net_detail_top(), dbot = BH - DT_NETBAR_H;
      struct netinfo *ni = &netinfo[(net_start + dt_net_sel) % NET_MAX];
      gfx_fill_rect(left, dy, w, DT_NETDETAIL_H, 0x14171c);
      gfx_fill_rect(left, dy, w, 1, 0x000000);                                      /* separator above */
      button_draw(BW - DT_CLOSE_W, dy, DT_CLOSE_W, DT_HDR, "\xc3\x97", BTN_NORMAL); /* × close */
      int lx = left + 8, ly = dy + 4;
      char b[256];
      int k;
      k = 0;
      sapp(b, &k, sizeof b, "Status: ");
      if (ni->state == NET_PENDING)
        sapp(b, &k, sizeof b, "(pending)");
      else if (ni->state == NET_CANCELLED)
        sapp(b, &k, sizeof b, "(cancelled)");
      else if (ni->status > 0)
        uapp(b, &k, sizeof b, (unsigned)ni->status);
      else
        sapp(b, &k, sizeof b, "(failed)");
      if (ni->state == NET_FAILED && ni->err[0]) {
        sapp(b, &k, sizeof b, " - ");
        sapp(b, &k, sizeof b, ni->err);
      }
      if (ni->ms > 0) {
        sapp(b, &k, sizeof b, "    ");
        uapp(b, &k, sizeof b, (unsigned)ni->ms);
        sapp(b, &k, sizeof b, " ms");
      }
      unsigned stcol = ni->state == NET_PENDING                 ? 0x8a93a0
                       : ni->state == NET_CANCELLED             ? 0xe0c060
                       : (ni->status == 0 || ni->status >= 400) ? 0xf08080
                                                                : 0x9bce8a;
      gfx_text_utf8(lx, ly, b, stcol, GFX_TRANSPARENT);
      ly += GLYPH_H;
      k = 0;
      sapp(b, &k, sizeof b, "Method: ");
      sapp(b, &k, sizeof b, ni->method[0] ? ni->method : "GET");
      sapp(b, &k, sizeof b, "    ");
      net_putsize(b, &k, sizeof b, ni->size);
      gfx_text_utf8(lx, ly, b, 0xcdd3de, GFX_TRANSPARENT);
      ly += GLYPH_H;
      k = 0;
      sapp(b, &k, sizeof b, "Type: ");
      sapp(b, &k, sizeof b, ni->ctype[0] ? ni->ctype : "(unknown)");
      gfx_text_utf8(lx, ly, b, 0xcdd3de, GFX_TRANSPARENT);
      ly += GLYPH_H;
      gfx_text_utf8(lx, ly, "URL:", 0x9aa0a6, GFX_TRANSPARENT);
      ly += GLYPH_H;
      int umax = (w - 16) / GLYPH_W;
      if (umax < 1)
        umax = 1;
      if (umax > (int)sizeof b - 1)
        umax = (int)sizeof b - 1;
      const char *u = ni->url ? ni->url : "";
      int ulen = 0;
      while (u[ulen])
        ulen++;
      for (int off = 0; off < ulen && ly + GLYPH_H <= dbot; ly += GLYPH_H) { /* wrap the full URL */
        int n = 0;
        while (off + n < ulen && n < umax) {
          b[n] = u[off + n];
          n++;
        }
        b[n] = '\0';
        gfx_text_utf8(lx, ly, b, 0x7cb7e8, GFX_TRANSPARENT);
        off += n;
      }
      /* Response headers — a scrollable list filling the rest of the detail. */
      ly += 2;
      if (ly + GLYPH_H <= dbot) {
        gfx_text_utf8(lx, ly, "Response Headers", 0x9aa0a6, GFX_TRANSPARENT);
        ly += GLYPH_H;
      }
      int hn = hdr_nlines(ni->resp_hdr);
      int hrows = (dbot - ly) / GLYPH_H;
      if (hrows < 0)
        hrows = 0;
      dt_nd_scroll = clamp_scroll(dt_nd_scroll, hn, hrows);
      int hmax = (w - 16 - SCROLL_W) / GLYPH_W;
      if (hmax < 1)
        hmax = 1;
      if (hmax > (int)sizeof b - 1)
        hmax = (int)sizeof b - 1;
      if (hn == 0) {
        gfx_text_utf8(lx, ly, "(not captured for this request)", 0x6b7177, GFX_TRANSPARENT);
      } else {
        for (int r = 0; r < hrows; r++) {
          int hi = dt_nd_scroll + r;
          if (hi >= hn)
            break;
          hdr_line(ni->resp_hdr, hi, b, hmax);
          draw_style_line(lx, ly + r * GLYPH_H, b); /* "Name: value" colouring */
        }
        if (hn > hrows)
          scrollbar_draw(BW - SCROLL_W, ly, SCROLL_W, hrows * GLYPH_H, dt_nd_scroll, hn, hrows);
      }
    }

    int by = BH - DT_NETBAR_H; /* summary + type-filter bar */
    gfx_fill_rect(left, by, w, DT_NETBAR_H, 0x14171c);
    gfx_fill_rect(left, by, w, 1, 0x000000);
    int fbw = 56;
    button_draw(left + 2, by + 1, fbw, DT_NETBAR_H - 2, net_filter_names[dt_net_filter],
                dt_net_filter ? BTN_PRESSED : BTN_NORMAL);
    int nshow = 0, failed = 0, pending = 0;
    long bytes = 0;
    for (int i = 0; i < net_count; i++) {
      int slot = (net_start + i) % NET_MAX;
      if (!net_pass_slot(slot))
        continue;
      nshow++;
      struct netinfo *si = &netinfo[slot];
      if (si->size > 0)
        bytes += si->size;
      if (si->state == NET_PENDING)
        pending++;
      else if (si->state == NET_FAILED || si->state == NET_CANCELLED || si->status == 0 || si->status >= 400)
        failed++;
    }
    char sb[96];
    int k = 0;
    uapp(sb, &k, sizeof sb, (unsigned)nshow);
    sapp(sb, &k, sizeof sb, nshow == 1 ? " req   " : " reqs   ");
    net_putsize(sb, &k, sizeof sb, bytes > 0x7fffffff ? 0x7fffffff : (int)bytes);
    if (pending) {
      sapp(sb, &k, sizeof sb, "   ");
      uapp(sb, &k, sizeof sb, (unsigned)pending);
      sapp(sb, &k, sizeof sb, " pending");
    }
    if (failed) {
      sapp(sb, &k, sizeof sb, "   ");
      uapp(sb, &k, sizeof sb, (unsigned)failed);
      sapp(sb, &k, sizeof sb, " failed");
    }
    gfx_text_utf8(left + fbw + 12, by + (DT_NETBAR_H - GLYPH_H) / 2, sb,
                  failed    ? 0xe0a060
                  : pending ? 0x8a93a0
                            : 0xcdd3de,
                  GFX_TRANSPARENT);
  }
  dt_unlock();
}

/* The logical Network row at screen-y `my`, accounting for line wrapping (a
 * long URL spans several physical rows). -1 if the click is below the last row.
 * Mirrors the render walk so the hit rect matches what was drawn. Caller holds
 * dt_lock. */
static int net_row_at(int my)
{
  int top = devtools_top(), ct = content_top(top);
  int bottom = ct + content_rows(top) * ROW_H;
  int max = (content_w() - 8) / GLYPH_W;
  if (max > 255)
    max = 255;
  if (max < 1)
    max = 1;
  int y = ct, vi = 0, sc = dt_scroll[TAB_NETWORK];
  for (int i = 0; i < net_count && y + ROW_H <= bottom; i++) {
    int slot = (net_start + i) % NET_MAX;
    if (!net_pass_slot(slot))
      continue; /* hidden by the type filter */
    if (vi < sc) {
      vi++;
      continue;
    } /* scrolled above the viewport */
    int hgt = wrap_rows_for(net[slot].text, max) * ROW_H;
    if (my >= y && my < y + hgt)
      return i; /* the raw ring index */
    y += hgt;
    vi++;
  }
  return -1;
}

/* Select (and scroll to) the next Elements line matching the find box, wrapping
 * past the current selection. Selecting also highlights the node on the page.
 */
static void dt_find_next(int fresh)
{
  if (!dt_find[0])
    return;
  int rows = content_rows(devtools_top());
  dt_lock();
  int nl = dt_dom.nlines, start = 0, found = -1;
  if (nl > 0) {
    if (!fresh) /* "next": resume past the current match */
      for (int i = 0; i < nl && i < dt_dom_node_cap; i++)
        if (dt_dom_node[i] == dt_sel) {
          start = i + 1;
          break;
        }
    char lb[256];
    for (int s = 0; s < nl; s++) {
      int i = (start + s) % nl;
      tb_line(&dt_dom, i, lb, sizeof lb);
      if (contains_ci(lb, dt_find)) {
        found = i;
        break;
      }
    }
  }
  int node = (found >= 0 && found < dt_dom_node_cap) ? dt_dom_node[found] : -1;
  dt_unlock();
  if (found >= 0) {
    dt_sel = node;
    int sc = found - rows / 2;
    if (sc < 0)
      sc = 0;
    dt_scroll[TAB_ELEMENTS] = sc;
    dt_dirty = true;
    B->seq++;
  }
}

int devtools_handle_click(int mx, int my)
{
  if (!dt_open)
    return 0;
  int left = devtools_left(), top = devtools_top();
  if (mx < left || my < top) {
    dt_input_focused = dt_find_focused = false;
    return 0;
  } /* not ours: blur */

  if (my < top + DT_HDR) { /* header buttons */
    if (button_hit(left, top, DT_PICK_W, DT_HDR, mx, my)) {
      devtools_pick_toggle();
      return 1; /* inspect-element toggle */
    }
    if (button_hit(BW - DT_CLOSE_W, top, DT_CLOSE_W, DT_HDR, mx, my)) {
      devtools_toggle();
      return 1;
    }
    for (int i = 0; i < N_TABS; i++)
      if (button_hit(left + tab_x(i), top, tab_w(i), DT_HDR, mx, my)) {
        dt_tab = i;
        dt_input_focused = (i == TAB_CONSOLE); /* the Console tab focuses its REPL */
        dt_find_focused = false;
        dt_dirty = true; /* refresh Styles/Application on reveal */
        B->seq++;
        return 1;
      }
    return 1;
  }

  if (dt_tab == TAB_CONSOLE && my >= BH - DT_INPUT_H) { /* REPL input + filter/clear toolbar */
    int iy = BH - DT_INPUT_H;
    int ftx = BW - 2 * DT_FBTN_W, fbx = BW - DT_FBTN_W;
    if (button_hit(ftx, iy + 1, DT_FBTN_W - 2, DT_INPUT_H - 2, mx, my)) {
      dt_con_filter = (dt_con_filter + 1) % 3;
      con_follow = true;
      B->seq++;
      return 1;
    }
    if (button_hit(fbx, iy + 1, DT_FBTN_W - 2, DT_INPUT_H - 2, mx, my)) {
      con_clear();
      B->seq++;
      return 1;
    }
    dt_input_ensure();
    textfield_click(&dt_input_tf, mx, my, left + 20, iy + 2, (ftx - 4) - (left + 20), DT_INPUT_H - 4);
    dt_input_focused = true;
    B->seq++;
    return 1;
  }

  if (dt_tab == TAB_ELEMENTS && my >= BH - DT_INPUT_H) { /* click in the find bar */
    int iy = BH - DT_INPUT_H, w = BW - left;
    if (!dt_find_init) {
      textfield_init(&dt_find_tf, dt_find, sizeof dt_find);
      dt_find_init = true;
    }
    textfield_click(&dt_find_tf, mx, my, left + 20, iy + 2, w - 24, DT_INPUT_H - 4);
    dt_find_focused = true;
    B->seq++;
    return 1;
  }

  if (crumb_h() && my >= BH - DT_INPUT_H - DT_CRUMB_H && my < BH - DT_INPUT_H) { /* breadcrumb */
    int cx = left + 6, hit = -1;
    dt_lock();
    for (int i = 0; i < dt_ncrumbs; i++) {
      if (i)
        cx += gfx_ui_width(DT_CRUMB_SEP);
      int seg = gfx_ui_width(dt_crumbs[i].label);
      if (mx >= cx && mx < cx + seg) {
        hit = dt_crumbs[i].node_idx;
        break;
      }
      cx += seg;
    }
    dt_unlock();
    if (hit >= 0) {
      dt_sel = hit;
      dt_dirty = true;
      B->seq++;
    }
    return 1;
  }

  if (dt_tab == TAB_NETWORK && my >= BH - DT_NETBAR_H) { /* summary / type-filter bar */
    int by = BH - DT_NETBAR_H, fbw = 56;
    if (button_hit(left + 2, by + 1, fbw, DT_NETBAR_H - 2, mx, my)) {
      dt_net_filter = (dt_net_filter + 1) % NF_N; /* cycle the resource-type filter */
      dt_scroll[TAB_NETWORK] = 0;
      if (dt_net_sel >= 0) { /* close the detail if it's now hidden */
        dt_lock();
        if (dt_net_sel >= net_count || !net_pass_slot((net_start + dt_net_sel) % NET_MAX))
          dt_net_sel = -1;
        dt_unlock();
      }
      B->seq++;
    }
    return 1;
  }

  if (dt_tab == TAB_NETWORK && dt_net_sel >= 0 && my >= net_detail_top()) { /* request detail */
    if (button_hit(BW - DT_CLOSE_W, net_detail_top(), DT_CLOSE_W, DT_HDR, mx, my)) {
      dt_net_sel = -1;
      B->seq++; /* × closes the detail and restores the list height */
    }
    return 1; /* swallow other clicks inside the detail panel */
  }

  int ct = content_top(top), rows = content_rows(top);
  dt_lock();
  int total = active_lines();
  dt_unlock();

  int part = scrollbar_hit(BW - SCROLL_W, ct, SCROLL_W, content_bottom() - ct, mx, my);
  if (part != SCROLLBAR_NONE) { /* scrollbar (reused widget) */
    if (part == SCROLLBAR_UP)
      dt_scroll[dt_tab] -= 1;
    else if (part == SCROLLBAR_DOWN)
      dt_scroll[dt_tab] += 1;
    else
      dt_scroll[dt_tab] =
          scrollbar_pos(BW - SCROLL_W, ct, SCROLL_W, content_bottom() - ct, mx, my, dt_scroll[dt_tab], total, rows);
    dt_scroll[dt_tab] = clamp_scroll(dt_scroll[dt_tab], total, rows);
    if (dt_tab == TAB_CONSOLE)
      con_follow = (dt_scroll[dt_tab] >= total - rows);
    B->seq++;
    return 1;
  }

  if (mx < BW - SCROLL_W && dt_tab == TAB_ELEMENTS) { /* select a DOM node, or toggle its ▸/▾ */
    int row = dt_scroll[dt_tab] + (my - ct) / ROW_H;
    int cell = (mx - (left + 6)) / GLYPH_W; /* monospace column of the click */
    dt_lock();
    bool ok = row >= 0 && row < dt_dom.nlines && row < dt_dom_node_cap;
    int node = ok ? dt_dom_node[row] : -1;
    int tri = -1; /* cell of this row's ▸/▾, or -1 */
    if (ok) {
      char lb[256];
      tb_line(&dt_dom, row, lb, sizeof lb);
      int sp = 0;
      while (lb[sp] == ' ')
        sp++; /* indent = one cell per space */
      if ((unsigned char)lb[sp] == 0xe2 && (unsigned char)lb[sp + 1] == 0x96 &&
          ((unsigned char)lb[sp + 2] == 0xba || (unsigned char)lb[sp + 2] == 0xbc))
        tri = sp;
    }
    dt_unlock();
    if (ok) {
      /* toggle_collapsed reallocs dt_collapsed, which the worker reads under
       * dt_lock in dom_emit->is_collapsed — take the lock so we can't free it
       * out from under a concurrent snapshot walk. */
      if (tri >= 0 && cell >= 0 && cell <= tri + 1) {
        dt_lock();
        toggle_collapsed(node);
        dt_unlock();
      } /* hit the triangle */
      else
        dt_sel = node;
      dt_dirty = true;
      B->seq++;
    }
  }

  if (mx < BW - SCROLL_W && dt_tab == TAB_NETWORK) { /* expand / collapse a request row */
    dt_lock();
    int row = net_row_at(my);
    dt_unlock();
    if (row >= 0) {
      dt_net_sel = (dt_net_sel == row) ? -1 : row;
      dt_nd_scroll = 0;
      B->seq++;
    }
  }
  return 1;
}

int devtools_handle_scroll(int delta)
{
  if (!dt_open)
    return 0;
  /* An expanded Network request takes the wheel for its response-header list
   * (the detail owns the focus); collapse it to scroll the request list again.
   */
  if (dt_tab == TAB_NETWORK && dt_net_sel >= 0) {
    int prev = dt_nd_scroll;
    dt_nd_scroll -= delta * 3;
    if (dt_nd_scroll < 0)
      dt_nd_scroll = 0;
    if (dt_nd_scroll != prev)
      B->seq++; /* render clamps to the header count */
    return 1;
  }
  int rows = content_rows(devtools_top());
  int prev = dt_scroll[dt_tab], next;
  if (dt_tab == TAB_CONSOLE || dt_tab == TAB_NETWORK) {
    /* Lines wrap here, so the max scroll is the wrap-aware tail (matching
     * render), not total-minus-rows — otherwise a wheel step near the bottom
     * moves the value only for render to clamp it straight back. */
    int cols = (content_w() - 8) / GLYPH_W;
    if (cols > 255)
      cols = 255;
    if (cols < 1)
      cols = 1;
    dt_lock();
    int maxstart = dt_tab == TAB_CONSOLE ? con_ftail(rows, cols) : net_ftail(rows, cols);
    dt_unlock();
    next = prev - delta * 3;
    if (next > maxstart)
      next = maxstart;
    if (next < 0)
      next = 0;
    if (dt_tab == TAB_CONSOLE)
      con_follow = (next >= maxstart);
  } else {
    dt_lock();
    int total = active_lines();
    dt_unlock();
    next = clamp_scroll(prev - delta * 3, total, rows);
  }
  if (next != prev) {
    dt_scroll[dt_tab] = next;
    B->seq++;
  } /* repaint only on real move */
  return 1;
}

/* Feed a key to the Console REPL input. Returns 1 when consumed (the panel is
 * open, the Console tab is shown, and its input is focused), so the caller
 * stops routing the key to the address bar / page. Enter queues the command for
 * the worker (which owns the JS context); Esc blurs the input without closing
 * the panel; Up/Down recall history. Non-edit keys are left for the caller. */
int devtools_handle_key(char c, int mods)
{
  (void)mods;
  if (dt_open && dt_tab == TAB_ELEMENTS && dt_find_focused) { /* Elements find box */
    if (!dt_find_init) {
      textfield_init(&dt_find_tf, dt_find, sizeof dt_find);
      dt_find_init = true;
    }
    unsigned char uc = (unsigned char)c;
    if (uc == 27) {
      dt_find_focused = false;
      B->seq++;
      return 1;
    } /* Esc: blur */
    if (uc == '\n' || uc == '\r') {
      dt_find_next(0);
      return 1;
    } /* Enter: next match */
    if (textfield_key(&dt_find_tf, c)) {
      dt_find_next(1);
      B->seq++;
      return 1;
    } /* incremental */
    return 0;
  }
  if (!dt_open || dt_tab != TAB_CONSOLE || !dt_input_focused)
    return 0;
  dt_input_ensure();
  unsigned char uc = (unsigned char)c;
  if (uc == 27) {
    dt_input_focused = false;
    B->seq++;
    return 1;
  } /* blur, keep the panel */
  if (uc == '\n' || uc == '\r') {
    if (dt_input_tf.len > 0) {
      dt_hist_add(dt_input);
      int n = 0;
      while (dt_input[n] && n < (int)sizeof dt_eval_buf - 1) {
        dt_eval_buf[n] = dt_input[n];
        n++;
      }
      dt_eval_buf[n] = '\0';
      dt_eval_pending = true;
      textfield_set(&dt_input_tf, "");
      con_follow = true;
      B->seq++;
    }
    return 1;
  }
  /* These edit only the REPL input field in the dock, not the page: request a
   * dock-only repaint (dt_paint_only), not a full-page render (B->seq++). A
   * full render per keystroke re-lays-out/repaints the whole document — seconds
   * of lag on a heavy page. (Enter keeps B->seq++: its eval may mutate the
   * DOM.) */
  if (uc == KEY_UP) {
    dt_hist_recall(-1);
    dt_paint_only = true;
    return 1;
  }
  if (uc == KEY_DOWN) {
    dt_hist_recall(+1);
    dt_paint_only = true;
    return 1;
  }
  if (textfield_key(&dt_input_tf, c)) {
    dt_paint_only = true;
    return 1;
  }
  return 0; /* not an edit key: fall through */
}
