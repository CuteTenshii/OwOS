/* archive.c: browse and extract ZIP, TAR, TAR.GZ, TAR.ZST and TAR.BR archives.
 *
 *   archive                  open the window empty
 *   archive <file>           open the window on <file> (how Files launches it)
 *   archive -l <file>        print the entry list to stdout
 *   archive -x <file> <dir>  extract everything into <dir>
 */
#include "archive.h"
#include "dialog.h"
#include "gfx.h"
#include "owos.h"

#define MENU_H 18
#define PATH_H 28
#define HDR_H 20
#define ROW_H 22
#define STATUS_H 22
#define PAD 8
#define ICON_W 14
#define SIZE_W 84
#define LIST_TOP (MENU_H + PATH_H + HDR_H)

/* Shared with Files so the two read as one system */
#define COL_BG 0x12161e
#define COL_BAR 0x232a36
#define COL_PATHBAR 0x1a1f2a
#define COL_HDR 0x1d2430
#define COL_TEXT 0xc8d0dc
#define COL_DIM 0x8aa0b8
#define COL_SEL 0x2f6fb0
#define COL_HOVER 0x1a212c

static int W = 560, H = 400;
static int win_id;

static struct archive *arc;
static char arc_path[256]; /* the archive currently open */
static int arc_count;
static int scroll, sel = -1, hover = -1;

static char path_buf[256];
static struct textfield path_tf;
static int path_focus;

static struct status_bar status;
static char dest_dir[256] = "/home/root";

static const char *const file_items[] = {"Open...", "Extract Selected...", "Extract All...", "Close"};
static const char *const file_hints[] = {"Ctrl+O", "Enter", "", ""};
static const unsigned char file_keys[] = {KEY_OPEN, 0, 0, 0};
static struct menu file_menu = {"File", file_items, 4, 0, file_hints, file_keys};
static const char *const help_items[] = {"About"};
static struct menu help_menu = {"Help", help_items, 1, 0, NULL, NULL};
static struct menu *const bar_menus[] = {&file_menu, &help_menu};
static struct menubar mbar = {bar_menus, 2, 0, 0};

static const char *const archive_exts[] = {"zip", "tar", "tgz", "gz", "zst", "br", NULL};

static int visible_rows(void)
{
  int v = (H - LIST_TOP - STATUS_H) / ROW_H;
  return v < 1 ? 1 : v;
}

static void clamp_scroll(void)
{
  int vis = visible_rows();
  if (scroll > arc_count - vis)
    scroll = arc_count - vis;
  if (scroll < 0)
    scroll = 0;
}

static void reveal(int idx)
{
  int vis = visible_rows();
  if (idx < scroll)
    scroll = idx;
  else if (idx >= scroll + vis)
    scroll = idx - vis + 1;
  clamp_scroll();
}

static void scopy(char *d, const char *s, int cap)
{
  int i = 0;
  while (s[i] && i < cap - 1) {
    d[i] = s[i];
    i++;
  }
  d[i] = 0;
}

/* "N B" / "N.N KiB" / "N.N MiB" */
static void size_str(unsigned v, char *out, int cap)
{
  if (v < 1024)
    snprintf(out, cap, "%u B", v);
  else if (v < 1024 * 1024)
    snprintf(out, cap, "%u.%u KiB", v >> 10, ((v & 0x3FF) * 10) >> 10);
  else
    snprintf(out, cap, "%u.%u MiB", v >> 20, ((v & 0xFFFFF) * 10) >> 20);
}

static const char *base_name(const char *path)
{
  const char *b = path;
  for (const char *p = path; *p; p++)
    if (*p == '/')
      b = p + 1;
  return b;
}

static int ends_with_ci(const char *s, const char *suffix)
{
  int sl = strlen(s), xl = strlen(suffix);
  if (xl > sl)
    return 0;
  for (int i = 0; i < xl; i++) {
    char c = s[sl - xl + i];
    if (c >= 'A' && c <= 'Z')
      c += 32;
    if (c != suffix[i])
      return 0;
  }
  return 1;
}

/* The archive's file name without its archive extension: the folder Extract All creates */
static void stem_of(const char *path, char *out, int cap)
{
  static const char *const suffixes[] = {".tar.gz", ".tar.zst", ".tar.br", ".tgz", ".zip", ".tar"};
  scopy(out, base_name(path), cap);
  for (unsigned i = 0; i < sizeof suffixes / sizeof suffixes[0]; i++)
    if (ends_with_ci(out, suffixes[i]) && strlen(out) > strlen(suffixes[i])) {
      out[strlen(out) - strlen(suffixes[i])] = 0;
      return;
    }
}

static void close_archive(void)
{
  if (arc)
    archive_close(arc);
  arc = NULL;
  arc_path[0] = 0;
  arc_count = 0;
  scroll = 0;
  sel = hover = -1;
}

static void open_archive(const char *path)
{
  char msg[320];
  close_archive();
  textfield_set(&path_tf, path);
  arc = archive_open(path);
  if (!arc) {
    snprintf(msg, sizeof msg, "Couldn't open %s. It isn't a supported archive, or it's damaged.", base_name(path));
    status_set(&status, msg);
    return;
  }
  scopy(arc_path, path, sizeof arc_path);
  arc_count = archive_count(arc);
  sel = arc_count > 0 ? 0 : -1;
  snprintf(msg, sizeof msg, "%s, %d %s", archive_format(arc), arc_count, arc_count == 1 ? "entry" : "entries");
  status_set(&status, msg);
}

/* The directory holding the open archive: where pickers start */
static void archive_dir(char *out, int cap)
{
  scopy(out, arc_path[0] ? arc_path : dest_dir, cap);
  if (arc_path[0])
    filelist_parent(out);
}

static void choose_and_open(void)
{
  char start[256], path[256];
  archive_dir(start, sizeof start);
  struct filepicker_opts o = {
      .title = "Open Archive", .mode = FILEPICK_OPEN, .start = start, .filter_label = "Archives", .exts = archive_exts};
  if (filepicker(win_id, &o, path, sizeof path))
    open_archive(path);
}

static int choose_folder(const char *title, char *out, int cap)
{
  struct filepicker_opts o = {.title = title, .mode = FILEPICK_FOLDER, .start = dest_dir};
  if (!filepicker(win_id, &o, out, cap))
    return 0;
  scopy(dest_dir, out, sizeof dest_dir);
  return 1;
}

/* The selected entry, or every entry under it when it is a directory, keeping archive paths below the folder */
static void extract_selected(void)
{
  struct arcent e, other;
  char dir[256], msg[400];
  if (!arc || sel < 0 || archive_entry(arc, sel, &e) < 0 || !choose_folder("Extract To", dir, sizeof dir))
    return;
  int failed = archive_extract_into(arc, sel, dir) < 0, done = !failed;
  if (e.is_dir) {
    int nl = strlen(e.name);
    for (int i = 0; i < arc_count; i++) {
      if (i == sel || archive_entry(arc, i, &other) < 0)
        continue;
      if (!memcmp(other.name, e.name, (unsigned)nl) && other.name[nl] == '/') {
        if (archive_extract_into(arc, i, dir) < 0)
          failed++;
        else
          done++;
      }
    }
  }
  if (failed)
    snprintf(msg, sizeof msg, "Extracted %d of %d from %s; the rest failed or point outside the folder.", done,
             done + failed, e.name);
  else
    snprintf(msg, sizeof msg, "Extracted %s to %s", e.name, dir);
  status_set(&status, msg);
}

static void extract_all(void)
{
  char dir[256], stem[128], target[400], msg[480];
  if (!arc || !choose_folder("Extract All To", dir, sizeof dir))
    return;
  stem_of(arc_path, stem, sizeof stem);
  snprintf(target, sizeof target, "%s/%s", dir, stem);
  if (archive_extract_all(arc, target) < 0) {
    snprintf(msg, sizeof msg, "Couldn't extract everything into %s. Some files may already be there.", target);
    dialog_message(win_id, "Extract All", msg);
    status_set(&status, "Extract All stopped partway.");
    return;
  }
  snprintf(msg, sizeof msg, "Extracted %d %s to %s", arc_count, arc_count == 1 ? "entry" : "entries", target);
  status_set(&status, msg);
}

static void menu_action(struct menu *leaf, int item)
{
  if (leaf == &file_menu) {
    if (item == 0)
      choose_and_open();
    else if (item == 1)
      extract_selected();
    else if (item == 2)
      extract_all();
    else if (item == 3) {
      close_archive();
      textfield_set(&path_tf, "");
      status_set(&status, "");
    }
  } else if (leaf == &help_menu) {
    dialog_message(win_id, "About Archive",
                   "Browse and extract ZIP, TAR, TAR.GZ, TAR.ZST and TAR.BR archives.\n"
                   "Entries whose paths point outside the destination are never written.");
  }
}

static void render(void)
{
  int vis = visible_rows();
  int list_w = arc_count > vis ? W - GUI_SCROLL_W : W;
  int size_x = list_w - PAD - SIZE_W, packed_x = size_x - SIZE_W;
  int ty = (ROW_H - UIFONT_H) / 2;

  gfx_clear(COL_BG);

  gfx_fill_rect(0, MENU_H, W, PATH_H, COL_PATHBAR);
  gfx_ui_text(PAD, MENU_H + (PATH_H - UIFONT_H) / 2, "Archive", COL_DIM);
  int fx = PAD + gfx_ui_width("Archive") + PAD;
  textfield_draw(&path_tf, fx, MENU_H + 3, W - fx - PAD, PATH_H - 6, path_focus);

  gfx_fill_rect(0, MENU_H + PATH_H, W, HDR_H, COL_HDR);
  int hy = MENU_H + PATH_H + (HDR_H - UIFONT_H) / 2;
  gfx_ui_text(PAD + ICON_W + PAD, hy, "Name", COL_DIM);
  gfx_ui_text(packed_x + SIZE_W - gfx_ui_width("Packed"), hy, "Packed", COL_DIM);
  gfx_ui_text(size_x + SIZE_W - gfx_ui_width("Size"), hy, "Size", COL_DIM);

  if (!arc) {
    gfx_ui_text_clip(PAD, LIST_TOP + PAD, "Open an archive with File > Open (Ctrl+O), or type its path above.", COL_DIM,
                     W - 2 * PAD);
  }
  for (int r = 0; r < vis && scroll + r < arc_count; r++) {
    int idx = scroll + r, ry = LIST_TOP + r * ROW_H;
    struct arcent e;
    char sz[24];
    if (archive_entry(arc, idx, &e) < 0)
      break;
    if (idx == sel)
      gfx_fill_rect(0, ry, list_w, ROW_H, COL_SEL);
    else if (idx == hover)
      gfx_fill_rect(0, ry, list_w, ROW_H, COL_HOVER);
    unsigned fg = idx == sel ? 0xffffff : COL_TEXT;
    filepicker_icon(PAD, ry + (ROW_H - 12) / 2, e.is_dir);
    int nx = PAD + ICON_W + PAD;
    gfx_ui_text_clip(nx, ry + ty, e.name, fg, packed_x - PAD - nx);
    if (!e.is_dir) {
      size_str(e.size, sz, sizeof sz);
      gfx_ui_text(size_x + SIZE_W - gfx_ui_width(sz), ry + ty, sz, fg);
      if (e.packed != e.size) {
        size_str(e.packed, sz, sizeof sz);
        gfx_ui_text(packed_x + SIZE_W - gfx_ui_width(sz), ry + ty, sz, idx == sel ? 0xffffff : COL_DIM);
      }
    }
  }
  if (arc_count > vis)
    scrollbar_draw(W - GUI_SCROLL_W, LIST_TOP, GUI_SCROLL_W, vis * ROW_H, scroll, arc_count, vis);

  status_draw(&status, 0, H - STATUS_H, W, COL_DIM, COL_PATHBAR);

  /* The menu bar last, so an open dropdown overlays the list */
  gfx_fill_rect(0, 0, W, MENU_H, COL_BAR);
  menubar_draw(&mbar);
}

static int row_at(int y)
{
  if (y < LIST_TOP || y >= LIST_TOP + visible_rows() * ROW_H)
    return -1;
  int idx = scroll + (y - LIST_TOP) / ROW_H;
  return idx < arc_count ? idx : -1;
}

static void on_click(int x, int y)
{
  struct menu *leaf;
  int item;
  if (y < MENU_H || file_menu.open || help_menu.open) {
    if (menubar_click(&mbar, x, y, &leaf, &item) && item >= 0)
      menu_action(leaf, item);
    return;
  }
  int fx = PAD + gfx_ui_width("Archive") + PAD;
  path_focus = textfield_click(&path_tf, x, y, fx, MENU_H + 3, W - fx - PAD, PATH_H - 6);
  if (path_focus)
    return;

  int vis = visible_rows();
  if (arc_count > vis && x >= W - GUI_SCROLL_W) {
    int hit = scrollbar_hit(W - GUI_SCROLL_W, LIST_TOP, GUI_SCROLL_W, vis * ROW_H, x, y);
    if (hit == SCROLLBAR_UP)
      scroll--;
    else if (hit == SCROLLBAR_DOWN)
      scroll++;
    else if (hit == SCROLLBAR_TRACK)
      scroll = scrollbar_pos(W - GUI_SCROLL_W, LIST_TOP, GUI_SCROLL_W, vis * ROW_H, x, y, scroll, arc_count, vis);
    clamp_scroll();
    return;
  }
  int idx = row_at(y);
  if (idx >= 0)
    sel = idx;
}

static void on_key(int key, int mods)
{
  struct menu *leaf;
  int item = menubar_shortcut(&mbar, key, &leaf);
  if (item >= 0) {
    menubar_close(&mbar);
    menu_action(leaf, item);
    return;
  }
  if (key == 27) {
    if (file_menu.open || help_menu.open)
      menubar_close(&mbar);
    else if (path_focus) {
      path_focus = 0;
      textfield_set(&path_tf, arc_path);
    }
    return;
  }
  if (path_focus) {
    if (key == '\n') {
      path_focus = 0;
      if (path_tf.len)
        open_archive(path_buf);
    } else {
      textfield_key(&path_tf, (char)key, mods);
    }
    return;
  }
  if (!arc_count)
    return;
  if (key == KEY_UP && sel > 0)
    sel--;
  else if (key == KEY_DOWN && sel < arc_count - 1)
    sel++;
  else if (key == KEY_HOME)
    sel = 0;
  else if (key == KEY_END)
    sel = arc_count - 1;
  else if (key == '\n') {
    extract_selected();
    return;
  } else
    return;
  reveal(sel);
}

static int cli(int argc, char **argv)
{
  if (argc >= 3 && streq(argv[1], "-l")) {
    struct archive *a = archive_open(argv[2]);
    if (!a) {
      printf("archive: %s: not a supported archive, or damaged\n", argv[2]);
      return 1;
    }
    printf("%s, %d entries\n", archive_format(a), archive_count(a));
    for (int i = 0; i < archive_count(a); i++) {
      struct arcent e;
      char sz[24];
      archive_entry(a, i, &e);
      size_str(e.size, sz, sizeof sz);
      printf("  %s%s  %s\n", e.name, e.is_dir ? "/" : "", e.is_dir ? "" : sz);
    }
    archive_close(a);
    return 0;
  }
  if (argc >= 4 && streq(argv[1], "-x")) {
    struct archive *a = archive_open(argv[2]);
    if (!a) {
      printf("archive: %s: not a supported archive, or damaged\n", argv[2]);
      return 1;
    }
    int rc = archive_extract_all(a, argv[3]);
    if (rc < 0)
      printf("archive: extracting into %s failed partway\n", argv[3]);
    else
      printf("extracted %d entries into %s\n", archive_count(a), argv[3]);
    archive_close(a);
    return rc < 0 ? 1 : 0;
  }
  printf("usage: archive [file]\n"
         "       archive -l <file>        list entries\n"
         "       archive -x <file> <dir>  extract everything into <dir>\n");
  return 2;
}

int main(int argc, char **argv)
{
  if (argc > 1 && argv[1][0] == '-')
    return cli(argc, argv);

  win_id = win_create(W, H, "Archive");
  if (win_id < 0)
    return 1;
  gfx_init(win_fb(), W, H);
  win_resizable(win_id, 1);
  textfield_init(&path_tf, path_buf, sizeof path_buf);
  if (argc > 1)
    open_archive(argv[1]);

  for (;;) {
    struct win_event ev;
    while (win_poll(win_id, &ev)) {
      if (ev.type == WEV_CLOSE) {
        close_archive();
        win_close(win_id);
        return 0;
      } else if (ev.type == WEV_RESIZE) {
        W = ev.a;
        H = ev.b;
        gfx_init(win_fb(), W, H);
        clamp_scroll();
      } else if (ev.type == WEV_MOUSE) {
        on_click(ev.a, ev.b);
      } else if (ev.type == WEV_DBLCLICK) {
        if (row_at(ev.b) >= 0)
          extract_selected();
      } else if (ev.type == WEV_DRAG && path_focus) {
        int fx = PAD + gfx_ui_width("Archive") + PAD;
        textfield_drag(&path_tf, ev.a, fx, W - fx - PAD);
      } else if (ev.type == WEV_MOVE) {
        hover = ev.a < 0 ? -1 : row_at(ev.b);
      } else if (ev.type == WEV_SCROLL) {
        scroll -= ev.a * 3;
        clamp_scroll();
      } else if (ev.type == WEV_KEY) {
        on_key(ev.a, ev.b);
      }
    }
    render();
    win_present(win_id);
    win_wait(win_id);
  }
}
