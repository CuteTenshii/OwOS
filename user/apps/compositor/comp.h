/* comp.h: shared state and entry points of the compositor modules */
#ifndef OWOS_COMP_H
#define OWOS_COMP_H

#include "clipboard.h"
#include "desktop.h"
#include "gfx.h"
#include "mediactl.h"
#include "owos.h"
#include "pngenc.h"

#include <stdbool.h>

#define WM_TITLE_H 22
#define WM_BORDER 2
#define WM_BAR_H 28
#define WM_TITLE_MAX 64
#define WM_GRIP 14
#define WM_EDGE 5
#define WM_MIN_W 140
#define WM_MIN_H 120
enum {
  CUR_ARROW = 0,
  CUR_HAND,
  CUR_TEXT,
  CUR_BUSY,
  CUR_RESIZE_NWSE,
  CUR_RESIZE_NESW,
  CUR_RESIZE_NS,
  CUR_RESIZE_EW,
  CUR_MOVE,
  CUR_NOT_ALLOWED,
  CUR_CROSSHAIR,
  CUR_HELP,
  CUR_GRAB,
  CUR_GRABBING,
  CUR_PROGRESS
};
#define KEY_TAB '\t'
#define KEY_ALT_UP 0x1B /* internal: "Alt released" marker (kernel wm KEY_ALT_UP) */
#define WM_LIST_MAX 64
#define WM_TITLE_DISP (WM_TITLE_MAX + 18)
#define START_W 70
#define SM_ITEM_H 20
#define SM_W 150
#define SM_SEP_H 9
#define TBM_W 120
#define TBM_ITEM_H 22
#define TBM_N 2
#define TB_BTN_W 14
#define TB_BTN_GAP 3
#define TB_WIN_GAP 4
#define APPS_DIR "/usr/share/applications"
enum { PA_SLEEP, PA_RESTART, PA_SHUTDOWN };
#define PW_W 240
#define PW_TITLE_H 40
#define PW_ROW_H 32
#define PW_PAD 8
#define PW_ROWS 4
#define PW_CANCEL 3
#define MAXKIDS 64
#define ICON_PX 16
#define MAXDMG 16
struct rect {
  int x, y, w, h;
};
struct cur_shape {
  const char *const *rows;
  int nrows, hotx, hoty;
};
#define CUR_DMG_PAD 9
#define CUR_DMG_W 32
#define CUR_DMG_H 34
struct taskbar_cache {
  bool valid;
  int right, n;
  int idxs[WM_LIST_MAX], bxs[WM_LIST_MAX], bws[WM_LIST_MAX];
};
struct clientmap {
  int id;
  bool valid;
  struct winmap m;
};
#define AT_ROW_H 24
#define AT_PAD 12
#define AT_PANEL_W 340
struct crashwin {
  bool used;
  int x, y, w, h;
  struct crash_rec rec;
};
#define MAXCRASHWIN 4
enum { RZ_L = 1, RZ_R = 2, RZ_T = 4, RZ_B = 8 };
#define OSD_W 220
#define OSD_H 44
#define OSD_MS 1500
#define OSD_BG 0x141a24
#define OSD_FG 0xe6ebf2
#define OSD_DIM 0x3a4458
#define OSD_ACCENT 0x5aa0ff
enum { OSD_NONE, OSD_VOLUME, OSD_BRIGHTNESS };
#define PW_BG 0x141a24
#define PW_FG 0xe6ebf2
#define PW_OFF 0x5a6578
#define PW_SEL 0x2f6fb0
#define DBLCLICK_MS 400
#define DBLCLICK_SLOP 6
struct maxrec {
  int id;
  bool used;
  int sx, sy, sw, sh;
};
#define PENDING_MAX 256
#define PASTE_MAX 8192
#define CONF_DISPLAY "/etc/display.conf"
#define CONF_INPUT "/etc/input.conf"
#define CONF_SOUND "/etc/sound.conf"
#define DISPLAY_RATE "/sys/class/graphics/fb0/rate"
struct tunable {
  const char *file;
  const char *key;
  const char *node;
  char cur[64];
  bool known; /* cur holds a real value */
};
#define NTUN ((int)(sizeof tun / sizeof tun[0]))
#define NCONF ((int)(sizeof conf_files / sizeof conf_files[0]))
struct pfsnap {
  int id, x, y, w, h;
  bool vis, min;
  unsigned seq;
};

/* screen.c */
extern unsigned *back;
extern int screen_w, screen_h;
extern int fb_pitch;
extern int hw_w, hw_h, hw_pitch;
extern int ui_scale;
extern bool scale_stale;
int work_bottom(void);
void clamp_frame_position(int *x, int *y, int w, int h);
void fit_frame_to_work_area(int *x, int *y, int *w, int *h);
extern int clip_x0, clip_y0, clip_x1, clip_y1;
void set_clip(int x, int y, int w, int h);
void clip_full(void);
int scale_now(void);
int bind_mode(void);
void present(const struct fbrect *r, int n);

/* wm.c */
extern struct winrec cur[WM_LIST_MAX];
extern int nwin;
bool win_visible(const struct winrec *w);
bool win_minimized(const struct winrec *w);
int slot_of(int id);
extern bool taskbar_layout_dirty;
void z_lower(int id);
int parent_id_of(int id);
int root_id_of(int id);
void zorder_apply(void);
void put2(char *p, int v);
void disp_title(const struct winrec *w, char *buf);
int topmost_visible(void);
int window_at(int px, int py);
void raise_family_id(int id);
void raise_window(int slot);
int blocking_modal(int idx);
extern int flash_id, flash_frames;
void flash_modal(int id);
extern bool at_active;
extern int at_order[WM_LIST_MAX];
extern int at_count;
extern int at_sel;
int alttab_build(void);
bool btn_present(const struct winrec *w, int kind);
int titlebtn_box(const struct winrec *w, int kind, int *bx, int *by);
int titlebtn_hit(const struct winrec *w, int mx, int my);
extern struct taskbar_cache tb_cache;
void taskbar_layout_cached(int right);
int taskbar_hit(int mx, int my);
extern bool hover_resizing;
extern int rsz_edge_hint;
int resize_hit(const struct winrec *w, int mx, int my);
int pick_cursor(int mx, int my);
extern struct maxrec maxed[WM_LIST_MAX];
int is_maxed(int id);
void maxed_forget_gone(void);
void toggle_maximize(int slot);

/* menus.c */
extern bool start_open;
extern bool tb_menu_open;
extern int tb_menu_win;
extern int tb_menu_x, tb_menu_y;
int tbm_h(void);
extern int tb_band_right;
extern unsigned frame_clock;
int tb_band_left(void);
extern struct desktop_list apps;
extern int *app_pid;
extern int app_player;
int apps_menu_count(void);
int app_find(const char *id);
void apps_load(void);
extern const char *const power_label[];
extern bool can_sleep, resumed;
void power_run(int action);
int sm_power_n(void);
int sm_power_action(int row);
int sm_apps_h(void);
int sm_h(void);
int sm_top(void);
extern bool pw_open;
extern int pw_sel;
int pw_x(void);
int pw_h(void);
int pw_y(void);
bool pw_enabled(int row);
bool pw_inside(int mx, int my);
int pw_row_at(int mx, int my);
extern bool force_full;
extern int kids[MAXKIDS];
void kid_track(int pid);
void reap_kids(void);
void launch_app(int i);

/* cursor.c */
extern unsigned cursor_frame;
void draw_cursor(int kind, int mx, int my);
extern bool hw_cursor;
bool cursor_hw_update(int kind); /* redefine the hardware cursor if kind/animation/scale changed */

/* draw.c */
void refresh_maps(void);
void client_rect(const struct winrec *w, int *x, int *y, int *width, int *height);
extern struct crashwin crashwins[MAXCRASHWIN];
void crash_ingest(void);
void compose(int mx, int my);

/* osd.c */
extern int osd_kind, osd_level;
int osd_x(void);
int osd_y(void);
void osd_show(int kind);
bool osd_tick(void);
void draw_osd(void);
void draw_power(void);
void pw_show(void);
void pw_activate(int row);
void pw_key(int c);

/* input.c */
extern bool prev_down, prev_rdown;
extern int prev_wheel;
void handle_input(int mx, int my, int buttons, int dz);

/* keys.c */
void cliphist_reap(void);
bool pend_empty(void);
void feed_key(int c, int mods);
void pending_flush(void);
extern int paste_len, paste_pos;
void paste_flush(void);

/* conf.c */
int scan_uint(const char *p);
int node_read(const char *path, char *out, int max);
void conf_load(void);
void conf_save(void);
void tun_restore(void);
bool tun_poll(void);
int rate_now(void);

/* damage.c */
extern struct rect dmg[MAXDMG];
extern int ndmg;
void damage_add(int x, int y, int w, int h);
void damage_add_union(int x0, int y0, int w0, int h0, int x1, int y1, int w1, int h1);

/* icon.c */
const unsigned *icon_lookup(const char *name);
const unsigned *app_icon(int app);
const unsigned *window_icon(const struct winrec *w);

/* tray.c */
extern volatile unsigned tray_gen;
void tray_start(void);
void tray_prune(void);
int tray_draw(int right, int y);
bool tray_click(int mx, int my, bool right);

#endif /* OWOS_COMP_H */
