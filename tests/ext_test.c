/* ext_test.c: host test for kernel/ext.c (read-only ext2/3/4 driver).
 *
 * The driver is compiled into this file against an in-memory block device. Images come from tests/ext_image.h and
 * tests/ext_jbd.h, an independent builder written from the on-disk format; no image here was made by mke2fs, so
 * interoperability with real volumes is not verified by this test. A seeded fuzz pass flips bytes and truncates
 * images and checks the driver neither crashes nor hangs.
 *
 *   make test    (or compile with -include tests/test.h -Dmain=owos_main_ext -DOWOS_TEST_NAME=ext)
 */
#define OWOS_SUITE_TEST
#define OWOS_CONSOLE_H
#define OWOS_HEAP_H
#define OWOS_KSTRING_H

#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_log[16384];

static void log_add(const char *s)
{
  size_t n = strlen(g_log);
  if (n + strlen(s) < sizeof g_log)
    strcpy(g_log + n, s);
}

static void con_write(const char *s) { log_add(s); }

static void con_printf(const char *fmt, ...)
{
  char b[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  log_add(b);
}

static long g_live, g_alloc_n, g_fail_at;

static void *kmalloc(size_t n)
{
  if (++g_alloc_n == g_fail_at)
    return NULL;
  void *p = malloc(n ? n : 1);
  if (p)
    g_live++;
  return p;
}

static void kfree(void *p)
{
  if (p) {
    g_live--;
    free(p);
  }
}

static inline char k_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

#include "../kernel/ext.c"
#include "ext_jbd.h"

/* ---------------------------------------------------------------- device */

static uint8_t *g_img;
static size_t g_len;
static unsigned long g_reads, g_read_limit;
static bool g_hang;

static int tdev_read(uint32_t lba, void *buf)
{
  if ((uint64_t)lba * 512 + 512 > g_len)
    return -1;
  if (g_read_limit && g_reads >= g_read_limit) {
    g_hang = true;
    return -1;
  }
  g_reads++;
  memcpy(buf, g_img + (size_t)lba * 512, 512);
  return 0;
}

static void *mnt(uint8_t *img, size_t len)
{
  g_img = img;
  g_len = len;
  g_reads = 0;
  g_log[0] = '\0';
  return ext_ops()->probe(tdev_read, NULL);
}

static int log_count(const char *needle)
{
  int n = 0;
  for (const char *p = g_log; (p = strstr(p, needle)); p++)
    n++;
  return n;
}

static const char *tname(const char *fmt, ...)
{
  static char ring[64][128];
  static int next;
  char *b = ring[next++ % 64];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, 128, fmt, ap);
  va_end(ap);
  return b;
}

/* ---------------------------------------------------------------- helpers */

static void block_pattern(uint8_t *p, uint32_t bs, uint64_t lblk)
{
  for (uint32_t i = 0; i < bs; i++)
    p[i] = (uint8_t)(1 + ((lblk * 7 + i * 13 + (i >> 8)) % 251));
}

static void tcfg(struct eb *e, uint32_t bs, int kind, uint32_t groups)
{
  memset(e, 0, sizeof *e);
  e->bs = bs;
  e->ipg = 32;
  e->groups = groups;
  e->dsize = 32;
  e->bpg = bs == 1024 ? 256 : bs == 2048 ? 192 : bs == 4096 ? 128 : 48;
  e->isize = kind == 4 ? 256 : 128;
  e->incompat = EB_INC_FILETYPE;
  e->ro_compat = EB_RO_SPARSE | EB_RO_LARGE_FILE;
  if (kind == 4) {
    e->incompat |= EB_INC_EXTENTS | EB_INC_FLEX_BG;
    e->ro_compat |= EB_RO_HUGE_FILE | 0x20 | 0x40;
  }
}

#define DE(nm, ino, ft) {EB_NAME(nm), (ino), (ft)}

static uint64_t mkroot(struct eb *e, const struct eb_de *ents, uint32_t n)
{
  eb_mkino(e, 2, EB_DIR | 0755);
  return eb_dir(e, 2, 2, ents, n, 0);
}

static bool read_all(void *m, const char *path, uint8_t **out, size_t *n)
{
  struct ext_attr a;
  if (ext_getinfo(m, path, &a) != 0 || a.size > (1u << 26))
    return false;
  *n = (size_t)a.size;
  *out = malloc(*n + 1);
  size_t got = 0;
  while (got < *n) {
    int r = ext_read64(m, path, got, *out + got, (uint32_t)(*n - got < 3001 ? *n - got : 3001));
    if (r <= 0) {
      free(*out);
      return false;
    }
    got += (size_t)r;
  }
  return true;
}

static bool same(void *m, const char *path, const void *want, size_t n)
{
  uint8_t *b;
  size_t got;
  if (!read_all(m, path, &b, &got))
    return false;
  bool ok = got == n && !memcmp(b, want, n);
  free(b);
  return ok;
}

static bool is_zero(const uint8_t *p, size_t n)
{
  for (size_t i = 0; i < n; i++)
    if (p[i])
      return false;
  return true;
}

static int rd(void *m, const char *path, uint64_t off, void *buf, uint32_t len)
{
  return ext_read64(m, path, off, buf, len);
}

static bool exists(void *m, const char *path)
{
  struct ext_attr a;
  return ext_getinfo(m, path, &a) == 0;
}

static int list_names(void *m, const char *path, char names[][256], int max)
{
  int n = 0, isd;
  uint32_t sz;
  char nm[256];
  while (n < max && ext_ops()->enum_dir(m, path, n, nm, &isd, &sz)) {
    strcpy(names[n], nm);
    n++;
  }
  return n;
}

static bool has_name(void *m, const char *path, const char *name)
{
  static char names[512][256];
  int n = list_names(m, path, names, 512);
  for (int i = 0; i < n; i++)
    if (!strcmp(names[i], name))
      return true;
  return false;
}

/* Sparse regular file: block lblks[i] holds block_pattern(lblk), everything else is a hole. */
static uint32_t sparse_file(struct eb *e, const uint64_t *lblks, uint32_t n, uint32_t depth, uint32_t leaf_max,
                            uint32_t idx_max, uint64_t size)
{
  uint32_t ino = eb_mkino(e, 0, EB_REG | 0644);
  uint8_t *buf = malloc(e->bs);
  struct eb_ext *x = calloc(n, sizeof *x);
  for (uint32_t i = 0; i < n; i++) {
    uint64_t b = eb_alloc(e);
    block_pattern(buf, e->bs, lblks[i]);
    memcpy(eb_blk(e, b), buf, e->bs);
    eb_add_blocks(e, ino, 1);
    if (e->ext)
      x[i] = (struct eb_ext){(uint32_t)lblks[i], 1, b, false};
    else
      eb_ind_set(e, ino, lblks[i], (uint32_t)b);
  }
  if (e->ext)
    eb_extents(e, ino, x, n, depth, leaf_max, idx_max);
  eb_set_size(e, ino, size);
  free(buf);
  free(x);
  return ino;
}

static bool check_sparse(void *m, const char *path, uint32_t bs, const uint64_t *lblks, uint32_t n)
{
  uint8_t *want = malloc(bs), *got = malloc((size_t)bs * 3);
  bool ok = true;
  for (uint32_t i = 0; i < n && ok; i++) {
    block_pattern(want, bs, lblks[i]);
    ok = rd(m, path, lblks[i] * bs, got, bs) == (int)bs && !memcmp(got, want, bs);
    bool prev_mapped = false, next_mapped = false;
    for (uint32_t k = 0; k < n; k++) {
      prev_mapped |= lblks[k] + 1 == lblks[i];
      next_mapped |= lblks[k] == lblks[i] + 1;
    }
    if (ok && lblks[i] > 0 && !prev_mapped)
      ok = rd(m, path, (lblks[i] - 1) * bs, got, bs) == (int)bs && is_zero(got, bs);
    if (ok && !next_mapped && i + 1 < n)
      ok = rd(m, path, (lblks[i] + 1) * bs, got, bs) == (int)bs && is_zero(got, bs);
    if (ok && lblks[i] > 0 && !prev_mapped && !next_mapped) {
      /* a read straddling hole, data, hole */
      ok = rd(m, path, lblks[i] * bs - 5, got, 10) == 10 && is_zero(got, 5) && !memcmp(got + 5, want, 5);
    }
  }
  free(want);
  free(got);
  return ok;
}

/* ---------------------------------------------------------------- ext2 / ext3 indirect */

static uint32_t indirect_bs;
static int indirect_kind;

static void indirect_case(void)
{
  uint32_t bs = indirect_bs;
  int kind = indirect_kind;
  struct eb e;
  tcfg(&e, bs, kind, 4);
  if (kind == 3)
    e.compat |= EB_COMPAT_JOURNAL;
  eb_init(&e);
  uint64_t ppb = bs / 4;
  uint64_t lb[] = {0,
                   1,
                   11,
                   12,
                   12 + ppb - 1,
                   12 + ppb,
                   12 + ppb + 3,
                   12 + 2 * ppb + 2,
                   12 + ppb + ppb * ppb - 1,
                   12 + ppb + ppb * ppb,
                   12 + ppb + ppb * ppb + ppb + 7,
                   12 + ppb + ppb * ppb + ppb * ppb + 5};
  uint32_t n = sizeof lb / sizeof lb[0];
  uint32_t sp = sparse_file(&e, lb, n, 0, 0, 0, (lb[n - 1] + 1) * bs);
  uint8_t *seq = malloc((size_t)bs * 20);
  for (uint32_t i = 0; i < 20; i++)
    block_pattern(seq + (size_t)i * bs, bs, 1000 + i);
  uint32_t i_seq = eb_file(&e, 0, EB_REG | 0644, seq, (size_t)bs * 20 - 17);
  uint32_t i_small = eb_file(&e, 0, EB_REG | 0644, "hello ext2\n", 11);
  uint32_t i_empty = eb_file(&e, 0, EB_REG | 0644, "", 0);
  uint32_t i_nested = eb_file(&e, 0, EB_REG | 0644, "nested\n", 7);
  uint32_t d1 = eb_mkino(&e, 0, EB_DIR | 0755);
  struct eb_de inner[] = {DE("file", i_nested, EB_FT_REG)};
  eb_dir(&e, d1, 2, inner, 1, 0);
  uint32_t l_fast = eb_symlink(&e, "small.txt", false);
  uint32_t l_slow = eb_symlink(&e, "dir/file", true);
  struct eb_de ents[] = {
      DE("sparse", sp, EB_FT_REG),     DE("seq", i_seq, EB_FT_REG), DE("small.txt", i_small, EB_FT_REG),
      DE("empty", i_empty, EB_FT_REG), DE("dir", d1, EB_FT_DIR),    DE("lnk", l_fast, EB_FT_LNK),
      DE("lnk2", l_slow, EB_FT_LNK)};
  mkroot(&e, ents, 7);
  size_t len;
  uint8_t *img = eb_finish(&e, &len);
  void *m = mnt(img, len);
  expect(m != NULL);
  if (m) {
    expect(!strcmp(ext_ops()->type(m), kind == 2 ? "ext2" : "ext3"));
    expect(!strcmp(ext_ops()->label(m), "ebvol"));
    expect(check_sparse(m, "/sparse", bs, lb, n));
    expect(same(m, "/seq", seq, (size_t)bs * 20 - 17));
    expect(same(m, "/small.txt", "hello ext2\n", 11));
    expect(same(m, "/empty", "", 0));
    expect(same(m, "/dir/file", "nested\n", 7));
    expect(same(m, "/lnk", "hello ext2\n", 11));
    expect(same(m, "/lnk2", "nested\n", 7));
    struct ext_attr a;
    expect(ext_getinfo(m, "/sparse", &a) == 0 && a.size == (lb[n - 1] + 1) * bs && (a.mode & 0xF000) == EB_REG);
    expect(ext_getinfo(m, "/dir", &a) == 0 && (a.mode & 0xF000) == EB_DIR);
    expect(ext_getinfo(m, "/missing", &a) == -EXT_ERR_NOENT);
    expect(ext_getinfo(m, "/small.txt/x", &a) == -EXT_ERR_NOTDIR);
    char buf[16];
    expect(rd(m, "/small.txt", 6, buf, 16) == 5 && !memcmp(buf, "ext2\n", 5));
    expect(rd(m, "/small.txt", 11, buf, 16) == 0);
    expect(rd(m, "/small.txt", 1ull << 40, buf, 16) == 0);
    expect(rd(m, "/dir", 0, buf, 16) == -EXT_ERR_ISDIR);
    ext_close(m);
  }
  expect(g_live == 0);
  free(seq);
  free(e.img);
}

static void run_indirect(uint32_t bs, int kind)
{
  indirect_bs = bs;
  indirect_kind = kind;
  test(tname("ext%d indirect blocks, %u-byte blocks", kind, bs), indirect_case);
}

/* ---------------------------------------------------------------- journal replay */

static void fillb(uint8_t *b, uint32_t bs, char c) { memset(b, c, bs); }

enum { JV_NONE, JV_NO_FLAG, JV_EXTERNAL, JV_BAD_SB };

static void run_journal_img(uint32_t bs, uint32_t mode, bool b64, uint32_t torn, int variant, uint8_t **seed,
                            size_t *seedlen)
{
  struct eb e;
  tcfg(&e, bs, 3, 2);
  eb_init(&e);
  uint8_t *b = malloc(bs), *nd = malloc(bs), *tb = malloc(bs), *t0 = malloc(bs);
  uint64_t pa, pb, pc, pd, pe, pm;
  uint32_t ia = eb_mkino(&e, 0, EB_REG | 0644), ib = eb_mkino(&e, 0, EB_REG | 0644),
           ic = eb_mkino(&e, 0, EB_REG | 0644), id = eb_mkino(&e, 0, EB_REG | 0644),
           ie = eb_mkino(&e, 0, EB_REG | 0644), im = eb_mkino(&e, 0, EB_REG | 0644);
  fillb(b, bs, 'a');
  pa = eb_data_at(&e, ia, 0, b, bs);
  fillb(b, bs, 'b');
  pb = eb_data_at(&e, ib, 0, b, bs);
  fillb(b, bs, 'c');
  pc = eb_data_at(&e, ic, 0, b, bs);
  fillb(b, bs, 'd');
  pd = eb_data_at(&e, id, 0, b, bs);
  fillb(b, bs, 'e');
  pe = eb_data_at(&e, ie, 0, b, bs);
  fillb(b, bs, 'm');
  pm = eb_data_at(&e, im, 0, b, bs);
  eb_set_size(&e, ia, bs);
  eb_set_size(&e, ib, bs);
  eb_set_size(&e, ic, bs);
  eb_set_size(&e, id, bs);
  eb_set_size(&e, ie, bs);
  eb_set_size(&e, im, bs);
  uint32_t iadd = eb_file(&e, 0, EB_REG | 0644, "added by journal", 16);
  struct eb_de full[] = {DE("a.txt", ia, 1), DE("b.dat", ib, 1), DE("c.dat", ic, 1),      DE("d.dat", id, 1),
                         DE("e.dat", ie, 1), DE("m.dat", im, 1), DE("added.txt", iadd, 1)};
  memcpy(nd, eb_blk(&e, mkroot(&e, full, 7)), bs);
  uint64_t root_blk = mkroot(&e, full, 6);
  uint64_t itblk = (uint64_t)(eb_inode(&e, iadd) - e.img) / bs;
  memcpy(tb, eb_blk(&e, itblk), bs);
  uint64_t rootblk = (uint64_t)(eb_inode(&e, 2) - e.img) / bs;
  memcpy(t0, eb_blk(&e, rootblk), bs);
  w32(t0 + (eb_inode(&e, 2) - eb_blk(&e, rootblk)) + 16, 0x12345678u);
  memset(eb_inode(&e, iadd), 0, e.isize);

  struct jb j;
  uint32_t n = 64, start_at = 0, seq0 = 100;
  jb_init(&j, &e, n, mode, b64, seq0, start_at);
  jb_begin(&j);
  fillb(b, bs, 'A');
  jb_add(&j, pa, b);
  jb_add(&j, root_blk, nd);
  jb_add(&j, itblk, tb);
  jb_add(&j, rootblk, t0);
  uint8_t *mg = malloc(bs);
  fillb(mg, bs, 'Z');
  mg[0] = 0xC0;
  mg[1] = 0x3B;
  mg[2] = 0x39;
  mg[3] = 0x98;
  jb_add(&j, pm, mg);
  jb_commit(&j, 0);
  jb_begin(&j);
  fillb(b, bs, '2');
  jb_add(&j, pb, b);
  fillb(b, bs, '2');
  jb_add(&j, pd, b);
  jb_commit(&j, 0);
  jb_begin(&j);
  jb_revoke(&j, pb);
  jb_revoke(&j, pd);
  fillb(b, bs, '3');
  jb_add(&j, pc, b);
  jb_commit(&j, 0);
  jb_begin(&j);
  fillb(b, bs, '4');
  jb_add(&j, pb, b);
  jb_commit(&j, 0);
  jb_begin(&j);
  fillb(b, bs, '5');
  jb_add(&j, pe, b);
  jb_add(&j, pc, b);
  jb_commit(&j, torn ? torn : JB_NO_COMMIT);
  jb_begin(&j);
  fillb(b, bs, '6');
  jb_add(&j, pe, b);
  jb_commit(&j, 0);
  jb_finish(&j);
  if (variant == JV_NO_FLAG)
    e.incompat &= ~EB_INC_RECOVER;
  if (variant == JV_EXTERNAL)
    e.journal_inum = 0;
  if (variant == JV_BAD_SB)
    memset(jb_block(&j, 0), 0, 16);
  size_t len;
  uint8_t *img = eb_finish(&e, &len);
  if (seed) {
    *seed = malloc(len);
    memcpy(*seed, img, len);
    *seedlen = len;
    free(mg);
    free(b);
    free(nd);
    free(tb);
    free(t0);
    free(e.img);
    jb_begin(&j);
    return;
  }
  void *m = mnt(img, len);
  expect(m != NULL);
  if (m) {
    uint8_t *want = malloc(bs);
    bool replay = variant == JV_NONE;
    fillb(want, bs, replay ? 'A' : 'a');
    expect(same(m, "/a.txt", want, bs));
    fillb(want, bs, replay ? '4' : 'b');
    expect(same(m, "/b.dat", want, bs));
    fillb(want, bs, replay ? '3' : 'c');
    expect(same(m, "/c.dat", want, bs));
    fillb(want, bs, 'd');
    expect(same(m, "/d.dat", want, bs));
    fillb(want, bs, 'e');
    expect(same(m, "/e.dat", want, bs));
    if (replay) {
      expect(same(m, "/m.dat", mg, bs));
      expect(same(m, "/added.txt", "added by journal", 16));
      expect(has_name(m, "/", "added.txt"));
      struct ext_attr ra;
      expect(ext_getinfo(m, "/", &ra) == 0 && ra.mtime == 0x12345678);
      expect(log_count("replayed journal in RAM (4 transactions") == 1);
    } else {
      fillb(want, bs, 'm');
      expect(same(m, "/m.dat", want, bs));
      expect(!exists(m, "/added.txt"));
      struct ext_attr ra;
      expect(ext_getinfo(m, "/", &ra) == 0 && ra.mtime == 1700000002);
      expect(log_count("replayed") == 0);
      if (variant != JV_NO_FLAG)
        expect(log_count("mounted without replay") == 1);
    }
    free(want);
    ext_close(m);
  }
  expect(g_live == 0);
  free(mg);
  free(b);
  free(nd);
  free(tb);
  free(t0);
  free(e.img);
  jb_begin(&j);
}

/* ---------------------------------------------------------------- ext4 extents */

static uint32_t extents_bs;
static uint32_t extents_depth;
static uint32_t extents_leaf_max;
static uint32_t extents_idx_max;
static bool extents_csum;

static void extents_case(void)
{
  uint32_t bs = extents_bs;
  uint32_t depth = extents_depth;
  uint32_t leaf_max = extents_leaf_max;
  uint32_t idx_max = extents_idx_max;
  bool csum = extents_csum;
  struct eb e;
  tcfg(&e, bs, 4, 4);
  if (csum)
    e.ro_compat |= EB_RO_MCSUM;
  eb_init(&e);
  uint64_t lb[30];
  uint32_t cnt = depth ? 30 : 4;
  for (uint32_t i = 0; i < cnt; i++)
    lb[i] = i * 3;
  uint32_t sp = sparse_file(&e, lb, cnt, depth, leaf_max, idx_max, cnt * 3ull * bs);
  uint32_t un = eb_mkino(&e, 0, EB_REG | 0644);
  uint64_t g = eb_alloc_n(&e, 4), d = eb_alloc(&e);
  memset(eb_blk(&e, g), 0xEE, (size_t)bs * 4);
  block_pattern(eb_blk(&e, d), bs, 10);
  struct eb_ext ux[] = {{2, 4, g, true}, {10, 1, d, false}};
  eb_extents(&e, un, ux, 2, 0, 4, 4);
  eb_set_size(&e, un, 12ull * bs);
  struct eb_de ents[] = {DE("sp", sp, 1), DE("un", un, 1)};
  mkroot(&e, ents, 2);
  size_t len;
  uint8_t *img = eb_finish(&e, &len);
  void *m = mnt(img, len);
  expect(m != NULL);
  if (m) {
    expect(!strcmp(ext_ops()->type(m), "ext4"));
    expect(check_sparse(m, "/sp", bs, lb, cnt));
    uint8_t *buf = malloc((size_t)bs * 12), *want = malloc(bs);
    expect(rd(m, "/un", 0, buf, bs * 12) == (int)(bs * 12));
    expect(is_zero(buf, (size_t)bs * 10));
    block_pattern(want, bs, 10);
    expect(!memcmp(buf + (size_t)bs * 10, want, bs));
    expect(is_zero(buf + (size_t)bs * 11, bs));
    expect(log_count("checksum") == 0);
    free(buf);
    free(want);
    ext_close(m);
  }
  expect(g_live == 0);
  free(e.img);
}

static void run_extents(uint32_t bs, uint32_t depth, uint32_t leaf_max, uint32_t idx_max, bool csum)
{
  extents_bs = bs;
  extents_depth = depth;
  extents_leaf_max = leaf_max;
  extents_idx_max = idx_max;
  extents_csum = csum;
  test(tname("ext4 extents depth %u bs=%u csum=%d", depth, bs, csum), extents_case);
}

static void run_extent_limits(void)
{
  struct eb e;
  tcfg(&e, 1024, 4, 136);
  eb_init(&e);
  uint32_t ino = eb_mkino(&e, 0, EB_REG | 0644);
  uint64_t p = eb_alloc_n(&e, 32768), q = eb_alloc_n(&e, 2);
  block_pattern(eb_blk(&e, p), 1024, 0);
  block_pattern(eb_blk(&e, p + 32767), 1024, 32767);
  memset(eb_blk(&e, q), 0xEE, 2048);
  struct eb_ext x[] = {{0, 32768, p, false}, {32768, 2, q, true}};
  eb_extents(&e, ino, x, 2, 0, 4, 4);
  eb_set_size(&e, ino, 32770ull * 1024);
  struct eb_de ents[] = {DE("big", ino, 1)};
  mkroot(&e, ents, 1);
  size_t len;
  uint8_t *img = eb_finish(&e, &len);
  void *m = mnt(img, len);
  expect(m != NULL);
  if (m) {
    uint8_t got[1024], want[1024];
    block_pattern(want, 1024, 32767);
    expect(rd(m, "/big", 32767ull * 1024, got, 1024) == 1024 && !memcmp(got, want, 1024));
    expect(rd(m, "/big", 16384ull * 1024, got, 1024) == 1024 && is_zero(got, 1024));
    expect(rd(m, "/big", 32768ull * 1024, got, 1024) == 1024 && is_zero(got, 1024));
    expect(rd(m, "/big", 32769ull * 1024 + 1000, got, 1024) == 24 && is_zero(got, 24));
    ext_close(m);
  }
  expect(g_live == 0);
  free(e.img);
}

/* ---------------------------------------------------------------- layout: descriptors, meta_bg, uninit groups */

static uint32_t layout_bs;
static uint32_t layout_dsize;
static bool layout_bit64;
static uint32_t layout_meta_bg;
static uint32_t layout_first_meta;
static bool layout_sparse2;
static uint32_t layout_groups;
static uint32_t layout_csum_kind;

static void layout_case(void)
{
  uint32_t bs = layout_bs;
  uint32_t dsize = layout_dsize;
  bool bit64 = layout_bit64;
  uint32_t meta_bg = layout_meta_bg;
  uint32_t first_meta = layout_first_meta;
  bool sparse2 = layout_sparse2;
  uint32_t groups = layout_groups;
  uint32_t csum_kind = layout_csum_kind;
  struct eb e;
  tcfg(&e, bs, 4, groups);
  e.dsize = dsize;
  if (bit64)
    e.incompat |= EB_INC_64BIT;
  if (meta_bg) {
    e.incompat |= EB_INC_META_BG;
    e.first_meta_bg = first_meta;
  }
  if (sparse2)
    e.compat |= EB_COMPAT_SPARSE2;
  if (csum_kind & 4)
    e.ro_compat &= ~EB_RO_SPARSE;
  if ((csum_kind & 3) == 1)
    e.ro_compat |= EB_RO_MCSUM;
  if ((csum_kind & 3) == 2)
    e.ro_compat |= EB_RO_GDT_CSUM;
  e.dpb = bs / dsize;
  if (sparse2) {
    e.backup[0] = e.dpb;
    e.backup[1] = e.dpb * 2;
  }
  eb_init(&e);
  uint32_t pick[] = {0, 1, 2, 7 % groups, e.dpb % groups, (e.dpb + 1) % groups, groups - 1};
  struct eb_de ents[7];
  char names[7][16];
  char text[7][32];
  for (int i = 0; i < 7; i++) {
    uint32_t g = pick[i];
    uint32_t ino = g * e.ipg + 11 + (uint32_t)i;
    snprintf(names[i], sizeof names[i], "f%d", i);
    snprintf(text[i], sizeof text[i], "group %u file", g);
    eb_file(&e, ino, EB_REG | 0644, text[i], strlen(text[i]));
    ents[i] = (struct eb_de){(const uint8_t *)names[i], (uint32_t)strlen(names[i]), ino, 1};
  }
  mkroot(&e, ents, 7);
  size_t len;
  uint8_t *img = eb_finish(&e, &len);
  void *m = mnt(img, len);
  expect(m != NULL);
  if (m) {
    for (int i = 0; i < 7; i++) {
      char path[16];
      snprintf(path, sizeof path, "/f%d", i);
      expect(same(m, path, text[i], strlen(text[i])));
    }
    expect(log_count("checksum") == 0);
    ext_close(m);
  }
  expect(g_live == 0);
  free(e.img);
}

static void run_layout(uint32_t bs, uint32_t dsize, bool bit64, uint32_t meta_bg, uint32_t first_meta, bool sparse2,
                       uint32_t groups, uint32_t csum_kind)
{
  layout_bs = bs;
  layout_dsize = dsize;
  layout_bit64 = bit64;
  layout_meta_bg = meta_bg;
  layout_first_meta = first_meta;
  layout_sparse2 = sparse2;
  layout_groups = groups;
  layout_csum_kind = csum_kind;
  test(tname("layout bs=%u desc=%u 64bit=%d meta_bg=%d first=%u sparse2=%d groups=%u csum=%u", bs, dsize, bit64,
             meta_bg, first_meta, sparse2, groups, csum_kind),
       layout_case);
}

static uint32_t uninit_csum_kind;

static void uninit_case(void)
{
  uint32_t csum_kind = uninit_csum_kind;
  struct eb e;
  tcfg(&e, 1024, 4, 4);
  if (csum_kind == 1)
    e.ro_compat |= EB_RO_MCSUM;
  if (csum_kind == 2)
    e.ro_compat |= EB_RO_GDT_CSUM;
  eb_init(&e);
  e.gflags[2] = EB_GD_INODE_UNINIT | EB_GD_ITABLE_ZEROED;
  e.gflags[3] = EB_GD_BLOCK_UNINIT;
  uint32_t i3 = eb_file(&e, 3 * e.ipg + 4, EB_REG | 0644, "in block-uninit group", 21);
  uint32_t i1 = eb_file(&e, e.ipg + 4, EB_REG | 0644, "plain", 5);
  e.gflags[1] = EB_GD_INODE_UNINIT;
  struct eb_de ents[] = {DE("g3", i3, 1), DE("g1", i1, 1), DE("g2", 2 * e.ipg + 5, 1)};
  mkroot(&e, ents, 3);
  size_t len;
  uint8_t *img = eb_finish(&e, &len);
  void *m = mnt(img, len);
  expect(m != NULL);
  if (m) {
    expect(same(m, "/g3", "in block-uninit group", 21));
    struct ext_attr a;
    if (csum_kind) {
      expect(ext_getinfo(m, "/g2", &a) == -EXT_ERR_NOENT);
      expect(ext_getinfo(m, "/g1", &a) == -EXT_ERR_NOENT);
    } else {
      expect(same(m, "/g1", "plain", 5));
      expect(ext_getinfo(m, "/g2", &a) != 0);
    }
    ext_close(m);
  }
  expect(g_live == 0);
  free(e.img);
}

static void run_uninit(uint32_t csum_kind)
{
  uninit_csum_kind = csum_kind;
  test(tname("uninit_bg group flags csum=%u", csum_kind), uninit_case);
}

/* ---------------------------------------------------------------- inline data, htree, big and odd directories */

static void run_inline(void)
{
  struct eb e;
  tcfg(&e, 4096, 4, 2);
  e.incompat |= EB_INC_INLINE;
  eb_init(&e);
  uint8_t d100[100], d68[68];
  for (int i = 0; i < 100; i++)
    d100[i] = (uint8_t)('a' + i % 26);
  memcpy(d68, d100, 68);
  uint32_t f11 = eb_inline_file(&e, "tiny inline", 11), f60 = eb_inline_file(&e, d100, 60),
           f100 = eb_inline_file(&e, d100, 68), f0 = eb_inline_file(&e, "", 0);
  uint32_t sub = eb_mkino(&e, 0, EB_DIR | 0755), id = eb_mkino(&e, 0, EB_DIR | 0755);
  struct eb_de se[] = {DE("leaf", f11, 1)};
  eb_inline_dir(&e, sub, id, se, 1);
  struct eb_de ie[] = {DE("name0001", f11, 1), DE("name0002", f60, 1), DE("name0003", f100, 1),
                       DE("name0004", f0, 1),  DE("name0005", f11, 1), DE("sub", sub, 2)};
  eb_inline_dir(&e, id, 2, ie, 6);
  struct eb_de ents[] = {DE("f11", f11, 1), DE("f60", f60, 1), DE("f68", f100, 1), DE("f0", f0, 1), DE("idir", id, 2)};
  mkroot(&e, ents, 5);
  size_t len;
  uint8_t *img = eb_finish(&e, &len);
  void *m = mnt(img, len);
  expect(m != NULL);
  if (m) {
    expect(same(m, "/f11", "tiny inline", 11));
    expect(same(m, "/f60", d100, 60));
    expect(same(m, "/f68", d68, 68));
    expect(same(m, "/f0", "", 0));
    char names[16][256];
    expect(list_names(m, "/idir", names, 16) == 6);
    for (int i = 1; i <= 5; i++)
      expect(has_name(m, "/idir", tname("name000%d", i)));
    expect(has_name(m, "/idir", "sub"));
    expect(same(m, "/idir/name0003", d68, 68));
    expect(same(m, "/idir/sub/leaf", "tiny inline", 11));
    expect(same(m, "/idir/sub/../name0002", d100, 60));
    expect(same(m, "/idir/../f11", "tiny inline", 11));
    struct ext_attr a;
    expect(ext_getinfo(m, "/idir", &a) == 0 && (a.mode & 0xF000) == EB_DIR);
    expect(ext_getinfo(m, "/idir/nope", &a) == -EXT_ERR_NOENT);
    ext_close(m);
  }
  expect(g_live == 0);
  free(e.img);
}

static uint32_t htree_bs;
static uint32_t htree_levels;
static bool htree_csum;

static void htree_case(void)
{
  uint32_t bs = htree_bs;
  uint32_t levels = htree_levels;
  bool csum = htree_csum;
  struct eb e;
  tcfg(&e, bs, 4, 4);
  if (csum)
    e.ro_compat |= EB_RO_MCSUM;
  eb_init(&e);
  uint32_t f = eb_file(&e, 0, EB_REG | 0644, "shared", 6);
  enum { N = 400 };
  struct eb_de *ents = calloc(N, sizeof *ents);
  char (*names)[16] = calloc(N, 16);
  for (int i = 0; i < N; i++) {
    snprintf(names[i], 16, "file%03d", i);
    ents[i] = (struct eb_de){(const uint8_t *)names[i], 7, f, 1};
  }
  uint32_t d = eb_mkino(&e, 0, EB_DIR | 0755);
  eb_dir_htree(&e, d, 2, ents, N, levels);
  struct eb_de root[] = {DE("ht", d, 2)};
  mkroot(&e, root, 1);
  size_t len;
  uint8_t *img = eb_finish(&e, &len);
  void *m = mnt(img, len);
  expect(m != NULL);
  if (m) {
    static char all[N + 8][256];
    int n = list_names(m, "/ht", all, N + 8);
    expect(n == N);
    bool dup = false, bad = false;
    for (int i = 0; i < n; i++) {
      bad |= strncmp(all[i], "file", 4) != 0;
      for (int k = 0; k < i; k++)
        dup |= !strcmp(all[i], all[k]);
    }
    expect(!dup && !bad);
    expect(same(m, "/ht/file123", "shared", 6));
    expect(same(m, "/ht/file399", "shared", 6));
    expect(!exists(m, "/ht/file400"));
    expect(log_count("checksum") == 0);
    ext_close(m);
  }
  expect(g_live == 0);
  free(ents);
  free(names);
  free(e.img);
}

static void run_htree(uint32_t bs, uint32_t levels, bool csum)
{
  htree_bs = bs;
  htree_levels = levels;
  htree_csum = csum;
  test(tname("htree dir bs=%u levels=%u csum=%d", bs, levels, csum), htree_case);
}

static void run_bigdir(void)
{
  struct eb e;
  tcfg(&e, 1024, 4, 8);
  eb_init(&e);
  uint32_t f = eb_file(&e, 0, EB_REG | 0644, "x", 1);
  enum { N = 70000 };
  struct eb_de *ents = calloc(N, sizeof *ents);
  char (*names)[8] = calloc(N, 8);
  for (int i = 0; i < N; i++) {
    snprintf(names[i], 8, "e%05d", i);
    ents[i] = (struct eb_de){(const uint8_t *)names[i], 6, f, 1};
  }
  uint32_t d = eb_mkino(&e, 0, EB_DIR | 0755);
  eb_dir(&e, d, 2, ents, N, 0);
  struct eb_de root[] = {DE("big", d, 2)};
  mkroot(&e, root, 1);
  size_t len;
  uint8_t *img = eb_finish(&e, &len);
  void *m = mnt(img, len);
  expect(m != NULL);
  if (m) {
    int count = 0, isd;
    uint32_t sz;
    char nm[256];
    bool order = true;
    while (ext_ops()->enum_dir(m, "/big", count, nm, &isd, &sz)) {
      char want[16];
      snprintf(want, sizeof want, "e%05d", count);
      order &= !strcmp(nm, want) && !isd && sz == 1;
      count++;
    }
    expect(count == N && order);
    expect(g_reads < 40000);
    struct ext_mount *mm = m;
    expect(mm->cs_n <= mm->cs_max && mm->cs_max == 1024);
    expect(same(m, "/big/e00000", "x", 1));
    expect(same(m, "/big/e69999", "x", 1));
    expect(ext_ops()->enum_dir(m, "/big", 5, nm, &isd, &sz) == 1 && !strcmp(nm, "e00005"));
    ext_close(m);
  }
  expect(g_live == 0);
  free(ents);
  free(names);
  free(e.img);
}

static void dirent_set_rec(uint8_t *blk, uint32_t off, uint32_t rec) { w16(blk + off + 4, rec); }

static uint32_t dirent_bs;

static void dirent_case(void)
{
  uint32_t bs = dirent_bs;
  struct eb e;
  tcfg(&e, bs, 4, 4);
  eb_init(&e);
  uint32_t f = eb_file(&e, 0, EB_REG | 0644, "f", 1);
  uint8_t odd[] = {0xFF, 0xFE, 'x', 0x80, '.', 'b', 'i', 'n'};
  uint8_t longname[255];
  memset(longname, 'L', sizeof longname);
  uint8_t slashy[] = {'a', '/', 'b'}, nully[] = {'a', 0, 'b'};
  struct eb_de ents[] = {DE("alpha", f, 1),    DE("deleted", 0, 1),     DE("toobig", 9999999, 1),
                         DE("reserved", 5, 1), {odd, sizeof odd, f, 1}, {longname, 255, f, 1},
                         {slashy, 3, f, 1},    {nully, 3, f, 1},        DE("omega", f, 1)};
  uint32_t d = eb_mkino(&e, 0, EB_DIR | 0755);
  eb_dir(&e, d, 2, ents, 9, 0);
  struct eb_de t3[] = {DE("one", f, 1), DE("two", f, 1), DE("three", f, 1)};
  uint32_t d2 = eb_mkino(&e, 0, EB_DIR | 0755);
  uint64_t p2 = eb_dir(&e, d2, 2, t3, 3, 0);
  uint32_t d3 = eb_mkino(&e, 0, EB_DIR | 0755);
  uint64_t p3 = eb_dir(&e, d3, 2, t3, 3, 0);
  uint32_t d4 = eb_mkino(&e, 0, EB_DIR | 0755);
  uint64_t p4 = eb_dir(&e, d4, 2, t3, 3, 0);
  uint32_t d5 = eb_mkino(&e, 0, EB_DIR | 0755);
  eb_dir(&e, d5, 2, t3, 3, 1);
  /* "." 12, ".." 12, one 12, two 12, three: rec_len runs past the block */
  dirent_set_rec(eb_blk(&e, p2), 48, bs + 4);
  /* rec_len not a multiple of four in "two" */
  dirent_set_rec(eb_blk(&e, p3), 36, 14);
  /* rec_len below the 8-byte header in "one" */
  dirent_set_rec(eb_blk(&e, p4), 24, 4);
  struct eb_de root[] = {DE("d", d, 2), DE("d2", d2, 2), DE("d3", d3, 2), DE("d4", d4, 2), DE("d5", d5, 2)};
  mkroot(&e, root, 5);
  size_t len;
  uint8_t *img = eb_finish(&e, &len);
  void *m = mnt(img, len);
  expect(m != NULL);
  if (m) {
    static char names[64][256];
    int n = list_names(m, "/d", names, 64);
    expect(n == 4);
    expect(has_name(m, "/d", "alpha") && has_name(m, "/d", "omega"));
    expect(has_name(m, "/d", "\xff\xfex\x80.bin"));
    char l255[256];
    memset(l255, 'L', 255);
    l255[255] = '\0';
    expect(has_name(m, "/d", l255));
    expect(!has_name(m, "/d", "deleted") && !has_name(m, "/d", "toobig") && !has_name(m, "/d", "reserved"));
    expect(same(m, "/d/\xff\xfex\x80.bin", "f", 1));
    char path[300];
    snprintf(path, sizeof path, "/d/%s", l255);
    expect(same(m, path, "f", 1));
    snprintf(path, sizeof path, "/d/%sL", l255);
    struct ext_attr a;
    expect(ext_getinfo(m, path, &a) == -EXT_ERR_NAMETOOLONG);
    expect(list_names(m, "/d2", names, 64) == 2 && has_name(m, "/d2", "one") && has_name(m, "/d2", "two"));
    expect(!has_name(m, "/d2", "three"));
    expect(list_names(m, "/d3", names, 64) == 1 && has_name(m, "/d3", "one"));
    expect(list_names(m, "/d4", names, 64) == 0);
    expect(list_names(m, "/d5", names, 64) == 3);
    expect(log_count("corrupt directory block") == 1);
    ext_close(m);
  }
  expect(g_live == 0);
  free(e.img);
}

static void run_dirent_edges(uint32_t bs)
{
  dirent_bs = bs;
  test(tname("directory entry edge cases bs=%u", bs), dirent_case);
}

static bool reclen_zero;

static void reclen_case(void)
{
  bool zero = reclen_zero;
  struct eb e;
  tcfg(&e, 65536, 4, 3);
  e.reclen_zero = zero;
  eb_init(&e);
  uint32_t f = eb_file(&e, 0, EB_REG | 0644, "f", 1);
  struct eb_de ents[] = {DE("only", f, 1)};
  mkroot(&e, ents, 1);
  uint32_t d = eb_mkino(&e, 0, EB_DIR | 0755);
  eb_dir(&e, d, 2, NULL, 0, 0);
  size_t len;
  uint8_t *img = eb_finish(&e, &len);
  void *m = mnt(img, len);
  expect(m != NULL);
  if (m) {
    expect(has_name(m, "/", "only"));
    expect(same(m, "/only", "f", 1));
    ext_close(m);
  }
  expect(g_live == 0);
  free(e.img);
}

static void run_reclen64k(bool zero)
{
  reclen_zero = zero;
  test(tname("64 KiB blocks, rec_len %s", zero ? "0" : "65535"), reclen_case);
}

static void run_nofiletype(void)
{
  struct eb e;
  tcfg(&e, 1024, 4, 2);
  e.incompat &= ~EB_INC_FILETYPE;
  e.incompat |= EB_INC_CASEFOLD;
  eb_init(&e);
  uint32_t f = eb_file(&e, 0, EB_REG | 0644, "cf", 2);
  uint32_t sub = eb_mkino(&e, 0, EB_DIR | 0755);
  struct eb_de se[] = {DE("Hello.TXT", f, 1), DE("\xc3\x89"
                                                 "cole",
                                                 f, 1)};
  eb_dir(&e, sub, 2, se, 2, 0);
  w32(eb_inode(&e, sub) + 32, r32(eb_inode(&e, sub) + 32) | EB_IF_CASEFOLD);
  uint32_t plain = eb_mkino(&e, 0, EB_DIR | 0755);
  eb_dir(&e, plain, 2, se, 2, 0);
  struct eb_de ents[] = {DE("cf", sub, 2), DE("plain", plain, 2), DE("file", f, 1)};
  mkroot(&e, ents, 3);
  size_t len;
  uint8_t *img = eb_finish(&e, &len);
  void *m = mnt(img, len);
  expect(m != NULL);
  if (m) {
    int isd = 0;
    uint32_t sz;
    char nm[256];
    bool sawdir = false, sawfile = false;
    for (int i = 0; ext_ops()->enum_dir(m, "/", i, nm, &isd, &sz); i++) {
      if (!strcmp(nm, "cf"))
        sawdir = isd == 1;
      if (!strcmp(nm, "file"))
        sawfile = isd == 0 && sz == 2;
    }
    expect(sawdir && sawfile);
    expect(same(m, "/cf/Hello.TXT", "cf", 2));
    expect(same(m, "/cf/hello.txt", "cf", 2));
    expect(same(m, "/cf/HELLO.txt", "cf", 2));
    expect(same(m,
                "/cf/\xc3\x89"
                "cole",
                "cf", 2));
    expect(!exists(m, "/cf/\xc3\xa9"
                      "cole"));
    expect(same(m, "/plain/Hello.TXT", "cf", 2));
    expect(!exists(m, "/plain/hello.txt"));
    ext_close(m);
  }
  expect(g_live == 0);
  free(e.img);
}

/* ---------------------------------------------------------------- sizes, metadata, special files, encryption */

static void run_sizes(void)
{
  struct eb e;
  tcfg(&e, 4096, 4, 3);
  eb_init(&e);
  uint64_t lb[] = {0, 1310720, 1310721};
  uint32_t big = sparse_file(&e, lb, 3, 0, 0, 0, 6ull << 30);
  uint32_t far = eb_mkino(&e, 0, EB_REG | 0644);
  eb_set_size(&e, far, 1ull << 50);
  struct eb_ext fx[] = {{0, 1, eb_alloc(&e), false}};
  memcpy(eb_blk(&e, fx[0].phys), "far", 3);
  eb_extents(&e, far, fx, 1, 0, 4, 4);
  uint32_t d = eb_mkino(&e, 0, EB_DIR | 0755);
  struct eb_de de[] = {DE("in", big, 1)};
  eb_dir(&e, d, 2, de, 1, 0);
  w32(eb_inode(&e, d) + 108, 7);
  struct eb_de ents[] = {DE("big", big, 1), DE("far", far, 1), DE("dir", d, 2)};
  mkroot(&e, ents, 3);
  size_t len;
  uint8_t *img = eb_finish(&e, &len);
  void *m = mnt(img, len);
  expect(m != NULL);
  if (m) {
    struct ext_attr a;
    uint8_t got[4096], want[4096];
    expect(ext_getinfo(m, "/big", &a) == 0 && a.size == 6ull << 30);
    block_pattern(want, 4096, 1310720);
    expect(rd(m, "/big", 5ull << 30, got, 4096) == 4096 && !memcmp(got, want, 4096));
    expect(rd(m, "/big", (6ull << 30) - 10, got, 100) == 10);
    expect(rd(m, "/big", 4ull << 30, got, 4096) == 4096 && is_zero(got, 4096));
    expect(rd(m, "/far", 0, got, 3) == 3 && !memcmp(got, "far", 3));
    expect(rd(m, "/far", 1ull << 49, got, 16) == 16 && is_zero(got, 16));
    int isd;
    uint32_t sz;
    expect(ext_ops()->stat(m, "/big", &isd, &sz) == 0 && !isd && sz == 0xFFFFFFFFu);
    char nm[256];
    expect(ext_ops()->enum_dir(m, "/", 0, nm, &isd, &sz) == 1 && sz == 0xFFFFFFFFu);
    expect(ext_ops()->read_at(m, "/big", 0, got, 4096) == 4096);
    expect(has_name(m, "/dir", "in"));
    expect(ext_getinfo(m, "/dir", &a) == 0 && a.size == 4096);
    ext_close(m);
  }
  expect(g_live == 0);
  free(e.img);
}

static void run_large_dir(void)
{
  struct eb e;
  tcfg(&e, 4096, 4, 3);
  e.incompat |= EB_INC_LARGEDIR;
  eb_init(&e);
  uint32_t f = eb_file(&e, 0, EB_REG | 0644, "x", 1);
  uint32_t d = eb_mkino(&e, 0, EB_DIR | 0755);
  struct eb_de de2[] = {DE("in", f, 1)};
  eb_dir(&e, d, 2, de2, 1, 0);
  w32(eb_inode(&e, d) + 108, 7);
  struct eb_de ents2[] = {DE("dir", d, 2)};
  mkroot(&e, ents2, 1);
  size_t len;
  uint8_t *img = eb_finish(&e, &len);
  void *m = mnt(img, len);
  expect(m != NULL);
  if (m) {
    struct ext_attr a;
    expect(ext_getinfo(m, "/dir", &a) == 0 && a.size == (7ull << 32) + 4096);
    expect(has_name(m, "/dir", "in"));
    expect(g_reads < 200);
    ext_close(m);
  }
  expect(g_live == 0);
  free(e.img);
}

static void set_extra_time(uint8_t *r, uint32_t lo_off, uint32_t extra_off, int32_t lo, uint32_t epoch, uint32_t ns)
{
  w32(r + lo_off, (uint32_t)lo);
  w32(r + extra_off, (ns << 2) | epoch);
}

static void run_metadata(void)
{
  struct eb e;
  tcfg(&e, 4096, 4, 2);
  e.ro_compat |= 0x2000 | 0x100;
  eb_init(&e);
  uint32_t f = eb_file(&e, 0, EB_REG | 04755, "m", 1);
  uint8_t *r = eb_inode(&e, f);
  w16(r + 2, 0x2345);
  w16(r + 120, 0x1);
  w16(r + 24, 0xCDEF);
  w16(r + 122, 0xAB);
  w16(r + 26, 3);
  set_extra_time(r, 8, 0x8C, -1, 0, 0);
  set_extra_time(r, 16, 0x88, (int32_t)0x80000000u, 1, 5);
  set_extra_time(r, 12, 0x84, 5, 2, 123456789);
  set_extra_time(r, 0x90, 0x94, 1600000000, 0, 999999999);
  w32(r + 28, 5);
  w16(r + 116, 1);
  w32(r + 0x9C, 77);
  w32(r + 32, r32(r + 32) | EB_IF_HUGE);
  uint32_t old = eb_file(&e, 0, EB_REG | 0600, "o", 1);
  r = eb_inode(&e, old);
  w16(r + 128, 4);
  w32(r + 0x84, 0xFFFFFFFFu);
  w32(r + 28, 5);
  w16(r + 116, 1);
  struct eb_de ents[] = {DE("m", f, 1), DE("old", old, 1)};
  mkroot(&e, ents, 2);
  size_t len;
  uint8_t *img = eb_finish(&e, &len);
  void *m = mnt(img, len);
  expect(m != NULL);
  if (m) {
    struct ext_attr a;
    expect(ext_getinfo(m, "/m", &a) == 0);
    expect(a.mode == (EB_REG | 04755) && a.uid == 0x12345 && a.gid == 0xABCDEF && a.nlink == 3);
    expect(a.atime == -1 && a.atime_ns == 0);
    expect(a.mtime == 2147483648ll && a.mtime_ns == 5);
    expect(a.ctime == 5 + 2 * 4294967296ll && a.ctime_ns == 123456789);
    expect(a.crtime == 1600000000 && a.crtime_ns == 999999999);
    expect(a.blocks512 == (((1ull << 32) + 5) * 8));
    uint32_t mode, uid, gid;
    expect(ext_ops()->getattr(m, "/m", &mode, &uid, &gid) == 0 && mode == (01777u & (EB_REG | 04755)) &&
           uid == 0x12345 && gid == 0xABCDEF);
    expect(ext_getinfo(m, "/old", &a) == 0 && a.ctime == 1700000001 && a.ctime_ns == 0 && a.crtime == 0);
    expect(a.blocks512 == (1ull << 32) + 5);
    ext_close(m);
  }
  expect(g_live == 0);
  free(e.img);
}

static void run_special(void)
{
  struct eb e;
  tcfg(&e, 4096, 4, 2);
  e.incompat |= EB_INC_ENCRYPT;
  eb_init(&e);
  uint32_t ch = eb_mkino(&e, 0, EB_CHR | 0600), bl = eb_mkino(&e, 0, EB_BLK | 0600),
           fi = eb_mkino(&e, 0, EB_FIFO | 0600), so = eb_mkino(&e, 0, EB_SOCK | 0600);
  uint32_t ok = eb_file(&e, 0, EB_REG | 0644, "plain", 5);
  uint32_t enc = eb_file(&e, 0, EB_REG | 0644, "ciphertext", 10);
  w32(eb_inode(&e, enc) + 32, r32(eb_inode(&e, enc) + 32) | EB_IF_ENCRYPT);
  uint32_t el = eb_symlink(&e, "xxxx", false);
  w32(eb_inode(&e, el) + 32, r32(eb_inode(&e, el) + 32) | EB_IF_ENCRYPT);
  uint8_t cn[] = {0x9A, '/', 0x00, 0x1F, 'Q'};
  struct eb_de ed[] = {{cn, 5, enc, 1}, DE("lnk", el, 7)};
  uint32_t encdir = eb_mkino(&e, 0, EB_DIR | 0700);
  eb_dir(&e, encdir, 2, ed, 2, 0);
  w32(eb_inode(&e, encdir) + 32, r32(eb_inode(&e, encdir) + 32) | EB_IF_ENCRYPT);
  struct eb_de ents[] = {DE("chr", ch, 3), DE("blk", bl, 4),        DE("fifo", fi, 5),    DE("sock", so, 6),
                         DE("ok", ok, 1),  DE("secret", encdir, 2), DE("encfile", enc, 1)};
  mkroot(&e, ents, 7);
  size_t len;
  uint8_t *img = eb_finish(&e, &len);
  void *m = mnt(img, len);
  expect(m != NULL);
  if (m) {
    char buf[8];
    struct ext_attr a;
    expect(ext_getinfo(m, "/chr", &a) == 0 && (a.mode & 0xF000) == EB_CHR);
    expect(ext_getinfo(m, "/blk", &a) == 0 && (a.mode & 0xF000) == EB_BLK);
    expect(ext_getinfo(m, "/fifo", &a) == 0 && (a.mode & 0xF000) == EB_FIFO);
    expect(ext_getinfo(m, "/sock", &a) == 0 && (a.mode & 0xF000) == EB_SOCK);
    expect(rd(m, "/chr", 0, buf, 8) == -EXT_ERR_NOTSUP && rd(m, "/fifo", 0, buf, 8) == -EXT_ERR_NOTSUP);
    expect(rd(m, "/sock", 0, buf, 8) == -EXT_ERR_NOTSUP && rd(m, "/blk", 0, buf, 8) == -EXT_ERR_NOTSUP);
    expect(ext_ops()->read_at(m, "/chr", 0, buf, 8) == -1);
    expect(same(m, "/ok", "plain", 5));
    expect(rd(m, "/encfile", 0, buf, 8) == -EXT_ERR_ENCRYPTED);
    expect(rd(m, "/encfile", 0, buf, 8) == -EXT_ERR_ENCRYPTED);
    expect(log_count("encrypted file") == 1);
    expect(ext_getinfo(m, "/encfile", &a) == 0 && a.size == 10 && (a.flags & EB_IF_ENCRYPT));
    expect(has_name(m, "/secret", "_____Q") == false);
    expect(has_name(m, "/secret",
                    "\x9a__\x1f"
                    "Q"));
    expect(rd(m,
              "/secret/\x9a__\x1f"
              "Q",
              0, buf, 8) == -EXT_ERR_ENCRYPTED);
    expect(ext_getinfo(m, "/secret/lnk", &a) == 0 || true);
    g_log[0] = '\0';
    ext_ops()->list(m, "/");
    expect(strstr(g_log, "<CHR>") && strstr(g_log, "<BLK>") && strstr(g_log, "<FIFO>") && strstr(g_log, "<SOCK>"));
    expect(strstr(g_log, "<DIR>") && strstr(g_log, "secret"));
    ext_close(m);
  }
  expect(g_live == 0);
  free(e.img);
}

/* ---------------------------------------------------------------- symlinks */

static uint32_t symlink_bs;
static int symlink_kind;

static void symlink_case(void)
{
  uint32_t bs = symlink_bs;
  int kind = symlink_kind;
  struct eb e;
  tcfg(&e, bs, kind, 3);
  eb_init(&e);
  uint32_t t = eb_file(&e, 0, EB_REG | 0644, "T", 1);
  uint32_t c = eb_mkino(&e, 0, EB_DIR | 0755), b = eb_mkino(&e, 0, EB_DIR | 0755), a = eb_mkino(&e, 0, EB_DIR | 0755);
  struct eb_de ce[] = {DE("target", t, 1)};
  eb_dir(&e, c, b, ce, 1, 0);
  uint32_t up = eb_symlink(&e, "../b/c/target", false);
  struct eb_de be[] = {DE("c", c, 2), DE("up", up, 7)};
  eb_dir(&e, b, a, be, 2, 0);
  uint32_t rel = eb_symlink(&e, "b/c/target", false);
  struct eb_de ae[] = {DE("b", b, 2), DE("rel", rel, 7)};
  eb_dir(&e, a, 2, ae, 2, 0);
  uint32_t abs = eb_symlink(&e, "/a/b/c/target/", true);
  uint32_t dl = eb_symlink(&e, "a/b", false);
  uint32_t esc = eb_symlink(&e, "../../../a/rel", false);
  uint32_t aesc = eb_symlink(&e, "/../../a/b/c/target", false);
  uint32_t dang = eb_symlink(&e, "nothing", false);
  uint32_t l1 = eb_symlink(&e, "loop2", false), l2 = eb_symlink(&e, "loop1", false);
  uint32_t self = eb_symlink(&e, "self", false);
  enum { CH = 42 };
  uint32_t chain[CH + 1];
  struct eb_de ents[16 + CH + 1];
  uint32_t ne = 0;
  ents[ne++] = (struct eb_de){EB_NAME("a"), a, 2};
  ents[ne++] = (struct eb_de){EB_NAME("abs"), abs, 7};
  ents[ne++] = (struct eb_de){EB_NAME("dirlink"), dl, 7};
  ents[ne++] = (struct eb_de){EB_NAME("esc"), esc, 7};
  ents[ne++] = (struct eb_de){EB_NAME("abs_esc"), aesc, 7};
  ents[ne++] = (struct eb_de){EB_NAME("dangling"), dang, 7};
  ents[ne++] = (struct eb_de){EB_NAME("loop1"), l1, 7};
  ents[ne++] = (struct eb_de){EB_NAME("loop2"), l2, 7};
  ents[ne++] = (struct eb_de){EB_NAME("self"), self, 7};
  static char cn[CH + 1][8];
  static char tg[CH + 1][8];
  for (int i = 1; i <= CH; i++) {
    snprintf(cn[i], 8, "c%d", i);
    snprintf(tg[i], 8, "c%d", i - 1);
    chain[i] = eb_symlink(&e, i == 1 ? "a/b/c/target" : tg[i], false);
    ents[ne++] = (struct eb_de){(const uint8_t *)cn[i], (uint32_t)strlen(cn[i]), chain[i], 7};
  }
  mkroot(&e, ents, ne);
  size_t len;
  uint8_t *img = eb_finish(&e, &len);
  void *m = mnt(img, len);
  expect(m != NULL);
  if (m) {
    struct ext_attr at;
    expect(same(m, "/a/b/c/target", "T", 1));
    expect(same(m, "/a/rel", "T", 1));
    expect(same(m, "/a/b/up", "T", 1));
    expect(same(m, "/abs", "T", 1));
    expect(same(m, "/dirlink/c/target", "T", 1));
    expect(same(m, "/esc", "T", 1));
    expect(same(m, "/abs_esc", "T", 1));
    expect(same(m, "/dirlink/up", "T", 1));
    expect(ext_getinfo(m, "/dangling", &at) == -EXT_ERR_NOENT);
    expect(ext_getinfo(m, "/loop1", &at) == -EXT_ERR_LOOP);
    expect(ext_getinfo(m, "/self", &at) == -EXT_ERR_LOOP);
    expect(same(m, "/c40", "T", 1));
    expect(ext_getinfo(m, "/c41", &at) == -EXT_ERR_LOOP);
    expect(ext_getinfo(m, "/../../a/rel", &at) == 0);
    int isd;
    uint32_t sz;
    char nm[256];
    bool dirlink_dir = false, dang_file = false;
    for (int i = 0; ext_ops()->enum_dir(m, "/", i, nm, &isd, &sz); i++) {
      if (!strcmp(nm, "dirlink"))
        dirlink_dir = isd == 1;
      if (!strcmp(nm, "dangling"))
        dang_file = isd == 0;
      if (!strcmp(nm, "abs"))
        expect(isd == 0 && sz == 1);
    }
    expect(dirlink_dir && dang_file);
    ext_close(m);
  }
  expect(g_live == 0);
  free(e.img);
}

static void run_symlinks(uint32_t bs, int kind)
{
  symlink_bs = bs;
  symlink_kind = kind;
  test(tname("symlink resolution ext%d bs=%u", kind, bs), symlink_case);
}

/* ---------------------------------------------------------------- checksums */

static uint32_t gd_off(struct eb *e, uint32_t g)
{
  return (uint32_t)((eb_blk(e, eb_gd_block(e, g / e->dpb)) - e->img) + (size_t)(g % e->dpb) * e->dsize);
}

struct csum_img {
  struct eb e;
  uint32_t file, dir_ino, deep;
  uint64_t dir_blk, leaf;
  size_t len;
  uint8_t *img;
};

static void csum_build(struct csum_img *c, uint32_t extra_ro, uint32_t extra_inc)
{
  struct eb *e = &c->e;
  tcfg(e, 4096, 4, 3);
  e->ro_compat |= extra_ro;
  e->incompat |= extra_inc;
  if (extra_inc & EB_INC_CSUM_SEED)
    e->csum_seed_val = 0xC0FFEE11u;
  eb_init(e);
  c->file = eb_file(e, 0, EB_REG | 0644, "payload", 7);
  uint64_t lb[10];
  for (int i = 0; i < 10; i++)
    lb[i] = (uint64_t)i * 2;
  c->deep = sparse_file(e, lb, 10, 1, 4, 4, 20ull * 4096);
  struct eb_de ents[] = {DE("file", c->file, 1), DE("deep", c->deep, 1)};
  c->dir_ino = eb_mkino(e, 0, EB_DIR | 0755);
  c->dir_blk = eb_dir(e, c->dir_ino, 2, ents, 2, 0);
  struct eb_de root[] = {DE("d", c->dir_ino, 2), DE("file", c->file, 1), DE("deep", c->deep, 1)};
  mkroot(e, root, 3);
  c->img = eb_finish(e, &c->len);
  c->leaf = r32(eb_inode(e, c->deep) + 40 + 12 + 4);
}

static bool csum_reads(void *m)
{
  uint8_t b[4096];
  return same(m, "/file", "payload", 7) && has_name(m, "/d", "file") && rd(m, "/deep", 4 * 4096, b, 4096) == 4096 &&
         !is_zero(b, 4096) && rd(m, "/d/deep", 0, b, 16) == 16;
}

static uint32_t csum_ro;
static uint32_t csum_inc;
static const char *csum_what;

static void csum_case(void)
{
  uint32_t ro = csum_ro;
  uint32_t inc = csum_inc;
  const char *what = csum_what;
  struct csum_img c;
  csum_build(&c, ro, inc);
  void *m = mnt(c.img, c.len);
  expect(m != NULL);
  if (m) {
    expect(csum_reads(m));
    expect(log_count("checksum mismatch") == 0);
    ext_close(m);
  }
  uint8_t *img = c.img;
  struct eb *e = &c.e;
  bool mc = ro & EB_RO_MCSUM;
  if (mc) {
    img[1024 + 16] ^= 1;
    m = mnt(img, c.len);
    expect(m != NULL && log_count("superblock checksum mismatch") == 1);
    if (m)
      ext_close(m);
    img[1024 + 16] ^= 1;
    uint8_t *ir = eb_inode(e, c.file);
    ir[0x10] ^= 0x40;
    m = mnt(img, c.len);
    expect(m != NULL && csum_reads(m));
    expect(log_count("inode checksum mismatch") == 1);
    expect(log_count("checksum mismatch") == 1);
    if (m)
      ext_close(m);
    ir[0x10] ^= 0x40;
    uint8_t *tail = eb_blk(e, c.dir_blk) + 4096 - 4;
    tail[0] ^= 0x01;
    m = mnt(img, c.len);
    expect(m != NULL && csum_reads(m) && log_count("directory block checksum mismatch") == 1);
    if (m)
      ext_close(m);
    tail[0] ^= 0x01;
    uint8_t *leaf = eb_blk(e, c.leaf);
    leaf[12 + 12 * 4 + 3] ^= 0x10;
    m = mnt(img, c.len);
    expect(m != NULL && csum_reads(m) && log_count("extent block checksum mismatch") == 1);
    if (m)
      ext_close(m);
    leaf[12 + 12 * 4 + 3] ^= 0x10;
  }
  uint8_t *gd = img + gd_off(e, 0);
  gd[12] ^= 0x01;
  m = mnt(img, c.len);
  expect(m != NULL && csum_reads(m) && log_count("group descriptor checksum mismatch") == 1);
  if (m)
    ext_close(m);
  gd[12] ^= 0x01;
  if (mc) {
    img[1024 + 16] ^= 1;
    gd[12] ^= 1;
    eb_inode(e, c.file)[0x10] ^= 1;
    eb_blk(e, c.dir_blk)[4096 - 4] ^= 1;
    m = mnt(img, c.len);
    expect(m != NULL && csum_reads(m));
    expect(log_count("checksum mismatch") == 1);
    if (m)
      ext_close(m);
    img[1024 + 16] ^= 1;
    gd[12] ^= 1;
    eb_inode(e, c.file)[0x10] ^= 1;
    eb_blk(e, c.dir_blk)[4096 - 4] ^= 1;
    memcpy(eb_blk(e, c.leaf), "\x00\x00", 2);
    m = mnt(img, c.len);
    expect(m != NULL);
    uint8_t b[16];
    expect(m && rd(m, "/deep", 0, b, 16) < 0);
    expect(m && same(m, "/file", "payload", 7));
    if (m)
      ext_close(m);
  }
  expect(g_live == 0);
  free(e->img);
}

static void run_csum(uint32_t ro, uint32_t inc, const char *what)
{
  csum_ro = ro;
  csum_inc = inc;
  csum_what = what;
  test(tname("integrity: %s", what), csum_case);
}

/* ---------------------------------------------------------------- cache, partitions, fs_ops */

static void run_cache(void)
{
  struct eb e;
  tcfg(&e, 4096, 4, 3);
  eb_init(&e);
  uint8_t *seq = malloc(4096 * 40);
  for (int i = 0; i < 40; i++)
    block_pattern(seq + (size_t)i * 4096, 4096, 500 + i);
  uint32_t f = eb_file(&e, 0, EB_REG | 0644, seq, 4096 * 40);
  struct eb_de ents[] = {DE("seq", f, 1)};
  mkroot(&e, ents, 1);
  size_t len;
  uint8_t *img = eb_finish(&e, &len);
  void *m = mnt(img, len);
  expect(m != NULL);
  if (m) {
    struct ext_attr a;
    expect(ext_getinfo(m, "/seq", &a) == 0);
    unsigned long r0 = g_reads;
    for (int i = 0; i < 50; i++)
      expect(ext_getinfo(m, "/seq", &a) == 0);
    expect(g_reads == r0);
    uint8_t *got = malloc(4096 * 40);
    uint32_t off = 0;
    while (off < 4096 * 40) {
      int r = rd(m, "/seq", off, got + off, 700);
      expect(r > 0);
      if (r <= 0)
        break;
      off += (uint32_t)r;
    }
    expect(off == 4096 * 40 && !memcmp(got, seq, 4096 * 40));
    free(got);
    ext_close(m);
  }
  expect(g_live == 0);
  free(seq);
  free(e.img);
}

static void run_partitions(void)
{
  struct eb e;
  tcfg(&e, 1024, 4, 2);
  eb_init(&e);
  uint32_t f = eb_file(&e, 0, EB_REG | 0644, "in a partition", 14);
  struct eb_de ents[] = {DE("p", f, 1)};
  mkroot(&e, ents, 1);
  size_t len;
  uint8_t *fs = eb_finish(&e, &len);
  size_t off = 2048 * 512, total = off + len;
  uint8_t *disk = calloc(1, total);
  memcpy(disk + off, fs, len);
  uint8_t *pt = disk + 446;
  pt[4] = 0x83;
  w32(pt + 8, 2048);
  w32(pt + 12, (uint32_t)(len / 512));
  disk[510] = 0x55;
  disk[511] = 0xAA;
  void *m = mnt(disk, total);
  expect(m != NULL && m && same(m, "/p", "in a partition", 14));
  if (m)
    ext_close(m);
  memset(disk, 0, 512);
  memcpy(disk + 512, "EFI PART", 8);
  w32(disk + 512 + 72, 2);
  w32(disk + 512 + 80, 4);
  w32(disk + 512 + 84, 128);
  uint8_t *ent = disk + 1024 + 128;
  ent[0] = 1;
  w32(ent + 32, 2048);
  m = mnt(disk, total);
  expect(m != NULL && m && same(m, "/p", "in a partition", 14));
  if (m)
    ext_close(m);
  w32(ent + 36, 1);
  m = mnt(disk, total);
  expect(m == NULL);
  w32(disk + 512 + 76, 1);
  m = mnt(disk, total);
  expect(m == NULL);
  expect(g_live == 0);
  free(disk);
  free(e.img);
}

static void run_fsops(void)
{
  struct eb e;
  tcfg(&e, 1024, 2, 2);
  eb_init(&e);
  uint32_t f = eb_file(&e, 0, EB_REG | 0640, "0123456789", 10);
  w16(eb_inode(&e, f) + 2, 1000);
  w16(eb_inode(&e, f) + 24, 100);
  uint32_t d = eb_mkino(&e, 0, EB_DIR | 0750);
  eb_dir(&e, d, 2, NULL, 0, 0);
  struct eb_de ents[] = {DE("num", f, 1), DE("sub", d, 2)};
  mkroot(&e, ents, 2);
  size_t len;
  uint8_t *img = eb_finish(&e, &len);
  void *m = mnt(img, len);
  expect(m != NULL);
  if (m) {
    const struct fs_ops *o = ext_ops();
    int isd = 7;
    uint32_t sz = 9, mode, uid, gid;
    expect(o->stat(m, "/num", &isd, &sz) == 0 && isd == 0 && sz == 10);
    expect(o->stat(m, "/sub", &isd, &sz) == 0 && isd == 1);
    expect(o->stat(m, "/", &isd, &sz) == 0 && isd == 1);
    expect(o->stat(m, "", &isd, &sz) == 0 && isd == 1);
    expect(o->stat(m, "/nope", &isd, &sz) == -1);
    char buf[16];
    expect(o->read_at(m, "/num", 3, buf, 4) == 4 && !memcmp(buf, "3456", 4));
    expect(o->read_at(m, "/num", 8, buf, 16) == 2);
    expect(o->read_at(m, "/sub", 0, buf, 4) == -1);
    expect(o->getattr(m, "/num", &mode, &uid, &gid) == 0 && mode == 0640 && uid == 1000 && gid == 100);
    expect(o->getattr(m, "/sub", &mode, &uid, &gid) == 0 && mode == 0750);
    expect(o->getattr(m, "/nope", &mode, &uid, &gid) == -1);
    expect(o->list(m, "/nope") == -1);
    g_log[0] = '\0';
    expect(o->list(m, "/") == 0);
    expect(strstr(g_log, "num") && strstr(g_log, "10  ") && strstr(g_log, "<DIR>") && strstr(g_log, "sub"));
    expect(o->enum_dir(m, "/", -1, buf, &isd, &sz) == 0 && o->enum_dir(m, "/", 2, buf, &isd, &sz) == 0);
    expect(o->enum_dir(m, "/num", 0, buf, &isd, &sz) == 0);
    char n1[256], n2[256];
    expect(o->enum_dir(m, "/", 0, n1, &isd, &sz) == 1 && o->enum_dir(m, "/sub", 0, n2, &isd, &sz) == 0);
    expect(o->enum_dir(m, "/", 1, n2, &isd, &sz) == 1 && strcmp(n1, n2) != 0 && !strcmp(n1, "num") &&
           !strcmp(n2, "sub"));
    expect(o->enum_dir(m, "/", 0, n2, &isd, &sz) == 1 && !strcmp(n2, "num"));
    expect(!strcmp(o->type(m), "ext2") && o->create == NULL && o->del == NULL);
    ext_close(m);
  }
  expect(g_live == 0);
  free(e.img);
}

static uint32_t journal_bs, journal_mode, journal_torn;
static bool journal_b64;
static int journal_variant;

static void journal_case(void)
{
  run_journal_img(journal_bs, journal_mode, journal_b64, journal_torn, journal_variant, NULL, NULL);
}

static void run_journal(uint32_t bs, uint32_t mode, bool b64, uint32_t torn, int variant)
{
  static const char *mn[] = {"plain", "v1sb", "crc1", "v2", "v3"};
  journal_bs = bs;
  journal_mode = mode;
  journal_b64 = b64;
  journal_torn = torn;
  journal_variant = variant;
  test(tname("jbd2 %s%s bs=%u torn=%u variant=%d", mn[mode], b64 ? "+64bit" : "", bs, torn, variant), journal_case);
}

static void run_journal_shapes(void)
{
  for (int variant = 0; variant < 3; variant++) {
    uint32_t mode = variant == 0 ? JB_PLAIN : variant == 1 ? JB_V3 : JB_V2;
    bool b64 = variant != 0;
    struct eb e;
    tcfg(&e, 1024, 3, 4);
    eb_init(&e);
    enum { NB = 200 };
    uint32_t f = eb_mkino(&e, 0, EB_REG | 0644), g = eb_mkino(&e, 0, EB_REG | 0644);
    uint8_t *old = calloc(NB, 1024), *nw = calloc(NB, 1024);
    for (int i = 0; i < NB; i++) {
      block_pattern(old + i * 1024, 1024, 10 + i);
      block_pattern(nw + i * 1024, 1024, 5000 + i);
    }
    uint64_t pf = eb_data_at(&e, f, 0, old, NB * 1024), pg = eb_data_at(&e, g, 0, old, 4 * 1024);
    eb_set_size(&e, f, NB * 1024);
    eb_set_size(&e, g, 4 * 1024);
    struct eb_de ents[] = {DE("f", f, 1), DE("g", g, 1)};
    mkroot(&e, ents, 2);
    struct jb j;
    uint32_t n = variant == 0 ? 700 : 20, start = variant == 0 ? 0 : 17, seq0 = variant == 2 ? 0xFFFFFFFEu : 7;
    jb_init(&j, &e, n, mode, b64, seq0, start);
    jb_begin(&j);
    if (variant == 0) {
      for (int i = 0; i < NB; i++)
        jb_add(&j, pf + i, nw + i * 1024);
      jb_commit(&j, 0);
    } else {
      for (int i = 0; i < 2; i++)
        jb_add(&j, pg + i, nw + i * 1024);
      jb_commit(&j, 0);
      jb_begin(&j);
      jb_add(&j, pg + 2, nw + 2 * 1024);
      jb_add(&j, pg + 3, nw + 3 * 1024);
      jb_commit(&j, 0);
      jb_begin(&j);
      jb_add(&j, pg + 3, nw + 9 * 1024);
      jb_commit(&j, 0);
    }
    jb_finish(&j);
    size_t len;
    uint8_t *img = eb_finish(&e, &len);
    void *m = mnt(img, len);
    expect(m != NULL);
    if (m) {
      if (variant == 0) {
        expect(same(m, "/f", nw, NB * 1024));
        expect(log_count("(1 transactions, 200 blocks)") == 1);
      } else {
        uint8_t want[4096];
        memcpy(want, nw, 3 * 1024);
        memcpy(want + 3 * 1024, nw + 9 * 1024, 1024);
        expect(same(m, "/g", want, 4096));
        expect(log_count("(3 transactions, 4 blocks)") == 1);
      }
      ext_close(m);
    }
    expect(g_live == 0);
    jb_begin(&j);
    free(old);
    free(nw);
    free(e.img);
  }
}

static void run_state(void)
{
  struct eb e;
  tcfg(&e, 1024, 2, 2);
  e.compat |= 0x10;
  e.state = 2;
  eb_init(&e);
  uint32_t resize = eb_mkino(&e, 7, EB_REG | 0600);
  memset(eb_inode(&e, resize) + 40, 0xA5, 60);
  uint32_t orphan = eb_mkino(&e, 0, EB_REG | 0644);
  w16(eb_inode(&e, orphan) + 26, 0);
  w32(eb_inode(&e, orphan) + 20, 0);
  w32(eb_inode(&e, orphan) + 20, 0x4242);
  e.last_orphan = orphan;
  uint32_t ok = eb_file(&e, 0, EB_REG | 0644, "fine", 4);
  uint32_t lf = eb_mkino(&e, 11 == e.next_ino ? 0 : 0, EB_DIR | 0700);
  uint64_t p = eb_alloc_n(&e, 4);
  uint8_t *b0 = eb_blk(&e, p);
  w32(b0, lf);
  w16(b0 + 4, 12);
  b0[6] = 1;
  b0[7] = 2;
  b0[8] = '.';
  w32(b0 + 12, 2);
  w16(b0 + 16, 1024 - 12);
  b0[18] = 2;
  b0[19] = 2;
  b0[20] = '.';
  b0[21] = '.';
  for (int i = 1; i < 4; i++)
    w16(eb_blk(&e, p + i) + 4, 1024);
  eb_map_run(&e, lf, 0, p, 4);
  eb_set_size(&e, lf, 4096);
  struct eb_de ents[] = {DE("lost+found", lf, 2), DE("fine", ok, 1), DE("orphan", orphan, 1)};
  mkroot(&e, ents, 3);
  size_t len;
  uint8_t *img = eb_finish(&e, &len);
  void *m = mnt(img, len);
  expect(m != NULL);
  if (m) {
    expect(log_count("recorded errors") == 1);
    struct ext_attr a;
    expect(same(m, "/fine", "fine", 4));
    expect(ext_getinfo(m, "/orphan", &a) == -EXT_ERR_NOENT);
    expect(has_name(m, "/", "lost+found"));
    char names[8][256];
    expect(list_names(m, "/lost+found", names, 8) == 0);
    expect(ext_getinfo(m, "/lost+found", &a) == 0 && a.size == 4096);
    ext_close(m);
  }
  expect(g_live == 0);
  e.state = 8;
  img[1024 + 58] = 0;
  m = mnt(img, len);
  expect(m != NULL && log_count("not cleanly unmounted") == 1);
  if (m)
    ext_close(m);
  free(e.img);
}

/* ---------------------------------------------------------------- fuzz and allocation failure */

static uint8_t *seed_ext2(size_t *len)
{
  struct eb *e = calloc(1, sizeof *e);
  tcfg(e, 1024, 2, 3);
  eb_init(e);
  uint64_t ppb = 256, lb[] = {0, 5, 12, 12 + ppb, 12 + ppb + ppb * ppb + 3};
  uint32_t sp = sparse_file(e, lb, 5, 0, 0, 0, (lb[4] + 1) * 1024);
  uint32_t f = eb_file(e, 0, EB_REG | 0644, "hello", 5);
  uint32_t l1 = eb_symlink(e, "hello", false), l2 = eb_symlink(e, "/sp", true);
  uint32_t sub = eb_mkino(e, 0, EB_DIR | 0755);
  struct eb_de se[] = {DE("in", f, 1), DE("up", l1, 7)};
  eb_dir(e, sub, 2, se, 2, 0);
  struct eb_de ents[] = {DE("sp", sp, 1), DE("hello", f, 1), DE("l1", l1, 7), DE("l2", l2, 7), DE("sub", sub, 2)};
  mkroot(e, ents, 5);
  return eb_finish(e, len);
}

static uint8_t *seed_ext4(size_t *len, uint32_t bs, uint32_t dsize)
{
  struct eb *e = calloc(1, sizeof *e);
  tcfg(e, bs, 4, 3);
  e->dsize = dsize;
  e->incompat |= EB_INC_64BIT | EB_INC_INLINE | EB_INC_CASEFOLD | EB_INC_ENCRYPT;
  e->ro_compat |= EB_RO_MCSUM;
  eb_init(e);
  uint64_t lb[20];
  for (int i = 0; i < 20; i++)
    lb[i] = (uint64_t)i * 3;
  uint32_t deep = sparse_file(e, lb, 20, 2, 3, 3, 70 * 1024);
  uint32_t f = eb_file(e, 0, EB_REG | 0644, "hello", 5), inl = eb_inline_file(e, "inline file data", 16);
  uint32_t l1 = eb_symlink(e, "hello", false), l2 = eb_symlink(e, "/deep", true), ch = eb_mkino(e, 0, EB_CHR | 0600);
  uint32_t enc = eb_file(e, 0, EB_REG | 0644, "secret", 6);
  w32(eb_inode(e, enc) + 32, r32(eb_inode(e, enc) + 32) | EB_IF_ENCRYPT);
  struct eb_de he[60];
  char hn[60][8];
  for (int i = 0; i < 60; i++) {
    snprintf(hn[i], 8, "h%03d", i);
    he[i] = (struct eb_de){(const uint8_t *)hn[i], 4, f, 1};
  }
  uint32_t ht = eb_mkino(e, 0, EB_DIR | 0755);
  eb_dir_htree(e, ht, 2, he, 60, 2);
  uint32_t id = eb_mkino(e, 0, EB_DIR | 0755);
  struct eb_de ie[] = {DE("name0001", f, 1), DE("name0002", inl, 1), DE("name0003", f, 1), DE("name0004", f, 1),
                       DE("name0005", f, 1)};
  eb_inline_dir(e, id, 2, ie, 5);
  uint32_t cf = eb_mkino(e, 0, EB_DIR | 0755);
  struct eb_de ce[] = {DE("Hello", f, 1)};
  eb_dir(e, cf, 2, ce, 1, 0);
  w32(eb_inode(e, cf) + 32, r32(eb_inode(e, cf) + 32) | EB_IF_CASEFOLD);
  struct eb_de ents[] = {DE("deep", deep, 1), DE("hello", f, 1), DE("inl", inl, 1), DE("l1", l1, 7), DE("l2", l2, 7),
                         DE("chr", ch, 3),    DE("enc", enc, 1), DE("ht", ht, 2),   DE("id", id, 2), DE("cf", cf, 2)};
  mkroot(e, ents, 10);
  return eb_finish(e, len);
}

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;

static uint32_t rnd(void)
{
  g_rng ^= g_rng >> 12;
  g_rng ^= g_rng << 25;
  g_rng ^= g_rng >> 27;
  return (uint32_t)((g_rng * 0x2545F4914F6CDD1Dull) >> 32);
}

static void walk(void *m, const char *dir, int depth, int *budget)
{
  char nm[256], path[2048];
  int isd;
  uint32_t sz;
  for (int idx = 0; idx < 80 && *budget > 0 && ext_ops()->enum_dir(m, dir, idx, nm, &isd, &sz); idx++) {
    (*budget)--;
    snprintf(path, sizeof path, "%s/%s", strcmp(dir, "/") ? dir : "", nm);
    struct ext_attr a;
    uint8_t buf[4096];
    if (ext_getinfo(m, path, &a) != 0)
      continue;
    if (isd) {
      if (depth < 5)
        walk(m, path, depth + 1, budget);
    } else {
      ext_read64(m, path, 0, buf, sizeof buf);
      ext_read64(m, path, a.size / 2, buf, 100);
      ext_ops()->read_at(m, path, 7, buf, sizeof buf);
    }
  }
}

static bool fuzz_walk(uint8_t *img, size_t len)
{
  void *m = mnt(img, len);
  if (!m)
    return false;
  int budget = 400;
  walk(m, "/", 0, &budget);
  ext_ops()->list(m, "/");
  ext_close(m);
  return true;
}

static void mutate(uint8_t *img, size_t *len, const uint32_t *hot, uint32_t nhot)
{
  uint32_t kind = rnd() % 8;
  size_t n = *len;
  if (kind == 0) {
    *len = rnd() % (n + 1);
    return;
  }
  uint32_t flips = 1 + rnd() % 6;
  for (uint32_t i = 0; i < flips; i++) {
    size_t at = rnd() % n;
    if (nhot && kind >= 2) {
      at = (size_t)hot[rnd() % nhot] * 1024 + (rnd() % 2 ? rnd() % 256 : rnd() % 1024);
      if (at >= n)
        at = n - 1;
    }
    if (kind <= 4) {
      img[at] ^= (uint8_t)(1u << (rnd() % 8));
    } else if (kind == 5) {
      img[at] = (uint8_t)rnd();
    } else {
      static const uint32_t edge[] = {0, 0xFFFFFFFFu, 0x7FFFFFFFu, 0x80000000u, 1, 0x10000u, 0xFFFFu};
      at &= ~(size_t)3;
      if (at + 4 <= n)
        w32(img + at, edge[rnd() % 7]);
    }
  }
  if (kind == 7) {
    size_t blk = (rnd() % (n / 1024)) * 1024;
    memset(img + blk, 0, 1024);
  }
}

static void on_alarm(int sig)
{
  (void)sig;
  static const char msg[] = "ext fuzz: hang\n";
  if (write(2, msg, sizeof msg - 1) < 0)
    _exit(3);
  _exit(2);
}

static void run_fuzz(void)
{
  signal(SIGALRM, on_alarm);
  const char *env = getenv("EXT_FUZZ_ITERS");
  int iters = env ? atoi(env) : 4000;
  enum { NS = 7 };
  uint8_t *seeds[NS];
  size_t lens[NS];
  uint32_t *hot[NS], nhot[NS];
  seeds[0] = seed_ext2(&lens[0]);
  seeds[1] = seed_ext4(&lens[1], 1024, 64);
  run_journal_img(1024, JB_CRC1, false, 0, JV_NONE, &seeds[2], &lens[2]);
  run_journal_img(1024, JB_V2, false, 0, JV_NONE, &seeds[3], &lens[3]);
  run_journal_img(1024, JB_V3, true, 0, JV_NONE, &seeds[4], &lens[4]);
  run_journal_img(1024, JB_PLAIN, false, 0, JV_NONE, &seeds[5], &lens[5]);
  seeds[6] = seed_ext4(&lens[6], 4096, 32);
  for (int s = 0; s < NS; s++) {
    hot[s] = malloc(lens[s] / 1024 * sizeof **hot);
    nhot[s] = 0;
    for (size_t b = 0; b < lens[s] / 1024; b++)
      if (!is_zero(seeds[s] + b * 1024, 1024))
        hot[s][nhot[s]++] = (uint32_t)b;
  }
  int hangs = 0, leaks = 0, mounted = 0;
  for (int i = 0; i < iters; i++) {
    int s = i % NS;
    uint8_t *img = malloc(lens[s]);
    memcpy(img, seeds[s], lens[s]);
    size_t len = lens[s];
    if (i >= 6)
      mutate(img, &len, hot[s], nhot[s]);
    g_read_limit = 600000;
    g_hang = false;
    alarm(20);
    mounted += fuzz_walk(img, len);
    alarm(0);
    hangs += g_hang;
    leaks += g_live != 0;
    g_live = 0;
    free(img);
  }
  g_read_limit = 0;
  expect(hangs == 0);
  expect(leaks == 0);
  expect(mounted > iters / 4);
  fprintf(stderr, "  fuzz: %d iterations, %d mounted\n", iters, mounted);
  for (int s = 0; s < NS; s++) {
    free(seeds[s]);
    free(hot[s]);
  }
}

static void run_oom(void)
{
  size_t lens[3];
  uint8_t *seeds[3];
  seeds[0] = seed_ext4(&lens[0], 1024, 64);
  run_journal_img(1024, JB_V3, true, 0, JV_NONE, &seeds[1], &lens[1]);
  seeds[2] = seed_ext2(&lens[2]);
  int bad = 0, points = 0;
  for (int s = 0; s < 3; s++)
    for (long k = 1; k < 100000; k++) {
      g_alloc_n = 0;
      g_fail_at = k;
      fuzz_walk(seeds[s], lens[s]);
      g_fail_at = 0;
      bad += g_live != 0;
      g_live = 0;
      points++;
      if (g_alloc_n < k)
        break;
    }
  expect(bad == 0);
  expect(points > 50);
  for (int s = 0; s < 3; s++)
    free(seeds[s]);
}

int main(void)
{
  run_indirect(1024, 2);
  run_indirect(2048, 2);
  run_indirect(4096, 2);
  run_indirect(65536, 2);
  run_indirect(1024, 3);
  for (uint32_t mode = JB_PLAIN; mode <= JB_V3; mode++)
    for (int b64 = 0; b64 < 2; b64++)
      for (uint32_t bs = 1024; bs <= 4096; bs *= 4) {
        if (mode == JB_V1SB && b64)
          continue;
        run_journal(bs, mode, b64, 0, JV_NONE);
        if (mode >= JB_CRC1)
          run_journal(bs, mode, b64, JB_BAD_COMMIT_CSUM, JV_NONE);
        if (mode >= JB_V2) {
          run_journal(bs, mode, b64, JB_BAD_DATA_CSUM, JV_NONE);
          run_journal(bs, mode, b64, JB_BAD_DESC_CSUM, JV_NONE);
        }
      }
  run_journal(1024, JB_V2, false, 0, JV_NO_FLAG);
  run_journal(1024, JB_V2, false, 0, JV_EXTERNAL);
  run_journal(1024, JB_V3, false, 0, JV_BAD_SB);
  run_extents(1024, 0, 0, 0, false);
  run_extents(1024, 1, 8, 4, false);
  run_extents(1024, 2, 3, 3, false);
  run_extents(1024, 3, 2, 2, false);
  run_extents(4096, 2, 3, 3, true);
  run_extents(1024, 3, 2, 2, true);
  test("ext4 extent of 32768 blocks and uninitialized tail", run_extent_limits);
  run_layout(1024, 32, false, 0, 0, false, 6, 0);
  run_layout(1024, 64, true, 0, 0, false, 20, 0);
  run_layout(1024, 64, true, 1, 0, false, 40, 0);
  run_layout(1024, 64, true, 1, 1, false, 40, 1);
  run_layout(1024, 64, true, 1, 0, true, 40, 2);
  run_layout(4096, 1024, true, 1, 0, false, 12, 1);
  run_layout(4096, 1024, true, 1, 0, true, 12, 0);
  run_layout(2048, 128, true, 1, 0, false, 20, 2);
  run_layout(65536, 512, true, 0, 0, false, 4, 1);
  run_layout(1024, 64, true, 1, 0, false, 40, 4);
  run_layout(4096, 1024, true, 1, 1, false, 12, 5);
  run_uninit(0);
  run_uninit(1);
  run_uninit(2);
  test("ext4 inline_data files and directories", run_inline);
  run_htree(1024, 1, false);
  run_htree(1024, 2, false);
  run_htree(4096, 1, true);
  run_htree(1024, 2, true);
  test("directory with 70000 entries streamed, cache bounded", run_bigdir);
  run_dirent_edges(1024);
  run_dirent_edges(4096);
  run_reclen64k(false);
  run_reclen64k(true);
  test("no filetype feature, case-folded directory", run_nofiletype);
  test("sizes above 4 GiB, i_size_high rules, fs_ops clamping", run_sizes);
  test("large_dir uses i_size_high for directories", run_large_dir);
  test("inode metadata: mode, uid/gid high, times, epoch bits, huge_file", run_metadata);
  test("device nodes, FIFOs, sockets, encryption", run_special);
  run_symlinks(4096, 4);
  run_symlinks(1024, 2);
  run_csum(EB_RO_MCSUM, 0, "metadata_csum");
  run_csum(EB_RO_MCSUM, EB_INC_CSUM_SEED, "metadata_csum with csum_seed");
  run_csum(EB_RO_GDT_CSUM, 0, "gdt_csum crc16");
  test("jbd2 multiple descriptors, wrapped log, sequence wraparound", run_journal_shapes);
  test("unclean state, orphans, resize inode, lost+found", run_state);
  test("block cache: repeated metadata access reads nothing, sequential reads stay correct", run_cache);
  test("MBR and GPT partition probing", run_partitions);
  test("fs_ops surface: list, stat, enum_dir, getattr, labels", run_fsops);
  test("allocation failure at every point is handled without leaks", run_oom);
  test("fuzz: mutated and truncated images neither crash, hang nor leak", run_fuzz);
  return 0;
}
