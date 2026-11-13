/* updates.c, the Updates page: asks GitHub for the latest release tag */
#include "http.h"
#include "settings.h"

#define RELEASES_URL "https://api.github.com/repos/CuteTenshii/OwOS/releases/latest"
#define REQ_HEADERS "User-Agent: owos-settings/1.0\r\nAccept: application/vnd.github+json\r\n"
#define FETCH_STACK (128 * 1024) /* TLS needs more than a default thread stack */
#define UPD_BTN_W 160
#define UPD_BTN_Y 104

enum { UPD_IDLE, UPD_CHECKING, UPD_DONE, UPD_FAILED };
static int upd_state = UPD_IDLE;
static int upd_tid;
static void *upd_stack_top;
static volatile bool upd_finished; /* set by the fetch thread */
static char upd_buf[8192];         /* head of the API reply; tag_name sits near the top */
static int upd_len;
static char upd_latest[32]; /* tag of the newest release */

/* Parse "v1.2.3" (or "1.2", trailing text ignored) into v[3]; false if no leading number */
static bool parse_ver(const char *s, int v[3])
{
  if (*s == 'v')
    s++;
  if (*s < '0' || *s > '9')
    return false;
  for (int i = 0; i < 3; i++) {
    v[i] = 0;
    while (*s >= '0' && *s <= '9')
      v[i] = v[i] * 10 + (*s++ - '0');
    if (*s == '.')
      s++;
  }
  return true;
}

static bool upd_newer(void)
{
  int cur[3], lat[3];
  if (!parse_ver(OWOS_VERSION, cur) || !parse_ver(upd_latest, lat))
    return false;
  for (int i = 0; i < 3; i++)
    if (lat[i] != cur[i])
      return lat[i] > cur[i];
  return false;
}

static int upd_sink(void *ctx, const unsigned char *buf, int len)
{
  (void)ctx;
  for (int i = 0; i < len && upd_len < (int)sizeof upd_buf - 1; i++)
    upd_buf[upd_len++] = (char)buf[i];
  return upd_len >= (int)sizeof upd_buf - 1; /* enough: stop the transfer */
}

static void upd_fetch(void)
{
  http_request_ex("GET", RELEASES_URL, 0, 0, REQ_HEADERS, 0, 0, 0, upd_sink, 0);
  upd_finished = true;
  exit(0);
}

static void upd_start(void)
{
  upd_len = 0;
  upd_finished = false;
  upd_stack_top = thread_stack_alloc(FETCH_STACK);
  upd_tid = upd_stack_top ? thread_create(upd_fetch, upd_stack_top) : -1;
  if (upd_tid < 0) {
    if (upd_stack_top)
      thread_stack_free(upd_stack_top);
    upd_state = UPD_FAILED;
  } else {
    upd_state = UPD_CHECKING;
  }
  seq++;
}

/* Once the fetch thread ends, pull tag_name out of the JSON */
void updates_poll(void)
{
  if (upd_state != UPD_CHECKING || !upd_finished)
    return;
  thread_join(upd_tid);
  thread_stack_free(upd_stack_top);
  upd_buf[upd_len] = 0;

  static const char key[] = "\"tag_name\"";
  const char *p = upd_buf;
  for (; *p; p++) {
    int k = 0;
    while (key[k] && p[k] == key[k])
      k++;
    if (!key[k])
      break;
  }
  if (!*p)
    p = 0;
  upd_state = UPD_FAILED;
  if (p && (p = strchr(p + sizeof key - 1, ':')) && (p = strchr(p, '"'))) {
    int i = 0;
    for (p++; *p && *p != '"' && i < (int)sizeof upd_latest - 1; p++)
      upd_latest[i++] = *p;
    upd_latest[i] = 0;
    if (i)
      upd_state = UPD_DONE;
  }
  seq++;
}

void updates_click(int cx, int mx, int my)
{
  if (upd_state != UPD_CHECKING && button_hit(cx + 16, UPD_BTN_Y, UPD_BTN_W, GUI_BTN_H, mx, my, NULL))
    upd_start();
}

void updates_draw(int cx, int cw)
{
  int maxw = cw - 32;
  char line[96] = "Installed version: ";
  strcat(line, OWOS_VERSION);
  gfx_ui_text(cx + 16, CONTROLS_Y0, "Software updates", FG_WHITE);
  gfx_ui_text_clip(cx + 16, CONTROLS_Y0 + 28, line, FG_TEXT, maxw);

  const char *msg = "";
  unsigned fg = FG_DIM;
  if (upd_state == UPD_CHECKING)
    msg = "Checking GitHub...";
  else if (upd_state == UPD_FAILED)
    msg = "Could not reach GitHub.";
  else if (upd_state == UPD_DONE) {
    strcpy(line, upd_newer() ? "Update available: " : "Latest release: ");
    strcat(line, upd_latest);
    if (!upd_newer())
      strcat(line, " (up to date)");
    msg = line;
    fg = FG_TEXT;
  }
  gfx_ui_text_clip(cx + 16, CONTROLS_Y0 + 50, msg, fg, maxw);
  button_draw(cx + 16, UPD_BTN_Y, UPD_BTN_W, GUI_BTN_H, "Check for updates",
              upd_state == UPD_CHECKING ? BTN_DISABLED : BTN_NORMAL, NULL);
}
