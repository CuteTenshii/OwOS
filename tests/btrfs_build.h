/* btrfs_build.h: independent btrfs image builder for tests/btrfs_test.c.
 * Written from the on-disk format documentation; it shares no code with kernel/btrfs.c (own crc32c, own chunk
 * mapping, own tree packer, own compressors). Images are single device unless a chunk lists stripes on other ids. */
#ifndef OWOS_BTRFS_BUILD_H
#define OWOS_BTRFS_BUILD_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The host compressors are declared by hand: kernel/gzip.c and kernel/zstd.h clash with zlib.h and zstd.h. */
#ifdef HAVE_HOST_ZLIB
#define BB_HAVE_ZLIB 1
extern int compress2(unsigned char *dst, unsigned long *dst_len, const unsigned char *src, unsigned long src_len,
                     int level);
#endif
#ifdef HAVE_HOST_ZSTD
#define BB_HAVE_ZSTD 1
extern size_t ZSTD_compress(void *dst, size_t dst_cap, const void *src, size_t src_len, int level);
extern unsigned ZSTD_isError(size_t code);
#endif
#if defined(HAVE_HOST_LZO) && __has_include(<lzo/lzo1x.h>)
#include <lzo/lzo1x.h>
#define BB_HAVE_LZO 1
#endif

#define BB_SYSTEM 2ull
#define BB_METADATA 4ull
#define BB_DATA 1ull
#define BB_RAID0 0x08ull
#define BB_RAID1 0x10ull
#define BB_DUP 0x20ull
#define BB_RAID10 0x40ull
#define BB_RAID5 0x80ull
#define BB_RAID1C3 0x200ull
#define BB_RAID1C4 0x400ull
#define BB_PROFILES (BB_RAID0 | BB_RAID1 | BB_DUP | BB_RAID10 | BB_RAID5 | BB_RAID1C3 | BB_RAID1C4)

struct bb_item {
  uint64_t oid;
  uint8_t type;
  uint64_t off;
  uint8_t *data;
  uint32_t size;
};

struct bb_tree {
  struct bb_item *it;
  size_t n, cap;
  uint32_t id;
  uint64_t next_ino;
  uint64_t dir_next[65536];
  uint32_t *dh;
};

struct bb_stripe {
  uint64_t devid, off;
};

struct bb_chunk {
  uint64_t logical, length, type, stripe_len;
  int ns, sub;
  struct bb_stripe st[8];
};

/* Profile of a metadata or data chunk: profile bits (0 = SINGLE), stripe count and the device id of each stripe. */
struct bb_prof {
  uint64_t profile;
  int ns, sub;
  uint64_t stripe_len;
  uint64_t devids[8];
};

struct bb {
  uint8_t *img;
  size_t size;
  uint32_t nodesize, sectorsize;
  uint8_t fsid[16], meta_uuid[16];
  struct bb_chunk ch[3];
  uint64_t phys_next, meta_next, data_next, sys_next;
  int alloc_sys;
  uint32_t leaf_max, ptr_max;
  uint64_t generation, incompat, devid;
  char label[64];
  uint32_t csum_type;
  uint64_t root_tree_logical, chunk_tree_logical, root_dir_objectid;
  uint8_t root_level, chunk_level;
  uint64_t fs_root[8];
};

static uint32_t bb_crc_table[256];

static uint32_t bb_crc32c(uint32_t crc, const void *buf, size_t n)
{
  const uint8_t *p = buf;
  if (!bb_crc_table[1]) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++)
        c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
      bb_crc_table[i] = c;
    }
  }
  while (n--)
    crc = (crc >> 8) ^ bb_crc_table[(crc ^ *p++) & 0xFF];
  return crc;
}

static uint32_t bb_name_hash(const char *name, size_t len) { return bb_crc32c(0xFFFFFFFEu, name, len); }

static void bb_p16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void bb_p32(uint8_t *p, uint32_t v)
{
  for (int i = 0; i < 4; i++)
    p[i] = (uint8_t)(v >> (8 * i));
}

static void bb_p64(uint8_t *p, uint64_t v)
{
  for (int i = 0; i < 8; i++)
    p[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t bb_g64(const uint8_t *p)
{
  uint64_t v = 0;
  for (int i = 7; i >= 0; i--)
    v = v << 8 | p[i];
  return v;
}

static void bb_key(uint8_t *p, uint64_t oid, uint8_t type, uint64_t off)
{
  bb_p64(p, oid);
  p[8] = type;
  bb_p64(p + 9, off);
}

/* ---------------------------------------------------------------- layout and logical writes */

static void bb_poke(struct bb *b, uint64_t phys, const void *data, size_t len)
{
  if (phys + len > b->size) {
    fprintf(stderr, "bb_poke: image too small (%llu + %zu > %zu)\n", (unsigned long long)phys, len, b->size);
    exit(2);
  }
  memcpy(b->img + phys, data, len);
}

static void bb_chunk_set(struct bb *b, struct bb_chunk *c, uint64_t logical, uint64_t length, uint64_t type,
                         const struct bb_prof *pr)
{
  memset(c, 0, sizeof *c);
  c->logical = logical;
  c->length = length;
  c->type = type | (pr ? pr->profile : 0);
  c->ns = pr ? pr->ns : 1;
  c->sub = pr ? pr->sub : 0;
  c->stripe_len = pr ? pr->stripe_len : 0;
  uint64_t per = length;
  if (pr && pr->profile == BB_RAID0)
    per = (length + c->ns - 1) / c->ns;
  if (pr && pr->profile == BB_RAID10)
    per = (length + (c->ns / c->sub) - 1) / (c->ns / c->sub);
  per = (per + 0xFFFFF) & ~0xFFFFFull;
  for (int i = 0; i < c->ns; i++) {
    c->st[i].devid = pr ? pr->devids[i] : b->devid;
    c->st[i].off = b->phys_next;
    if (c->st[i].devid == b->devid)
      b->phys_next += per;
  }
}

static void bb_init(struct bb *b, uint32_t nodesize, uint32_t sectorsize, const struct bb_prof *meta,
                    const struct bb_prof *data, uint64_t meta_len, uint64_t data_len)
{
  memset(b, 0, sizeof *b);
  b->size = 96u << 20;
  b->img = calloc(1, b->size);
  b->nodesize = nodesize;
  b->sectorsize = sectorsize;
  for (int i = 0; i < 16; i++) {
    b->fsid[i] = (uint8_t)(0xA0 + i);
    b->meta_uuid[i] = (uint8_t)(0x50 + i);
  }
  b->devid = 1;
  b->generation = 7;
  b->incompat = 0x1 | 0x2 | 0x20 | 0x40 | 0x100 | 0x200;
  b->root_dir_objectid = 6;
  strcpy(b->label, "bbtest");
  b->phys_next = 0x100000;
  bb_chunk_set(b, &b->ch[0], 0x400000, 0x100000, BB_SYSTEM, NULL);
  bb_chunk_set(b, &b->ch[1], 0x1000000, meta_len, BB_METADATA, meta);
  bb_chunk_set(b, &b->ch[2], 0x2000000, data_len, BB_DATA, data);
  b->sys_next = b->ch[0].logical;
  b->meta_next = b->ch[1].logical;
  b->data_next = b->ch[2].logical;
}

static void bb_free(struct bb *b)
{
  free(b->img);
  memset(b, 0, sizeof *b);
}

static void bb_wr_stripe(struct bb *b, const struct bb_stripe *s, uint64_t off, const void *data, size_t len)
{
  if (s->devid == b->devid)
    bb_poke(b, s->off + off, data, len);
}

/* Write len bytes at a logical address through the chunk's profile (the builder's own mapping). */
static void bb_wr_logical(struct bb *b, uint64_t logical, const void *data, size_t len)
{
  const uint8_t *src = data;
  while (len) {
    struct bb_chunk *c = NULL;
    for (int i = 0; i < 3; i++)
      if (logical >= b->ch[i].logical && logical < b->ch[i].logical + b->ch[i].length)
        c = &b->ch[i];
    if (!c) {
      fprintf(stderr, "bb_wr_logical: unmapped %llu\n", (unsigned long long)logical);
      exit(2);
    }
    uint64_t off = logical - c->logical, prof = c->type & BB_PROFILES;
    size_t n = len;
    if (prof == BB_RAID0 || prof == BB_RAID10) {
      uint64_t nr = off / c->stripe_len, within = off % c->stripe_len;
      if (n > c->stripe_len - within)
        n = (size_t)(c->stripe_len - within);
      int groups = prof == BB_RAID0 ? c->ns : c->ns / c->sub;
      uint64_t po = (nr / groups) * c->stripe_len + within;
      int g = (int)(nr % groups);
      if (prof == BB_RAID0) {
        bb_wr_stripe(b, &c->st[g], po, src, n);
      } else {
        for (int k = 0; k < c->sub; k++)
          bb_wr_stripe(b, &c->st[g * c->sub + k], po, src, n);
      }
    } else {
      if (n > c->length - off)
        n = (size_t)(c->length - off);
      int copies = prof == 0 ? 1 : c->ns;
      for (int k = 0; k < copies; k++)
        bb_wr_stripe(b, &c->st[k], off, src, n);
    }
    logical += n;
    src += n;
    len -= n;
  }
}

/* Physical offset of the first copy of a logical byte on our device (for tests that corrupt one copy). */
static uint64_t bb_phys_of(const struct bb *b, uint64_t logical, int copy)
{
  for (int i = 0; i < 3; i++) {
    const struct bb_chunk *c = &b->ch[i];
    if (logical >= c->logical && logical < c->logical + c->length)
      return c->st[copy].off + (logical - c->logical);
  }
  return 0;
}

/* ---------------------------------------------------------------- trees */

static struct bb_item *bb_add(struct bb_tree *t, uint64_t oid, uint8_t type, uint64_t off, const void *data,
                              uint32_t size)
{
  if (t->n == t->cap) {
    t->cap = t->cap ? t->cap * 2 : 64;
    t->it = realloc(t->it, t->cap * sizeof *t->it);
  }
  struct bb_item *it = &t->it[t->n++];
  it->oid = oid;
  it->type = type;
  it->off = off;
  it->size = size;
  it->data = malloc(size ? size : 1);
  if (size)
    memcpy(it->data, data, size);
  return it;
}

static int bb_item_cmp(const void *a, const void *b)
{
  const struct bb_item *x = a, *y = b;
  if (x->oid != y->oid)
    return x->oid < y->oid ? -1 : 1;
  if (x->type != y->type)
    return x->type < y->type ? -1 : 1;
  if (x->off != y->off)
    return x->off < y->off ? -1 : 1;
  return 0;
}

static void bb_free_tree(struct bb_tree *t)
{
  for (size_t i = 0; i < t->n; i++)
    free(t->it[i].data);
  free(t->it);
  free(t->dh);
  memset(t, 0, sizeof *t);
}

static uint64_t bb_alloc_meta(struct bb *b)
{
  if (b->alloc_sys) {
    uint64_t at = b->sys_next;
    b->sys_next += b->nodesize;
    if (b->sys_next > b->ch[0].logical + b->ch[0].length) {
      fprintf(stderr, "bb: system chunk full\n");
      exit(2);
    }
    return at;
  }
  uint64_t at = b->meta_next;
  b->meta_next += b->nodesize;
  if (b->meta_next > b->ch[1].logical + b->ch[1].length) {
    fprintf(stderr, "bb: metadata chunk full\n");
    exit(2);
  }
  return at;
}

static void bb_node_header(struct bb *b, uint8_t *n, uint64_t bytenr, uint64_t owner, uint32_t nritems, uint8_t level)
{
  memcpy(n + 32, (b->incompat & 0x400) ? b->meta_uuid : b->fsid, 16);
  bb_p64(n + 48, bytenr);
  bb_p64(n + 56, 1ull | (1ull << 56));
  for (int i = 0; i < 16; i++)
    n[64 + i] = (uint8_t)(0xC0 + i);
  bb_p64(n + 80, b->generation);
  bb_p64(n + 88, owner);
  bb_p32(n + 96, nritems);
  n[100] = level;
}

static void bb_seal_node(struct bb *b, uint8_t *n, uint64_t bytenr)
{
  bb_p32(n, ~bb_crc32c(0xFFFFFFFFu, n + 32, b->nodesize - 32));
  bb_wr_logical(b, bytenr, n, b->nodesize);
}

/* Pack the sorted items into leaves and internal nodes bottom-up; returns the root's logical address. */
static uint64_t bb_write_tree(struct bb *b, struct bb_tree *t, uint64_t owner, uint8_t *level_out)
{
  struct lv {
    uint64_t bytenr;
    uint8_t key[17];
  } *cur, *next;
  size_t ncur = 0;
  uint8_t *buf = malloc(b->nodesize);
  qsort(t->it, t->n, sizeof *t->it, bb_item_cmp);
  cur = malloc((t->n + 2) * sizeof *cur);
  next = malloc((t->n + 2) * sizeof *next);

  size_t i = 0;
  do {
    memset(buf, 0, b->nodesize);
    uint32_t cnt = 0, data_end = b->nodesize;
    while (i < t->n) {
      if (101 + (cnt + 1) * 25 + (b->nodesize - data_end) + t->it[i].size > b->nodesize)
        break;
      if (b->leaf_max && cnt >= b->leaf_max)
        break;
      data_end -= t->it[i].size;
      uint8_t *ip = buf + 101 + cnt * 25;
      bb_key(ip, t->it[i].oid, t->it[i].type, t->it[i].off);
      bb_p32(ip + 17, data_end - 101);
      bb_p32(ip + 21, t->it[i].size);
      memcpy(buf + data_end, t->it[i].data, t->it[i].size);
      cnt++;
      i++;
    }
    if (cnt == 0 && i < t->n) {
      fprintf(stderr, "bb_write_tree: item too large\n");
      exit(2);
    }
    uint64_t at = bb_alloc_meta(b);
    bb_node_header(b, buf, at, owner, cnt, 0);
    bb_seal_node(b, buf, at);
    cur[ncur].bytenr = at;
    memset(cur[ncur].key, 0, 17);
    if (cnt)
      memcpy(cur[ncur].key, buf + 101, 17);
    ncur++;
  } while (i < t->n);

  uint8_t level = 0;
  while (ncur > 1) {
    level++;
    size_t nn = 0, j = 0;
    uint32_t maxp = (b->nodesize - 101) / 33;
    if (b->ptr_max && b->ptr_max < maxp)
      maxp = b->ptr_max;
    while (j < ncur) {
      memset(buf, 0, b->nodesize);
      uint32_t cnt = 0;
      uint8_t firstkey[17];
      memcpy(firstkey, cur[j].key, 17);
      while (j < ncur && cnt < maxp) {
        uint8_t *kp = buf + 101 + cnt * 33;
        memcpy(kp, cur[j].key, 17);
        bb_p64(kp + 17, cur[j].bytenr);
        bb_p64(kp + 25, b->generation);
        cnt++;
        j++;
      }
      uint64_t at = bb_alloc_meta(b);
      bb_node_header(b, buf, at, owner, cnt, level);
      bb_seal_node(b, buf, at);
      next[nn].bytenr = at;
      memcpy(next[nn].key, firstkey, 17);
      nn++;
    }
    struct lv *tmp = cur;
    cur = next;
    next = tmp;
    ncur = nn;
  }
  uint64_t root = cur[0].bytenr;
  free(cur);
  free(next);
  free(buf);
  if (level_out)
    *level_out = level;
  return root;
}

/* ---------------------------------------------------------------- item payloads */

static void bb_inode_item(uint8_t *d, uint32_t mode, uint32_t uid, uint32_t gid, uint64_t size, uint32_t nlink)
{
  memset(d, 0, 160);
  bb_p64(d, 1);
  bb_p64(d + 8, 1);
  bb_p64(d + 16, size);
  bb_p64(d + 24, size);
  bb_p32(d + 40, nlink);
  bb_p32(d + 44, uid);
  bb_p32(d + 48, gid);
  bb_p32(d + 52, mode);
  bb_p64(d + 112, 1700000001);
  bb_p32(d + 120, 111);
  bb_p64(d + 124, 1700000002);
  bb_p32(d + 132, 222);
  bb_p64(d + 136, 1700000003);
  bb_p32(d + 144, 333);
  bb_p64(d + 148, 1700000004);
  bb_p32(d + 156, 444);
}

static void bb_root_item(uint8_t *d, uint64_t bytenr, uint8_t level)
{
  memset(d, 0, 439);
  bb_inode_item(d, 040755, 0, 0, 3, 1);
  bb_p64(d + 160, 1);
  bb_p64(d + 168, 256);
  bb_p64(d + 176, bytenr);
  bb_p32(d + 216, 1);
  d[238] = level;
}

static uint32_t bb_dir_payload(uint8_t *d, uint64_t loc_oid, uint8_t loc_type, uint8_t ftype, const char *name,
                               size_t nlen)
{
  memset(d, 0, 30);
  bb_key(d, loc_oid, loc_type, loc_type == 132 ? ~0ull : 0);
  bb_p64(d + 17, 1);
  bb_p16(d + 25, 0);
  bb_p16(d + 27, (uint16_t)nlen);
  d[29] = ftype;
  memcpy(d + 30, name, nlen);
  return (uint32_t)(30 + nlen);
}

/* ---------------------------------------------------------------- file system content */

static void bb_tree_init(struct bb_tree *t, uint32_t id)
{
  memset(t, 0, sizeof *t);
  t->id = id;
  t->next_ino = 257;
}

/* Add name -> (loc_oid, loc_type) to directory dir: DIR_ITEM (merged on hash collision), DIR_INDEX, and for inodes
 * an INODE_REF in the child. */
static void bb_entry_ex(struct bb_tree *t, uint64_t dir, const char *name, uint64_t loc_oid, uint8_t loc_type,
                        uint8_t ftype)
{
  uint8_t d[512];
  size_t nlen = strlen(name);
  uint32_t n = bb_dir_payload(d, loc_oid, loc_type, ftype, name, nlen);
  uint64_t idx = t->dir_next[dir - 256] ? t->dir_next[dir - 256] : 2;
  t->dir_next[dir - 256] = idx + 1;
  uint64_t h = bb_name_hash(name, nlen);
  if (!t->dh)
    t->dh = calloc(1u << 21, sizeof *t->dh);
  uint32_t slot = (uint32_t)((dir * 0x9E3779B1u + h) & ((1u << 21) - 1));
  while (t->dh[slot] && !(t->it[t->dh[slot] - 1].oid == dir && t->it[t->dh[slot] - 1].off == h))
    slot = (slot + 1) & ((1u << 21) - 1);
  if (t->dh[slot]) {
    struct bb_item *old = &t->it[t->dh[slot] - 1];
    old->data = realloc(old->data, old->size + n);
    memcpy(old->data + old->size, d, n);
    old->size += n;
  } else {
    bb_add(t, dir, 84, h, d, n);
    t->dh[slot] = (uint32_t)t->n;
  }
  bb_add(t, dir, 96, idx, d, n);
  if (loc_type == 1) {
    uint8_t r[256 + 10];
    bb_p64(r, idx);
    bb_p16(r + 8, (uint16_t)nlen);
    memcpy(r + 10, name, nlen);
    bb_add(t, loc_oid, 12, dir, r, (uint32_t)(10 + nlen));
  }
}

static uint64_t bb_mkdir(struct bb_tree *t, uint64_t parent, const char *name, uint32_t mode, uint32_t uid, uint32_t gid)
{
  uint8_t ii[160];
  uint64_t ino = t->next_ino++;
  bb_inode_item(ii, 040000 | mode, uid, gid, 0, 1);
  bb_add(t, ino, 1, 0, ii, 160);
  bb_entry_ex(t, parent, name, ino, 1, 2);
  return ino;
}

static uint64_t bb_mkfile(struct bb_tree *t, uint64_t parent, const char *name, uint32_t mode, uint32_t uid,
                          uint32_t gid, uint64_t size, uint8_t ftype)
{
  uint8_t ii[160];
  uint64_t ino = t->next_ino++;
  bb_inode_item(ii, mode, uid, gid, size, 1);
  bb_add(t, ino, 1, 0, ii, 160);
  bb_entry_ex(t, parent, name, ino, 1, ftype);
  return ino;
}

/* The tree's root directory (inode 256), with the self INODE_REF a real mkfs writes. */
static void bb_root_dir(struct bb_tree *t)
{
  uint8_t ii[160], r[12];
  bb_inode_item(ii, 040755, 0, 0, 0, 1);
  bb_add(t, 256, 1, 0, ii, 160);
  bb_p64(r, 0);
  bb_p16(r + 8, 2);
  memcpy(r + 10, "..", 2);
  bb_add(t, 256, 12, 256, r, 12);
}

static void bb_fe_head(uint8_t *d, uint64_t ram, uint8_t comp, uint8_t type)
{
  memset(d, 0, 21);
  bb_p64(d, 1);
  bb_p64(d + 8, ram);
  d[16] = comp;
  d[20] = type;
}

static void bb_fe_inline(struct bb_tree *t, uint64_t ino, uint8_t comp, uint64_t ram, const void *data, uint32_t len)
{
  uint8_t *d = calloc(1, 21 + len);
  bb_fe_head(d, ram, comp, 0);
  memcpy(d + 21, data, len);
  bb_add(t, ino, 108, 0, d, 21 + len);
  free(d);
}

static void bb_fe_reg(struct bb_tree *t, uint64_t ino, uint64_t file_off, uint8_t comp, uint64_t disk, uint64_t disk_len,
                      uint64_t eoff, uint64_t nbytes, uint64_t ram, uint8_t type)
{
  uint8_t d[53] = {0};
  bb_fe_head(d, ram, comp, type);
  bb_p64(d + 21, disk);
  bb_p64(d + 29, disk_len);
  bb_p64(d + 37, eoff);
  bb_p64(d + 45, nbytes);
  bb_add(t, ino, 108, file_off, d, 53);
}

/* Store data in the data chunk (padded to a sector) and return its logical address. */
static uint64_t bb_data(struct bb *b, const void *data, size_t len)
{
  size_t padded = (len + b->sectorsize - 1) / b->sectorsize * b->sectorsize;
  uint8_t *tmp = calloc(1, padded ? padded : 1);
  memcpy(tmp, data, len);
  uint64_t at = b->data_next;
  b->data_next += padded;
  bb_wr_logical(b, at, tmp, padded);
  free(tmp);
  return at;
}

/* ---------------------------------------------------------------- compressors (builder side) */

static size_t bb_zlib_stored(const uint8_t *src, size_t n, uint8_t *dst)
{
  size_t o = 0, pos = 0;
  dst[o++] = 0x78;
  dst[o++] = 0x01;
  do {
    size_t chunk = n - pos > 65535 ? 65535 : n - pos;
    dst[o++] = pos + chunk == n;
    dst[o++] = (uint8_t)chunk;
    dst[o++] = (uint8_t)(chunk >> 8);
    dst[o++] = (uint8_t)~chunk;
    dst[o++] = (uint8_t)(~chunk >> 8);
    memcpy(dst + o, src + pos, chunk);
    o += chunk;
    pos += chunk;
  } while (pos < n);
  uint32_t a = 1, bs = 0;
  for (size_t i = 0; i < n; i++) {
    a = (a + src[i]) % 65521;
    bs = (bs + a) % 65521;
  }
  uint32_t ad = bs << 16 | a;
  for (int i = 3; i >= 0; i--)
    dst[o++] = (uint8_t)(ad >> (8 * i));
  return o;
}

static size_t bb_lzo_ext(uint8_t *o, size_t v)
{
  size_t p = 0;
  while (v > 255) {
    o[p++] = 0;
    v -= 255;
  }
  o[p++] = (uint8_t)v;
  return p;
}

/* A literal run: the opening run (any n >= 1) or a run in state 0 (n >= 4). */
static size_t bb_lzo_lit(uint8_t *o, const uint8_t *src, size_t n, int opening)
{
  size_t p = 0;
  if (opening && n <= 238) {
    o[p++] = (uint8_t)(n + 17);
  } else if (n <= 18) {
    o[p++] = (uint8_t)(n - 3);
  } else {
    o[p++] = 0;
    p += bb_lzo_ext(o + p, n - 18);
  }
  memcpy(o + p, src, n);
  return p + n;
}

/* One match (len >= 3, 1 <= dist <= 49151). *trail receives the offset of the byte whose low two bits carry the
 * number of trailing literals. */
static size_t bb_lzo_match(uint8_t *o, size_t len, size_t dist, size_t *trail)
{
  size_t p = 0;
  if (dist <= 2048 && len <= 8) {
    size_t d = dist - 1;
    *trail = p;
    o[p++] = (uint8_t)(((len - 1) << 5) | ((d & 7) << 2));
    o[p++] = (uint8_t)(d >> 3);
  } else if (dist <= 16384) {
    size_t d = dist - 1;
    if (len - 2 <= 31) {
      o[p++] = (uint8_t)(32 | (len - 2));
    } else {
      o[p++] = 32;
      p += bb_lzo_ext(o + p, len - 2 - 31);
    }
    *trail = p;
    o[p++] = (uint8_t)((d << 2) & 0xFF);
    o[p++] = (uint8_t)(d >> 6);
  } else {
    size_t d = dist - 16384;
    uint8_t hbit = (uint8_t)((d >> 14) & 1 ? 8 : 0);
    d &= 0x3FFF;
    if (len - 2 <= 7) {
      o[p++] = (uint8_t)(16 | hbit | (len - 2));
    } else {
      o[p++] = (uint8_t)(16 | hbit);
      p += bb_lzo_ext(o + p, len - 2 - 7);
    }
    *trail = p;
    o[p++] = (uint8_t)((d << 2) & 0xFF);
    o[p++] = (uint8_t)(d >> 6);
  }
  return p;
}

/* A small greedy LZO1X encoder: literal runs and M2/M3/M4 matches, ending in the EOS marker. out needs
 * n + n / 8 + 64 bytes. */
static size_t bb_lzo_compress(const uint8_t *src, size_t n, uint8_t *out)
{
  static int64_t head[1 << 14];
  for (size_t i = 0; i < (1 << 14); i++)
    head[i] = -1;
  size_t ip = 0, lit = 0, op = 0, trail = 0;
  int have_match = 0;

  for (;;) {
    size_t len = 0, dist = 0;
    if (ip + 3 <= n) {
      uint32_t h = (uint32_t)(((src[ip] << 16 | src[ip + 1] << 8 | src[ip + 2]) * 2654435761u) >> 18);
      int64_t cand = head[h];
      head[h] = (int64_t)ip;
      if (cand >= 0 && ip - (size_t)cand <= 49151) {
        while (ip + len < n && src[cand + len] == src[ip + len] && len < 700)
          len++;
        dist = ip - (size_t)cand;
      }
    }
    int at_end = ip >= n;
    if (len < 3 && !at_end) {
      ip++;
      continue;
    }
    size_t pend = ip - lit;
    if (pend) {
      if (have_match && pend <= 3) {
        out[trail] |= (uint8_t)pend;
        memcpy(out + op, src + lit, pend);
        op += pend;
      } else {
        op += bb_lzo_lit(out + op, src + lit, pend, !have_match && op == 0);
      }
    }
    if (at_end)
      break;
    size_t rel;
    size_t mlen = bb_lzo_match(out + op, len, dist, &rel);
    trail = op + rel;
    op += mlen;
    have_match = 1;
    ip += len;
    lit = ip;
  }
  out[op++] = 0x11;
  out[op++] = 0;
  out[op++] = 0;
  return op;
}

/* Frame an extent in btrfs LZO format: total length, then one length-prefixed segment per sector. */
static size_t bb_lzo_extent(const uint8_t *src, size_t n, uint32_t sector, uint8_t *dst, int use_host)
{
  size_t o = 4;
  for (size_t pos = 0; pos < n; pos += sector) {
    size_t chunk = n - pos > sector ? sector : n - pos;
    size_t left = sector - (o % sector);
    if (left < 4)
      o += left;
    uint8_t *seg = malloc(chunk + chunk / 8 + 128);
    size_t sl = 0;
#ifdef BB_HAVE_LZO
    if (use_host) {
      static uint8_t wrk[LZO1X_999_MEM_COMPRESS];
      lzo_uint l = 0;
      lzo_init();
      lzo1x_999_compress(src + pos, chunk, seg, &l, wrk);
      sl = l;
    }
#endif
    if (!use_host)
      sl = bb_lzo_compress(src + pos, chunk, seg);
    bb_p32(dst + o, (uint32_t)sl);
    memcpy(dst + o + 4, seg, sl);
    o += 4 + sl;
    free(seg);
  }
  bb_p32(dst, (uint32_t)o);
  return o;
}

/* A zstd frame of raw and RLE blocks only. */
static size_t bb_zstd_hand(const uint8_t *src, size_t n, uint8_t *dst)
{
  size_t o = 0, pos = 0;
  bb_p32(dst, 0xFD2FB528u);
  o = 4;
  dst[o++] = 0xA0;
  bb_p32(dst + o, (uint32_t)n);
  o += 4;
  do {
    size_t run = 1;
    while (pos + run < n && src[pos + run] == src[pos] && run < 131072)
      run++;
    size_t chunk;
    uint32_t type;
    if (run >= 64) {
      chunk = run;
      type = 1;
    } else {
      chunk = n - pos > 4096 ? 4096 : n - pos;
      type = 0;
      for (size_t k = 1; k < chunk; k++)
        if (pos + k + 64 <= n) {
          size_t r = 1;
          while (pos + k + r < n && src[pos + k + r] == src[pos + k] && r < 64)
            r++;
          if (r >= 64) {
            chunk = k;
            break;
          }
        }
    }
    uint32_t last = pos + chunk >= n;
    uint32_t hdr = (uint32_t)chunk << 3 | type << 1 | last;
    dst[o++] = (uint8_t)hdr;
    dst[o++] = (uint8_t)(hdr >> 8);
    dst[o++] = (uint8_t)(hdr >> 16);
    if (type == 1) {
      dst[o++] = src[pos];
    } else {
      memcpy(dst + o, src + pos, chunk);
      o += chunk;
    }
    pos += chunk;
  } while (pos < n);
  return o;
}

/* ---------------------------------------------------------------- superblock and commit */

static void bb_chunk_payload(const struct bb_chunk *c, uint8_t *d)
{
  memset(d, 0, 48 + 32 * c->ns);
  bb_p64(d, c->length);
  bb_p64(d + 8, 2);
  bb_p64(d + 16, c->stripe_len ? c->stripe_len : 65536);
  bb_p64(d + 24, c->type);
  bb_p32(d + 32, 65536);
  bb_p32(d + 36, 65536);
  bb_p32(d + 40, 4096);
  bb_p16(d + 44, (uint16_t)c->ns);
  bb_p16(d + 46, (uint16_t)c->sub);
  for (int i = 0; i < c->ns; i++) {
    bb_p64(d + 48 + 32 * i, c->st[i].devid);
    bb_p64(d + 48 + 32 * i + 8, c->st[i].off);
    for (int k = 0; k < 16; k++)
      d[48 + 32 * i + 16 + k] = (uint8_t)(0x70 + k);
  }
}

/* Write a superblock whose bytenr field is off at byte offset store of the image. */
static void bb_write_sb_at(struct bb *b, uint64_t off, uint64_t store, uint64_t generation, const char *label)
{
  uint8_t sb[4096];
  memset(sb, 0, sizeof sb);
  memcpy(sb + 32, b->fsid, 16);
  bb_p64(sb + 48, off);
  bb_p64(sb + 56, 1);
  memcpy(sb + 64, "_BHRfS_M", 8);
  bb_p64(sb + 72, generation);
  bb_p64(sb + 80, b->root_tree_logical);
  bb_p64(sb + 88, b->chunk_tree_logical);
  bb_p64(sb + 112, b->size);
  bb_p64(sb + 120, 4096 * 100);
  bb_p64(sb + 128, b->root_dir_objectid);
  bb_p64(sb + 136, 1);
  bb_p32(sb + 144, b->sectorsize);
  bb_p32(sb + 148, b->nodesize);
  bb_p32(sb + 152, b->nodesize);
  bb_p32(sb + 156, b->sectorsize);
  bb_p64(sb + 164, generation);
  bb_p64(sb + 188, b->incompat);
  bb_p16(sb + 196, (uint16_t)b->csum_type);
  sb[198] = b->root_level;
  sb[199] = b->chunk_level;
  bb_p64(sb + 201, b->devid);
  bb_p64(sb + 209, b->size);
  bb_p64(sb + 217, 4096 * 100);
  bb_p32(sb + 225, 4096);
  bb_p32(sb + 229, 4096);
  bb_p32(sb + 233, b->sectorsize);
  bb_p64(sb + 245, generation);
  memcpy(sb + 267, b->meta_uuid, 16);
  memcpy(sb + 283, b->fsid, 16);
  strncpy((char *)sb + 299, label, 255);
  memcpy(sb + 571, b->meta_uuid, 16);
  uint8_t chunk[48 + 32 * 8];
  bb_chunk_payload(&b->ch[0], chunk);
  bb_key(sb + 811, 256, 228, b->ch[0].logical);
  memcpy(sb + 811 + 17, chunk, 48 + 32 * b->ch[0].ns);
  bb_p32(sb + 160, (uint32_t)(17 + 48 + 32 * b->ch[0].ns));
  bb_p32(sb, ~bb_crc32c(0xFFFFFFFFu, sb + 32, sizeof sb - 32));
  bb_poke(b, store, sb, sizeof sb);
}

static void bb_write_sb(struct bb *b, uint64_t off, uint64_t generation, const char *label)
{
  bb_write_sb_at(b, off, off, generation, label);
}

/* Write the fs trees, the root tree and the chunk tree and the primary superblock. extra (may be NULL) holds
 * additional root tree items such as ROOT_BACKREFs. default_id selects the default subvolume. */
static void bb_commit(struct bb *b, struct bb_tree **fs, int nfs, uint32_t default_id, struct bb_tree *extra)
{
  struct bb_tree root, chunk;
  uint8_t ri[439], lvl;
  memset(&root, 0, sizeof root);
  memset(&chunk, 0, sizeof chunk);
  for (int i = 0; i < nfs; i++) {
    uint64_t at = bb_write_tree(b, fs[i], fs[i]->id, &lvl);
    b->fs_root[i] = at;
    bb_root_item(ri, at, lvl);
    bb_add(&root, fs[i]->id, 132, 0, ri, 439);
  }
  if (extra)
    for (size_t i = 0; i < extra->n; i++)
      bb_add(&root, extra->it[i].oid, extra->it[i].type, extra->it[i].off, extra->it[i].data, extra->it[i].size);
  uint8_t d[64];
  uint32_t n = bb_dir_payload(d, default_id, 132, 2, "default", 7);
  bb_add(&root, b->root_dir_objectid, 84, bb_name_hash("default", 7), d, n);
  b->root_tree_logical = bb_write_tree(b, &root, 1, &b->root_level);

  uint8_t di[98], cp[48 + 32 * 8];
  memset(di, 0, sizeof di);
  bb_p64(di, b->devid);
  bb_p64(di + 8, b->size);
  bb_p64(di + 16, 4096 * 100);
  bb_p32(di + 24, 4096);
  bb_p32(di + 28, 4096);
  bb_p32(di + 32, b->sectorsize);
  bb_add(&chunk, 1, 216, b->devid, di, sizeof di);
  for (int i = 0; i < 3; i++) {
    bb_chunk_payload(&b->ch[i], cp);
    bb_add(&chunk, 256, 228, b->ch[i].logical, cp, (uint32_t)(48 + 32 * b->ch[i].ns));
  }
  b->alloc_sys = 1;
  b->chunk_tree_logical = bb_write_tree(b, &chunk, 3, &b->chunk_level);
  b->alloc_sys = 0;
  bb_write_sb(b, 0x10000, b->generation, b->label);
  bb_free_tree(&root);
  bb_free_tree(&chunk);
}

#endif
