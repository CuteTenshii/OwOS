/* unrar.c - list, test and extract RAR archives: unrar l|v|t|x|e [-p[PASSWORD]] [-o+|-o-] ARCHIVE [DEST] */
#include "owos.h"
#include "rar.h"
#include <stdbool.h>
#include <stdint.h>

struct opts {
  char verb;
  const char *password;
  int overwrite; /* 1 = -o+, -1 = -o-, 0 = ask */
  const char *archive, *dest;
};

static bool failed;
static unsigned cur_umask;

static int fs_read(void *ctx, uint64_t off, void *buf, uint32_t len)
{
  if (off > 0xffffffffu)
    return 0;
  return readfile_at((const char *)ctx, (unsigned)off, buf, len);
}

static void fs_close(void *ctx) { free(ctx); }

static char *dup_str(const char *s)
{
  char *p = malloc((unsigned)strlen(s) + 1);
  if (p)
    strcpy(p, s);
  return p;
}

static int fs_open_src(const char *path, struct rar_src *out)
{
  struct statinfo st;
  char *copy;
  if (stat(path, &st) != 0 || st.is_dir || !(copy = dup_str(path)))
    return -1;
  out->read = fs_read;
  out->close = fs_close;
  out->ctx = copy;
  return 0;
}

static int fs_open_volume(void *ctx, const char *name, struct rar_src *out)
{
  (void)ctx;
  return fs_open_src(name, out);
}

static void put_u64(uint64_t v, int width)
{
  char buf[24];
  int n = 0;
  do {
    buf[n++] = (char)('0' + (int)(v % 10));
    v /= 10;
  } while (v);
  for (int i = n; i < width; i++)
    puts(" ");
  while (n)
    write(1, &buf[--n], 1);
}

static void put_num(unsigned v, int width, char pad)
{
  char buf[12];
  int n = 0;
  do {
    buf[n++] = (char)('0' + (int)(v % 10));
    v /= 10;
  } while (v);
  for (int i = n; i < width; i++)
    write(1, &pad, 1);
  while (n)
    write(1, &buf[--n], 1);
}

static void put_hex8(uint32_t v)
{
  for (int i = 28; i >= 0; i -= 4)
    write(1, &"0123456789ABCDEF"[(v >> i) & 15], 1);
}

static void put_date(uint32_t t)
{
  if (!t) {
    puts("                ");
    return;
  }
  unsigned days = t / 86400, secs = t % 86400;
  int z = (int)days + 719468;
  int era = z / 146097;
  unsigned doe = (unsigned)(z - era * 146097);
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int y = (int)yoe + era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned mp = (5 * doy + 2) / 153;
  unsigned d = doy - (153 * mp + 2) / 5 + 1;
  unsigned m = mp < 10 ? mp + 3 : mp - 9;
  if (m <= 2)
    y++;
  put_num((unsigned)y, 4, '0');
  puts("-");
  put_num(m, 2, '0');
  puts("-");
  put_num(d, 2, '0');
  puts(" ");
  put_num(secs / 3600, 2, '0');
  puts(":");
  put_num(secs / 60 % 60, 2, '0');
}

static void report(const char *name, const char *what, int rc)
{
  puts(name);
  puts(": ");
  puts(what);
  if (rc) {
    puts(" (");
    puts(rar_strerror(rc));
    puts(")");
  }
  puts("\n");
}

static int read_password(char *out, int max)
{
  int len = 0;
  for (;;) {
    char c;
    int n = read_nb(0, &c, 1);
    if (n < 0)
      return -1;
    if (n == 0) {
      msleep(4);
      continue;
    }
    if (c == '\r' || c == '\n') {
      puts("\n");
      break;
    }
    if ((unsigned char)c == KEY_INTR) {
      puts("\n");
      return -1;
    }
    if (c == '\b' || c == 0x7f) {
      if (len > 0)
        len--;
      continue;
    }
    if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7f && len < max - 1)
      out[len++] = c;
  }
  out[len] = '\0';
  return len;
}

static char pw_buf[128];

static const char *ask_password(const char *archive)
{
  puts("Enter password for ");
  puts(archive);
  puts(": ");
  return read_password(pw_buf, sizeof pw_buf) > 0 ? pw_buf : NULL;
}

static char *join(const char *dir, const char *name)
{
  int dl = dir ? (int)strlen(dir) : 0;
  char *p = malloc((unsigned)(dl + (int)strlen(name) + 2));
  if (!p)
    return NULL;
  p[0] = 0;
  if (dl) {
    strcpy(p, dir);
    if (dir[dl - 1] != '/')
      strcat(p, "/");
  }
  strcat(p, name);
  return p;
}

static bool exists_dir(const char *path)
{
  struct statinfo st;
  return stat(path, &st) == 0 && st.is_dir;
}

/* mkdir -p for the directory components of path (the last one too when whole is set) */
static bool make_dirs(const char *path, bool whole)
{
  char *p = dup_str(path);
  if (!p)
    return false;
  bool ok = true;
  for (char *s = p + (p[0] == '/'); *s && ok; s++) {
    if (*s != '/')
      continue;
    *s = 0;
    if (*p && !exists_dir(p) && mkdir(p) != 0)
      ok = false;
    *s = '/';
  }
  if (ok && whole && !exists_dir(p) && mkdir(p) != 0)
    ok = false;
  free(p);
  return ok;
}

static void apply_mode(const char *path, const struct rar_entry *e)
{
  if (e->host == 1 && e->mode)
    chmod(path, e->mode & 01777u & ~cur_umask);
}

static int to_stream(void *ctx, const void *buf, uint32_t len)
{
  (void)ctx;
  while (len) {
    uint32_t n = len > 0x10000 ? 0x10000 : len;
    if (stream_write(buf, (int)n) < 0)
      return 1;
    buf = (const char *)buf + n;
    len -= n;
  }
  return 0;
}

static int to_null(void *ctx, const void *buf, uint32_t len)
{
  (void)ctx;
  (void)buf;
  (void)len;
  return 0;
}

struct linkbuf {
  char *p;
  uint32_t n, cap;
};

static int to_link(void *ctx, const void *buf, uint32_t len)
{
  struct linkbuf *l = ctx;
  if (l->n + len > 4096)
    return 1;
  if (l->n + len + 1 > l->cap) {
    char *q = realloc(l->p, l->n + len + 64);
    if (!q)
      return 1;
    l->p = q;
    l->cap = l->n + len + 64;
  }
  memcpy(l->p + l->n, buf, len);
  l->n += len;
  return 0;
}

static int begin_file(const char *path) { return stream_begin(path) < 0; }

/* run the extraction, asking for a password again (up to 3 times) when it is wrong or missing */
static int extract_retry(struct rar *r, const char *name, int (*sink)(void *, const void *, uint32_t), void *ctx,
                         const char *path)
{
  int rc;
  for (int tries = 0;; tries++) {
    if (path && begin_file(path))
      return RAR_EWRITE;
    rc = rar_extract(r, sink, ctx);
    if (rc != RAR_EPASSWORD || tries >= 2)
      return rc;
    if (path)
      stream_end();
    const char *pw = ask_password(name);
    if (!pw || rar_set_password(r, pw))
      return rc;
  }
}

static const char *base_name(const char *name)
{
  const char *b = name;
  for (const char *p = name; *p; p++)
    if (*p == '/')
      b = p + 1;
  return b;
}

static bool want_overwrite(const struct opts *o, const char *path)
{
  if (o->overwrite)
    return o->overwrite > 0;
  puts(path);
  puts(" exists. Overwrite? [y/N] ");
  char line[16];
  int n = readline(line, sizeof line);
  return n > 0 && (line[0] == 'y' || line[0] == 'Y');
}

static void skip_link(const char *name, const char *kind, const char *target)
{
  puts(name);
  puts(": skipped, ");
  puts(kind);
  if (target) {
    puts(" to ");
    puts(target);
  }
  puts("\n");
}

static void extract_entry(struct rar *r, const struct rar_entry *e, const struct opts *o)
{
  const char *rel = e->name;
  if (!rar_path_is_safe(rel)) {
    report(rel, "skipped, unsafe path", 0);
    failed = true;
    return;
  }
  if (e->is_symlink || e->is_hardlink) {
    skip_link(e->name, e->is_hardlink ? "hard link" : "symbolic link", e->link);
    return;
  }
  if (o->verb == 'e' && !e->is_dir)
    rel = base_name(rel);
  if (!*rel)
    return;
  char *path = join(o->dest, rel);
  if (!path) {
    report(e->name, "out of memory", RAR_ENOMEM);
    failed = true;
    return;
  }
  if (e->is_dir) {
    if (o->verb == 'x') {
      if (make_dirs(path, true)) {
        apply_mode(path, e);
        puts("creating ");
        puts(path);
        puts("\n");
      } else {
        report(path, "cannot create directory", 0);
        failed = true;
      }
    }
    free(path);
    return;
  }
  if (e->host == 1 && (e->attr & 0xf000) == 0xa000) {
    struct linkbuf lb = {0};
    int rc = extract_retry(r, e->name, to_link, &lb, NULL);
    if (lb.p)
      lb.p[lb.n] = 0;
    skip_link(e->name, "symbolic link", rc == RAR_OK ? lb.p : NULL);
    free(lb.p);
    free(path);
    return;
  }
  struct statinfo st;
  if (stat(path, &st) == 0 && !want_overwrite(o, path)) {
    report(path, "skipped, exists", 0);
    free(path);
    return;
  }
  if (!make_dirs(path, false)) {
    report(path, "cannot create directory", 0);
    failed = true;
    free(path);
    return;
  }
  int rc = extract_retry(r, e->name, to_stream, NULL, path);
  int end = stream_end();
  if (rc == RAR_OK && end < 0)
    rc = RAR_EWRITE;
  if (rc) {
    unlink(path);
    report(path, "FAILED", rc);
    failed = true;
  } else {
    apply_mode(path, e);
    puts("extracting ");
    puts(path);
    puts("  OK\n");
  }
  free(path);
}

static void test_entry(struct rar *r, const struct rar_entry *e)
{
  if (e->is_dir || e->is_symlink || e->is_hardlink)
    return;
  int rc = extract_retry(r, e->name, to_null, NULL, NULL);
  if (rc) {
    report(e->name, "FAILED", rc);
    failed = true;
  } else {
    puts("testing ");
    puts(e->name);
    puts("  OK\n");
  }
}

static void list_entry(const struct rar_entry *e, bool verbose, uint64_t *total, uint64_t *packed)
{
  if (e->size == UINT64_MAX)
    puts("         ?");
  else
    put_u64(e->size, 10);
  puts(" ");
  put_u64(e->packed, 10);
  puts(" ");
  if (e->size && e->size != UINT64_MAX && !e->is_dir) {
    put_u64(e->packed * 100 / e->size, 4);
    puts("% ");
  } else
    puts("   -  ");
  put_date(e->mtime);
  puts(" ");
  if (e->has_crc && !e->is_dir)
    put_hex8(e->crc);
  else
    puts("        ");
  puts("  ");
  if (verbose) {
    if (e->host == 1) {
      put_num((e->mode >> 6) & 7, 1, '0');
      put_num((e->mode >> 3) & 7, 1, '0');
      put_num(e->mode & 7, 1, '0');
      puts("   ");
    } else
      puts("-     ");
    put_num(e->method, 3, ' ');
    puts(" ");
    put_num(e->version, 3, ' ');
    puts(" ");
    puts(e->encrypted ? "E" : "-");
    puts(e->solid ? "S" : "-");
    puts(e->split ? "C" : "-");
    puts(e->is_dir ? "D" : "-");
    puts(e->is_symlink || e->is_hardlink ? "L" : "-");
    puts(" ");
  }
  puts(e->name);
  if (e->link) {
    puts(" -> ");
    puts(e->link);
  }
  puts("\n");
  if (e->size != UINT64_MAX)
    *total += e->size;
  *packed += e->packed;
}

static int usage(void)
{
  puts("usage: unrar l|v|t|x|e [-p[PASSWORD]] [-o+|-o-] ARCHIVE [DEST]\n"
       "  l list  v list with details  t test  x extract with paths  e extract flat\n");
  return 2;
}

/* the rar-style verb and -p/-o switches do not fit opt.h, so the arguments are parsed by hand */
static bool parse_args(int argc, char **argv, struct opts *o)
{
  int pos = 0;
  bool rest = false;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (!rest && a[0] == '-' && a[1]) {
      if (a[1] == '-' && !a[2])
        rest = true;
      else if (a[1] == 'p')
        o->password = a[2] ? a + 2 : NULL;
      else if (a[1] == 'o' && (a[2] == '+' || a[2] == '-') && !a[3])
        o->overwrite = a[2] == '+' ? 1 : -1;
      else
        return false;
      continue;
    }
    if (pos == 0 && a[0] && !a[1] && (a[0] == 'l' || a[0] == 'v' || a[0] == 't' || a[0] == 'x' || a[0] == 'e')) {
      o->verb = a[0];
      pos++;
    } else if (pos == 1) {
      o->archive = a;
      pos++;
    } else if (pos == 2) {
      o->dest = a;
      pos++;
    } else
      return false;
  }
  return o->verb && o->archive;
}

int main(int argc, char **argv)
{
  struct opts o;
  memset(&o, 0, sizeof o);
  if (!parse_args(argc, argv, &o))
    return usage();
  cur_umask = umask((unsigned)-1);
  struct rar *r = NULL;
  int rc;
  for (int tries = 0;; tries++) {
    struct rar_src src;
    struct rar_cfg cfg;
    if (fs_open_src(o.archive, &src) != 0) {
      report(o.archive, "cannot open", 0);
      return 1;
    }
    memset(&cfg, 0, sizeof cfg);
    cfg.open_volume = fs_open_volume;
    cfg.name = o.archive;
    cfg.password = o.password;
    cfg.list_only = o.verb == 'l' || o.verb == 'v';
    rc = rar_open(&r, &src, &cfg);
    if (rc != RAR_EPASSWORD || tries >= 2)
      break;
    o.password = ask_password(o.archive);
    if (!o.password)
      break;
  }
  if (rc) {
    report(o.archive, "cannot open archive", rc);
    return 1;
  }
  bool listing = o.verb == 'l' || o.verb == 'v';
  if (listing) {
    puts("Archive: ");
    puts(o.archive);
    puts(rar_format(r) == 5 ? "  (RAR 5)" : "  (RAR 3/4)");
    if (rar_is_solid(r))
      puts("  solid");
    if (rar_is_multivolume(r))
      puts("  multi-volume");
    puts("\n      Size     Packed Ratio Date             CRC32     ");
    puts(o.verb == 'v' ? "Attr Mth Ver Flags Name\n" : "Name\n");
  }
  uint64_t total = 0, packed = 0, count = 0;
  struct rar_entry e;
  for (int pw_tries = 0;;) {
    rc = rar_next(r, &e);
    if (rc == RAR_EPASSWORD && pw_tries++ < 2) {
      const char *pw = ask_password(o.archive);
      if (pw && rar_set_password(r, pw) == RAR_OK)
        continue;
    }
    if (rc <= 0)
      break;
    count++;
    if (listing)
      list_entry(&e, o.verb == 'v', &total, &packed);
    else if (o.verb == 't')
      test_entry(r, &e);
    else
      extract_entry(r, &e, &o);
  }
  if (rc < 0) {
    report(o.archive, "error reading archive", rc);
    failed = true;
  }
  if (listing) {
    puts("---------- ---------- ----- ----------------\n");
    put_u64(total, 10);
    puts(" ");
    put_u64(packed, 10);
    puts("       ");
    put_u64(count, 0);
    puts(count == 1 ? " file\n" : " files\n");
  } else
    puts(failed ? "Errors found.\n" : "All OK.\n");
  rar_close(r);
  return failed ? 1 : 0;
}
