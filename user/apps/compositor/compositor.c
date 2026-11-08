/* compositor.c: the user-space window manager and desktop */
#include "comp.h"

/* Active-frame target rate (Hz) */
static int target_hz = 60;
static unsigned active_period(void) { return target_hz > 0 ? 1000u / (unsigned)target_hz : 16u; }

/* Frame pacing: sleep only the slack to a running deadline */
static unsigned frame_deadline;

static void frame_pace(unsigned period)
{
  unsigned now = msec();
  int slack = (int)(frame_deadline - now); /* signed: tolerates wrap + overrun */
  if (slack < 0 || slack > (int)period) {
    frame_deadline = now + period;
    return;
  }
  frame_deadline += period;
  msleep((unsigned)slack);
}

/* prev-frame */
static struct pfsnap pf[WM_LIST_MAX];
static int npf;

/* Detect a resolution change via fb_info each frame */
/* Center each transient dialog over its parent once, so it stays draggable */
static int centered[WM_LIST_MAX], ncentered;
static bool was_centered(int id)
{
  for (int i = 0; i < ncentered; i++)
    if (centered[i] == id)
      return true;
  return false;
}
static void center_new_dialogs(void)
{
  for (int i = 0; i < ncentered;) { /* prune closed dialogs */
    if (slot_of(centered[i]) < 0)
      centered[i] = centered[--ncentered];
    else
      i++;
  }
  for (int i = 0; i < nwin; i++) {
    int pid = parent_id_of(cur[i].id);
    if (pid < 0 || was_centered(cur[i].id))
      continue;
    int ps = slot_of(pid);
    if (ps < 0)
      continue;
    struct winrec *w = &cur[i], *p = &cur[ps];
    int nx = p->x + (p->w - w->w) / 2;
    int ny = p->y + (p->h - w->h) / 2;
    clamp_frame_position(&nx, &ny, w->w, w->h);
    win_set_geom(w->id, nx, ny, w->w, w->h);
    w->x = nx;
    w->y = ny; /* reflect this frame's compose */
    if (ncentered < WM_LIST_MAX)
      centered[ncentered++] = w->id;
  }
}

static void check_resolution(void)
{
  struct fbinfo fi;
  if (fb_info(&fi) < 0)
    return;
  if (fi.w == hw_w && fi.h == hw_h && fi.pitch == hw_pitch && !scale_stale)
    return;

  bind_mode();
  for (int i = 0; i < nwin; i++) {
    struct winrec *w = &cur[i];
    int m = is_maxed(w->id);
    if (m >= 0) {
      win_set_geom(w->id, 0, 0, screen_w, work_bottom());
      continue;
    }
    int nx = w->x, ny = w->y, nw = w->w, nh = w->h;
    fit_frame_to_work_area(&nx, &ny, &nw, &nh);
    if (nx != w->x || ny != w->y || nw != w->w || nh != w->h)
      win_set_geom(w->id, nx, ny, nw, nh);
  }
}

int main(void)
{
  if (getuid() != 0) {
    puts("compositor: must run as root\n");
    return 1;
  }

  if (bind_mode() < 0) {
    puts("compositor: no framebuffer\n");
    return 1;
  }

  apps_load();

  /* Session autostart: clipboard server, notification daemon */
  kid_track(spawn("/bin/clipd", 0));
  kid_track(spawn("/bin/vmclipd", 0)); /* VMware host clipboard bridge; self-exits off VMware */
  kid_track(spawn("/bin/notifyd", 0));

  int prev_mx = -9999, prev_my = -9999, prev_cur = -1;
  bool prev_start = false, prev_tb_menu = false, prev_at_active = false, prev_pw_open = false;
  int prev_pw_sel = -1;
  int prev_at_sel = -1;
  int prev_focus = -1, last_sec = -1;
  bool inited = false;

  /* Restore saved tunables, then adopt the live refresh rate as the pacing target */
  conf_load();
  tun_restore();
  target_hz = rate_now();
  if (target_hz <= 0)
    target_hz = 60;
  unsigned conf_poll_at = 0;
  can_sleep = power(3) > 0;

  for (;;) {
    struct inputstate in;
    if (input_poll(&in) < 0) {
      msleep(16);
      continue;
    }
    if (resumed) {
      resumed = false;
      in.nkeys = 0;
      prev_wheel = in.wheel;
      prev_down = in.buttons & 1;
      prev_rdown = in.buttons & 2;
      osd_kind = OSD_NONE;
      force_full = true;
      frame_deadline = msec();
    }
    frame_clock = now_hms();

    cliphist_reap(); /* Super+V viewer exited -> maybe paste */
    reap_kids();
    crash_ingest();

    /* Refresh the window set, then (re)pin client buffers to match it */
    nwin = win_enum(cur, WM_LIST_MAX);
    if (nwin < 0)
      nwin = 0;
    refresh_maps(); /* maps must be current before compose */
    zorder_apply(); /* reconcile z-stack; reorder cur[] bottom->top */
    taskbar_layout_dirty = true;
    center_new_dialogs(); /* place a fresh dialog over its parent */

    check_resolution(); /* Settings may have changed the fb mode */

    /* Pick up a Settings change to any tunable ~1x/sec */
    unsigned nowms = msec();
    if ((int)(nowms - conf_poll_at) >= 0) {
      conf_poll_at = nowms + 1000;
      scale_stale = scale_now() != ui_scale;
      if (tun_poll()) {
        conf_save();
        int r = rate_now();
        if (r > 0)
          target_hz = r;
      }
    }

    if (flash_frames > 0) {
      flash_frames--;
      force_full = true;
    } /* pulse a modal */

    /* Drain any keys parked by paste back-pressure before pulling new input */
    pending_flush();
    paste_flush(); /* inject any in-progress clipboard paste */

    /* Keys first (may open menus / switch focus) */
    for (int i = 0; i < in.nkeys && !resumed; i++)
      feed_key(in.keys[i].c, in.keys[i].mods);

    int dz = in.wheel - prev_wheel; /* wheel is a running total; delta = this frame */
    prev_wheel = in.wheel;

    handle_input(in.mx, in.my, in.buttons, dz);

    int mx = in.mx, my = in.my;
    ndmg = 0;
    bool osd = osd_tick();
    bool full = !inited || force_full;
    force_full = false;
    bool was_hw = hw_cursor;
    if (cursor_hw_update(pick_cursor(mx, my)) != was_hw)
      full = true;

    /* Focus / z-order / visibility / add / remove change -> full repaint */
    if (!full) {
      int focus = topmost_visible();
      int focus_id = focus >= 0 ? cur[focus].id : -1;
      int zvis = (focus_id != prev_focus);
      if (nwin != npf)
        zvis = 1; /* window added or removed */
      for (int i = 0; i < nwin && !zvis; i++)
        if (pf[i].id != cur[i].id || pf[i].vis != win_visible(&cur[i]) || pf[i].min != win_minimized(&cur[i]))
          zvis = 1;
      if (zvis)
        full = true;
      prev_focus = focus_id;

      if (at_active != prev_at_active || at_sel != prev_at_sel)
        full = true;
      prev_at_active = at_active;
      prev_at_sel = at_sel;
      if (tb_menu_open != prev_tb_menu)
        full = true;
      prev_tb_menu = tb_menu_open;

      /* moved/resized windows: old ∪ new as one region */
      for (int i = 0; i < nwin; i++) {
        const struct pfsnap *p = &pf[i];
        if ((p->x != cur[i].x || p->y != cur[i].y || p->w != cur[i].w || p->h != cur[i].h))
          damage_add_union(p->x, p->y, p->w, p->h, cur[i].x, cur[i].y, cur[i].w, cur[i].h);
      }
      /* cursor moved/changed/animating */
      int cur_kind = pick_cursor(mx, my);
      if (!hw_cursor && (mx != prev_mx || my != prev_my || cur_kind != prev_cur || cur_kind == CUR_BUSY)) {
        damage_add(prev_mx - CUR_DMG_PAD, prev_my - CUR_DMG_PAD, CUR_DMG_W, CUR_DMG_H);
        damage_add(mx - CUR_DMG_PAD, my - CUR_DMG_PAD, CUR_DMG_W, CUR_DMG_H);
      }
      prev_cur = cur_kind;
      /* content changed: only the client rect the app reported, or the whole frame */
      for (int i = 0; i < nwin; i++) {
        const struct winrec *w = &cur[i];
        if (!win_visible(w) || pf[i].seq == w->present_seq)
          continue;
        if (w->dirty_w < 0) {
          damage_add(w->x, w->y, w->w, w->h);
        } else if (w->dirty_w > 0 && w->dirty_h > 0) {
          int cx, cy;
          client_rect(w, &cx, &cy, 0, 0);
          damage_add(cx + w->dirty_x, cy + w->dirty_y, w->dirty_w, w->dirty_h);
        }
      }
      /* clock tick */
      int sec = (int)(frame_clock & 0xFF);
      if (sec != last_sec)
        damage_add(0, screen_h - WM_BAR_H, screen_w, WM_BAR_H);
      if (osd)
        damage_add(osd_x(), osd_y(), OSD_W, OSD_H);
      /* Start menu toggle */
      if (start_open != prev_start)
        damage_add(0, sm_top(), SM_W, sm_h() + WM_BAR_H);
      if (pw_open != prev_pw_open || (pw_open && pw_sel != prev_pw_sel))
        damage_add(pw_x(), pw_y(), PW_W, pw_h());
    }

    if (full) {
      clip_full();
      compose(mx, my);
      struct fbrect all = {0, 0, screen_w, screen_h};
      present(&all, 1);
    } else if (ndmg > 0) {
      struct fbrect rects[MAXDMG];
      int n = 0;
      for (int k = 0; k < ndmg; k++) {
        set_clip(dmg[k].x, dmg[k].y, dmg[k].w, dmg[k].h);
        if (clip_x1 <= clip_x0 || clip_y1 <= clip_y0)
          continue;
        compose(mx, my);
        rects[n++] = (struct fbrect){clip_x0, clip_y0, clip_x1 - clip_x0, clip_y1 - clip_y0};
      }
      clip_full();
      if (n)
        present(rects, n);
    }

    /* snapshot for next frame */
    npf = 0;
    for (int i = 0; i < nwin && npf < WM_LIST_MAX; i++) {
      pf[npf].id = cur[i].id;
      pf[npf].x = cur[i].x;
      pf[npf].y = cur[i].y;
      pf[npf].w = cur[i].w;
      pf[npf].h = cur[i].h;
      pf[npf].vis = win_visible(&cur[i]);
      pf[npf].min = win_minimized(&cur[i]);
      pf[npf].seq = cur[i].present_seq;
      npf++;
    }
    prev_mx = mx;
    prev_my = my;
    prev_start = start_open;
    prev_pw_open = pw_open;
    prev_pw_sel = pw_sel;
    last_sec = (int)(frame_clock & 0xFF);
    inited = true;
    cursor_frame++;

    int idle =
        in.nkeys == 0 && in.buttons == 0 && dz == 0 && !full && ndmg == 0 && pend_empty() && paste_pos >= paste_len;
    frame_pace(idle ? 40 : active_period()); /* target_hz while active; ~25 fps when static */
  }
}
