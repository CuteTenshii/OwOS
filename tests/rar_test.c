/* Host tests for user/lib/rar. Archives come from the test writers in rar_enc.h, rar4_enc.h and rar_ppmd_enc.h;
 * unar and bsdtar (when installed) extract the same archives as independent oracles. */
#define OWOS_SUITE_TEST
#include "rar4_enc.h"
#include "rar_enc.h"
#include "test.h"
#include <dirent.h>
#include <unistd.h>

/* ------------------------------------------------------------------ harness */

struct msrc {
  const uint8_t *p;
  size_t n;
};

static int ms_read(void *ctx, uint64_t off, void *buf, uint32_t len)
{
  struct msrc *m = ctx;
  if (off >= m->n)
    return 0;
  size_t k = m->n - (size_t)off;
  if (k > len)
    k = len;
  memcpy(buf, m->p + off, k);
  return (int)k;
}
static void ms_close(void *ctx) { free(ctx); }

static struct rar_src make_src(const uint8_t *p, size_t n)
{
  struct msrc *m = malloc(sizeof *m);
  m->p = p;
  m->n = n;
  return (struct rar_src){ms_read, ms_close, m};
}

struct vols {
  const uint8_t *p[16];
  size_t n[16];
  const char *name[16];
  int count;
};

static int vols_open(void *ctx, const char *name, struct rar_src *out)
{
  struct vols *v = ctx;
  for (int i = 0; i < v->count; i++)
    if (!strcmp(v->name[i], name)) {
      *out = make_src(v->p[i], v->n[i]);
      return 0;
    }
  return -1;
}

struct ent {
  char *name, *link;
  uint8_t *data;
  size_t len;
  int rc;
  bool dir, symlink, hardlink, encrypted, solid, split;
  uint32_t mode, mtime, crc;
  uint64_t size, packed;
  uint8_t method, version, host;
};

struct listing {
  struct ent *e;
  int n;
  int open_rc, next_rc;
  bool solid, multivol, hdr_enc;
  int fmt;
};

static void listing_free(struct listing *l)
{
  for (int i = 0; i < l->n; i++) {
    free(l->e[i].name);
    free(l->e[i].link);
    free(l->e[i].data);
  }
  free(l->e);
  memset(l, 0, sizeof *l);
}

struct sink {
  uint8_t *p;
  size_t n, cap;
};
static int sink_write(void *ctx, const void *buf, uint32_t len)
{
  struct sink *s = ctx;
  if (s->n + len > s->cap) {
    s->cap = (s->n + len) * 2 + 64;
    s->p = realloc(s->p, s->cap);
  }
  memcpy(s->p + s->n, buf, len);
  s->n += len;
  return 0;
}

/* list and extract every entry */
static void slurp(const uint8_t *arc, size_t n, struct rar_cfg cfg, struct listing *l, bool extract)
{
  memset(l, 0, sizeof *l);
  struct rar *r;
  struct rar_src src = make_src(arc, n);
  l->open_rc = rar_open(&r, &src, &cfg);
  if (l->open_rc)
    return;
  l->solid = rar_is_solid(r);
  l->multivol = rar_is_multivolume(r);
  l->hdr_enc = rar_headers_encrypted(r);
  l->fmt = rar_format(r);
  for (;;) {
    struct rar_entry e;
    int rc = rar_next(r, &e);
    if (rc <= 0) {
      l->next_rc = rc;
      break;
    }
    l->e = realloc(l->e, (size_t)(l->n + 1) * sizeof *l->e);
    struct ent *t = &l->e[l->n++];
    memset(t, 0, sizeof *t);
    t->name = strdup(e.name);
    t->link = e.link ? strdup(e.link) : NULL;
    t->dir = e.is_dir;
    t->symlink = e.is_symlink;
    t->hardlink = e.is_hardlink;
    t->encrypted = e.encrypted;
    t->solid = e.solid;
    t->split = e.split;
    t->mode = e.mode;
    t->mtime = e.mtime;
    t->crc = e.crc;
    t->size = e.size;
    t->packed = e.packed;
    t->method = e.method;
    t->version = e.version;
    t->host = e.host;
    if (extract) {
      struct sink s = {0};
      t->rc = rar_extract(r, sink_write, &s);
      t->data = s.p;
      t->len = s.n;
    }
  }
  rar_close(r);
}

static struct rar_cfg plain_cfg(const char *pw)
{
  struct rar_cfg c;
  memset(&c, 0, sizeof c);
  c.password = pw;
  return c;
}

static const struct ent *find_ent(const struct listing *l, const char *name)
{
  for (int i = 0; i < l->n; i++)
    if (!strcmp(l->e[i].name, name))
      return &l->e[i];
  return NULL;
}

static bool same(const struct listing *l, const char *name, const uint8_t *d, size_t n)
{
  const struct ent *e = find_ent(l, name);
  return e && e->rc == 0 && e->len == n && (n == 0 || !memcmp(e->data, d, n));
}

/* ------------------------------------------------------------------ test data */

static uint32_t rnd(uint32_t *s)
{
  *s = *s * 1103515245u + 12345u;
  return (*s >> 16) & 0x7fff;
}

static uint8_t *gen_text(size_t n, uint32_t seed)
{
  static const char *w[] = {"the ",     "quick ",  "brown ",  "fox ",    "jumps ", "over ",    "lazy ",    "dog ",
                            "archive ", "window ", "kernel ", "memory ", "\n",     "huffman ", "literal ", "distance "};
  uint8_t *p = malloc(n + 16);
  size_t k = 0;
  while (k < n) {
    const char *s = w[rnd(&seed) % 16];
    for (; *s && k < n; s++)
      p[k++] = (uint8_t)*s;
    if (rnd(&seed) % 29 == 0 && k < n)
      p[k++] = (uint8_t)rnd(&seed);
  }
  return p;
}

static uint8_t *gen_random(size_t n, uint32_t seed)
{
  uint8_t *p = malloc(n + 16);
  for (size_t i = 0; i < n; i++)
    p[i] = (uint8_t)rnd(&seed);
  return p;
}

static uint8_t *gen_x86(size_t n, uint32_t seed)
{
  uint8_t *p = malloc(n + 16);
  size_t k = 0;
  while (k < n) {
    unsigned r = rnd(&seed) % 8;
    if (r < 3 && k + 5 <= n) {
      p[k++] = r == 0 ? 0xe8 : r == 1 ? 0xe9 : 0xe8;
      int32_t rel = (int32_t)(rnd(&seed) * 37 % 0x300000) - 0x1000 * (int32_t)(rnd(&seed) % 4);
      if (rnd(&seed) % 7 == 0)
        rel = (int32_t)(0x7ff00000 + rnd(&seed));
      for (int i = 0; i < 4; i++)
        p[k++] = (uint8_t)((uint32_t)rel >> (8 * i));
    } else
      p[k++] = (uint8_t)rnd(&seed);
  }
  return p;
}

static uint8_t *gen_audio(size_t n, uint32_t seed, int ch)
{
  uint8_t *p = malloc(n + 16);
  for (size_t i = 0; i < n; i++) {
    int c = (int)(i % (size_t)ch);
    p[i] = (uint8_t)(128 + (int)(100.0 * __builtin_sin((double)(i / (size_t)ch) * (0.01 + 0.003 * c))) +
                     (int)(rnd(&seed) % 5));
  }
  return p;
}

/* ------------------------------------------------------------------ oracles */

static bool have_tool(const char *name)
{
  char cmd[256];
  snprintf(cmd, sizeof cmd, "command -v %s >/dev/null 2>&1", name);
  return system(cmd) == 0;
}

static char *make_tmpdir(void)
{
  const char *t = getenv("TMPDIR");
  char *d = malloc(512);
  snprintf(d, 512, "%s/rar-test.XXXXXX", t && *t ? t : "/tmp");
  char *dbl = strstr(d, "//");
  if (dbl)
    memmove(dbl, dbl + 1, strlen(dbl));
  if (!mkdtemp(d)) {
    free(d);
    return NULL;
  }
  return d;
}

static void rm_rf(const char *dir)
{
  char cmd[600];
  snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
  if (strncmp(dir, "/", 1) == 0 && strlen(dir) > 8)
    (void)!system(cmd);
}

static uint8_t *slurp_file(const char *path, size_t *n)
{
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *p = malloc((size_t)sz + 1);
  *n = fread(p, 1, (size_t)sz, f);
  fclose(f);
  return p;
}

/* Extract `arc` with an external tool into a fresh directory and compare to the expected members. */
struct want {
  const char *name;
  const uint8_t *data;
  size_t len;
  bool dir;
};

struct vfile {
  const char *name;
  const uint8_t *data;
  size_t len;
};

static char g_label[96] = "x";

static bool oracle_check_v(const char *tool, const struct vfile *vf, int nvf, const char *pw, const struct want *w,
                           int nw)
{
  char *dir = make_tmpdir();
  if (!dir)
    return false;
  char path[700], out[700], cmd[1600];
  const char *arcname = vf[0].name;
  snprintf(out, sizeof out, "%s/out.XXXXXX", dir);
  if (!mkdtemp(out))
    return false;
  for (int i = 0; i < nvf; i++) {
    snprintf(path, sizeof path, "%s/%s", dir, vf[i].name);
    FILE *f = fopen(path, "wb");
    fwrite(vf[i].data, 1, vf[i].len, f);
    fclose(f);
  }
  snprintf(path, sizeof path, "%s/%s", dir, arcname);
  if (!strcmp(tool, "unar"))
    snprintf(cmd, sizeof cmd, "env -u DYLD_INSERT_LIBRARIES timeout 60 unar -q -D -f %s%s%s -o '%s' '%s' >%s 2>&1",
             pw ? "-p '" : "", pw ? pw : "", pw ? "'" : "", out, path, "/dev/null");
  else
    snprintf(cmd, sizeof cmd,
             "env -u DYLD_INSERT_LIBRARIES LC_ALL=en_US.UTF-8 timeout 60 bsdtar -xf '%s' -C '%s' >/dev/null 2>&1", path,
             out);
  int rc = system(cmd);
  bool ok = rc == 0;
  for (int i = 0; i < nw && ok; i++) {
    char fp[1400];
    snprintf(fp, sizeof fp, "%s/%s", out, w[i].name);
    if (w[i].dir) {
      DIR *dp = opendir(fp);
      ok = dp != NULL;
      if (dp)
        closedir(dp);
      continue;
    }
    size_t got;
    uint8_t *d = slurp_file(fp, &got);
    ok = d && got == w[i].len && (got == 0 || !memcmp(d, w[i].data, got));
    if (!ok)
      fprintf(stderr, "oracle %s: mismatch on %s (%s) got %zu want %zu\n", tool, w[i].name, arcname, d ? got : 0,
              w[i].len);
    free(d);
  }
  if (!ok && rc != 0)
    fprintf(stderr, "oracle %s: exit %d on %s\n", tool, rc, arcname);
  if (!ok && getenv("RAR_DUMP")) {
    snprintf(cmd, sizeof cmd, "for f in '%s'/*.rar; do b=$(basename $f); cp $f \"%s/%s-$b\"; done", dir,
             getenv("RAR_DUMP"), g_label);
    (void)!system(cmd);
  }
  if (getenv("RAR_KEEP"))
    fprintf(stderr, "kept %s\n", dir);
  else
    rm_rf(dir);
  free(dir);
  return ok;
}

static bool oracle_check(const char *tool, const char *arcname, const uint8_t *arc, size_t n, const char *pw,
                         const struct want *w, int nw)
{
  struct vfile v = {arcname, arc, n};
  return oracle_check_v(tool, &v, 1, pw, w, nw);
}

static bool have_unar, have_bsdtar;

/* ------------------------------------------------------------------ RAR5 cases */

struct case5 {
  const char *label;
  struct r5f f[6];
  int nf;
  unsigned arc_flags;
  bool bsdtar_ok;
};

static void expect_oracles(const char *label, const uint8_t *arc, size_t n, const char *arcname, const char *pw,
                           const struct want *w, int nw, bool bsdtar_ok)
{
  snprintf(g_label, sizeof g_label, "%s", label);
  for (char *p = g_label; *p; p++)
    if (*p == ' ')
      *p = '_';
  if (have_unar)
    if (!oracle_check("unar", arcname, arc, n, pw, w, nw)) {
      printf("  oracle unar failed: %s\n", label);
      expect(0 && "unar");
    }
  if (have_bsdtar && bsdtar_ok)
    if (!oracle_check("bsdtar", arcname, arc, n, NULL, w, nw)) {
      printf("  oracle bsdtar failed: %s\n", label);
      expect(0 && "bsdtar");
    }
}

static void r5_roundtrip(const char *label, struct r5a *a, const struct r5f *files, int nf, const char *pw,
                         bool bsdtar_ok)
{
  r5a_end(a, false);
  struct listing l;
  slurp(a->out.p, a->out.n, plain_cfg(pw), &l, true);
  bool ok = l.open_rc == 0 && l.next_rc == 0 && l.n == nf;
  if (!ok)
    printf("  %s: open=%d next=%d n=%d\n", label, l.open_rc, l.next_rc, l.n);
  expect(ok);
  struct want w[16];
  int nw = 0;
  for (int i = 0; i < nf && ok; i++) {
    const struct r5f *f = &files[i];
    if (f->link)
      continue;
    bool same_ok = f->is_dir ? find_ent(&l, f->name) && find_ent(&l, f->name)->dir : same(&l, f->name, f->data, f->len);
    if (!same_ok)
      printf("  %s: entry %s differs (rc=%d)\n", label, f->name,
             find_ent(&l, f->name) ? find_ent(&l, f->name)->rc : -99);
    expect(same_ok);
    w[nw++] = (struct want){f->name, f->data, f->len, f->is_dir};
  }
  expect_oracles(label, a->out.p, a->out.n, "t.rar", pw, w, nw, bsdtar_ok);
  listing_free(&l);
}

static void finish5(struct r5a *a)
{
  eb_free(&a->out);
  eb_free(&a->st.hist);
}

static void test_rar5_basic(void)
{
  uint8_t *text = gen_text(70000, 1), *rndm = gen_random(3000, 2);
  struct r5a a = {{0}};
  r5a_begin(&a, 0, 0, NULL, 0);
  struct r5f f[4];
  memset(f, 0, sizeof f);
  f[0] =
      (struct r5f){.name = "stored.txt", .data = text, .len = 5000, .method = 0, .mtime = 1700000000, .attr = 0100644};
  f[1] = (struct r5f){.name = "dir", .is_dir = true, .attr = 040755, .mtime = 1700000000};
  f[2] =
      (struct r5f){.name = "dir/lz.txt", .data = text, .len = 70000, .method = 3, .attr = 0100600, .mtime = 1700000001};
  f[3] = (struct r5f){.name = "rand.bin", .data = rndm, .len = 3000, .method = 3, .attr = 0100644};
  for (int i = 0; i < 4; i++)
    r5a_add(&a, &f[i]);
  r5_roundtrip("rar5 basic", &a, f, 4, NULL, true);
  struct listing l;
  slurp(a.out.p, a.out.n, plain_cfg(NULL), &l, false);
  const struct ent *e = find_ent(&l, "dir/lz.txt");
  expect(e && e->host == 1 && e->mode == 0600 && e->mtime == 1700000001 && e->size == 70000 && e->method == 3 &&
         e->version == 50 && !e->encrypted && !e->solid);
  e = find_ent(&l, "dir");
  expect(e && e->dir);
  listing_free(&l);
  free(text);
  free(rndm);
  finish5(&a);
}

static void test_rar5_blocks_solid(void)
{
  uint8_t *t1 = gen_text(90000, 3), *t2 = gen_text(60000, 3), *r = gen_random(2000, 4);
  for (int mode = 0; mode < 4; mode++) {
    struct r5a a = {{0}};
    r5a_begin(&a, 4, 0, NULL, 0);
    struct r5f f[4];
    memset(f, 0, sizeof f);
    bool uni = mode & 1;
    int bt = mode & 2 ? 150 : 0;
    f[0] = (struct r5f){.name = "a.txt", .data = t1, .len = 90000, .method = 3, .universal = uni, .block_tokens = bt};
    f[1] = (struct r5f){
        .name = "b.txt", .data = t2, .len = 60000, .method = 3, .solid = true, .universal = uni, .block_tokens = bt};
    f[2] = (struct r5f){
        .name = "c.bin", .data = r, .len = 2000, .method = 3, .solid = true, .universal = uni, .block_tokens = bt};
    f[3] = (struct r5f){
        .name = "d.txt", .data = t1, .len = 40000, .method = 3, .solid = true, .universal = uni, .block_tokens = bt};
    for (int i = 0; i < 4; i++)
      r5a_add(&a, &f[i]);
    char label[64];
    snprintf(label, sizeof label, "rar5 solid mode %d", mode);
    r5_roundtrip(label, &a, f, 4, NULL, true);
    struct listing l;
    slurp(a.out.p, a.out.n, plain_cfg(NULL), &l, false);
    expect(l.solid && l.n == 4 && !l.e[0].solid && l.e[1].solid);
    listing_free(&l);
    finish5(&a);
  }
  free(t1);
  free(t2);
  free(r);
}

static void test_rar5_filters(void)
{
  size_t n = 120000;
  uint8_t *x86 = gen_x86(n, 5), *aud = gen_audio(40000, 6, 2), *arm = gen_random(30000, 7);
  for (size_t i = 3; i < 30000; i += 4)
    if (rnd(&(uint32_t){(uint32_t)i}) % 3 == 0)
      arm[i] = 0xeb;
  struct filt fe8[] = {{0, 0, 120000, 1, 0}};
  struct filt fe9[] = {{0, 1000, 50000, 2, 0}, {30000, 60000, 40000, 1, 0}};
  struct filt fdl[] = {{0, 0, 40000, 0, 2}};
  struct filt fdl3[] = {{100, 4000, 30000, 0, 3}};
  struct filt farm[] = {{0, 0, 30000, 3, 0}};
  struct r5f f[6];
  memset(f, 0, sizeof f);
  f[0] = (struct r5f){.name = "e8.bin", .data = x86, .len = n, .method = 3, .filters = fe8, .nfilters = 1};
  f[1] = (struct r5f){
      .name = "e9.bin", .data = x86, .len = n, .method = 3, .filters = fe9, .nfilters = 2, .block_tokens = 500};
  f[2] = (struct r5f){.name = "delta2.raw", .data = aud, .len = 40000, .method = 3, .filters = fdl, .nfilters = 1};
  f[3] = (struct r5f){.name = "delta3.raw", .data = aud, .len = 40000, .method = 3, .filters = fdl3, .nfilters = 1};
  f[4] = (struct r5f){.name = "arm.bin", .data = arm, .len = 30000, .method = 3, .filters = farm, .nfilters = 1};
  /* libarchive hangs on archives with E8 filters, so filtered archives go to unar only */
  int plain[3] = {0, 0, 0}, late[4] = {1, 2, 3, 4};
  struct r5a a = {{0}};
  r5a_begin(&a, 0, 0, NULL, 0);
  struct r5f g[3];
  for (int i = 0; i < 3; i++)
    g[i] = f[plain[i]];
  r5a_add(&a, &g[0]);
  r5_roundtrip("rar5 filters", &a, g, 1, NULL, false);
  finish5(&a);
  struct r5a b = {{0}};
  r5a_begin(&b, 0, 0, NULL, 0);
  struct r5f h[4];
  for (int i = 0; i < 4; i++) {
    h[i] = f[late[i]];
    r5a_add(&b, &h[i]);
  }
  r5_roundtrip("rar5 late filters", &b, h, 4, NULL, false);
  finish5(&b);
  struct r5a c = {{0}};
  r5a_begin(&c, 4, 0, NULL, 0);
  struct r5f s3[3] = {f[0], f[2], f[4]};
  s3[1].solid = s3[2].solid = true;
  for (int i = 0; i < 3; i++)
    r5a_add(&c, &s3[i]);
  r5_roundtrip("rar5 filters solid", &c, s3, 3, NULL, false);
  finish5(&c);
  free(x86);
  free(aud);
  free(arm);
}

static void test_rar5_distances(void)
{
  size_t n = 700000;
  uint8_t *d = gen_random(n, 8);
  memcpy(d + 520000, d + 1000, 150000);
  memcpy(d + 300000, d + 100, 20000);
  struct r5a a = {{0}};
  r5a_begin(&a, 0, 0, NULL, 0);
  struct r5f f[2];
  memset(f, 0, sizeof f);
  f[0] = (struct r5f){.name = "far.bin", .data = d, .len = n, .method = 5, .dict_shift = 4, .block_tokens = 3000};
  r5a_add(&a, &f[0]);
  r5_roundtrip("rar5 far matches", &a, f, 1, NULL, true);
  struct listing l;
  slurp(a.out.p, a.out.n, plain_cfg(NULL), &l, false);
  expect(l.n == 1 && l.e[0].packed < n);
  listing_free(&l);
  finish5(&a);
  free(d);
}

static void test_rar5_names_links(void)
{
  uint8_t text[] = "tiny";
  struct r5a a = {{0}};
  r5a_begin(&a, 0, 0, NULL, 0);
  struct r5f f[5];
  memset(f, 0, sizeof f);
  f[0] = (struct r5f){.name = "caf\xc3\xa9/\xe6\x97\xa5\xe6\x9c\xac.txt", .data = text, .len = 4, .method = 0};
  f[1] = (struct r5f){.name = "empty", .data = text, .len = 0, .method = 0};
  f[2] =
      (struct r5f){.name = "link", .link = "caf\xc3\xa9/\xe6\x97\xa5\xe6\x9c\xac.txt", .link_type = 1, .attr = 0120777};
  f[3] = (struct r5f){.name = "hl", .link = "empty", .link_type = 4};
  f[4] = (struct r5f){.name = "hashed.txt", .data = text, .len = 4, .method = 0, .b2hash = true};
  for (int i = 0; i < 5; i++)
    r5a_add(&a, &f[i]);
  r5a_end(&a, false);
  struct listing l;
  slurp(a.out.p, a.out.n, plain_cfg(NULL), &l, true);
  expect(l.n == 5 && l.next_rc == 0);
  expect(same(&l, "caf\xc3\xa9/\xe6\x97\xa5\xe6\x9c\xac.txt", text, 4));
  expect(same(&l, "empty", text, 0));
  expect(same(&l, "hashed.txt", text, 4));
  const struct ent *e = find_ent(&l, "link");
  expect(e && e->symlink && !e->hardlink && e->link && !strcmp(e->link, "caf\xc3\xa9/\xe6\x97\xa5\xe6\x9c\xac.txt") &&
         e->len == 0);
  e = find_ent(&l, "hl");
  expect(e && e->hardlink && !e->symlink && e->link && !strcmp(e->link, "empty"));
  struct want w[] = {{"caf\xc3\xa9/\xe6\x97\xa5\xe6\x9c\xac.txt", text, 4, false}, {"hashed.txt", text, 4, false}};
  expect_oracles("rar5 names", a.out.p, a.out.n, "t.rar", NULL, w, 2, true);
  listing_free(&l);
  finish5(&a);
}

static void test_rar5_volumes(int method)
{
  uint8_t *d = gen_text(200000, 9), *s = gen_text(3000, 10);
  struct r5a st = {{0}};
  struct r5f f = {.name = "big.txt", .data = d, .len = 200000, .method = method, .block_tokens = 800};
  struct eb packed = {0};
  r5_pack(&st, &f, &packed);
  size_t cuts[3] = {packed.n / 3, packed.n / 3, packed.n - 2 * (packed.n / 3)};
  if (st.st.nbends > 3) {
    size_t c1 = 0, c2 = 0;
    for (int i = 0; i < st.st.nbends; i++) {
      if (!c1 && st.st.bends[i] >= packed.n / 3)
        c1 = st.st.bends[i];
      if (!c2 && st.st.bends[i] >= 2 * (packed.n / 3))
        c2 = st.st.bends[i];
    }
    if (c1 && c2 && c2 > c1 && c2 < packed.n) {
      cuts[0] = c1;
      cuts[1] = c2 - c1;
      cuts[2] = packed.n - c2;
    }
  }
  struct r5a v[3] = {{{0}}, {{0}}, {{0}}};
  size_t pos = 0;
  for (int i = 0; i < 3; i++) {
    r5a_begin(&v[i], i ? 3 : 1, (unsigned)i, NULL, 0);
    if (i == 0) {
      struct r5f pre = {.name = "pre.txt", .data = s, .len = 3000, .method = 0};
      struct eb pk = {0};
      eb_put(&pk, s, 3000);
      r5a_put_file(&v[i], &pre, pk.p, pk.n, false, false);
      eb_free(&pk);
    }
    f.crc_in_parts = false;
    r5a_put_file(&v[i], &f, packed.p + pos, cuts[i], i > 0, i < 2);
    pos += cuts[i];
    r5a_end(&v[i], i < 2);
  }
  if (getenv("RAR_DUMP")) {
    char p[300];
    snprintf(p, sizeof p, "%s/big.exp", getenv("RAR_DUMP"));
    FILE *ef = fopen(p, "wb");
    fwrite(d, 1, 200000, ef);
    fclose(ef);
  }
  {
    struct r5a one = {{0}};
    r5a_begin(&one, 0, 0, NULL, 0);
    r5a_add(&one, &f);
    r5a_end(&one, false);
    struct want w1[] = {{"big.txt", d, 200000, false}};
    expect_oracles("rar5 single 200000", one.out.p, one.out.n, "t.rar", NULL, w1, 1, false);
    finish5(&one);
  }
  const char *names[3] = {"t.part1.rar", "t.part2.rar", "t.part3.rar"};
  struct vols vs = {
      {v[0].out.p, v[1].out.p, v[2].out.p}, {v[0].out.n, v[1].out.n, v[2].out.n}, {names[0], names[1], names[2]}, 3};
  struct rar_cfg cfg = plain_cfg(NULL);
  cfg.open_volume = vols_open;
  cfg.ctx = &vs;
  cfg.name = names[0];
  struct listing l;
  slurp(v[0].out.p, v[0].out.n, cfg, &l, true);
  expect(l.open_rc == 0 && l.next_rc == 0 && l.n == 2 && l.multivol);
  expect(same(&l, "big.txt", d, 200000) && same(&l, "pre.txt", s, 3000));
  const struct ent *e = find_ent(&l, "big.txt");
  expect(e && e->split && e->packed == packed.n);
  listing_free(&l);
  /* missing middle volume */
  vs.count = 1;
  slurp(v[0].out.p, v[0].out.n, cfg, &l, true);
  expect(l.n == 2 && l.e[1].rc == RAR_EVOLUME && l.next_rc == RAR_EVOLUME);
  listing_free(&l);
  vs.count = 3;
  struct vfile vf[3] = {
      {names[0], v[0].out.p, v[0].out.n}, {names[1], v[1].out.p, v[1].out.n}, {names[2], v[2].out.p, v[2].out.n}};
  struct want w[] = {{"big.txt", d, 200000, false}, {"pre.txt", s, 3000, false}};
  if (have_unar && !oracle_check_v("unar", vf, 3, NULL, w, 2)) {
    printf("  oracle unar failed: rar5 volumes\n");
    expect(0 && "unar volumes");
  }
  for (int i = 0; i < 3; i++)
    finish5(&v[i]);
  finish5(&st);
  eb_free(&packed);
  free(d);
  free(s);
}

#ifdef RAR_ENC_CRYPTO
static void test_rar5_crypto(void)
{
  uint8_t *t = gen_text(50000, 11), *sm = gen_text(900, 12);
  for (int mode = 0; mode < 4; mode++) {
    struct r5a a = {{0}};
    bool hdr = mode == 3;
    r5a_begin(&a, 0, 0, hdr ? "s3cret" : NULL, 5);
    struct r5f f[3];
    memset(f, 0, sizeof f);
    const char *pw = "s3cret";
    f[0] = (struct r5f){.name = "enc.txt",
                        .data = t,
                        .len = 50000,
                        .method = 3,
                        .password = pw,
                        .lg2 = 5,
                        .pswcheck = mode != 1,
                        .tweak = mode == 2};
    f[1] = (struct r5f){.name = "enc_stored.bin",
                        .data = sm,
                        .len = 900,
                        .method = 0,
                        .password = pw,
                        .lg2 = 6,
                        .pswcheck = mode != 1,
                        .tweak = mode == 2,
                        .b2hash = mode == 2};
    f[2] = (struct r5f){.name = "plain.txt", .data = sm, .len = 900, .method = 0};
    for (int i = 0; i < 3; i++)
      r5a_add(&a, &f[i]);
    char label[64];
    snprintf(label, sizeof label, "rar5 crypto mode %d", mode);
    r5a_end(&a, false);
    struct listing l;
    slurp(a.out.p, a.out.n, plain_cfg(pw), &l, true);
    expect(l.open_rc == 0 && l.next_rc == 0 && l.n == 3);
    expect(same(&l, "enc.txt", t, 50000) && same(&l, "enc_stored.bin", sm, 900) && same(&l, "plain.txt", sm, 900));
    for (int i = 0; i < l.n; i++)
      if (l.e[i].rc)
        printf("  mode %d %s rc=%d\n", mode, l.e[i].name, l.e[i].rc);
    expect(l.hdr_enc == hdr);
    expect(l.n < 1 || l.e[0].encrypted);
    listing_free(&l);
    /* wrong and missing password */
    slurp(a.out.p, a.out.n, plain_cfg("wrong"), &l, true);
    if (hdr)
      expect(l.open_rc == RAR_EPASSWORD);
    else if (mode == 1)
      expect(l.n == 3 && (l.e[0].rc == RAR_EPASSWORD || l.e[0].rc == RAR_ECRC || l.e[0].rc == RAR_ECORRUPT) &&
             l.e[2].rc == 0);
    else
      expect(l.n == 3 && l.e[0].rc == RAR_EPASSWORD && l.e[1].rc == RAR_EPASSWORD && l.e[2].rc == 0);
    listing_free(&l);
    slurp(a.out.p, a.out.n, plain_cfg(NULL), &l, true);
    if (hdr)
      expect(l.open_rc == RAR_EPASSWORD);
    else
      expect(l.n == 3 && l.e[0].rc == RAR_EPASSWORD && l.e[2].rc == 0);
    listing_free(&l);
    struct want w[] = {{"enc.txt", t, 50000, false}, {"enc_stored.bin", sm, 900, false}, {"plain.txt", sm, 900, false}};
    expect_oracles(label, a.out.p, a.out.n, "t.rar", pw, w, 3, false);
    finish5(&a);
  }
  free(t);
  free(sm);
}
#endif

/* ------------------------------------------------------------------ RAR 3/4 cases */

static void finish4(struct r4a *a)
{
  eb_free(&a->out);
  eb_free(&a->st.hist);
}

static void r4_roundtrip(const char *label, struct r4a *a, const struct r4f *files, int nf, const char *pw,
                         bool bsdtar_ok)
{
  r4a_end(a, false);
  struct listing l;
  slurp(a->out.p, a->out.n, plain_cfg(pw), &l, true);
  bool ok = l.open_rc == 0 && l.next_rc == 0 && l.n == nf && l.fmt == 4;
  if (!ok)
    printf("  %s: open=%d next=%d n=%d\n", label, l.open_rc, l.next_rc, l.n);
  expect(ok);
  struct want w[16];
  int nw = 0;
  for (int i = 0; i < nf && ok; i++) {
    const struct r4f *f = &files[i];
    bool same_ok = f->is_dir ? l.e[i].dir : same(&l, l.e[i].name, f->data, f->len);
    if (!same_ok)
      printf("  %s: entry %d (%s) differs (rc=%d)\n", label, i, l.e[i].name, l.e[i].rc);
    expect(same_ok);
    w[nw++] = (struct want){l.e[i].name, f->data, f->len, f->is_dir};
  }
  expect_oracles(label, a->out.p, a->out.n, "t.rar", pw, w, nw, bsdtar_ok);
  listing_free(&l);
}

/* libarchive does not reset the low-distance state at a new table block, so multi-block RAR4 streams go to unar only */
static void test_rar4_basic(void)
{
  uint8_t *text = gen_text(150000, 21), *rndm = gen_random(3000, 22);
  struct r4a a = {{0}};
  r4a_begin(&a, 0, NULL);
  struct r4f f[6];
  memset(f, 0, sizeof f);
  f[0] = (struct r4f){.name = "stored.txt", .data = text, .len = 5000, .method = 0};
  f[1] = (struct r4f){.name = "dir", .is_dir = true};
  f[2] = (struct r4f){.name = "dir/lz.txt", .data = text, .len = 150000, .method = 3};
  f[3] = (struct r4f){.name = "rand.bin", .data = rndm, .len = 3000, .method = 3};
  f[4] = (struct r4f){.name = "caf\xc3\xa9\xe6\x97\xa5.txt", .data = text, .len = 100, .method = 0, .unicode = true};
  for (int i = 0; i < 5; i++)
    r4a_add(&a, &f[i], false);
  r4_roundtrip("rar4 basic", &a, f, 5, NULL, true);
  struct listing l;
  slurp(a.out.p, a.out.n, plain_cfg(NULL), &l, false);
  const struct ent *e = find_ent(&l, "dir/lz.txt");
  expect(e && e->host == 1 && e->mode == 0644 && e->size == 150000 && e->method == 3 && e->version == 29 && !e->solid);
  expect(e && e->mtime == 1684326614);
  listing_free(&l);
  finish4(&a);
  free(text);
  free(rndm);
}

static void test_rar4_solid(void)
{
  uint8_t *t1 = gen_text(80000, 23), *t2 = gen_text(50000, 23), *r = gen_random(1500, 24);
  for (int mode = 0; mode < 2; mode++) {
    struct r4a a = {{0}};
    r4a_begin(&a, 8, NULL);
    bool uni = mode == 1;
    struct r4f f[4];
    memset(f, 0, sizeof f);
    f[0] = (struct r4f){.name = "a.txt",
                        .data = t1,
                        .len = 80000,
                        .method = 3,
                        .universal = uni,
                        .end_keep = uni,
                        .block_tokens = 2500};
    f[1] = (struct r4f){.name = "b.txt",
                        .data = t2,
                        .len = 50000,
                        .method = 3,
                        .solid = true,
                        .universal = uni,
                        .end_keep = uni,
                        .block_tokens = 2500};
    f[2] = (struct r4f){
        .name = "c.bin", .data = r, .len = 1500, .method = 3, .solid = true, .universal = uni, .end_keep = uni};
    f[3] = (struct r4f){.name = "d.txt", .data = t1, .len = 30000, .method = 3, .solid = true, .universal = uni};
    for (int i = 0; i < 4; i++)
      r4a_add(&a, &f[i], false);
    char label[48];
    snprintf(label, sizeof label, "rar4 solid mode %d", mode);
    r4_roundtrip(label, &a, f, 4, NULL, false);
    struct listing l;
    slurp(a.out.p, a.out.n, plain_cfg(NULL), &l, false);
    expect(l.solid && !l.e[0].solid && l.e[1].solid);
    listing_free(&l);
    finish4(&a);
  }
  free(t1);
  free(t2);
  free(r);
}

static void test_rar4_distances(void)
{
  size_t n = 900000;
  uint8_t *d = gen_random(n, 25);
  memcpy(d + 600000, d + 1000, 150000);
  memcpy(d + 300000, d + 100, 20000);
  memcpy(d + 800000, d + 799000, 1000);
  struct r4a a = {{0}};
  r4a_begin(&a, 0, NULL);
  struct r4f f[2];
  memset(f, 0, sizeof f);
  f[0] = (struct r4f){.name = "far.bin", .data = d, .len = n, .method = 5, .block_tokens = 6000};
  r4a_add(&a, &f[0], false);
  r4_roundtrip("rar4 far matches", &a, f, 1, NULL, false);
  finish4(&a);
  free(d);
}

#ifdef RAR_ENC_CRYPTO
static void test_rar4_crypto(void)
{
  uint8_t *t = gen_text(30000, 26), *sm = gen_text(700, 27);
  for (int mode = 0; mode < 2; mode++) {
    struct r4a a = {{0}};
    const char *pw = "p\xc3\xa4ss";
    pw = "pass";
    r4a_begin(&a, 0, mode ? pw : NULL);
    struct r4f f[2];
    memset(f, 0, sizeof f);
    f[0] = (struct r4f){.name = "enc.txt", .data = t, .len = 30000, .method = 3, .password = pw};
    f[1] = (struct r4f){.name = "enc_stored.bin", .data = sm, .len = 700, .method = 0, .password = pw};
    for (int i = 0; i < 2; i++)
      r4a_add(&a, &f[i], false);
    char label[48];
    snprintf(label, sizeof label, "rar4 crypto mode %d", mode);
    r4a_end(&a, false);
    struct listing l;
    slurp(a.out.p, a.out.n, plain_cfg(pw), &l, true);
    expect(l.open_rc == 0 && l.next_rc == 0 && l.n == 2);
    expect(same(&l, "enc.txt", t, 30000) && same(&l, "enc_stored.bin", sm, 700));
    expect(l.hdr_enc == (mode == 1) && l.n && l.e[0].encrypted);
    listing_free(&l);
    slurp(a.out.p, a.out.n, plain_cfg("wrong"), &l, true);
    if (mode)
      expect(l.open_rc == RAR_EPASSWORD || l.next_rc == RAR_EPASSWORD);
    else
      expect(l.n == 2 && l.e[0].rc == RAR_EPASSWORD && l.e[1].rc == RAR_EPASSWORD);
    listing_free(&l);
    slurp(a.out.p, a.out.n, plain_cfg(NULL), &l, true);
    expect(l.open_rc == RAR_EPASSWORD || (l.n == 2 && l.e[0].rc == RAR_EPASSWORD));
    listing_free(&l);
    struct want w[] = {{"enc.txt", t, 30000, false}, {"enc_stored.bin", sm, 700, false}};
    expect_oracles(label, a.out.p, a.out.n, "t.rar", pw, w, 2, false);
    finish4(&a);
  }
  free(t);
  free(sm);
}
#endif

static void test_rar4_volumes(bool new_naming, int method)
{
  uint8_t *d = gen_text(120000, 28);
  struct r4a st = {{0}};
  struct r4f f = {.name = "big.txt", .data = d, .len = 120000, .method = method, .block_tokens = 3000};
  struct eb packed = {0};
  if (method)
    r4_pack_lz(&st, &f, &packed, true);
  else
    eb_put(&packed, d, 120000);
  size_t c1 = packed.n / 3, c2 = packed.n / 3, c3 = packed.n - 2 * (packed.n / 3);
  size_t cuts[3] = {c1, c2, c3};
  struct r4a v[3] = {{{0}}, {{0}}, {{0}}};
  size_t pos = 0;
  for (int i = 0; i < 3; i++) {
    r4a_begin(&v[i], 1 | (i == 0 ? 0x100 : 0) | (new_naming ? 0x10 : 0), NULL);
    f.part_crc = i < 2;
    r4a_put_file(&v[i], &f, packed.p + pos, cuts[i], i > 0, i < 2);
    pos += cuts[i];
    r4a_end(&v[i], i < 2);
  }
  const char *names_new[3] = {"t.part1.rar", "t.part2.rar", "t.part3.rar"};
  const char *names_old[3] = {"t.rar", "t.r00", "t.r01"};
  const char **names = new_naming ? names_new : names_old;
  struct vols vs = {
      {v[0].out.p, v[1].out.p, v[2].out.p}, {v[0].out.n, v[1].out.n, v[2].out.n}, {names[0], names[1], names[2]}, 3};
  struct rar_cfg cfg = plain_cfg(NULL);
  cfg.open_volume = vols_open;
  cfg.ctx = &vs;
  cfg.name = names[0];
  struct listing l;
  slurp(v[0].out.p, v[0].out.n, cfg, &l, true);
  expect(l.open_rc == 0 && l.next_rc == 0 && l.n == 1 && l.multivol);
  expect(same(&l, "big.txt", d, 120000) && l.n && l.e[0].split && l.e[0].packed == packed.n);
  listing_free(&l);
  struct vfile vf[3] = {
      {names[0], v[0].out.p, v[0].out.n}, {names[1], v[1].out.p, v[1].out.n}, {names[2], v[2].out.p, v[2].out.n}};
  struct want w[] = {{"big.txt", d, 120000, false}};
  if (have_unar && !oracle_check_v("unar", vf, 3, NULL, w, 1)) {
    printf("  oracle unar failed: rar4 volumes\n");
    expect(0 && "unar rar4 volumes");
  }
  for (int i = 0; i < 3; i++)
    finish4(&v[i]);
  finish4(&st);
  eb_free(&packed);
  free(d);
}

/* ------------------------------------------------------------------ PPMd model against 7z streams */

struct rc7z {
  struct ppmd_rc rc;
  const uint8_t *p;
  size_t n, pos;
  uint32_t range, code;
};

static uint32_t rc7_byte(struct rc7z *c) { return c->pos < c->n ? c->p[c->pos++] : 0; }
static void rc7_norm(struct rc7z *c)
{
  if (c->range < (1u << 24)) {
    c->code = (c->code << 8) | rc7_byte(c);
    c->range <<= 8;
    if (c->range < (1u << 24)) {
      c->code = (c->code << 8) | rc7_byte(c);
      c->range <<= 8;
    }
  }
}
static uint32_t rc7_threshold(struct ppmd_rc *r, uint32_t total)
{
  struct rc7z *c = (struct rc7z *)r;
  c->range /= total;
  return c->code / c->range;
}
static void rc7_decode(struct ppmd_rc *r, uint32_t start, uint32_t size)
{
  struct rc7z *c = (struct rc7z *)r;
  c->code -= start * c->range;
  c->range *= size;
  rc7_norm(c);
}
static uint32_t rc7_bit(struct ppmd_rc *r, uint32_t size0, uint32_t total)
{
  struct rc7z *c = (struct rc7z *)r;
  uint32_t bound = (c->range / total) * size0, sym;
  if (c->code < bound) {
    sym = 0;
    c->range = bound;
  } else {
    sym = 1;
    c->code -= bound;
    c->range -= bound;
  }
  rc7_norm(c);
  return sym;
}

/* libarchive writes 7z with PPMd7 (order 6, 16 MiB), the reference implementation of the same model */
static void test_ppmd_vs_7z(size_t n, int kind)
{
  if (!have_bsdtar)
    return;
  char *dir = make_tmpdir();
  char path[700], cmd[1500];
  uint8_t *d = kind == 0 ? gen_text(n, 31) : kind == 1 ? gen_random(n, 32) : gen_x86(n, 33);
  if (kind == 1)
    for (size_t i = 0; i < n; i += 3)
      d[i] = (uint8_t)(d[i] & 0x1f);
  snprintf(path, sizeof path, "%s/x.bin", dir);
  FILE *f = fopen(path, "wb");
  fwrite(d, 1, n, f);
  fclose(f);
  snprintf(cmd, sizeof cmd,
           "cd '%s' && env -u DYLD_INSERT_LIBRARIES bsdtar --format 7zip --options 7zip:compression=ppmd -cf x.7z "
           "x.bin >/dev/null 2>&1",
           dir);
  int rc = system(cmd);
  snprintf(path, sizeof path, "%s/x.7z", dir);
  size_t sz = 0;
  uint8_t *z = rc == 0 ? slurp_file(path, &sz) : NULL;
  expect(z && sz > 40);
  if (z && sz > 40) {
    uint64_t next = 0;
    for (int i = 0; i < 8; i++)
      next |= (uint64_t)z[12 + i] << (8 * i);
    struct ppmd *p = ppmd_new(16u << 20);
    ppmd_init(p, 6);
    struct rc7z c = {{rc7_threshold, rc7_decode, rc7_bit}, z + 32, (size_t)next, 0, 0xFFFFFFFFu, 0};
    expect(rc7_byte(&c) == 0);
    for (int i = 0; i < 4; i++)
      c.code = (c.code << 8) | rc7_byte(&c);
    size_t bad = 0;
    for (size_t i = 0; i < n; i++) {
      int s = ppmd_decode(p, &c.rc);
      if (s != d[i]) {
        bad = i + 1;
        break;
      }
    }
    if (bad)
      printf("  ppmd vs 7z: kind %d first mismatch at %zu of %zu\n", kind, bad - 1, n);
    expect(bad == 0);
    ppmd_free(p);
  }
  free(z);
  free(d);
  rm_rf(dir);
  free(dir);
}

struct rar4_fp {
  uint32_t len, crc;
  uint8_t type;
};
extern struct rar4_fp rar4_extra_fp;

struct ppm_cfg {
  int order, mem_mb, esc;
  bool matches, two_blocks;
};
static void ppm_encode_stream(const uint8_t *d, size_t n, const struct ppm_cfg *c, struct eb *out);

static void test_rar4_ppmd(void)
{
  uint8_t *t = gen_text(120000, 41), *r = gen_random(40000, 42);
  for (size_t i = 0; i < 40000; i += 7)
    r[i] = 2;
  memset(t + 5000, 'x', 3000);
  memcpy(t + 90000, t + 20000, 6000);
  struct {
    const char *label;
    const uint8_t *d;
    size_t n;
    int order, mem, esc;
    bool matches, two;
  } cases[] = {
      {"text o6", t, 120000, 6, 4, -1, false, false},
      {"text matches esc", t, 120000, 8, 2, 7, true, false},
      {"random esc2 restarts", r, 40000, 4, 1, -1, true, false},
      {"text two blocks", t, 120000, 12, 3, 0x41, true, true},
      {"order 31", t, 60000, 31, 8, -1, false, false},
      {"order 64", t, 60000, 64, 8, -1, true, false},
  };
  for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
    struct ppm_cfg cfg = {cases[k].order, cases[k].mem, cases[k].esc, cases[k].matches, cases[k].two};
    struct eb packed = {0};
    ppm_encode_stream(cases[k].d, cases[k].n, &cfg, &packed);
    struct r4a a = {{0}};
    r4a_begin(&a, 0, NULL);
    struct r4f f = {.name = "p.bin", .data = cases[k].d, .len = cases[k].n, .method = 3};
    r4a_put_file(&a, &f, packed.p, packed.n, false, false);
    r4a_end(&a, false);
    struct listing l;
    slurp(a.out.p, a.out.n, plain_cfg(NULL), &l, true);
    bool ok = l.n == 1 && same(&l, "p.bin", cases[k].d, cases[k].n);
    if (!ok)
      printf("  ppmd case %s: n=%d rc=%d next=%d\n", cases[k].label, l.n, l.n ? l.e[0].rc : -99, l.next_rc);
    expect(ok);
    char label[96];
    snprintf(label, sizeof label, "rar4 ppmd %s", cases[k].label);
    struct want w[] = {{"p.bin", cases[k].d, cases[k].n, false}};
    expect_oracles(label, a.out.p, a.out.n, "t.rar", NULL, w, 1, true);
    listing_free(&l);
    eb_free(&packed);
    finish4(&a);
  }
  free(t);
  free(r);
}

/* ------------------------------------------------------------------ RAR4 filters (fake program fingerprints) */

static void fwd_audio(const uint8_t *in, uint8_t *out, size_t len, unsigned channels)
{
  size_t sp = 0;
  for (unsigned ch = 0; ch < channels; ch++) {
    uint32_t prev_byte = 0, prev_delta = 0, dif[7] = {0};
    int d1 = 0, d2 = 0, d3, k1 = 0, k2 = 0, k3 = 0;
    unsigned count = 0;
    for (size_t i = ch; i < len; i += channels, count++) {
      d3 = d2;
      d2 = (int)(prev_delta - (uint32_t)d1);
      d1 = (int)prev_delta;
      uint32_t pred = (8 * prev_byte + (uint32_t)(k1 * d1 + k2 * d2 + k3 * d3)) >> 3 & 0xff;
      uint8_t orig = in[i], enc = (uint8_t)(pred - orig);
      out[sp++] = enc;
      prev_delta = (uint32_t)(int32_t)(int8_t)(uint8_t)(orig - prev_byte);
      prev_byte = orig;
      int d = (int)(int8_t)enc * 8;
      dif[0] += (uint32_t)abs(d);
      dif[1] += (uint32_t)abs(d - d1);
      dif[2] += (uint32_t)abs(d + d1);
      dif[3] += (uint32_t)abs(d - d2);
      dif[4] += (uint32_t)abs(d + d2);
      dif[5] += (uint32_t)abs(d - d3);
      dif[6] += (uint32_t)abs(d + d3);
      if ((count & 0x1f) == 0) {
        uint32_t mn = dif[0], nm = 0;
        dif[0] = 0;
        for (unsigned j = 1; j < 7; j++) {
          if (dif[j] < mn) {
            mn = dif[j];
            nm = j;
          }
          dif[j] = 0;
        }
        if (nm == 1 && k1 >= -16)
          k1--;
        else if (nm == 2 && k1 < 16)
          k1++;
        else if (nm == 3 && k2 >= -16)
          k2--;
        else if (nm == 4 && k2 < 16)
          k2++;
        else if (nm == 5 && k3 >= -16)
          k3--;
        else if (nm == 6 && k3 < 16)
          k3++;
      }
    }
  }
}

static void fwd_rgb(const uint8_t *in, uint8_t *out, size_t len, size_t width, size_t pos_r)
{
  uint8_t *t = malloc(len + 4);
  memcpy(t, in, len);
  for (size_t i = pos_r; i + 2 < len; i += 3) {
    t[i] = (uint8_t)(in[i] - in[i + 1]);
    t[i + 2] = (uint8_t)(in[i + 2] - in[i + 1]);
  }
  size_t sp = 0;
  for (size_t ch = 0; ch < 3; ch++) {
    int prev = 0;
    for (size_t i = ch; i < len; i += 3) {
      int pred = prev;
      if (i >= width + 3) {
        int ub = t[i - width], ul = t[i - width - 3];
        int p = prev + ub - ul;
        int pa = abs(p - prev), pb = abs(p - ub), pc = abs(p - ul);
        pred = pa <= pb && pa <= pc ? prev : pb <= pc ? ub : ul;
      }
      out[sp++] = (uint8_t)(pred - t[i]);
      prev = t[i];
    }
  }
  free(t);
}

static void make_fake_prog(uint8_t *code, size_t n, uint8_t seed, uint8_t type)
{
  uint8_t x = 0;
  for (size_t i = 1; i < n; i++) {
    code[i] = (uint8_t)(seed * 31 + i * 7);
    x ^= code[i];
  }
  code[0] = x;
  rar4_extra_fp.len = (uint32_t)n;
  rar4_extra_fp.crc = crc32(0, code, (uint32_t)n);
  rar4_extra_fp.type = type;
}

static void run_vm_case(const char *label, const uint8_t *orig, const uint8_t *comp, size_t n, const struct ops *pre)
{
  struct r4a a = {{0}};
  r4a_begin(&a, 0, NULL);
  struct r4f f = {.name = "f.bin", .data = orig, .comp = comp, .len = n, .method = 3, .prefix = pre};
  r4a_add(&a, &f, false);
  r4a_end(&a, false);
  struct listing l;
  slurp(a.out.p, a.out.n, plain_cfg(NULL), &l, true);
  bool ok = l.n == 1 && same(&l, "f.bin", orig, n);
  if (!ok)
    printf("  vm case %s: rc=%d\n", label, l.n ? l.e[0].rc : -99);
  expect(ok);
  listing_free(&l);
  finish4(&a);
}

static void test_rar4_filters(void)
{
  size_t n = 80000;
  uint8_t *x86 = gen_x86(n, 51), *aud = gen_audio(n, 52, 2), *buf = malloc(n + 16);
  uint8_t code[40];
  struct ops pre;

  /* E8 over two ranges, the second reusing the program with start+258 and the old length */
  struct filt fe[] = {{0, 0, 30000, 1, 0}, {0, 40000, 30000, 1, 0}};
  memcpy(buf, x86, n);
  r5_forward(buf, n, fe, 2);
  make_fake_prog(code, 5, 1, FT_E8);
  memset(&pre, 0, sizeof pre);
  vm_record(&pre, 0x80 | 0x20, 0, 0, 30000, 0, NULL, code, 5);
  vm_record(&pre, 0x40, 0, 40000, 0, 0, NULL, NULL, 0);
  run_vm_case("e8 two ranges", x86, buf, n, &pre);
  ops_free(&pre);

  /* E8E9 with large values for start and length (32-bit ReadData form) and registers */
  struct filt fe9[] = {{0, 100, 70000, 2, 0}};
  memcpy(buf, x86, n);
  r5_forward(buf, n, fe9, 1);
  make_fake_prog(code, 6, 2, FT_E8E9);
  uint32_t regs[7] = {0, 0, 0, 0, 0, 0, 0};
  memset(&pre, 0, sizeof pre);
  vm_record(&pre, 0x80 | 0x20 | 0x10, 0, 100, 70000, 0x01, regs, code, 6);
  run_vm_case("e8e9 regs", x86, buf, n, &pre);
  ops_free(&pre);

  /* delta, channels from register 0 */
  struct filt fd[] = {{0, 0, 60000, 0, 3}};
  memcpy(buf, aud, n);
  r5_forward(buf, n, fd, 1);
  make_fake_prog(code, 12, 3, FT_DELTA);
  regs[0] = 3;
  memset(&pre, 0, sizeof pre);
  vm_record(&pre, 0x80 | 0x20 | 0x10, 0, 0, 60000, 0x01, regs, code, 12);
  run_vm_case("delta", aud, buf, n, &pre);
  ops_free(&pre);

  /* audio, 2 channels */
  memcpy(buf, aud, n);
  fwd_audio(aud, buf, 50000, 2);
  make_fake_prog(code, 20, 4, FT_AUDIO);
  regs[0] = 2;
  memset(&pre, 0, sizeof pre);
  vm_record(&pre, 0x80 | 0x20 | 0x10, 0, 0, 50000, 0x01, regs, code, 20);
  run_vm_case("audio", aud, buf, n, &pre);
  ops_free(&pre);

  /* RGB: width register and the red channel offset */
  uint8_t *rgb = gen_audio(n, 53, 3);
  memcpy(buf, rgb, n);
  fwd_rgb(rgb, buf, 60000, 300 * 3 - 3, 1);
  make_fake_prog(code, 30, 5, FT_RGB);
  regs[0] = 300 * 3;
  regs[1] = 1;
  memset(&pre, 0, sizeof pre);
  vm_record(&pre, 0x80 | 0x20 | 0x10, 0, 0, 60000, 0x03, regs, code, 30);
  run_vm_case("rgb", rgb, buf, n, &pre);
  ops_free(&pre);
  free(rgb);

  /* an unknown program is rejected with a clear error */
  make_fake_prog(code, 7, 6, FT_E8);
  rar4_extra_fp.len = 0;
  memset(&pre, 0, sizeof pre);
  vm_record(&pre, 0x80 | 0x20, 0, 0, 1000, 0, NULL, code, 7);
  struct r4a a = {{0}};
  r4a_begin(&a, 0, NULL);
  struct r4f f = {.name = "f.bin", .data = x86, .comp = x86, .len = 2000, .method = 3, .prefix = &pre};
  r4a_add(&a, &f, false);
  r4a_end(&a, false);
  struct listing l;
  slurp(a.out.p, a.out.n, plain_cfg(NULL), &l, true);
  expect(l.n == 1 && l.e[0].rc == RAR_EUNSUPPORTED);
  listing_free(&l);
  finish4(&a);
  ops_free(&pre);
  free(x86);
  free(aud);
  free(buf);
}

/* ------------------------------------------------------------------ primitives */

static void hexcmp_b2sp(unsigned n, const char *want)
{
  uint8_t *d = malloc(n + 1);
  for (unsigned i = 0; i < n; i++)
    d[i] = (uint8_t)((i * 7 + n) & 255);
  struct b2sp s;
  b2sp_init(&s);
  for (unsigned i = 0; i < n; i += 37)
    b2sp_update(&s, d + i, n - i < 37 ? n - i : 37);
  uint8_t out[32];
  b2sp_final(&s, out);
  char hex[65];
  for (int i = 0; i < 32; i++)
    snprintf(hex + 2 * i, 3, "%02x", out[i]);
  expect(!strcmp(hex, want));
  free(d);
}

static void unhex(const char *h, uint8_t *o)
{
  for (; h[0]; h += 2)
    *o++ = (uint8_t)((((h[0] & 15) + (h[0] > '9' ? 9 : 0)) << 4) | ((h[1] & 15) + (h[1] > '9' ? 9 : 0)));
}

static void ref_e8(uint8_t *d, uint32_t n, uint32_t fileoff, bool e9, bool v5)
{
  for (uint32_t i = 0; i + 4 < n; i++) {
    if (d[i] != 0xe8 && !(e9 && d[i] == 0xe9))
      continue;
    uint32_t off = i + 1 + fileoff;
    if (v5)
      off %= 0x1000000;
    uint32_t a = d[i + 1] | d[i + 2] << 8 | d[i + 3] << 16 | (uint32_t)d[i + 4] << 24;
    int32_t sa = (int32_t)a;
    if (sa < 0) {
      if ((int32_t)(a + off) >= 0)
        a += 0x1000000;
    } else if (a < 0x1000000)
      a -= off;
    for (int k = 0; k < 4; k++)
      d[i + 1 + k] = (uint8_t)(a >> (8 * k));
    i += 4;
  }
}

static void test_primitives(void)
{
  expect(crc32(0, "123456789", 9) == 0xCBF43926u);
  uint8_t key[32], pt[16], ct[16], iv[16] = {0}, want[16];
  for (int i = 0; i < 32; i++)
    key[i] = (uint8_t)i;
  unhex("00112233445566778899aabbccddeeff", want);
  struct rar_aes a;
  unhex("69c4e0d86a7b0430d8cdb78070b4c55a", ct);
  rar_aes_init(&a, key, 16);
  memcpy(pt, ct, 16);
  rar_aes_cbc_decrypt(&a, iv, pt, 16);
  expect(!memcmp(pt, want, 16));
  unhex("8ea2b7ca516745bfeafc49904b496089", ct);
  rar_aes_init(&a, key, 32);
  memset(iv, 0, 16);
  memcpy(pt, ct, 16);
  rar_aes_cbc_decrypt(&a, iv, pt, 16);
  expect(!memcmp(pt, want, 16));

  hexcmp_b2sp(0, "dd0e891776933f43c7d032b08a917e25741f8aa9a12c12e1cac8801500f2ca4f");
  hexcmp_b2sp(1, "ea31a9c7fb61fc5d0a5ab2e5d6c781cef1c5c2aacc14cbabe69f921c0e5116fa");
  hexcmp_b2sp(64, "8c43f324e5345839a3b74406ae891211db3a1c625258f574caba18e0cd8d8928");
  hexcmp_b2sp(513, "535c2376d696f385e60aa2082c59b7fe463f5d18f133a003230ce0b4687ebd6e");
  hexcmp_b2sp(5000, "cbab26a93b1d7f6c518cda72c543f43140405f2a8266a68a12c0e24b2e41c14e");

#ifdef RAR_ENC_CRYPTO
  {
    uint8_t salt[16], k[32], hk[32], chk[8], hk2[32], c2[32], c8[8] = {0}, k2[32];
    for (int i = 0; i < 16; i++)
      salt[i] = (uint8_t)(i * 13 + 1);
    for (unsigned lg = 0; lg < 7; lg += 3) {
      expect(rar5_kdf((const uint8_t *)"pass word", 9, salt, lg, k, hk, chk));
      host_pbkdf2("pass word", salt, 16, 1u << lg, k2, 32);
      host_pbkdf2("pass word", salt, 16, (1u << lg) + 16, hk2, 32);
      host_pbkdf2("pass word", salt, 16, (1u << lg) + 32, c2, 32);
      memset(c8, 0, 8);
      for (int i = 0; i < 32; i++)
        c8[i & 7] ^= c2[i];
      expect(!memcmp(k, k2, 32) && !memcmp(hk, hk2, 32) && !memcmp(chk, c8, 8));
    }
    expect(!rar5_kdf((const uint8_t *)"x", 1, salt, 25, k, hk, chk));
    uint8_t s8[8] = {1, 2, 3, 4, 5, 6, 7, 8}, rk[16], riv[16], hk3[16], hiv[16];
    uint8_t w16[8] = {'a', 0, 'b', 0, 'c', 0, 'd', 0};
    rar3_kdf(w16, 8, s8, rk, riv);
    host_rar3_kdf("abcd", s8, hk3, hiv);
    expect(!memcmp(rk, hk3, 16) && !memcmp(riv, hiv, 16));
    uint8_t buf[160], enc[160], ivv[16];
    enc_rand(&(uint32_t){5}, buf, 160);
    for (int ks = 16; ks <= 32; ks += 16) {
      uint8_t kk[32];
      enc_rand(&(uint32_t){(uint32_t)ks}, kk, 32);
      enc_rand(&(uint32_t){9}, ivv, 16);
      host_aes_cbc(1, kk, (size_t)ks, ivv, buf, enc, 160);
      rar_aes_init(&a, kk, (unsigned)ks);
      uint8_t iv2[16], dec[160];
      memcpy(iv2, ivv, 16);
      memcpy(dec, enc, 160);
      rar_aes_cbc_decrypt(&a, iv2, dec, 160);
      expect(!memcmp(dec, buf, 160));
    }
  }
#endif

  /* E8/E8E9/ARM/delta against separately written references */
  uint8_t *x = gen_x86(70000, 61), *w = malloc(70000), *sc = malloc(70000);
  static const uint32_t offs[] = {0, 12345, 0x00fffff0u, 0x7ffffff0u};
  for (unsigned oi = 0; oi < 4; oi++)
    for (int e9 = 0; e9 < 2; e9++)
      for (int v5 = 0; v5 < 2; v5++) {
        struct lzfilter f = {.type = e9 ? FT_E8E9 : FT_E8, .v5 = v5};
        memcpy(w, x, 70000);
        const uint8_t *r = rar_filter_run(&f, w, sc, 70000, offs[oi]);
        memcpy(sc, x, 70000);
        ref_e8(sc, 70000, offs[oi], e9, v5);
        expect(r == w && !memcmp(w, sc, 70000));
      }
  struct lzfilter fd = {.type = FT_DELTA};
  fd.r[0] = 4;
  memcpy(w, x, 4000);
  const uint8_t *r = rar_filter_run(&fd, w, sc, 4000, 0);
  uint8_t ref[4000];
  for (int ch = 0, sp = 0; ch < 4; ch++) {
    uint8_t prev = 0;
    for (int i = ch; i < 4000; i += 4)
      ref[i] = (uint8_t)(prev -= x[sp++]);
  }
  expect(!memcmp(r, ref, 4000));
  struct lzfilter bad = {.type = FT_DELTA};
  expect(rar_filter_run(&bad, w, sc, 100, 0) == NULL);
  bad.type = FT_AUDIO;
  expect(rar_filter_run(&bad, w, sc, 100, 0) == NULL);
  bad.type = FT_RGB;
  expect(rar_filter_run(&bad, w, sc, 100, 0) == NULL);
  free(x);
  free(w);
  free(sc);

  /* huffman */
  struct huff h;
  uint8_t over[3] = {1, 1, 1};
  expect(!huff_build(&h, over, 3));
  uint8_t ok[4] = {1, 2, 3, 3};
  expect(huff_build(&h, ok, 4));
  uint8_t inc[2] = {2, 0};
  expect(huff_build(&h, inc, 2));
  uint8_t last_level[3] = {1, 1, 2};
  expect(!huff_build(&h, last_level, 3));
  uint8_t toolong[1] = {16};
  expect(!huff_build(&h, toolong, 1));
}

/* ------------------------------------------------------------------ safety and errors */

static void test_paths_and_names(void)
{
  expect(rar_path_is_safe("a/b/c.txt") && rar_path_is_safe("a..b/c") && rar_path_is_safe(".hidden") &&
         rar_path_is_safe("dir/") && rar_path_is_safe("...") && rar_path_is_safe("a/./b"));
  expect(!rar_path_is_safe("../x") && !rar_path_is_safe("a/../../x") && !rar_path_is_safe("a/..") &&
         !rar_path_is_safe("/etc/passwd") && !rar_path_is_safe("C:/x") && !rar_path_is_safe("c:x") &&
         !rar_path_is_safe("..\\x") && !rar_path_is_safe("a\\..\\b") && !rar_path_is_safe("\\x") &&
         !rar_path_is_safe("") && !rar_path_is_safe(NULL));
}

static char g_asked[8][64];
static int g_nasked;
static int record_open(void *ctx, const char *name, struct rar_src *out)
{
  (void)ctx;
  (void)out;
  if (g_nasked < 8)
    snprintf(g_asked[g_nasked++], 64, "%s", name);
  return -1;
}

static void test_volume_names(void)
{
  uint8_t d[200];
  memset(d, 'a', sizeof d);
  struct r4a st = {{0}};
  struct r4f f = {.name = "x", .data = d, .len = 200, .method = 0};
  struct r4a v = {{0}};
  r4a_begin(&v, 1, NULL);
  r4a_put_file(&v, &f, d, 100, false, true);
  r4a_end(&v, true);
  static const char *firsts[] = {"a.part01.rar", "a.part9.rar", "b.rar", "B.RAR", "c.r98", "dir.x/e.PART007.rar"};
  static const char *want[] = {"a.part02.rar", "a.part10.rar", "b.r00", "B.R00", "c.r99", "dir.x/e.PART008.rar"};
  for (unsigned i = 0; i < 6; i++) {
    g_nasked = 0;
    struct rar_cfg cfg = plain_cfg(NULL);
    cfg.open_volume = record_open;
    cfg.name = firsts[i];
    struct listing l;
    slurp(v.out.p, v.out.n, cfg, &l, true);
    if (g_nasked < 1 || strcmp(g_asked[0], want[i]))
      printf("  volume name %s -> %s (want %s)\n", firsts[i], g_nasked ? g_asked[0] : "none", want[i]);
    expect(g_nasked >= 1 && !strcmp(g_asked[0], want[i]));
    expect(l.n == 1 && l.e[0].rc == RAR_EVOLUME && l.next_rc == RAR_EVOLUME);
    listing_free(&l);
  }
  /* c.r99 -> c.s00 */
  finish4(&v);
  finish4(&st);
}

static void flip(uint8_t *p, size_t at) { p[at] ^= 0x5a; }

static void test_errors_and_limits(void)
{
  uint8_t *t = gen_text(60000, 71);
  /* RAR5 and RAR4 stored with CRC damage */
  for (int fmt = 4; fmt <= 5; fmt++) {
    struct r5a a5 = {{0}};
    struct r4a a4 = {{0}};
    struct eb *out;
    if (fmt == 5) {
      r5a_begin(&a5, 0, 0, NULL, 0);
      struct r5f f = {.name = "s.txt", .data = t, .len = 5000, .method = 0};
      r5a_add(&a5, &f);
      struct r5f g = {.name = "l.txt", .data = t, .len = 60000, .method = 3};
      r5a_add(&a5, &g);
      r5a_end(&a5, false);
      out = &a5.out;
    } else {
      r4a_begin(&a4, 0, NULL);
      struct r4f f = {.name = "s.txt", .data = t, .len = 5000, .method = 0};
      r4a_add(&a4, &f, false);
      struct r4f g = {.name = "l.txt", .data = t, .len = 60000, .method = 3};
      r4a_add(&a4, &g, false);
      r4a_end(&a4, false);
      out = &a4.out;
    }
    struct listing l;
    slurp(out->p, out->n, plain_cfg(NULL), &l, true);
    expect(l.n == 2 && same(&l, "s.txt", t, 5000) && same(&l, "l.txt", t, 60000));
    uint64_t packed_lz = l.n == 2 ? l.e[1].packed : 0;
    listing_free(&l);
    uint8_t *c = malloc(out->n);
    /* a flipped byte in the stored data is reported after the data was delivered */
    size_t at = 0;
    for (size_t i = 100; i + 20 < out->n; i++)
      if (!memcmp(out->p + i, t + 1000, 20)) {
        at = i;
        break;
      }
    expect(at != 0);
    memcpy(c, out->p, out->n);
    flip(c, at + 5);
    slurp(c, out->n, plain_cfg(NULL), &l, true);
    expect(l.n == 2 && l.e[0].rc == RAR_ECRC && l.e[0].len == 5000 && same(&l, "l.txt", t, 60000));
    listing_free(&l);
    /* header corruption: every single-byte flip in the first file header is caught or harmless, never a crash */
    size_t hdr_start = fmt == 5 ? 8 + 12 : 7 + 13;
    for (size_t i = hdr_start; i < hdr_start + 60 && i < out->n; i++) {
      memcpy(c, out->p, out->n);
      flip(c, i);
      slurp(c, out->n, plain_cfg(NULL), &l, true);
      listing_free(&l);
    }
    memcpy(c, out->p, out->n);
    flip(c, hdr_start + 8);
    slurp(c, out->n, plain_cfg(NULL), &l, true);
    expect(l.open_rc == RAR_OK && (l.next_rc == RAR_ECRC || l.n == 0));
    listing_free(&l);
    /* damage inside the LZ data: error or CRC failure, never silent */
    memcpy(c, out->p, out->n);
    flip(c, out->n - 40 - (size_t)(packed_lz / 2));
    slurp(c, out->n, plain_cfg(NULL), &l, true);
    expect(l.n == 2 && l.e[1].rc != 0);
    listing_free(&l);
    /* truncation */
    for (size_t cut = 1; cut < out->n; cut += out->n / 17 + 1) {
      slurp(out->p, out->n - cut, plain_cfg(NULL), &l, true);
      expect(l.open_rc != 0 || l.next_rc != 0 || l.n < 2 || l.e[1].rc != 0 || l.e[0].rc != 0 || cut < 8);
      listing_free(&l);
    }
    /* limits: total output and decoder memory */
    struct rar_cfg cfg = plain_cfg(NULL);
    cfg.max_total = 30000;
    slurp(out->p, out->n, cfg, &l, true);
    expect(l.n == 2 && l.e[0].rc == 0 && l.e[1].rc == RAR_ELIMIT && l.e[1].len <= 30000);
    listing_free(&l);
    cfg.max_total = 0;
    cfg.max_mem = 8192;
    slurp(out->p, out->n, cfg, &l, true);
    expect(l.n == 2 && l.e[1].rc == RAR_ENOMEM);
    listing_free(&l);
    /* list-only never decodes */
    cfg.max_mem = 0;
    cfg.list_only = true;
    slurp(out->p, out->n, cfg, &l, true);
    expect(l.n == 2 && l.e[1].rc == RAR_ESTATE);
    listing_free(&l);
    /* skipping entries without extracting them */
    {
      struct rar *r;
      struct rar_src src = make_src(out->p, out->n);
      struct rar_cfg c0 = plain_cfg(NULL);
      expect(rar_open(&r, &src, &c0) == 0);
      struct rar_entry e;
      expect(rar_next(r, &e) == 1 && rar_next(r, &e) == 1 && !strcmp(e.name, "l.txt"));
      struct sink s = {0};
      expect(rar_extract(r, sink_write, &s) == 0 && s.n == 60000 && !memcmp(s.p, t, 60000));
      expect(rar_extract(r, sink_write, &s) == RAR_ESTATE);
      expect(rar_next(r, &e) == 0);
      free(s.p);
      rar_close(r);
    }
    free(c);
    if (fmt == 5)
      finish5(&a5);
    else
      finish4(&a4);
  }
  /* not an archive, empty archive, SFX stub */
  struct listing l;
  slurp((const uint8_t *)"hello world, not a rar", 22, plain_cfg(NULL), &l, true);
  expect(l.open_rc == RAR_EFORMAT);
  struct r5a e5 = {{0}};
  r5a_begin(&e5, 0, 0, NULL, 0);
  r5a_end(&e5, false);
  slurp(e5.out.p, e5.out.n, plain_cfg(NULL), &l, true);
  expect(l.open_rc == 0 && l.n == 0 && l.next_rc == 0);
  listing_free(&l);
  struct r5a s5 = {{0}};
  uint8_t stub[3000];
  memset(stub, 0x90, sizeof stub);
  memcpy(stub + 100, "Rar!\x1a\x07", 6);
  eb_put(&s5.out, stub, sizeof stub);
  r5a_begin(&s5, 0, 0, NULL, 0);
  struct r5f sf = {.name = "in_sfx.txt", .data = t, .len = 100, .method = 0};
  r5a_add(&s5, &sf);
  r5a_end(&s5, false);
  slurp(s5.out.p, s5.out.n, plain_cfg(NULL), &l, true);
  expect(l.open_rc == 0 && l.n == 1 && same(&l, "in_sfx.txt", t, 100));
  listing_free(&l);
  finish5(&s5);
  finish5(&e5);
  /* old RAR methods are named as such */
  struct r4a o4 = {{0}};
  r4a_begin(&o4, 0, NULL);
  struct r4f of = {.name = "old.txt", .data = t, .len = 100, .method = 3, .unp_ver = 20};
  r4a_put_file(&o4, &of, t, 50, false, false);
  struct r4f oks = {.name = "oldstored.txt", .data = t, .len = 100, .method = 0, .unp_ver = 20};
  r4a_put_file(&o4, &oks, t, 100, false, false);
  r4a_end(&o4, false);
  slurp(o4.out.p, o4.out.n, plain_cfg(NULL), &l, true);
  expect(l.n == 2 && l.e[0].rc == RAR_EOLD && l.e[0].version == 20 && same(&l, "oldstored.txt", t, 100));
  expect(!strcmp(rar_strerror(RAR_EOLD), "unsupported (old RAR)"));
  listing_free(&l);
  finish4(&o4);
  free(t);
}

/* ------------------------------------------------------------------ fuzz */

static void fuzz_one(const uint8_t *arc, size_t n, const char *pw, uint32_t *seed, int iters)
{
  uint8_t *c = malloc(n + 64);
  for (int it = 0; it < iters; it++) {
    memcpy(c, arc, n);
    size_t len = n;
    unsigned kind = rnd(seed) % 5;
    if (kind == 0) {
      c[rnd(seed) * 65536u % n % n] ^= (uint8_t)(1u << (rnd(seed) & 7));
    } else if (kind == 1) {
      for (int k = 0; k < 1 + (int)(rnd(seed) % 8); k++)
        c[(size_t)rnd(seed) * 7919u % n] = (uint8_t)rnd(seed);
    } else if (kind == 2) {
      len = (size_t)rnd(seed) * 31u % n;
    } else if (kind == 3) {
      size_t at = (size_t)rnd(seed) * 13u % n, cnt = 1 + rnd(seed) % 12;
      for (size_t k = 0; k < cnt && at + k < n; k++)
        c[at + k] = (uint8_t)(rnd(seed) & 1 ? 0xff : 0);
    } else {
      size_t at = 8 + (size_t)rnd(seed) % (n > 80 ? 70 : n - 8);
      c[at % n] = (uint8_t)rnd(seed);
    }
    struct rar_cfg cfg = plain_cfg(pw);
    cfg.max_mem = 24u << 20;
    cfg.max_total = 8u << 20;
    struct listing l;
    slurp(c, len, cfg, &l, true);
    listing_free(&l);
  }
  free(c);
}

static void test_fuzz(void)
{
  uint8_t *t = gen_text(30000, 81), *x = gen_x86(20000, 82), *r = gen_random(4000, 83);
  uint32_t seed = 4242;
  struct eb arcs[16] = {{0}};
  const char *pws[16] = {0};
  int na = 0;
  {
    struct r5a a = {{0}};
    r5a_begin(&a, 4, 0, NULL, 0);
    struct filt fl[] = {{0, 0, 20000, 1, 0}};
    struct r5f f[4] = {
        {.name = "a.txt", .data = t, .len = 30000, .method = 3, .block_tokens = 700},
        {.name = "b.bin", .data = x, .len = 20000, .method = 3, .solid = true, .filters = fl, .nfilters = 1},
        {.name = "c.rnd", .data = r, .len = 4000, .method = 0},
        {.name = "d", .is_dir = true}};
    for (int i = 0; i < 4; i++)
      r5a_add(&a, &f[i]);
    r5a_end(&a, false);
    eb_put(&arcs[na++], a.out.p, a.out.n);
    finish5(&a);
  }
#ifdef RAR_ENC_CRYPTO
  {
    struct r5a a = {{0}};
    r5a_begin(&a, 0, 0, "pw", 4);
    struct r5f f = {.name = "e.txt",
                    .data = t,
                    .len = 8000,
                    .method = 3,
                    .password = "pw",
                    .lg2 = 4,
                    .pswcheck = true,
                    .b2hash = true};
    r5a_add(&a, &f);
    r5a_end(&a, false);
    pws[na] = "pw";
    eb_put(&arcs[na++], a.out.p, a.out.n);
    finish5(&a);
    struct r4a b = {{0}};
    r4a_begin(&b, 0, NULL);
    struct r4f g = {.name = "e.txt", .data = t, .len = 8000, .method = 3, .password = "pw"};
    r4a_add(&b, &g, false);
    r4a_end(&b, false);
    pws[na] = "pw";
    eb_put(&arcs[na++], b.out.p, b.out.n);
    finish4(&b);
  }
#endif
  {
    struct r4a a = {{0}};
    r4a_begin(&a, 8, NULL);
    struct r4f f[3] = {{.name = "a.txt", .data = t, .len = 30000, .method = 3, .block_tokens = 900},
                       {.name = "b.txt", .data = t, .len = 10000, .method = 3, .solid = true},
                       {.name = "c.rnd", .data = r, .len = 4000, .method = 0}};
    for (int i = 0; i < 3; i++)
      r4a_add(&a, &f[i], false);
    r4a_end(&a, false);
    eb_put(&arcs[na++], a.out.p, a.out.n);
    finish4(&a);
  }
  {
    struct ppm_cfg pc = {6, 1, -1, true, true};
    struct eb packed = {0};
    ppm_encode_stream(t, 30000, &pc, &packed);
    struct r4a a = {{0}};
    r4a_begin(&a, 0, NULL);
    struct r4f f = {.name = "p.txt", .data = t, .len = 30000, .method = 3};
    r4a_put_file(&a, &f, packed.p, packed.n, false, false);
    r4a_end(&a, false);
    eb_put(&arcs[na++], a.out.p, a.out.n);
    finish4(&a);
    eb_free(&packed);
  }
  {
    uint8_t code[8], *buf = malloc(20000);
    struct filt fe[] = {{0, 0, 20000, 1, 0}};
    memcpy(buf, x, 20000);
    r5_forward(buf, 20000, fe, 1);
    make_fake_prog(code, 5, 1, FT_E8);
    struct ops pre = {0};
    vm_record(&pre, 0x80 | 0x20, 0, 0, 20000, 0, NULL, code, 5);
    struct r4a a = {{0}};
    r4a_begin(&a, 0, NULL);
    struct r4f f = {.name = "v.bin", .data = x, .comp = buf, .len = 20000, .method = 3, .prefix = &pre};
    r4a_add(&a, &f, false);
    r4a_end(&a, false);
    eb_put(&arcs[na++], a.out.p, a.out.n);
    finish4(&a);
    ops_free(&pre);
    free(buf);
  }
  for (int i = 0; i < na; i++)
    fuzz_one(arcs[i].p, arcs[i].n, pws[i], &seed, pws[i] && i == 2 ? 40 : 600);
  for (int i = 0; i < na; i++)
    eb_free(&arcs[i]);
  rar4_extra_fp.len = 0;
  free(t);
  free(x);
  free(r);
}

/* matches at distances around the length-bonus thresholds and the repeat-distance cache */
static void test_distance_boundaries(void)
{
  size_t n = 700000;
  uint8_t *d = gen_random(n, 91);
  static const uint32_t dist[] = {0xff, 0x100, 0x101, 0x1fff, 0x2000, 0x2001, 0x3ffff, 0x40000, 0x40001};
  size_t pos = 1000;
  for (unsigned i = 0; i < sizeof dist / sizeof dist[0]; i++) {
    pos = (size_t)0x41000 + i * 700;
    memcpy(d + pos, d + pos - dist[i], 40);
    memcpy(d + pos + 300, d + pos + 300 - dist[i], 40);
  }
  struct r5a a5 = {{0}};
  r5a_begin(&a5, 0, 0, NULL, 0);
  struct r5f f5 = {.name = "b.bin", .data = d, .len = n, .method = 3, .dict_shift = 3};
  r5a_add(&a5, &f5);
  r5_roundtrip("rar5 distance boundaries", &a5, &f5, 1, NULL, true);
  finish5(&a5);
  struct r4a a4 = {{0}};
  r4a_begin(&a4, 0, NULL);
  struct r4f f4 = {.name = "b.bin", .data = d, .len = n, .method = 3, .no16 = true};
  r4a_add(&a4, &f4, false);
  r4_roundtrip("rar4 distance boundaries", &a4, &f4, 1, NULL, true);
  finish4(&a4);
  free(d);
}

static void test_hash_failures(void)
{
  uint8_t *t = gen_text(3000, 92);
  struct r5a a = {{0}};
  r5a_begin(&a, 0, 0, NULL, 0);
  struct r5f f[2] = {{.name = "good.txt", .data = t, .len = 3000, .method = 0, .b2hash = true, .no_crc = true},
                     {.name = "bad.txt", .data = t, .len = 3000, .method = 0, .b2hash = true, .no_crc = true}};
  r5a_add(&a, &f[0]);
  size_t hdr2 = a.out.n;
  r5a_add(&a, &f[1]);
  r5a_end(&a, false);
  struct listing l;
  slurp(a.out.p, a.out.n, plain_cfg(NULL), &l, true);
  expect(l.n == 2 && l.e[0].rc == 0 && l.e[1].rc == 0);
  listing_free(&l);
  size_t at = 0;
  for (size_t i = hdr2; i + 20 < a.out.n; i++)
    if (!memcmp(a.out.p + i, t + 500, 20)) {
      at = i;
      break;
    }
  expect(at != 0);
  a.out.p[at + 3] ^= 1;
  slurp(a.out.p, a.out.n, plain_cfg(NULL), &l, true);
  expect(l.n == 2 && l.e[0].rc == 0 && l.e[1].rc == RAR_ECRC && l.e[1].len == 3000);
  listing_free(&l);
  finish5(&a);
  free(t);
}

static void test_paths_and_volume_names(void)
{
  test_paths_and_names();
  test_volume_names();
}

static void test_distances_and_hashes(void)
{
  test_distance_boundaries();
  test_hash_failures();
}

static void test_rar5_volumes_lz(void) { test_rar5_volumes(3); }

static void test_rar5_volumes_stored(void) { test_rar5_volumes(0); }

static void test_ppmd_streams(void)
{
  test_ppmd_vs_7z(200000, 0);
  test_ppmd_vs_7z(1500000, 1);
  test_ppmd_vs_7z(900000, 2);
}

static void test_rar4_volume_sets(void)
{
  test_rar4_volumes(true, 3);
  test_rar4_volumes(false, 3);
  test_rar4_volumes(true, 0);
}

int main(void)
{
  have_unar = have_tool("unar");
  have_bsdtar = have_tool("bsdtar");
  alarm(280);
  test("primitives: crc, aes, kdf, blake2sp, filters", test_primitives);
  test("path safety and volume names", test_paths_and_volume_names);
  test("errors, limits, skipping, old methods", test_errors_and_limits);
  test("distance boundaries and hash failures", test_distances_and_hashes);
  test("rar5 stored and lz round trip, oracles", test_rar5_basic);
  test("rar5 solid, blocks, universal tables", test_rar5_blocks_solid);
  test("rar5 filters", test_rar5_filters);
  test("rar5 far distances", test_rar5_distances);
  test("rar5 names and links", test_rar5_names_links);
  test("rar5 volumes", test_rar5_volumes_lz);
  test("rar5 volumes stored", test_rar5_volumes_stored);
#ifdef RAR_ENC_CRYPTO
  test("rar5 encryption", test_rar5_crypto);
#endif
  test("ppmd model against libarchive 7z streams", test_ppmd_streams);
  test("rar4 basic", test_rar4_basic);
  test("rar4 solid", test_rar4_solid);
  test("rar4 far distances", test_rar4_distances);
#ifdef RAR_ENC_CRYPTO
  test("rar4 encryption", test_rar4_crypto);
#endif
  test("rar4 ppmd blocks (self-consistent encoder, model verified against 7z)", test_rar4_ppmd);
  test("rar4 filters through the VM record parser", test_rar4_filters);
  test("fuzz: mutated and truncated archives", test_fuzz);
  test("rar4 volumes", test_rar4_volume_sets);
  return 0;
}

#include "rar_ppmd_enc.h"
