/* input.c: pointer input, dragging and resizing */
#include "comp.h"

/* input */
static void deliver_client(int slot, unsigned type, int mx, int my)
{
  struct winrec *w = &cur[slot];
  int cx, cy;
  client_rect(w, &cx, &cy, 0, 0);
  struct winevent ev = {type, mx - cx, my - cy};
  win_deliver(w->id, &ev);
}

/* Live-drag / resize state */
static int drag_id = -1, resize_id = -1;
static int drag_dx, drag_dy, drag_mx0, drag_my0;
static int rsz_edge;
static int rsz_ox, rsz_oy, rsz_ow, rsz_oh, rsz_mx0, rsz_my0;
static int app_drag_id = -1;


bool prev_down, prev_rdown;
int prev_wheel;

void handle_input(int mx, int my, int buttons, int dz)
{
  static int dbl_id = -1, dbl_x, dbl_y; /* last title-bar press, for double-click */
  static unsigned dbl_ms;
  bool down = buttons & 1;
  bool rdown = buttons & 2;

  if (pw_open) {
    static int last_mx, last_my;
    int row = pw_row_at(mx, my);
    if (down && !prev_down) {
      if (row >= 0 && pw_enabled(row))
        pw_activate(row);
      else if (!pw_inside(mx, my))
        pw_open = false;
    } else if ((mx != last_mx || my != last_my) && row >= 0 && pw_enabled(row)) {
      pw_sel = row;
    }
    last_mx = mx;
    last_my = my;
    prev_down = down;
    prev_rdown = rdown;
    return;
  }

  /* Wheel -> focused window under cursor (dropped if a modal blocks it) */
  if (dz) {
    int idx = window_at(mx, my);
    if (idx >= 0 && blocking_modal(idx) < 0) {
      struct winevent ev = {WEV_SCROLL, dz, 0};
      win_deliver(cur[idx].id, &ev);
    }
  }

  /* Right-press */
  if (rdown && !prev_rdown) {
    int bar_y = screen_h - WM_BAR_H;
    int tbw = (my >= bar_y) ? taskbar_hit(mx, my) : -1;
    if (tbw >= 0) {
      tb_menu_open = true;
      tb_menu_win = cur[tbw].id;
      start_open = false;
      tb_menu_x = mx;
      if (tb_menu_x + TBM_W > screen_w)
        tb_menu_x = screen_w - TBM_W;
      if (tb_menu_x < 0)
        tb_menu_x = 0;
      tb_menu_y = bar_y - tbm_h();
    } else {
      tb_menu_open = false;
      int idx = window_at(mx, my);
      if (idx >= 0) {
        int mb = blocking_modal(idx);
        if (mb >= 0) {
          raise_family_id(mb);
          flash_modal(mb);
        } else {
          raise_window(idx);
          if (my >= cur[idx].y + WM_TITLE_H)
            deliver_client(idx, WEV_RCLICK, mx, my);
        }
      }
    }
  }
  prev_rdown = rdown;

  if (down && !prev_down) { /* left press */
                            /* crash windows sit above everything but the menus: close box / dismiss */
    for (int i = MAXCRASHWIN - 1; i >= 0; i--) {
      struct crashwin *c = &crashwins[i];
      if (!c->used)
        continue;
      if (mx >= c->x && mx < c->x + c->w && my >= c->y && my < c->y + c->h) {
        int bx = c->x + c->w - 4 - TB_BTN_W, by = c->y + 4;
        if (mx >= bx && mx < bx + TB_BTN_W && my >= by && my < by + TB_BTN_W) {
          c->used = false;
          force_full = true;
        }
        prev_down = down;
        return; /* clicks over a crash panel stop here */
      }
    }
    if (tb_menu_open) {
      int inside = mx >= tb_menu_x && mx < tb_menu_x + TBM_W && my >= tb_menu_y && my < tb_menu_y + tbm_h();
      if (inside) {
        int item = (my - tb_menu_y) / TBM_ITEM_H;
        int id = tb_menu_win;
        tb_menu_open = false;
        int s = slot_of(id);
        if (s >= 0) {
          if (item == 0) { /* Close: ask the app */
            struct winevent ev = {WEV_CLOSE, 0, 0};
            win_deliver(id, &ev);
          } else { /* Force Kill */
            if (cur[s].owner_pid > 0)
              kill(cur[s].owner_pid, SIGKILL);
          }
        }
        prev_down = down;
        return;
      }
      tb_menu_open = false;
    }
    int bar_y = screen_h - WM_BAR_H;
    if (my >= bar_y && mx >= 0 && mx < START_W) { /* Start button */
      start_open = !start_open;
      prev_down = down;
      return;
    }
    int tbw = (my >= bar_y) ? taskbar_hit(mx, my) : -1;
    if (tbw >= 0) {
      start_open = false;
      /* restore/minimize/raise via focus + geom */
      raise_window(tbw);
      prev_down = down;
      return;
    }
    if (start_open) {
      int top = sm_top();
      if (mx >= 0 && mx < SM_W && my >= top && my < bar_y) {
        int rel = my - top;
        if (rel < sm_apps_h()) {
          launch_app(rel / SM_ITEM_H);
          start_open = false;
        } else {
          int prel = rel - sm_apps_h() - SM_SEP_H;
          if (prel >= 0 && prel / SM_ITEM_H < sm_power_n()) {
            start_open = false;
            power_run(sm_power_action(prel / SM_ITEM_H));
          }
        }
        prev_down = down;
        return;
      }
      start_open = false;
    }
    int idx = window_at(mx, my);
    if (idx >= 0) {
      /* A modal dialog holds its family */
      int mb = blocking_modal(idx);
      if (mb >= 0) {
        raise_family_id(mb);
        flash_modal(mb);
        prev_down = down;
        return;
      }
      raise_window(idx);
      struct winrec *w = &cur[idx];
      bool resizable = (w->flags & UWIN_F_RESIZABLE) != 0; /* dialogs opt out */
      int tb = titlebtn_hit(w, mx, my);
      int resize = resize_hit(w, mx, my);
      if (tb == 0) { /* close */
        struct winevent ev = {WEV_CLOSE, 0, 0};
        win_deliver(w->id, &ev);
      } else if (tb == 1) { /* maximize / restore */
        toggle_maximize(idx);
      } else if (tb == 2) { /* minimize: no cross-process minimize primitive yet */
        z_lower(w->id);
      } else if (resizable && is_maxed(w->id) < 0 && resize) {
        int e = resize;
        resize_id = w->id;
        rsz_edge = e;
        rsz_ox = w->x;
        rsz_oy = w->y;
        rsz_ow = w->w;
        rsz_oh = w->h;
        rsz_mx0 = mx;
        rsz_my0 = my;
      } else if (my < w->y + WM_TITLE_H) { /* title bar */
        unsigned t = msec();
        int dx = mx - dbl_x, dy = my - dbl_y;
        if (dx < 0)
          dx = -dx;
        if (dy < 0)
          dy = -dy;
        if (btn_present(w, 1) && dbl_id == w->id && (unsigned)(t - dbl_ms) <= DBLCLICK_MS && dx <= DBLCLICK_SLOP &&
            dy <= DBLCLICK_SLOP) {
          toggle_maximize(idx); /* double-click: [un-]maximize */
          dbl_id = -1;          /* consume, so a triple-click isn't two toggles */
        } else {                /* single click: title-bar drag */
          drag_id = w->id;
          drag_dx = mx - w->x;
          drag_dy = my - w->y;
          drag_mx0 = mx;
          drag_my0 = my;
          dbl_id = w->id;
          dbl_ms = t;
          dbl_x = mx;
          dbl_y = my;
        }
      } else { /* client press */
        deliver_client(idx, WEV_MOUSE, mx, my);
        app_drag_id = w->id;
      }
    }
  } else if (down && app_drag_id >= 0) { /* app-captured drag */
    int s = slot_of(app_drag_id);
    if (s >= 0)
      deliver_client(s, WEV_DRAG, mx, my);
  } else if (down && drag_id >= 0) { /* title drag */
    int s = slot_of(drag_id);
    if (s >= 0) {
      struct winrec *w = &cur[s];
      int rw = w->w, rh = w->h;
      int m = is_maxed(w->id);
      /* Drag off a maximized window */
      if (m >= 0 && (mx - drag_mx0 > DBLCLICK_SLOP || drag_mx0 - mx > DBLCLICK_SLOP || my - drag_my0 > DBLCLICK_SLOP ||
                     drag_my0 - my > DBLCLICK_SLOP)) {
        rw = maxed[m].sw;
        rh = maxed[m].sh;
        maxed[m].used = false;
        drag_dx = (screen_w > 0) ? mx * rw / screen_w : rw / 2;
      }
      int nx = mx - drag_dx, ny = my - drag_dy;
      clamp_frame_position(&nx, &ny, rw, WM_TITLE_H);
      win_set_geom(w->id, nx, ny, rw, rh);
    }
  } else if (down && resize_id >= 0) { /* resize */
    int s = slot_of(resize_id);
    if (s >= 0) {
      int dx = mx - rsz_mx0, dy = my - rsz_my0;
      int nx = rsz_ox, ny = rsz_oy, nw = rsz_ow, nh = rsz_oh;
      if (rsz_edge & RZ_R)
        nw = rsz_ow + dx;
      if (rsz_edge & RZ_B)
        nh = rsz_oh + dy;
      if (rsz_edge & RZ_L) {
        nw = rsz_ow - dx;
        nx = rsz_ox + dx;
      }
      if (rsz_edge & RZ_T) {
        nh = rsz_oh - dy;
        ny = rsz_oy + dy;
      }
      if (nw < WM_MIN_W) {
        if (rsz_edge & RZ_L)
          nx -= WM_MIN_W - nw;
        nw = WM_MIN_W;
      }
      if (nh < WM_MIN_H) {
        if (rsz_edge & RZ_T)
          ny -= WM_MIN_H - nh;
        nh = WM_MIN_H;
      }
      if (nx < 0) {
        nw += nx;
        nx = 0;
      }
      if (ny < 0) {
        nh += ny;
        ny = 0;
      }
      if (nx + nw > screen_w)
        nw = screen_w - nx;
      if (ny + nh > screen_h - WM_BAR_H)
        nh = screen_h - WM_BAR_H - ny;
      win_set_geom(cur[s].id, nx, ny, nw, nh);
    }
  } else if (!down) { /* release */
    drag_id = resize_id = app_drag_id = -1;
  }

  /* Hover: no button held -> WEV_MOVE to the window under the cursor */
  static int hover_id = -1, hover_px = -1, hover_py = -1;
  if (!down && !rdown && drag_id < 0 && resize_id < 0 && app_drag_id < 0 && (mx != hover_px || my != hover_py)) {
    hover_px = mx;
    hover_py = my;
    int idx = window_at(mx, my);
    struct winrec *w = idx >= 0 ? &cur[idx] : 0;
    int cx, cy, cw, ch;
    if (w)
      client_rect(w, &cx, &cy, &cw, &ch);
    bool in_client = w && mx >= cx && mx < cx + cw && my >= cy && my < cy + ch;
    if (in_client && blocking_modal(idx) >= 0)
      in_client = false; /* modal blocks hover */
    if (hover_id >= 0 && (idx < 0 || cur[idx].id != hover_id || !in_client)) {
      struct winevent ev = {WEV_MOVE, -1, -1};
      win_deliver(hover_id, &ev);
      hover_id = -1;
    }
    if (in_client) {
      deliver_client(idx, WEV_MOVE, mx, my);
      hover_id = w->id;
    }
  }

  hover_resizing = (resize_id >= 0);
  rsz_edge_hint = rsz_edge;
  prev_down = down;
}
