/* menus.c: start menu, power actions and child processes */
#include "comp.h"

/* taskbar / menus */
bool start_open;

bool tb_menu_open;
int tb_menu_win = -1; /* targeted cur[] id */
int tb_menu_x, tb_menu_y;
int tbm_h(void) { return TBM_N * TBM_ITEM_H; }

int tb_band_right;
unsigned frame_clock;
int tb_band_left(void) { return START_W + 6; }

/* Start menu apps: the .desktop entries under APPS_DIR */
struct desktop_list apps;
int *app_pid;
int app_player = -1;
int apps_menu_count(void) { return apps.n; }

int app_find(const char *id)
{
  for (int i = 0; i < apps.n; i++)
    if (streq(apps.v[i]->id, id))
      return i;
  return -1;
}

void apps_load(void)
{
  struct desktop_list all = {0};
  if (desktop_scan(APPS_DIR, &all) < 0)
    return;
  desktop_sort(&all);
  for (int i = 0; i < all.n; i++) {
    if (desktop_visible(all.v[i]))
      all.v[apps.n++] = all.v[i];
    else
      desktop_free(all.v[i]);
  }
  all.n = apps.n;
  apps = all;
  app_pid = calloc((unsigned)apps.n + 1, sizeof *app_pid);
  app_player = app_find("player");
}

/* Power actions */
const char *const power_label[] = {"Sleep", "Restart", "Shut Down"};
bool can_sleep, resumed;

void power_run(int action)
{
  if (action == PA_SLEEP) {
    power(2);
    resumed = true; /* main loop resyncs input and pacing */
  } else {
    power(action == PA_RESTART ? 1 : 0);
  }
}

int sm_power_n(void) { return can_sleep ? 3 : 2; }
int sm_power_action(int row) { return can_sleep ? row : row + 1; }

int sm_apps_h(void) { return apps_menu_count() * SM_ITEM_H; }
int sm_h(void) { return sm_apps_h() + SM_SEP_H + sm_power_n() * SM_ITEM_H; }
int sm_top(void) { return screen_h - WM_BAR_H - sm_h(); }

/* Power overlay: Sleep, Restart, Shut Down, Cancel */
bool pw_open;
int pw_sel;
int pw_x(void) { return (screen_w - PW_W) / 2; }
int pw_h(void) { return PW_TITLE_H + PW_ROWS * PW_ROW_H + PW_PAD; }
int pw_y(void) { return (screen_h - pw_h()) / 2; }
bool pw_enabled(int row) { return row != PA_SLEEP || can_sleep; }
bool pw_inside(int mx, int my) { return mx >= pw_x() && mx < pw_x() + PW_W && my >= pw_y() && my < pw_y() + pw_h(); }
int pw_row_at(int mx, int my)
{
  int rel = my - pw_y() - PW_TITLE_H;
  if (mx < pw_x() + PW_PAD || mx >= pw_x() + PW_W - PW_PAD || rel < 0 || rel >= PW_ROWS * PW_ROW_H)
    return -1;
  return rel / PW_ROW_H;
}

bool force_full;

/* child reaping */
int kids[MAXKIDS];
static int nkids;

void kid_track(int pid)
{
  if (pid > 0 && nkids < MAXKIDS)
    kids[nkids++] = pid;
}

/* Reap exited children non-blocking so zombies don't accumulate */
void reap_kids(void)
{
  int st;
  for (int i = 0; i < nkids;) {
    if (waitpid_nb(kids[i], &st))
      kids[i] = kids[--nkids];
    else
      i++;
  }
}

void launch_app(int i)
{
  if (i < 0 || i >= apps_menu_count())
    return;
  char **argv;
  int argc;
  if (desktop_exec_argv(apps.v[i], NULL, NULL, 0, &argv, &argc) != 0 || argc < 1)
    return;
  char path[64];
  snprintf(path, sizeof path, argv[0][0] == '/' ? "%s" : "/bin/%s", argv[0]);
  int pid = spawn(path, argc > 1 ? argv[1] : 0);
  app_pid[i] = pid;
  kid_track(pid);
  desktop_argv_free(argv, argc);
}
