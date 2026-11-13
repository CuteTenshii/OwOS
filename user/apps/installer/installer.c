/* installer.c: the Installer app. Pick a disk, confirm, and install OwOS onto it; kernel/osinstall.c does the
 * writing, one bounded step per call, so the window stays live and shows progress. */
#include "dialog.h"
#include "gfx.h"
#include "owos.h"

#define W 560
#define H 380
#define PAD 20
#define HEAD_PX 22
#define LINE_H 20
#define ROW_H 44
#define LIST_Y 112
#define MAX_DISKS 6
#define BTN_W 150
#define BTN_Y (H - PAD - GUI_BTN_H)

/* Settings' panel colours, so the Installer reads as part of the system. */
#define BG 0x12161eu
#define FG_HEAD 0xffffffu
#define FG_TEXT 0xc8d0dcu
#define FG_DIM 0x8aa0b8u
/* The one accent: why a disk can't be used, or why an install could not start. */
#define FG_WARN 0xf0b060u

enum screen { LIST, INSTALLING, DONE, FAILED };
enum action { A_REFRESH, A_CLOSE, A_INSTALL, A_RESTART, A_BACK };

struct button {
  const char *label;
  int x, w;
  enum action act;
  bool enabled;
};

static int win;
static enum screen screen = LIST;
static struct install_disk disks[MAX_DISKS];
static int ndisks, selected = -1;
static bool denied, no_image;
static char notice[112];
static char target[64];
static struct install_progress prog;
static struct button buttons[3];
static int nbuttons;
static struct gfx_focus focus; /* entry 0: the disk list; entry i + 1: buttons[i] */
static unsigned char unfocusable[4];
static int hover_button = -1, hover_row = -1;

static void size_text(uint32_t sectors, char *out, unsigned cap)
{
  uint32_t mib = sectors / 2048;
  if (mib < 10240)
    snprintf(out, cap, "%u MiB", mib);
  else
    snprintf(out, cap, "%u.%u GiB", mib / 1024, mib % 1024 * 10 / 1024);
}

static const char *problem_text(int problem)
{
  static char small[48];
  switch (problem) {
  case INSTALL_TOO_SMALL:
    snprintf(small, sizeof small, "Too small: OwOS needs %u MiB.", INSTALL_MIN_SECTORS / 2048 + 1);
    return small;
  case INSTALL_READ_ONLY:
    return "This disk can't be written to.";
  case INSTALL_LIVE_MEDIUM:
    return "Holds an ISO image, likely the one OwOS started from.";
  case INSTALL_NO_IMAGE:
    return "This copy of OwOS has nothing to install.";
  case INSTALL_BUSY:
    return "Another install is already running.";
  case INSTALL_NO_DEVICE:
    return "The disk is no longer connected.";
  case INSTALL_DENIED:
    return "Installing needs administrator rights.";
  default:
    return "";
  }
}

static void load_disks(void)
{
  ndisks = 0;
  denied = false;
  int usable = 0, imageless = 0;
  for (int i = 0; ndisks < MAX_DISKS; i++) {
    int rc = install_disk(i, &disks[ndisks]);
    if (rc < 0)
      denied = true;
    if (rc != 1)
      break;
    usable += disks[ndisks].problem == INSTALL_OK;
    imageless += disks[ndisks].problem == INSTALL_NO_IMAGE;
    ndisks++;
  }
  no_image = ndisks > 0 && imageless > 0 && usable == 0;
  if (selected >= ndisks || (selected >= 0 && disks[selected].problem != INSTALL_OK))
    selected = -1;
  for (int i = 0; i < ndisks && selected < 0; i++)
    if (disks[i].problem == INSTALL_OK)
      selected = i;
}

static void describe_target(int i)
{
  char size[24];
  size_text(disks[i].sectors, size, sizeof size);
  if (disks[i].model[0])
    snprintf(target, sizeof target, "%s (%s, %s)", disks[i].dev, disks[i].model, size);
  else
    snprintf(target, sizeof target, "%s (%s)", disks[i].dev, size);
}

static void add_button(const char *label, int x, enum action act, bool enabled)
{
  buttons[nbuttons++] = (struct button){label, x, BTN_W, act, enabled};
}

/* The buttons a screen offers, laid out right to left from the window's edge, Refresh on the left. */
static void layout_buttons(void)
{
  nbuttons = 0;
  int right = W - PAD - BTN_W, next = right - 10 - BTN_W;
  switch (screen) {
  case LIST:
    add_button("Refresh", PAD, A_REFRESH, true);
    add_button("Close", next, A_CLOSE, true);
    add_button("Erase and install", right, A_INSTALL, selected >= 0);
    break;
  case DONE:
    add_button("Close", next, A_CLOSE, true);
    add_button("Restart now", right, A_RESTART, true);
    break;
  case FAILED:
    add_button("Back to disks", right, A_BACK, true);
    break;
  case INSTALLING:
    break;
  }
  unfocusable[0] = !(screen == LIST && selected >= 0);
  for (int i = 0; i < nbuttons; i++)
    unfocusable[i + 1] = !buttons[i].enabled;
  /* Keep the focused entry while it is usable, else move to the next one that is. */
  gfx_focus_init(&focus, nbuttons + 1, focus.cur < 0 || focus.cur > nbuttons ? 0 : focus.cur, unfocusable);
}

/* Focus the first usable entry of a screen just shown. */
static void focus_first(void)
{
  focus.cur = 0;
  layout_buttons();
}

static void draw_lines(int y, const char *a, const char *b, unsigned fg)
{
  gfx_ui_text_clip(PAD, y, a, fg, W - 2 * PAD);
  if (b)
    gfx_ui_text_clip(PAD, y + LINE_H, b, fg, W - 2 * PAD);
}

static void draw_list(void)
{
  gfx_ui_text_px(PAD, PAD, "Install OwOS", FG_HEAD, HEAD_PX);
  draw_lines(58, "Choose the disk to install OwOS on.", "Everything on that disk will be erased.", FG_TEXT);
  if (denied) {
    draw_lines(LIST_Y, problem_text(INSTALL_DENIED), "Start the Installer as root, or with sudo.", FG_TEXT);
  } else if (no_image) {
    draw_lines(LIST_Y, problem_text(INSTALL_NO_IMAGE),
               "Start OwOS from its USB stick or ISO, then open the Installer again.", FG_TEXT);
  } else if (ndisks == 0) {
    draw_lines(LIST_Y, "No disks found.", "Connect a disk, then choose Refresh.", FG_TEXT);
  }
  for (int i = 0; i < ndisks && !denied && !no_image; i++) {
    int y = LIST_Y + i * ROW_H;
    bool usable = disks[i].problem == INSTALL_OK;
    char label[64], size[24];
    snprintf(label, sizeof label, "%s  %s", disks[i].dev, disks[i].model);
    size_text(disks[i].sectors, size, sizeof size);
    int state = !usable ? BTN_DISABLED : hover_row == i ? BTN_HOVER : BTN_NORMAL;
    radio_draw(PAD, y, label, i == selected, state, gfx_focus_is(&focus, 0) && i == selected);
    if (disks[i].sectors)
      gfx_ui_text(W - PAD - gfx_ui_width(size), y, size, usable ? FG_TEXT : FG_DIM);
    if (!usable)
      gfx_ui_text_clip(PAD + GUI_CHECK_SIZE + 8, y + LINE_H, problem_text(disks[i].problem), FG_WARN,
                       W - 2 * PAD - GUI_CHECK_SIZE - 8);
  }
  if (notice[0])
    gfx_ui_text_clip(PAD, BTN_Y - 2 * LINE_H, notice, FG_WARN, W - 2 * PAD);
}

static void draw(void)
{
  layout_buttons();
  gfx_clear(BG);
  char line[112];
  switch (screen) {
  case LIST:
    draw_list();
    break;
  case INSTALLING:
    gfx_ui_text_px(PAD, PAD, "Installing OwOS", FG_HEAD, HEAD_PX);
    snprintf(line, sizeof line, "On %s.", target);
    draw_lines(58, line, 0, FG_TEXT);
    draw_lines(LIST_Y, prog.stage, 0, FG_TEXT);
    progress_draw(PAD, LIST_Y + 30, W - 2 * PAD, GUI_PROGRESS_H, (int)prog.done, (int)prog.total, 1);
    draw_lines(LIST_Y + 66, "Keep this window open until the install finishes.", 0, FG_DIM);
    break;
  case DONE:
    gfx_ui_text_px(PAD, PAD, "OwOS is installed", FG_HEAD, HEAD_PX);
    snprintf(line, sizeof line, "It is on %s.", target);
    draw_lines(58, line, "Remove the USB stick or disc OwOS started from, then restart.", FG_TEXT);
    break;
  case FAILED:
    gfx_ui_text_px(PAD, PAD, "The install stopped", FG_HEAD, HEAD_PX);
    snprintf(line, sizeof line, "It failed at: %s.", prog.stage);
    draw_lines(58, line, "The disk won't start OwOS. Try again, or choose another disk.", FG_TEXT);
    break;
  }
  for (int i = 0; i < nbuttons; i++) {
    struct button *b = &buttons[i];
    int state = !b->enabled ? BTN_DISABLED : hover_button == i ? BTN_HOVER : BTN_NORMAL;
    button_draw(b->x, BTN_Y, b->w, GUI_BTN_H, b->label, state | (gfx_focus_is(&focus, i + 1) ? GFX_FOCUSED : 0));
  }
  win_present(win);
}

static void start_install(void)
{
  if (selected < 0)
    return;
  describe_target(selected);
  char message[160];
  snprintf(message, sizeof message, "Everything on %s will be erased. This can't be undone.", target);
  /* The dialog's last button is its default; Enter must not erase a disk. */
  static const char *const labels[] = {"Erase and install", "Cancel"};
  if (dialog_choose(win, "Erase this disk?", message, labels, 2, DLG_ICON_WARN) != 0)
    return;
  int rc = install_start(disks[selected].dev);
  if (rc != 0) {
    snprintf(notice, sizeof notice, "Can't install on %s: %s", disks[selected].dev, problem_text(-rc));
    load_disks();
    return;
  }
  notice[0] = '\0';
  prog = (struct install_progress){0};
  screen = INSTALLING;
  focus_first();
}

/* False when the action closes the app. */
static bool run(enum action act)
{
  switch (act) {
  case A_REFRESH:
    notice[0] = '\0';
    load_disks();
    break;
  case A_CLOSE:
    return false;
  case A_INSTALL:
    start_install();
    break;
  case A_RESTART:
    power(1);
    break;
  case A_BACK:
    screen = LIST;
    load_disks();
    focus_first();
    break;
  }
  return true;
}

static int row_at(int x, int y)
{
  if (screen != LIST || denied || no_image || x < PAD || x >= W - PAD || y < LIST_Y)
    return -1;
  int i = (y - LIST_Y) / ROW_H;
  return i < ndisks ? i : -1;
}

static int button_at(int x, int y)
{
  for (int i = 0; i < nbuttons; i++)
    if (buttons[i].enabled && button_hit(buttons[i].x, BTN_Y, buttons[i].w, GUI_BTN_H, x, y))
      return i;
  return -1;
}

static void move_selection(int dir)
{
  for (int i = selected + dir; i >= 0 && i < ndisks; i += dir)
    if (disks[i].problem == INSTALL_OK) {
      selected = i;
      return;
    }
}

/* False when the event closes the app. */
static bool handle(const struct win_event *ev)
{
  bool installing = screen == INSTALLING;
  switch (ev->type) {
  case WEV_CLOSE:
    return installing; /* closing mid-install would leave the disk unbootable */
  case WEV_MOVE:
    hover_button = ev->a < 0 ? -1 : button_at(ev->a, ev->b);
    hover_row = ev->a < 0 ? -1 : row_at(ev->a, ev->b);
    return true;
  case WEV_MOUSE: {
    int b = button_at(ev->a, ev->b), r = row_at(ev->a, ev->b);
    if (b >= 0)
      return run(buttons[b].act);
    if (r >= 0 && disks[r].problem == INSTALL_OK) {
      selected = r;
      focus_first();
    }
    return true;
  }
  case WEV_KEY:
    if (installing)
      return true;
    if (ev->a == 27)
      return false;
    if (gfx_focus_key(&focus, ev->a, ev->b))
      return true;
    if (gfx_focus_is(&focus, 0)) {
      if (ev->a == KEY_UP || ev->a == KEY_DOWN)
        move_selection(ev->a == KEY_UP ? -1 : 1);
      else if (ev->a != ' ' && button_key(ev->a)) /* Enter on the list installs onto the selected disk */
        return run(A_INSTALL);
      return true;
    }
    if (focus.cur > 0 && button_key(ev->a))
      return run(buttons[focus.cur - 1].act);
    return true;
  default:
    return true;
  }
}

int main(void)
{
  win = win_create(W, H, "Installer");
  if (win < 0) {
    puts("installer: no window\n");
    return 1;
  }
  gfx_init(win_fb(), W, H);
  win_resizable(win, 0);
  load_disks();
  focus_first();
  draw();

  for (;;) {
    struct win_event ev;
    bool dirty = false;
    while (win_poll(win, &ev)) {
      if (!handle(&ev)) {
        win_close(win);
        return 0;
      }
      dirty = true;
    }
    if (screen == INSTALLING) {
      dirty = true;
      int rc = install_step(&prog);
      if (rc != 0) {
        screen = rc > 0 ? DONE : FAILED;
        focus_first();
      }
    }
    if (dirty)
      draw();
    if (screen != INSTALLING)
      win_wait(win);
  }
}
