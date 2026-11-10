#include "devtools_int.h"
#include "features/cdp/cdp.h"

/* In-band `%c` style markers; keep in sync with JS_STYLE_ON/OFF in user/lib/js/interp.h. */
#define CON_STYLE_ON 0x01
#define CON_STYLE_OFF 0x02

/* Console ring: circular buffer of heap lines + level; worker appends, main reads under dt_lk. */
struct conline dt_con[CON_MAX];
int dt_con_start, dt_con_count;
char *dt_con_pend;
int dt_con_pend_len, dt_con_pend_cap;
int dt_con_pend_level;
bool dt_con_follow = true; /* keep the view pinned to the newest line      */
/* Level filter: 0 = all, 1 = warnings+errors, 2 = errors only (line shown if level >= filter). */
int dt_con_filter;
bool dt_con_pass(int level) { return level >= dt_con_filter; }
int dt_con_visible_count(void)
{
  if (dt_con_filter == 0)
    return dt_con_count;
  int c = 0;
  for (int i = 0; i < dt_con_count; i++)
    if (dt_con_pass(dt_con[(dt_con_start + i) % CON_MAX].level))
      c++;
  return c;
}
/* Drop every captured console line (the Clear button). Takes dt_lk itself. */
void dt_con_clear(void)
{
  mutex_lock(&dt_lk);
  for (int i = 0; i < dt_con_count; i++) {
    int s = (dt_con_start + i) % CON_MAX;
    free(dt_con[s].text);
    dt_con[s].text = 0;
    dt_con_free_runs(s);
  }
  dt_con_start = dt_con_count = 0;
  free(dt_con_pend);
  dt_con_pend = 0;
  dt_con_pend_len = dt_con_pend_cap = 0;
  dt_con_follow = true;
  mutex_unlock(&dt_lk);
}

/* Console REPL input: expressions go to the worker via dt_eval_*; history ring backs Up/Down. */
char dt_input[256];
struct textfield dt_input_tf;
static bool dt_input_init;
bool dt_input_focused;

#define DT_HIST_MAX 32
static char *dt_hist[DT_HIST_MAX];
static int dt_hist_cnt; /* commands stored (<= DT_HIST_MAX)            */
static int dt_hist_nav; /* recall cursor; == dt_hist_cnt is the new line */

volatile bool dt_eval_pending; /* main -> worker: a REPL command is queued    */
char dt_eval_buf[256];         /* its source text                             */

void dt_input_ensure(void)
{
  if (!dt_input_init) {
    textfield_init(&dt_input_tf, dt_input, sizeof dt_input);
    dt_input_init = true;
  }
}

void dt_hist_add(const char *s)
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

void dt_hist_recall(int dir)
{
  int p = dt_hist_nav + dir;
  if (p < 0)
    p = 0;
  if (p > dt_hist_cnt)
    p = dt_hist_cnt;
  dt_hist_nav = p;
  textfield_set(&dt_input_tf, p >= dt_hist_cnt ? "" : dt_hist[p]);
}

/* Parse one `%c` CSS block into overrides of `r`: color, background-color, bold/normal weight. */
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

void dt_con_free_runs(int slot)
{
  free(dt_con[slot].runs);
  dt_con[slot].runs = 0;
  dt_con[slot].nruns = 0;
}

/* Store one console line: strip %c markers, recording each style directive as a run by offset. */
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
  if (dt_con_count < CON_MAX) {
    slot = (dt_con_start + dt_con_count) % CON_MAX;
    dt_con_count++;
  } else { /* full: drop the oldest */
    slot = dt_con_start;
    free(dt_con[slot].text);
    dt_con_free_runs(slot);
    dt_con_start = (dt_con_start + 1) % CON_MAX;
  }
  dt_con[slot].text = dup;
  dt_con[slot].level = (unsigned char)level;
  dt_con[slot].runs = runs;
  dt_con[slot].nruns = nruns;
}

/* Draw wrapped row s[off,end) at (x,y) applying %c runs; bold is a 1px overstrike. */
void dt_con_draw_runs(int x, int y, const char *s, int off, int end, unsigned deffg, const struct crun *runs, int nruns)
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
  /* Capture even while the panel is closed so load-time errors are kept; only repaint is gated. */
  mutex_lock(&dt_lk);
  for (int i = 0; i < len; i++) {
    char c = s[i];
    if (c == '\n') {
      con_push(dt_con_pend_level, dt_con_pend ? dt_con_pend : "", dt_con_pend_len);
      if (cdp_connected() && dt_con_pend) {
        dt_con_pend[dt_con_pend_len] = '\0'; /* the growth check keeps a byte free */
        cdp_console_line(dt_con_pend_level, dt_con_pend);
      }
      dt_con_pend_len = 0;
    } else {
      if (dt_con_pend_len + 1 >= dt_con_pend_cap) {
        int nc = dt_con_pend_cap ? dt_con_pend_cap * 2 : 128;
        char *p = realloc(dt_con_pend, (unsigned)nc);
        if (!p)
          break;
        dt_con_pend = p;
        dt_con_pend_cap = nc;
      }
      dt_con_pend[dt_con_pend_len++] = c;
      dt_con_pend_level = level; /* the level of the line being built */
    }
  }
  dt_con_follow = true; /* a new line: jump to the bottom    */
  mutex_unlock(&dt_lk);
  if (dt_open && dt_tab == TAB_CONSOLE)
    dt_paint_only = true; /* repaint only if visible */
}

/* Network ring: one preformatted line per HTTP response, reset on navigation (devtools_nav_reset). */
struct conline dt_net[NET_MAX];
int dt_net_start, dt_net_count;
/* Structured request fields parallel to dt_net[], backing the detail view; url is heap. */
struct netinfo dt_netinfo[NET_MAX];
int dt_net_sel = -1;        /* logical index of the expanded request, -1 none */
static int net_next_id = 1; /* monotonic id: the begin -> update handshake key */

void dt_netinfo_clear(struct netinfo *ni)
{
  free(ni->url);
  ni->url = 0;
  free(ni->resp_hdr);
  ni->resp_hdr = 0;
  ni->id = ni->state = ni->status = ni->size = ni->ms = 0;
  ni->method[0] = ni->ctype[0] = ni->err[0] = '\0';
}

void devtools_nav_reset(void)
{
  mutex_lock(&dt_lk);
  for (int i = 0; i < dt_net_count; i++) {
    int s = (dt_net_start + i) % NET_MAX;
    free(dt_net[s].text);
    dt_net[s].text = 0;
    dt_netinfo_clear(&dt_netinfo[s]);
  }
  dt_net_start = 0;
  dt_net_count = 0;
  dt_net_sel = -1;
  mutex_unlock(&dt_lk);
  cdp_net_reset();
  dt_con_clear();
  if (dt_open)
    B->seq++;
}

/* Append a human-readable byte size: "923 B" / "12.3 KB" / "4.1 MB". */
void dt_net_putsize(char *b, int *n, int cap, int size)
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
  dt_net_putsize(buf, &n, cap, e->size);
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

/* (Re)write ring slot `slot` from e, freeing prior heap strings and keeping its id. Caller holds dt_lk. */
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
  free(dt_net[slot].text);
  dt_net[slot].text = dup;
  dt_net[slot].level = (unsigned char)net_level(e);

  struct netinfo *ni = &dt_netinfo[slot];
  int id = ni->id;
  dt_netinfo_clear(ni);
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
  ni->url = strdup(e->url);
  ni->resp_hdr = strdup(e->resp_hdr);
}

/* Reserve the next ring slot, evicting the oldest when full. Caller holds dt_lk. */
static int net_alloc_slot(void)
{
  int slot;
  if (dt_net_count < NET_MAX) {
    slot = (dt_net_start + dt_net_count) % NET_MAX;
    dt_net_count++;
  } else {
    slot = dt_net_start;
    free(dt_net[slot].text);
    dt_net[slot].text = 0;
    dt_netinfo_clear(&dt_netinfo[slot]);
    dt_net_start = (dt_net_start + 1) % NET_MAX;
    /* dt_net_sel is a logical index off dt_net_start: shift it as the oldest drops. */
    if (dt_net_sel == 0)
      dt_net_sel = -1;
    else if (dt_net_sel > 0)
      dt_net_sel--;
  }
  return slot;
}

/* Append a fresh request row from e and return its id. Caller holds dt_lk. */
static int net_push_e(const struct net_log *e)
{
  int slot = net_alloc_slot();
  dt_netinfo[slot].id = net_next_id++;
  int id = dt_netinfo[slot].id;
  net_fill(slot, e); /* preserves the id just assigned */
  return id;
}

/* Capture even while the panel is closed; only the repaint is gated on visibility. */
static void net_repaint(void)
{
  if (dt_open && dt_tab == TAB_NETWORK)
    dt_paint_only = true;
}

void devtools_net_add(const struct net_log *e)
{
  mutex_lock(&dt_lk);
  int id = net_push_e(e);
  mutex_unlock(&dt_lk);
  cdp_net_begin(id, e->method, e->url, e->ctype);
  if (e->body)
    cdp_net_body(id, e->body, e->body_len, e->ctype);
  cdp_net_finish(id, e);
  net_repaint();
}

int devtools_net_begin(const char *method, const char *url)
{
  struct net_log e = {method, url, 0, 0, 0, 0, 0, NET_PENDING, 0};
  mutex_lock(&dt_lk);
  int id = net_push_e(&e);
  mutex_unlock(&dt_lk);
  cdp_net_begin(id, method, url, 0);
  net_repaint();
  return id;
}

/* Resolve in-flight row `id` to its final state; appended fresh if already evicted. */
void devtools_net_update(int id, const struct net_log *e)
{
  mutex_lock(&dt_lk);
  int slot = -1;
  for (int i = 0; i < dt_net_count; i++) {
    int s = (dt_net_start + i) % NET_MAX;
    if (dt_netinfo[s].id == id) {
      slot = s;
      break;
    }
  }
  if (slot >= 0)
    net_fill(slot, e);
  else
    net_push_e(e);
  mutex_unlock(&dt_lk);
  if (e->body)
    cdp_net_body(id, e->body, e->body_len, e->ctype);
  cdp_net_finish(id, e);
  net_repaint();
}

/* Network type filter + response-header iteration */
const char *const dt_net_filter_names[NF_N] = {"All", "Doc", "CSS", "JS", "Img", "Font", "Other"};
int dt_net_filter; /* NF_* category shown; NF_ALL = everything    */
int dt_nd_scroll;  /* response-header scroll in the request detail */

/* Case-insensitive: does `url` end with `suf`, ignoring a trailing ?query/#frag? */
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

/* Resource category for the type filter: Content-Type first, URL extension as fallback. */
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
bool dt_net_pass_slot(int slot) { return dt_net_filter == NF_ALL || net_category(&dt_netinfo[slot]) == dt_net_filter; }
int dt_net_visible_count(void)
{
  if (dt_net_filter == NF_ALL)
    return dt_net_count;
  int c = 0;
  for (int i = 0; i < dt_net_count; i++)
    if (dt_net_pass_slot((dt_net_start + i) % NET_MAX))
      c++;
  return c;
}

/* Iterate raw response-header lines ('\n'-split, trailing '\r' and blank terminator dropped). */
int dt_hdr_nlines(const char *h)
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
int dt_hdr_line(const char *h, int idx, char *out, int max)
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
