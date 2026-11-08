/* keys.c: keyboard routing, Print Screen and clipboard paste */
#include "comp.h"

/* global key routing (Super -> Start menu; Alt+Tab -> switcher; else the focused window) */

/* Clipboard-history viewer (Super+V): launch /bin/cliphist */
static void paste_begin(void);
static int cliphist_pid;
static void open_cliphist(void)
{
  if (cliphist_pid > 0)
    return; /* one at a time */
  cliphist_pid = spawn("/bin/cliphist", 0);
  /* Reaped exclusively by cliphist_reap, so it is deliberately NOT added to kids[] */
}
void cliphist_reap(void)
{
  if (cliphist_pid <= 0)
    return;
  int st;
  if (waitpid_nb(cliphist_pid, &st)) {
    int picked = (st == 0);
    cliphist_pid = 0;
    if (picked)
      paste_begin(); /* paste the promoted clip into focus */
  }
}

static struct {
  int c, mods;
} pending[PENDING_MAX];
static int pend_head, pend_tail;
bool pend_empty(void) { return pend_head == pend_tail; }
static void pend_push(int c, int mods)
{
  int next = (pend_head + 1) % PENDING_MAX;
  if (next == pend_tail)
    return; /* full: drop (paste larger than 255 backlog) */
  pending[pend_head].c = c;
  pending[pend_head].mods = mods;
  pend_head = next;
}

/* Print Screen */
static unsigned *shot_pixels; /* tightly-packed w*h capture (grown on demand) */
static unsigned shot_cap;     /* capacity of shot_pixels, in pixels */
static int shot_tid;          /* encode worker, 0 = idle */
static void *shot_stack;      /* worker stack (allocated once, reused) */
static char shot_path[64];
static unsigned shot_w, shot_h;

static void shot_worker(void)
{
  png_write(shot_path, shot_pixels, shot_w, shot_h, shot_w);
  exit(0);
}

static void take_screenshot(void)
{
  if (shot_tid) { /* previous encode still running? */
    int st;
    if (!waitpid_nb(shot_tid, &st))
      return; /* busy: drop this shot */
    shot_tid = 0;
  }
  unsigned w = screen_w, h = screen_h;
  if (!w || !h || !back)
    return;

  if (shot_cap < w * h) { /* (re)size the capture buffer */
    free(shot_pixels);
    shot_pixels = malloc(w * h * 4);
    shot_cap = shot_pixels ? w * h : 0;
  }
  if (!shot_pixels)
    return;
  for (unsigned y = 0; y < h; y++) /* fast capture: strip the fb stride */
    memcpy(shot_pixels + y * w, back + y * fb_pitch, w * 4);
  shot_w = w;
  shot_h = h;

  /* First free /home/root/screenshotN.png */
  struct statinfo si;
  for (unsigned idx = 0; idx <= 9999; idx++) {
    int p = 0;
    for (const char *s = "/home/root/screenshot"; *s; s++)
      shot_path[p++] = *s;
    char t[6];
    int n = 0;
    unsigned v = idx;
    if (!v)
      t[n++] = '0';
    while (v) {
      t[n++] = (char)('0' + v % 10);
      v /= 10;
    }
    while (n)
      shot_path[p++] = t[--n];
    for (const char *s = ".png"; *s; s++)
      shot_path[p++] = *s;
    shot_path[p] = '\0';
    if (stat(shot_path, &si) != 0)
      break; /* name is free */
    if (idx == 9999)
      return;
  }

  if (!shot_stack)
    shot_stack = thread_stack_alloc(32 * 1024);
  if (!shot_stack)
    return;
  shot_tid = thread_create(shot_worker, shot_stack);
}

static void key_to_focus(int c, int mods)
{
  int t = topmost_visible();
  if (t < 0)
    return;
  /* Flow control: if the focused window's event ring is full */
  if (win_key_space(cur[t].id) <= 0) {
    pend_push(c, mods);
    return;
  }
  struct winevent ev = {WEV_KEY, c, mods};
  win_deliver(cur[t].id, &ev);
}

static void close_focused(void)
{
  int t = topmost_visible();
  if (t < 0)
    return;
  int mb = blocking_modal(t);
  struct winevent ev = {WEV_CLOSE, 0, 0};
  win_deliver(mb >= 0 ? mb : cur[t].id, &ev);
}

static bool title_is(const char *a, const char *b)
{
  while (*a && *a == *b)
    a++, b++;
  return *a == *b;
}

/* cur[] slot of an app's window: spawned by us, or (player) known by title */
static int app_window(int app)
{
  if (app < 0)
    return -1;
  for (int i = nwin - 1; i >= 0; i--) {
    if (app_pid[app] > 0 && cur[i].owner_pid == app_pid[app])
      return i;
    if (app == app_player && (title_is(cur[i].title, "Media Player") || title_is(cur[i].title, "Video Player")))
      return i;
  }
  return -1;
}

static void app_raise_or_launch(int app)
{
  int s = app_window(app);
  if (s >= 0)
    raise_window(s);
  else
    launch_app(app);
}

/* Media transport goes to the active media session, else to the player even when it is not focused */
static void media_key(int c)
{
  int cmd = c == KEY_MEDIA_PLAY   ? MEDIA_CMD_TOGGLE
            : c == KEY_MEDIA_STOP ? MEDIA_CMD_STOP
            : c == KEY_MEDIA_NEXT ? MEDIA_CMD_NEXT
                                  : MEDIA_CMD_PREV;
  if (mediactl_send(cmd, 0) == 0)
    return;
  int s = app_window(app_player);
  if (s < 0) {
    if (c == KEY_MEDIA_PLAY)
      launch_app(app_player);
    return;
  }
  if (win_key_space(cur[s].id) <= 0)
    return;
  struct winevent ev = {WEV_KEY, c, 0};
  win_deliver(cur[s].id, &ev);
}

/* Global special keys; true when consumed (apps never see them) */
static bool special_key(int c)
{
  switch (c) {
  case KEY_VOL_UP:
  case KEY_VOL_DOWN:
  case KEY_MUTE:
    osd_show(OSD_VOLUME);
    return true;
  case KEY_BRIGHT_UP:
  case KEY_BRIGHT_DOWN:
    osd_show(OSD_BRIGHTNESS);
    return true;
  case KEY_MIC_MUTE:    /* no mic state is exposed: swallowed */
  case KEY_LAUNCH_MAIL: /* no mail app */
    return true;
  case KEY_MEDIA_PLAY:
  case KEY_MEDIA_STOP:
  case KEY_MEDIA_NEXT:
  case KEY_MEDIA_PREV:
    media_key(c);
    return true;
  case KEY_LAUNCH_CALC:
    app_raise_or_launch(app_find("calc"));
    return true;
  case KEY_LAUNCH_BROWSER:
    app_raise_or_launch(app_find("browser"));
    return true;
  case KEY_LAUNCH_FILES:
    app_raise_or_launch(app_find("files"));
    return true;
  case KEY_SEARCH:
    start_open = true;
    return true;
  case KEY_POWER:
    pw_show();
    return true;
  case KEY_SLEEP:
    if (can_sleep)
      power_run(PA_SLEEP);
    else
      pw_show();
    return true;
  case KEY_WWW_BACK:
    key_to_focus(KEY_LEFT, KMOD_ALT);
    return true;
  case KEY_WWW_FORWARD:
    key_to_focus(KEY_RIGHT, KMOD_ALT);
    return true;
  case KEY_WWW_REFRESH:
    key_to_focus(KEY_F5, 0);
    return true;
  }
  return false;
}

void feed_key(int c, int mods)
{
  if (pw_open) {
    pw_key(c);
    return;
  }
  if (special_key(c))
    return;
  if (c == KEY_F4 && (mods & KMOD_ALT)) {
    close_focused();
    return;
  }
  if (c == KEY_PRTSC) {
    take_screenshot();
    return;
  }
  if (c == 'v' && (mods & KMOD_SUPER)) {
    open_cliphist();
    return;
  } /* Super+V */
  if (c == 'v' && (mods & KMOD_CTRL)) {
    paste_begin();
    return;
  } /* Ctrl+V */
  if (c == KEY_SUPER) {
    start_open = !start_open;
    return;
  }
  if (c == KEY_TAB && (mods & KMOD_ALT)) {
    if (!at_active) {
      at_count = alttab_build();
      if (at_count <= 1) {
        at_count = 0;
        return;
      }
      at_active = true;
      at_sel = 0;
    }
    if (mods & KMOD_SHIFT)
      at_sel = (at_sel - 1 + at_count) % at_count;
    else
      at_sel = (at_sel + 1) % at_count;
    return;
  }
  /* We don't get an "Alt released" event from input_poll */
  if (at_active && !(mods & KMOD_ALT)) {
    int s = slot_of(at_order[at_sel]);
    at_active = false;
    if (s >= 0)
      raise_window(s);
    return;
  }
  key_to_focus(c, mods);
}

/* Drain parked keys before pulling fresh input; stop as soon as the target fills again */
void pending_flush(void)
{
  while (!pend_empty()) {
    int c = pending[pend_tail].c, mods = pending[pend_tail].mods;
    int t = topmost_visible();
    if (t < 0) {
      pend_tail = pend_head;
      return;
    } /* nothing focused: discard */
    if (win_key_space(cur[t].id) <= 0)
      return; /* still full: try next frame */
    pend_tail = (pend_tail + 1) % PENDING_MAX;
    struct winevent ev = {WEV_KEY, c, mods};
    win_deliver(cur[t].id, &ev);
  }
}

static char paste_buf[PASTE_MAX];
int paste_len, paste_pos;

static void paste_begin(void)
{
  if (paste_pos < paste_len)
    return; /* a paste is still draining */
  char mime[32];
  void *data = 0;
  int len = 0;
  if (clip_get(0, mime, sizeof mime, &data, &len) < 0)
    return;
  int out = 0;
  if (mime[0] == 't' && mime[1] == 'e' && mime[2] == 'x' && mime[3] == 't' && mime[4] == '/') {
    const char *raw = data;
    for (int i = 0; i < len && out < PASTE_MAX; i++) {
      char c = raw[i];
      if (c == '\r') { /* fold CR and CRLF down to one LF */
        if (i + 1 < len && raw[i + 1] == '\n')
          i++;
        c = '\n';
      }
      if (c != '\n' && c != '\t' && (unsigned char)c < 0x20)
        continue; /* drop stray control bytes */
      paste_buf[out++] = c;
    }
  }
  free(data);
  paste_pos = 0;
  paste_len = out; /* publish last */
}

/* Drain the pending paste into the focused window */
void paste_flush(void)
{
  while (paste_pos < paste_len) {
    int t = topmost_visible();
    if (t < 0) {
      paste_pos = paste_len;
      return;
    } /* nothing focused: discard */
    if (win_key_space(cur[t].id) <= 0)
      return; /* full: try next frame */
    struct winevent ev = {WEV_KEY, (unsigned char)paste_buf[paste_pos++], 0};
    win_deliver(cur[t].id, &ev);
  }
}
