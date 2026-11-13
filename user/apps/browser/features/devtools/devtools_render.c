#include "devtools_int.h"

/* Right-side split (content | devtools): the page pane shrinks to its left. */
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
/* Drag-to-resize: width is (BW - pointer x). */
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

/* X of the panel's left edge (BW when closed); [0, devtools_left()) is the page region. */
int devtools_left(void) { return dt_open ? BW - dt_width() : BW; }

/* Y of the panel's top edge, just below the toolbar. */
int devtools_top(void) { return MENU_H + TAB_H + ADDR_H; }

static const char *const tab_names[N_TABS] = {"Elements", "Styles", "Console", "Network", "Application"};

/* Header buttons: inspect toggle, one per view (active drawn pressed), close; shared by render and click. */
#define DT_CLOSE_W 24
#define DT_PICK_W 26                 /* inspect-element toggle, left of the tab strip */
#define DT_PICK_GLYPH "\xe2\x86\x96" /* ↖ inspect cursor icon */
static int tab_x(int i)
{
  int x = DT_PICK_W;
  for (int k = 0; k < i; k++)
    x += gfx_ui_width(tab_names[k]) + 18;
  return x;
}
static int tab_w(int i) { return gfx_ui_width(tab_names[i]) + 18; }

/* Elements breadcrumb strip, shown above the find bar when a node is selected. */
#define DT_CRUMB_H 20
#define DT_CRUMB_SEP " \xe2\x80\xba " /* " › " between segments */
static int crumb_h(void) { return (dt_tab == TAB_ELEMENTS && dt_ncrumbs > 0) ? DT_CRUMB_H : 0; }

#define DT_NETDETAIL_H 168 /* Network detail panel height when a request is expanded */
#define DT_NETBAR_H 20     /* Network summary + type-filter bar at the very bottom  */
static int content_top(int top) { return top + DT_HDR; }
static int net_detail_top(void) { return BH - DT_NETBAR_H - DT_NETDETAIL_H; }
/* Console reserves a bottom row for the REPL; Elements a find bar (+ breadcrumb); Network a summary bar (+ detail). */
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
int dt_content_rows(int top)
{
  int h = content_bottom() - content_top(top);
  return h > 0 ? h / ROW_H : 0;
}
static int content_w(void) { return (BW - devtools_left()) - SCROLL_W; } /* panel minus scrollbar */

/* Total scrollable line count of the active tab (caller holds dt_lk). */
static int active_lines(void)
{
  if (dt_tab == TAB_ELEMENTS)
    return dt_dom.nlines;
  if (dt_tab == TAB_STYLES)
    return dt_sty.nlines;
  if (dt_tab == TAB_APP)
    return dt_app.nlines;
  if (dt_tab == TAB_NETWORK)
    return dt_net_visible_count();
  return dt_con_visible_count();
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

/* Syntax-highlight palette (dark theme). */
#define DT_C_TEXT 0xcdd3de
#define DT_C_PUNCT 0x9aa0a6 /* < > / = and whitespace             */
#define DT_C_TAG 0x7cb7e8   /* element tag name                   */
#define DT_C_ATTR 0xe0a060  /* attribute name                     */
#define DT_C_VALUE 0x9bce8a /* attribute value (incl. its quotes) */

/* Draw run [run,run+rl) at x in colour col, returning the advanced x. */
static int flush_run(int x, int y, char *run, int *rl, unsigned col)
{
  if (*rl <= 0)
    return x;
  run[*rl] = '\0';
  x = gfx_text_utf8(x, y, run, col, GFX_TRANSPARENT);
  *rl = 0;
  return x;
}

/* Render one DOM-markup line with token colours (in_tag tracks `<...>`, in_quote a value). */
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

/* Render one Styles line: `<tag#id.class>` header, or `name: value` with the value highlighted. */
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

/* Render one Application line: section header, or indented "name=value" with the value highlighted. */
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

/* Physical rows a console/network line occupies once wrapped at `max` columns. */
static int wrap_rows_for(const char *s, int max)
{
  int len = 0;
  while (s && s[len])
    len++;
  int r = (len + max - 1) / max;
  return r < 1 ? 1 : r;
}
/* Topmost visible (level-filtered) console line that keeps the tail in view. */
static int con_ftail(int rows, int max)
{
  int used = 0, seen = 0, vis = dt_con_visible_count();
  for (int i = dt_con_count - 1; i >= 0; i--) {
    int slot = (dt_con_start + i) % CON_MAX;
    if (!dt_con_pass(dt_con[slot].level))
      continue;
    int r = wrap_rows_for(dt_con[slot].text, max);
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
/* con_tail_start over the type-filtered network log. */
static int net_ftail(int rows, int max)
{
  int used = 0, seen = 0, vis = dt_net_visible_count();
  for (int i = dt_net_count - 1; i >= 0; i--) {
    int slot = (dt_net_start + i) % NET_MAX;
    if (!dt_net_pass_slot(slot))
      continue;
    int r = wrap_rows_for(dt_net[slot].text, max);
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

  mutex_lock(&dt_lk);

  gfx_fill_rect(left, top, w, h, 0x1e1e1e);      /* panel body */
  gfx_fill_rect(left, top, w, DT_HDR, 0x14171c); /* header row */
  gfx_fill_rect(left, top, 1, h, 0x000000);      /* splitter line */

  button_draw(left, top, DT_PICK_W, DT_HDR, DT_PICK_GLYPH, dt_pick ? BTN_PRESSED : BTN_NORMAL, NULL);
  for (int i = 0; i < N_TABS; i++)
    button_draw(left + tab_x(i), top, tab_w(i), DT_HDR, tab_names[i], i == dt_tab ? BTN_PRESSED : BTN_NORMAL, NULL);
  button_draw(BW - DT_CLOSE_W, top, DT_CLOSE_W, DT_HDR, "\xc3\x97", BTN_NORMAL, NULL); /* × */

  int ct = content_top(top), rows = dt_content_rows(top), cw = content_w();
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
    /* Console/Network lines wrap; scrolling steps by logical line. */
    bool isconsole = dt_tab == TAB_CONSOLE;
    struct conline *ring = isconsole ? dt_con : dt_net;
    int cap = isconsole ? CON_MAX : NET_MAX;
    int base = isconsole ? dt_con_start : dt_net_start;
    int raw = isconsole ? dt_con_count : dt_net_count; /* raw ring length (pre-filter) */
    int bottom = ct + rows * ROW_H, y;
    int maxstart = isconsole ? con_ftail(rows, max) : net_ftail(rows, max);
    if (isconsole && dt_con_follow)
      *sc = maxstart; /* pinned to newest */
    if (*sc > maxstart)
      *sc = maxstart;
    if (*sc < 0)
      *sc = 0;
    if (isconsole)
      dt_con_follow = (*sc >= maxstart);
    y = ct;
    int vi = 0; /* visible (filtered) index */
    for (int i = 0; i < raw && y + ROW_H <= bottom; i++) {
      int slot = (base + i) % cap;
      if (isconsole ? !dt_con_pass(ring[slot].level) : !dt_net_pass_slot(slot))
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
          dt_con_draw_runs(tx, y, s, off, off + n, fg, runs, nruns);
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
        dt_tb_line(&dt_dom, idx, line, max);
        draw_dom_line(tx, y, line);
      } else if (dt_tab == TAB_STYLES) {
        dt_tb_line(&dt_sty, idx, line, max);
        draw_style_line(tx, y, line);
      } else { /* TAB_APP */
        dt_tb_line(&dt_app, idx, line, max);
        draw_app_line(tx, y, line);
      }
    }

  scrollbar_draw(BW - SCROLL_W, ct, SCROLL_W, content_bottom() - ct, *sc, total, rows, NULL);

  if (dt_tab == TAB_CONSOLE) { /* REPL input + filter/clear toolbar */
    int iy = BH - DT_INPUT_H;
    int ftx = BW - 2 * DT_FBTN_W, fbx = BW - DT_FBTN_W; /* filter / clear button x */
    dt_input_ensure();
    gfx_fill_rect(left, iy, w, DT_INPUT_H, 0x14171c);
    gfx_fill_rect(left, iy, w, 1, 0x000000);                             /* separator above the field */
    gfx_text_utf8(left + 5, iy + (DT_INPUT_H - GLYPH_H) / 2, "\xc2\xbb", /* » prompt */
                  0x7cb7e8, GFX_TRANSPARENT);
    textfield_draw(&dt_input_tf, left + 20, iy + 2, (ftx - 4) - (left + 20), DT_INPUT_H - 4, dt_input_focused, NULL);
    button_draw(ftx, iy + 1, DT_FBTN_W - 2, DT_INPUT_H - 2,
                dt_con_filter == 0   ? "All"
                : dt_con_filter == 1 ? "Warn"
                                     : "Err",
                dt_con_filter ? BTN_PRESSED : BTN_NORMAL, NULL);
    button_draw(fbx, iy + 1, DT_FBTN_W - 2, DT_INPUT_H - 2, "Clear", BTN_NORMAL, NULL);
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
    textfield_draw(&dt_find_tf, left + 20, iy + 2, w - 24, DT_INPUT_H - 4, dt_find_focused, NULL);
  }

  if (dt_tab == TAB_NETWORK) { /* request detail (above) + summary bar */
    if (dt_net_sel >= 0 && dt_net_sel < dt_net_count) {
      int dy = net_detail_top(), dbot = BH - DT_NETBAR_H;
      struct netinfo *ni = &dt_netinfo[(dt_net_start + dt_net_sel) % NET_MAX];
      gfx_fill_rect(left, dy, w, DT_NETDETAIL_H, 0x14171c);
      gfx_fill_rect(left, dy, w, 1, 0x000000);                                            /* separator above */
      button_draw(BW - DT_CLOSE_W, dy, DT_CLOSE_W, DT_HDR, "\xc3\x97", BTN_NORMAL, NULL); /* × close */
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
      dt_net_putsize(b, &k, sizeof b, ni->size);
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
      /* Response headers: scrollable list filling the rest of the detail. */
      ly += 2;
      if (ly + GLYPH_H <= dbot) {
        gfx_text_utf8(lx, ly, "Response Headers", 0x9aa0a6, GFX_TRANSPARENT);
        ly += GLYPH_H;
      }
      int hn = dt_hdr_nlines(ni->resp_hdr);
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
          dt_hdr_line(ni->resp_hdr, hi, b, hmax);
          draw_style_line(lx, ly + r * GLYPH_H, b); /* "Name: value" colouring */
        }
        if (hn > hrows)
          scrollbar_draw(BW - SCROLL_W, ly, SCROLL_W, hrows * GLYPH_H, dt_nd_scroll, hn, hrows, NULL);
      }
    }

    int by = BH - DT_NETBAR_H; /* summary + type-filter bar */
    gfx_fill_rect(left, by, w, DT_NETBAR_H, 0x14171c);
    gfx_fill_rect(left, by, w, 1, 0x000000);
    int fbw = 56;
    button_draw(left + 2, by + 1, fbw, DT_NETBAR_H - 2, dt_net_filter_names[dt_net_filter],
                dt_net_filter ? BTN_PRESSED : BTN_NORMAL, NULL);
    int nshow = 0, failed = 0, pending = 0;
    long bytes = 0;
    for (int i = 0; i < dt_net_count; i++) {
      int slot = (dt_net_start + i) % NET_MAX;
      if (!dt_net_pass_slot(slot))
        continue;
      nshow++;
      struct netinfo *si = &dt_netinfo[slot];
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
    dt_net_putsize(sb, &k, sizeof sb, bytes > 0x7fffffff ? 0x7fffffff : (int)bytes);
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
  mutex_unlock(&dt_lk);
}

/* Logical Network row at screen-y `my` accounting for wrapping; -1 below the last row. Caller holds dt_lk. */
static int net_row_at(int my)
{
  int top = devtools_top(), ct = content_top(top);
  int bottom = ct + dt_content_rows(top) * ROW_H;
  int max = (content_w() - 8) / GLYPH_W;
  if (max > 255)
    max = 255;
  if (max < 1)
    max = 1;
  int y = ct, vi = 0, sc = dt_scroll[TAB_NETWORK];
  for (int i = 0; i < dt_net_count && y + ROW_H <= bottom; i++) {
    int slot = (dt_net_start + i) % NET_MAX;
    if (!dt_net_pass_slot(slot))
      continue; /* hidden by the type filter */
    if (vi < sc) {
      vi++;
      continue;
    } /* scrolled above the viewport */
    int hgt = wrap_rows_for(dt_net[slot].text, max) * ROW_H;
    if (my >= y && my < y + hgt)
      return i; /* the raw ring index */
    y += hgt;
    vi++;
  }
  return -1;
}

/* Select and scroll to the next Elements line matching the find box, wrapping. */
static void dt_find_next(int fresh)
{
  if (!dt_find[0])
    return;
  int rows = dt_content_rows(devtools_top());
  mutex_lock(&dt_lk);
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
      dt_tb_line(&dt_dom, i, lb, sizeof lb);
      if (contains_ci(lb, dt_find)) {
        found = i;
        break;
      }
    }
  }
  int node = (found >= 0 && found < dt_dom_node_cap) ? dt_dom_node[found] : -1;
  mutex_unlock(&dt_lk);
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

int devtools_field_at(int mx, int my, struct textfield **tf, int *x, int *w)
{
  if (!dt_open || mx < devtools_left() || my < BH - DT_INPUT_H)
    return 0;
  int left = devtools_left(), iy = BH - DT_INPUT_H;
  if (dt_tab == TAB_CONSOLE) {
    dt_input_ensure();
    *tf = &dt_input_tf;
    *x = left + 20;
    *w = (BW - 2 * DT_FBTN_W - 4) - (left + 20);
    if (mx < *x || mx >= *x + *w || my < iy + 2)
      return 0;
    dt_input_focused = true;
    return 1;
  }
  if (dt_tab == TAB_ELEMENTS) {
    if (!dt_find_init) {
      textfield_init(&dt_find_tf, dt_find, sizeof dt_find);
      dt_find_init = true;
    }
    *tf = &dt_find_tf;
    *x = left + 20;
    *w = BW - left - 24;
    if (mx < *x || mx >= *x + *w || my < iy + 2)
      return 0;
    dt_find_focused = true;
    return 1;
  }
  return 0;
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
    if (button_hit(left, top, DT_PICK_W, DT_HDR, mx, my, NULL)) {
      devtools_pick_toggle();
      return 1; /* inspect-element toggle */
    }
    if (button_hit(BW - DT_CLOSE_W, top, DT_CLOSE_W, DT_HDR, mx, my, NULL)) {
      devtools_toggle();
      return 1;
    }
    for (int i = 0; i < N_TABS; i++)
      if (button_hit(left + tab_x(i), top, tab_w(i), DT_HDR, mx, my, NULL)) {
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
    if (button_hit(ftx, iy + 1, DT_FBTN_W - 2, DT_INPUT_H - 2, mx, my, NULL)) {
      dt_con_filter = (dt_con_filter + 1) % 3;
      dt_con_follow = true;
      B->seq++;
      return 1;
    }
    if (button_hit(fbx, iy + 1, DT_FBTN_W - 2, DT_INPUT_H - 2, mx, my, NULL)) {
      dt_con_clear();
      B->seq++;
      return 1;
    }
    dt_input_ensure();
    drag_field(&dt_input_tf, mx, my, left + 20, iy + 2, (ftx - 4) - (left + 20), DT_INPUT_H - 4);
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
    drag_field(&dt_find_tf, mx, my, left + 20, iy + 2, w - 24, DT_INPUT_H - 4);
    dt_find_focused = true;
    B->seq++;
    return 1;
  }

  if (crumb_h() && my >= BH - DT_INPUT_H - DT_CRUMB_H && my < BH - DT_INPUT_H) { /* breadcrumb */
    int cx = left + 6, hit = -1;
    mutex_lock(&dt_lk);
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
    mutex_unlock(&dt_lk);
    if (hit >= 0) {
      dt_sel = hit;
      dt_dirty = true;
      B->seq++;
    }
    return 1;
  }

  if (dt_tab == TAB_NETWORK && my >= BH - DT_NETBAR_H) { /* summary / type-filter bar */
    int by = BH - DT_NETBAR_H, fbw = 56;
    if (button_hit(left + 2, by + 1, fbw, DT_NETBAR_H - 2, mx, my, NULL)) {
      dt_net_filter = (dt_net_filter + 1) % NF_N; /* cycle the resource-type filter */
      dt_scroll[TAB_NETWORK] = 0;
      if (dt_net_sel >= 0) { /* close the detail if it's now hidden */
        mutex_lock(&dt_lk);
        if (dt_net_sel >= dt_net_count || !dt_net_pass_slot((dt_net_start + dt_net_sel) % NET_MAX))
          dt_net_sel = -1;
        mutex_unlock(&dt_lk);
      }
      B->seq++;
    }
    return 1;
  }

  if (dt_tab == TAB_NETWORK && dt_net_sel >= 0 && my >= net_detail_top()) { /* request detail */
    if (button_hit(BW - DT_CLOSE_W, net_detail_top(), DT_CLOSE_W, DT_HDR, mx, my, NULL)) {
      dt_net_sel = -1;
      B->seq++; /* × closes the detail and restores the list height */
    }
    return 1; /* swallow other clicks inside the detail panel */
  }

  int ct = content_top(top), rows = dt_content_rows(top);
  mutex_lock(&dt_lk);
  int total = active_lines();
  mutex_unlock(&dt_lk);

  int part = scrollbar_hit(BW - SCROLL_W, ct, SCROLL_W, content_bottom() - ct, mx, my, NULL);
  if (part != SCROLLBAR_NONE) { /* scrollbar (reused widget) */
    if (part == SCROLLBAR_UP)
      dt_scroll[dt_tab] -= 1;
    else if (part == SCROLLBAR_DOWN)
      dt_scroll[dt_tab] += 1;
    else
      dt_scroll[dt_tab] = scrollbar_pos(BW - SCROLL_W, ct, SCROLL_W, content_bottom() - ct, mx, my, dt_scroll[dt_tab],
                                        total, rows, NULL);
    dt_scroll[dt_tab] = clamp_scroll(dt_scroll[dt_tab], total, rows);
    if (dt_tab == TAB_CONSOLE)
      dt_con_follow = (dt_scroll[dt_tab] >= total - rows);
    B->seq++;
    return 1;
  }

  if (mx < BW - SCROLL_W && dt_tab == TAB_ELEMENTS) { /* select a DOM node, or toggle its ▸/▾ */
    int row = dt_scroll[dt_tab] + (my - ct) / ROW_H;
    int cell = (mx - (left + 6)) / GLYPH_W; /* monospace column of the click */
    mutex_lock(&dt_lk);
    bool ok = row >= 0 && row < dt_dom.nlines && row < dt_dom_node_cap;
    int node = ok ? dt_dom_node[row] : -1;
    int tri = -1; /* cell of this row's ▸/▾, or -1 */
    if (ok) {
      char lb[256];
      dt_tb_line(&dt_dom, row, lb, sizeof lb);
      int sp = 0;
      while (lb[sp] == ' ')
        sp++; /* indent = one cell per space */
      if ((unsigned char)lb[sp] == 0xe2 && (unsigned char)lb[sp + 1] == 0x96 &&
          ((unsigned char)lb[sp + 2] == 0xba || (unsigned char)lb[sp + 2] == 0xbc))
        tri = sp;
    }
    mutex_unlock(&dt_lk);
    if (ok) {
      /* dt_toggle_collapsed reallocs dt_collapsed, which the worker reads under dt_lk. */
      if (tri >= 0 && cell >= 0 && cell <= tri + 1) {
        mutex_lock(&dt_lk);
        dt_toggle_collapsed(node);
        mutex_unlock(&dt_lk);
      } /* hit the triangle */
      else
        dt_sel = node;
      dt_dirty = true;
      B->seq++;
    }
  }

  if (mx < BW - SCROLL_W && dt_tab == TAB_NETWORK) { /* expand / collapse a request row */
    mutex_lock(&dt_lk);
    int row = net_row_at(my);
    mutex_unlock(&dt_lk);
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
  /* An expanded Network request takes the wheel for its header list. */
  if (dt_tab == TAB_NETWORK && dt_net_sel >= 0) {
    int prev = dt_nd_scroll;
    dt_nd_scroll -= delta * 3;
    if (dt_nd_scroll < 0)
      dt_nd_scroll = 0;
    if (dt_nd_scroll != prev)
      B->seq++; /* render clamps to the header count */
    return 1;
  }
  int rows = dt_content_rows(devtools_top());
  int prev = dt_scroll[dt_tab], next;
  if (dt_tab == TAB_CONSOLE || dt_tab == TAB_NETWORK) {
    /* Max scroll is the wrap-aware tail (as in render), else render clamps the wheel step back. */
    int cols = (content_w() - 8) / GLYPH_W;
    if (cols > 255)
      cols = 255;
    if (cols < 1)
      cols = 1;
    mutex_lock(&dt_lk);
    int maxstart = dt_tab == TAB_CONSOLE ? con_ftail(rows, cols) : net_ftail(rows, cols);
    mutex_unlock(&dt_lk);
    next = prev - delta * 3;
    if (next > maxstart)
      next = maxstart;
    if (next < 0)
      next = 0;
    if (dt_tab == TAB_CONSOLE)
      dt_con_follow = (next >= maxstart);
  } else {
    mutex_lock(&dt_lk);
    int total = active_lines();
    mutex_unlock(&dt_lk);
    next = clamp_scroll(prev - delta * 3, total, rows);
  }
  if (next != prev) {
    dt_scroll[dt_tab] = next;
    B->seq++;
  } /* repaint only on real move */
  return 1;
}

/* Feed a key to the Console REPL input; returns 1 when consumed (Enter queues for the worker, Esc blurs). */
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
    if (field_key(&dt_find_tf, c, mods)) {
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
      dt_con_follow = true;
      B->seq++;
    }
    return 1;
  }
  /* Field edits repaint only the dock (dt_paint_only); a full render per key lags heavy pages. */
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
  if (field_key(&dt_input_tf, c, mods)) {
    dt_paint_only = true;
    return 1;
  }
  return 0; /* not an edit key: fall through */
}
