/* fat_test.c: host test for kernel/fat.c on an in-memory block device, with fsck_msdos/mtools
 * cross-checks. Run with `make test`. */
#define OWOS_SUITE_TEST
#define OWOS_CONSOLE_H
#define OWOS_HEAP_H
#define OWOS_KSTRING_H
#define OWOS_WALLCLOCK_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

struct timeval64 {
  long long tv_sec;
  int tv_usec;
};

static long long g_clock = 1700000000LL;
static int g_clock_usec;

static void wallclock_get(struct timeval64 *tv)
{
  tv->tv_sec = g_clock;
  tv->tv_usec = g_clock_usec;
}

static void *kmalloc(size_t n) { return malloc(n); }
static void kfree(void *p) { free(p); }
static void con_write(const char *s) { (void)s; }
static void con_printf(const char *fmt, ...) { (void)fmt; }
static inline int streq(const char *a, const char *b) { return strcmp(a, b) == 0; }

#include "../kernel/fat.c"
#undef M
#undef SEC

static uint8_t *g_img;
static size_t g_img_bytes;
static uint32_t g_secs;
static unsigned g_nread, g_nwrite;
static long g_fail_write = -1;

static int dev_read(uint32_t lba, void *buf)
{
  if (lba >= g_secs)
    return -1;
  g_nread++;
  memcpy(buf, g_img + (size_t)lba * 512, 512);
  return 0;
}

static int dev_write(uint32_t lba, const void *buf)
{
  if (lba >= g_secs || g_fail_write == 0)
    return -1;
  if (g_fail_write > 0)
    g_fail_write--;
  g_nwrite++;
  memcpy(g_img + (size_t)lba * 512, buf, 512);
  return 0;
}

static void dev_new(uint32_t secs)
{
  if (g_img)
    munmap(g_img, g_img_bytes);
  g_secs = secs;
  g_img_bytes = (size_t)secs * 512;
  g_img = mmap(NULL, g_img_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
  if (g_img == MAP_FAILED)
    abort();
  g_fail_write = -1;
}

static void dev_load(const char *path)
{
  FILE *f = fopen(path, "rb");
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  dev_new((uint32_t)(n / 512));
  if (fread(g_img, 1, (size_t)n, f) != (size_t)n)
    abort();
  fclose(f);
}

static int fmt_dev(int kind, uint32_t secs, uint32_t bps, uint32_t spc, const char *label)
{
  dev_new(secs);
  struct fat_format_opts o = {.write = dev_write,
                              .sectors = secs,
                              .kind = kind,
                              .bytes_per_sec = bps,
                              .sec_per_clus = spc,
                              .label = label,
                              .serial = 0x12345678};
  return fat_format(&o);
}

static void *mnt(void) { return fat_ops()->probe(dev_read, dev_write); }

static void *remount(void *inst)
{
  fat_ops()->close(inst);
  return mnt();
}

static uint32_t g_seed = 1;
static uint32_t rnd(void)
{
  g_seed = g_seed * 1664525u + 1013904223u;
  return g_seed >> 8;
}

static void fill(uint8_t *b, size_t n, uint32_t seed)
{
  for (size_t i = 0; i < n; i++)
    b[i] = (uint8_t)((seed * 2654435761u + i * 40503u) >> 13);
}

static int put(void *fs, const char *path, const void *data, uint32_t n)
{
  return fat_ops()->create(fs, path, data, n);
}

static int put_seed(void *fs, const char *path, uint32_t n, uint32_t seed)
{
  uint8_t *b = malloc(n + 1);
  fill(b, n, seed);
  int rc = put(fs, path, b, n);
  free(b);
  return rc;
}

static bool is_seed(void *fs, const char *path, uint32_t n, uint32_t seed)
{
  uint8_t *want = malloc(n + 1), *got = malloc(n + 1);
  fill(want, n, seed);
  int is_dir = 0;
  uint32_t size = 0;
  bool ok = fat_ops()->stat(fs, path, &is_dir, &size) == 0 && !is_dir && size == n;
  for (uint32_t off = 0; ok && off < n; off += 3000) {
    uint32_t chunk = n - off < 3000 ? n - off : 3000;
    ok = fat_ops()->read_at(fs, path, off, got + off, chunk) == (int)chunk;
  }
  ok = ok && memcmp(want, got, n) == 0;
  free(want);
  free(got);
  return ok;
}

static bool is_str(void *fs, const char *path, const char *s)
{
  uint32_t n = (uint32_t)strlen(s);
  uint8_t *got = malloc(n + 8);
  int is_dir = 0;
  uint32_t size = 0;
  bool ok = fat_ops()->stat(fs, path, &is_dir, &size) == 0 && !is_dir && size == n;
  ok = ok && fat_ops()->read_at(fs, path, 0, got, n + 8) == (int)n && memcmp(got, s, n) == 0;
  free(got);
  return ok;
}

static bool exists(void *fs, const char *path) { return fat_ops()->stat(fs, path, NULL, NULL) == 0; }

static bool is_dir_path(void *fs, const char *path)
{
  int d = 0;
  return fat_ops()->stat(fs, path, &d, NULL) == 0 && d;
}

static int count_dir(void *fs, const char *path)
{
  char name[64];
  int n = 0;
  while (fat_ops()->enum_dir(fs, path, n, name, NULL, NULL))
    n++;
  return n;
}

static bool dir_has(void *fs, const char *path, const char *want)
{
  char name[1100];
  for (int i = 0; fat_enum_dir_cap(fs, path, i, name, sizeof name, NULL, NULL); i++)
    if (streq(name, want))
      return true;
  return false;
}

static uint32_t free_clusters(void *fs)
{
  struct fat_statfs s;
  return fat_statfs(fs, &s) == 0 ? s.free_clusters : 0xFFFFFFFFu;
}

struct ck_ent {
  char path[800];
  uint8_t sfn[11], attr, ntres, ctenth;
  uint16_t cdate, ctime, mdate, mtime, adate;
  uint32_t cluster, size;
  bool lfn;
};

#define CK_MAX 4096
struct ck {
  const uint8_t *img;
  uint32_t base, bps, spc, rsvd, nfat, rootent, fatsz, tot, data0, nclus, root0, root_secs, rootc;
  int kind;
  uint32_t active;
  uint8_t *seen;
  int errs;
  char first[300];
  struct ck_ent *ents;
  int nents;
};

static struct ck g_ck;

static void ck_err(struct ck *c, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  if (!c->errs++)
    vsnprintf(c->first, sizeof c->first, fmt, ap);
  va_end(ap);
}

static const uint8_t *ck_at(const struct ck *c, uint64_t off) { return c->img + (size_t)c->base * 512 + off; }

static uint32_t ck_fat(const struct ck *c, uint32_t copy, uint32_t n)
{
  const uint8_t *f = ck_at(c, ((uint64_t)c->rsvd + (uint64_t)copy * c->fatsz) * c->bps);
  if (c->kind == 12) {
    uint32_t o = n + n / 2, v = (uint32_t)(f[o] | (f[o + 1] << 8));
    return n & 1 ? v >> 4 : v & 0xFFF;
  }
  if (c->kind == 16)
    return (uint32_t)(f[n * 2] | (f[n * 2 + 1] << 8));
  return (f[n * 4] | (f[n * 4 + 1] << 8) | (f[n * 4 + 2] << 16) | ((uint32_t)f[n * 4 + 3] << 24)) & 0x0FFFFFFF;
}

static bool ck_eoc(const struct ck *c, uint32_t v)
{
  return v >= (c->kind == 12 ? 0xFF8u : c->kind == 16 ? 0xFFF8u : 0x0FFFFFF8u);
}

static const uint8_t *ck_clus(const struct ck *c, uint32_t n)
{
  return ck_at(c, ((uint64_t)c->data0 + (uint64_t)(n - 2) * c->spc) * c->bps);
}

static uint8_t ck_sum(const uint8_t *s)
{
  uint8_t x = 0;
  for (int i = 0; i < 11; i++)
    x = (uint8_t)(((x & 1) << 7) + (x >> 1) + s[i]);
  return x;
}

static long ck_chain(struct ck *c, uint32_t first, const char *what)
{
  long n = 0;
  uint32_t cl = first;
  for (;;) {
    if (cl < 2 || cl > c->nclus + 1) {
      ck_err(c, "%s: cluster %u out of range", what, cl);
      return -1;
    }
    if (c->seen[cl]) {
      ck_err(c, "%s: cluster %u cross-linked or looped", what, cl);
      return -1;
    }
    c->seen[cl] = 1;
    n++;
    uint32_t nx = ck_fat(c, c->active, cl);
    if (ck_eoc(c, nx))
      return n;
    cl = nx;
  }
}

static void utf8_append(char *dst, size_t cap, const uint16_t *u, int n)
{
  size_t o = strlen(dst);
  for (int i = 0; i < n && o + 5 < cap; i++) {
    uint32_t ch = u[i];
    if (ch >= 0xD800 && ch < 0xDC00 && i + 1 < n) {
      ch = 0x10000 + ((ch - 0xD800) << 10) + (u[i + 1] - 0xDC00);
      i++;
    }
    if (ch < 0x80) {
      dst[o++] = (char)ch;
    } else if (ch < 0x800) {
      dst[o++] = (char)(0xC0 | (ch >> 6));
      dst[o++] = (char)(0x80 | (ch & 0x3F));
    } else if (ch < 0x10000) {
      dst[o++] = (char)(0xE0 | (ch >> 12));
      dst[o++] = (char)(0x80 | ((ch >> 6) & 0x3F));
      dst[o++] = (char)(0x80 | (ch & 0x3F));
    } else {
      dst[o++] = (char)(0xF0 | (ch >> 18));
      dst[o++] = (char)(0x80 | ((ch >> 12) & 0x3F));
      dst[o++] = (char)(0x80 | ((ch >> 6) & 0x3F));
      dst[o++] = (char)(0x80 | (ch & 0x3F));
    }
  }
  dst[o] = 0;
}

/* dotdot: cluster the ".." entry must hold (0 for children of the root). */
static void ck_dir(struct ck *c, uint32_t dir, uint32_t dotdot, const char *path, int depth)
{
  bool fixed_root = dir == 0;
  bool is_root = fixed_root || (c->kind == 32 && dir == c->rootc);
  uint8_t *buf;
  size_t len;
  if (fixed_root) {
    len = (size_t)c->root_secs * c->bps;
    buf = malloc(len);
    memcpy(buf, ck_at(c, (uint64_t)c->root0 * c->bps), len);
  } else {
    long n = ck_chain(c, dir, path);
    if (n < 0)
      return;
    size_t cb = (size_t)c->spc * c->bps;
    len = (size_t)n * cb;
    buf = malloc(len);
    uint32_t cl = dir;
    for (long i = 0; i < n; i++) {
      memcpy(buf + (size_t)i * cb, ck_clus(c, cl), cb);
      cl = ck_fat(c, c->active, cl);
    }
  }
  if (depth > 40) {
    ck_err(c, "directory nesting too deep");
    free(buf);
    return;
  }

  static const int lo[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
  uint16_t lname[300];
  int lseq = 0, ltotal = 0;
  uint8_t lcrc = 0;
  bool ended = false;
  uint8_t (*shorts)[11] = malloc((len / 32 + 1) * 11);
  int nshorts = 0, nlongs = 0;
  char **longs = calloc(len / 32 + 1, sizeof(char *));

  for (size_t o = 0; o + 32 <= len; o += 32) {
    const uint8_t *e = buf + o;
    if (ended) {
      if (e[0] != 0)
        ck_err(c, "%s: data after the end marker", path);
      continue;
    }
    if (e[0] == 0) {
      ended = true;
      if (lseq)
        ck_err(c, "%s: LFN run without a short entry", path);
      continue;
    }
    if (e[0] == 0xE5) {
      lseq = 0;
      continue;
    }
    if ((e[11] & 0x3F) == 0x0F) {
      uint32_t ord = e[0] & 0x3F;
      if (ord < 1 || ord > 20 || e[12] || e[26] || e[27]) {
        ck_err(c, "%s: bad LFN fields", path);
        lseq = 0;
        continue;
      }
      if (e[0] & 0x40) {
        if (lseq)
          ck_err(c, "%s: LFN run restarted", path);
        ltotal = lseq = (int)ord;
        lcrc = e[13];
        memset(lname, 0xFF, sizeof lname);
      } else if (!lseq || (int)ord != lseq - 1 || e[13] != lcrc) {
        ck_err(c, "%s: bad LFN ordinal or checksum (ord %u)", path, ord);
        lseq = 0;
        continue;
      } else {
        lseq = (int)ord;
      }
      for (int i = 0; i < 13; i++)
        lname[(ord - 1) * 13 + i] = (uint16_t)(e[lo[i]] | (e[lo[i] + 1] << 8));
      continue;
    }
    if (e[11] & 0x08) {
      lseq = 0;
      continue;
    }

    int nl = 0;
    bool has_lfn = false;
    if (lseq) {
      if (lseq != 1)
        ck_err(c, "%s: incomplete LFN run", path);
      if (lcrc != ck_sum(e))
        ck_err(c, "%s: LFN checksum mismatch for %.11s", path, e);
      while (nl < ltotal * 13 && lname[nl] != 0)
        nl++;
      for (int i = nl + 1; i < ltotal * 13; i++)
        if (lname[i] != 0xFFFF)
          ck_err(c, "%s: LFN padding is not 0xFFFF", path);
      if (nl == 0 || nl > 255 || (nl + 12) / 13 != ltotal)
        ck_err(c, "%s: LFN length %d does not match %d slots", path, nl, ltotal);
      has_lfn = true;
      lseq = 0;
    }
    uint32_t cluster = (uint32_t)(e[26] | (e[27] << 8));
    if (c->kind == 32)
      cluster |= (uint32_t)(e[20] | (e[21] << 8)) << 16;
    uint32_t size = (uint32_t)e[28] | ((uint32_t)e[29] << 8) | ((uint32_t)e[30] << 16) | ((uint32_t)e[31] << 24);
    bool is_dir = e[11] & 0x10;

    if (!is_root && o == 0) {
      if (memcmp(e, ".          ", 11) != 0 || !is_dir || cluster != dir)
        ck_err(c, "%s: bad '.' entry", path);
      continue;
    }
    if (!is_root && o == 32) {
      if (memcmp(e, "..         ", 11) != 0 || !is_dir || cluster != dotdot)
        ck_err(c, "%s: bad '..' entry (%u, want %u)", path, cluster, dotdot);
      continue;
    }

    for (int i = 0; i < nshorts; i++)
      if (memcmp(shorts[i], e, 11) == 0)
        ck_err(c, "%s: duplicate short name %.11s", path, e);
    memcpy(shorts[nshorts++], e, 11);
    for (int i = 0; i < 11; i++)
      if (e[i] < 0x20 && !(i == 0 && e[0] == 0x05))
        ck_err(c, "%s: control character in short name", path);

    char full[800];
    snprintf(full, sizeof full, "%s/", path);
    if (has_lfn) {
      utf8_append(full, sizeof full, lname, nl);
    } else {
      char t[16];
      int k = 0;
      for (int i = 0; i < 8 && e[i] != ' '; i++)
        t[k++] = (char)((e[12] & 0x08) && e[i] >= 'A' && e[i] <= 'Z' ? e[i] + 32 : e[i]);
      if (e[8] != ' ') {
        t[k++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++)
          t[k++] = (char)((e[12] & 0x10) && e[i] >= 'A' && e[i] <= 'Z' ? e[i] + 32 : e[i]);
      }
      t[k] = 0;
      strcat(full, t);
    }
    char *key = strdup(full);
    for (char *p = key; *p; p++)
      if (*p >= 'A' && *p <= 'Z')
        *p = (char)(*p + 32);
    for (int i = 0; i < nlongs; i++)
      if (strcmp(longs[i], key) == 0)
        ck_err(c, "duplicate name %s", full);
    longs[nlongs++] = key;

    if (c->nents < CK_MAX) {
      struct ck_ent *r = &c->ents[c->nents++];
      memset(r, 0, sizeof *r);
      snprintf(r->path, sizeof r->path, "%s", full);
      memcpy(r->sfn, e, 11);
      r->attr = e[11];
      r->ntres = e[12];
      r->ctenth = e[13];
      r->ctime = (uint16_t)(e[14] | (e[15] << 8));
      r->cdate = (uint16_t)(e[16] | (e[17] << 8));
      r->adate = (uint16_t)(e[18] | (e[19] << 8));
      r->mtime = (uint16_t)(e[22] | (e[23] << 8));
      r->mdate = (uint16_t)(e[24] | (e[25] << 8));
      r->cluster = cluster;
      r->size = size;
      r->lfn = has_lfn;
    }

    if (is_dir) {
      if (size != 0)
        ck_err(c, "%s: directory with size %u", full, size);
      if (cluster < 2)
        ck_err(c, "%s: directory without a cluster", full);
      else
        ck_dir(c, cluster, is_root ? 0 : dir, full, depth + 1);
    } else if (cluster == 0) {
      if (size != 0)
        ck_err(c, "%s: size %u without clusters", full, size);
    } else {
      long n = ck_chain(c, cluster, full);
      uint64_t cb = (uint64_t)c->spc * c->bps;
      if (n >= 0 && (uint64_t)n != ((uint64_t)size + cb - 1) / cb)
        ck_err(c, "%s: %ld clusters for size %u", full, n, size);
    }
  }
  for (int i = 0; i < nlongs; i++)
    free(longs[i]);
  free(longs);
  free(shorts);
  free(buf);
}

#define CK_SINGLE_FAT 1

/* Structural error count of the volume at `base`; entries stay available via ck_find(). */
static int check_volume(const uint8_t *img, uint32_t base, int flags)
{
  struct ck *c = &g_ck;
  free(c->seen);
  free(c->ents);
  memset(c, 0, sizeof *c);
  c->img = img;
  c->base = base;
  c->ents = malloc(CK_MAX * sizeof *c->ents);
  const uint8_t *bs = img + (size_t)base * 512;
  c->bps = (uint32_t)(bs[11] | (bs[12] << 8));
  c->spc = bs[13];
  c->rsvd = (uint32_t)(bs[14] | (bs[15] << 8));
  c->nfat = bs[16];
  c->rootent = (uint32_t)(bs[17] | (bs[18] << 8));
  uint32_t fat16 = (uint32_t)(bs[22] | (bs[23] << 8));
  c->fatsz = fat16 ? fat16 : (uint32_t)(bs[36] | (bs[37] << 8) | (bs[38] << 16) | ((uint32_t)bs[39] << 24));
  uint32_t t16 = (uint32_t)(bs[19] | (bs[20] << 8));
  c->tot = t16 ? t16 : (uint32_t)(bs[32] | (bs[33] << 8) | (bs[34] << 16) | ((uint32_t)bs[35] << 24));
  c->root_secs = (c->rootent * 32 + c->bps - 1) / c->bps;
  c->root0 = c->rsvd + c->nfat * c->fatsz;
  c->data0 = c->root0 + c->root_secs;
  c->nclus = (c->tot - c->data0) / c->spc;
  c->kind = fat16 == 0 ? 32 : c->nclus >= 4085 ? 16 : 12;
  c->active = (flags & CK_SINGLE_FAT) ? 1 : 0;
  c->seen = calloc(c->nclus + 2, 1);
  if (bs[510] != 0x55 || bs[511] != 0xAA)
    ck_err(c, "boot signature");

  uint32_t fat_entries = 0, free_n = 0;
  if (c->kind == 32) {
    c->rootc = (uint32_t)(bs[44] | (bs[45] << 8) | (bs[46] << 16) | ((uint32_t)bs[47] << 24));
    ck_dir(c, c->rootc, 0, "", 0);
  } else {
    ck_dir(c, 0, 0, "", 0);
  }
  (void)fat_entries;

  for (uint32_t n = 2; n < c->nclus + 2; n++) {
    uint32_t v = ck_fat(c, c->active, n);
    if (v == 0)
      free_n++;
    if (v == 0 && c->seen[n])
      ck_err(c, "cluster %u in use but marked free", n);
    if (v != 0 && !c->seen[n])
      ck_err(c, "cluster %u allocated but unreferenced (orphan)", n);
  }
  uint32_t media = ck_fat(c, c->active, 0) & 0xFF;
  if (media != bs[21])
    ck_err(c, "FAT[0] media byte %02x != BPB %02x", media, bs[21]);
  if (c->kind != 12) {
    uint32_t f1 = ck_fat(c, c->active, 1);
    uint32_t clean = c->kind == 16 ? 0x8000u : 0x08000000u;
    if (!(f1 & clean))
      ck_err(c, "volume marked dirty in FAT[1]");
  }
  if (bs[c->kind == 32 ? 65 : 37] & 1)
    ck_err(c, "volume marked dirty in the boot sector");

  if (!(flags & CK_SINGLE_FAT)) {
    for (uint32_t f = 1; f < c->nfat; f++)
      if (memcmp(ck_at(c, (uint64_t)c->rsvd * c->bps), ck_at(c, ((uint64_t)c->rsvd + (uint64_t)f * c->fatsz) * c->bps),
                 (size_t)c->fatsz * c->bps) != 0)
        ck_err(c, "FAT copy %u differs from FAT copy 0", f);
  }
  if (c->kind == 32) {
    uint32_t fsi = (uint32_t)(bs[48] | (bs[49] << 8));
    const uint8_t *p = ck_at(c, (uint64_t)fsi * c->bps);
    uint32_t fc = (uint32_t)(p[488] | (p[489] << 8) | (p[490] << 16) | ((uint32_t)p[491] << 24));
    uint32_t nf = (uint32_t)(p[492] | (p[493] << 8) | (p[494] << 16) | ((uint32_t)p[495] << 24));
    if (p[0] != 'R' || p[1] != 'R' || p[2] != 'a' || p[3] != 'A' || p[484] != 'r' || p[485] != 'r' || p[486] != 'A' ||
        p[487] != 'a' || p[510] != 0x55 || p[511] != 0xAA)
      ck_err(c, "FSInfo signatures");
    if (fc != 0xFFFFFFFFu && fc != free_n)
      ck_err(c, "FSInfo free count %u, actual %u", fc, free_n);
    if (nf != 0xFFFFFFFFu && (nf < 2 || nf > c->nclus + 1))
      ck_err(c, "FSInfo next-free hint %u out of range", nf);
  }
  return c->errs;
}

static const struct ck_ent *ck_find(const char *path)
{
  for (int i = 0; i < g_ck.nents; i++)
    if (strcmp(g_ck.ents[i].path, path) == 0)
      return &g_ck.ents[i];
  return NULL;
}

#define EXPECT_CLEAN(base)                                                                                             \
  do {                                                                                                                 \
    int _e = check_volume(g_img, (base), 0);                                                                           \
    if (_e)                                                                                                            \
      printf("  checker: %d errors, first: %s\n", _e, g_ck.first);                                                     \
    expect(_e == 0);                                                                                                   \
  } while (0)

static char g_tmp[256];

static bool have_cmd(const char *cmd)
{
  char line[300];
  snprintf(line, sizeof line, "command -v %s >/dev/null 2>&1", cmd);
  return system(line) == 0;
}

static void tmp_init(void)
{
  if (g_tmp[0])
    return;
  const char *base = getenv("TMPDIR");
  snprintf(g_tmp, sizeof g_tmp, "%s/owos-fat-XXXXXX", base && *base ? base : "/tmp");
  if (!mkdtemp(g_tmp))
    g_tmp[0] = 0;
}

static void tmp_path(char *out, size_t cap, const char *name) { snprintf(out, cap, "%s/%s", g_tmp, name); }

static void img_save(const char *path)
{
  FILE *f = fopen(path, "wb");
  fwrite(g_img, 1, g_img_bytes, f);
  fclose(f);
}

/* 1 = clean, 0 = fsck_msdos reported errors, -1 = tool not available. */
static int host_fsck(void)
{
  static int avail = -1;
  if (avail < 0)
    avail = have_cmd("fsck_msdos");
  tmp_init();
  if (!avail || !g_tmp[0])
    return -1;
  char img[300], cmd[700], out[8192];
  tmp_path(img, sizeof img, "fsck.img");
  img_save(img);
  snprintf(cmd, sizeof cmd, "fsck_msdos -n '%s' 2>&1", img);
  FILE *p = popen(cmd, "r");
  size_t n = fread(out, 1, sizeof out - 1, p);
  out[n] = 0;
  int rc = pclose(p);
  unlink(img);
  if (rc != 0)
    printf("  fsck_msdos rc=%d:\n%s\n", rc, out);
  return rc == 0;
}

#define EXPECT_FSCK()                                                                                                  \
  do {                                                                                                                 \
    int _f = host_fsck();                                                                                              \
    expect(_f != 0);                                                                                                   \
  } while (0)

static int run(const char *fmt, ...)
{
  char cmd[1500];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(cmd, sizeof cmd, fmt, ap);
  va_end(ap);
  return system(cmd);
}

static void text_utf(void)
{
  uint16_t u[300];
  expect(utf8_to_u16("abc", 3, u, 255) == 3 && u[2] == 'c');
  expect(utf8_to_u16("\xC3\xA9", 2, u, 255) == 1 && u[0] == 0xE9);
  expect(utf8_to_u16("\xE6\x97\xA5", 3, u, 255) == 1 && u[0] == 0x65E5);
  expect(utf8_to_u16("\xF0\x9F\x98\x80", 4, u, 255) == 2 && u[0] == 0xD83D && u[1] == 0xDE00);
  expect(utf8_to_u16("\xC0\x80", 2, u, 255) == -1);
  expect(utf8_to_u16("\xED\xA0\x80", 3, u, 255) == -1);
  expect(utf8_to_u16("\xF4\x90\x80\x80", 4, u, 255) == -1);
  expect(utf8_to_u16("\xE6\x97", 2, u, 255) == -1);
  expect(utf8_to_u16("\x80", 1, u, 255) == -1);
  expect(utf8_to_u16("abc", 3, u, 2) == -2);
  char out[32];
  uint16_t pair[2] = {0xD83D, 0xDE00};
  expect(u16_to_utf8(pair, 2, out, sizeof out) == 4 && memcmp(out, "\xF0\x9F\x98\x80", 5) == 0);
  uint16_t lone[1] = {0xD800};
  expect(u16_to_utf8(lone, 1, out, sizeof out) == 3 && memcmp(out, "\xEF\xBF\xBD", 4) == 0);
  expect(u16_to_utf8(pair, 2, out, 4) == -1);
}

static void text_fold(void)
{
  expect(ufold('A') == 'a' && ufold('z') == 'z' && ufold(0xC4) == 0xE4 && ufold(0xD7) == 0xD7);
  expect(ufold(0xDF) == 0xDF && ufold(0x178) == 0xFF && ufold(0x100) == 0x101 && ufold(0x139) == 0x13A);
  expect(ufold(0x3A9) == 0x3C9 && ufold(0x386) == 0x3AC && ufold(0x3C2) == 0x3C3);
  expect(ufold(0x414) == 0x434 && ufold(0x401) == 0x451 && ufold(0x4C0) == 0x4CF);
  expect(ufold(0x4E00) == 0x4E00);
}

static void text_fit83(void)
{
  uint8_t sfn[11], nt;
  uint16_t n1[20];
  int n = utf8_to_u16("readme.txt", 10, n1, 20);
  expect(fit_83(n1, (uint32_t)n, sfn, &nt) && nt == 0x18 && memcmp(sfn, "README  TXT", 11) == 0);
  n = utf8_to_u16("README.txt", 10, n1, 20);
  expect(fit_83(n1, (uint32_t)n, sfn, &nt) && nt == 0x10);
  n = utf8_to_u16("Readme.txt", 10, n1, 20);
  expect(!fit_83(n1, (uint32_t)n, sfn, &nt));
  n = utf8_to_u16("a.b.c", 5, n1, 20);
  expect(!fit_83(n1, (uint32_t)n, sfn, &nt));
  n = utf8_to_u16("toolongname.txt", 15, n1, 20);
  expect(!fit_83(n1, (uint32_t)n, sfn, &nt));
  n = utf8_to_u16("a b.txt", 7, n1, 20);
  expect(!fit_83(n1, (uint32_t)n, sfn, &nt));
  n = utf8_to_u16("NOEXT", 5, n1, 20);
  expect(fit_83(n1, (uint32_t)n, sfn, &nt) && nt == 0 && memcmp(sfn, "NOEXT      ", 11) == 0);
  expect(lfn_sum((const uint8_t *)"FOO     BAR") == lfn_sum((const uint8_t *)"FOO     BAR"));
  expect(lfn_sum((const uint8_t *)"FOO     BAR") != lfn_sum((const uint8_t *)"FOO     BAS"));
}

static void t_text(void)
{
  test("UTF-8 and UTF-16 conversion", text_utf);
  test("case folding", text_fold);
  test("8.3 fitting", text_fit83);
}

static int case_kind;
static uint32_t case_secs;

static struct format_case {
  int kind;
  uint32_t secs, bps;
  const char *type;
} format_cases[] = {
    {12, 2880, 512, "FAT12"},    {0, 2880, 512, "FAT12"},      {12, 720, 512, "FAT12"},    {16, 40960, 512, "FAT16"},
    {0, 131072, 512, "FAT16"},   {16, 1000000, 512, "FAT16"},  {32, 100000, 512, "FAT32"}, {0, 8400000, 512, "FAT32"},
    {16, 131072, 4096, "FAT16"}, {32, 1048576, 4096, "FAT32"},
};
static const struct format_case *cur_format;

static void format_one(void)
{
  const struct format_case *c = cur_format;
  expect(fmt_dev(c->kind, c->secs, c->bps, 0, "OWOSVOL") == 0);
  EXPECT_CLEAN(0);
  void *fs = mnt();
  expect(fs != NULL);
  if (!fs)
    return;
  expect_string(fat_ops()->type(fs), c->type);
  expect_string(fat_ops()->label(fs), "OWOSVOL");
  expect(count_dir(fs, "/") == 0);
  struct fat_statfs s;
  expect(fat_statfs(fs, &s) == 0);
  expect(s.free_clusters == s.total_clusters - (strcmp(c->type, "FAT32") == 0 ? 1 : 0));
  expect(s.bits == atoi(c->type + 3));
  fat_ops()->close(fs);
  if (c->secs < 2000000)
    EXPECT_FSCK();
}

static void format_geometry(void)
{
  expect(fmt_dev(32, 2000, 512, 0, NULL) == FAT_EINVAL);
  expect(fmt_dev(16, 2000, 512, 0, NULL) == FAT_EINVAL);
  expect(fmt_dev(12, 500000, 512, 1, NULL) == FAT_EINVAL);
  expect(fmt_dev(7, 20000, 512, 0, NULL) == FAT_EINVAL);
  expect(fmt_dev(16, 40960, 1000, 0, NULL) == FAT_EINVAL);
}

static void format_no_label(void)
{
  expect(fmt_dev(16, 40960, 512, 0, NULL) == 0);
  void *fs = mnt();
  expect_string(fat_ops()->label(fs), "");
  fat_ops()->close(fs);
}

static void format_fat32_backup(void)
{
  expect(fmt_dev(32, 100000, 512, 0, NULL) == 0);
  expect(memcmp(g_img, g_img + 6 * 512, 512) == 0);
  expect(memcmp(g_img + 512, g_img + 7 * 512, 512) == 0);
  void *fs = mnt();
  expect(load_le32(g_img + 512 + 488) == free_clusters(fs) && free_clusters(fs) > 60000);
  fat_ops()->close(fs);
}

static void t_format(void)
{
  for (unsigned i = 0; i < sizeof format_cases / sizeof format_cases[0]; i++) {
    char name[80];
    snprintf(name, sizeof name, "format kind %d, %u sectors, %u-byte sectors", format_cases[i].kind,
             format_cases[i].secs, format_cases[i].bps);
    cur_format = &format_cases[i];
    test(name, format_one);
  }
  test("format rejects impossible geometry", format_geometry);
  test("no label gives NO NAME to the BPB and an empty label to the mount", format_no_label);
  test("FAT32 format has a backup boot sector and FSInfo copy", format_fat32_backup);
}

static uint32_t case_bps, case_spc;

static void ops_body(void)
{
  int kind = case_kind;
  uint32_t secs = case_secs;
  expect(fmt_dev(kind, secs, case_bps, case_spc, "OPS") == 0);
  void *fs = mnt();
  expect(fs != NULL);
  if (!fs)
    return;
  uint32_t base_free = free_clusters(fs);

  expect(fat_ops()->mkdir(fs, "/dir") == 0);
  expect(fat_ops()->mkdir(fs, "/dir/sub") == 0);
  expect(fat_ops()->mkdir(fs, "/dir") != 0);
  expect(put(fs, "/a.txt", "hello", 5) == 0);
  expect(put_seed(fs, "/dir/big.bin", 100000, 7) == 0);
  expect(put(fs, "/dir/sub/empty", "", 0) == 0);
  expect(put_seed(fs, "/dir/sub/mid.bin", 513, 9) == 0);
  expect(put(fs, "/nodir/x", "x", 1) != 0);
  expect(put(fs, "/dir", "x", 1) != 0);

  expect(is_str(fs, "/a.txt", "hello"));
  expect(is_seed(fs, "/dir/big.bin", 100000, 7));
  expect(is_seed(fs, "/dir/sub/mid.bin", 513, 9));
  expect(is_seed(fs, "/dir/sub/empty", 0, 0));
  expect(is_dir_path(fs, "/dir/sub") && is_dir_path(fs, "/dir/sub/") && is_dir_path(fs, "/") && is_dir_path(fs, ""));
  expect(is_dir_path(fs, "/dir/sub/..") && is_dir_path(fs, "/dir/./sub"));
  expect(count_dir(fs, "/") == 2 && count_dir(fs, "/dir") == 2 && count_dir(fs, "/dir/sub") == 2);
  expect(dir_has(fs, "/", "a.txt") && dir_has(fs, "/", "dir") && !dir_has(fs, "/dir", "."));

  uint8_t buf[2000];
  uint8_t want[2000];
  fill(want, sizeof want, 7);
  expect(fat_ops()->read_at(fs, "/dir/big.bin", 0, buf, 2000) == 2000 && memcmp(buf, want, 2000) == 0);
  expect(fat_ops()->read_at(fs, "/dir/big.bin", 99990, buf, 2000) == 10);
  expect(fat_ops()->read_at(fs, "/dir/big.bin", 100000, buf, 10) == 0);
  expect(fat_ops()->read_at(fs, "/dir/big.bin", 5000000, buf, 10) == 0);
  expect(fat_ops()->read_at(fs, "/dir", 0, buf, 10) < 0);
  expect(fat_ops()->read_at(fs, "/missing", 0, buf, 10) < 0);
  EXPECT_CLEAN(0);

  fs = remount(fs);
  expect(is_seed(fs, "/dir/big.bin", 100000, 7));

  expect(put(fs, "/a.txt", "a much longer replacement body", 30) == 0);
  expect(is_str(fs, "/a.txt", "a much longer replacement body"));
  expect(put(fs, "/a.txt", "tiny", 4) == 0);
  expect(is_str(fs, "/a.txt", "tiny"));
  expect(put_seed(fs, "/dir/big.bin", 3000, 11) == 0);
  expect(is_seed(fs, "/dir/big.bin", 3000, 11));
  expect(put(fs, "/a.txt", "", 0) == 0);
  expect(is_str(fs, "/a.txt", ""));
  EXPECT_CLEAN(0);

  expect(fat_ops()->del(fs, "/dir") != 0);
  expect(fat_ops()->del(fs, "/nothing") != 0);
  expect(fat_ops()->rmdir(fs, "/dir") != 0);
  expect(fat_ops()->rmdir(fs, "/a.txt") != 0);
  expect(fat_ops()->rmdir(fs, "/dir/sub") != 0);
  expect(fat_ops()->del(fs, "/dir/sub/mid.bin") == 0);
  expect(fat_ops()->del(fs, "/dir/sub/empty") == 0);
  expect(fat_ops()->rmdir(fs, "/dir/sub") == 0);
  expect(!exists(fs, "/dir/sub"));
  expect(fat_ops()->del(fs, "/dir/big.bin") == 0);
  expect(fat_ops()->del(fs, "/a.txt") == 0);
  expect(fat_ops()->rmdir(fs, "/dir") == 0);
  expect(count_dir(fs, "/") == 0);
  expect(free_clusters(fs) == base_free);
  EXPECT_CLEAN(0);
  fat_ops()->close(fs);
  if (secs < 2000000)
    EXPECT_FSCK();
}

static void ops_cycle(int kind, uint32_t secs, uint32_t bps, uint32_t spc, const char *tag)
{
  char name[100];
  snprintf(name, sizeof name, "basic operations: %s", tag);
  case_kind = kind;
  case_secs = secs;
  case_bps = bps;
  case_spc = spc;
  test(name, ops_body);
}

static void t_basic(void)
{
  ops_cycle(12, 2880, 512, 0, "FAT12 1.44M");
  ops_cycle(12, 2880, 512, 4, "FAT12 2 KiB clusters");
  ops_cycle(16, 40960, 512, 0, "FAT16");
  ops_cycle(16, 40960, 512, 1, "FAT16 512-byte clusters");
  ops_cycle(32, 100000, 512, 0, "FAT32");
  ops_cycle(32, 700000, 512, 8, "FAT32 4 KiB clusters");
  ops_cycle(16, 131072, 4096, 0, "FAT16 4096-byte sectors");
  ops_cycle(32, 1048576, 4096, 0, "FAT32 4096-byte sectors");
}

static void *names_fs;

static void names_create(void)
{
  expect(fmt_dev(case_kind, case_secs, 512, 0, NULL) == 0);
  void *fs = mnt();
  const char *names[] = {"A Long File Name.txt",
                         "\xC3\xBCn\xC3\xAF"
                         "c\xC3\xB6"
                         "d\xC3\xA9-\xC3\xB1"
                         "ame.txt",
                         "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E.txt",
                         "\xF0\x9F\x98\x80 smile.txt",
                         "ReadMe.TXT",
                         "lower.txt",
                         "UPPER.TXT",
                         "Mixed.txt",
                         "readme",
                         "SHOUT.txt",
                         ".hidden",
                         "archive.tar.gz",
                         "a b.txt",
                         "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82.txt",
                         "x"};
  for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
    char path[200];
    snprintf(path, sizeof path, "/%s", names[i]);
    expect(put_seed(fs, path, 100 + i, i) == 0);
  }
  fs = remount(fs);
  for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
    char path[200];
    snprintf(path, sizeof path, "/%s", names[i]);
    expect(is_seed(fs, path, 100 + i, i));
    expect(dir_has(fs, "/", names[i]));
  }
  EXPECT_CLEAN(0);
  expect(g_ck.nents == (int)(sizeof names / sizeof names[0]));

  const struct ck_ent *e = ck_find("/A Long File Name.txt");
  expect(e && e->lfn && memcmp(e->sfn, "ALONGF~1TXT", 11) == 0 && e->ntres == 0);
  e = ck_find("/lower.txt");
  expect(e && !e->lfn && memcmp(e->sfn, "LOWER   TXT", 11) == 0 && e->ntres == 0x18);
  e = ck_find("/UPPER.TXT");
  expect(e && !e->lfn && e->ntres == 0);
  e = ck_find("/SHOUT.txt");
  expect(e && !e->lfn && e->ntres == 0x10 && memcmp(e->sfn, "SHOUT   TXT", 11) == 0);
  e = ck_find("/readme");
  expect(e && !e->lfn && e->ntres == 0x08 && memcmp(e->sfn, "README     ", 11) == 0);
  e = ck_find("/Mixed.txt");
  expect(e && e->lfn && memcmp(e->sfn, "MIXED   TXT", 11) == 0 && e->ntres == 0);
  e = ck_find("/ReadMe.TXT");
  expect(e && e->lfn && memcmp(e->sfn, "README  TXT", 11) == 0);
  e = ck_find("/.hidden");
  expect(e && e->lfn && memcmp(e->sfn, "HIDDEN~1   ", 11) == 0);
  e = ck_find("/archive.tar.gz");
  expect(e && e->lfn && memcmp(e->sfn, "ARCHIV~1GZ ", 11) == 0);
  e = ck_find("/a b.txt");
  expect(e && e->lfn && memcmp(e->sfn, "AB~1    TXT", 11) == 0);
  e = ck_find("/\xC3\xBCn\xC3\xAF"
              "c\xC3\xB6"
              "d\xC3\xA9-\xC3\xB1"
              "ame.txt");
  expect(e && e->lfn && memcmp(e->sfn, "_N_C_D~1TXT", 11) == 0);
  e = ck_find("/\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E.txt");
  expect(e && e->lfn && memcmp(e->sfn, "___~1   TXT", 11) == 0);
  e = ck_find("/x");
  expect(e && !e->lfn && e->ntres == 0x08);
  names_fs = fs;
}

static void names_lookup(void)
{
  void *fs = names_fs;
  expect(exists(fs, "/readme.txt") && exists(fs, "/README.TXT") && exists(fs, "/README"));
  expect(exists(fs, "/LOWER.TXT") && exists(fs, "/Lower.Txt") && exists(fs, "/mixed.TXT"));
  expect(exists(fs, "/\xC3\x9CN\xC3\x8F"
                    "C\xC3\x96"
                    "D\xC3\x89-\xC3\x91"
                    "AME.TXT"));
  expect(exists(fs, "/\xD0\x9F\xD0\xA0\xD0\x98\xD0\x92\xD0\x95\xD0\xA2.TXT"));
  expect(!exists(fs, "/\xD0\x9F\xD0\xA0\xD0\x98\xD0\x92\xD0\x95\xD0\xA2.TXX"));
  expect(exists(fs, "/ALONGF~1.TXT") && exists(fs, "/alongf~1.txt"));
  expect(exists(fs, "/A LONG FILE NAME.TXT"));
  expect(!exists(fs, "/A LONG FILE NAM.TXT"));
  expect(put(fs, "/READme.txt", "second", 6) == 0);
  expect(is_str(fs, "/readme.txt", "second") && count_dir(fs, "/") == 15);
  EXPECT_CLEAN(0);
}

static void names_greek(void)
{
  void *fs = names_fs;
  expect(put(fs, "/\xCE\xA3\xCE\xBF\xCF\x86\xCE\xAF\xCE\xB1.txt", "g", 1) == 0);
  expect(exists(fs, "/\xCE\xA3\xCE\x9F\xCE\xA6\xCE\x8A\xCE\x91.TXT"));
  expect(exists(fs, "/\xCF\x83\xCE\xBF\xCF\x86\xCE\xAF\xCE\xB1.txt"));
  fat_ops()->close(fs);
  EXPECT_FSCK();
}

static void names_numeric_tails(void)
{
  expect(fmt_dev(16, 65536, 512, 0, NULL) == 0);
  void *fs = mnt();
  for (int i = 1; i <= 12; i++) {
    char path[80];
    snprintf(path, sizeof path, "/Long file name %02d.txt", i);
    expect(put_seed(fs, path, 10, (uint32_t)i) == 0);
  }
  EXPECT_CLEAN(0);
  expect(ck_find("/Long file name 01.txt") && memcmp(ck_find("/Long file name 01.txt")->sfn, "LONGFI~1TXT", 11) == 0);
  expect(ck_find("/Long file name 09.txt") && memcmp(ck_find("/Long file name 09.txt")->sfn, "LONGFI~9TXT", 11) == 0);
  expect(ck_find("/Long file name 10.txt") && memcmp(ck_find("/Long file name 10.txt")->sfn, "LONGF~10TXT", 11) == 0);
  expect(ck_find("/Long file name 12.txt") && memcmp(ck_find("/Long file name 12.txt")->sfn, "LONGF~12TXT", 11) == 0);
  expect(put(fs, "/longfi~1.txt", "own", 3) == 0);
  expect(is_str(fs, "/Long file name 01.txt", "own"));
  expect(count_dir(fs, "/") == 12);
  EXPECT_CLEAN(0);
  fat_ops()->close(fs);
}

static void names_rejected(void)
{
  expect(fmt_dev(16, 65536, 512, 0, NULL) == 0);
  void *fs = mnt();
  const char *bad[] = {"/a:b",    "/a?b",       "/a*b",       "/a<b",     "/a>b",       "/a|b",      "/a\"b",
                       "/a\\b",   "/trailing.", "/trailing ", "/dots...", "/tab\tname", "/ctl\x01x", "/bad\xC0\x80utf8",
                       "/bad\xFF"};
  for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    expect(put(fs, bad[i], "x", 1) != 0);
    expect(fat_ops()->mkdir(fs, bad[i]) != 0);
  }
  expect(put(fs, "/.", "x", 1) != 0 && put(fs, "/..", "x", 1) != 0 && put(fs, "/", "x", 1) != 0 &&
         put(fs, "", "x", 1) != 0);
  char longname[400];
  memset(longname, 'n', 257);
  longname[0] = '/';
  longname[257] = 0;
  expect(put(fs, longname, "x", 1) == FAT_EINVAL);
  longname[256] = 0;
  expect(put(fs, longname, "x", 1) == 0);
  expect(exists(fs, longname));
  char name[1100];
  expect(fat_enum_dir_cap(fs, "/", 0, name, sizeof name, NULL, NULL) == 1 && strlen(name) == 255);
  expect(fat_ops()->enum_dir(fs, "/", 0, name, NULL, NULL) == 1 && strlen(name) < 64);
  char wide[900];
  strcpy(wide, "/");
  for (int i = 0; i < 255; i++)
    strcat(wide, "\xE6\x97\xA5");
  expect(put(fs, wide, "x", 1) == 0 && exists(fs, wide));
  expect(dir_has(fs, "/", wide + 1));
  strcat(wide, "\xE6\x97\xA5");
  expect(put(fs, wide, "x", 1) == FAT_EINVAL);
  EXPECT_CLEAN(0);
  expect(g_ck.nents == 2);
  fat_ops()->close(fs);
  EXPECT_FSCK();
}

static void t_names(void)
{
  static const int kinds[] = {12, 16, 32};
  static const uint32_t sizes[] = {8192, 65536, 100000};
  for (int k = 0; k < 3; k++) {
    char title[80];
    snprintf(title, sizeof title, "long and unicode names on FAT%d", kinds[k]);
    case_kind = kinds[k];
    case_secs = sizes[k];
    test(title, names_create);
    test("case-insensitive lookup incl. Latin-1, Greek and Cyrillic", names_lookup);
    test("case-insensitive lookup on Greek", names_greek);
  }
  test("numeric tails and short-name collisions", names_numeric_tails);
  test("illegal and over-long names are rejected", names_rejected);
}

static void dir_fat16_root_limit(void)
{
  expect(fmt_dev(16, 40960, 512, 0, NULL) == 0);
  void *fs = mnt();
  int made = 0, rc = 0;
  for (int i = 0; i < 600 && rc == 0; i++) {
    char p[40];
    snprintf(p, sizeof p, "/F%05d.TXT", i);
    rc = put_seed(fs, p, 10, (uint32_t)i);
    if (rc == 0)
      made++;
  }
  expect(made == 512 && rc == FAT_ENOSPC);
  expect(count_dir(fs, "/") == 512);
  expect(is_seed(fs, "/F00000.TXT", 10, 0) && is_seed(fs, "/F00511.TXT", 10, 511));
  expect(fat_ops()->mkdir(fs, "/DIR") != 0);
  expect(fat_ops()->del(fs, "/F00100.TXT") == 0);
  expect(fat_ops()->mkdir(fs, "/DIR") == 0);
  expect(put(fs, "/DIR/in.txt", "x", 1) == 0);
  EXPECT_CLEAN(0);
  fat_ops()->close(fs);
  EXPECT_FSCK();
}

static void dir_fat12_root_limit(void)
{
  expect(fmt_dev(12, 2880, 512, 0, NULL) == 0);
  void *fs = mnt();
  int made = 0, rc = 0;
  for (int i = 0; i < 300 && rc == 0; i++) {
    char p[40];
    snprintf(p, sizeof p, "/G%04d", i);
    rc = put(fs, p, "", 0);
    if (rc == 0)
      made++;
  }
  expect(made == 224 && rc == FAT_ENOSPC);
  EXPECT_CLEAN(0);
  fat_ops()->close(fs);
  EXPECT_FSCK();
}

static void dir_long_names_fill_root(void)
{
  expect(fmt_dev(16, 40960, 512, 0, NULL) == 0);
  void *fs = mnt();
  int made = 0, rc = 0;
  for (int i = 0; i < 300 && rc == 0; i++) {
    char p[80];
    snprintf(p, sizeof p, "/A fairly long file name number %04d.txt", i);
    rc = put(fs, p, "", 0);
    if (rc == 0)
      made++;
  }
  expect(made == 512 / 4 && rc == FAT_ENOSPC);
  EXPECT_CLEAN(0);
  fat_ops()->close(fs);
}

static void dir_subdir_growth(void)
{
  expect(fmt_dev(case_kind, case_secs, 512, 1, NULL) == 0);
  void *fs = mnt();
  expect(fat_ops()->mkdir(fs, "/many") == 0);
  int n = case_kind == 12 ? 150 : 600;
  for (int i = 0; i < n; i++) {
    char p[120];
    snprintf(p, sizeof p, "/many/Entry with a long name %04d.txt", i);
    if (put_seed(fs, p, 5, (uint32_t)i) != 0) {
      expect(false);
      break;
    }
  }
  expect(count_dir(fs, "/many") == n);
  fs = remount(fs);
  expect(count_dir(fs, "/many") == n);
  expect(is_seed(fs, "/many/Entry with a long name 0000.txt", 5, 0));
  expect(is_seed(fs, "/many/Entry with a long name 0149.txt", 5, 149));
  EXPECT_CLEAN(0);
  for (int i = 0; i < n; i += 2) {
    char p[120];
    snprintf(p, sizeof p, "/many/Entry with a long name %04d.txt", i);
    expect(fat_ops()->del(fs, p) == 0);
  }
  expect(count_dir(fs, "/many") == n / 2);
  for (int i = 0; i < n / 2; i++) {
    char p[120];
    snprintf(p, sizeof p, "/many/Refill %04d.txt", i);
    expect(put_seed(fs, p, 5, (uint32_t)i) == 0);
  }
  expect(count_dir(fs, "/many") == n);
  EXPECT_CLEAN(0);
  fat_ops()->close(fs);
  EXPECT_FSCK();
}

static void t_dir_growth(void)
{
  test("FAT16 root directory is fixed at 512 entries", dir_fat16_root_limit);
  test("FAT12 root directory limit", dir_fat12_root_limit);
  test("long names use several slots and fill the fixed root sooner", dir_long_names_fill_root);

  static const int kinds[] = {12, 16, 32};
  static const uint32_t sizes[] = {4000, 65536, 100000};
  for (int k = 0; k < 3; k++) {
    char title[80];
    snprintf(title, sizeof title, "subdirectory grows across clusters on FAT%d", kinds[k]);
    case_kind = kinds[k];
    case_secs = sizes[k];
    test(title, dir_subdir_growth);
  }
}

static void t_deep(void)
{
  expect(fmt_dev(32, 100000, 512, 0, NULL) == 0);
  void *fs = mnt();
  char path[2000] = "";
  for (int i = 0; i < 30; i++) {
    char comp[40];
    snprintf(comp, sizeof comp, "/level number %d", i);
    strcat(path, comp);
    expect(fat_ops()->mkdir(fs, path) == 0);
  }
  strcat(path, "/leaf.txt");
  expect(put(fs, path, "deep", 4) == 0);
  fs = remount(fs);
  expect(is_str(fs, path, "deep"));
  EXPECT_CLEAN(0);
  char up[2100];
  snprintf(up, sizeof up, "%s/../../level number 28/leaf.txt", path - 0);
  path[strlen(path) - strlen("/leaf.txt")] = 0;
  snprintf(up, sizeof up, "%s/../leaf2.txt", path);
  expect(put(fs, up, "up", 2) == 0);
  expect(
      is_str(fs,
             "/level number 0/level number 1/level number 2/level number 3/level number 4/level number 5/level number "
             "6/level number 7/level number 8/level number 9/level number 10/level number 11/level number 12/level "
             "number 13/level number 14/level number 15/level number 16/level number 17/level number 18/level number "
             "19/level number 20/level number 21/level number 22/level number 23/level number 24/level number "
             "25/level number 26/level number 27/level number 28/leaf2.txt",
             "up"));
  fat_ops()->close(fs);
  EXPECT_FSCK();
}

static void fragmented_files(void)
{
  expect(fmt_dev(case_kind, case_secs, 512, 1, NULL) == 0);
  void *fs = mnt();
  for (int i = 0; i < 100; i++) {
    char p[40];
    snprintf(p, sizeof p, "/s%03d.bin", i);
    expect(put_seed(fs, p, 700 + (uint32_t)i, (uint32_t)i) == 0);
  }
  for (int i = 0; i < 100; i += 2) {
    char p[40];
    snprintf(p, sizeof p, "/s%03d.bin", i);
    expect(fat_ops()->del(fs, p) == 0);
  }
  expect(put_seed(fs, "/big.bin", 60000, 99) == 0);
  expect(is_seed(fs, "/big.bin", 60000, 99));
  fs = remount(fs);
  expect(is_seed(fs, "/big.bin", 60000, 99));
  for (int i = 1; i < 100; i += 2) {
    char p[40];
    snprintf(p, sizeof p, "/s%03d.bin", i);
    expect(is_seed(fs, p, 700 + (uint32_t)i, (uint32_t)i));
  }
  uint8_t *want = malloc(60000), *got = malloc(60000);
  fill(want, 60000, 99);
  for (int i = 0; i < 300; i++) {
    uint32_t off = rnd() % 60000, len = rnd() % 3000 + 1;
    int r = fat_ops()->read_at(fs, "/big.bin", off, got, len);
    uint32_t exp_n = off + len > 60000 ? 60000 - off : len;
    expect(r == (int)exp_n && memcmp(got, want + off, exp_n) == 0);
  }
  free(want);
  free(got);
  EXPECT_CLEAN(0);
  fat_ops()->close(fs);
  EXPECT_FSCK();
}

static void fat12_straddle(void)
{
  expect(fmt_dev(12, 2880, 512, 1, NULL) == 0);
  void *fs = mnt();
  expect(put_seed(fs, "/long.bin", 400 * 512, 5) == 0);
  expect(put_seed(fs, "/second.bin", 300 * 512, 6) == 0);
  fs = remount(fs);
  expect(is_seed(fs, "/long.bin", 400 * 512, 5) && is_seed(fs, "/second.bin", 300 * 512, 6));
  expect(fat_ops()->del(fs, "/long.bin") == 0);
  expect(put_seed(fs, "/third.bin", 350 * 512, 8) == 0);
  expect(is_seed(fs, "/third.bin", 350 * 512, 8) && is_seed(fs, "/second.bin", 300 * 512, 6));
  EXPECT_CLEAN(0);
  fat_ops()->close(fs);
  EXPECT_FSCK();
}

static void t_fragmentation(void)
{
  static const int kinds[] = {12, 16, 32};
  static const uint32_t sizes[] = {4000, 65536, 100000};
  for (int k = 0; k < 3; k++) {
    char title[80];
    snprintf(title, sizeof title, "fragmented files on FAT%d", kinds[k]);
    case_kind = kinds[k];
    case_secs = sizes[k];
    test(title, fragmented_files);
  }
  test("FAT12 entries straddling FAT sector boundaries", fat12_straddle);
}

static void *wt_fs;

static void write_at_extends(void)
{
  expect(fmt_dev(16, 40960, 512, 1, NULL) == 0);
  void *fs = mnt();
  expect(put(fs, "/f.bin", "", 0) == 0);
  uint8_t a[700];
  fill(a, sizeof a, 3);
  expect(fat_write_at(fs, "/f.bin", 0, a, 700) == 700);
  expect(is_seed(fs, "/f.bin", 700, 3));
  expect(fat_write_at(fs, "/f.bin", 700, a, 300) == 300);
  uint8_t full[3000];
  uint8_t got[3000];
  memcpy(full, a, 700);
  memcpy(full + 700, a, 300);
  expect(fat_write_at(fs, "/f.bin", 2000, a, 100) == 100);
  memset(full + 1000, 0, 1000);
  memcpy(full + 2000, a, 100);
  expect(fat_ops()->read_at(fs, "/f.bin", 0, got, 3000) == 2100 && memcmp(got, full, 2100) == 0);
  expect(fat_write_at(fs, "/f.bin", 100, "XYZ", 3) == 3);
  memcpy(full + 100, "XYZ", 3);
  expect(fat_ops()->read_at(fs, "/f.bin", 0, got, 3000) == 2100 && memcmp(got, full, 2100) == 0);
  expect(fat_write_at(fs, "/missing", 0, a, 1) < 0);
  expect(fat_write_at(fs, "/", 0, a, 1) < 0);
  EXPECT_CLEAN(0);
  wt_fs = fs;
}

static void write_at_gap_zeros(void)
{
  void *fs = wt_fs;
  expect(put_seed(fs, "/junk.bin", 8000, 77) == 0);
  expect(fat_ops()->del(fs, "/junk.bin") == 0);
  expect(put(fs, "/z.bin", "", 0) == 0);
  expect(fat_write_at(fs, "/z.bin", 6000, "end", 3) == 3);
  uint8_t *z = calloc(6003, 1), *zg = malloc(6003);
  memcpy(z + 6000, "end", 3);
  expect(fat_ops()->read_at(fs, "/z.bin", 0, zg, 6003) == 6003 && memcmp(z, zg, 6003) == 0);
  free(z);
  free(zg);
  EXPECT_CLEAN(0);
}

static void truncate_resizes(void)
{
  void *fs = wt_fs;
  uint32_t before = free_clusters(fs);
  expect(put_seed(fs, "/t.bin", 20000, 4) == 0);
  uint32_t used = before - free_clusters(fs);
  expect(used == 40);
  expect(fat_truncate(fs, "/t.bin", 5000) == 0);
  expect(is_seed(fs, "/t.bin", 5000, 4));
  expect(before - free_clusters(fs) == 10);
  uint8_t want[5000], g2[5000];
  fill(want, 5000, 4);
  expect(fat_ops()->read_at(fs, "/t.bin", 0, g2, 5000) == 5000 && memcmp(g2, want, 5000) == 0);
  expect(fat_truncate(fs, "/t.bin", 0) == 0);
  expect(before == free_clusters(fs));
  expect(is_str(fs, "/t.bin", ""));
  expect(fat_truncate(fs, "/t.bin", 3000) == 0);
  uint8_t zero[3000];
  memset(zero, 0, sizeof zero);
  expect(fat_ops()->read_at(fs, "/t.bin", 0, g2, 3000) == 3000 && memcmp(g2, zero, 3000) == 0);
  expect(fat_truncate(fs, "/t.bin", 100) == 0);
  expect(fat_truncate(fs, "/t.bin", 1000) == 0);
  expect(fat_ops()->read_at(fs, "/t.bin", 0, g2, 1000) == 1000 && memcmp(g2, zero, 1000) == 0);
  expect(fat_truncate(fs, "/nope", 1) < 0 && fat_truncate(fs, "/", 1) < 0);
  EXPECT_CLEAN(0);
}

static void file_size_limit(void)
{
  void *fs = wt_fs;
  uint8_t g2[1000];
  uint32_t fc = free_clusters(fs);
  expect(fat_write_at(fs, "/t.bin", 0xFFFFFFFFu, "xx", 2) == FAT_EFBIG);
  expect(fat_write_at(fs, "/t.bin", 0xFFFFFFFEu, "xx", 2) == FAT_EFBIG);
  expect(fat_write_at(fs, "/t.bin", 0xFFFFFFF0u, "0123456789abcde", 15) == FAT_ENOSPC);
  expect(fat_truncate(fs, "/t.bin", 0xFFFFFFFFu) == FAT_ENOSPC);
  expect(free_clusters(fs) == fc);
  expect(fat_ops()->read_at(fs, "/t.bin", 0, g2, 1000) == 1000);
  EXPECT_CLEAN(0);
  fat_ops()->close(fs);
  EXPECT_FSCK();
}

static void t_write_trunc(void)
{
  test("write_at extends, overwrites and zero-fills gaps", write_at_extends);
  test("a gap over stale cluster contents reads as zeros", write_at_gap_zeros);
  test("truncate shrinks and frees clusters, grows with zeros", truncate_resizes);
  test("files cannot exceed 4 GiB - 1 and failed growth leaves the file alone", file_size_limit);
}

static void *rn_fs;

static void rename_basic(void)
{
  expect(fmt_dev(32, 100000, 512, 1, NULL) == 0);
  void *fs = mnt();
  expect(fat_ops()->mkdir(fs, "/a") == 0 && fat_ops()->mkdir(fs, "/a/inner") == 0 && fat_ops()->mkdir(fs, "/b") == 0);
  expect(put_seed(fs, "/a/inner/file.txt", 2000, 1) == 0);
  expect(put_seed(fs, "/top.txt", 300, 2) == 0);
  expect(put_seed(fs, "/b/other.txt", 400, 3) == 0);

  expect(fat_rename(fs, "/top.txt", "/renamed to a long name.txt") == 0);
  expect(!exists(fs, "/top.txt") && is_seed(fs, "/renamed to a long name.txt", 300, 2));
  expect(fat_rename(fs, "/renamed to a long name.txt", "/short.txt") == 0);
  expect(is_seed(fs, "/short.txt", 300, 2));
  EXPECT_CLEAN(0);
  expect(ck_find("/short.txt") && !ck_find("/short.txt")->lfn);
  expect(fat_rename(fs, "/short.txt", "/b/moved \xC3\xA9.txt") == 0);
  expect(is_seed(fs, "/b/moved \xC3\xA9.txt", 300, 2) && !exists(fs, "/short.txt"));

  expect(fat_rename(fs, "/a/inner", "/b/inner2") == 0);
  expect(!exists(fs, "/a/inner") && is_seed(fs, "/b/inner2/file.txt", 2000, 1));
  EXPECT_CLEAN(0);
  expect(fat_rename(fs, "/b/inner2", "/inner3") == 0);
  expect(is_seed(fs, "/inner3/file.txt", 2000, 1));
  EXPECT_CLEAN(0);
  expect(fat_rename(fs, "/inner3", "/b/inner3/") == 0 && is_seed(fs, "/b/inner3/file.txt", 2000, 1));
  expect(fat_rename(fs, "/b/inner3", "/inner3") == 0);
  EXPECT_CLEAN(0);
  rn_fs = fs;
}

static void rename_rejects(void)
{
  void *fs = rn_fs;
  expect(fat_rename(fs, "/b", "/b/sub") == FAT_EINVAL);
  expect(fat_ops()->mkdir(fs, "/b/c") == 0 && fat_ops()->mkdir(fs, "/b/c/d") == 0);
  expect(fat_rename(fs, "/b", "/b/c/d/b") == FAT_EINVAL);
  expect(fat_rename(fs, "/b", "/b") == 0);
  expect(fat_rename(fs, "/missing", "/x") < 0);
  expect(fat_rename(fs, "/b/other.txt", "/nodir/x") < 0);
  expect(fat_rename(fs, "/b/other.txt", "/b/bad:name") == FAT_EINVAL);
  expect(fat_rename(fs, "/b/other.txt", "/a") == FAT_EEXIST);
  expect(fat_rename(fs, "/", "/x") < 0 && fat_rename(fs, "/b/.", "/x") < 0 &&
         fat_rename(fs, "/b/other.txt", "/..") < 0);
  expect(is_seed(fs, "/b/other.txt", 400, 3));
  EXPECT_CLEAN(0);
}

static void rename_replaces(void)
{
  void *fs = rn_fs;
  expect(put_seed(fs, "/victim.txt", 5000, 9) == 0);
  uint32_t mid = free_clusters(fs);
  expect(fat_rename(fs, "/b/other.txt", "/VICTIM.TXT") == 0);
  expect(is_seed(fs, "/VICTIM.TXT", 400, 3) && !exists(fs, "/b/other.txt"));
  expect(free_clusters(fs) == mid + 10);
  expect(fat_rename(fs, "/VICTIM.TXT", "/victim.txt") == 0);
  EXPECT_CLEAN(0);
  expect(g_ck.nents > 0 && ck_find("/victim.txt") && ck_find("/victim.txt")->ntres == 0x18 &&
         !ck_find("/victim.txt")->lfn);
  expect(fat_rename(fs, "/victim.txt", "/Victim.txt") == 0);
  EXPECT_CLEAN(0);
  expect(ck_find("/Victim.txt") && ck_find("/Victim.txt")->lfn &&
         memcmp(ck_find("/Victim.txt")->sfn, "VICTIM  TXT", 11) == 0);
  expect(fat_rename(fs, "/Victim.txt", "/a/inner.dir") == 0 && is_seed(fs, "/a/inner.dir", 400, 3));
  fs = remount(fs);
  expect(is_seed(fs, "/a/inner.dir", 400, 3) && is_seed(fs, "/b/moved \xC3\xA9.txt", 300, 2));
  expect(fat_ops()->mkdir(fs, "/dirdst") == 0 && put(fs, "/filesrc", "f", 1) == 0);
  expect(fat_rename(fs, "/filesrc", "/dirdst") == FAT_EEXIST);
  expect(fat_rename(fs, "/dirdst", "/filesrc") == FAT_EEXIST);
  fat_ops()->close(fs);
  EXPECT_FSCK();
}

static void t_rename(void)
{
  test("rename within and across directories", rename_basic);
  test("rename rejects moving a directory into itself and bad targets", rename_rejects);
  test("rename replaces an existing file, case-only rename keeps the entry", rename_replaces);
}

static void *at_fs;

static void timestamps_wall_clock(void)
{
  expect(fmt_dev(32, 100000, 512, 0, NULL) == 0);
  g_clock = 1700000000LL;
  g_clock_usec = 150000;
  void *fs = mnt();
  expect(put(fs, "/t.txt", "one", 3) == 0);
  struct fat_stat st;
  expect(fat_getattr(fs, "/t.txt", &st) == 0);
  expect(st.ctime == 1700000000u && st.mtime == 1700000000u && st.atime == 1700000000u / 86400 * 86400);
  expect(st.size == 3 && !st.is_dir && (st.attr & FAT_ATTR_ARCHIVE));
  EXPECT_CLEAN(0);
  const struct ck_ent *e = ck_find("/t.txt");
  expect(e && e->ctenth == 15);
  expect((e->cdate >> 9) + 1980 == 2023 && ((e->cdate >> 5) & 15) == 11 && (e->cdate & 31) == 14);
  expect((e->ctime >> 11) == 22 && ((e->ctime >> 5) & 63) == 13 && (e->ctime & 31) == 10);

  g_clock = 1700003601LL;
  g_clock_usec = 990000;
  expect(fat_write_at(fs, "/t.txt", 3, "two", 3) == 3);
  expect(fat_getattr(fs, "/t.txt", &st) == 0);
  expect(st.ctime == 1700000000u && st.mtime == 1700003600u);
  g_clock = 1700007200LL;
  expect(put(fs, "/t.txt", "three", 5) == 0);
  expect(fat_getattr(fs, "/t.txt", &st) == 0 && st.mtime == 1700007200u && st.ctime == 1700000000u);
  g_clock = 1700010000LL;
  expect(fat_truncate(fs, "/t.txt", 2) == 0);
  expect(fat_getattr(fs, "/t.txt", &st) == 0 && st.mtime == 1700010000u);
  at_fs = fs;
}

static void creation_tenths(void)
{
  void *fs = at_fs;
  struct fat_stat st;
  g_clock = 1700000001LL;
  g_clock_usec = 250000;
  expect(put(fs, "/odd.txt", "", 0) == 0);
  EXPECT_CLEAN(0);
  expect(ck_find("/odd.txt")->ctenth == 125 && (ck_find("/odd.txt")->ctime & 31) == 10);
  expect(fat_ops()->mkdir(fs, "/newdir") == 0);
  expect(fat_getattr(fs, "/newdir", &st) == 0 && st.is_dir && st.mtime == 1700000000u);
}

static void read_only_files(void)
{
  void *fs = at_fs;
  struct fat_stat st;
  expect(fat_setattr(fs, "/t.txt", FAT_ATTR_RO | FAT_ATTR_HIDDEN, 0) == 0);
  expect(fat_getattr(fs, "/t.txt", &st) == 0 && (st.attr & FAT_ATTR_RO) && (st.attr & FAT_ATTR_HIDDEN));
  expect(fat_write_at(fs, "/t.txt", 0, "x", 1) == FAT_EACCES);
  expect(fat_truncate(fs, "/t.txt", 0) == FAT_EACCES);
  expect(fat_ops()->del(fs, "/t.txt") == FAT_EACCES);
  expect(fat_rename(fs, "/t.txt", "/u.txt") == FAT_EACCES);
  expect(put(fs, "/t.txt", "zzzz", 4) == FAT_EACCES);
  expect(put(fs, "/other", "q", 1) == 0 && fat_rename(fs, "/other", "/t.txt") == FAT_EACCES);
  expect(is_str(fs, "/t.txt", "th"));
  EXPECT_CLEAN(0);
  expect(ck_find("/t.txt") && (ck_find("/t.txt")->attr & 3) == 3);
  expect(fat_setattr(fs, "/t.txt", FAT_ATTR_SYSTEM, FAT_ATTR_RO) == 0);
  expect(fat_getattr(fs, "/t.txt", &st) == 0 && !(st.attr & FAT_ATTR_RO) && (st.attr & FAT_ATTR_SYSTEM));
  expect(fat_setattr(fs, "/t.txt", FAT_ATTR_DIR | 0x08, 0) == 0);
  expect(fat_getattr(fs, "/t.txt", &st) == 0 && !st.is_dir);
  expect(put(fs, "/t.txt", "zzzz", 4) == 0 && is_str(fs, "/t.txt", "zzzz"));
  expect(fat_ops()->mkdir(fs, "/rodir") == 0 && fat_setattr(fs, "/rodir", FAT_ATTR_RO, 0) == 0);
  expect(fat_ops()->rmdir(fs, "/rodir") == FAT_EACCES);
  expect(fat_setattr(fs, "/rodir", 0, FAT_ATTR_RO) == 0 && fat_ops()->rmdir(fs, "/rodir") == 0);
  expect(fat_setattr(fs, "/nope", 1, 0) < 0);
  EXPECT_CLEAN(0);
  fat_ops()->close(fs);
  EXPECT_FSCK();
}

static void t_attrs_times(void)
{
  test("timestamps follow the wall clock", timestamps_wall_clock);
  test("creation tenths carry the odd second", creation_tenths);
  test("read-only files refuse write, truncate, delete, rename and overwrite", read_only_files);
}

static void poke_fat(uint32_t clus, uint32_t val)
{
  struct ck *c = &g_ck;
  for (uint32_t f = 0; f < c->nfat; f++) {
    uint8_t *p = g_img + (size_t)c->base * 512 + ((size_t)c->rsvd + (size_t)f * c->fatsz) * c->bps;
    if (c->kind == 16) {
      p[clus * 2] = (uint8_t)val;
      p[clus * 2 + 1] = (uint8_t)(val >> 8);
    } else if (c->kind == 32) {
      p[clus * 4] = (uint8_t)val;
      p[clus * 4 + 1] = (uint8_t)(val >> 8);
      p[clus * 4 + 2] = (uint8_t)(val >> 16);
      p[clus * 4 + 3] = (uint8_t)(val >> 24);
    }
  }
}

static void corrupt_chains(void)
{
  int kind = case_kind;
  expect(fmt_dev(kind, kind == 16 ? 40960 : 100000, 512, 1, NULL) == 0);
  void *fs = mnt();
  expect(put_seed(fs, "/loop.bin", 5 * 512, 1) == 0);
  expect(put_seed(fs, "/bad.bin", 5 * 512, 2) == 0);
  expect(put_seed(fs, "/short.bin", 5 * 512, 3) == 0);
  expect(put_seed(fs, "/freed.bin", 5 * 512, 4) == 0);
  expect(fat_ops()->mkdir(fs, "/dloop") == 0 && put(fs, "/dloop/x", "x", 1) == 0);
  fat_ops()->close(fs);
  check_volume(g_img, 0, 0);
  uint32_t loop0 = ck_find("/loop.bin")->cluster, bad0 = ck_find("/bad.bin")->cluster;
  uint32_t short0 = ck_find("/short.bin")->cluster, freed0 = ck_find("/freed.bin")->cluster;
  uint32_t d0 = ck_find("/dloop")->cluster;
  poke_fat(loop0 + 4, loop0 + 1);
  poke_fat(bad0 + 2, kind == 16 ? 0xFFF7 : 0x0FFFFFF7);
  poke_fat(short0 + 1, kind == 16 ? 0xFFFF : 0x0FFFFFFF);
  poke_fat(freed0 + 2, 0);
  poke_fat(d0, d0);
  fs = mnt();
  uint8_t *b = malloc(5 * 512 + 1);
  int r = fat_ops()->read_at(fs, "/loop.bin", 0, b, 5 * 512);
  expect(r <= 5 * 512);
  uint8_t want[5 * 512];
  fill(want, sizeof want, 1);
  expect(r < 0 || memcmp(b, want, (size_t)r) == 0);
  r = fat_ops()->read_at(fs, "/loop.bin", 4 * 512, b, 512);
  expect(r <= 512);
  r = fat_ops()->read_at(fs, "/bad.bin", 0, b, 5 * 512);
  fill(want, sizeof want, 2);
  expect(r <= 3 * 512 && (r < 0 || memcmp(b, want, (size_t)r) == 0));
  r = fat_ops()->read_at(fs, "/short.bin", 0, b, 5 * 512);
  fill(want, sizeof want, 3);
  expect(r <= 2 * 512 && (r < 0 || memcmp(b, want, (size_t)r) == 0));
  r = fat_ops()->read_at(fs, "/freed.bin", 0, b, 5 * 512);
  fill(want, sizeof want, 4);
  expect(r <= 3 * 512 && (r < 0 || memcmp(b, want, (size_t)r) == 0));
  free(b);
  expect(count_dir(fs, "/dloop") <= 1);
  for (int i = 0; i < 40; i++) {
    char p[40];
    snprintf(p, sizeof p, "/dloop/f%02d", i);
    (void)put(fs, p, "", 0);
  }
  (void)fat_ops()->del(fs, "/loop.bin");
  (void)fat_ops()->del(fs, "/bad.bin");
  (void)fat_ops()->del(fs, "/freed.bin");
  (void)fat_truncate(fs, "/short.bin", 0);
  expect(fat_ops()->del(fs, "/missing") != 0);
  fat_ops()->close(fs);
}

static void corrupt_entry_clusters(void)
{
  expect(fmt_dev(32, 100000, 512, 1, NULL) == 0);
  void *fs = mnt();
  expect(put_seed(fs, "/c.bin", 1024, 1) == 0 && put_seed(fs, "/d.bin", 1024, 2) == 0 &&
         put_seed(fs, "/e.bin", 1024, 3) == 0 && put_seed(fs, "/f.bin", 1024, 4) == 0);
  fat_ops()->close(fs);
  check_volume(g_img, 0, 0);
  const struct ck_ent *e = ck_find("/c.bin");
  uint32_t root_sec = g_ck.root0 + (g_ck.rootc - 2) * g_ck.spc;
  uint8_t *dir = g_img + (size_t)root_sec * 512;
  for (int i = 0; i < 16; i++) {
    uint8_t *ent = dir + i * 32;
    if (memcmp(ent, "C       BIN", 11) == 0) {
      ent[26] = 0;
      ent[27] = 0;
      ent[20] = ent[21] = 0;
    }
    if (memcmp(ent, "D       BIN", 11) == 0) {
      ent[26] = 1;
      ent[27] = 0;
      ent[20] = ent[21] = 0;
    }
    if (memcmp(ent, "E       BIN", 11) == 0) {
      ent[26] = 0xFF;
      ent[27] = 0xFF;
      ent[20] = ent[21] = 0xFF;
    }
  }
  (void)e;
  fs = mnt();
  uint8_t tb[1024];
  /* A read leaves a chain hint and a write op zeroes only its key: the cluster-0 file must not inherit it. */
  expect(fat_ops()->read_at(fs, "/f.bin", 0, tb, 512) == 512); /* one cluster: hint index 0 */
  expect(fat_setattr(fs, "/f.bin", FAT_ATTR_ARCHIVE, 0) == 0);
  expect(fat_ops()->read_at(fs, "/c.bin", 0, tb, 1024) <= 0);
  expect(fat_ops()->read_at(fs, "/d.bin", 0, tb, 1024) <= 0);
  expect(fat_ops()->read_at(fs, "/e.bin", 0, tb, 1024) <= 0);
  expect(fat_ops()->del(fs, "/c.bin") == 0 && fat_ops()->del(fs, "/d.bin") == 0 && fat_ops()->del(fs, "/e.bin") == 0);
  fat_ops()->close(fs);
}

/* Rounding a size within one cluster of 4 GiB up to clusters must not wrap to zero and free the whole chain. */
static void huge_size_truncate(void)
{
  expect(fmt_dev(32, 100000, 512, 1, NULL) == 0);
  void *fs = mnt();
  expect(put_seed(fs, "/g.bin", 512, 7) == 0);
  fat_ops()->close(fs);
  check_volume(g_img, 0, 0);
  uint8_t *dir = g_img + (size_t)(g_ck.root0 + (g_ck.rootc - 2) * g_ck.spc) * 512;
  for (int i = 0; i < 16; i++)
    if (memcmp(dir + i * 32, "G       BIN", 11) == 0) {
      dir[i * 32 + 28] = 0xF8;
      dir[i * 32 + 29] = dir[i * 32 + 30] = dir[i * 32 + 31] = 0xFF;
    }
  fs = mnt();
  fat_truncate(fs, "/g.bin", 0xFFFFFFF0u);
  uint8_t tb[512], want[512];
  fill(want, sizeof want, 7);
  expect(fat_ops()->read_at(fs, "/g.bin", 0, tb, sizeof tb) == (int)sizeof tb && memcmp(tb, want, sizeof tb) == 0);
  fat_ops()->close(fs);
}

static void t_corrupt(void)
{
  alarm(60);
  static const int kinds[] = {16, 32};
  for (int k = 0; k < 2; k++) {
    char title[80];
    snprintf(title, sizeof title, "corrupt chains do not hang or read garbage (FAT%d)", kinds[k]);
    case_kind = kinds[k];
    test(title, corrupt_chains);
  }
  test("entry pointing at cluster 0, 1 or beyond the volume", corrupt_entry_clusters);
  test("size near 4 GiB keeps its chain on truncate", huge_size_truncate);
  alarm(0);
}

static uint32_t vs_fc0;

static void volume_clean(void)
{
  expect(fmt_dev(32, 100000, 512, 0, NULL) == 0);
  void *fs = mnt();
  expect(!(g_img[65] & 1));
  expect(put_seed(fs, "/x.bin", 20000, 1) == 0);
  expect(!(g_img[65] & 1));
  expect(check_volume(g_img, 0, 0) == 0);
  fat_ops()->close(fs);
}

static void volume_dirty_markers(void)
{
  expect(fmt_dev(32, 100000, 512, 0, NULL) == 0);
  void *fs = mnt();
  g_fail_write = 40;
  int r = put_seed(fs, "/y.bin", 60000, 2);
  expect(r != 0);
  expect(g_img[65] & 1);
  uint32_t fatsec = load_le16(g_img + 14);
  uint32_t f1 = load_le32(g_img + (size_t)fatsec * 512 + 4);
  expect(!(f1 & 0x08000000u));
  g_fail_write = -1;
  fat_ops()->close(fs);
}

static void volume_stays_dirty(void)
{
  void *fs = mnt();
  expect(fs != NULL);
  expect(put(fs, "/z.txt", "zz", 2) == 0);
  fat_ops()->close(fs);
  expect(g_img[65] & 1);
}

static void fsinfo_valid(void)
{
  expect(fmt_dev(32, 100000, 512, 0, NULL) == 0);
  void *fs = mnt();
  uint32_t fc0 = load_le32(g_img + 512 + 488);
  vs_fc0 = fc0;
  expect(fc0 == free_clusters(fs));
  expect(put_seed(fs, "/a.bin", 10000, 1) == 0);
  fat_ops()->close(fs);
  uint32_t fc1 = load_le32(g_img + 512 + 488);
  expect(fc1 == fc0 - 20 && load_le32(g_img + 512 + 492) >= 3);
  expect(check_volume(g_img, 0, 0) == 0);
  fs = mnt();
  expect(fat_ops()->del(fs, "/a.bin") == 0);
  fat_ops()->close(fs);
  expect(load_le32(g_img + 512 + 488) == fc0);
  expect(check_volume(g_img, 0, 0) == 0);
}

static void fsinfo_rebuilt(void)
{
  uint32_t fc0 = vs_fc0;
  g_img[512 + 488] = g_img[512 + 489] = g_img[512 + 490] = g_img[512 + 491] = 0xFF;
  void *fs = mnt();
  expect(free_clusters(fs) == fc0);
  expect(put(fs, "/q", "q", 1) == 0);
  fat_ops()->close(fs);
  expect(check_volume(g_img, 0, 0) == 0);
  expect(load_le32(g_img + 512 + 488) == fc0 - 1);
}

static void fat_copies_updated(void)
{
  expect(fmt_dev(16, 40960, 512, 0, NULL) == 0);
  void *fs = mnt();
  expect(put_seed(fs, "/m.bin", 50000, 1) == 0 && fat_ops()->mkdir(fs, "/d") == 0);
  expect(fat_ops()->del(fs, "/m.bin") == 0 && put_seed(fs, "/m2.bin", 70000, 2) == 0);
  fat_ops()->close(fs);
  expect(check_volume(g_img, 0, 0) == 0);
}

static void fat_mirroring_disabled(void)
{
  expect(fmt_dev(32, 100000, 512, 0, NULL) == 0);
  uint32_t fsz = load_le32(g_img + 36), rs = load_le16(g_img + 14);
  g_img[40] = 0x81;
  uint8_t fat0_before[512];
  memcpy(fat0_before, g_img + (size_t)rs * 512, 512);
  void *fs = mnt();
  expect(put_seed(fs, "/m.bin", 30000, 1) == 0);
  fat_ops()->close(fs);
  expect(memcmp(fat0_before, g_img + (size_t)rs * 512, 512) == 0);
  expect(memcmp(g_img + (size_t)rs * 512, g_img + ((size_t)rs + fsz) * 512, 512) != 0);
  expect(check_volume(g_img, 0, CK_SINGLE_FAT) == 0);
  fs = mnt();
  expect(is_seed(fs, "/m.bin", 30000, 1));
  fat_ops()->close(fs);
}

static void write_errors_surface(void)
{
  expect(fmt_dev(16, 40960, 512, 0, NULL) == 0);
  void *fs = mnt();
  expect(put_seed(fs, "/keep.bin", 5000, 1) == 0);
  for (long n = 0; n < 40; n += 3) {
    g_fail_write = n;
    (void)put_seed(fs, "/new.bin", 30000, 2);
    g_fail_write = -1;
    (void)fat_ops()->del(fs, "/new.bin");
  }
  g_fail_write = -1;
  fat_ops()->close(fs);
  fs = mnt();
  expect(fs != NULL && is_seed(fs, "/keep.bin", 5000, 1));
  fat_ops()->close(fs);
}

static void t_volume_state(void)
{
  test("volume is clean after an operation", volume_clean);
  test("a mid-operation image shows the dirty markers", volume_dirty_markers);
  test("an image left dirty is not marked clean by the driver", volume_stays_dirty);
  test("FSInfo free count and hints stay valid", fsinfo_valid);
  test("FAT32 FSInfo with unknown free count is rebuilt on demand", fsinfo_rebuilt);
  test("all FAT copies are updated", fat_copies_updated);
  test("FAT32 mirroring disabled: only the active FAT is written", fat_mirroring_disabled);
  test("write errors surface and leave a mountable volume", write_errors_surface);
}

static void part_mbr(void)
{
  dev_new(200000);
  uint8_t *mbr = g_img;
  memset(mbr + 446, 0, 64);
  mbr[446] = 0x80;
  mbr[446 + 4] = 0x0C;
  put_le32(mbr + 446 + 8, 2048);
  put_le32(mbr + 446 + 12, 150000);
  mbr[510] = 0x55;
  mbr[511] = 0xAA;
  struct fat_format_opts o = {
      .write = dev_write, .base_lba = 2048, .sectors = 150000, .kind = 32, .label = "PART", .serial = 5};
  expect(fat_format(&o) == 0);
  uint8_t head[512 * 2048];
  memcpy(head, g_img, sizeof head);
  uint8_t tail[512 * 4000];
  memcpy(tail, g_img + (size_t)(2048 + 150000) * 512, sizeof tail);
  void *fs = mnt();
  expect(fs != NULL);
  if (fs) {
    expect_string(fat_ops()->label(fs), "PART");
    expect(put_seed(fs, "/p.bin", 100000, 5) == 0 && fat_ops()->mkdir(fs, "/dd") == 0);
    expect(put(fs, "/dd/Long Name Here.txt", "hi", 2) == 0);
    fs = remount(fs);
    expect(is_seed(fs, "/p.bin", 100000, 5) && is_str(fs, "/dd/Long Name Here.txt", "hi"));
    fat_ops()->close(fs);
  }
  expect(memcmp(head, g_img, sizeof head) == 0);
  expect(memcmp(tail, g_img + (size_t)(2048 + 150000) * 512, sizeof tail) == 0);
  expect(check_volume(g_img, 2048, 0) == 0);
}

static void part_second_and_non_fat(void)
{
  uint8_t *mbr = g_img;
  mbr[446 + 4] = 0x07;
  expect(mnt() == NULL);
  mbr[446 + 4] = 0x0C;
  memcpy(mbr + 446 + 16, mbr + 446, 16);
  mbr[446 + 4] = 0x83;
  memset(mbr + 446 + 16 + 0, 0, 16);
  mbr[446 + 16 + 4] = 0x0B;
  put_le32(mbr + 446 + 16 + 8, 160000);
  put_le32(mbr + 446 + 16 + 12, 30000);
  mbr[446 + 4] = 0x83;
  struct fat_format_opts o2 = {.write = dev_write, .base_lba = 160000, .sectors = 30000, .kind = 16, .label = "SECOND"};
  expect(fat_format(&o2) == 0);
  void *fs = mnt();
  expect(fs != NULL);
  if (fs) {
    expect_string(fat_ops()->label(fs), "SECOND");
    expect_string(fat_ops()->type(fs), "FAT16");
    fat_ops()->close(fs);
  }
}

static void part_gpt_non_fat_first(void)
{
  dev_new(200000);
  memset(g_img, 0, 512 * 40);
  g_img[446 + 4] = 0xEE;
  put_le32(g_img + 446 + 8, 1);
  put_le32(g_img + 446 + 12, 199999);
  g_img[510] = 0x55;
  g_img[511] = 0xAA;
  uint8_t *h = g_img + 512;
  memcpy(h, "EFI PART", 8);
  put_le32(h + 8, 0x00010000);
  put_le32(h + 12, 92);
  put_le32(h + 72, 2);
  put_le32(h + 76, 0);
  put_le32(h + 80, 4);
  put_le32(h + 84, 128);
  uint8_t *ents = g_img + 1024;
  static const uint8_t linux_guid[16] = {0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
                                         0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4};
  memcpy(ents, linux_guid, 16);
  put_le32(ents + 32, 40960);
  put_le32(ents + 40, 70000);
  memcpy(ents + 128, guid_basic, 16);
  put_le32(ents + 128 + 32, 100000);
  put_le32(ents + 128 + 40, 189999);
  struct fat_format_opts o3 = {.write = dev_write, .base_lba = 100000, .sectors = 90000, .kind = 32, .label = "GPTVOL"};
  expect(fat_format(&o3) == 0);
  void *fs = mnt();
  expect(fs != NULL);
  if (fs) {
    expect_string(fat_ops()->label(fs), "GPTVOL");
    expect(put(fs, "/g.txt", "gpt", 3) == 0 && is_str(fs, "/g.txt", "gpt"));
    fat_ops()->close(fs);
  }
  expect(check_volume(g_img, 100000, 0) == 0);
}

static void part_esp_and_garbage(void)
{
  uint8_t *ents = g_img + 1024;
  memcpy(ents + 128, guid_esp, 16);
  void *fs = mnt();
  expect(fs != NULL);
  if (fs)
    fat_ops()->close(fs);
  memset(ents + 128, 0, 16);
  expect(mnt() == NULL);
  dev_new(5000);
  for (size_t i = 0; i < 5000 * 512; i++)
    g_img[i] = (uint8_t)(i * 31 + (i >> 8));
  g_img[510] = 0x55;
  g_img[511] = 0xAA;
  expect(mnt() == NULL);
  memset(g_img, 0, 5000 * 512);
  expect(mnt() == NULL);
  g_img[446 + 4] = 0x0C;
  put_le32(g_img + 446 + 8, 100);
  g_img[510] = 0x55;
  g_img[511] = 0xAA;
  expect(mnt() == NULL);
}

static void t_partitions(void)
{
  test("MBR partition, writes stay inside it", part_mbr);
  test("second MBR partition and non-FAT partition types are handled", part_second_and_non_fat);
  test("GPT with a non-FAT entry first", part_gpt_non_fat_first);
  test("EFI system partition GUID and garbage disks", part_esp_and_garbage);
}

static void large_fat32(void)
{
  expect(fmt_dev(32, 8388608, 512, 0, "BIG") == 0);
  void *fs = mnt();
  expect(fs != NULL);
  if (!fs)
    return;
  struct fat_statfs s;
  expect(fat_statfs(fs, &s) == 0 && s.cluster_bytes == 4096 && s.total_clusters > 1000000 && s.bits == 32);
  expect(put_seed(fs, "/big.bin", 3 * 1024 * 1024 + 17, 1) == 0);
  expect(is_seed(fs, "/big.bin", 3 * 1024 * 1024 + 17, 1));
  fs = remount(fs);
  expect(is_seed(fs, "/big.bin", 3 * 1024 * 1024 + 17, 1));
  expect(free_clusters(fs) == s.free_clusters - 769);
  expect(check_volume(g_img, 0, 0) == 0);
  fat_ops()->close(fs);
}

static void large_fat16(void)
{
  expect(fmt_dev(16, 4177920, 512, 0, "BIG16") == 0);
  void *fs = mnt();
  struct fat_statfs s;
  expect(fat_statfs(fs, &s) == 0 && s.cluster_bytes == 32768 && s.bits == 16);
  expect(put_seed(fs, "/b.bin", 700000, 3) == 0 && is_seed(fs, "/b.bin", 700000, 3));
  expect(check_volume(g_img, 0, 0) == 0);
  fat_ops()->close(fs);
}

static void large_512k_clusters(void)
{
  expect(fmt_dev(32, 4194304, 4096, 128, NULL) == FAT_EINVAL);
  expect(fmt_dev(16, 8388608, 4096, 128, NULL) == 0);
  void *fs = mnt();
  struct fat_statfs s;
  expect(fs != NULL);
  if (fs) {
    expect(fat_statfs(fs, &s) == 0 && s.cluster_bytes == 524288);
    expect(put_seed(fs, "/c.bin", 1200000, 4) == 0 && is_seed(fs, "/c.bin", 1200000, 4));
    expect(fat_ops()->mkdir(fs, "/d") == 0 && put(fs, "/d/e", "e", 1) == 0);
    expect(check_volume(g_img, 0, 0) == 0);
    fat_ops()->close(fs);
  }
}

static void t_large(void)
{
  test("FAT32 4 GiB volume", large_fat32);
  test("FAT16 nearly 2 GiB volume with 32 KiB clusters", large_fat16);
  test("512 KiB clusters on 4096-byte sectors", large_512k_clusters);
}

static void *perf_fs;
static uint8_t *perf_want, *perf_got;
static const uint32_t perf_n = 8 * 1024 * 1024;

static void perf_sequential(void)
{
  expect(fmt_dev(32, 100000, 512, 1, NULL) == 0);
  void *fs = mnt();
  uint32_t n = perf_n;
  expect(put_seed(fs, "/seq.bin", n, 5) == 0);
  uint8_t *want = perf_want = malloc(n), *got = perf_got = malloc(n);
  perf_fs = fs;
  fill(want, n, 5);
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  unsigned reads0 = g_nread;
  for (uint32_t off = 0; off < n; off += 512)
    if (fat_ops()->read_at(fs, "/seq.bin", off, got + off, 512) != 512)
      break;
  clock_gettime(CLOCK_MONOTONIC, &t1);
  double secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
  expect(memcmp(want, got, n) == 0);
  expect(secs < 2.0);
  expect(g_nread - reads0 < n / 512 + 200);
}

static void perf_large_read(void)
{
  void *fs = perf_fs;
  uint32_t n = perf_n;
  uint8_t *want = perf_want, *got = perf_got;
  memset(got, 0, n);
  unsigned reads0 = g_nread;
  expect(fat_ops()->read_at(fs, "/seq.bin", 0, got, n) == (int)n && memcmp(want, got, n) == 0);
  expect(g_nread - reads0 < n / 512 + 200);
  expect(fat_write_at(fs, "/seq.bin", 1000, "COHERENT", 8) == 8);
  uint8_t tmp[16];
  expect(fat_ops()->read_at(fs, "/seq.bin", 1000, tmp, 8) == 8 && memcmp(tmp, "COHERENT", 8) == 0);
  expect(fat_ops()->read_at(fs, "/seq.bin", 996, tmp, 16) == 16 && memcmp(tmp, want + 996, 4) == 0 &&
         memcmp(tmp + 4, "COHERENT", 8) == 0 && memcmp(tmp + 12, want + 1008, 4) == 0);
  free(want);
  free(got);
  fat_ops()->close(fs);
}

static void t_perf(void)
{
  test("sequential reads do not re-walk the chain from the start", perf_sequential);
  test("a large single read uses contiguous runs and stays coherent with writes", perf_large_read);
}

static void full_disk(void)
{
  int kind = case_kind;
  expect(fmt_dev(kind, case_secs, 512, 0, NULL) == 0);
  void *fs = mnt();
  uint32_t total = free_clusters(fs);
  int made = 0;
  for (int i = 0; i < 100000; i++) {
    char p[40];
    snprintf(p, sizeof p, "/fill%03d.bin", i % 400);
    if (put_seed(fs, p, 150000 + (uint32_t)i, (uint32_t)i) != 0)
      break;
    made++;
    if (made == 400)
      break;
  }
  expect(made > 1 && made < 400);
  uint32_t left = free_clusters(fs);
  expect(put_seed(fs, "/nospace.bin", 4000000, 1) == FAT_ENOSPC);
  expect(!exists(fs, "/nospace.bin"));
  expect(free_clusters(fs) == left);
  expect(put_seed(fs, "/fill000.bin", 4000000, 1) == FAT_ENOSPC);
  expect(is_seed(fs, "/fill000.bin", 150000, 0));
  EXPECT_CLEAN(0);
  for (int i = 0; i < made && i < 400; i++) {
    char p[40];
    snprintf(p, sizeof p, "/fill%03d.bin", i);
    expect(fat_ops()->del(fs, p) == 0);
  }
  expect(free_clusters(fs) == total || (kind == 32 && total - free_clusters(fs) < 20));
  EXPECT_CLEAN(0);
  fat_ops()->close(fs);
  EXPECT_FSCK();
}

static void t_nospace(void)
{
  static const int kinds[] = {12, 16, 32};
  static const uint32_t sizes[] = {2880, 40960, 70000};
  for (int k = 0; k < 3; k++) {
    char title[80];
    snprintf(title, sizeof title, "full disk on FAT%d fails cleanly", kinds[k]);
    case_kind = kinds[k];
    case_secs = sizes[k];
    test(title, full_disk);
  }
}

struct mfile {
  char path[160];
  bool dir;
  uint8_t *data;
  uint32_t size;
};

#define MODEL_MAX 200
static struct mfile g_model[MODEL_MAX];
static int g_model_n;

static bool ieq(const char *a, const char *b)
{
  for (; *a && *b; a++, b++) {
    char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a, y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
    if (x != y)
      return false;
  }
  return !*a && !*b;
}

static int mfind(const char *p)
{
  for (int i = 0; i < g_model_n; i++)
    if (ieq(g_model[i].path, p))
      return i;
  return -1;
}

static void mremove(int i)
{
  free(g_model[i].data);
  g_model[i] = g_model[--g_model_n];
}

static bool mhas_children(const char *dir)
{
  size_t n = strlen(dir);
  for (int i = 0; i < g_model_n; i++)
    if (strlen(g_model[i].path) > n && strncasecmp(g_model[i].path, dir, n) == 0 && g_model[i].path[n] == '/')
      return true;
  return false;
}

static void model_verify(void *fs)
{
  for (int i = 0; i < g_model_n; i++) {
    struct mfile *f = &g_model[i];
    if (f->dir) {
      if (!is_dir_path(fs, f->path)) {
        printf("  model: dir %s missing\n", f->path);
        expect(false);
      }
    } else {
      uint8_t *got = malloc(f->size + 1);
      int is_dir = 0;
      uint32_t size = 0;
      bool ok = fat_ops()->stat(fs, f->path, &is_dir, &size) == 0 && !is_dir && size == f->size;
      for (uint32_t off = 0; ok && off < f->size; off += 4096) {
        uint32_t ch = f->size - off < 4096 ? f->size - off : 4096;
        ok = fat_ops()->read_at(fs, f->path, off, got + off, ch) == (int)ch;
      }
      ok = ok && memcmp(got, f->data, f->size) == 0;
      if (!ok)
        printf("  model: file %s (size %u) mismatch\n", f->path, f->size);
      expect(ok);
      free(got);
    }
  }
}

static void rand_name(char *out, size_t cap)
{
  static const char *pool[] = {"a.txt",
                               "B.TXT",
                               "Long Name Number One.dat",
                               "ünï.bin",
                               "lower",
                               "UPPER",
                               "Mixed Case.Txt",
                               "日本.txt",
                               "x y z.c",
                               "file.tar.gz",
                               "d1",
                               "d2",
                               "d3",
                               "a1",
                               "b2",
                               "CamelCase.md",
                               "with space",
                               "dot.in.name.txt",
                               "n01",
                               "n02",
                               "n03",
                               "n04",
                               "😀.bin",
                               "ПРИВЕТ.txt"};
  snprintf(out, cap, "%s", pool[rnd() % (sizeof pool / sizeof pool[0])]);
}

static void rand_dir(char *out, size_t cap)
{
  int dirs[MODEL_MAX], n = 0;
  for (int i = 0; i < g_model_n; i++)
    if (g_model[i].dir)
      dirs[n++] = i;
  if (n == 0 || rnd() % 3 == 0) {
    snprintf(out, cap, "");
    return;
  }
  snprintf(out, cap, "%s", g_model[dirs[rnd() % (unsigned)n]].path);
}

static int model_ops;
static uint32_t model_seed;

static void model_body(void)
{
  int kind = case_kind, ops = model_ops;
  uint32_t secs = case_secs, spc = case_spc, seed = model_seed;
  g_seed = seed;
  expect(fmt_dev(kind, secs, 512, spc, NULL) == 0);
  void *fs = mnt();
  if (!fs)
    return;
  g_model_n = 0;
  uint32_t budget = secs * 512 / 3;
  static const uint32_t sizes[] = {0, 1, 100, 511, 512, 513, 1500, 4096, 5000, 20000};
  for (int op = 0; op < ops; op++) {
    uint32_t used = 0;
    for (int i = 0; i < g_model_n; i++)
      used += g_model[i].size;
    char dir[160], name[100], path[300];
    rand_dir(dir, sizeof dir);
    rand_name(name, sizeof name);
    snprintf(path, sizeof path, "%s/%s", dir, name);
    int idx = mfind(path);
    uint32_t pick = rnd() % 100;
    if (pick < 30 && g_model_n < MODEL_MAX - 5) {
      uint32_t sz = sizes[rnd() % (sizeof sizes / sizeof sizes[0])];
      if ((idx >= 0 && g_model[idx].dir) || used + sz > budget || strlen(path) > 150)
        continue;
      uint32_t sd = rnd();
      uint8_t *b = malloc(sz + 1);
      fill(b, sz, sd);
      int rc = put(fs, path, b, sz);
      if (rc != 0 && rc != FAT_ENOSPC) {
        printf("  create %s rc=%d\n", path, rc);
        expect(false);
      }
      if (rc == 0) {
        if (idx < 0)
          idx = g_model_n++, snprintf(g_model[idx].path, sizeof g_model[idx].path, "%s", path);
        else
          free(g_model[idx].data);
        g_model[idx].dir = false;
        g_model[idx].data = b;
        g_model[idx].size = sz;
      } else {
        free(b);
      }
    } else if (pick < 42) {
      if (idx < 0 || g_model[idx].dir)
        continue;
      expect(fat_ops()->del(fs, path) == 0);
      mremove(idx);
    } else if (pick < 54 && g_model_n < MODEL_MAX - 5) {
      int rc = fat_ops()->mkdir(fs, path);
      if (idx >= 0) {
        expect(rc != 0);
      } else if (rc == 0) {
        idx = g_model_n++;
        snprintf(g_model[idx].path, sizeof g_model[idx].path, "%s", path);
        g_model[idx].dir = true;
        g_model[idx].data = malloc(1);
        g_model[idx].size = 0;
      } else {
        expect(rc == FAT_ENOSPC);
      }
    } else if (pick < 62) {
      if (idx < 0 || !g_model[idx].dir)
        continue;
      int rc = fat_ops()->rmdir(fs, path);
      if (mhas_children(path)) {
        expect(rc != 0);
      } else {
        expect(rc == 0);
        mremove(idx);
      }
    } else if (pick < 78) {
      if (idx < 0)
        continue;
      char ndir[160], nname[100], npath[300];
      rand_dir(ndir, sizeof ndir);
      rand_name(nname, sizeof nname);
      snprintf(npath, sizeof npath, "%s/%s", ndir, nname);
      int nidx = mfind(npath);
      size_t pl = strlen(path);
      bool into_self = g_model[idx].dir && strlen(npath) > pl && strncasecmp(npath, path, pl) == 0 && npath[pl] == '/';
      bool ok_expected = !into_self && (nidx < 0 || (nidx == idx) || (!g_model[idx].dir && !g_model[nidx].dir));
      if (ieq(path, npath))
        ok_expected = true;
      int rc = fat_rename(fs, path, npath);
      if (ok_expected != (rc == 0)) {
        printf("  rename %s -> %s rc=%d expected %s\n", path, npath, rc, ok_expected ? "ok" : "fail");
        expect(false);
      }
      if (rc == 0 && !ieq(path, npath)) {
        if (nidx >= 0 && nidx != idx) {
          mremove(nidx);
          idx = mfind(path);
        }
        size_t nl = strlen(npath);
        for (int i = 0; i < g_model_n; i++) {
          if (i == idx || (g_model[idx].dir && strlen(g_model[i].path) > pl &&
                           strncasecmp(g_model[i].path, path, pl) == 0 && g_model[i].path[pl] == '/')) {
            char tmp[300];
            snprintf(tmp, sizeof tmp, "%s%s", npath, i == idx ? "" : g_model[i].path + pl);
            snprintf(g_model[i].path, sizeof g_model[i].path, "%s", tmp);
          }
        }
        (void)nl;
      } else if (rc == 0) {
        snprintf(g_model[idx].path, sizeof g_model[idx].path, "%s", npath);
      }
    } else if (pick < 90) {
      if (idx < 0 || g_model[idx].dir)
        continue;
      struct mfile *f = &g_model[idx];
      uint32_t off = rnd() % (f->size + 600), len = rnd() % 3000;
      if (used + off + len > budget)
        continue;
      uint8_t *b = malloc(len + 1);
      fill(b, len, rnd());
      int rc = fat_write_at(fs, path, off, b, len);
      expect(rc == (int)len);
      if (rc == (int)len) {
        uint32_t ns = off + len > f->size ? off + len : f->size;
        f->data = realloc(f->data, ns + 1);
        if (off > f->size)
          memset(f->data + f->size, 0, off - f->size);
        memcpy(f->data + off, b, len);
        f->size = ns;
      }
      free(b);
    } else {
      if (idx < 0 || g_model[idx].dir)
        continue;
      struct mfile *f = &g_model[idx];
      uint32_t ns = rnd() % (f->size + 1500);
      if (used + ns > budget)
        continue;
      expect(fat_truncate(fs, path, ns) == 0);
      f->data = realloc(f->data, ns + 1);
      if (ns > f->size)
        memset(f->data + f->size, 0, ns - f->size);
      f->size = ns;
    }
    if (op % 40 == 39) {
      fs = remount(fs);
      model_verify(fs);
      int e = check_volume(g_img, 0, 0);
      if (e)
        printf("  after op %d: %d errors, first: %s\n", op, e, g_ck.first);
      expect(e == 0);
    }
  }
  model_verify(fs);
  EXPECT_CLEAN(0);
  fat_ops()->close(fs);
  EXPECT_FSCK();
  for (int i = 0; i < g_model_n; i++)
    free(g_model[i].data);
  g_model_n = 0;
}

static void model_run(int kind, uint32_t secs, uint32_t spc, int ops, uint32_t seed)
{
  char title[100];
  snprintf(title, sizeof title, "random operations vs model, FAT%d spc %u, seed %u", kind, spc, seed);
  case_kind = kind;
  case_secs = secs;
  case_spc = spc;
  model_ops = ops;
  model_seed = seed;
  test(title, model_body);
}

static void t_random(void)
{
  model_run(12, 2880, 1, 400, 11);
  model_run(16, 40960, 1, 500, 12);
  model_run(32, 100000, 1, 500, 13);
  model_run(32, 540000, 8, 400, 14);
  model_run(16, 20000, 4, 400, 15);
}

static void write_file(const char *path, const void *data, size_t n)
{
  FILE *f = fopen(path, "wb");
  fwrite(data, 1, n, f);
  fclose(f);
}

static bool file_matches(const char *path, const void *data, size_t n)
{
  FILE *f = fopen(path, "rb");
  if (!f)
    return false;
  uint8_t *b = malloc(n + 1);
  size_t got = fread(b, 1, n + 1, f);
  fclose(f);
  bool ok = got == n && memcmp(b, data, n) == 0;
  free(b);
  return ok;
}

static const char *interop_label;

static void interop_body(void)
{
  int kind = case_kind;
  uint32_t secs = case_secs;
  const char *label = interop_label;
  tmp_init();
  char img[300], src[300], out[300];
  tmp_path(img, sizeof img, "interop.img");
  tmp_path(src, sizeof src, "src.bin");
  tmp_path(out, sizeof out, "out.bin");
  unlink(img);
  FILE *f = fopen(img, "wb");
  fseek(f, (long)secs * 512 - 1, SEEK_SET);
  fputc(0, f);
  fclose(f);
  expect(run("newfs_msdos -F %d -S 512 -s %u -v %s '%s' >/dev/null 2>&1", kind, secs, label, img) == 0);

  uint8_t data1[3000], data2[40000], data3[777];
  fill(data1, sizeof data1, 1);
  fill(data2, sizeof data2, 2);
  fill(data3, sizeof data3, 3);
  write_file(src, data1, sizeof data1);
  expect(run("mmd -i '%s' ::sub >/dev/null 2>&1", img) == 0);
  expect(run("mcopy -i '%s' '%s' ::Short.txt >/dev/null 2>&1", img, src) == 0);
  write_file(src, data2, sizeof data2);
  expect(run("mcopy -i '%s' '%s' '::A Long File Name.txt' >/dev/null 2>&1", img, src) == 0);
  write_file(src, data3, sizeof data3);
  expect(run("mcopy -i '%s' '%s' '::sub/Deep Long Name.dat' >/dev/null 2>&1", img, src) == 0);
  EXPECT_FSCK();

  dev_load(img);
  void *fs = mnt();
  expect(fs != NULL);
  if (!fs)
    return;
  expect_string(fat_ops()->label(fs), label);
  expect(fat_ops()->type(fs)[3] == '0' + kind / 10 && fat_ops()->type(fs)[4] == '0' + kind % 10);
  expect(count_dir(fs, "/") == 3 && count_dir(fs, "/sub") == 1);
  expect(dir_has(fs, "/", "Short.txt") && dir_has(fs, "/", "A Long File Name.txt") && dir_has(fs, "/", "sub"));
  uint8_t got[40000];
  expect(fat_ops()->read_at(fs, "/short.txt", 0, got, sizeof got) == 3000 && memcmp(got, data1, 3000) == 0);
  expect(fat_ops()->read_at(fs, "/a long file name.TXT", 0, got, sizeof got) == 40000 &&
         memcmp(got, data2, 40000) == 0);
  expect(fat_ops()->read_at(fs, "/sub/deep long name.dat", 0, got, sizeof got) == 777 && memcmp(got, data3, 777) == 0);
  expect(fat_ops()->read_at(fs, "/ALONGF~1.TXT", 0, got, 10) == 10);
  uint32_t free0 = free_clusters(fs);

  expect(put(fs,
             "/sub/\xC3\xBCn\xC3\xAF"
             "code \xE6\x97\xA5\xE6\x9C\xAC.txt",
             "unicode body", 12) == 0);
  expect(put_seed(fs, "/Another Long Name.bin", 50000, 4) == 0);
  expect(fat_ops()->mkdir(fs, "/newdir") == 0);
  expect(fat_rename(fs, "/Short.txt", "/newdir/moved long name.txt") == 0);
  expect(fat_ops()->del(fs, "/A Long File Name.txt") == 0);
  expect(fat_truncate(fs, "/sub/Deep Long Name.dat", 100) == 0);
  expect(fat_write_at(fs, "/newdir/moved long name.txt", 3000, "tail", 4) == 4);
  expect(free_clusters(fs) < free0 + 100);
  fat_ops()->close(fs);
  EXPECT_CLEAN(0);
  img_save(img);
  EXPECT_FSCK();

  uint8_t want_tail[3004];
  memcpy(want_tail, data1, 3000);
  memcpy(want_tail + 3000, "tail", 4);
  expect(run("mcopy -n -i '%s' '::newdir/moved long name.txt' '%s' >/dev/null 2>&1", img, out) == 0);
  expect(file_matches(out, want_tail, sizeof want_tail));
  unlink(out);
  uint8_t seed4[50000];
  fill(seed4, sizeof seed4, 4);
  expect(run("mcopy -n -i '%s' '::Another Long Name.bin' '%s' >/dev/null 2>&1", img, out) == 0);
  expect(file_matches(out, seed4, sizeof seed4));
  unlink(out);
  expect(run("mcopy -n -i '%s' '::sub/Deep Long Name.dat' '%s' >/dev/null 2>&1", img, out) == 0);
  expect(file_matches(out, data3, 100));
  unlink(out);
  char cmd[900];
  snprintf(cmd, sizeof cmd, "mdir -i '%s' :: 2>&1 | grep -c 'Another Long Name.bin'", img);
  FILE *p = popen(cmd, "r");
  char line[64] = "";
  if (!fgets(line, sizeof line, p))
    line[0] = 0;
  pclose(p);
  expect(atoi(line) >= 1);
  snprintf(cmd, sizeof cmd, "mdir -i '%s' ::sub 2>&1 | grep -c 'Deep Long Name.dat'", img);
  p = popen(cmd, "r");
  if (!fgets(line, sizeof line, p))
    line[0] = 0;
  pclose(p);
  expect(atoi(line) >= 1);
  unlink(img);
  unlink(src);
}

static void interop_one(int kind, uint32_t secs, const char *label)
{
  char title[100];
  snprintf(title, sizeof title, "interop with newfs_msdos and mtools: FAT%d", kind);
  case_kind = kind;
  case_secs = secs;
  interop_label = label;
  test(title, interop_body);
}

static void interop_unavailable(void) {}

static void interop_4kn(void)
{
  tmp_init();
  char img[300];
  tmp_path(img, sizeof img, "k4.img");
  unlink(img);
  FILE *f = fopen(img, "wb");
  fseek(f, 64 * 1024 * 1024 - 1, SEEK_SET);
  fputc(0, f);
  fclose(f);
  expect(run("newfs_msdos -F 16 -S 4096 -s 16384 -v KFOUR '%s' >/dev/null 2>&1", img) == 0);
  dev_load(img);
  void *fs = mnt();
  expect(fs != NULL);
  if (fs) {
    expect_string(fat_ops()->label(fs), "KFOUR");
    expect(put_seed(fs, "/Long name on 4Kn.bin", 90000, 8) == 0 && fat_ops()->mkdir(fs, "/dir") == 0);
    fat_ops()->close(fs);
    EXPECT_CLEAN(0);
    EXPECT_FSCK();
  }
  unlink(img);
}

static void t_interop(void)
{
  if (!have_cmd("newfs_msdos") || !have_cmd("mcopy") || !have_cmd("mmd") || !have_cmd("fsck_msdos")) {
    test("interop skipped (newfs_msdos/mtools/fsck_msdos not available)", interop_unavailable);
    return;
  }
  interop_one(12, 2880, "FLOPPY");
  interop_one(16, 40960, "SIXTEEN");
  interop_one(32, 102400, "THIRTYTWO");
  test("newfs_msdos 4096-byte-sector volume is readable and writable", interop_4kn);
}

static void t_cleanup(void)
{
  if (g_tmp[0]) {
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_tmp);
    if (system(cmd) != 0)
      printf("  could not remove %s\n", g_tmp);
    g_tmp[0] = 0;
  }
}

int main(void)
{
  t_text();
  t_format();
  t_basic();
  t_names();
  t_dir_growth();
  test("deep directory tree", t_deep);
  t_fragmentation();
  t_write_trunc();
  t_rename();
  t_attrs_times();
  t_corrupt();
  t_volume_state();
  t_partitions();
  t_large();
  t_perf();
  t_nospace();
  t_random();
  t_interop();
  t_cleanup();
  return 0;
}
