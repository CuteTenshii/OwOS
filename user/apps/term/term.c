/* term.c, a terminal: a resizable, scrollable window hosting a shell. A text grid drawn with the
 * 8x16 font (gfx.c) over a scrollback ring buffer */

#include "clipboard.h" /* the clipd-backed multi-item clipboard */
#include "crypto.h"    /* SHA-256 for passwd hashing (su/login/passwd) */
#include "deflate.h"
#include "gfx.h"
#include "idnames.h"
#include "lzss.h"
#include "modeutil.h"
#include "owos.h"
#include <stdbool.h>

#define FONT_W 8
#define FONT_H 16
#define MAXCOLS 200
#define SBROWS 400 /* scrollback lines */

#define BG 0x0e1116u
#define FG 0xc8d0dcu
#define FG_PROMPT 0x49d17fu
#define FG_DIR 0x6cb6ffu
#define FG_ERR 0xe06c75u

/* The shell's identity */
static char username[32] = "root";
static char home[256] = "/home/root";
static char hostnm[64] = "owos";

/* Refresh `username`/`home` for the calling process */
static void reload_identity(void) { user_ident(username, sizeof username, home, sizeof home); }

/* scrollback ring buffer */

static unsigned char gch[SBROWS][MAXCOLS];
static unsigned gfg[SBROWS][MAXCOLS];
static int top;             /* phys index of logical line 0 */
static int nlines = 1;      /* logical lines in use */
static int cl, cc;          /* cursor: logical line, column */
static int view;            /* lines scrolled up from the bottom */
static int ls_line, ls_col; /* where the input line starts */
static unsigned cur_fg = FG;
static int cols = 90, rows = 27;   /* active grid (from window size) */
static int winw = 720, winh = 432; /* window client size in pixels */
static int win;
static bool dirty = true;

static int phys(int logical) { return (top + logical) % SBROWS; }

static void line_blank(int p)
{
  for (int x = 0; x < MAXCOLS; x++) {
    gch[p][x] = ' ';
    gfg[p][x] = FG;
  }
}

static void grid_reset(void)
{
  for (int p = 0; p < SBROWS; p++)
    line_blank(p);
  top = 0;
  nlines = 1;
  cl = cc = 0;
  view = 0;
  dirty = true;
}

/* Advance to a fresh next line */
static void tnewline(void)
{
  cc = 0;
  cl++;
  if (cl >= nlines) {
    if (nlines < SBROWS) {
      nlines = cl + 1;
    } else {
      top = (top + 1) % SBROWS; /* drop the oldest line */
      cl--;
      if (ls_line > 0)
        ls_line--;
      line_blank((top + nlines - 1) % SBROWS);
    }
  }
}

/* 16-colour ANSI palette */
static const unsigned ansi_rgb[16] = {
    0x1b1f26, 0xe06c75, 0x98c379, 0xe5c07b, 0x61afef, 0xc678dd, 0x56b6c2, 0xc8d0dc,
    0x5c6370, 0xff7b86, 0xb5e890, 0xffd68a, 0x7ec5ff, 0xe0a0ff, 0x70d8e4, 0xffffff,
};
static int esc; /* 0 normal, 1 saw ESC, 2 in CSI */
static int eparm[8], enp;

static void sgr_apply(void)
{
  for (int i = 0; i <= enp; i++) {
    int p = eparm[i];
    if (p == 0)
      cur_fg = FG; /* reset */
    else if (p >= 30 && p <= 37)
      cur_fg = ansi_rgb[p - 30];
    else if (p >= 90 && p <= 97)
      cur_fg = ansi_rgb[8 + p - 90];
    /* bg codes (40-47, 100-107) ignored: the grid background is fixed */
  }
}

/* Headless stdio mode (term -stdio): no window/grid */
static int headless;

static void tputc(char c)
{
  if (headless) { /* stream to stdout; LF -> CRLF for a raw tty */
    if (c == '\n') {
      write(1, "\r\n", 2);
    } else {
      write(1, &c, 1);
    }
    return;
  }
  /* ANSI SGR (colour) escape parser: ESC [ params m */
  if (esc == 1) {
    esc = (c == '[') ? 2 : 0;
    if (esc == 2) {
      enp = 0;
      eparm[0] = 0;
    }
    return;
  }
  if (esc == 2) {
    if (c >= '0' && c <= '9')
      eparm[enp] = eparm[enp] * 10 + (c - '0');
    else if (c == ';') {
      if (enp < 7)
        eparm[++enp] = 0;
    } else {
      if (c == 'm')
        sgr_apply();
      esc = 0;
    }
    return;
  }
  if (c == 0x1b) {
    esc = 1;
    return;
  }

  view = 0; /* any output snaps to the bottom */
  if (c == '\n') {
    tnewline();
  } else if (c == '\r') {
    cc = 0;
  } /* carriage return (e.g. ssh's CRLF) */
  else if (c == '\b') {
    if (cc > 0) {
      cc--;
      gch[phys(cl)][cc] = ' ';
    }
  } else if (c == '\t') {
    do {
      tputc(' ');
    } while (cc % 8);
  } else {
    if (cc >= cols)
      tnewline();
    int p = phys(cl);
    gch[p][cc] = (unsigned char)c;
    gfg[p][cc] = cur_fg;
    cc++;
  }
  dirty = true;
}

static void tputs(const char *s)
{
  while (*s)
    tputc(*s++);
}
static void tputs_c(const char *s, unsigned f)
{
  unsigned o = cur_fg;
  cur_fg = f;
  tputs(s);
  cur_fg = o;
}
static void tdec(unsigned v)
{
  char b[12];
  int n = 0;
  if (!v) {
    tputc('0');
    return;
  }
  while (v) {
    b[n++] = (char)('0' + v % 10);
    v /= 10;
  }
  while (n--)
    tputc(b[n]);
}

static void redraw(void)
{
  if (headless)
    return;                                           /* no window/grid in SSH stdio mode */
  gfx_fill_rect(0, 0, winw - GUI_SCROLL_W, winh, BG); /* text area */
  int bottom = nlines > rows ? nlines - rows : 0;
  int vt = bottom - view;
  if (vt < 0)
    vt = 0;
  for (int r = 0; r < rows; r++) {
    int L = vt + r;
    if (L >= nlines)
      break;
    int p = phys(L);
    for (int x = 0; x < cols; x++)
      if (gch[p][x] != ' ')
        gfx_glyph(x * FONT_W, r * FONT_H, gch[p][x], gfg[p][x], BG);
  }
  if (view == 0 && cl >= vt && cl < vt + rows) /* caret at bottom */
    gfx_fill_rect(cc * FONT_W, (cl - vt) * FONT_H + FONT_H - 2, FONT_W, 2, FG);

  /* Scrollbar down the right edge: `scroll` is the top visible line (vt) */
  scrollbar_draw(winw - GUI_SCROLL_W, 0, GUI_SCROLL_W, winh, bottom - view, nlines, rows);

  win_present(win);
  dirty = false;
}

/* tiny string helpers */

static bool has_prefix(const char *s, const char *p)
{
  while (*p) {
    if (*s++ != *p++)
      return false;
  }
  return true;
}

/* cwd + path resolution */

static char cwd[256] = "/home/root";

/* Resolve `name` against cwd into `out`: ~ expands to the user's home, then  */
static void abspath(const char *name, char *out)
{
  char in[512];
  if (name[0] == '~' && (name[1] == '\0' || name[1] == '/')) {
    strcpy(in, home);
    strcat(in, name + 1);
  } else if (name[0] == '/') {
    strcpy(in, name);
  } else {
    strcpy(in, cwd);
    int l = strlen(in);
    if (l == 0 || in[l - 1] != '/') {
      in[l] = '/';
      in[l + 1] = '\0';
    }
    strcat(in, name);
  }
  char seg[64][32];
  int ns = 0, i = 0;
  while (in[i]) {
    if (in[i] == '/') {
      i++;
      continue;
    }
    char s[32];
    int j = 0;
    while (in[i] && in[i] != '/' && j < 31)
      s[j++] = in[i++];
    s[j] = '\0';
    if (strcmp(s, ".") == 0)
      continue;
    if (strcmp(s, "..") == 0) {
      if (ns > 0)
        ns--;
      continue;
    }
    if (ns < 64)
      strcpy(seg[ns++], s);
  }
  if (ns == 0) {
    strcpy(out, "/");
    return;
  }
  out[0] = '\0';
  for (int k = 0; k < ns; k++) {
    strcat(out, "/");
    strcat(out, seg[k]);
  }
}

/* builtins */

static char iobuf[64 * 1024];

static void prompt(void)
{
  tputs_c(username, FG_PROMPT);
  tputs_c("@", FG_PROMPT);
  tputs_c(hostnm, FG_PROMPT);
  tputs_c(":", FG_PROMPT);
  int hl = strlen(home);
  if (strcmp(cwd, home) == 0)
    tputs_c("~", FG_PROMPT);
  else if (strlen(cwd) > hl && cwd[hl] == '/' && has_prefix(cwd, home)) {
    tputs_c("~", FG_PROMPT);
    tputs_c(cwd + hl, FG_PROMPT);
  } else
    tputs_c(cwd, FG_PROMPT);
  /* '#' for root, '$' for everyone else, the classic privilege cue */
  tputs_c(strcmp(username, "root") == 0 ? "# " : "$ ", FG_PROMPT);
  ls_line = cl;
  ls_col = cc;
}

static void err(const char *a, const char *b)
{
  tputs_c(a, FG_ERR);
  if (b)
    tputs_c(b, FG_ERR);
  tputc('\n');
}

/* "cmd: <msg>path", with "permission denied: " in place of msg when rc is -EACCES */
static void errc(int rc, const char *cmd, const char *msg, const char *path)
{
  char b[96];
  strcpy(b, cmd);
  strcat(b, ": ");
  strcat(b, rc == -EACCES ? "permission denied: " : msg);
  err(b, path);
}

static void bi_help(void)
{
  tputs_c("OwOS shell.  Ctrl+C cancels the line / aborts a command.\n", FG_DIR);
  tputs("files    ls cat cp mv rm mkdir rmdir stat chmod chown umask pwd cd grep\n");
  tputs("archive  zip unzip gzip gunzip\n");
  tputs("system   uptime hostname whoami id cpuinfo meminfo date ps journal "
        "which\n");
  tputs("users    useradd userdel passwd usermod groupadd groupdel users "
        "groups\n");
  tputs("session  su login logout    (su alice [password]; logout -> root)\n");
  tputs("device   storage lsusb mount umount loadkeys clip\n");
  tputs("net      services (dhcp/ntp run as services: `services start ntp`)\n");
  tputs("power    poweroff restart\n");
  tputs("shell    help clear echo sleep exit\n");
  tputs("remote   ssh user@host (interactive SSH-2 session)\n");
  tputs("Any /bin program runs here: its output streams inline; if it opens a\n");
  tputs("window (editor files clock browser ...) the prompt returns.\n");
}

static void tpad_dec(unsigned v, int width)
{
  int digits = 1;
  for (unsigned t = v; t >= 10; t /= 10)
    digits++;
  for (; digits < width; digits++)
    tputc(' ');
  tdec(v);
}

static void t2(unsigned v)
{
  tputc((char)('0' + v / 10 % 10));
  tputc((char)('0' + v % 10));
}

/* Packed owofs mtime as "YYYY-MM-DD HH:MM", or dashes when unstamped */
static void tmtime(unsigned m)
{
  if (!m) {
    tputs("   -            ");
    return;
  }
  tdec(2000 + (m >> 26));
  tputc('-');
  t2((m >> 22) & 0xF);
  tputc('-');
  t2((m >> 17) & 0x1F);
  tputc(' ');
  t2((m >> 12) & 0x1F);
  tputc(':');
  t2((m >> 6) & 0x3F);
}

/* One `ls -l` row for the entry at `path` */
static void ls_long_row(const struct idnames *names, const char *path, const char *name, int is_dir, unsigned size)
{
  struct statinfo st;
  char mode[11], who[32], grp[32];
  if (stat(path, &st) != 0) {
    st.mode = 0;
    st.uid = st.gid = 0;
    st.mtime = 0;
    st.size = size;
    st.is_dir = is_dir;
    strcpy(mode, "??????????");
  } else {
    perm_mode_str(st.mode, st.is_dir, mode);
  }
  idn_user_name(names, st.uid, who, sizeof who);
  idn_group_name(names, st.gid, grp, sizeof grp);
  tputs(mode);
  tputc(' ');
  tputs(who);
  tputc(' ');
  tputs(grp);
  tputc(' ');
  tpad_dec(st.is_dir ? 0 : st.size, 8);
  tputc(' ');
  tmtime(st.mtime);
  tputc(' ');
  tputs_c(name, st.is_dir ? FG_DIR : FG);
  if (st.is_dir)
    tputc('/');
  tputc('\n');
}

static void bi_ls(int argc, char **argv)
{
  char p[256];
  bool long_fmt = false;
  const char *target = ".";
  for (int k = 1; k < argc; k++) {
    if (argv[k][0] == '-' && argv[k][1]) {
      for (const char *f = argv[k] + 1; *f; f++)
        if (*f == 'l')
          long_fmt = true;
    } else {
      target = argv[k];
    }
  }
  abspath(target, p);
  struct dirent_u de;
  int i = 0;
  struct statinfo st;
  int rc = stat(p, &st);
  if (rc != 0) {
    errc(rc, "ls", "no such path: ", p);
    return;
  }
  rc = path_access(p, R_OK);
  if (rc != 0) {
    errc(rc, "ls", "cannot read: ", p);
    return;
  }
  struct idnames names = {0};
  if (long_fmt)
    idn_load(&names);
  if (!st.is_dir) {
    if (long_fmt) {
      ls_long_row(&names, p, target, 0, st.size);
    } else {
      tputs(target);
      tputc('\n');
    }
    idn_free(&names);
    return;
  }
  while (readdir(p, i++, &de) == 1) {
    if (!long_fmt) {
      tputs_c(de.name, de.is_dir ? FG_DIR : FG);
      if (de.is_dir)
        tputc('/');
      tputc('\n');
      continue;
    }
    char child[320];
    strcpy(child, p);
    if (strcmp(p, "/") != 0)
      strcat(child, "/");
    strcat(child, de.name);
    ls_long_row(&names, child, de.name, de.is_dir, de.size);
  }
  if (long_fmt)
    idn_free(&names);
}

static void bi_cat(int argc, char **argv)
{
  if (argc < 2) {
    tputs("usage: cat <file>\n");
    return;
  }
  char p[256];
  abspath(argv[1], p);
  int n = readfile(p, iobuf, sizeof iobuf);
  if (n < 0) {
    errc(n, "cat", "cannot read: ", argv[1]);
    return;
  }
  for (int i = 0; i < n; i++)
    tputc(iobuf[i]);
  if (n && iobuf[n - 1] != '\n')
    tputc('\n');
}

static void bi_cp(int argc, char **argv)
{
  if (argc < 3) {
    tputs("usage: cp <src> <dst>\n");
    return;
  }
  char a[256], b[256];
  abspath(argv[1], a);
  abspath(argv[2], b);
  int n = readfile(a, iobuf, sizeof iobuf);
  if (n < 0) {
    errc(n, "cp", "cannot read source: ", argv[1]);
    return;
  }
  int wr = writefile(b, iobuf, n);
  if (wr < 0)
    errc(wr, "cp", "cannot write dest: ", argv[2]);
}

static void bi_stat(int argc, char **argv)
{
  if (argc < 2) {
    tputs("usage: stat <path>\n");
    return;
  }
  char p[256], mode[11], who[32], grp[32];
  abspath(argv[1], p);
  struct statinfo st;
  int rc = stat(p, &st);
  if (rc != 0) {
    errc(rc, "stat", "no such path: ", p);
    return;
  }
  struct idnames names;
  idn_load(&names);
  idn_user_name(&names, st.uid, who, sizeof who);
  idn_group_name(&names, st.gid, grp, sizeof grp);
  idn_free(&names);
  perm_mode_str(st.mode, st.is_dir, mode);
  tputs(p);
  tputs(st.is_dir ? "  <dir>  " : "  ");
  if (!st.is_dir) {
    tdec(st.size);
    tputs(" bytes  ");
  }
  tputs(mode);
  tputs(" (0");
  tdec((st.mode >> 9 & 7) * 1000 + (st.mode >> 6 & 7) * 100 + (st.mode >> 3 & 7) * 10 + (st.mode & 7));
  tputs(") ");
  tputs(who);
  tputc(':');
  tputs(grp);
  tputs("  ");
  tmtime(st.mtime);
  tputc('\n');
}

static void bi_umask(int argc, char **argv)
{
  if (argc < 2) {
    unsigned m = umask((unsigned)-1);
    tputc('0');
    tputc((char)('0' + (m >> 6 & 7)));
    tputc((char)('0' + (m >> 3 & 7)));
    tputc((char)('0' + (m & 7)));
    tputc('\n');
    return;
  }
  unsigned v = 0;
  for (const char *c = argv[1]; *c; c++) {
    if (*c < '0' || *c > '7' || v > 077) {
      err("umask: invalid mask: ", argv[1]);
      return;
    }
    v = v * 8 + (unsigned)(*c - '0');
  }
  umask(v);
}

/* info builtins: read /proc and render (no kernel-shell bridge) */

/* Print /proc/<name> verbatim */
static int cat_proc(const char *name)
{
  char p[64];
  strcpy(p, "/proc/");
  strcat(p, name);
  int n = readfile(p, iobuf, sizeof iobuf);
  if (n < 0)
    return 0;
  for (int i = 0; i < n; i++)
    tputc(iobuf[i]);
  if (n && iobuf[n - 1] != '\n')
    tputc('\n');
  return 1;
}

static unsigned to_uint(const char *s)
{
  unsigned v = 0;
  while (*s >= '0' && *s <= '9')
    v = v * 10 + (unsigned)(*s++ - '0');
  return v;
}

static void bi_uptime(void)
{
  if (readfile("/proc/uptime", iobuf, sizeof iobuf) < 0) {
    err("uptime: unavailable", 0);
    return;
  }
  unsigned s = to_uint(iobuf), h = s / 3600, m = (s % 3600) / 60;
  tputs("up ");
  if (h) {
    tdec(h);
    tputs("h ");
  }
  if (h || m) {
    tdec(m);
    tputs("m ");
  }
  tdec(s % 60);
  tputs("s\n");
}

static void bi_which(int argc, char **argv)
{
  static const char *const ring3[] = {
      "help",    "clear",    "echo",     "pwd",     "cd",     "ls",     "cat",      "cp",      "mv",
      "stat",    "mkdir",    "rmdir",    "rm",      "exit",   "uptime", "hostname", "whoami",  "which",
      "umask",  "cpuinfo", "meminfo",  "date",     "storage", "sleep",  "id",       "useradd", "userdel",
      "passwd", "usermod", "groupadd", "groupdel", "users",   "groups", "su",       "login",   "logout",
  };
  if (argc < 2) {
    tputs("usage: which <name>\n");
    return;
  }
  for (unsigned i = 0; i < sizeof ring3 / sizeof ring3[0]; i++)
    if (!strcmp(argv[1], ring3[i])) {
      tputs(argv[1]);
      tputs(": shell builtin\n");
      return;
    }
  char path[256];
  strcpy(path, "/bin/");
  strcat(path, argv[1]);
  struct statinfo st;
  if (stat(path, &st) == 0 && !st.is_dir) {
    tputs(path);
    tputc('\n');
    return;
  }
  err(argv[1], " not found");
}

/* control builtins: act via /proc + /sys, no kernel-shell bridge */

static void bi_loadkeys(int argc, char **argv)
{
  if (argc < 2) {
    tputs("usage: loadkeys <us|fr>\n");
    return;
  }
  int rc = writefile("/sys/class/input/kbd0/layout", argv[1], strlen(argv[1]));
  if (rc == -EACCES)
    err("loadkeys: permission denied", 0);
  else if (rc < 0)
    err("loadkeys: unknown layout (try us or fr)", 0);
  else {
    tputs("keyboard layout: ");
    tputs(argv[1]);
    tputc('\n');
  }
}

static void bi_clip(int argc, char **argv)
{
  if (argc < 2) { /* no text: print the clipboard */
    char mime[64];
    void *data;
    int len;
    if (clip_get(0, mime, sizeof mime, &data, &len) == 0) {
      if (has_prefix(mime, "text/")) {
        for (int i = 0; i < len; i++)
          tputc(((char *)data)[i]);
        if (len == 0 || ((char *)data)[len - 1] != '\n')
          tputc('\n');
      } else {
        tputs("[");
        tputs(mime);
        tputc(']');
        tputc('\n');
      }
      free(data);
    } else
      tputc('\n');
    return;
  }
  int len = 0; /* join argv[1..] with spaces */
  for (int k = 1; k < argc; k++) {
    for (const char *a = argv[k]; *a && len < (int)sizeof iobuf - 1; a++)
      iobuf[len++] = *a;
    if (k < argc - 1 && len < (int)sizeof iobuf - 1)
      iobuf[len++] = ' ';
  }
  if (clip_put_text(iobuf, len) < 0)
    err("clip: failed", 0);
}

/* Default mount point for a device: /media/<dev> */
static void media_point(const char *dev, char *out)
{
  strcpy(out, "/media/");
  strcat(out, dev);
}

static void bi_mount(int argc, char **argv)
{
  if (argc > 1) { /* mount <dev> [point] */
    char point[64];
    if (argc > 2)
      strcpy(point, argv[2]);
    else
      media_point(argv[1], point);
    if (mount(argv[1], point) == 0) {
      tputs("mounted ");
      tputs(argv[1]);
      tputs(" at ");
      tputs(point);
      tputc('\n');
    } else
      err("mount: cannot mount ", argv[1]);
    return;
  }
  struct volinfo v; /* no arg: mount first unmounted */
  for (int i = 0; volumes(i, &v) == 1; i++) {
    if (!v.mounted) {
      char point[64];
      media_point(v.dev, point);
      if (mount(v.dev, point) == 0) {
        tputs("mounted ");
        tputs(v.label);
        tputs(" at ");
        tputs(point);
        tputc('\n');
      } else
        err("mount: failed to mount ", v.dev);
      return;
    }
  }
  tputs("mount: no unmounted device (see `ls /dev`)\n");
}

static void bi_umount(int argc, char **argv)
{
  if (argc > 1) { /* umount <point> */
    if (umount(argv[1]) == 0) {
      tputs("unmounted ");
      tputs(argv[1]);
      tputc('\n');
    } else
      err("umount: nothing mounted at ", argv[1]);
    return;
  }
  struct volinfo v; /* no arg: unmount first mounted */
  for (int i = 0; volumes(i, &v) == 1; i++)
    if (v.mounted) {
      if (umount(v.point) == 0) {
        tputs("unmounted ");
        tputs(v.point);
        tputc('\n');
      } else
        err("umount: failed", 0);
      return;
    }
  tputs("umount: nothing mounted\n");
}

static void bi_services(int argc, char **argv)
{
  if (argc < 2 || !strcmp(argv[1], "list") || !strcmp(argv[1], "status")) {
    cat_proc("services");
    return;
  }
  char req[128]; /* "start X" / "stop X" /  */
  int len = 0;
  for (int k = 1; k < argc; k++) {
    for (const char *a = argv[k]; *a && len < (int)sizeof req - 1; a++)
      req[len++] = *a;
    if (k < argc - 1 && len < (int)sizeof req - 1)
      req[len++] = ' ';
  }
  req[len] = '\0';
  if (writefile("/proc/services", req, len) < 0)
    err("services: failed", 0);
}

/* file codecs: zip/unzip (LZSS) + gzip/gunzip (DEFLATE) */

static unsigned char cbuf[96 * 1024]; /* codec output (input is iobuf) */

/* zip/gzip: read src, compress into cbuf, write dst (default = src + suffix) */
static void do_compress(int argc, char **argv, const char *suffix, const char *cmd,
                        int (*comp)(const uint8_t *, uint32_t, uint8_t *, uint32_t), uint32_t (*bound)(uint32_t))
{
  if (argc < 2) {
    tputs("usage: ");
    tputs(cmd);
    tputs(" <src> [dst]\n");
    return;
  }
  char sp[256];
  abspath(argv[1], sp);
  int n = readfile(sp, iobuf, sizeof iobuf);
  if (n < 0) {
    err(cmd, n == -EACCES ? ": permission denied" : ": no such file");
    return;
  }
  if (bound((uint32_t)n) > sizeof cbuf) {
    err(cmd, ": file too large");
    return;
  }
  int clen = comp((const uint8_t *)iobuf, (uint32_t)n, cbuf, sizeof cbuf);
  if (clen < 0) {
    err(cmd, ": compression failed");
    return;
  }
  char dp[256];
  if (argc > 2)
    abspath(argv[2], dp);
  else {
    abspath(argv[1], dp);
    strcat(dp, suffix);
  }
  int wr = writefile(dp, cbuf, clen);
  if (wr < 0) {
    err(cmd, wr == -EACCES ? ": permission denied" : ": write failed");
    return;
  }
  tputs(argv[1]);
  tputs(" -> ");
  tputs(argc > 2 ? argv[2] : "");
  if (argc <= 2) {
    tputs(argv[1]);
    tputs(suffix);
  }
  tputs(" (");
  tdec((unsigned)n);
  tputs(" -> ");
  tdec((unsigned)clen);
  tputs(" bytes)\n");
}

/* unzip/gunzip: read src, decompress into cbuf, write dst (default = src minus suffix) */
static void do_decompress(int argc, char **argv, const char *suffix, const char *cmd,
                          int (*dec)(const uint8_t *, uint32_t, uint8_t *, uint32_t),
                          int (*orig)(const uint8_t *, uint32_t))
{
  if (argc < 2) {
    tputs("usage: ");
    tputs(cmd);
    tputs(" <src> [dst]\n");
    return;
  }
  char sp[256];
  abspath(argv[1], sp);
  int n = readfile(sp, iobuf, sizeof iobuf);
  if (n < 0) {
    err(cmd, n == -EACCES ? ": permission denied" : ": no such file");
    return;
  }
  int o = orig((const uint8_t *)iobuf, (uint32_t)n);
  if (o < 0 || (unsigned)o > sizeof cbuf) {
    err(cmd, ": not a valid archive");
    return;
  }
  int dlen = dec((const uint8_t *)iobuf, (uint32_t)n, cbuf, (uint32_t)o);
  if (dlen < 0) {
    err(cmd, ": corrupt archive");
    return;
  }
  char dp[256];
  if (argc > 2) {
    abspath(argv[2], dp);
  } else { /* strip the suffix from src */
    int sl = strlen(argv[1]), xl = strlen(suffix);
    if (sl <= xl || strcmp(argv[1] + sl - xl, suffix) != 0) {
      tputs(cmd);
      tputs(": pass an explicit dst\n");
      return;
    }
    char base[256];
    strcpy(base, argv[1]);
    base[sl - xl] = '\0';
    abspath(base, dp);
  }
  int wr = writefile(dp, cbuf, dlen);
  if (wr < 0) {
    err(cmd, wr == -EACCES ? ": permission denied" : ": write failed");
    return;
  }
  tputs(argv[1]);
  tputs(" -> ");
  tputs(argc > 2 ? argv[2] : "(decompressed)");
  tputs(" (");
  tdec((unsigned)dlen);
  tputs(" bytes)\n");
}

/* grep <pattern> <file>: print lines of the file that contain the pattern */
static void bi_grep(int argc, char **argv)
{
  if (argc < 3) {
    tputs("usage: grep <pattern> <file>\n");
    return;
  }
  char p[256];
  abspath(argv[2], p);
  int n = readfile(p, iobuf, sizeof iobuf);
  if (n < 0) {
    errc(n, "grep", "cannot read ", argv[2]);
    return;
  }
  const char *pat = argv[1];
  int i = 0;
  while (i < n) {
    int ls = i;
    while (i < n && iobuf[i] != '\n')
      i++;
    iobuf[i < n ? i : n - 1] = '\0'; /* terminate the line for strstr */
    int hit = 0;
    for (const char *s = &iobuf[ls]; *s; s++) { /* substring search */
      const char *a = s, *b = pat;
      while (*a && *b && *a == *b) {
        a++;
        b++;
      }
      if (!*b) {
        hit = 1;
        break;
      }
    }
    if (hit) {
      tputs(&iobuf[ls]);
      tputc('\n');
    }
    i++;
  }
}

/* /bin programs that drive an interactive session (read keystrokes, not just emit output) */
static bool is_interactive(const char *name) { return !strcmp(name, "ssh"); }

/* Spawn a /bin CLI tool with its stdout wired to a pipe and render the output as it streams in */
/* Backgrounded windowed children */
#define BG_MAX 16
static struct {
  int pid, rfd;
  bool active;
} bg_jobs[BG_MAX];

static void bg_add(int pid, int rfd)
{
  for (int i = 0; i < BG_MAX; i++)
    if (!bg_jobs[i].active) {
      bg_jobs[i].pid = pid;
      bg_jobs[i].rfd = rfd;
      bg_jobs[i].active = true;
      return;
    }
  close(rfd); /* table full: drop the pipe */
}

static void bg_service(void)
{
  char drain[512];
  for (int i = 0; i < BG_MAX; i++) {
    if (!bg_jobs[i].active)
      continue;
    if (bg_jobs[i].rfd >= 0) {
      int n;
      do {
        n = read_nb(bg_jobs[i].rfd, drain, sizeof drain);
      } while (n > 0);
      if (n < 0) {
        close(bg_jobs[i].rfd);
        bg_jobs[i].rfd = -1;
      } /* child closed stdout */
    }
    if (bg_jobs[i].rfd < 0 && waitpid_nb(bg_jobs[i].pid, 0))
      bg_jobs[i].active = false;
  }
}

static void render_child(const char *path, char **argv, int argc, int interactive)
{
  int rfd = -1, wfd = -1;
  /* Headless (SSH): no window */
  if (headless) {
    int pid = interactive ? spawn_cap_io(path, argv, argc, &rfd, &wfd) : spawn_cap(path, argv, argc, &rfd);
    if (pid < 0) {
      err(pid == -EACCES ? "permission denied: " : "failed to launch ", path);
      return;
    }
    for (;;) {
      char k;
      int kn = read_nb(0, &k, 1);
      if (kn > 0) {
        if ((unsigned char)k == 0x03) {
          kill(pid, SIGINT);
        } /* ^C */
        else if (wfd >= 0)
          write(wfd, &k, 1); /* forward to child */
      }
      int n = read_nb(rfd, iobuf, sizeof iobuf);
      if (n > 0)
        for (int i = 0; i < n; i++)
          tputc(iobuf[i]);
      else if (n < 0)
        break;
      else if (kn <= 0)
        msleep(4);
    }
    if (wfd >= 0)
      close(wfd);
    close(rfd);
    waitpid(pid);
    return;
  }
  int pid = interactive ? spawn_cap_io(path, argv, argc, &rfd, &wfd) : spawn_cap(path, argv, argc, &rfd);
  if (pid < 0) {
    err(pid == -EACCES ? "permission denied: " : "failed to launch ", path);
    return;
  }
  int closing = 0, intr = 0;
  for (;;) {
    struct win_event ev;
    while (win_poll(win, &ev)) { /* stay responsive mid-command */
      if (ev.type == WEV_CLOSE && !closing) {
        closing = 1;
        kill(pid, SIGKILL);
      } else if (ev.type == WEV_KEY && interactive && !closing) {
        char b = (char)ev.a; /* forward the keystroke raw */
        if (wfd >= 0)
          write(wfd, &b, 1);
      } else if (ev.type == WEV_KEY && (unsigned char)ev.a == KEY_INTR && !intr && !closing) {
        intr = 1;
        kill(pid, SIGINT);
        tputs("^C\n");
        redraw(); /* Ctrl+C: abort */
      }
    }
    /* A windowed app (opened its own window): hand it off and return to the prompt */
    if (!interactive && !closing && !intr && win_owned(pid)) {
      bg_add(pid, rfd);
      return;
    }
    int n = read_nb(rfd, iobuf, sizeof iobuf);
    if (n > 0) {
      if (!closing && !intr) {
        for (int i = 0; i < n; i++)
          tputc(iobuf[i]);
        redraw();
      }
    } else if (n < 0) {
      break; /* child closed stdout / exited */
    } else {
      if (dirty && !closing)
        redraw(); /* running; nothing yet */
      msleep(interactive ? 4 : 8);
    }
  }
  if (wfd >= 0)
    close(wfd);
  close(rfd);
  waitpid(pid); /* reap the zombie */
  if (closing) {
    win_close(win);
    exit(0);
  }
  /* intr: fall through -- run()'s caller prints a fresh prompt */
}

/* multi-user: account & group management */

static char pbuf[8192]; /* working copy of /etc/passwd */
static char gbuf[8192]; /* working copy of /etc/group */

/* Load a line file into buf (NUL-terminated) */
static int load_file(const char *path, char *buf, int max)
{
  int n = readfile(path, buf, max - 1);
  if (n < 0) {
    buf[0] = '\0';
    return -1;
  }
  buf[n] = '\0';
  return n;
}

static int save_file(const char *path, const char *buf) { return writefile(path, buf, strlen(buf)); }

/* /etc/passwd into pbuf; if the caller may not read it, only their own line (from /proc/user) */
static void load_passwd(void)
{
  if (load_file("/etc/passwd", pbuf, sizeof pbuf) >= 0)
    return;
  char name[32], hm[80];
  int uid = getuid(), gid = getgid();
  if (!user_ident(name, sizeof name, hm, sizeof hm) || gid < 0) {
    pbuf[0] = '\0';
    return;
  }
  snprintf(pbuf, sizeof pbuf, "%s::%d:%d:%s\n", name, uid, gid, hm);
}

/* Unsigned int -> decimal string */
static void u2s(unsigned v, char *out)
{
  char t[12];
  int n = 0;
  if (!v) {
    out[0] = '0';
    out[1] = '\0';
    return;
  }
  while (v) {
    t[n++] = (char)('0' + v % 10);
    v /= 10;
  }
  int i = 0;
  while (n)
    out[i++] = t[--n];
  out[i] = '\0';
}

/* Copy the idx-th ':'-delimited field of the line at `line` into out[max] */
static int line_field(const char *line, int idx, char *out, int max)
{
  const char *p = line;
  for (int f = 0; f < idx; f++) {
    while (*p && *p != ':' && *p != '\n')
      p++;
    if (*p != ':') {
      out[0] = '\0';
      return 0;
    }
    p++;
  }
  int j = 0;
  while (*p && *p != ':' && *p != '\n' && *p != '\r' && j < max - 1)
    out[j++] = *p++;
  out[j] = '\0';
  return 1;
}

/* Pointer to the line in `buf` whose field 0 equals `name`, or NULL */
static char *find_line(char *buf, const char *name)
{
  for (char *line = buf; *line;) {
    char nm[64];
    line_field(line, 0, nm, sizeof nm);
    if (strcmp(nm, name) == 0)
      return line;
    while (*line && *line != '\n')
      line++;
    if (*line == '\n')
      line++;
  }
  return 0;
}

/* Delete the line beginning at `p` from `buf` (shift the tail down over it) */
static void remove_line(char *buf, char *p)
{
  (void)buf;
  char *e = p;
  while (*e && *e != '\n')
    e++;
  if (*e == '\n')
    e++;
  int tail = strlen(e) + 1; /* include the NUL */
  for (int i = 0; i < tail; i++)
    p[i] = e[i];
}

/* Smallest id >= start in column `idx` that no line uses */
static unsigned next_free_id(const char *buf, int idx, unsigned start)
{
  for (unsigned c = start;; c++) {
    int taken = 0;
    for (const char *line = buf; *line;) {
      char f[16];
      line_field(line, idx, f, sizeof f);
      if (to_uint(f) == c) {
        taken = 1;
        break;
      }
      while (*line && *line != '\n')
        line++;
      if (*line == '\n')
        line++;
    }
    if (!taken)
      return c;
  }
}

/* Lowercase-hex SHA-256 of `pass` into out[65] */
static void pw_hash(const char *pass, char out[65])
{
  unsigned char dg[SHA256_DIGEST];
  sha256(pass, (unsigned)strlen(pass), dg);
  static const char hx[] = "0123456789abcdef";
  for (int i = 0; i < SHA256_DIGEST; i++) {
    out[i * 2] = hx[(dg[i] >> 4) & 0xF];
    out[i * 2 + 1] = hx[dg[i] & 0xF];
  }
  out[64] = '\0';
}

static int require_root(const char *cmd)
{
  if (getuid() == 0)
    return 1;
  err(cmd, ": permission denied (must be root)");
  return 0;
}

/* A legal account/group name: [A-Za-z_][A-Za-z0-9_-]*, <= 16 chars */
static bool valid_name(const char *s)
{
  if (!s || !s[0])
    return false;
  int n = 0;
  for (const char *p = s; *p; p++, n++) {
    char c = *p;
    bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    bool rest = alpha || (c >= '0' && c <= '9') || c == '-';
    if (n == 0 ? !alpha : !rest)
      return false;
  }
  return n <= 16;
}

/* Does comma-separated list `csv` contain the exact token `tok`? */
static int csv_has(const char *csv, const char *tok)
{
  const char *p = csv;
  while (*p) {
    const char *s = p;
    while (*p && *p != ',')
      p++;
    int len = (int)(p - s), tl = strlen(tok);
    if (len == tl) {
      int eq = 1;
      for (int i = 0; i < len; i++)
        if (s[i] != tok[i]) {
          eq = 0;
          break;
        }
      if (eq)
        return 1;
    }
    if (*p == ',')
      p++;
  }
  return 0;
}

/* Group name whose gid field == gid, into out[max] ("" if none) */
static void group_name_for_gid(char *groups, unsigned gid, char *out, int max)
{
  out[0] = '\0';
  for (char *line = groups; *line;) {
    char gn[64], gf[16];
    line_field(line, 0, gn, sizeof gn);
    line_field(line, 1, gf, sizeof gf);
    if (to_uint(gf) == gid) {
      int i = 0;
      for (; gn[i] && i < max - 1; i++)
        out[i] = gn[i];
      out[i] = '\0';
      return;
    }
    while (*line && *line != '\n')
      line++;
    if (*line == '\n')
      line++;
  }
}

/* account commands (each is its own builtin) */

static void cmd_useradd(int argc, char **argv)
{
  if (argc < 2) {
    err("usage: useradd <name>", 0);
    return;
  }
  if (!require_root("useradd"))
    return;
  const char *name = argv[1];
  if (!valid_name(name)) {
    err("useradd: invalid name: ", name);
    return;
  }

  load_file("/etc/passwd", pbuf, sizeof pbuf);
  if (find_line(pbuf, name)) {
    err("useradd: user already exists: ", name);
    return;
  }

  unsigned uid = next_free_id(pbuf, 2, 1000);
  unsigned gid = uid; /* user-private group (same id) */
  char ub[12], gb[12];
  u2s(uid, ub);
  u2s(gid, gb);

  char line[256];
  strcpy(line, name);
  strcat(line, "::"); /* empty pwhash = no password */
  strcat(line, ub);
  strcat(line, ":");
  strcat(line, gb);
  strcat(line, ":/home/");
  strcat(line, name);
  strcat(line, "\n");
  strcat(pbuf, line);
  if (save_file("/etc/passwd", pbuf) < 0) {
    err("useradd: cannot write /etc/passwd", 0);
    return;
  }

  /* The matching primary group */
  load_file("/etc/group", gbuf, sizeof gbuf);
  if (!find_line(gbuf, name)) {
    char gl[128];
    strcpy(gl, name);
    strcat(gl, ":");
    strcat(gl, gb);
    strcat(gl, ":\n");
    strcat(gbuf, gl);
    save_file("/etc/group", gbuf);
  }

  /* The home directory */
  char hp[64];
  strcpy(hp, "/home/");
  strcat(hp, name);
  if (mkdir(hp) == 0) {
    chown(hp, (int)uid, (int)gid);
    chmod(hp, 0700);
  }

  tputs("useradd: created ");
  tputs(name);
  tputs(" (uid=");
  tputs(ub);
  tputs(", home=");
  tputs(hp);
  tputs(")\n");
}

static void cmd_userdel(int argc, char **argv)
{
  if (argc < 2) {
    err("usage: userdel <name>", 0);
    return;
  }
  if (!require_root("userdel"))
    return;
  const char *name = argv[1];
  if (strcmp(name, "root") == 0) {
    err("userdel: cannot remove root", 0);
    return;
  }
  if (strcmp(name, username) == 0) {
    err("userdel: cannot remove the current user", 0);
    return;
  }

  load_file("/etc/passwd", pbuf, sizeof pbuf);
  char *p = find_line(pbuf, name);
  if (!p) {
    err("userdel: no such user: ", name);
    return;
  }
  remove_line(pbuf, p);
  if (save_file("/etc/passwd", pbuf) < 0) {
    err("userdel: cannot write /etc/passwd", 0);
    return;
  }

  /* Drop the user's primary group and strip it from every member list */
  load_file("/etc/group", gbuf, sizeof gbuf);
  char *gp = find_line(gbuf, name);
  if (gp)
    remove_line(gbuf, gp);
  /* Rebuild each group line with `name` filtered out of its members */
  static char nb[8192];
  int o = 0;
  for (char *line = gbuf; *line;) {
    char gn[64], gf[16], mem[512];
    line_field(line, 0, gn, sizeof gn);
    line_field(line, 1, gf, sizeof gf);
    line_field(line, 2, mem, sizeof mem);
    /* name:gid: */
    for (int i = 0; gn[i]; i++)
      nb[o++] = gn[i];
    nb[o++] = ':';
    for (int i = 0; gf[i]; i++)
      nb[o++] = gf[i];
    nb[o++] = ':';
    /* filtered members */
    bool first = true;
    for (const char *q = mem; *q;) {
      const char *s = q;
      while (*q && *q != ',')
        q++;
      int len = (int)(q - s);
      char tok[64];
      int tl = len < 63 ? len : 63;
      for (int i = 0; i < tl; i++)
        tok[i] = s[i];
      tok[tl] = '\0';
      if (tok[0] && strcmp(tok, name) != 0) {
        if (!first)
          nb[o++] = ',';
        for (int i = 0; i < tl; i++)
          nb[o++] = tok[i];
        first = false;
      }
      if (*q == ',')
        q++;
    }
    nb[o++] = '\n';
    while (*line && *line != '\n')
      line++;
    if (*line == '\n')
      line++;
  }
  nb[o] = '\0';
  save_file("/etc/group", nb);

  tputs("userdel: removed ");
  tputs(name);
  tputs(" (home /home/");
  tputs(name);
  tputs(" left in place)\n");
}

static void cmd_passwd(int argc, char **argv)
{
  const char *target, *newpass;
  int clear = 0;
  if (argc >= 3 && strcmp(argv[1], "-d") == 0) {
    target = argv[2];
    newpass = "";
    clear = 1;
  } else if (argc >= 3) {
    target = argv[1];
    newpass = argv[2];
  } else if (argc == 2) {
    target = username;
    newpass = argv[1];
  } else {
    err("usage: passwd [user] <newpassword>   (passwd -d <user> clears it)", 0);
    return;
  }

  if (getuid() != 0) {
    err("passwd: permission denied (/etc/passwd is root-only)", 0);
    return;
  }

  load_file("/etc/passwd", pbuf, sizeof pbuf);
  char *p = find_line(pbuf, target);
  if (!p) {
    err("passwd: no such user: ", target);
    return;
  }

  char f0[32], f2[16], f3[16], f4[80];
  line_field(p, 0, f0, sizeof f0);
  line_field(p, 2, f2, sizeof f2);
  line_field(p, 3, f3, sizeof f3);
  line_field(p, 4, f4, sizeof f4);
  char hash[65];
  if (clear)
    hash[0] = '\0';
  else
    pw_hash(newpass, hash);

  remove_line(pbuf, p);
  char nl[256];
  strcpy(nl, f0);
  strcat(nl, ":");
  strcat(nl, hash);
  strcat(nl, ":");
  strcat(nl, f2);
  strcat(nl, ":");
  strcat(nl, f3);
  strcat(nl, ":");
  strcat(nl, f4);
  strcat(nl, "\n");
  strcat(pbuf, nl);
  if (save_file("/etc/passwd", pbuf) < 0) {
    err("passwd: cannot write /etc/passwd", 0);
    return;
  }

  tputs(clear ? "passwd: password cleared for " : "passwd: password updated for ");
  tputs(target);
  tputc('\n');
}

static void cmd_groupadd(int argc, char **argv)
{
  if (argc < 2) {
    err("usage: groupadd <name>", 0);
    return;
  }
  if (!require_root("groupadd"))
    return;
  const char *name = argv[1];
  if (!valid_name(name)) {
    err("groupadd: invalid name: ", name);
    return;
  }

  load_file("/etc/group", gbuf, sizeof gbuf);
  if (find_line(gbuf, name)) {
    err("groupadd: group already exists: ", name);
    return;
  }

  unsigned gid = next_free_id(gbuf, 1, 100);
  char gb[12];
  u2s(gid, gb);
  char gl[128];
  strcpy(gl, name);
  strcat(gl, ":");
  strcat(gl, gb);
  strcat(gl, ":\n");
  strcat(gbuf, gl);
  if (save_file("/etc/group", gbuf) < 0) {
    err("groupadd: cannot write /etc/group", 0);
    return;
  }

  tputs("groupadd: created group ");
  tputs(name);
  tputs(" (gid=");
  tputs(gb);
  tputs(")\n");
}

static void cmd_groupdel(int argc, char **argv)
{
  if (argc < 2) {
    err("usage: groupdel <name>", 0);
    return;
  }
  if (!require_root("groupdel"))
    return;
  const char *name = argv[1];
  if (strcmp(name, "root") == 0) {
    err("groupdel: cannot remove the root group", 0);
    return;
  }

  load_file("/etc/group", gbuf, sizeof gbuf);
  char *gp = find_line(gbuf, name);
  if (!gp) {
    err("groupdel: no such group: ", name);
    return;
  }
  remove_line(gbuf, gp);
  if (save_file("/etc/group", gbuf) < 0) {
    err("groupdel: cannot write /etc/group", 0);
    return;
  }

  tputs("groupdel: removed group ");
  tputs(name);
  tputc('\n');
}

/* usermod -aG <group> <user>: add an existing user to a supplementary group */
static void cmd_usermod(int argc, char **argv)
{
  if (argc < 4 || strcmp(argv[1], "-aG") != 0) {
    err("usage: usermod -aG <group> <user>", 0);
    return;
  }
  if (!require_root("usermod"))
    return;
  const char *grp = argv[2], *usr = argv[3];

  load_file("/etc/passwd", pbuf, sizeof pbuf);
  if (!find_line(pbuf, usr)) {
    err("usermod: no such user: ", usr);
    return;
  }

  load_file("/etc/group", gbuf, sizeof gbuf);
  char *gp = find_line(gbuf, grp);
  if (!gp) {
    err("usermod: no such group: ", grp);
    return;
  }

  char f0[64], f1[16], f2[512];
  line_field(gp, 0, f0, sizeof f0);
  line_field(gp, 1, f1, sizeof f1);
  line_field(gp, 2, f2, sizeof f2);
  if (csv_has(f2, usr)) {
    tputs(usr);
    tputs(" is already in group ");
    tputs(grp);
    tputc('\n');
    return;
  }

  remove_line(gbuf, gp);
  char nl[640];
  strcpy(nl, f0);
  strcat(nl, ":");
  strcat(nl, f1);
  strcat(nl, ":");
  if (f2[0]) {
    strcat(nl, f2);
    strcat(nl, ",");
  }
  strcat(nl, usr);
  strcat(nl, "\n");
  strcat(gbuf, nl);
  if (save_file("/etc/group", gbuf) < 0) {
    err("usermod: cannot write /etc/group", 0);
    return;
  }

  tputs("usermod: added ");
  tputs(usr);
  tputs(" to group ");
  tputs(grp);
  tputc('\n');
}

/* users: list every account */
static void cmd_users(void)
{
  load_passwd();
  for (char *line = pbuf; *line;) {
    char nm[32], uid[16], hm[80];
    line_field(line, 0, nm, sizeof nm);
    line_field(line, 2, uid, sizeof uid);
    line_field(line, 4, hm, sizeof hm);
    if (nm[0]) {
      tputs_c(nm, FG_DIR);
      tputs("  uid=");
      tputs(uid);
      tputs("  ");
      tputs(hm);
      tputc('\n');
    }
    while (*line && *line != '\n')
      line++;
    if (*line == '\n')
      line++;
  }
}

/* The groups a user belongs to: its primary group plus any whose member list names it */
static void user_groups(const char *user, char *out, int max)
{
  out[0] = '\0';
  int o = 0;
  load_passwd();
  char *p = find_line(pbuf, user);
  unsigned pgid = 0;
  if (p) {
    char g[16];
    line_field(p, 3, g, sizeof g);
    pgid = to_uint(g);
  }

  load_file("/etc/group", gbuf, sizeof gbuf);
  /* primary group first */
  char primary[64];
  group_name_for_gid(gbuf, pgid, primary, sizeof primary);
  if (primary[0])
    for (int i = 0; primary[i] && o < max - 1; i++)
      out[o++] = primary[i];

  for (char *line = gbuf; *line;) {
    char gn[64], gf[16], mem[512];
    line_field(line, 0, gn, sizeof gn);
    line_field(line, 1, gf, sizeof gf);
    line_field(line, 2, mem, sizeof mem);
    if (csv_has(mem, user) && strcmp(gn, primary) != 0) {
      if (o > 0 && o < max - 1)
        out[o++] = ' ';
      for (int i = 0; gn[i] && o < max - 1; i++)
        out[o++] = gn[i];
    }
    while (*line && *line != '\n')
      line++;
    if (*line == '\n')
      line++;
  }
  out[o] = '\0';
}

static void cmd_groups(int argc, char **argv)
{
  const char *u = argc >= 2 ? argv[1] : username;
  load_passwd();
  if (!find_line(pbuf, u)) {
    err("groups: no such user: ", u);
    return;
  }
  char gl[256];
  user_groups(u, gl, sizeof gl);
  tputs(u);
  tputs(" : ");
  tputs(gl);
  tputc('\n');
}

static void cmd_id(int argc, char **argv)
{
  const char *u = argc >= 2 ? argv[1] : username;
  load_passwd();
  char *p = find_line(pbuf, u);
  if (!p) {
    err("id: no such user: ", u);
    return;
  }
  char uid[16], gid[16];
  line_field(p, 2, uid, sizeof uid);
  line_field(p, 3, gid, sizeof gid);

  load_file("/etc/group", gbuf, sizeof gbuf);
  char pgrp[64];
  group_name_for_gid(gbuf, to_uint(gid), pgrp, sizeof pgrp);
  char gl[256];
  user_groups(u, gl, sizeof gl);

  tputs("uid=");
  tputs(uid);
  tputs("(");
  tputs(u);
  tputs(") ");
  tputs("gid=");
  tputs(gid);
  tputs("(");
  tputs(pgrp[0] ? pgrp : u);
  tputs(") ");
  tputs("groups=");
  tputs(gl);
  tputc('\n');
}

/* su / login / logout: switch this shell's identity */
static void do_login(const char *name, const char *pass)
{
  char req[200];
  strcpy(req, name);
  if (pass && pass[0]) {
    strcat(req, " ");
    strcat(req, pass);
  }
  if (writefile("/proc/login", req, strlen(req)) < 0) {
    err("su: authentication failure", 0);
    return;
  }
  reload_identity();
  strcpy(cwd, home);
  tputs("now logged in as ");
  tputs_c(username, FG_PROMPT);
  tputc('\n');
}

static void cmd_su(int argc, char **argv)
{
  const char *name = argc >= 2 ? argv[1] : "root";
  const char *pass = argc >= 3 ? argv[2] : 0;
  do_login(name, pass);
}

static void cmd_login(int argc, char **argv)
{
  if (argc < 2) {
    err("usage: login <user> [password]", 0);
    return;
  }
  do_login(argv[1], argc >= 3 ? argv[2] : 0);
}

static void bi_whoami(void)
{
  tputs(username);
  tputc('\n');
}

/* hostname: print the system hostname, or (with an arg) set it via /etc/hostname */
static void bi_hostname(int argc, char **argv)
{
  if (argc > 1) {
    if (sethostname(argv[1]) < 0) {
      err("hostname: cannot write /etc/hostname", 0);
      return;
    }
    gethostname(hostnm, sizeof hostnm);
  } else {
    tputs(hostnm);
    tputc('\n');
  }
}

/* A non-builtin command: a captured CLI tool, a detached GUI app, or the kernel-shell bridge */
static void run_other(const char *orig, int argc, char **argv)
{
  char path[256];
  if (argv[0][0] == '/')
    strcpy(path, argv[0]);
  else {
    strcpy(path, "/bin/");
    strcat(path, argv[0]);
  }

  if (is_interactive(argv[0])) {
    render_child(path, argv, argc, 1);
    return;
  }

  struct statinfo st;
  if (stat(path, &st) == 0 && !st.is_dir) {
    /* Run every program the same way: captured, streaming its output inline */
    render_child(path, argv, argc, 0);
    return;
  }

  /* The shell dispatches entirely in user space -- there is no kernel-shell bridge */
  err(argv[0], ": command not found");
}

static void run(const char *orig)
{
  char buf[256];
  strcpy(buf, orig);
  char *argv[16];
  int argc = 0, i = 0;
  /* Split on whitespace, with double-quoted runs kept whole and the quotes stripped */
  while (buf[i] && argc < 16) {
    while (buf[i] == ' ')
      i++;
    if (!buf[i])
      break;
    char *w = &buf[i];
    argv[argc++] = w;
    while (buf[i] && buf[i] != ' ') {
      if (buf[i] == '"') {
        i++;
        while (buf[i] && buf[i] != '"')
          *w++ = buf[i++];
        if (buf[i] == '"')
          i++;
      } else {
        *w++ = buf[i++];
      }
    }
    if (buf[i])
      i++;
    *w = '\0';
  }
  if (argc == 0)
    return;
  const char *c = argv[0];

  if (!strcmp(c, "help"))
    bi_help();
  else if (!strcmp(c, "clear"))
    grid_reset();
  else if (!strcmp(c, "echo")) {
    for (int k = 1; k < argc; k++) {
      tputs(argv[k]);
      if (k < argc - 1)
        tputc(' ');
    }
    tputc('\n');
  } else if (!strcmp(c, "pwd")) {
    tputs(cwd);
    tputc('\n');
  } else if (!strcmp(c, "cd")) {
    if (argc < 2) {
      strcpy(cwd, home);
    } else {
      char p[256];
      abspath(argv[1], p);
      struct statinfo st;
      int rc = stat(p, &st);
      if (rc == 0 && st.is_dir && (rc = path_access(p, X_OK)) == 0)
        strcpy(cwd, p);
      else
        errc(rc, "cd", "not a directory: ", argv[1]);
    }
  } else if (!strcmp(c, "ls"))
    bi_ls(argc, argv);
  else if (!strcmp(c, "cat"))
    bi_cat(argc, argv);
  else if (!strcmp(c, "cp"))
    bi_cp(argc, argv);
  else if (!strcmp(c, "stat"))
    bi_stat(argc, argv);
  else if (!strcmp(c, "umask"))
    bi_umask(argc, argv);
  else if (!strcmp(c, "mkdir")) {
    for (int k = 1; k < argc; k++) {
      char p[256];
      abspath(argv[k], p);
      int rc = mkdir(p);
      if (rc < 0)
        errc(rc, "mkdir", "failed: ", argv[k]);
    }
  } else if (!strcmp(c, "rmdir")) {
    for (int k = 1; k < argc; k++) {
      char p[256];
      abspath(argv[k], p);
      int rc = rmdir(p);
      if (rc < 0)
        errc(rc, "rmdir", "failed: ", argv[k]);
    }
  } else if (!strcmp(c, "rm")) {
    for (int k = 1; k < argc; k++) {
      char p[256];
      abspath(argv[k], p);
      int rc = unlink(p);
      if (rc < 0)
        errc(rc, "rm", "failed: ", argv[k]);
    }
  } else if (!strcmp(c, "mv")) {
    if (argc < 3)
      tputs("usage: mv <src> <dst>\n");
    else {
      char a[256], b[256];
      abspath(argv[1], a);
      abspath(argv[2], b);
      int rc = rename(a, b);
      if (rc < 0)
        errc(rc, "mv", "failed: ", argv[1]);
    }
  } else if (!strcmp(c, "exit")) {
    win_close(win);
    exit(0);
  } else if (!strcmp(c, "uptime"))
    bi_uptime();
  else if (!strcmp(c, "whoami"))
    bi_whoami();
  else if (!strcmp(c, "hostname"))
    bi_hostname(argc, argv);
  else if (!strcmp(c, "useradd"))
    cmd_useradd(argc, argv);
  else if (!strcmp(c, "userdel"))
    cmd_userdel(argc, argv);
  else if (!strcmp(c, "passwd"))
    cmd_passwd(argc, argv);
  else if (!strcmp(c, "groupadd"))
    cmd_groupadd(argc, argv);
  else if (!strcmp(c, "groupdel"))
    cmd_groupdel(argc, argv);
  else if (!strcmp(c, "usermod"))
    cmd_usermod(argc, argv);
  else if (!strcmp(c, "users"))
    cmd_users();
  else if (!strcmp(c, "groups"))
    cmd_groups(argc, argv);
  else if (!strcmp(c, "id"))
    cmd_id(argc, argv);
  else if (!strcmp(c, "su"))
    cmd_su(argc, argv);
  else if (!strcmp(c, "login"))
    cmd_login(argc, argv);
  else if (!strcmp(c, "logout"))
    do_login("root", 0);
  else if (!strcmp(c, "which"))
    bi_which(argc, argv);
  else if (!strcmp(c, "meminfo"))
    cat_proc("meminfo");
  else if (!strcmp(c, "cpuinfo"))
    cat_proc("cpuinfo");
  else if (!strcmp(c, "date"))
    cat_proc("date");
  else if (!strcmp(c, "storage")) {
    if (!cat_proc("storage"))
      tputs("no storage devices\n");
  } else if (!strcmp(c, "sleep")) {
    if (argc > 1)
      msleep(to_uint(argv[1]) * 1000);
  } else if (!strcmp(c, "journal"))
    cat_proc("journal");
  else if (!strcmp(c, "poweroff"))
    power(0);
  else if (!strcmp(c, "restart"))
    power(1);
  else if (!strcmp(c, "loadkeys"))
    bi_loadkeys(argc, argv);
  else if (!strcmp(c, "clip"))
    bi_clip(argc, argv);
  else if (!strcmp(c, "mount"))
    bi_mount(argc, argv);
  else if (!strcmp(c, "umount"))
    bi_umount(argc, argv);
  else if (!strcmp(c, "services"))
    bi_services(argc, argv);
  else if (!strcmp(c, "lsusb"))
    cat_proc("usb");
  else if (!strcmp(c, "ps"))
    cat_proc("tasks");
  else if (!strcmp(c, "grep"))
    bi_grep(argc, argv);
  else if (!strcmp(c, "zip"))
    do_compress(argc, argv, ".z", "zip", zip_compress, zip_bound);
  else if (!strcmp(c, "unzip"))
    do_decompress(argc, argv, ".z", "unzip", zip_decompress, zip_orig_size);
  else if (!strcmp(c, "gzip"))
    do_compress(argc, argv, ".gz", "gzip", gzip_compress, gzip_bound);
  else if (!strcmp(c, "gunzip"))
    do_decompress(argc, argv, ".gz", "gunzip", gzip_decompress, gzip_orig_size);
  else
    run_other(orig, argc, argv);
}

/* line editor */

static char line[256];
static int llen, lpos, rlen;
static char hist[16][256];
static int hcount, hpos;

static void caret_to(int off)
{
  int tcol = ls_col + off, tline = ls_line;
  while (tcol >= cols) {
    tcol -= cols;
    tline++;
  }
  cc = tcol;
  cl = tline;
}

static void line_redraw(void)
{
  cl = ls_line;
  cc = ls_col;
  for (int i = 0; i < llen; i++)
    tputc(line[i]);
  for (int i = llen; i < rlen; i++)
    tputc(' ');
  rlen = llen;
  caret_to(lpos);
  dirty = true;
}

static void line_set(const char *s)
{
  strcpy(line, s);
  llen = lpos = strlen(line);
  line_redraw();
}

static void line_submit(void)
{
  line[llen] = '\0';
  cl = ls_line;
  cc = ls_col;
  for (int i = 0; i < llen; i++)
    tputc(line[i]);
  tputc('\n');
  if (llen) {
    for (int k = 15; k > 0; k--)
      strcpy(hist[k], hist[k - 1]);
    strcpy(hist[0], line);
    if (hcount < 16)
      hcount++;
  }
  char tmp[256];
  strcpy(tmp, line);
  llen = lpos = rlen = 0;
  hpos = -1;
  redraw(); /* flush the echoed line + newline now */
  run(tmp);
  cur_fg = FG;
  esc = 0; /* don't let a command's colour bleed */
  prompt();
}

static void on_key(char c)
{
  switch ((unsigned char)c) {
  case '\n':
  case '\r':
    line_submit();
    break;
  case KEY_INTR: /* Ctrl+C: abandon the line */
    cl = ls_line;
    cc = ls_col;
    for (int i = 0; i < llen; i++)
      tputc(line[i]);
    tputs("^C\n");
    llen = lpos = rlen = 0;
    hpos = -1;
    prompt();
    break;
  case '\b':
    if (lpos > 0) {
      for (int i = lpos - 1; i < llen - 1; i++)
        line[i] = line[i + 1];
      llen--;
      lpos--;
      line_redraw();
    }
    break;
  case KEY_LEFT:
    if (lpos > 0) {
      lpos--;
      line_redraw();
    }
    break;
  case KEY_RIGHT:
    if (lpos < llen) {
      lpos++;
      line_redraw();
    }
    break;
  case KEY_HOME:
  case KEY_SELALL:
    lpos = 0;
    line_redraw();
    break; /* Home / Ctrl+A */
  case KEY_END:
    lpos = llen;
    line_redraw();
    break;
  case KEY_DELETE:
    if (lpos < llen) {
      for (int i = lpos; i < llen - 1; i++)
        line[i] = line[i + 1];
      llen--;
      line_redraw();
    }
    break;
  case KEY_UP:
    if (hcount && hpos + 1 < hcount) {
      hpos++;
      line_set(hist[hpos]);
    }
    break;
  case KEY_DOWN:
    if (hpos > 0) {
      hpos--;
      line_set(hist[hpos]);
    } else if (hpos == 0) {
      hpos = -1;
      line_set("");
    }
    break;
  default:
    if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7f && llen < (int)sizeof line - 1) {
      for (int i = llen; i > lpos; i--)
        line[i] = line[i - 1];
      line[lpos++] = c;
      llen++;
      line_redraw();
    }
    break;
  }
}

static void on_scroll(int dz)
{
  int maxoff = nlines > rows ? nlines - rows : 0;
  view -= dz * 3; /* wheel up scrolls back to older lines */
  if (view < 0)
    view = 0;
  if (view > maxoff)
    view = maxoff;
  dirty = true;
}

/* The scrollbar widget works in top-line units (`scroll`, 0..bottom) */
static int sb_drag; /* 1 while the thumb is held */

static void sb_set(int scroll)
{
  int maxoff = nlines > rows ? nlines - rows : 0;
  view = maxoff - scroll;
  if (view < 0)
    view = 0;
  if (view > maxoff)
    view = maxoff;
  dirty = true;
}

/* Left-click on the bar: arrows step a line */
static void on_click(int mx, int my)
{
  sb_drag = 0;
  int maxoff = nlines > rows ? nlines - rows : 0;
  if (maxoff <= 0)
    return; /* nothing to scroll */
  int sbx = winw - GUI_SCROLL_W;
  int part = scrollbar_hit(sbx, 0, GUI_SCROLL_W, winh, mx, my);
  int scroll = maxoff - view;
  if (part == SCROLLBAR_UP)
    sb_set(scroll - 3);
  else if (part == SCROLLBAR_DOWN)
    sb_set(scroll + 3);
  else if (part == SCROLLBAR_TRACK) {
    sb_drag = 1;
    sb_set(scrollbar_pos(sbx, 0, GUI_SCROLL_W, winh, mx, my, scroll, nlines, rows));
  }
}

/* Drag while the thumb is held: track the cursor's y */
static void on_drag(int mx, int my)
{
  if (!sb_drag)
    return;
  int maxoff = nlines > rows ? nlines - rows : 0;
  int sbx = winw - GUI_SCROLL_W;
  sb_set(scrollbar_pos(sbx, 0, GUI_SCROLL_W, winh, sbx + GUI_SCROLL_W / 2, my, maxoff - view, nlines, rows));
}

static void on_resize(int w, int h)
{
  gfx_init(win_fb(), w, h);
  winw = w;
  winh = h;
  cols = (w - GUI_SCROLL_W) / FONT_W; /* reserve the right edge for the bar */
  if (cols > MAXCOLS)
    cols = MAXCOLS;
  if (cols < 1)
    cols = 1;
  rows = h / FONT_H;
  if (rows < 1)
    rows = 1;
  dirty = true;
}

/* Headless stdio REPL (term -stdio): the shell over stdin/stdout instead of a window */
static void stdio_repl(void)
{
  headless = 1;
  reload_identity();
  gethostname(hostnm, sizeof hostnm);
  strcpy(cwd, home);
  tputs("OwOS -- type 'help'.\n");
  prompt();

  char buf[256];
  int len = 0;
  for (;;) {
    char c;
    if (read(0, &c, 1) <= 0)
      break; /* EOF: the session closed */
    if (c == '\r' || c == '\n') {
      write(1, "\r\n", 2);
      buf[len] = '\0';
      len = 0;
      run(buf);
      prompt();
    } else if (c == '\b' || c == 0x7f) {
      if (len > 0) {
        len--;
        write(1, "\b \b", 3);
      }
    } else if ((unsigned char)c == 0x03) { /* Ctrl+C: cancel the line */
      write(1, "^C\r\n", 4);
      len = 0;
      prompt();
    } else if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7f) {
      if (len < (int)sizeof buf - 1) {
        buf[len++] = c;
        write(1, &c, 1);
      }
    }
    /* other control/escape bytes (arrows, etc.) are ignored for now */
  }
}

int main(int argc, char **argv)
{
  if (argc > 1 && argv[1] && streq(argv[1], "-stdio")) {
    stdio_repl();
    return 0;
  }

  win = win_create(720, 432, "terminal");
  if (win < 0) {
    puts("term: no window\n");
    return 1;
  }
  gfx_init(win_fb(), 720, 432);
  win_resizable(win, 1);
  winw = 720;
  winh = 432;
  cols = (720 - GUI_SCROLL_W) / FONT_W;
  rows = 432 / FONT_H;

  reload_identity();                  /* pick up our launch user (root) + its home */
  gethostname(hostnm, sizeof hostnm); /* system hostname for the prompt */
  strcpy(cwd, home);
  grid_reset();
  tputs_c("OwOS terminal -- type 'help'.\n", FG);
  prompt();

  for (;;) {
    struct win_event ev;
    while (win_poll(win, &ev)) {
      switch (ev.type) {
      case WEV_CLOSE:
        win_close(win);
        return 0;
      case WEV_KEY:
        on_key((char)ev.a);
        break;
      case WEV_SCROLL:
        on_scroll(ev.a);
        break;
      case WEV_MOUSE:
        on_click(ev.a, ev.b);
        break;
      case WEV_DRAG:
        on_drag(ev.a, ev.b);
        break;
      case WEV_RESIZE:
        on_resize(ev.a, ev.b);
        break;
      }
    }
    bg_service(); /* drain + reap handed-off windowed children */
    if (dirty)
      redraw();
    win_wait(win);
  }
}
