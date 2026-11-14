/* draw.c: window, taskbar and overlay rendering */
#include "comp.h"

/* rendering */

/* Vertical gradient (deep indigo -> slate), a function of y only: one colour per screen row */
static unsigned *bg_rows;
static int bg_rows_h;

static void bg_rows_build(void)
{
  free(bg_rows);
  bg_rows = malloc((unsigned)screen_h * sizeof *bg_rows);
  bg_rows_h = bg_rows ? screen_h : 0;
  for (int y = 0; y < bg_rows_h; y++) {
    int t = y * 255 / screen_h;
    unsigned r = (unsigned)(24 + t * 40 / 255);
    unsigned g = (unsigned)(26 + t * 56 / 255);
    unsigned b = (unsigned)(54 + t * 70 / 255);
    bg_rows[y] = (r << 16) | (g << 8) | b;
  }
}

static void draw_background(void)
{
  if (bg_rows_h != screen_h)
    bg_rows_build();
  if (!bg_rows_h)
    return;
  for (int y = clip_y0; y < clip_y1; y++)
    gfx_fill_rect(0, y, screen_w, 1, bg_rows[y]);
}

/* Blit a window's client buffer into the back buffer at the client rect */
static void blit_client(int dx, int dy, const unsigned *src, int sw, int sh, int sstride)
{
  long long x1 = (long long)dx + sw, y1 = (long long)dy + sh;
  int x0 = dx > clip_x0 ? dx : clip_x0;
  int y0 = dy > clip_y0 ? dy : clip_y0;
  int x2 = x1 <= 0 ? 0 : x1 >= clip_x1 ? clip_x1 : (int)x1;
  int y2 = y1 <= 0 ? 0 : y1 >= clip_y1 ? clip_y1 : (int)y1;
  if (x0 < 0)
    x0 = 0;
  if (y0 < 0)
    y0 = 0;
  if (x2 > screen_w)
    x2 = screen_w;
  if (y2 > screen_h)
    y2 = screen_h;
  if (sw <= 0 || sh <= 0 || x2 <= x0 || y2 <= y0)
    return;

  int src_x = x0 - dx;
  for (int y = y0; y < y2; y++)
    memcpy(back + (long)y * fb_pitch + x0, src + (long)(y - dy) * sstride + src_x, (unsigned)(x2 - x0) * 4);
}

/* the per-window client buffer */
static struct clientmap cmap[WM_LIST_MAX];
static int ncmap;

static const struct winmap *client_for(int id)
{
  for (int i = 0; i < ncmap; i++)
    if (cmap[i].id == id)
      return cmap[i].valid ? &cmap[i].m : 0;
  return 0;
}

/* Refresh the cached buffer view of every enumerated window and forget ids that are gone */
void refresh_maps(void)
{
  for (int i = 0; i < ncmap;) {
    if (slot_of(cmap[i].id) < 0) {
      cmap[i] = cmap[--ncmap];
    } else
      i++;
  }
  /* (re)map every enumerated window */
  for (int s = 0; s < nwin; s++) {
    int id = cur[s].id, found = -1;
    for (int i = 0; i < ncmap; i++)
      if (cmap[i].id == id) {
        found = i;
        break;
      }
    if (found < 0) {
      if (ncmap >= WM_LIST_MAX)
        continue;
      found = ncmap++;
      cmap[found].id = id;
      /* The slot may hold a stale copy left by the swap-remove above; a same-sized one would skip win_map */
      cmap[found].valid = false;
    }
    if (!cmap[found].valid || cmap[found].m.w != cur[s].buf_w || cmap[found].m.h != cur[s].buf_h)
      cmap[found].valid = (win_map(id, &cmap[found].m) == 0);
  }
  maxed_forget_gone(); /* clear maximize state for closed windows */
}

static void draw_close_button(int bx, int by, unsigned bg)
{
  gfx_fill_rect(bx, by, TB_BTN_W, TB_BTN_W, bg);
  for (int d = 0; d < 7; d++) {
    gfx_pixel(bx + 4 + d, by + 4 + d, 0xffffff);
    gfx_pixel(bx + 10 - d, by + 4 + d, 0xffffff);
  }
}

void client_rect(const struct winrec *w, int *x, int *y, int *width, int *height)
{
  if (fs_active(w->id)) {
    *x = w->x;
    *y = w->y;
    if (width)
      *width = w->w;
    if (height)
      *height = w->h;
    return;
  }
  *x = w->x + WM_BORDER;
  *y = w->y + WM_TITLE_H;
  if (width)
    *width = w->w - 2 * WM_BORDER;
  if (height)
    *height = w->h - WM_TITLE_H - WM_BORDER;
}

static void draw_window(const struct winrec *w, bool focused)
{
  unsigned title_bg = focused ? 0x2f6fb0 : 0x55606e;
  unsigned frame = 0x10141a;
  if (w->id == flash_id && flash_frames > 0 && ((flash_frames >> 1) & 1))
    title_bg = 0xf0a020;
  bool fs = fs_active(w->id);

  gfx_fill_rect(w->x, w->y, w->w, w->h, fs ? 0x000000 : frame);
  if (fs) {
    int fx, fy, fw, fh;
    client_rect(w, &fx, &fy, &fw, &fh);
    const struct winmap *fm = client_for(w->id);
    if (fm && fm->vaddr)
      blit_client(fx, fy, (const unsigned *)(unsigned long)fm->vaddr, fm->w < fw ? fm->w : fw, fm->h < fh ? fm->h : fh,
                  fm->stride);
    return;
  }
  gfx_fill_rect(w->x, w->y, w->w, WM_TITLE_H, title_bg);

  const unsigned *icon = window_icon(w);
  int title_x = w->x + 8 + (icon ? ICON_PX + 4 : 0);
  if (icon)
    gfx_blit_argb(w->x + 8, w->y + (WM_TITLE_H - ICON_PX) / 2, ICON_PX, ICON_PX, icon);
  int title_right = w->x + w->w - 4;
  for (int k = 0; k < 3; k++) {
    int bbx, bby;
    if (titlebtn_box(w, k, &bbx, &bby) && bbx < title_right)
      title_right = bbx;
  }
  char dt[WM_TITLE_DISP];
  disp_title(w, dt);
  gfx_ui_text_clip(title_x, w->y + (WM_TITLE_H - UIFONT_H) / 2, dt, 0xffffff, title_right - title_x - 6);

  int bx, by;
  if (titlebtn_box(w, 2, &bx, &by)) { /* minimize */
    gfx_fill_rect(bx, by, TB_BTN_W, TB_BTN_W, 0x3a4658);
    gfx_fill_rect(bx + 3, by + 9, 8, 2, 0xffffff);
  }
  if (titlebtn_box(w, 1, &bx, &by)) { /* maximize / restore */
    gfx_fill_rect(bx, by, TB_BTN_W, TB_BTN_W, 0x3a4658);
    if (is_maxed(w->id) >= 0) {
      gfx_rect(bx + 4, by + 2, 6, 6, 0xffffff);
      gfx_fill_rect(bx + 2, by + 5, 6, 6, 0x3a4658);
      gfx_rect(bx + 2, by + 5, 6, 6, 0xffffff);
    } else {
      gfx_rect(bx + 3, by + 3, 8, 8, 0xffffff);
    }
  }
  if (titlebtn_box(w, 0, &bx, &by)) { /* close */
    draw_close_button(bx, by, 0xc04030);
  }

  /* Client area: blit the app's pixel buffer, or fill the chrome bg if unmapped */
  int cx, cy, cw, ch;
  client_rect(w, &cx, &cy, &cw, &ch);
  const struct winmap *cm = client_for(w->id);
  if (cm && cm->vaddr) {
    int bw = cm->w < cw ? cm->w : cw;
    int bh = cm->h < ch ? cm->h : ch;
    blit_client(cx, cy, (const unsigned *)(unsigned long)cm->vaddr, bw, bh, cm->stride);
    if (bw < cw)
      gfx_fill_rect(cx + bw, cy, cw - bw, ch, 0x10141a);
    if (bh < ch)
      gfx_fill_rect(cx, cy + bh, cw, ch - bh, 0x10141a);
  } else {
    gfx_fill_rect(cx, cy, cw, ch, 0x10141a);
  }

  gfx_rect(w->x, w->y, w->w, w->h, 0x05070a);
}

static void draw_taskbar_windows(int y, int uy, int right)
{
  tb_band_right = right;
  taskbar_layout_cached(right);
  int focus = topmost_visible();
  int focus_root = focus >= 0 ? root_id_of(cur[focus].id) : -1;
  int by = y + 3, bh = WM_BAR_H - 6;
  for (int k = 0; k < tb_cache.n; k++) {
    struct winrec *w = &cur[tb_cache.idxs[k]];
    bool active = (w->id == focus_root);
    unsigned bg = active ? 0x2f6fb0 : win_minimized(w) ? 0x12161e : 0x1d2533;
    gfx_fill_rect(tb_cache.bxs[k], by, tb_cache.bws[k], bh, bg);
    gfx_fill_rect(tb_cache.bxs[k], by, tb_cache.bws[k], 1, active ? 0x4f8fd0 : 0x2a3550);
    unsigned fg = win_minimized(w) ? 0x8b94a4 : 0xc8d0dc;
    char dt[WM_TITLE_DISP];
    disp_title(w, dt);
    const unsigned *icon = window_icon(w);
    int tx = tb_cache.bxs[k] + 8;
    if (icon) {
      gfx_blit_argb(tx, by + (bh - ICON_PX) / 2, ICON_PX, ICON_PX, icon);
      tx += ICON_PX + 4;
    }
    gfx_ui_text_clip(tx, uy, dt, fg, tb_cache.bxs[k] + tb_cache.bws[k] - 4 - tx);
  }
}

static void draw_taskbar(void)
{
  int y = screen_h - WM_BAR_H;
  gfx_fill_rect(0, y, screen_w, WM_BAR_H, 0x0c0e14);
  gfx_fill_rect(0, y, screen_w, 1, 0x2a3550);
  unsigned btn = start_open ? 0x2f6fb0 : 0x0c0e14;
  int uy = y + (WM_BAR_H - UIFONT_H) / 2;
  gfx_fill_rect(0, y + 1, START_W, WM_BAR_H - 1, btn);
  gfx_ui_text(10, uy, "OwOS", 0x9ad0ff);

  unsigned t = frame_clock;
  char s[9] = "00:00:00";
  put2(s + 0, (int)((t >> 16) & 0xFF));
  put2(s + 3, (int)((t >> 8) & 0xFF));
  put2(s + 6, (int)(t & 0xFF));
  int clock_x = screen_w - gfx_ui_width(s) - 12;
  gfx_ui_text(clock_x, uy, s, 0xc8d0dc);

  int tray_left = tray_draw(clock_x - 6, y);
  draw_taskbar_windows(y, uy, tray_left - 4);
}

static void draw_start_menu(void)
{
  int n = apps_menu_count();
  int top = sm_top();
  gfx_fill_rect(0, top, SM_W, sm_h(), 0x141a24);
  for (int i = 0; i < n; i++) {
    int iy = top + i * SM_ITEM_H;
    const unsigned *icon = app_icon(i);
    if (icon)
      gfx_blit_argb(8, iy + (SM_ITEM_H - ICON_PX) / 2, ICON_PX, ICON_PX, icon);
    gfx_ui_text(icon ? 8 + ICON_PX + 6 : 10, iy + (SM_ITEM_H - UIFONT_H) / 2, apps.v[i]->name, 0xc8d0dc);
  }
  int sep_y = top + sm_apps_h() + SM_SEP_H / 2;
  gfx_fill_rect(8, sep_y, SM_W - 16, 1, 0x05070a);
  int py = top + sm_apps_h() + SM_SEP_H;
  for (int i = 0; i < sm_power_n(); i++) {
    int iy = py + i * SM_ITEM_H;
    gfx_ui_text(10, iy + (SM_ITEM_H - UIFONT_H) / 2, power_label[sm_power_action(i)], 0xa8b4c4);
  }
  gfx_rect(0, top, SM_W, sm_h(), 0x05070a);
}

static void draw_tb_menu(void)
{
  static const char *const items[TBM_N] = {"Close", "Force Kill"};
  int x = tb_menu_x, y = tb_menu_y;
  gfx_fill_rect(x, y, TBM_W, tbm_h(), 0x141a24);
  for (int i = 0; i < TBM_N; i++) {
    int iy = y + i * TBM_ITEM_H;
    unsigned fg = (i == 1) ? 0xe08070 : 0xc8d0dc;
    gfx_ui_text(x + 12, iy + (TBM_ITEM_H - UIFONT_H) / 2, items[i], fg);
  }
  gfx_rect(x, y, TBM_W, tbm_h(), 0x05070a);
}

/* Alt+Tab overlay */
static void switcher_box(int *px, int *py, int *pw, int *ph)
{
  int h = AT_PAD * 2 + at_count * AT_ROW_H;
  int maxh = screen_h - WM_BAR_H - 20;
  if (h > maxh)
    h = maxh;
  *pw = AT_PANEL_W;
  *ph = h;
  *px = (screen_w - AT_PANEL_W) / 2;
  *py = (screen_h - h) / 2;
}
static void draw_switcher(void)
{
  int x, y, pw, ph;
  switcher_box(&x, &y, &pw, &ph);
  gfx_fill_rect(x, y, pw, ph, 0x141a24);
  gfx_rect(x, y, pw, ph, 0x05070a);
  gfx_rect(x + 1, y + 1, pw - 2, ph - 2, 0x2a3550);
  int rows = (ph - 2 * AT_PAD) / AT_ROW_H;
  if (rows > at_count)
    rows = at_count;
  int first = at_sel >= rows ? at_sel - rows + 1 : 0; /* keep the selection on screen */
  for (int r = 0; r < rows; r++) {
    int i = first + r;
    int ry = y + AT_PAD + r * AT_ROW_H;
    if (i == at_sel)
      gfx_fill_rect(x + 4, ry, pw - 8, AT_ROW_H, 0x2f6fb0);
    int s = slot_of(at_order[i]);
    if (s < 0)
      continue;
    struct winrec *w = &cur[s];
    unsigned fg = (i == at_sel) ? 0xffffff : 0xc8d0dc;
    const unsigned *icon = window_icon(w);
    int tx = x + 14;
    if (icon) {
      gfx_blit_argb(tx, ry + (AT_ROW_H - ICON_PX) / 2, ICON_PX, ICON_PX, icon);
      tx += ICON_PX + 6;
    }
    gfx_ui_text_clip(tx, ry + (AT_ROW_H - UIFONT_H) / 2, w->title, fg, x + pw - 14 - tx);
  }
}

/* crash windows: rendered directly (no kernel window) */
struct crashwin crashwins[MAXCRASHWIN];

void crash_ingest(void)
{
  struct crash_rec recs[MAXCRASHWIN];
  int n = crash_poll(recs, MAXCRASHWIN);
  for (int i = 0; i < n; i++) {
    for (int s = 0; s < MAXCRASHWIN; s++) {
      if (crashwins[s].used)
        continue;
      crashwins[s].used = true;
      crashwins[s].x = 140 + s * 22;
      crashwins[s].y = 110 + s * 22;
      crashwins[s].w = 460;
      crashwins[s].h = 300;
      crashwins[s].rec = recs[i];
      force_full = true;
      break;
    }
  }
}

/* `v` in hex, at least 8 digits and all of a wider one; out holds 17 bytes. */
static const char *hexw(sys_word v, char *out)
{
  static const char d[] = "0123456789abcdef";
  int n = 8;
  while (n < (int)sizeof v * 2 && (v >> (n * 4)))
    n++;
  for (int i = 0; i < n; i++)
    out[i] = d[(v >> ((n - 1 - i) * 4)) & 0xF];
  out[n] = '\0';
  return out;
}
static void draw_crashwin(struct crashwin *c)
{
  unsigned bg = 0x1a1020;
  int x0 = c->x, y0 = c->y;
  gfx_fill_rect(x0, y0, c->w, WM_TITLE_H, 0xc04030);
  gfx_ui_text(x0 + 8, y0 + (WM_TITLE_H - UIFONT_H) / 2, "crash", 0xffffff);
  /* close box (X) top-right */
  int bx = x0 + c->w - 4 - TB_BTN_W, by = y0 + 4;
  draw_close_button(bx, by, 0x902020);
  int cy = y0 + WM_TITLE_H;
  gfx_fill_rect(x0, cy, c->w, c->h - WM_TITLE_H, bg);

  struct crash_rec *r = &c->rec;
  char hx[20];
  int x = x0 + 12, y = cy + 10, lh = 16;
  char title[48];
  int t = 0;
  for (; r->name[t] && t < 23; t++)
    title[t] = r->name[t];
  const char *suf = " has crashed.";
  for (int k = 0; suf[k] && t < 47; k++)
    title[t++] = suf[k];
  title[t] = '\0';
  gfx_ui_text(x, y, title, 0xff8080);
  y += lh + 8;

  gfx_ui_text(x, y, "fault vector:", 0x9098a0);
  char vs[4];
  put2(vs, (int)(r->vector % 100));
  vs[2] = 0;
  gfx_ui_text(x + 120, y, vs, 0xe0e6ee);
  y += lh;
  gfx_ui_text(x, y, "pc=", 0x9098a0);
  gfx_ui_text(x + 40, y, hexw(r->pc, hx), 0x66ddff);
  if (r->page_fault) {
    gfx_ui_text(x + 190, y, "addr=", 0x9098a0);
    gfx_ui_text(x + 240, y, hexw(r->addr, hx), 0xffe08a);
  }
  y += lh;
  gfx_ui_text(x, y, "err=", 0x9098a0);
  gfx_ui_text(x + 40, y, hexw(r->err, hx), 0xc8d0dc);
  y += lh + 8;
  gfx_ui_text(x, y, "stack trace:", 0x9098a0);
  y += lh;
  for (int i = 0; i < r->ntrace && i < CRASH_TRACE_MAX; i++) {
    gfx_ui_text(x + 12, y, hexw(r->trace[i], hx), 0xb0c0d0);
    if (i == 0)
      gfx_ui_text(x + 12 + 150, y, "<- fault", 0x707880);
    y += lh;
  }
}

/* compose */
static bool clip_hits(int x, int y, int w, int h)
{
  return x < clip_x1 && x + w > clip_x0 && y < clip_y1 && y + h > clip_y0;
}

static bool occluded(int i)
{
  const struct winrec *w = &cur[i];
  int x0 = w->x > clip_x0 ? w->x : clip_x0, y0 = w->y > clip_y0 ? w->y : clip_y0;
  int x1 = w->x + w->w < clip_x1 ? w->x + w->w : clip_x1;
  int y1 = w->y + w->h < clip_y1 ? w->y + w->h : clip_y1;
  for (int j = i + 1; j < nwin; j++) {
    const struct winrec *o = &cur[j];
    if (win_visible(o) && o->x <= x0 && o->y <= y0 && o->x + o->w >= x1 && o->y + o->h >= y1)
      return true;
  }
  return false;
}

void compose(int mx, int my)
{
  draw_background();
  int top = topmost_visible();
  for (int i = 0; i < nwin; i++)
    if (win_visible(&cur[i]) && clip_hits(cur[i].x, cur[i].y, cur[i].w, cur[i].h) && !occluded(i))
      draw_window(&cur[i], i == top);
  bool fs = fs_top();
  if (!fs && clip_hits(0, screen_h - WM_BAR_H, screen_w, WM_BAR_H))
    draw_taskbar();
  if (start_open && !fs)
    draw_start_menu();
  if (tb_menu_open && !fs)
    draw_tb_menu();
  if (tray_menu_open && !fs)
    tray_menu_draw();
  for (int i = 0; i < MAXCRASHWIN; i++)
    if (crashwins[i].used)
      draw_crashwin(&crashwins[i]);
  if (at_active)
    draw_switcher();
  if (pw_open && clip_hits(pw_x(), pw_y(), PW_W, pw_h()))
    draw_power();
  if (osd_kind != OSD_NONE && clip_hits(osd_x(), osd_y(), OSD_W, OSD_H))
    draw_osd();
  if (!hw_cursor)
    draw_cursor(pick_cursor(mx, my), mx, my);
}
