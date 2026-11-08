/* shell.c, line editing and a handful of built-in commands. */

#include "shell.h"
#include "ahci.h"
#include "ata.h"
#include "blk.h"
#include "console.h"
#include "cpu.h"
#include "e1000.h"
#include "ext.h"
#include "fat.h"
#include "fs.h"
#include "hda.h"
#include "heap.h"
#include "hfs.h"
#include "iso9660.h"
#include "keyboard.h"
#include "kstring.h"
#include "loader.h"
#include "log.h"
#include "net.h"
#include "nvme.h"
#include "pci.h"
#include "pipe.h"
#include "pmm.h"
#include "power.h"
#include "rtc.h"
#include "sb16.h"
#include "sched.h"
#include "serial.h"
#include "timer.h"
#include "usb.h"
#include "vmtools.h"
#include "xhci.h"
#include <stddef.h>
#include <stdint.h>

#define LINE_MAX 16384
#define HIST_MAX 64

static char line[LINE_MAX];
static size_t line_len;     /* characters currently in line */
static size_t cursor;       /* insertion point, 0..line_len */
static uint16_t edit_start; /* VGA linear pos where the line text begins */
static size_t render_len;   /* chars currently drawn for this line */

/* Command history ring (Up/Down); hist_pos == hist_count means the line being typed. */
static char history[HIST_MAX][LINE_MAX];
static int hist_count;
static int hist_pos;

/* Captured output of A in `A | B` as a NUL-terminated string, read by input-consuming builtins (grep). */
static char *g_pipe_in;

static void str_copy(char *dst, const char *src)
{
  size_t i = 0;
  for (; src[i] && i < LINE_MAX - 1; i++)
    dst[i] = src[i];
  dst[i] = '\0';
}

/* working directory */

#define PATH_MAX 128

/* Shell cwd as a clean absolute path; owofs has no cwd, so file args are made absolute first. */
#define HOME_DIR "/home/root"
static char cwd[PATH_MAX] = HOME_DIR;

/* Resolve `arg` against the cwd into a normalized absolute path in out[PATH_MAX]; 0, or -1 on overflow. */
static int path_resolve(const char *arg, char *out)
{
  char tmp[PATH_MAX * 2];
  int n = 0;

  if (arg[0] == '/') { /* absolute: start from root */
    tmp[n++] = '/';
  } else { /* relative: seed with cwd */
    for (int i = 0; cwd[i]; i++) {
      if (n >= (int)sizeof(tmp) - 2)
        return -1;
      tmp[n++] = cwd[i];
    }
    if (n == 0 || tmp[n - 1] != '/')
      tmp[n++] = '/';
  }
  for (int i = 0; arg[i]; i++) { /* append the argument */
    if (n >= (int)sizeof(tmp) - 2)
      return -1;
    tmp[n++] = arg[i];
  }
  tmp[n] = '\0';

  /* Normalize: walk components onto a stack of (offset,len) into out[]. */
  int starts[PATH_MAX / 2];
  int ends[PATH_MAX / 2];
  int depth = 0;
  int i = 0;
  while (tmp[i]) {
    while (tmp[i] == '/')
      i++;
    int s = i;
    while (tmp[i] && tmp[i] != '/')
      i++;
    int len = i - s;
    if (len == 0)
      continue;
    if (len == 1 && tmp[s] == '.')
      continue; /* "." : stay */
    if (len == 2 && tmp[s] == '.' && tmp[s + 1] == '.') {
      if (depth > 0)
        depth--; /* ".." : pop */
      continue;
    }
    if (depth < (int)(sizeof(starts) / sizeof(starts[0]))) {
      starts[depth] = s;
      ends[depth] = i;
      depth++;
    }
  }

  int o = 0;
  if (depth == 0) {
    out[o++] = '/'; /* the root */
  } else {
    for (int d = 0; d < depth; d++) {
      if (o >= PATH_MAX - 1)
        return -1;
      out[o++] = '/';
      for (int k = starts[d]; k < ends[d]; k++) {
        if (o >= PATH_MAX - 1)
          return -1;
        out[o++] = tmp[k];
      }
    }
  }
  out[o] = '\0';
  return 0;
}

/* If drawing scrolled the console since `before`, slide edit_start up to match. */
static void edit_fix_anchor(uint32_t before)
{
  uint32_t rows = con_scroll_count() - before;
  if (rows)
    edit_start = con_pos_scroll_up(edit_start, rows);
}

/* Redraw the current line in place, wiping any leftover tail, then park the cursor; survives mid-redraw scroll. */
static void edit_redraw(void)
{
  uint32_t s0 = con_scroll_count();
  con_set_pos(edit_start);
  for (size_t i = 0; i < line_len; i++)
    con_putc(line[i]);
  for (size_t i = line_len; i < render_len; i++)
    con_putc(' ');
  render_len = line_len;
  edit_fix_anchor(s0);
  con_set_pos((uint16_t)(edit_start + cursor));
}

/* Replace the editable line with s, cursor at the end, and redraw. */
static void edit_set(const char *s)
{
  size_t n = 0;
  while (s[n] && n < LINE_MAX - 1) {
    line[n] = s[n];
    n++;
  }
  line_len = cursor = n;
  edit_redraw();
}

static void hist_add(const char *s)
{
  if (s[0] == '\0')
    return;
  if (hist_count > 0 && streq(history[hist_count - 1], s)) {
    hist_pos = hist_count; /* skip consecutive duplicate */
    return;
  }
  if (hist_count == HIST_MAX) { /* full: drop the oldest */
    for (int i = 1; i < HIST_MAX; i++)
      str_copy(history[i - 1], history[i]);
    hist_count--;
  }
  str_copy(history[hist_count++], s);
  hist_pos = hist_count;
}

static void hist_up(void) /* recall older entries */
{
  if (hist_pos > 0) {
    hist_pos--;
    edit_set(history[hist_pos]);
  }
}

static void hist_down(void) /* back toward the new line */
{
  if (hist_pos >= hist_count)
    return;
  hist_pos++;
  edit_set(hist_pos == hist_count ? "" : history[hist_pos]);
}

static void print_prompt(void)
{
  con_set_color(CON_LIGHT_CYAN, CON_BLACK);
  con_write("owos:");
  /* Show the working directory, abbreviating the home directory to "~". */
  size_t hlen = strlen(HOME_DIR);
  bool in_home = true;
  for (size_t i = 0; i < hlen; i++)
    if (cwd[i] != HOME_DIR[i]) {
      in_home = false;
      break;
    }
  if (in_home && (cwd[hlen] == '\0' || cwd[hlen] == '/')) {
    con_printf("~%s", cwd + hlen); /* the path below home, if any */
  } else {
    con_write(cwd);
  }
  con_write("$ ");
  con_set_color(CON_LIGHT_GREY, CON_BLACK);
}

/* Begin a fresh input line: draw the prompt and reset the editor state. */
static void prompt(void)
{
  print_prompt();
  edit_start = con_get_pos();
  line_len = cursor = render_len = 0;
}

/* "help": list every built-in by walking the `builtins` table. */
static void cmd_help(const char *a);

static void print_kib(const char *label, uint32_t kib) { con_printf("%s%u KiB\n", label, kib); }

/* List storage devices with capacity and a read-only LBA 0 check on DMA controllers. */
/* Print a 512-byte-sector capacity as MiB, or GiB from 1 GiB up. */
static void write_capacity(uint32_t sectors)
{
  uint32_t mib = sectors / 2048;
  if (mib >= 1024) {
    uint32_t tenths = (mib * 10) / 1024; /* MiB -> tenths of a GiB */
    con_printf("%u.%u GiB", tenths / 10, tenths % 10);
  } else {
    con_printf("%u MiB", mib);
  }
}

static void cmd_storage(void)
{
  uint8_t buf[512];
  int n = 0;

  if (ata_present()) {
    con_printf("  IDE/ATA    %s  ", ata_model());
    write_capacity(ata_sectors());
    con_write(" (PIO, primary master)\n");
    n++;
  }
  if (ahci_present()) {
    con_printf("  SATA/AHCI  %s  ", ahci_model());
    write_capacity(ahci_sectors());
    con_write(" (DMA) -- read LBA0: ");
    con_write(ahci_read_sector(0, buf) == 0 ? "ok\n" : "FAIL\n");
    n++;
  }
  if (nvme_present()) {
    con_printf("  NVMe       %s  ", nvme_model());
    write_capacity(nvme_sectors());
    con_write(" (ns 1) -- read LBA0: ");
    con_write(nvme_read_sector(0, buf) == 0 ? "ok\n" : "FAIL\n");
    n++;
  }
  if (xhci_msc_present()) {
    con_printf("  USB        %s  ", xhci_msc_model());
    write_capacity(xhci_msc_sectors());
    con_write(" (xHCI BOT) -- read LBA0: ");
    /* Bounded waits and a device drop on silence make this report FAIL, never freeze. */
    con_write(xhci_msc_read(0, buf) == 0 ? "ok\n" : "FAIL\n");
    n++;
  }
  if (!n)
    con_write("  no storage devices detected\n");
}

static void cmd_meminfo(void)
{
  print_kib("phys total: ", pmm_total_kib());
  print_kib("phys used:  ", pmm_used_kib());
  print_kib("phys free:  ", pmm_free_kib());
  print_kib("heap used:  ", (uint32_t)(heap_used_bytes() / 1024));
  print_kib("heap free:  ", (uint32_t)(heap_free_bytes() / 1024));
}

/* Split "prog a b c" in place into argv; returns argc. Single/double quotes group, quote chars are removed. */
static int tokenize(char *s, char **argv, int max)
{
  int argc = 0;
  while (argc < max) {
    while (*s == ' ')
      s++;
    if (!*s)
      break;
    argv[argc++] = s;
    char *w = s; /* compacted write head (<= s) */
    char q = 0;  /* open quote char, or 0 */
    while (*s) {
      char c = *s;
      if (q) { /* inside quotes: copy until close */
        if (c == q)
          q = 0;
        else
          *w++ = c;
        s++;
      } else if (c == '"' || c == '\'') {
        q = c;
        s++; /* open a quote (dropped) */
      } else if (c == ' ') {
        break; /* unquoted space ends the token */
      } else {
        *w++ = c;
        s++;
      }
    }
    if (*s == ' ')
      s++;     /* consume the delimiter */
    *w = '\0'; /* terminate the compacted token */
  }
  return argc;
}

/* Read argv[0] off disk and spawn it; pid or -1. Looked up in the cwd (or absolute), then /bin. */
static int spawn(int argc, char **argv)
{
  if (argc == 0)
    return -1;

  /* Resolve the program to a path: cwd-relative (or absolute), then /bin. */
  char path[PATH_MAX];
  int sz = -1;
  if (path_resolve(argv[0], path) == 0)
    sz = fs_size(path);
  if (sz < 0 && argv[0][0] != '/') { /* fall back to /bin/<name> */
    int j = 0;
    const char *pfx = "/bin/";
    for (int i = 0; pfx[i]; i++)
      path[j++] = pfx[i];
    for (int i = 0; argv[0][i] && j < PATH_MAX - 1; i++)
      path[j++] = argv[0][i];
    path[j] = '\0';
    sz = fs_size(path);
  }
  if (sz < 0)
    return -1;

  /* Size the load buffer to the file; a fixed small buffer would truncate large ELFs. */
  uint8_t *buf = kmalloc((uint32_t)sz);
  if (!buf)
    return -1;
  int n = fs_read(path, buf, (uint32_t)sz);
  int pid = (n < 0) ? -1 : exec_program(buf, (uint32_t)n, argc, argv);
  if (pid >= 0)
    sched_set_cwd(pid, cwd); /* child resolves relative paths from here */
  kfree(buf);
  return pid;
}

static void wait_reap(int pid)
{
  int status;
  while (!sched_try_reap(pid, &status)) {
    /* While a foreground child runs, leave keystrokes in the ring for its read(); the desktop drains keys elsewhere. */
    __asm__ volatile("sti; hlt");
  }
  con_printf("[pid %u exited, status %u]\n", (uint32_t)pid, (uint32_t)status);
}

static void cmd_run(char *args)
{
  /* A pipeline "a | b" wires a's stdout to b's stdin through a kernel pipe. */
  char *bar = args;
  while (*bar && *bar != '|')
    bar++;

  if (*bar == '|') {
    *bar = '\0';
    char *lhs = args, *rhs = bar + 1;
    char *av1[8], *av2[8];
    int ac1 = tokenize(lhs, av1, 8);
    int ac2 = tokenize(rhs, av2, 8);
    if (ac1 == 0 || ac2 == 0) {
      con_write("usage: run <a> | <b>\n");
      return;
    }
    int id = pipe_alloc();
    if (id < 0) {
      con_write("out of pipes\n");
      return;
    }
    /* Set both up before either can be scheduled. */
    __asm__ volatile("cli");
    int p1 = spawn(ac1, av1);
    int p2 = spawn(ac2, av2);
    if (p1 >= 0)
      sched_set_pipe_fd(p1, 1, 1, id); /* a's stdout -> pipe write */
    if (p2 >= 0)
      sched_set_pipe_fd(p2, 0, 0, id); /* b's stdin -> pipe read */
    __asm__ volatile("sti");

    if (p1 < 0 || p2 < 0) {
      con_write("pipeline: bad program\n");
      return;
    }
    wait_reap(p1);
    wait_reap(p2);
    return;
  }

  char *argv[8];
  int argc = tokenize(args, argv, 8);
  if (argc == 0) {
    con_write("usage: run <file> [args]\n");
    return;
  }
  int pid = spawn(argc, argv);
  if (pid < 0) {
    con_write("no such file / not a valid program\n");
    return;
  }
  wait_reap(pid);
}

/* Options given to the running built-in, one bit per letter (see flag()). */
static uint64_t g_flags;
static uint64_t flag_bit(char c)
{
  return c >= 'a' && c <= 'z' ? 1ull << (c - 'a') : c >= 'A' && c <= 'Z' ? 1ull << (26 + c - 'A') : 0;
}
static bool flag(char c) { return (g_flags & flag_bit(c)) != 0; }

/* Print lines of a writable NUL-terminated buffer that contain pattern, walking it in place. */
static void grep_emit(const char *pattern, char *buf)
{
  char *p = buf;
  while (*p) {
    char *line_start = p;
    while (*p && *p != '\n')
      p++;
    int had_newline = (*p == '\n');
    *p = '\0';
    if ((flag('i') ? strcasestr : strstr)(line_start, pattern)) {
      con_printf("%s\n", line_start);
    }
    if (had_newline)
      p++;
  }
}

/* "grep <pattern> [file]": plain substring match on a file, or on piped input when no file. */
static void cmd_grep(const char *arg)
{
  char pattern[256];
  int i = 0;
  while (arg[i] && arg[i] != ' ' && i < (int)sizeof(pattern) - 1) {
    pattern[i] = arg[i];
    i++;
  }
  pattern[i] = '\0';
  while (arg[i] == ' ')
    i++;
  const char *name = arg + i;

  if (pattern[0] == '\0') {
    con_write("usage: grep [-i] <pattern> <file>  (or: <cmd> | grep [-i] <pattern>)\n");
    return;
  }

  if (name[0] != '\0') { /* file mode */
    int size = fs_size(name);
    char *buf = size < 0 ? 0 : kmalloc((uint32_t)size + 1);
    int n = buf ? fs_read(name, buf, (uint32_t)size) : -1;
    if (n < 0) {
      con_write(size < 0 ? "no such file\n" : "grep: cannot read file\n");
    } else {
      buf[n] = '\0';
      grep_emit(pattern, buf);
    }
    kfree(buf);
  } else if (g_pipe_in) { /* piped mode */
    grep_emit(pattern, g_pipe_in);
  } else {
    con_write("usage: grep [-i] <pattern> <file>  (or: <cmd> | grep [-i] <pattern>)\n");
  }
}

static void cmd_cat(const char *name)
{
  char rpath[PATH_MAX];
  if (path_resolve(name, rpath) != 0) {
    con_write("cat: path too long\n");
    return;
  }
  char *buf = kmalloc(4096);
  if (!buf)
    return;
  int n = fs_read(rpath, buf, 4095);
  if (n < 0) {
    con_write("no such file\n");
  } else {
    buf[n] = '\0';
    con_write(buf);
    if (n > 0 && buf[n - 1] != '\n')
      con_putc('\n');
  }
  kfree(buf);
}

/* Load and parse a .torrent into a kmalloc'd buffer (caller frees); NULL on failure. *t owns a pool freed by
 * torrent_free(). */

/* Parse a dotted-quad into ip[4]; returns the pointer past it, or NULL if malformed. */

/* torrent get <file>: concurrent multi-peer download, verifying each piece's SHA-1. */

/* torrent subcommands: info, peers, peer, get, test, peers test. */

/* System identity, the fields a POSIX uname reports. */
#define UNAME_SYSNAME "OwOS"  /* -s kernel name */
#define UNAME_NODENAME "owos" /* -n network node name */
#define UNAME_RELEASE "0.1"   /* -r kernel release */
#define UNAME_MACHINE "i386"  /* -m hardware */

/* "uname [-asnrmo]": print system identity; -s is the default. */

/* A built-in is a `builtins` row: name, handler, help; fn == NULL rows are help-only. Single source for dispatch, which
 * and help. */
typedef void (*cmd_fn)(const char *arg);
struct builtin {
  const char *name;
  cmd_fn fn; /* takes the argument string */
  const char *help;
  void (*fn0)(void); /* alternative for commands without arguments */
  const char *flags; /* option letters the command accepts (none when NULL) */
};

/* "which <name>...": report built-in or runnable file for each name, in run() order. */

/* Forward decl: parse_two is defined just below, but cp/mv use it. */
static void parse_two(const char *arg, char *a, char *b, int cap);

/* If `path` is a directory, append the basename of `src` ("cp f /dir" -> "/dir/f"). */
static void into_dir(char *path, const char *src)
{
  if (fs_isdir(path) != 1)
    return;
  const char *base = src;
  for (const char *p = src; *p; p++)
    if (*p == '/' && p[1])
      base = p + 1;
  int n = (int)strlen(path);
  if (n == 0 || path[n - 1] != '/')
    path[n++] = '/';
  for (int i = 0; base[i] && n < PATH_MAX - 1; i++)
    path[n++] = base[i];
  path[n] = '\0';
}

/* "cd [dir]", change the working directory (no arg -> home). */
static void cmd_cd(const char *arg)
{
  while (*arg == ' ')
    arg++;
  const char *a = (*arg == '\0') ? HOME_DIR : arg;
  char target[PATH_MAX];
  if (path_resolve(a, target) != 0) {
    con_write("cd: path too long\n");
    return;
  }
  int d = fs_isdir(target);
  if (d < 0) {
    con_printf("cd: no such directory: %s\n", a);
    return;
  }
  if (d == 0) {
    con_printf("cd: not a directory: %s\n", a);
    return;
  }
  int i = 0;
  for (; target[i]; i++)
    cwd[i] = target[i];
  cwd[i] = '\0';
}

/* "pwd", print the working directory. */
static void cmd_pwd(void) { con_printf("%s\n", cwd); }

/* "cp <src> <dst>": copy a file; a directory dst receives it under src's basename. */
#define ENTRY_NAME_MAX 64 /* size of the name buffer fs_enum_dir fills */

/* Append "/name" to a path, false when it does not fit. */
static bool path_join(char *path, const char *name)
{
  int n = (int)strlen(path);
  if (n + 1 + (int)strlen(name) >= PATH_MAX)
    return false;
  if (n == 0 || path[n - 1] != '/')
    path[n++] = '/';
  strlcpy(path + n, name, PATH_MAX - (uint32_t)n);
  return true;
}

/* Drop the last component of a path in place. */
static void path_parent(char *path)
{
  int n = (int)strlen(path);
  while (n > 1 && path[n - 1] != '/')
    n--;
  path[n > 1 ? n - 1 : n] = '\0';
}

/* Copy one file; false after reporting the failure. */
static bool copy_file(const char *src, const char *dst)
{
  int sz = fs_size(src);
  if (sz < 0) {
    con_printf("cp: no such file: %s\n", src);
    return false;
  }
  uint8_t *buf = kmalloc(sz ? (uint32_t)sz : 1);
  if (!buf) {
    con_write("cp: out of memory\n");
    return false;
  }
  int n = fs_read(src, buf, (uint32_t)sz);
  bool ok = false;
  if (n < 0)
    con_write("cp: read failed\n");
  else if (fs_create(dst, buf, (uint32_t)n) != 0)
    con_write("cp: write failed (disk or directory full?)\n");
  else
    ok = true;
  kfree(buf);
  return ok;
}

/* Copy a directory tree. Walks with an explicit stack: this runs on the 16 KiB boot stack. */
static void copy_tree(const char *src, const char *dst)
{
  uint32_t sl = strlen(src);
  if (strlen(dst) >= sl && memcmp(dst, src, sl) == 0 && (dst[sl] == '\0' || dst[sl] == '/' || sl == 1)) {
    con_write("cp: cannot copy a directory into itself\n");
    return;
  }
  if (fs_isdir(dst) != 1 && fs_mkdir(dst) != 0) {
    con_printf("cp: cannot create %s\n", dst);
    return;
  }
  char sp[PATH_MAX], dp[PATH_MAX];
  uint16_t next[PATH_MAX / 2 + 1]; /* next entry to look at, per level */
  int depth = 0, files = 0, failed = 0;
  strlcpy(sp, src, sizeof sp);
  strlcpy(dp, dst, sizeof dp);
  next[0] = 0;
  for (;;) {
    char name[ENTRY_NAME_MAX];
    int is_dir;
    if (!fs_enum_dir(sp, next[depth], name, &is_dir, 0, 0)) {
      if (depth == 0)
        break;
      path_parent(sp);
      path_parent(dp);
      depth--;
      continue;
    }
    next[depth]++;
    char cs[PATH_MAX], cd[PATH_MAX];
    strlcpy(cs, sp, sizeof cs);
    strlcpy(cd, dp, sizeof cd);
    if (!path_join(cs, name) || !path_join(cd, name)) {
      con_printf("cp: path too long: %s\n", name);
      failed++;
    } else if (is_dir) {
      if (depth >= PATH_MAX / 2 || (fs_isdir(cd) != 1 && fs_mkdir(cd) != 0)) {
        con_printf("cp: cannot create %s\n", cd);
        failed++;
      } else {
        strlcpy(sp, cs, sizeof sp);
        strlcpy(dp, cd, sizeof dp);
        next[++depth] = 0;
      }
    } else if (copy_file(cs, cd)) {
      files++;
    } else {
      failed++;
    }
  }
  con_printf("%s -> %s (%d files%s)\n", src, dst, files, failed ? ", some failed" : "");
}

static void cmd_cp(const char *arg)
{
  char src[PATH_MAX], dst[PATH_MAX], rsrc[PATH_MAX], rdst[PATH_MAX];
  parse_two(arg, src, dst, (int)sizeof(src));
  if (src[0] == '\0' || dst[0] == '\0') {
    con_write("usage: cp [-r] <src> <dst>\n");
    return;
  }
  if (path_resolve(src, rsrc) != 0 || path_resolve(dst, rdst) != 0) {
    con_write("cp: path too long\n");
    return;
  }
  into_dir(rdst, rsrc);
  if (streq(rsrc, rdst)) {
    con_write("cp: source and destination are the same\n");
    return;
  }
  if (fs_isdir(rsrc) == 1) {
    if (flag('r') || flag('R'))
      copy_tree(rsrc, rdst);
    else
      con_printf("cp: %s is a directory (use -r)\n", src);
    return;
  }
  if (copy_file(rsrc, rdst))
    con_printf("%s -> %s\n", rsrc, rdst);
}

/* "mv <src> <dst>": rename or move; a directory dst receives it under src's basename. */
static void cmd_mv(const char *arg)
{
  char src[PATH_MAX], dst[PATH_MAX], rsrc[PATH_MAX], rdst[PATH_MAX];
  parse_two(arg, src, dst, (int)sizeof(src));
  if (src[0] == '\0' || dst[0] == '\0') {
    con_write("usage: mv <src> <dst>\n");
    return;
  }
  if (path_resolve(src, rsrc) != 0 || path_resolve(dst, rdst) != 0) {
    con_write("mv: path too long\n");
    return;
  }
  into_dir(rdst, rsrc);
  if (streq(rsrc, rdst))
    return; /* a no-op, like real mv */
  if (fs_rename(rsrc, rdst) != 0) {
    con_write("mv: failed (no such source, or illegal move)\n");
    return;
  }
  con_printf("%s -> %s\n", rsrc, rdst);
}

/* "rm <file> [file...]", delete one or more files (sectors leak). */
/* Delete a file, or a directory with everything below it. Walks with an explicit stack: this runs on the
 * 16 KiB boot stack. Returns 0 when everything was removed. */
static int rm_tree(const char *root)
{
  if (fs_isdir(root) != 1)
    return fs_delete(root) == 0 ? 0 : -1;
  char cur[PATH_MAX];
  uint16_t skip[PATH_MAX / 2 + 1]; /* entries left in place at each level (undeletable) */
  int depth = 0, failed = 0;
  strlcpy(cur, root, sizeof cur);
  skip[0] = 0;
  for (;;) {
    char name[ENTRY_NAME_MAX];
    int is_dir;
    if (!fs_enum_dir(cur, skip[depth], name, &is_dir, 0, 0)) {
      bool removed = fs_rmdir(cur) == 0;
      if (depth == 0)
        return removed && !failed ? 0 : -1;
      path_parent(cur);
      depth--;
      if (!removed) {
        failed = 1;
        skip[depth]++;
      }
      continue;
    }
    char child[PATH_MAX];
    strlcpy(child, cur, sizeof child);
    if (!path_join(child, name)) {
      failed = 1;
      skip[depth]++;
    } else if (is_dir && depth < PATH_MAX / 2) {
      strlcpy(cur, child, sizeof cur);
      skip[++depth] = 0;
    } else if (is_dir || fs_delete(child) != 0) {
      failed = 1;
      skip[depth]++;
    }
  }
}

static void cmd_rm(const char *arg)
{
  char name[PATH_MAX], extra[PATH_MAX], rpath[PATH_MAX];
  parse_two(arg, name, extra, (int)sizeof(name));
  if (name[0] == '\0') {
    con_write("usage: rm [-rf] <file>\n");
    return;
  }
  bool recursive = flag('r') || flag('R');
  if (path_resolve(name, rpath) != 0) {
    con_write("rm: path too long\n");
  } else if (recursive && streq(rpath, "/")) {
    con_write("rm: refusing to remove /\n");
  } else if (recursive && fs_isdir(rpath) == 1) {
    if (rm_tree(rpath) == 0)
      con_printf("removed %s\n", rpath);
    else
      con_printf("rm: could not remove everything under %s\n", name);
  } else {
    int r = fs_delete(rpath);
    if (r == 0) {
      con_printf("removed %s\n", rpath);
    } else if (r == -2) {
      con_printf("rm: is a directory (use -r or rmdir): %s\n", name);
    } else if (!flag('f')) {
      con_printf("rm: no such file: %s\n", name);
    }
  }
  if (extra[0]) /* a courtesy for "rm a b" */
    cmd_rm(extra);
}
static void cmd_mkdir(const char *arg)
{
  char name[PATH_MAX], extra[PATH_MAX], rpath[PATH_MAX];
  parse_two(arg, name, extra, (int)sizeof(name));
  if (name[0] == '\0') {
    con_write("usage: mkdir [-p] <dir>\n");
    return;
  }
  if (path_resolve(name, rpath) != 0)
    con_write("mkdir: path too long\n");
  else if (flag('p')) {
    bool ok = true;
    for (int k = 1; rpath[k] && ok; k++) {
      if (rpath[k] != '/')
        continue;
      rpath[k] = '\0';
      ok = fs_isdir(rpath) == 1 || fs_mkdir(rpath) == 0;
      rpath[k] = '/';
    }
    ok = ok && (fs_isdir(rpath) == 1 || fs_mkdir(rpath) == 0);
    if (!ok)
      con_printf("mkdir: cannot create %s\n", name);
  } else if (fs_mkdir(rpath) == 0) {
    con_printf("created %s\n", rpath);
  } else
    con_write("mkdir: cannot create (exists, bad path, or directory full)\n");
  if (extra[0])
    cmd_mkdir(extra);
}

/* "rmdir <dir>", remove an empty directory. */
static void cmd_rmdir(const char *arg)
{
  char name[PATH_MAX], extra[PATH_MAX], rpath[PATH_MAX];
  parse_two(arg, name, extra, (int)sizeof(name));
  if (name[0] == '\0') {
    con_write("usage: rmdir <dir>\n");
    return;
  }
  if (path_resolve(name, rpath) != 0) {
    con_write("rmdir: path too long\n");
  } else {
    int r = fs_rmdir(rpath);
    if (r == 0) {
      con_printf("removed %s\n", rpath);
    } else if (r == -2) {
      con_printf("rmdir: not a directory: %s\n", name);
    } else if (r == -3) {
      con_printf("rmdir: directory not empty: %s\n", name);
    } else {
      con_printf("rmdir: no such directory: %s\n", name);
    }
  }
  if (extra[0])
    cmd_rmdir(extra);
}

/* Copy up to two space-separated tokens of arg into a and b (`cap` bytes each); b is empty for one token. */
static void parse_two(const char *arg, char *a, char *b, int cap)
{
  int i = 0;
  while (*arg == ' ')
    arg++;
  while (*arg && *arg != ' ' && i < cap - 1)
    a[i++] = *arg++;
  a[i] = '\0';
  while (*arg == ' ')
    arg++;
  i = 0;
  while (*arg && *arg != ' ' && i < cap - 1)
    b[i++] = *arg++;
  b[i] = '\0';
}

/* "unzip <src> [dst]": dst defaults to src minus ".z"; otherwise dst is required. */

/* "gzip <src> [dst]": dst defaults to "<src>.gz". */

/* "gunzip <src> [dst]": dst defaults to src minus ".gz". */

#define REDIR_MAX (16 * 1024) /* cap on captured output for "cmd > file" */

/* Built-in command handlers. fn takes the argument string; argument-less commands set fn0 instead. */
static void bi_ls(const char *a)
{
  if (a[0] == '\0') {
    fs_list(cwd);
    return;
  }
  char rpath[PATH_MAX];
  if (path_resolve(a, rpath) != 0)
    con_write("ls: path too long\n");
  else
    fs_list(rpath);
}

/* mtp: browse a phone/camera in MTP mode; objects, not blocks, so it is not mounted at /media. */

/* Detach the volume at /media; writes are write-through so nothing needs flushing. */
/* Build the default mount point "/media/<dev>" into out. */
static void media_point(const char *dev, char *out)
{
  const char *pfx = "/media/";
  int i = 0;
  for (; pfx[i]; i++)
    out[i] = pfx[i];
  for (int j = 0; dev[j] && i < 60; j++)
    out[i++] = dev[j];
  out[i] = '\0';
}

/* umount [point]: detach the volume at `point`, or the first mount if no arg. */
static void bi_umount(const char *a)
{
  while (*a == ' ')
    a++;
  if (*a) {
    if (fs_unmount(a) == 0) {
      con_printf("umount: detached %s\n", a);
    } else
      con_write("umount: nothing mounted there\n");
    return;
  }
  struct fs_mountinfo mi;
  if (fs_mount_enum(0, &mi) == 1) {
    fs_unmount(mi.point);
    con_printf("umount: detached %s\n", mi.point);
  } else
    con_write("umount: nothing mounted\n");
}

/* mount [dev [point]]: mount a removable device, or the first unmounted one. */
static void bi_mount(const char *a)
{
  while (*a == ' ')
    a++;
  char dev[16] = "", point[64] = "";
  int i = 0;
  while (*a && *a != ' ' && i < 15)
    dev[i++] = *a++;
  dev[i] = '\0';
  while (*a == ' ')
    a++;
  i = 0;
  while (*a && *a != ' ' && i < 63)
    point[i++] = *a++;
  point[i] = '\0';

  if (dev[0]) {
    if (!point[0])
      media_point(dev, point);
    if (fs_mount(dev, point) == 0) {
      con_printf("mount: %s -> %s\n", dev, point);
    } else
      con_write("mount: cannot mount (unknown device or filesystem)\n");
    return;
  }
  struct blk_devinfo devs[8];
  int n = blk_dev_list(devs, 8);
  for (int k = 0; k < n; k++) {
    struct fs_mountinfo mi;
    bool mounted = false;
    for (int j = 0; fs_mount_enum(j, &mi); j++)
      if (streq(mi.dev, devs[k].dev)) {
        mounted = true;
        break;
      }
    if (!mounted) {
      media_point(devs[k].dev, point);
      if (fs_mount(devs[k].dev, point) == 0) {
        con_printf("mount: %s -> %s\n", devs[k].dev, point);
      } else
        con_printf("mount: no recognised filesystem on %s\n", devs[k].dev);
      return;
    }
  }
  con_write("mount: no unmounted device (try `ls /dev`)\n");
}
/* `play` is the program /bin/play; is_capture_cli() routes it like neofetch/curl. */
static void bi_echo(const char *a) { con_printf("%s\n", a); }
/* clip: copy text to the clipboard, or print the clipboard when no argument. */
static void bi_poweroff(const char *a)
{
  (void)a;
  con_write("powering off...\n");
  power_off();
  con_write("poweroff not supported here\n");
}
static void bi_restart(const char *a)
{
  (void)a;
  con_write("restarting...\n");
  power_reset();
}

/* The one command list; order is `help` order, fn == NULL rows are help-only notes. */
/* Minimal recovery console for COM1; everything else lives in /bin/term. Bare names still spawn /bin programs. */
static const struct builtin builtins[] = {
    {.name = "help", .fn = cmd_help, .help = "help          show this list"},
    {.name = "clear", .help = "clear         clear the screen", .fn0 = con_clear},
    {.name = "journal", .help = "journal       replay the verbose boot log", .fn0 = journal_dump},
    {.name = "meminfo", .help = "meminfo       physical and heap memory usage", .fn0 = cmd_meminfo},
    {.name = "ps", .help = "ps            list scheduler threads and run counts", .fn0 = sched_dump},
    {.name = "ls", .fn = bi_ls, .flags = "l", .help = "ls [-l] [path] list a directory (default: current)"},
    {.name = "pwd", .help = "pwd           print the working directory", .fn0 = cmd_pwd},
    {.name = "cd", .fn = cmd_cd, .help = "cd [dir]      change directory (no arg -> home)"},
    {.name = "cat", .fn = cmd_cat, .help = "cat <file>    print a file"},
    {.name = "grep", .fn = cmd_grep, .flags = "i", .help = "grep <p> <f>  print lines of <f> that contain <p>"},
    {.name = "cp", .fn = cmd_cp, .flags = "rR", .help = "cp <src> <dst> copy a file"},
    {.name = "mv", .fn = cmd_mv, .help = "mv <src> <dst> rename/move a file or directory"},
    {.name = "rm", .fn = cmd_rm, .flags = "rRf", .help = "rm <file>...  delete files"},
    {.name = "mkdir", .fn = cmd_mkdir, .flags = "p", .help = "mkdir <dir>...  create directories"},
    {.name = "rmdir", .fn = cmd_rmdir, .help = "rmdir <dir>...  remove empty directories"},
    {.name = "storage", .help = "storage       list storage devices (IDE/SATA/NVMe)", .fn0 = cmd_storage},
    {.name = "mount",
     .fn = bi_mount,
     .help = "mount [dev [pt]]  mount a removable device (see ls /dev) at "
             "/media/<name>"},
    {.name = "umount", .fn = bi_umount, .help = "umount [point]    detach a mounted volume"},
    {.name = "echo", .fn = bi_echo, .help = "echo <text>   print text back"},
    {.name = "poweroff", .fn = bi_poweroff, .help = "poweroff      shut the machine down"},
    {.name = "restart", .fn = bi_restart, .help = "restart       reboot the machine"},
    {NULL}, /* sentinel */
};

/* Strip leading options ("--" ends them) from *arg; false after reporting one that `allowed` lacks. */
static bool builtin_flags(const char *cmd, const char *allowed, const char **arg, uint64_t *seen)
{
  const char *a = *arg;
  while (a[0] == '-' && a[1]) {
    if (a[1] == '-' && (a[2] == '\0' || a[2] == ' ')) {
      for (a += 2; *a == ' '; a++)
        ;
      break;
    }
    for (a++; *a && *a != ' '; a++) {
      const char *f = allowed;
      while (f && *f && *f != *a)
        f++;
      if (!f || !*f) {
        con_printf("%s: unknown option -%c\n", cmd, *a);
        return false;
      }
      *seen |= flag_bit(*a);
    }
    while (*a == ' ')
      a++;
  }
  *arg = a;
  return true;
}

/* Resolve a command name to its row, skipping help-only (fn == NULL) entries. */
static const struct builtin *builtin_find(const char *name)
{
  for (const struct builtin *b = builtins; b->name || b->help; b++)
    if ((b->fn || b->fn0) && streq(b->name, name))
      return b;
  return NULL;
}

static void cmd_help(const char *a)
{
  (void)a;
  con_write("commands:\n");
  for (const struct builtin *b = builtins; b->name || b->help; b++) {
    if (!b->help)
      continue;
    con_printf("  %s\n", b->help);
  }
}

static void run(char *cmd)
{
  if (cmd[0] == '\0')
    return; /* blank line */

  /* "<cmd> > <file>": capture cmd's output (all built-ins emit via con_putc) and write it to file. */
  char *gt = cmd;
  while (*gt && *gt != '>')
    gt++;
  if (*gt == '>') {
    *gt = '\0';
    char *fname = gt + 1;
    while (*fname == ' ') /* skip blanks after '>' */
      fname++;
    char *fend = fname; /* stop at first blank */
    while (*fend && *fend != ' ')
      fend++;
    *fend = '\0';
    char *cend = gt; /* trim blanks before '>' */
    while (cend > cmd && cend[-1] == ' ')
      *--cend = '\0';

    if (cmd[0] == '\0' || fname[0] == '\0') {
      con_write("usage: <command> > <file>\n");
      return;
    }
    char *cap = kmalloc(REDIR_MAX);
    if (!cap) {
      con_write("redirect: out of memory\n");
      return;
    }
    con_capture_begin(cap, REDIR_MAX);
    run(cmd); /* no '>' left: dispatches */
    uint32_t n = con_capture_end();
    if (fs_create(fname, cap, n) == 0) {
      con_printf("wrote %u bytes to %s\n", n, fname);
    } else {
      con_write("redirect: write failed\n");
    }
    kfree(cap);
    return;
  }

  /* Pipe "A | B": if B is a program, run both through a kernel pipe; if a builtin, capture A into g_pipe_in. */
  {
    char *bar = cmd;
    while (*bar && *bar != '|')
      bar++;
    if (*bar == '|') {
      /* Peek B's first token without disturbing cmd. */
      char *rp = bar + 1;
      while (*rp == ' ')
        rp++;
      char rname[64];
      int rn = 0;
      while (rp[rn] && rp[rn] != ' ' && rn < (int)sizeof(rname) - 1) {
        rname[rn] = rp[rn];
        rn++;
      }
      rname[rn] = '\0';
      if (!builtin_find(rname)) { /* B is a program: real pipe */
        cmd_run(cmd);             /* splits on '|', spawns both */
        return;
      }
      *bar = '\0';
      char *left = cmd, *right = bar + 1;
      while (*right == ' ')
        right++;
      char *lend = bar; /* trim blanks before '|' */
      while (lend > left && lend[-1] == ' ')
        *--lend = '\0';
      if (left[0] == '\0' || right[0] == '\0') {
        con_write("usage: <command> | grep <pattern>\n");
        return;
      }
      char *cap = kmalloc(REDIR_MAX);
      if (!cap) {
        con_write("pipe: out of memory\n");
        return;
      }
      con_capture_begin(cap, REDIR_MAX);
      run(left);
      uint32_t n = con_capture_end();
      if (n >= REDIR_MAX)
        n = REDIR_MAX - 1;
      cap[n] = '\0';
      g_pipe_in = cap;
      run(right);
      g_pipe_in = NULL;
      kfree(cap);
      return;
    }
  }

  /* Split off the command name; the rest (leading blanks trimmed) is the argument. `cmd` is left intact. */
  char name[64];
  int n = 0;
  while (cmd[n] && cmd[n] != ' ' && n < (int)sizeof(name) - 1) {
    name[n] = cmd[n];
    n++;
  }
  name[n] = '\0';
  const char *carg = cmd + n;
  while (*carg == ' ')
    carg++;

  const struct builtin *b = builtin_find(name);
  if (b) {
    g_flags = 0;
    if (!builtin_flags(b->name, b->flags, &carg, &g_flags))
      return;
    if (b->fn0)
      b->fn0();
    else
      b->fn(carg);
    return;
  }

  /* Not a builtin: look for a program of that name (cwd, then /bin) and run it in user space. */
  char *av[8] = {0};
  int ac = tokenize(cmd, av, 8);
  int pid = (ac > 0) ? spawn(ac, av) : -1;
  if (pid >= 0) {
    wait_reap(pid);
  } else if (ac > 0) {
    con_printf("unknown command: %s\ntype 'help'\n", av[0]);
  }
}

void shell_feed(char c)
{
  switch (c) {
  case '\n':
    con_set_pos((uint16_t)(edit_start + line_len)); /* end of line */
    con_putc('\n');
    line[line_len] = '\0';
    hist_add(line);
    run(line);
    prompt();
    return;
  case '\b': /* backspace: delete left of cursor */
    if (cursor > 0) {
      int at_end = (cursor == line_len);
      for (size_t i = cursor - 1; i + 1 < line_len; i++)
        line[i] = line[i + 1];
      cursor--;
      line_len--;
      if (at_end) { /* fast path: just erase the last char */
        con_putc('\b');
        render_len = line_len;
      } else {
        edit_redraw();
      }
    }
    return;
  case KEY_DELETE: /* delete char under the cursor */
    if (cursor < line_len) {
      for (size_t i = cursor; i + 1 < line_len; i++)
        line[i] = line[i + 1];
      line_len--;
      edit_redraw();
    }
    return;
  case KEY_LEFT:
    if (cursor > 0)
      con_set_pos((uint16_t)(edit_start + --cursor));
    return;
  case KEY_RIGHT:
    if (cursor < line_len)
      con_set_pos((uint16_t)(edit_start + ++cursor));
    return;
  case KEY_HOME:
    cursor = 0;
    con_set_pos(edit_start);
    return;
  case KEY_END:
    cursor = line_len;
    con_set_pos((uint16_t)(edit_start + line_len));
    return;
  case KEY_UP:
    hist_up();
    return;
  case KEY_DOWN:
    hist_down();
    return;
  case KEY_KILL: /* Ctrl+U: erase the whole line */
    line_len = cursor = 0;
    edit_redraw();
    return;
  case KEY_CLEAR: /* Ctrl+L: clear, keep the line */
    con_clear();
    print_prompt();
    edit_start = con_get_pos();
    render_len = 0;
    edit_redraw();
    return;
  default:
    /* Insert a printable byte at the cursor; high bytes are FR-layout CP437 accents (0x15 is '§'); other controls
     * ignored. */
    if (((unsigned char)c >= 0x20 || (unsigned char)c == 0x15) && line_len < LINE_MAX - 1) {
      int at_end = (cursor == line_len);
      for (size_t i = line_len; i > cursor; i--)
        line[i] = line[i - 1];
      line[cursor] = c;
      cursor++;
      line_len++;
      if (at_end) { /* fast path: just echo the new char */
        uint32_t s0 = con_scroll_count();
        con_putc(c);
        render_len = line_len;
        edit_fix_anchor(s0); /* the echo may have scrolled */
      } else {
        edit_redraw();
      }
    }
    return;
  }
}

/* Ctrl+C at the prompt: abandon the line (a running command handles its own Ctrl+C). */
void shell_interrupt(void)
{
  con_set_pos((uint16_t)(edit_start + line_len));
  con_write("^C\n");
  prompt();
}

void shell_init(void)
{
  /* The shell owns the console: stop journaling and clear the screen (`journal` replays the bring-up log). */
  journal_capture(0);
  con_clear();
  con_write("type 'help' for commands, 'journal' for the boot log.\n");
  prompt();
}
