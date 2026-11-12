/* btrfs_test.c: host test for kernel/btrfs.c (read-only btrfs driver), kernel/lzo.c and kernel/zstd.c.
 *
 * The driver is compiled into this file against an in-memory block device. Images come from tests/btrfs_build.h,
 * an independent builder written from the on-disk format documents (own crc32c, chunk mapping, tree packer and
 * compressors). Compressed extents use the host zlib, liblzo2 and libzstd when the build found them (run-tests.sh
 * passes HAVE_HOST_ZLIB, HAVE_HOST_ZSTD, HAVE_HOST_LZO); hand-built zlib stored, LZO and zstd raw/RLE streams are
 * always covered. Interoperability with real mkfs.btrfs images is not verified here.
 *
 *   make test    (or compile with -include tests/test.h -Dmain=owos_main_btrfs -DOWOS_TEST_NAME=btrfs)
 */
#define OWOS_SUITE_TEST
#define OWOS_CONSOLE_H
#define OWOS_HEAP_H
#define OWOS_KSTRING_H

#include <execinfo.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#include "btrfs_build.h"

static long g_live, g_allocs, g_fail_at = -1, g_fail_from = -1;
static char g_log[8192];
static char g_out[1 << 16];

static void *kmalloc(size_t n)
{
  long k = g_allocs++;
  if (k == g_fail_at || (g_fail_from >= 0 && k >= g_fail_from))
    return NULL;
  void *p = malloc(n);
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

static void con_printf(const char *fmt, ...)
{
  size_t l = strlen(g_log);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_log + l, sizeof g_log - l, fmt, ap);
  va_end(ap);
}

static void con_write(const char *s)
{
  size_t l = strlen(g_out);
  snprintf(g_out + l, sizeof g_out - l, "%s", s);
}

#include "../kernel/btrfs.c"
#include "../kernel/gzip.c"
#include "../kernel/lzo.c"
#include "../kernel/zstd.c"

/* ---------------------------------------------------------------- device */

static const uint8_t *g_img;
static size_t g_size;
static unsigned long g_nread;

static int dev_read(uint32_t lba, void *buf)
{
  if ((uint64_t)lba * 512 + 512 > g_size)
    return -1;
  g_nread++;
  memcpy(buf, g_img + (size_t)lba * 512, 512);
  return 0;
}

static void *mount_dev(const uint8_t *img, size_t size)
{
  g_img = img;
  g_size = size;
  g_log[0] = 0;
  return btrfs_ops()->probe(dev_read, NULL);
}

static void *mount_bb(const struct bb *b) { return mount_dev(b->img, b->size); }

static void unmount(void *m)
{
  btrfs_ops()->close(m);
  expect(g_live == 0);
  g_live = 0;
}

/* ---------------------------------------------------------------- data generators */

static uint32_t g_rng = 0x1234567u;

static uint32_t rnd(void)
{
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 17;
  g_rng ^= g_rng << 5;
  return g_rng;
}

static void fill_random(uint8_t *p, size_t n, uint32_t seed)
{
  uint32_t s = seed * 2654435761u + 1;
  for (size_t i = 0; i < n; i++) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    p[i] = (uint8_t)s;
  }
}

static void fill_text(uint8_t *p, size_t n, uint32_t seed)
{
  static const char *words[] = {"btrfs ", "extent ", "chunk ", "leaf ",  "node ",  "subvolume ",  "snapshot ",
                                "the ",   "of ",     "and ",   "inode ", "data\n", "0123456789 ", "zzzzzzzzzzzzzzzz "};
  uint32_t s = seed * 2654435761u + 7;
  size_t o = 0;
  while (o < n) {
    s = s * 1664525u + 1013904223u;
    const char *w = words[(s >> 16) % 14];
    for (; *w && o < n; w++)
      p[o++] = (uint8_t)*w;
  }
}

/* ---------------------------------------------------------------- expectation table */

enum { EX_FILE, EX_DIR, EX_BROKEN, EX_SPECIAL, EX_DIRLINK };

struct ex {
  char path[300];
  int kind;
  uint8_t *data;
  size_t len;
  uint32_t mode, uid, gid;
};

static struct ex *g_ex;
static size_t g_nex, g_cex;

static struct ex *reg(const char *path, int kind, const void *data, size_t len, uint32_t mode, uint32_t uid,
                      uint32_t gid)
{
  if (g_nex == g_cex) {
    g_cex = g_cex ? g_cex * 2 : 64;
    g_ex = realloc(g_ex, g_cex * sizeof *g_ex);
  }
  struct ex *e = &g_ex[g_nex++];
  snprintf(e->path, sizeof e->path, "%s", path);
  e->kind = kind;
  e->data = NULL;
  if (data) {
    e->data = malloc(len ? len : 1);
    memcpy(e->data, data, len);
  }
  e->len = len;
  e->mode = mode;
  e->uid = uid;
  e->gid = gid;
  return e;
}

static struct ex *find_ex(const char *path)
{
  for (size_t i = 0; i < g_nex; i++)
    if (strcmp(g_ex[i].path, path) == 0)
      return &g_ex[i];
  return NULL;
}

static void clear_ex(void)
{
  for (size_t i = 0; i < g_nex; i++)
    free(g_ex[i].data);
  g_nex = 0;
}

/* Register the same content under another name (a symlink that resolves to the target). */
static void reg_alias(const char *path, const char *target)
{
  struct ex *t = find_ex(target);
  reg(path, t->kind == EX_DIR ? EX_DIRLINK : t->kind, t->data, t->len, t->mode, t->uid, t->gid);
}

static const char *join(char *buf, size_t cap, const char *dir, const char *name)
{
  snprintf(buf, cap, "%s%s%s", dir, dir[0] ? "/" : "", name);
  return buf;
}

/* ---------------------------------------------------------------- main image */

struct cfg {
  uint32_t nodesize, sectorsize, leaf_max, ptr_max;
  struct bb_prof meta, data;
  bool has_meta, has_data;
  uint64_t incompat;
  uint32_t default_id;
};

static uint32_t g_unit; /* extent granule: max(sectorsize, 4096) */

static struct bb_tree *new_tree(uint32_t id)
{
  struct bb_tree *t = malloc(sizeof *t);
  bb_tree_init(t, id);
  return t;
}

static void del_tree(struct bb_tree *t)
{
  bb_free_tree(t);
  free(t);
}

/* A regular file with one uncompressed extent per chunk of ext bytes, data at logical addresses of the data chunk. */
static uint64_t add_reg(struct bb *b, struct bb_tree *t, uint64_t dir, const char *name, const uint8_t *data,
                        size_t len, size_t ext, uint32_t mode, uint32_t uid, uint32_t gid)
{
  uint64_t ino = bb_mkfile(t, dir, name, 0100000 | mode, uid, gid, len, 1);
  for (size_t off = 0; off < len; off += ext) {
    size_t n = len - off > ext ? ext : len - off;
    size_t padded = (n + b->sectorsize - 1) / b->sectorsize * b->sectorsize;
    uint64_t at = bb_data(b, data + off, n);
    bb_fe_reg(t, ino, off, 0, at, padded, 0, padded, padded, 1);
  }
  return ino;
}

static void add_inline(struct bb_tree *t, uint64_t dir, const char *name, const void *data, size_t len, uint32_t mode,
                       uint32_t uid, uint32_t gid)
{
  uint64_t ino = bb_mkfile(t, dir, name, 0100000 | mode, uid, gid, len, 1);
  bb_fe_inline(t, ino, 0, len, data, (uint32_t)len);
}

static void add_symlink(struct bb_tree *t, uint64_t dir, const char *name, const char *target)
{
  uint64_t ino = bb_mkfile(t, dir, name, 0120777, 0, 0, strlen(target), 7);
  bb_fe_inline(t, ino, 0, strlen(target), target, (uint32_t)strlen(target));
}

static size_t compress_ext(uint8_t comp, int variant, const uint8_t *src, size_t n, uint8_t *dst, uint32_t ss)
{
  if (comp == 1) {
#ifdef BB_HAVE_ZLIB
    if (variant == 0) {
      unsigned long out = 2 * n + 4096;
      return compress2(dst, &out, src, (unsigned long)n, 6) == 0 ? out : 0;
    }
#else
    if (variant == 0)
      return 0;
#endif
    return bb_zlib_stored(src, n, dst);
  }
  if (comp == 2) {
    if (variant == 0)
      return bb_lzo_extent(src, n, ss, dst, 0);
#ifdef BB_HAVE_LZO
    return bb_lzo_extent(src, n, ss, dst, 1);
#else
    return 0;
#endif
  }
  if (variant == 0)
    return bb_zstd_hand(src, n, dst);
#ifdef BB_HAVE_ZSTD
  size_t r = ZSTD_compress(dst, 2 * n + 4096, src, n, variant == 1 ? 3 : 19);
  return ZSTD_isError(r) ? 0 : r;
#else
  return 0;
#endif
}

/* A compressed file: one extent per ram granule; returns false when the variant is unavailable on this host. */
static bool add_comp(struct bb *b, struct bb_tree *t, uint64_t dir, const char *name, uint8_t comp, int variant,
                     const uint8_t *data, size_t len, const char *dirpath)
{
  size_t ram = (98304 / g_unit) * g_unit;
  uint8_t *tmp = malloc(2 * ram + 8192), *pad = calloc(1, ram);
  if (compress_ext(comp, variant, pad, 1, tmp, b->sectorsize) == 0) {
    free(tmp);
    free(pad);
    return false;
  }
  uint64_t ino = bb_mkfile(t, dir, name, 0100644, 0, 0, len, 1);
  for (size_t off = 0; off < len; off += ram) {
    size_t n = len - off > ram ? ram : len - off;
    size_t rb = (n + b->sectorsize - 1) / b->sectorsize * b->sectorsize;
    memset(pad, 0, ram);
    memcpy(pad, data + off, n);
    size_t cl = compress_ext(comp, variant, pad, rb, tmp, b->sectorsize);
    uint64_t at = bb_data(b, tmp, cl);
    size_t padded = (cl + b->sectorsize - 1) / b->sectorsize * b->sectorsize;
    bb_fe_reg(t, ino, off, comp, at, padded, 0, rb, rb, 1);
  }
  char path[300];
  reg(join(path, sizeof path, dirpath, name), EX_FILE, data, len, 0644, 0, 0);
  free(tmp);
  free(pad);
  return true;
}

static void add_inline_comp(struct bb *b, struct bb_tree *t, uint64_t dir, const char *name, uint8_t comp, int variant,
                            const uint8_t *data, size_t len, const char *dirpath)
{
  uint8_t *tmp = malloc(2 * len + 8192);
  size_t cl = compress_ext(comp, variant, data, len, tmp, b->sectorsize);
  if (cl) {
    uint64_t ino = bb_mkfile(t, dir, name, 0100644, 0, 0, len, 1);
    bb_fe_inline(t, ino, comp, len, tmp, (uint32_t)cl);
    char path[300];
    reg(join(path, sizeof path, dirpath, name), EX_FILE, data, len, 0644, 0, 0);
  }
  free(tmp);
}

/* Two names with the same btrfs name hash, found by birthday search (cached after the first call). */
static void find_collision(char *a, char *b)
{
  static char ca[32], cb[32];
  if (!ca[0]) {
    const uint32_t mask = (1u << 22) - 1;
    uint32_t *tab = calloc(mask + 1, sizeof *tab), *hs = malloc(sizeof *hs * (1u << 21));
    for (uint32_t i = 0; i < (1u << 21); i++) {
      char name[32];
      snprintf(name, sizeof name, "c%u", i);
      uint32_t h = bb_name_hash(name, strlen(name));
      hs[i] = h;
      uint32_t slot = h & mask;
      while (tab[slot] && hs[tab[slot] - 1] != h)
        slot = (slot + 1) & mask;
      if (tab[slot]) {
        snprintf(ca, sizeof ca, "c%u", tab[slot] - 1);
        snprintf(cb, sizeof cb, "%s", name);
        break;
      }
      tab[slot] = i + 1;
    }
    free(tab);
    free(hs);
  }
  strcpy(a, ca);
  strcpy(b, cb);
}

static int g_many = 700;

/* Build the reference file system and the expectation table. */
static void build_main(struct bb *b, const struct cfg *c)
{
  g_unit = c->sectorsize > 4096 ? c->sectorsize : 4096;
  bb_init(b, c->nodesize, c->sectorsize, c->has_meta ? &c->meta : NULL, c->has_data ? &c->data : NULL, 12u << 20,
          24u << 20);
  b->leaf_max = c->leaf_max;
  b->ptr_max = c->ptr_max;
  if (c->incompat)
    b->incompat = c->incompat;
  clear_ex();

  struct bb_tree *t = new_tree(5), *sub = new_tree(256), *snap = new_tree(257);
  bb_root_dir(t);
  bb_root_dir(sub);
  bb_root_dir(snap);
  reg("", EX_DIR, NULL, 0, 0755, 0, 0);
  char path[300];
  uint32_t U = g_unit;

  add_inline(t, 256, "hello.txt", "hello btrfs\n", 12, 0644, 1000, 100);
  reg("hello.txt", EX_FILE, "hello btrfs\n", 12, 0644, 1000, 100);

  uint64_t docs = bb_mkdir(t, 256, "docs", 0700, 1, 2);
  reg("docs", EX_DIR, NULL, 0, 0700, 1, 2);
  size_t rl = 3 * (size_t)U + 100;
  uint8_t *buf = malloc(8u << 20);
  fill_random(buf, rl, 11);
  add_reg(b, t, docs, "readme", buf, rl, 4 * (size_t)U, 0644, 1, 2);
  reg("docs/readme", EX_FILE, buf, rl, 0644, 1, 2);
  uint64_t nested = bb_mkdir(t, docs, "nested", 0755, 1, 2);
  reg("docs/nested", EX_DIR, NULL, 0, 0755, 1, 2);
  fill_random(buf, 5000, 12);
  add_reg(b, t, nested, "deep.bin", buf, 5000, U, 0600, 3, 4);
  reg("docs/nested/deep.bin", EX_FILE, buf, 5000, 0600, 3, 4);

  size_t bigsz = 5 * 131072 + 4321;
  fill_random(buf, bigsz, 13);
  add_reg(b, t, 256, "big.bin", buf, bigsz, 131072, 0644, 0, 0);
  reg("big.bin", EX_FILE, buf, bigsz, 0644, 0, 0);

  /* sparse: data, gap, prealloc, explicit hole, shared extent at offsets, tail gap */
  {
    size_t ssz = 40 * (size_t)U;
    uint8_t *want = calloc(1, ssz), *d4 = malloc(4 * (size_t)U), *d1 = malloc(U);
    fill_random(d1, U, 21);
    fill_random(d4, 4 * (size_t)U, 22);
    uint64_t ino = bb_mkfile(t, 256, "sparse", 0100644, 0, 0, ssz, 1);
    uint64_t a1 = bb_data(b, d1, U), a4 = bb_data(b, d4, 4 * (size_t)U);
    bb_fe_reg(t, ino, 0, 0, a1, U, 0, U, U, 1);
    memcpy(want, d1, U);
    bb_fe_reg(t, ino, 4 * (uint64_t)U, 0, 0x777000, 2 * (uint64_t)U, 0, 2 * (uint64_t)U, 2 * (uint64_t)U, 2);
    bb_fe_reg(t, ino, 6 * (uint64_t)U, 0, 0, 0, 0, 2 * (uint64_t)U, 2 * (uint64_t)U, 1);
    bb_fe_reg(t, ino, 8 * (uint64_t)U, 0, a4, 4 * (uint64_t)U, U, 2 * (uint64_t)U, 4 * (uint64_t)U, 1);
    memcpy(want + 8 * (size_t)U, d4 + U, 2 * (size_t)U);
    bb_fe_reg(t, ino, 10 * (uint64_t)U, 0, a4, 4 * (uint64_t)U, 0, U, 4 * (uint64_t)U, 1);
    memcpy(want + 10 * (size_t)U, d4, U);
    reg("sparse", EX_FILE, want, ssz, 0644, 0, 0);
    free(want);
    free(d4);
    free(d1);
  }

  /* compressed files */
  size_t csz = 2 * ((98304 / U) * U) + 7000;
  uint8_t *text = malloc(csz), *rnd8 = malloc(csz), *mix = malloc(csz);
  fill_text(text, csz, 31);
  fill_random(rnd8, csz, 32);
  for (size_t i = 0; i < csz; i++)
    mix[i] = (i / 997) & 1 ? rnd8[i] : text[i];
  uint64_t comp_dir = bb_mkdir(t, 256, "comp", 0755, 0, 0);
  reg("comp", EX_DIR, NULL, 0, 0755, 0, 0);
  static const char *const cname[] = {"", "zlib", "lzo", "zstd"};
  for (uint8_t comp = 1; comp <= 3; comp++) {
    for (int variant = 0; variant < 3; variant++) {
      for (int kind = 0; kind < 3; kind++) {
        char name[64];
        const uint8_t *src = kind == 0 ? text : kind == 1 ? rnd8 : mix;
        snprintf(name, sizeof name, "%s_v%d_%s.dat", cname[comp], variant,
                 kind == 0   ? "text"
                 : kind == 1 ? "rand"
                             : "mix");
        add_comp(b, t, comp_dir, name, comp, variant, src, csz, "comp");
      }
      char name[64];
      snprintf(name, sizeof name, "%s_v%d_inline", cname[comp], variant);
      add_inline_comp(b, t, comp_dir, name, comp, variant, text, 1500, "comp");
    }
  }
  /* a compressed extent read through a nonzero offset into it (reflink of a sub-range) */
  {
    size_t ram = (98304 / U) * U;
    uint8_t *pad = calloc(1, ram), *out = malloc(2 * ram + 8192);
    memcpy(pad, text, ram);
    size_t cl = compress_ext(2, 0, pad, ram, out, c->sectorsize);
    uint64_t at = bb_data(b, out, cl);
    size_t padded = (cl + c->sectorsize - 1) / c->sectorsize * c->sectorsize;
    size_t eoff = ram >= 3 * (size_t)U ? 3 * (size_t)U : 0, plen = ram >= 3 * (size_t)U ? 2 * (size_t)U : U;
    uint64_t ino = bb_mkfile(t, comp_dir, "lzo_part.dat", 0100644, 0, 0, plen, 1);
    bb_fe_reg(t, ino, 0, 2, at, padded, eoff, plen, ram, 1);
    reg("comp/lzo_part.dat", EX_FILE, text + eoff, plen, 0644, 0, 0);
    free(pad);
    free(out);
  }
  free(text);
  free(rnd8);
  free(mix);

  /* symlinks */
  add_symlink(t, 256, "link", "hello.txt");
  reg_alias("link", "hello.txt");
  add_symlink(t, 256, "abs", "/docs/readme");
  reg_alias("abs", "docs/readme");
  add_symlink(t, 256, "dirlink", "docs");
  reg_alias("dirlink", "docs");
  add_symlink(t, 256, "up", "../../hello.txt");
  reg_alias("up", "hello.txt");
  add_symlink(t, 256, "viadir", "docs/../hello.txt");
  reg_alias("viadir", "hello.txt");
  add_symlink(t, docs, "rel", "nested/deep.bin");
  reg_alias("docs/rel", "docs/nested/deep.bin");
  add_symlink(t, 256, "loop1", "loop2");
  add_symlink(t, 256, "loop2", "loop1");
  add_symlink(t, 256, "dangling", "nothere");
  reg("loop1", EX_BROKEN, NULL, 5, 0, 0, 0);
  reg("loop2", EX_BROKEN, NULL, 5, 0, 0, 0);
  reg("dangling", EX_BROKEN, NULL, 7, 0, 0, 0);
  for (int i = 1; i <= 41; i++) {
    char name[16], target[16];
    snprintf(name, sizeof name, "c%d", i);
    snprintf(target, sizeof target, i == 41 ? "hello.txt" : "c%d", i + 1);
    add_symlink(t, 256, name, target);
    if (i >= 2)
      reg_alias(name, "hello.txt");
    else
      reg(name, EX_BROKEN, NULL, 2, 0, 0, 0);
  }

  /* subvolumes */
  bb_entry_ex(t, 256, "sub", 256, 132, 2);
  reg("sub", EX_DIR, NULL, 0, 0755, 0, 0);
  add_inline(sub, 256, "inside.txt", "inside the subvolume\n", 21, 0644, 0, 0);
  reg("sub/inside.txt", EX_FILE, "inside the subvolume\n", 21, 0644, 0, 0);
  uint64_t subd = bb_mkdir(sub, 256, "d", 0755, 0, 0);
  (void)subd;
  reg("sub/d", EX_DIR, NULL, 0, 0755, 0, 0);
  add_symlink(sub, 256, "out", "../hello.txt");
  reg_alias("sub/out", "hello.txt");
  bb_entry_ex(t, 256, "snap", 257, 132, 2);
  reg("snap", EX_DIR, NULL, 0, 0755, 0, 0);
  add_inline(snap, 256, "hello.txt", "snapshot hello\n", 15, 0644, 0, 0);
  reg("snap/hello.txt", EX_FILE, "snapshot hello\n", 15, 0644, 0, 0);

  /* directories with many entries, hash collisions, odd names */
  uint64_t many = bb_mkdir(t, 256, "many", 0755, 0, 0);
  reg("many", EX_DIR, NULL, 0, 0755, 0, 0);
  for (int i = 0; i < g_many; i++) {
    char name[32], content[32];
    snprintf(name, sizeof name, "f%04d", i);
    snprintf(content, sizeof content, "n%d", i);
    add_inline(t, many, name, content, strlen(content), 0644, 0, 0);
    reg(join(path, sizeof path, "many", name), EX_FILE, content, strlen(content), 0644, 0, 0);
  }
  uint64_t coll = bb_mkdir(t, 256, "coll", 0755, 0, 0);
  reg("coll", EX_DIR, NULL, 0, 0755, 0, 0);
  char ca[32], cb[32];
  find_collision(ca, cb);
  add_inline(t, coll, ca, "first", 5, 0644, 0, 0);
  add_inline(t, coll, cb, "second", 6, 0644, 0, 0);
  reg(join(path, sizeof path, "coll", ca), EX_FILE, "first", 5, 0644, 0, 0);
  reg(join(path, sizeof path, "coll", cb), EX_FILE, "second", 6, 0644, 0, 0);

  bb_mkdir(t, 256, "empty", 0755, 0, 0);
  reg("empty", EX_DIR, NULL, 0, 0755, 0, 0);
  bb_mkdir(t, 256, "sticky", 01777, 0, 0);
  reg("sticky", EX_DIR, NULL, 0, 01777, 0, 0);
  add_inline(t, 256, "setuid", "x", 1, 04755, 0, 0);
  reg("setuid", EX_FILE, "x", 1, 0755, 0, 0);
  bb_mkfile(t, 256, "pipe", 010644, 0, 0, 0, 5);
  reg("pipe", EX_SPECIAL, NULL, 0, 0644, 0, 0);

  char longname[256];
  memset(longname, 'n', 255);
  longname[255] = 0;
  add_inline(t, 256, longname, "long", 4, 0644, 0, 0);
  reg(longname, EX_FILE, "long", 4, 0644, 0, 0);
  add_inline(t, 256, "h\xc3\xa9llo world.txt", "utf8", 4, 0644, 0, 0);
  reg("h\xc3\xa9llo world.txt", EX_FILE, "utf8", 4, 0644, 0, 0);

  struct bb_tree *extra = new_tree(0);
  uint8_t br[32];
  for (int i = 0; i < 2; i++) {
    const char *nm = i ? "snap" : "sub";
    bb_p64(br, 256);
    bb_p64(br + 8, 1);
    bb_p16(br + 16, (uint16_t)strlen(nm));
    memcpy(br + 18, nm, strlen(nm));
    bb_add(extra, 256 + i, 144, 5, br, (uint32_t)(18 + strlen(nm)));
  }
  struct bb_tree *fs[3] = {t, sub, snap};
  bb_commit(b, fs, 3, c->default_id ? c->default_id : 5, extra);
  del_tree(t);
  del_tree(sub);
  del_tree(snap);
  del_tree(extra);
  free(buf);
}

/* ---------------------------------------------------------------- verification */

static const struct fs_ops *ops(void) { return btrfs_ops(); }

static int rd(void *m, const char *path, uint32_t off, void *buf, uint32_t len)
{
  return ops()->read_at(m, path, off, buf, len);
}

static void chk(bool ok, const char *what, const char *path)
{
  if (!ok)
    printf("  FAIL %s: '%s'\n", what, path);
  expect(ok);
}

static uint32_t clamp32(size_t v) { return v > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)v; }

static void verify_file(void *m, const struct ex *e)
{
  int is_dir = -1;
  uint32_t size = 0, mode, uid, gid;
  chk(ops()->stat(m, e->path, &is_dir, &size) == 0 && is_dir == 0 && size == clamp32(e->len), "stat file", e->path);
  chk(ops()->getattr(m, e->path, &mode, &uid, &gid) == 0 && mode == e->mode && uid == e->uid && gid == e->gid,
      "getattr", e->path);
  uint8_t *got = malloc(e->len + 64);
  size_t pos = 0;
  bool ok = true;
  while (pos < e->len) {
    uint32_t chunk = 1 + rnd() % 9000;
    int r = rd(m, e->path, (uint32_t)pos, got + pos, chunk);
    size_t want = e->len - pos < chunk ? e->len - pos : chunk;
    if (r != (int)want) {
      ok = false;
      break;
    }
    pos += (size_t)r;
  }
  chk(ok && memcmp(got, e->data, e->len) == 0, "sequential read", e->path);
  chk(rd(m, e->path, (uint32_t)e->len, got, 10) == 0, "read at EOF", e->path);
  ok = true;
  for (int i = 0; i < 24 && e->len; i++) {
    uint32_t off = rnd() % (uint32_t)(e->len + 100), len = 1 + rnd() % 70000;
    int r = rd(m, e->path, off, got, len < 60 ? len : len % 64 + 1);
    uint32_t want = off >= e->len ? 0 : (uint32_t)(e->len - off);
    uint32_t asked = len < 60 ? len : len % 64 + 1;
    if (want > asked)
      want = asked;
    if (r != (int)want || (want && memcmp(got, e->data + off, want) != 0))
      ok = false;
  }
  chk(ok, "random reads", e->path);
  free(got);
}

static const char *parent_of(const char *path, char *buf, size_t cap)
{
  const char *slash = strrchr(path, '/');
  size_t n = slash ? (size_t)(slash - path) : 0;
  snprintf(buf, cap, "%.*s", (int)n, path);
  return buf;
}

static void verify_dir(void *m, const struct ex *d)
{
  int is_dir = 0;
  uint32_t size = 1;
  chk(ops()->stat(m, d->path, &is_dir, &size) == 0 && is_dir == 1 && size == 0, "stat dir", d->path);
  uint32_t mode, uid, gid;
  chk(ops()->getattr(m, d->path, &mode, &uid, &gid) == 0 && mode == d->mode && uid == d->uid && gid == d->gid,
      "getattr dir", d->path);

  int idx = 0;
  bool order_ok = true;
  for (size_t i = 0; i < g_nex; i++) {
    char par[300];
    const struct ex *c = &g_ex[i];
    if (c == d || strcmp(parent_of(c->path, par, sizeof par), d->path) != 0 || (c->path[0] == 0))
      continue;
    char name[300];
    int cdir = -1;
    uint32_t csize = 0;
    if (ops()->enum_dir(m, d->path, idx, name, &cdir, &csize) != 1) {
      order_ok = false;
      break;
    }
    const char *path_base = strrchr(c->path, '/') ? strrchr(c->path, '/') + 1 : c->path;
    char base[FS_ENUM_NAME_MAX];
    snprintf(base, sizeof base, "%s", path_base); /* long names are listed cut to the enum_dir buffer */
    bool want_dir = c->kind == EX_DIR || c->kind == EX_DIRLINK;
    uint32_t want_size = want_dir ? 0 : c->kind == EX_FILE ? clamp32(c->len) : clamp32(c->len);
    if (strcmp(name, base) != 0 || (cdir != 0) != want_dir || csize != want_size) {
      printf("  entry %d of '%s': got '%s' dir=%d size=%u, want '%s' dir=%d size=%u\n", idx, d->path, name, cdir, csize,
             base, want_dir, want_size);
      order_ok = false;
      break;
    }
    idx++;
  }
  char name[300];
  chk(order_ok && ops()->enum_dir(m, d->path, idx, name, NULL, NULL) == 0, "directory order", d->path);
  chk(ops()->enum_dir(m, d->path, -1, name, NULL, NULL) == 0, "negative index", d->path);
}

static void verify_all(void *m)
{
  expect(strcmp(ops()->type(m), "btrfs") == 0);
  expect(strcmp(ops()->label(m), "bbtest") == 0);
  for (size_t i = 0; i < g_nex; i++) {
    const struct ex *e = &g_ex[i];
    int is_dir;
    uint32_t size;
    if (e->kind == EX_FILE) {
      verify_file(m, e);
    } else if (e->kind == EX_DIR) {
      verify_dir(m, e);
    } else if (e->kind == EX_DIRLINK) {
      char nm[300];
      chk(ops()->stat(m, e->path, &is_dir, &size) == 0 && is_dir == 1 &&
              ops()->enum_dir(m, e->path, 0, nm, NULL, NULL) == 1,
          "directory symlink", e->path);
    } else if (e->kind == EX_BROKEN) {
      chk(ops()->stat(m, e->path, &is_dir, &size) != 0, "broken stat fails", e->path);
      chk(rd(m, e->path, 0, &size, 4) < 0, "broken read fails", e->path);
    } else {
      uint8_t b[4];
      chk(ops()->stat(m, e->path, &is_dir, &size) == 0 && !is_dir && size == 0, "special stat", e->path);
      chk(rd(m, e->path, 0, b, 4) < 0, "special read fails", e->path);
    }
  }
}

static void verify_negative(void *m)
{
  int is_dir;
  uint32_t size;
  static const char *const bad[] = {"nope",       "hello.txt/x", "hello.txt/..", "docs/nope/x", "docs/readme/.",
                                    "dangling/x", "loop1/x",     "sub/nope",     "snap/docs"};
  for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
    chk(ops()->stat(m, bad[i], &is_dir, &size) != 0, "negative stat", bad[i]);
  static const char *const good[] = {"/",
                                     "//",
                                     ".",
                                     "docs/..",
                                     "docs/./nested/../..",
                                     "sub/..",
                                     "sub/d/../..",
                                     "..",
                                     "../..",
                                     "/docs/nested/../readme",
                                     "snap/../hello.txt"};
  for (size_t i = 0; i < sizeof good / sizeof *good; i++)
    chk(ops()->stat(m, good[i], &is_dir, &size) == 0, "dot paths", good[i]);
  chk(ops()->stat(m, "docs/readme", &is_dir, &size) == 0 && ops()->stat(m, "//docs//readme//", &is_dir, &size) == 0,
      "slashes", "docs//readme");
  chk(ops()->stat(m, "sub/..", &is_dir, &size) == 0 && is_dir, "sub/.. is the root", "sub/..");
  uint8_t a[16], b2[16];
  chk(rd(m, "snap/../hello.txt", 0, a, 12) == 12 && rd(m, "hello.txt", 0, b2, 12) == 12 && memcmp(a, b2, 12) == 0,
      "snapshot dotdot content", "snap/../hello.txt");
  char name[300];
  chk(ops()->enum_dir(m, "hello.txt", 0, name, NULL, NULL) == 0, "enum on a file", "hello.txt");
  chk(ops()->list(m, "hello.txt") != 0 && ops()->list(m, "nope") != 0, "list on non-dirs", "hello.txt");
  uint8_t tiny[4];
  chk(rd(m, "docs", 0, tiny, 4) < 0, "read of a directory", "docs");
  chk(ops()->create == NULL && ops()->del == NULL && ops()->mkdir == NULL && ops()->rmdir == NULL, "read-only", "");

  g_out[0] = 0;
  g_log[0] = 0;
  chk(ops()->list(m, "docs") == 0 && strstr(g_out, "<DIR>") && strstr(g_log, "readme") && strstr(g_log, "nested"),
      "list prints", "docs");
  struct btrfs_attr at;
  chk(btrfs_getinfo(m, "hello.txt", &at) == 0 && at.size == 12 && at.nlink == 1 && at.mode == 0100644 &&
          at.uid == 1000 && at.gid == 100 && at.atime == 1700000001 && at.atime_ns == 111 && at.ctime == 1700000002 &&
          at.ctime_ns == 222 && at.mtime == 1700000003 && at.mtime_ns == 333 && at.otime == 1700000004 &&
          at.otime_ns == 444,
      "getinfo times", "hello.txt");
}

/* ---------------------------------------------------------------- variants */

static struct bb_prof prof(uint64_t profile, int ns, int sub, uint64_t stripe_len, uint64_t d0, uint64_t d1,
                           uint64_t d2, uint64_t d3)
{
  struct bb_prof p = {profile, ns, sub, stripe_len, {d0, d1, d2, d3}};
  return p;
}

static struct cfg vc;

static void run_variant(void)
{
  struct bb b;
  build_main(&b, &vc);
  void *m = mount_bb(&b);
  expect(m != NULL);
  if (m) {
    verify_all(m);
    verify_negative(m);
    unmount(m);
  } else {
    printf("  mount failed: %s\n", g_log);
  }
  bb_free(&b);
}

/* A stored deflate block carries LEN and its complement NLEN; a mismatch is the only sign of corruption. */
static void zlib_stored_nlen(void)
{
  static const uint8_t text[] = "stored block payload";
  uint8_t z[64], out[64];
  int n = zlib_store(text, sizeof text, z, sizeof z);
  expect(n > 0 && zlib_inflate(z, (uint32_t)n, out, sizeof out) == (int)sizeof text);
  z[5] ^= 0x01; /* NLEN low byte, after the 2-byte header, block type and LEN */
  expect(zlib_inflate(z, (uint32_t)n, out, sizeof out) < 0);
}

static void t_variants(void)
{
  test("zlib stored block rejects a bad NLEN", zlib_stored_nlen);
  vc = (struct cfg){.nodesize = 4096, .sectorsize = 4096};
  test("btrfs: 4K nodes, natural packing", run_variant);

  vc = (struct cfg){.nodesize = 4096, .sectorsize = 4096, .leaf_max = 8, .ptr_max = 4};
  test("btrfs: 4K nodes, forced height 3+", run_variant);

  vc = (struct cfg){.nodesize = 16384, .sectorsize = 4096, .leaf_max = 24, .ptr_max = 5};
  test("btrfs: 16K nodes, forced height", run_variant);

  vc = (struct cfg){.nodesize = 65536, .sectorsize = 4096};
  test("btrfs: 64K nodes", run_variant);

  vc = (struct cfg){.nodesize = 65536, .sectorsize = 65536, .leaf_max = 40, .ptr_max = 7};
  test("btrfs: 64K sectors, 64K nodes", run_variant);

  vc = (struct cfg){.nodesize = 4096, .sectorsize = 512, .leaf_max = 12, .ptr_max = 5};
  test("btrfs: 512 byte sectors", run_variant);

  vc = (struct cfg){.nodesize = 16384,
                    .sectorsize = 4096,
                    .leaf_max = 16,
                    .ptr_max = 6,
                    .incompat = 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x40 | 0x80 | 0x100 | 0x200 | 0x400 | 0x800};
  test("btrfs: every accepted incompat bit incl. METADATA_UUID", run_variant);

  vc = (struct cfg){.nodesize = 4096, .sectorsize = 4096, .leaf_max = 10, .ptr_max = 4};
  vc.meta = prof(BB_DUP, 2, 0, 0, 1, 1, 0, 0);
  vc.data = prof(0, 1, 0, 0, 1, 0, 0, 0);
  vc.has_meta = vc.has_data = true;
  test("btrfs: DUP metadata", run_variant);

  vc.meta = prof(BB_RAID1, 2, 0, 0, 2, 1, 0, 0);
  vc.data = prof(BB_RAID1, 2, 0, 0, 1, 2, 0, 0);
  test("btrfs: RAID1 with the other mirror on a missing device", run_variant);

  vc.meta = prof(BB_RAID1C3, 3, 0, 0, 2, 3, 1, 0);
  vc.data = prof(BB_RAID1C4, 4, 0, 0, 2, 3, 4, 1);
  test("btrfs: RAID1C3 and RAID1C4", run_variant);

  vc.meta = prof(BB_RAID0, 2, 0, 65536, 1, 1, 0, 0);
  vc.data = prof(BB_RAID0, 3, 0, 65536, 1, 1, 1, 0);
  vc.leaf_max = 0;
  vc.ptr_max = 0;
  vc.nodesize = 16384;
  test("btrfs: RAID0 striping on one device", run_variant);

  vc.nodesize = 4096;
  vc.meta = prof(BB_RAID10, 4, 2, 65536, 2, 1, 1, 2);
  vc.data = prof(BB_RAID10, 4, 2, 65536, 1, 2, 2, 1);
  test("btrfs: RAID10 with one device per mirror pair", run_variant);

  vc.meta = prof(BB_RAID10, 4, 2, 4096, 1, 1, 1, 1);
  vc.data = prof(BB_RAID10, 6, 2, 8192, 1, 1, 1, 1);
  vc.data.devids[4] = vc.data.devids[5] = 1;
  test("btrfs: RAID10 striping with small stripes", run_variant);
}

/* ---------------------------------------------------------------- superblock, features, corruption */

static const struct cfg kPlain = {.nodesize = 4096, .sectorsize = 4096, .leaf_max = 16, .ptr_max = 5};

static void fix_sb(uint8_t *sb) { bb_p32(sb, ~bb_crc32c(0xFFFFFFFFu, sb + 32, 4096 - 32)); }

static unsigned count_log(const char *needle)
{
  unsigned n = 0;
  for (const char *p = g_log; (p = strstr(p, needle)); p++)
    n++;
  return n;
}

static uint64_t g_hi_off;
static uint8_t g_hi_buf[4096];

static int dev_read_hi(uint32_t lba, void *buf)
{
  uint64_t off = (uint64_t)lba * 512;
  if (off >= g_hi_off && off < g_hi_off + 4096) {
    memcpy(buf, g_hi_buf + (off - g_hi_off), 512);
    return 0;
  }
  return dev_read(lba, buf);
}

static void superblock_selection(void)
{
  struct bb b;
  build_main(&b, &kPlain);
  uint8_t *sb = b.img + 0x10000;
  void *m = mount_bb(&b);
  expect(m && strcmp(ops()->label(m), "bbtest") == 0);
  if (m)
    unmount(m);

  sb[500] ^= 1;
  expect(mount_bb(&b) == NULL && strstr(g_log, "no valid superblock"));
  sb[500] ^= 1;

  bb_write_sb(&b, 64ull << 20, 7, "mirror64");
  sb[500] ^= 1;
  m = mount_bb(&b);
  expect(m && strcmp(ops()->label(m), "mirror64") == 0);
  if (m)
    unmount(m);
  sb[500] ^= 1;
  m = mount_bb(&b);
  expect(m && strcmp(ops()->label(m), "bbtest") == 0);
  if (m)
    unmount(m);

  bb_write_sb(&b, 64ull << 20, 9, "newer");
  b.img[0x10000 + 500] ^= 1;
  m = mount_bb(&b);
  expect(m && strcmp(ops()->label(m), "newer") == 0);
  if (m)
    unmount(m);

  uint8_t *m64 = b.img + (64ull << 20);
  m64[48] ^= 1;
  expect(mount_bb(&b) == NULL);
  m64[48] ^= 1;
  m64[500] ^= 1;
  expect(mount_bb(&b) == NULL);
  m64[500] ^= 1;

  /* the 256 GiB mirror, served from a small buffer behind a sparse device */
  bb_write_sb_at(&b, 256ull << 30, 90u << 20, 11, "m256");
  memcpy(g_hi_buf, b.img + (90u << 20), 4096);
  g_hi_off = 256ull << 30;
  g_img = b.img;
  g_size = (size_t)g_hi_off + 4096;
  g_log[0] = 0;
  m = btrfs_ops()->probe(dev_read_hi, NULL);
  expect(m && strcmp(ops()->label(m), "m256") == 0);
  if (m)
    unmount(m);
  bb_write_sb(&b, 64ull << 20, 12, "m64newer");
  g_log[0] = 0;
  m = btrfs_ops()->probe(dev_read_hi, NULL);
  expect(m && strcmp(ops()->label(m), "m64newer") == 0);
  if (m)
    unmount(m);
  memset(b.img + (64ull << 20), 0, 4096);
  m = btrfs_ops()->probe(dev_read_hi, NULL);
  expect(m && strcmp(ops()->label(m), "m256") == 0);
  if (m)
    unmount(m);
  g_hi_off = 0;

  memset(b.img + 0x10000 + 64, 'x', 8);
  expect(mount_bb(&b) == NULL);
  bb_free(&b);
}

static void other_checksum_types(void)
{
  struct bb b;
  void *m;
  struct cfg c = kPlain;
  build_main(&b, &c);
  b.img[0x10000 + 196] = 1;
  b.img[0x10000 + 3] ^= 0xFF;
  m = mount_bb(&b);
  expect(m != NULL);
  if (m) {
    verify_all(m);
    unmount(m);
  }
  bb_free(&b);
}

static void incompat_features(void)
{
  struct bb b;
  build_main(&b, &kPlain);
  uint8_t *sb = b.img + 0x10000;
  static const uint64_t refuse[] = {0x1000, 0x2000, 0x4000, 0x10000, 1ull << 40, 0x1000 | 0x1};
  for (size_t i = 0; i < sizeof refuse / sizeof *refuse; i++) {
    uint64_t base = bb_g64(sb + 188);
    bb_p64(sb + 188, base | refuse[i]);
    fix_sb(sb);
    void *m = mount_bb(&b);
    expect(m == NULL && strstr(g_log, "unsupported incompat"));
    if (m)
      unmount(m);
    bb_p64(sb + 188, base);
    fix_sb(sb);
  }
  static const uint64_t accept[] = {0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x40, 0x80, 0x100, 0x200, 0x800, 0x8000};
  for (size_t i = 0; i < sizeof accept / sizeof *accept; i++) {
    uint64_t base = bb_g64(sb + 188);
    bb_p64(sb + 188, base | accept[i]);
    fix_sb(sb);
    void *m = mount_bb(&b);
    expect(m != NULL);
    if (m)
      unmount(m);
    bb_p64(sb + 188, base);
    fix_sb(sb);
  }

  static const struct {
    int off;
    uint32_t val;
  } bad[] = {{148, 3000}, {148, 2048}, {148, 131072}, {152, 8192},
             {144, 100},  {144, 0},    {144, 131072}, {144, 8192 * 2}};
  for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
    uint32_t keep = (uint32_t)bb_g64(sb + bad[i].off) & 0xFFFFFFFFu;
    bb_p32(sb + bad[i].off, bad[i].val);
    fix_sb(sb);
    void *m = mount_bb(&b);
    expect(m == NULL);
    if (m)
      unmount(m);
    bb_p32(sb + bad[i].off, keep);
    fix_sb(sb);
  }
  void *m = mount_bb(&b);
  expect(m != NULL);
  if (m)
    unmount(m);

  bb_free(&b);
}

static void raid56_refused(void)
{
  struct bb b;
  struct cfg c = kPlain;
  c.has_data = true;
  c.data = prof(BB_RAID5, 3, 0, 65536, 1, 1, 1, 0);
  build_main(&b, &c);
  expect(mount_bb(&b) == NULL && strstr(g_log, "RAID5/6"));
  bb_free(&b);
}

static void overlapping_chunks(void)
{
  struct bb b;
  void *m;
  build_main(&b, &kPlain);
  uint8_t *leaf = b.img + bb_phys_of(&b, b.chunk_tree_logical, 0);
  bb_p64(leaf + 101 + 25 * 3 + 9, b.ch[1].logical + 4096);
  bb_p32(leaf, ~bb_crc32c(0xFFFFFFFFu, leaf + 32, b.nodesize - 32));
  expect(mount_bb(&b) == NULL);
  bb_p64(leaf + 101 + 25 * 3 + 9, b.ch[2].logical);
  bb_p64(leaf + 101 + 25 * 2 + 9, b.ch[0].logical);
  bb_p32(leaf, ~bb_crc32c(0xFFFFFFFFu, leaf + 32, b.nodesize - 32));
  expect(mount_bb(&b) == NULL);
  bb_p64(leaf + 101 + 25 * 2 + 9, b.ch[1].logical);
  bb_p32(leaf, ~bb_crc32c(0xFFFFFFFFu, leaf + 32, b.nodesize - 32));
  m = mount_bb(&b);
  expect(m != NULL);
  if (m)
    unmount(m);
  bb_free(&b);
}

static uint8_t *node_phys(struct bb *b, uint64_t logical) { return b->img + bb_phys_of(b, logical, 0); }

static void reseal(struct bb *b, uint8_t *n) { bb_p32(n, ~bb_crc32c(0xFFFFFFFFu, n + 32, b->nodesize - 32)); }

static void checksum_mismatch(void)
{
  struct bb b;
  build_main(&b, &kPlain);
  for (uint64_t at = b.ch[1].logical; at < b.meta_next; at += b.nodesize)
    node_phys(&b, at)[0] ^= 0x5A;
  void *m = mount_bb(&b);
  expect(m != NULL);
  if (m) {
    verify_all(m);
    expect(count_log("checksum mismatch") == 1);
    unmount(m);
  }
  bb_free(&b);
}

static void dup_copy_replaced(void)
{
  struct bb b;
  void *m;
  struct cfg c = kPlain;
  c.has_meta = true;
  c.has_data = true;
  c.meta = prof(BB_DUP, 2, 0, 0, 1, 1, 0, 0);
  c.data = prof(0, 1, 0, 0, 1, 0, 0, 0);
  build_main(&b, &c);
  for (uint64_t at = b.ch[1].logical; at < b.meta_next; at += b.nodesize)
    node_phys(&b, at)[b.nodesize - 1] ^= 0xFF;
  m = mount_bb(&b);
  expect(m != NULL);
  if (m) {
    verify_all(m);
    expect(count_log("checksum") == 0);
    unmount(m);
  }
  bb_free(&b);
}

static void raid1_fallback(void)
{
  struct bb b;
  void *m;
  struct cfg c = kPlain;
  c.has_meta = true;
  c.has_data = true;
  c.data = prof(0, 1, 0, 0, 1, 0, 0, 0);
  c.meta = prof(BB_RAID1, 2, 0, 0, 1, 1, 0, 0);
  build_main(&b, &c);
  for (uint64_t at = b.ch[1].logical; at < b.meta_next; at += b.nodesize)
    b.img[bb_phys_of(&b, at, 0) + 1000] ^= 0xFF;
  m = mount_bb(&b);
  expect(m != NULL);
  if (m) {
    verify_all(m);
    expect(count_log("checksum") == 0);
    unmount(m);
  }
  bb_free(&b);
}

static void fsid_bytenr_mismatch(void)
{
  struct bb b;
  build_main(&b, &kPlain);
  uint8_t *root = node_phys(&b, b.root_tree_logical);
  root[40] ^= 1;
  expect(mount_bb(&b) == NULL);
  root[40] ^= 1;
  bb_p64(root + 48, b.root_tree_logical + 4096);
  expect(mount_bb(&b) == NULL);
  bb_p64(root + 48, b.root_tree_logical);
  uint8_t *fsroot = node_phys(&b, b.fs_root[0]);
  fsroot[100] ^= 1;
  expect(mount_bb(&b) == NULL);
  fsroot[100] ^= 1;
  fsroot[96] ^= 0x40;
  void *m2 = mount_bb(&b);
  if (m2) {
    int d;
    uint32_t sz;
    ops()->stat(m2, "docs/readme", &d, &sz);
    unmount(m2);
  }
  fsroot[96] ^= 0x40;
  m2 = mount_bb(&b);
  expect(m2 != NULL);
  if (m2)
    unmount(m2);
  bb_free(&b);
}

/* Guard every test body that could hang: a fired timer kills the process with a message. */
static int g_iter = -1;

static void on_alarm(int sig)
{
  (void)sig;
  char msg[80];
  int n = snprintf(msg, sizeof msg, "btrfs_test: hang detected (fuzz iteration %d)\n", g_iter);
  if (write(2, msg, (size_t)n) < 0)
    _exit(3);
  void *bt[40];
  backtrace_symbols_fd(bt, backtrace(bt, 40), 2);
  _exit(3);
}

static void guard(unsigned secs)
{
  struct itimerval it = {{0, 0}, {secs, 0}};
  signal(SIGALRM, on_alarm);
  setitimer(ITIMER_REAL, &it, NULL);
}

static void t_cycles(void)
{
  struct bb b;
  struct cfg c = {.nodesize = 4096, .sectorsize = 4096, .leaf_max = 8, .ptr_max = 4};
  build_main(&b, &c);
  uint8_t *root = node_phys(&b, b.fs_root[0]);
  uint32_t level = root[100], n = bb_g64(root + 96) & 0xFFFFFFFFu;
  expect(level >= 2 && n >= 2);

  guard(20);
  uint64_t save = bb_g64(root + 101 + 17), second = bb_g64(root + 101 + 33 + 17);
  bb_p64(root + 101 + 33 + 17, save);
  reseal(&b, root);
  void *m = mount_bb(&b);
  if (m) {
    char name[300];
    for (int i = 0; i < 2000; i++)
      if (ops()->enum_dir(m, "many", i, name, NULL, NULL) != 1)
        break;
    for (int i = 0; i < 2000; i++)
      if (ops()->enum_dir(m, "", i, name, NULL, NULL) != 1)
        break;
    unmount(m);
  }
  bb_p64(root + 101 + 33 + 17, second);

  bb_p64(root + 101 + 17, b.fs_root[0]);
  reseal(&b, root);
  m = mount_bb(&b);
  expect(m == NULL);
  if (m)
    unmount(m);
  bb_p64(root + 101 + 17, save);
  reseal(&b, root);
  m = mount_bb(&b);
  expect(m != NULL);
  if (m) {
    verify_all(m);
    unmount(m);
  }
  guard(0);
  bb_free(&b);
}

static unsigned g_walk_budget = 3000;

static void walk(void *m, const char *dir, int depth, unsigned *budget)
{
  for (int idx = 0;; idx++) {
    char name[300], path[2048];
    int is_dir = 0;
    uint32_t size = 0;
    if (*budget == 0 || ops()->enum_dir(m, dir, idx, name, &is_dir, &size) != 1)
      return;
    (*budget)--;
    join(path, sizeof path, dir, name);
    if (is_dir) {
      if (depth < 6)
        walk(m, path, depth + 1, budget);
    } else if (size) {
      uint8_t buf[512];
      uint32_t offs[3] = {0, size / 2, size - 1};
      for (int k = 0; k < 3; k++)
        (void)rd(m, path, offs[k], buf, sizeof buf);
    }
    uint32_t mode, uid, gid;
    (void)ops()->getattr(m, path, &mode, &uid, &gid);
  }
}

static void walk_all(void *m)
{
  unsigned budget = g_walk_budget;
  walk(m, "", 0, &budget);
  int d;
  uint32_t s;
  uint8_t buf[64];
  (void)ops()->stat(m, "sub/../docs/./nested/..", &d, &s);
  (void)ops()->stat(m, "c2", &d, &s);
  (void)rd(m, "big.bin", 400000, buf, sizeof buf);
  (void)rd(m, "sparse", 5000, buf, sizeof buf);
  (void)rd(m, "comp/zstd_v0_text.dat", 100000, buf, sizeof buf);
  (void)rd(m, "comp/lzo_v0_text.dat", 100000, buf, sizeof buf);
  (void)rd(m, "comp/zlib_v0_text.dat", 100000, buf, sizeof buf);
  g_out[0] = 0;
  g_log[0] = 0;
  (void)ops()->list(m, "docs");
  (void)ops()->list(m, "");
}

static void t_truncated(void)
{
  struct bb b;
  build_main(&b, &kPlain);
  size_t used = bb_phys_of(&b, b.meta_next - 1, 0) + 4096;
  size_t sizes[] = {0,       1,       511,      512,      0x10000,     0x10040, 0x10048,      0x10fff,
                    0x11000, 0x20000, 0x100000, 0x180000, used - 4096, used,    b.size - 512, b.size};
  for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
    void *m = mount_dev(b.img, sizes[i]);
    if (m) {
      walk_all(m);
      unmount(m);
    }
  }
  for (size_t cut = 0x100000; cut < used + 0x10000; cut += 0x9000 + (cut % 7) * 512) {
    void *m = mount_dev(b.img, cut);
    if (m) {
      walk_all(m);
      unmount(m);
    }
  }
  bb_free(&b);
}

static void t_nonbtrfs(void)
{
  size_t n = 4u << 20;
  uint8_t *img = calloc(1, n);
  expect(mount_dev(img, n) == NULL);
  fill_random(img, n, 5);
  expect(mount_dev(img, n) == NULL);
  memset(img, 0, n);
  img[510] = 0x55;
  img[511] = 0xAA;
  memcpy(img + 0x10000 + 64, "_BHRfS_M", 8);
  expect(mount_dev(img, n) == NULL && strstr(g_log, "no valid superblock"));
  memset(img, 0, n);
  memcpy(img + 1024 + 56, "\x53\xEF", 2);
  expect(mount_dev(img, n) == NULL);
  free(img);
}

static void t_partitions(void)
{
  struct bb b;
  build_main(&b, &kPlain);
  size_t img_bytes = bb_phys_of(&b, b.data_next - 1, 0) + 4096;
  size_t total = (4u << 20) + img_bytes;
  uint8_t *disk = calloc(1, total);
  disk[510] = 0x55;
  disk[511] = 0xAA;
  uint8_t *pe = disk + 446;
  pe[4] = 0x83;
  bb_p32(pe + 8, 2048);
  bb_p32(pe + 12, (uint32_t)(img_bytes / 512));
  memcpy(disk + 2048 * 512, b.img, img_bytes);
  void *m = mount_dev(disk, total);
  expect(m != NULL);
  if (m) {
    verify_all(m);
    unmount(m);
  }

  memset(disk, 0, 4u << 20);
  memcpy(disk + 512, "EFI PART", 8);
  bb_p64(disk + 512 + 72, 2);
  bb_p32(disk + 512 + 80, 4);
  bb_p32(disk + 512 + 84, 128);
  uint8_t *ent = disk + 1024 + 128;
  memset(ent, 7, 16);
  bb_p64(ent + 32, 4096);
  memcpy(disk + 4096 * 512, b.img, img_bytes);
  m = mount_dev(disk, total);
  expect(m != NULL);
  if (m) {
    verify_all(m);
    unmount(m);
  }
  free(disk);
  bb_free(&b);
}

static void t_default_subvol(void)
{
  struct bb b;
  struct cfg c = kPlain;
  c.default_id = 257;
  build_main(&b, &c);
  void *m = mount_bb(&b);
  expect(m != NULL);
  if (m) {
    uint8_t buf[32];
    int d;
    uint32_t sz;
    expect(rd(m, "hello.txt", 0, buf, 32) == 15 && memcmp(buf, "snapshot hello\n", 15) == 0);
    expect(ops()->stat(m, "docs", &d, &sz) != 0);
    expect(ops()->stat(m, "..", &d, &sz) == 0 && d);
    char name[300];
    expect(ops()->enum_dir(m, "", 0, name, NULL, NULL) == 1 && strcmp(name, "hello.txt") == 0);
    expect(ops()->enum_dir(m, "", 1, name, NULL, NULL) == 0);
    unmount(m);
  }
  bb_free(&b);

  c.default_id = 999;
  build_main(&b, &c);
  m = mount_bb(&b);
  expect(m != NULL);
  if (m) {
    verify_all(m);
    unmount(m);
  }
  bb_free(&b);

  c.default_id = 256;
  build_main(&b, &c);
  m = mount_bb(&b);
  expect(m != NULL);
  if (m) {
    uint8_t buf[32];
    int d;
    uint32_t sz;
    expect(rd(m, "inside.txt", 0, buf, 32) == 21);
    expect(rd(m, "out", 0, buf, 32) < 0);
    expect(ops()->stat(m, "d/../..", &d, &sz) == 0 && d);
    unmount(m);
  }
  bb_free(&b);
}

/* ---------------------------------------------------------------- devices that are missing, huge files */

static void missing_device_reads(void)
{
  struct bb b;
  struct cfg c = kPlain;
  c.has_meta = c.has_data = true;
  c.meta = prof(0, 1, 0, 0, 1, 0, 0, 0);
  c.data = prof(BB_RAID0, 2, 0, 65536, 1, 2, 0, 0);
  build_main(&b, &c);
  void *m = mount_bb(&b);
  expect(m != NULL);
  if (m) {
    struct ex *big = find_ex("big.bin");
    uint8_t *got = malloc(8192);
    unsigned ok = 0, failed = 0;
    for (size_t off = 0; off < big->len; off += 4096) {
      uint32_t want = big->len - off < 4096 ? (uint32_t)(big->len - off) : 4096;
      int r = rd(m, "big.bin", (uint32_t)off, got, 4096);
      if (r == (int)want) {
        expect(memcmp(got, big->data + off, want) == 0);
        ok++;
      } else {
        expect(r < 0);
        failed++;
      }
    }
    expect(ok > 20 && failed > 20);
    free(got);
    unmount(m);
  }
  bb_free(&b);
}

static void no_stripe_on_device(void)
{
  struct bb b;
  struct cfg c = kPlain;
  c.has_meta = c.has_data = true;
  c.meta = prof(0, 1, 0, 0, 2, 0, 0, 0);
  c.data = prof(0, 1, 0, 0, 1, 0, 0, 0);
  build_main(&b, &c);
  expect(mount_bb(&b) == NULL);
  bb_free(&b);
  c.meta = prof(BB_RAID1, 2, 0, 0, 2, 3, 0, 0);
  build_main(&b, &c);
  expect(mount_bb(&b) == NULL);
  bb_free(&b);
}

static void raid10_pair_missing(void)
{
  struct bb b;
  void *m;
  struct cfg c = kPlain;
  c.has_meta = c.has_data = true;
  c.meta = prof(0, 1, 0, 0, 1, 0, 0, 0);
  c.data = prof(BB_RAID10, 4, 2, 65536, 1, 2, 3, 3);
  build_main(&b, &c);
  m = mount_bb(&b);
  expect(m != NULL);
  if (m) {
    struct ex *big = find_ex("big.bin");
    uint8_t *got = malloc(8192);
    unsigned ok = 0, failed = 0;
    for (size_t off = 0; off < big->len; off += 4096) {
      uint32_t want = big->len - off < 4096 ? (uint32_t)(big->len - off) : 4096;
      int r = rd(m, "big.bin", (uint32_t)off, got, 4096);
      if (r == (int)want) {
        expect(memcmp(got, big->data + off, want) == 0);
        ok++;
      } else {
        expect(r < 0);
        failed++;
      }
    }
    expect(ok > 20 && failed > 20);
    free(got);
    unmount(m);
  }
  bb_free(&b);
}

static void t_huge_files(void)
{
  struct bb b;
  bb_init(&b, 4096, 4096, NULL, NULL, 12u << 20, 24u << 20);
  struct bb_tree *t = new_tree(5);
  bb_root_dir(t);
  uint8_t A[4096], B[8192], C[4096];
  fill_random(A, sizeof A, 1);
  fill_random(B, sizeof B, 2);
  fill_random(C, sizeof C, 3);
  uint64_t ino = bb_mkfile(t, 256, "huge", 0100644, 0, 0, 0x200000000ull + 8192, 1);
  bb_fe_reg(t, ino, 0, 0, bb_data(&b, A, sizeof A), 4096, 0, 4096, 4096, 1);
  bb_fe_reg(t, ino, 0x200000000ull, 0, bb_data(&b, B, sizeof B), 8192, 0, 8192, 8192, 1);
  uint64_t top = 0x7FFFFFFFFFFFF000ull;
  ino = bb_mkfile(t, 256, "top", 0100644, 0, 0, top, 1);
  bb_fe_reg(t, ino, top - 4096, 0, bb_data(&b, C, sizeof C), 4096, 0, 4096, 4096, 1);
  ino = bb_mkfile(t, 256, "wrap", 0100644, 0, 0, 0xFFFFFFFFFFFFFFFFull, 1);
  bb_fe_reg(t, ino, 0xFFFFFFFFFFFFF000ull, 0, bb_data(&b, A, sizeof A), 8192, 0, 8192, 8192, 1);
  struct bb_tree *fs[1] = {t};
  bb_commit(&b, fs, 1, 5, NULL);
  del_tree(t);

  void *m = mount_bb(&b);
  expect(m != NULL);
  if (m) {
    uint8_t got[8192], zero[4096] = {0};
    int is_dir;
    uint32_t size;
    expect(ops()->stat(m, "huge", &is_dir, &size) == 0 && size == 0xFFFFFFFFu);
    expect(btrfs_read64(m, "huge", 0, got, 4096) == 4096 && memcmp(got, A, 4096) == 0);
    expect(btrfs_read64(m, "huge", 0x1FFFFF000ull, got, 4096) == 4096 && memcmp(got, zero, 4096) == 0);
    expect(btrfs_read64(m, "huge", 0x200000000ull, got, 8192) == 8192 && memcmp(got, B, 8192) == 0);
    expect(btrfs_read64(m, "huge", 0x200000000ull + 4090, got, 100) == 100 && memcmp(got, B + 4090, 100) == 0);
    expect(btrfs_read64(m, "huge", 0x200000000ull + 8192, got, 100) == 0);
    expect(btrfs_read64(m, "huge", ~0ull, got, 100) == 0);
    expect(rd(m, "huge", 0xFFFFFFFFu, got, 4096) == 4096 && memcmp(got, zero, 4096) == 0);
    expect(btrfs_read64(m, "top", top - 4096, got, 4096) == 4096 && memcmp(got, C, 4096) == 0);
    expect(btrfs_read64(m, "top", top - 1, got, 4096) == 1 && got[0] == C[4095]);
    expect(btrfs_read64(m, "top", top, got, 10) == 0);
    struct btrfs_attr at;
    expect(btrfs_getinfo(m, "top", &at) == 0 && at.size == top);
    expect(btrfs_read64(m, "wrap", 0, got, 4096) == 4096 && memcmp(got, zero, 4096) == 0);
    expect(btrfs_read64(m, "wrap", 0xFFFFFFFFFFFFF000ull, got, 4096) < 0);
    unmount(m);
  }
  bb_free(&b);
}

/* ---------------------------------------------------------------- cache and performance */

static void node_cache_budget(void)
{
  struct bb b;
  bb_init(&b, 4096, 4096, NULL, NULL, 12u << 20, 24u << 20);
  struct bb_tree *t = new_tree(5);
  bb_root_dir(t);
  const int N = 9000;
  uint64_t dir = bb_mkdir(t, 256, "big", 0755, 0, 0);
  for (int i = 0; i < N; i++) {
    char name[32], content[16];
    snprintf(name, sizeof name, "e%05d", i);
    snprintf(content, sizeof content, "%d", i);
    add_inline(t, dir, name, content, strlen(content), 0644, 0, 0);
  }
  struct bb_tree *fs[1] = {t};
  bb_commit(&b, fs, 1, 5, NULL);
  del_tree(t);
  void *m = mount_bb(&b);
  expect(m != NULL);
  if (m) {
    char name[300], want[32];
    bool ok = true;
    g_nread = 0;
    for (int i = 0; i < N && ok; i++) {
      int is_dir;
      uint32_t size;
      char content[16];
      snprintf(want, sizeof want, "e%05d", i);
      snprintf(content, sizeof content, "%d", i);
      ok = ops()->enum_dir(m, "big", i, name, &is_dir, &size) == 1 && strcmp(name, want) == 0 && !is_dir &&
           size == strlen(content);
    }
    expect(ok);
    unsigned long seq_reads = g_nread;
    printf("  sequential enumeration of %d entries: %lu sector reads\n", N, seq_reads);
    expect(seq_reads < 20ul * N);
    struct bt_mount *bm = m;
    expect((uint64_t)bm->cached_nodes * 4096 <= BT_CACHE_BUDGET);
    expect(bm->cached_nodes > 100);

    int order[] = {8999, 3, 4500, 4501, 0, 8998, 17, 17, 6000, 5999};
    for (size_t k = 0; k < sizeof order / sizeof *order; k++) {
      snprintf(want, sizeof want, "e%05d", order[k]);
      expect(ops()->enum_dir(m, "big", order[k], name, NULL, NULL) == 1 && strcmp(name, want) == 0);
    }
    expect(ops()->enum_dir(m, "big", N, name, NULL, NULL) == 0);
    uint8_t got[16];
    expect(rd(m, "big/e04321", 0, got, 16) == 4 && memcmp(got, "4321", 4) == 0);
    unmount(m);
  }
  bb_free(&b);
}

static void sequential_file_reads(void)
{
  struct bb b;
  void *m;
  build_main(&b, &kPlain);
  m = mount_bb(&b);
  expect(m != NULL);
  if (m) {
    struct ex *big = find_ex("big.bin");
    uint8_t *got = malloc(big->len);
    int is_dir;
    uint32_t size;
    ops()->stat(m, "big.bin", &is_dir, &size);
    g_nread = 0;
    size_t pos = 0;
    while (pos < big->len) {
      int r = rd(m, "big.bin", (uint32_t)pos, got + pos, 4096);
      if (r <= 0)
        break;
      pos += (size_t)r;
    }
    unsigned long sectors = (big->len + 511) / 512;
    printf("  %zu bytes in %lu sector reads (%lu sectors of data)\n", pos, g_nread, sectors);
    expect(pos == big->len && memcmp(got, big->data, big->len) == 0);
    expect(g_nread <= sectors + sectors / 4 + 200);
    free(got);
    unmount(m);
  }
  bb_free(&b);
}

/* ---------------------------------------------------------------- allocation failures */

struct oom_ctx {
  const struct bb *b;
  int mode;
  long *points; /* shared: allocation points reached, per worker */
};

static void oom_worker(int id, int workers, void *arg)
{
  struct oom_ctx *c = arg;
  for (long k = id;; k += workers) {
    g_allocs = 0;
    g_fail_at = c->mode == 0 ? k : -1;
    g_fail_from = c->mode == 1 ? k : -1;
    void *m = mount_bb(c->b);
    if (m) {
      walk_all(m);
      btrfs_ops()->close(m);
    }
    bool hit = g_allocs > k;
    g_fail_at = g_fail_from = -1;
    if (g_live != 0) {
      printf("  leak after failing allocation %ld (mode %d): %ld live\n", k, c->mode, g_live);
      expect(false);
      return;
    }
    if (!hit) {
      c->points[id] = k;
      return;
    }
  }
}

static void t_oom(void)
{
  struct bb b;
  g_many = 24;
  build_main(&b, &kPlain);
  g_many = 700;
  unsigned saved_budget = g_walk_budget;
  g_walk_budget = 80;
  int workers = (int)sysconf(_SC_NPROCESSORS_ONLN);
  long *points = mmap(0, sizeof *points * (size_t)workers, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
  for (int mode = 0; mode < 2; mode++) {
    struct oom_ctx c = {&b, mode, points};
    owos_parallel(workers, oom_worker, &c);
    long reached = 0;
    for (int i = 0; i < workers; i++)
      if (points[i] > reached)
        reached = points[i];
    printf("  mode %d: %ld allocation points exercised\n", mode, reached);
  }
  munmap(points, sizeof *points * (size_t)workers);
  g_walk_budget = saved_budget;
  bb_free(&b);
}

/* ---------------------------------------------------------------- decoders */

static uint8_t *src, *enc, *dec;

static void lzo_hand_built(void)
{
  uint8_t out[64];
  uint32_t n;
  static const uint8_t lit4[] = {0x15, 'a', 'b', 'c', 'd', 0x11, 0, 0};
  expect(lzo1x_unpack(lit4, sizeof lit4, out, sizeof out, &n) == 0 && n == 4 && memcmp(out, "abcd", 4) == 0);
  static const uint8_t m2[] = {0x14, 'a', 'b', 'c', 0x48, 0x00, 0x11, 0, 0};
  expect(lzo1x_unpack(m2, sizeof m2, out, sizeof out, &n) == 0 && n == 6 && memcmp(out, "abcabc", 6) == 0);
  static const uint8_t trail[] = {0x14, 'a', 'b', 'c', 0x4B, 0x00, 'x', 'y', 'z', 0x11, 0, 0};
  expect(lzo1x_unpack(trail, sizeof trail, out, sizeof out, &n) == 0 && n == 9 && memcmp(out, "abcabcxyz", 9) == 0);

  expect(lzo1x_unpack(lit4, 0, out, sizeof out, &n) != 0);
  for (size_t cut = 1; cut < sizeof lit4; cut++)
    expect(lzo1x_unpack(lit4, cut, out, sizeof out, &n) != 0);
  expect(lzo1x_unpack(lit4, sizeof lit4, out, 3, &n) != 0);
  expect(lzo1x_unpack(m2, sizeof m2, out, 5, &n) != 0);
  static const uint8_t far[] = {0x14, 'a', 'b', 'c', 0x48, 0x10, 0x11, 0, 0};
  expect(lzo1x_unpack(far, sizeof far, out, sizeof out, &n) != 0);
  static const uint8_t extra[] = {0x15, 'a', 'b', 'c', 'd', 0x11, 0, 0, 0};
  expect(lzo1x_unpack(extra, sizeof extra, out, sizeof out, &n) != 0);
  static const uint8_t noeos[] = {0x15, 'a', 'b', 'c', 'd'};
  expect(lzo1x_unpack(noeos, sizeof noeos, out, sizeof out, &n) != 0);
  static const uint8_t runaway[] = {0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  expect(lzo1x_unpack(runaway, sizeof runaway, out, sizeof out, &n) != 0);
}

static void lzo_round_trips(void)
{
  uint32_t n;
  src = malloc(1 << 17);
  enc = malloc((1 << 17) + (1 << 14) + 256);
  dec = malloc(1 << 17);
  static const size_t sizes[] = {1,   2,   3,    4,    17,   18,    19,    238,    239,
                                 255, 256, 1000, 4096, 5000, 20000, 65536, 100000, 131072};
  for (int kind = 0; kind < 4; kind++) {
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
      size_t len = sizes[i];
      if (kind == 0)
        fill_random(src, len, (uint32_t)i);
      else if (kind == 1)
        fill_text(src, len, (uint32_t)i);
      else if (kind == 2)
        memset(src, 'z', len);
      else
        for (size_t k = 0; k < len; k++)
          src[k] = (uint8_t)((k * k >> 7) ^ (k % 61 == 0 ? rnd() : 0));
      size_t el = bb_lzo_compress(src, len, enc);
      expect(lzo1x_unpack(enc, (uint32_t)el, dec, (uint32_t)len, &n) == 0 && n == len && memcmp(dec, src, len) == 0);
      if (len > 1)
        expect(lzo1x_unpack(enc, (uint32_t)el, dec, (uint32_t)len - 1, &n) != 0);
#ifdef BB_HAVE_LZO
      static uint8_t wrk[LZO1X_999_MEM_COMPRESS];
      lzo_uint hl = 0;
      lzo_init();
      lzo1x_999_compress(src, len, enc, &hl, wrk);
      expect(lzo1x_unpack(enc, (uint32_t)hl, dec, (uint32_t)len, &n) == 0 && n == len && memcmp(dec, src, len) == 0);
      lzo1x_1_compress(src, len, enc, &hl, wrk);
      expect(lzo1x_unpack(enc, (uint32_t)hl, dec, (uint32_t)len, &n) == 0 && n == len && memcmp(dec, src, len) == 0);
#endif
    }
  }
}

static void lzo_mutated_streams(void)
{
  uint32_t n;
  fill_text(src, 20000, 9);
  size_t el = bb_lzo_compress(src, 20000, enc);
  for (int it = 0; it < 20000; it++) {
    uint8_t *cp = malloc(el);
    memcpy(cp, enc, el);
    for (int k = 0, nm = 1 + rnd() % 4; k < nm; k++)
      cp[rnd() % el] = (uint8_t)rnd();
    size_t cl = rnd() % 8 == 0 ? rnd() % el : el;
    uint32_t cap = rnd() % 3 == 0 ? rnd() % 30000 : 20000;
    if (lzo1x_unpack(cp, (uint32_t)cl, dec, cap, &n) == 0)
      expect(n <= cap);
    free(cp);
  }
  for (int it = 0; it < 20000; it++) {
    uint8_t rbuf[200];
    size_t len = rnd() % sizeof rbuf;
    for (size_t k = 0; k < len; k++)
      rbuf[k] = (uint8_t)(rnd() % 4 ? rnd() % 64 : rnd());
    if (lzo1x_unpack(rbuf, (uint32_t)len, dec, 300, &n) == 0)
      expect(n <= 300);
  }
  free(src);
  free(enc);
  free(dec);
}

static const size_t zstd_cap = 300000;

static void zstd_hand_built(void)
{
  src = malloc(zstd_cap);
  enc = malloc(zstd_cap + 4096);
  dec = malloc(zstd_cap);
  static const size_t sizes[] = {0, 1, 100, 4096, 5000, 70000, 131072, 131073, 262144, 300000};
  for (int kind = 0; kind < 3; kind++) {
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
      size_t len = sizes[i];
      if (kind == 0)
        fill_random(src, len, (uint32_t)i);
      else if (kind == 1)
        memset(src, 0x5A, len);
      else
        for (size_t k = 0; k < len; k++)
          src[k] = (k / 500) % 3 == 0 ? 7 : (uint8_t)(k * 31);
      size_t el = bb_zstd_hand(src, len, enc);
      expect(zstd_decompress(enc, (uint32_t)el, dec, (uint32_t)len) == (int)len && memcmp(dec, src, len) == 0);
      if (len)
        expect(zstd_decompress(enc, (uint32_t)el, dec, (uint32_t)len - 1) < 0);
      for (size_t cut = 0; cut < el && cut < 40; cut++)
        expect(zstd_decompress(enc, (uint32_t)cut, dec, (uint32_t)zstd_cap) < 0);
    }
  }
}

static void zstd_frame_header(void)
{
  fill_random(src, 100, 1);
  size_t el = bb_zstd_hand(src, 100, enc);
  uint8_t *two = malloc(el + 16);
  bb_p32(two, 0x184D2A5Bu);
  bb_p32(two + 4, 5);
  memset(two + 8, 0xEE, 5);
  memcpy(two + 13, enc, el);
  expect(zstd_decompress(two, (uint32_t)(el + 13), dec, 100) == 100 && memcmp(dec, src, 100) == 0);
  memcpy(two, enc, el);
  memset(two + el, 0, 8);
  expect(zstd_decompress(two, (uint32_t)(el + 8), dec, 100) == 100);
  uint8_t *bad = malloc(el);
  memcpy(bad, enc, el);
  bad[4] = 0xA1;
  expect(zstd_decompress(bad, (uint32_t)el, dec, 100) < 0);
  memcpy(bad, enc, el);
  bad[4] = 0xA8;
  expect(zstd_decompress(bad, (uint32_t)el, dec, 100) < 0);
  memcpy(bad, enc, el);
  bb_p32(bad + 5, 1000);
  expect(zstd_decompress(bad, (uint32_t)el, dec, 100) < 0);
  memcpy(bad, enc, el);
  bad[0] ^= 1;
  expect(zstd_decompress(bad, (uint32_t)el, dec, 100) < 0);
  memcpy(bad, enc, el);
  bad[9] |= 6;
  expect(zstd_decompress(bad, (uint32_t)el, dec, 100) < 0);
  uint8_t big[16] = {0x28, 0xB5, 0x2F, 0xFD, 0xA0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF};
  expect(zstd_decompress(big, sizeof big, dec, 100) < 0);
  free(two);
  free(bad);
}

#ifdef BB_HAVE_ZSTD
static void zstd_host_levels(void)
{
  static const size_t hs[] = {1, 10, 100, 1000, 4096, 5000, 30000, 98304, 131072, 200000, 300000};
  for (int kind = 0; kind < 4; kind++) {
    for (size_t i = 0; i < sizeof hs / sizeof *hs; i++) {
      size_t len = hs[i];
      if (kind == 0)
        fill_random(src, len, (uint32_t)i + 50);
      else if (kind == 1)
        fill_text(src, len, (uint32_t)i + 50);
      else if (kind == 2)
        for (size_t k = 0; k < len; k++)
          src[k] = (uint8_t)((k / 3) & 7);
      else
        for (size_t k = 0; k < len; k++)
          src[k] = (rnd() % 10 < 8) ? 'e' : (uint8_t)rnd();
      static const int levels[] = {-3, 1, 3, 9, 19};
      for (size_t l = 0; l < sizeof levels / sizeof *levels; l++) {
        size_t zl = ZSTD_compress(enc, zstd_cap + 4096, src, len, levels[l]);
        expect(!ZSTD_isError(zl));
        expect(zstd_decompress(enc, (uint32_t)zl, dec, (uint32_t)len) == (int)len && memcmp(dec, src, len) == 0);
        if (len > 1)
          expect(zstd_decompress(enc, (uint32_t)zl, dec, (uint32_t)len - 1) < 0);
      }
    }
  }
}

static void zstd_host_mutated(void)
{
  fill_text(src, 60000, 77);
  for (size_t i = 0; i < 60000; i += 5)
    src[i] = (uint8_t)rnd();
  size_t zl = ZSTD_compress(enc, zstd_cap + 4096, src, 60000, 3);
  for (size_t cut = 0; cut < zl; cut += 1 + zl / 400)
    expect(zstd_decompress(enc, (uint32_t)cut, dec, 60000) < 0);
  for (int it = 0; it < 30000; it++) {
    uint8_t *cp = malloc(zl);
    memcpy(cp, enc, zl);
    for (int k = 0, nm = 1 + rnd() % 4; k < nm; k++)
      cp[(rnd() % 3 == 0 ? rnd() % 64 : rnd()) % zl] ^= (uint8_t)(1u << (rnd() % 8));
    uint32_t dcap = rnd() % 4 == 0 ? rnd() % 70000 : 60000;
    int r = zstd_decompress(cp, (uint32_t)zl, dec, dcap);
    expect(r < 0 || (uint32_t)r <= dcap);
    free(cp);
  }
}
#endif

static void zstd_random_behind_header(void)
{
  for (int it = 0; it < 20000; it++) {
    uint8_t rbuf[300];
    memcpy(rbuf, "\x28\xB5\x2F\xFD\xA0\x00\x04\x00\x00", 9);
    size_t len = 9 + rnd() % 200;
    for (size_t k = 9; k < len; k++)
      rbuf[k] = (uint8_t)(rnd() % 3 ? rnd() % 40 : rnd());
    int r = zstd_decompress(rbuf, (uint32_t)len, dec, 4);
    expect(r < 0 || r <= 4);
  }
  free(src);
  free(enc);
  free(dec);
}

/* ---------------------------------------------------------------- fuzz */

struct undo {
  size_t pos;
  uint8_t old;
};

struct fuzz_ctx {
  struct bb *b;
  struct {
    size_t base, len;
    unsigned weight;
  } reg[4];
  unsigned total_w;
  int iters, only;
  unsigned *mounted; /* shared, one slot per worker */
};

/* Worker `id` runs iterations id, id + workers, ... on its own copy of the image, each from a seed of its own. */
static void fuzz_worker(int id, int workers, void *arg)
{
  struct fuzz_ctx *c = arg;
  struct bb *b = c->b;
  unsigned mounted = 0;
  g_rng = 0xB7F5u + (unsigned)id * 0x9E3779B1u;
  unsigned saved_budget = g_walk_budget;
  g_walk_budget = 400;
  struct undo log[16];
  for (int it = id; it < c->iters; it += workers) {
    g_iter = it;
    guard(30);
    int nm = 1 + rnd() % 6, nlog = 0;
    for (int k = 0; k < nm; k++) {
      unsigned w = rnd() % c->total_w, r = 0;
      while (w >= c->reg[r].weight)
        w -= c->reg[r++].weight;
      size_t pos = c->reg[r].base + rnd() % c->reg[r].len;
      log[nlog].pos = pos;
      log[nlog++].old = b->img[pos];
      static const uint8_t special[] = {0x00, 0xFF, 0x7F, 0x80, 0x01};
      uint32_t how = rnd() % 4;
      b->img[pos] = how == 0   ? special[rnd() % 5]
                    : how == 1 ? (uint8_t)(b->img[pos] ^ (1u << (rnd() % 8)))
                               : (uint8_t)rnd();
    }
    uint8_t keep[4];
    bool resealed = rnd() % 2 == 0;
    memcpy(keep, b->img + 0x10000, 4);
    if (resealed)
      fix_sb(b->img + 0x10000);
    size_t size = b->size;
    if (rnd() % 20 == 0)
      size = 0x10000 + rnd() % (c->reg[3].base + c->reg[3].len);
    void *m = c->only >= 0 && it != c->only ? NULL : mount_dev(b->img, size);
    if (m) {
      mounted++;
      walk_all(m);
      unmount(m);
    } else {
      expect(g_live == 0);
    }
    if (resealed)
      memcpy(b->img + 0x10000, keep, 4);
    while (nlog--)
      b->img[log[nlog].pos] = log[nlog].old;
  }
  guard(0);
  g_walk_budget = saved_budget;
  c->mounted[id] = mounted;
}

static void t_fuzz(void)
{
  struct bb b;
  struct cfg c = {.nodesize = 4096, .sectorsize = 4096, .leaf_max = 12, .ptr_max = 5};
  g_many = 40;
  build_main(&b, &c);
  g_many = 700;
  struct fuzz_ctx fc = {.b = &b,
                        .reg = {{0x10000, 4096, 12},
                                {b.ch[0].st[0].off, (size_t)(b.sys_next - b.ch[0].logical), 12},
                                {b.ch[1].st[0].off, (size_t)(b.meta_next - b.ch[1].logical), 60},
                                {b.ch[2].st[0].off, (size_t)(b.data_next - b.ch[2].logical), 16}}};
  for (size_t i = 0; i < 4; i++)
    fc.total_w += fc.reg[i].weight;
  const char *env = getenv("BTRFS_FUZZ_ITERS");
  fc.iters = env ? atoi(env) : 3000;
  const char *only_env = getenv("BTRFS_FUZZ_ONLY");
  fc.only = only_env ? atoi(only_env) : -1;
  int workers = (int)sysconf(_SC_NPROCESSORS_ONLN);
  fc.mounted = mmap(0, sizeof *fc.mounted * (size_t)workers, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
  owos_parallel(workers, fuzz_worker, &fc);
  unsigned mounted = 0;
  for (int i = 0; i < workers; i++)
    mounted += fc.mounted[i];
  printf("  %d iterations, %u mounted\n", fc.iters, mounted);
  expect(mounted > (unsigned)fc.iters / 4);
  munmap(fc.mounted, sizeof *fc.mounted * (size_t)workers);
  void *m = mount_bb(&b);
  expect(m != NULL);
  if (m)
    unmount(m);
  bb_free(&b);
}

int main(void)
{
#ifdef BTRFS_ONLY
  BTRFS_ONLY();
  return 0;
#endif
  t_variants();
  test("btrfs: superblock selection and mirrors", superblock_selection);
  test("btrfs: other checksum types are accepted unverified", other_checksum_types);
  test("btrfs: incompat features and geometry", incompat_features);
  test("btrfs: RAID5/6 chunks are refused", raid56_refused);
  test("btrfs: overlapping and conflicting chunks are refused", overlapping_chunks);
  test("btrfs: metadata checksum mismatch warns once and continues", checksum_mismatch);
  test("btrfs: a bad DUP copy is replaced by the good one without a warning", dup_copy_replaced);
  test("btrfs: RAID1 falls back to the second mirror", raid1_fallback);
  test("btrfs: fsid and bytenr mismatches reject the node", fsid_bytenr_mismatch);
  test("btrfs: cyclic and repeated tree pointers cannot hang or loop", t_cycles);
  test("btrfs: truncated devices never crash", t_truncated);
  test("btrfs: other media are not mistaken for btrfs", t_nonbtrfs);
  test("btrfs: found inside MBR and GPT partitions", t_partitions);
  test("btrfs: the default subvolume is mounted", t_default_subvol);
  test("btrfs: reads that need a missing device fail, nothing is invented", missing_device_reads);
  test("btrfs: metadata with no stripe on our device does not mount", no_stripe_on_device);
  test("btrfs: RAID10 with both copies of a pair missing fails only those reads", raid10_pair_missing);
  test("btrfs: files up to 2^63 and the 64-bit read call", t_huge_files);
  test("btrfs: node cache stays within its budget on a large directory", node_cache_budget);
  test("btrfs: sequential file reads touch each sector about once", sequential_file_reads);
  test("btrfs: allocation failures leave nothing behind", t_oom);
  test("lzo: hand-built streams and malformed input", lzo_hand_built);
  test("lzo: round trips through the builder and the host library", lzo_round_trips);
  test("lzo: mutated and random streams are rejected or decoded within bounds", lzo_mutated_streams);
  test("zstd: hand-built raw and RLE frames", zstd_hand_built);
  test("zstd: frame header rules", zstd_frame_header);
#ifdef BB_HAVE_ZSTD
  test("zstd: host libzstd frames at several levels decode exactly", zstd_host_levels);
  test("zstd: truncated and mutated host frames never overrun", zstd_host_mutated);
#endif
  test("zstd: random bytes behind a valid header", zstd_random_behind_header);
  test("btrfs: seeded corruption fuzz of mount, listing and reads", t_fuzz);
  clear_ex();
  free(g_ex);
  return 0;
}
