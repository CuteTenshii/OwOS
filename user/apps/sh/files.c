/* files.c: file, device and system-information builtins */

#include "sh.h"

/* builtins */

static char iobuf[64 * 1024];

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

void bi_ls(int argc, char **argv)
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

void bi_cat(int argc, char **argv)
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

void bi_cp(int argc, char **argv)
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

void bi_stat(int argc, char **argv)
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

void bi_umask(int argc, char **argv)
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
int cat_proc(const char *name)
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

void bi_uptime(int, char **)
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

void bi_which(int argc, char **argv)
{
  if (argc < 2) {
    tputs("usage: which <name>\n");
    return;
  }
  char path[256];
  if (is_builtin(argv[1])) {
    tputs(argv[1]);
    tputs(": shell builtin\n");
  } else if (find_command(argv[1], path, sizeof path)) {
    tputs(path);
    tputc('\n');
  } else {
    err(argv[1], " not found");
  }
}

/* control builtins: act via /proc + /sys, no kernel-shell bridge */

void bi_loadkeys(int argc, char **argv)
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

void bi_clip(int argc, char **argv)
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

void bi_mount(int argc, char **argv)
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

void bi_umount(int argc, char **argv)
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

void bi_services(int argc, char **argv)
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
void do_compress(int argc, char **argv, const char *suffix, const char *cmd,
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
void do_decompress(int argc, char **argv, const char *suffix, const char *cmd,
                   int (*dec)(const uint8_t *, uint32_t, uint8_t *, uint32_t), int (*orig)(const uint8_t *, uint32_t))
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
void bi_grep(int argc, char **argv)
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
    if (strstr(&iobuf[ls], pat)) {
      tputs(&iobuf[ls]);
      tputc('\n');
    }
    i++;
  }
}

/* Run argv with its output written to path (appended when append is set) */
void run_redirected(int argc, char **argv, const char *path, bool append)
{
  char full[256];
  abspath(path, full);
  struct statinfo st;
  char *old = 0;
  int oldlen = 0;
  if (append && stat(full, &st) == 0 && !st.is_dir && st.size) {
    old = malloc(st.size);
    oldlen = old ? readfile(full, old, (int)st.size) : -1;
    if (oldlen < 0) {
      free(old);
      err("cannot read ", path);
      return;
    }
  }
  out_capture_begin();
  run_argv(argc, argv);
  unsigned len;
  const char *text = out_capture_end(&len);
  char *all = malloc(oldlen + len + 1);
  if (all) {
    memcpy(all, old, (unsigned)oldlen);
    memcpy(all + oldlen, text, len);
    if (writefile(full, all, oldlen + (int)len) != 0)
      err("sh: cannot write ", path);
  }
  free(all);
  free(old);
}
