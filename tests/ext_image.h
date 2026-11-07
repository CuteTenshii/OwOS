/* ext_image.h: independent ext2/3/4 image builder for tests/ext_test.c.
 *
 * Lays out superblock, group descriptors, inode tables, extent trees, indirect blocks and directories straight
 * from the on-disk format, sharing no code with kernel/ext.c. Images are built in RAM; nothing is mkfs-verified.
 */
#ifndef EXT_IMAGE_H
#define EXT_IMAGE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define EB_COMPAT_JOURNAL 0x4u
#define EB_COMPAT_SPARSE2 0x200u
#define EB_RO_SPARSE 0x1u
#define EB_RO_LARGE_FILE 0x2u
#define EB_RO_HUGE_FILE 0x8u
#define EB_RO_GDT_CSUM 0x10u
#define EB_RO_MCSUM 0x400u
#define EB_INC_FILETYPE 0x2u
#define EB_INC_RECOVER 0x4u
#define EB_INC_META_BG 0x10u
#define EB_INC_EXTENTS 0x40u
#define EB_INC_64BIT 0x80u
#define EB_INC_FLEX_BG 0x200u
#define EB_INC_CSUM_SEED 0x2000u
#define EB_INC_LARGEDIR 0x4000u
#define EB_INC_INLINE 0x8000u
#define EB_INC_ENCRYPT 0x10000u
#define EB_INC_CASEFOLD 0x20000u

#define EB_IF_ENCRYPT 0x800u
#define EB_IF_INDEX 0x1000u
#define EB_IF_HUGE 0x40000u
#define EB_IF_EXTENTS 0x80000u
#define EB_IF_INLINE 0x10000000u
#define EB_IF_CASEFOLD 0x40000000u

#define EB_REG 0x8000u
#define EB_DIR 0x4000u
#define EB_LNK 0xA000u
#define EB_CHR 0x2000u
#define EB_BLK 0x6000u
#define EB_FIFO 0x1000u
#define EB_SOCK 0xC000u

#define EB_FT_REG 1
#define EB_FT_DIR 2
#define EB_FT_LNK 7

#define EB_GD_INODE_UNINIT 1u
#define EB_GD_BLOCK_UNINIT 2u
#define EB_GD_ITABLE_ZEROED 4u

static inline void w16(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static inline void w32(uint8_t *p, uint32_t v)
{
  w16(p, v);
  w16(p + 2, v >> 16);
}

static inline uint32_t r16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static inline uint32_t r32(const uint8_t *p) { return r16(p) | r16(p + 2) << 16; }

static inline void wb16(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}

static inline void wb32(uint8_t *p, uint32_t v)
{
  wb16(p, v >> 16);
  wb16(p + 2, v);
}

static inline uint32_t rb32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static uint32_t eb_crc32c(uint32_t crc, const void *p, size_t n)
{
  const uint8_t *b = p;
  while (n--) {
    crc ^= *b++;
    for (int k = 0; k < 8; k++)
      crc = (crc >> 1) ^ (0x82F63B78u & (0u - (crc & 1)));
  }
  return crc;
}

static uint16_t eb_crc16(uint16_t crc, const void *p, size_t n)
{
  const uint8_t *b = p;
  while (n--) {
    crc ^= *b++;
    for (int k = 0; k < 8; k++)
      crc = (uint16_t)((crc >> 1) ^ (0xA001u & (0u - (crc & 1))));
  }
  return crc;
}

static uint32_t eb_crc32be(uint32_t crc, const void *p, size_t n)
{
  const uint8_t *b = p;
  while (n--) {
    crc ^= (uint32_t)*b++ << 24;
    for (int k = 0; k < 8; k++)
      crc = (crc << 1) ^ (0x04C11DB7u & (0u - (crc >> 31)));
  }
  return crc;
}

struct eb {
  /* configuration, set before eb_init */
  uint32_t bs, isize, ipg, bpg, groups, dsize;
  uint32_t compat, incompat, ro_compat, first_meta_bg, backup[2];
  uint32_t state, last_orphan, journal_inum, csum_seed_val;
  bool reclen_zero;
  /* derived */
  uint32_t first_data, dpb;
  uint64_t nblocks;
  uint8_t *img, *used, *ino_used;
  uint64_t *itab;
  uint32_t *gflags;
  uint32_t next_ino;
  uint8_t uuid[16];
  uint32_t seed;
  bool metacsum, gdtcsum, ext;
};

static uint8_t *eb_blk(struct eb *e, uint64_t b) { return e->img + b * e->bs; }

static bool eb_has_super(const struct eb *e, uint32_t g)
{
  if (g == 0)
    return true;
  if (e->compat & EB_COMPAT_SPARSE2)
    return g == e->backup[0] || g == e->backup[1];
  if (!(e->ro_compat & EB_RO_SPARSE))
    return true;
  if (g == 1)
    return true;
  static const uint32_t bases[3] = {3, 5, 7};
  for (int i = 0; i < 3; i++)
    for (uint64_t p = bases[i]; p <= g; p *= bases[i])
      if (p == g)
        return true;
  return false;
}

static uint64_t eb_gd_block(const struct eb *e, uint32_t dg)
{
  if (!(e->incompat & EB_INC_META_BG) || dg < e->first_meta_bg)
    return (e->bs == 1024 ? 1u : 0u) + 1 + dg;
  uint32_t g = dg * e->dpb;
  return e->first_data + (uint64_t)g * e->bpg + (eb_has_super(e, g) ? 1 : 0);
}

static uint64_t eb_alloc_n(struct eb *e, uint32_t n)
{
  for (uint64_t b = 0; b + n <= e->nblocks; b++) {
    uint32_t k = 0;
    while (k < n && !e->used[b + k])
      k++;
    if (k == n) {
      for (k = 0; k < n; k++)
        e->used[b + k] = 1;
      memset(eb_blk(e, b), 0, (size_t)n * e->bs);
      return b;
    }
    b += k;
  }
  abort();
}

static uint64_t eb_alloc(struct eb *e) { return eb_alloc_n(e, 1); }

static uint8_t *eb_inode(struct eb *e, uint32_t ino)
{
  uint32_t g = (ino - 1) / e->ipg, i = (ino - 1) % e->ipg;
  return eb_blk(e, e->itab[g]) + (size_t)i * e->isize;
}

static void eb_init(struct eb *e)
{
  e->first_data = e->bs == 1024 ? 1 : 0;
  e->dpb = e->bs / e->dsize;
  e->nblocks = e->first_data + (uint64_t)e->groups * e->bpg;
  e->img = calloc(e->nblocks, e->bs);
  e->used = calloc(e->nblocks, 1);
  e->ino_used = calloc((size_t)e->groups * e->ipg + 2, 1);
  e->itab = calloc(e->groups, sizeof *e->itab);
  e->gflags = calloc(e->groups, sizeof *e->gflags);
  e->ext = (e->incompat & EB_INC_EXTENTS) != 0;
  e->metacsum = (e->ro_compat & EB_RO_MCSUM) != 0;
  e->gdtcsum = (e->ro_compat & EB_RO_GDT_CSUM) && !e->metacsum;
  for (int i = 0; i < 16; i++)
    e->uuid[i] = (uint8_t)(0xA0 + i * 3);
  e->seed = (e->incompat & EB_INC_CSUM_SEED) ? e->csum_seed_val : eb_crc32c(~0u, e->uuid, 16);
  e->used[0] = 1;
  e->used[e->bs == 1024 ? 1 : 0] = 1;
  uint32_t ngd = (e->groups + e->dpb - 1) / e->dpb;
  for (uint32_t dg = 0; dg < ngd; dg++)
    e->used[eb_gd_block(e, dg)] = 1;
  uint32_t itb = (e->ipg * e->isize + e->bs - 1) / e->bs;
  for (uint32_t g = 0; g < e->groups; g++)
    e->itab[g] = eb_alloc_n(e, itb);
  e->next_ino = 11;
}

static uint32_t eb_inode_seed(struct eb *e, uint32_t ino)
{
  uint8_t b[4];
  w32(b, ino);
  uint32_t c = eb_crc32c(e->seed, b, 4);
  w32(b, r32(eb_inode(e, ino) + 100));
  return eb_crc32c(c, b, 4);
}

/* ino 0 = next free. Zeroed inode with mode, one link, 32 bytes of extra fields and fixed timestamps. */
static uint32_t eb_mkino(struct eb *e, uint32_t ino, uint32_t mode)
{
  if (!ino)
    ino = e->next_ino++;
  uint8_t *r = eb_inode(e, ino);
  memset(r, 0, e->isize);
  w16(r, mode);
  w16(r + 26, 1);
  w32(r + 100, 0x5000u + ino);
  w32(r + 8, 1700000000u);
  w32(r + 12, 1700000001u);
  w32(r + 16, 1700000002u);
  if (e->isize > 128) {
    w16(r + 128, 32);
    w32(r + 0x90, 1600000000u);
  }
  e->ino_used[ino] = 1;
  return ino;
}

static void eb_set_size(struct eb *e, uint32_t ino, uint64_t size)
{
  uint8_t *r = eb_inode(e, ino);
  w32(r + 4, (uint32_t)size);
  w32(r + 108, (uint32_t)(size >> 32));
}

static void eb_add_blocks(struct eb *e, uint32_t ino, uint64_t nb)
{
  uint8_t *r = eb_inode(e, ino);
  uint64_t v = ((uint64_t)r32(r + 28) | (uint64_t)r16(r + 116) << 32) + nb * (e->bs / 512);
  w32(r + 28, (uint32_t)v);
  w16(r + 116, (uint32_t)(v >> 32));
}

/* ------------------------------------------------------------------ extents and indirect blocks */

struct eb_ext {
  uint32_t lblk, len;
  uint64_t phys;
  bool uninit;
};

static void eb_ext_csum(struct eb *e, uint32_t ino, uint8_t *node)
{
  if (!e->metacsum)
    return;
  uint32_t t = 12 + 12 * r16(node + 4);
  w32(node + t, eb_crc32c(eb_inode_seed(e, ino), node, t));
}

static void eb_ext_hdr(uint8_t *n, uint32_t entries, uint32_t max, uint32_t depth)
{
  w16(n, 0xF30A);
  w16(n + 2, entries);
  w16(n + 4, max);
  w16(n + 6, depth);
}

/* Build an extent tree of exactly `depth` levels below the inode: leaves hold leaf_max extents, index nodes idx_max. */
static void eb_extents(struct eb *e, uint32_t ino, const struct eb_ext *x, uint32_t n, uint32_t depth,
                       uint32_t leaf_max, uint32_t idx_max)
{
  uint8_t *r = eb_inode(e, ino);
  w32(r + 32, r32(r + 32) | EB_IF_EXTENTS);
  uint8_t *root = r + 40;
  memset(root, 0, 60);
  uint32_t cap = (e->bs - 12) / 12;
  if (depth == 0) {
    eb_ext_hdr(root, n, 4, 0);
    for (uint32_t i = 0; i < n; i++) {
      uint8_t *p = root + 12 + 12 * i;
      w32(p, x[i].lblk);
      w16(p + 4, x[i].len + (x[i].uninit ? 32768 : 0));
      w16(p + 6, (uint32_t)(x[i].phys >> 32));
      w32(p + 8, (uint32_t)x[i].phys);
    }
    return;
  }
  uint32_t nodes = 0;
  uint64_t blks[4096];
  uint32_t first[4096];
  for (uint32_t i = 0; i < n; i += leaf_max) {
    uint32_t c = n - i < leaf_max ? n - i : leaf_max;
    uint64_t b = eb_alloc(e);
    uint8_t *nd = eb_blk(e, b);
    eb_ext_hdr(nd, c, cap, 0);
    for (uint32_t k = 0; k < c; k++) {
      uint8_t *p = nd + 12 + 12 * k;
      w32(p, x[i + k].lblk);
      w16(p + 4, x[i + k].len + (x[i + k].uninit ? 32768 : 0));
      w16(p + 6, (uint32_t)(x[i + k].phys >> 32));
      w32(p + 8, (uint32_t)x[i + k].phys);
    }
    eb_ext_csum(e, ino, nd);
    blks[nodes] = b;
    first[nodes++] = x[i].lblk;
    eb_add_blocks(e, ino, 1);
  }
  for (uint32_t lvl = 1; lvl < depth; lvl++) {
    uint32_t nn = 0;
    uint64_t nb2[4096];
    uint32_t nf[4096];
    for (uint32_t i = 0; i < nodes; i += idx_max) {
      uint32_t c = nodes - i < idx_max ? nodes - i : idx_max;
      uint64_t b = eb_alloc(e);
      uint8_t *nd = eb_blk(e, b);
      eb_ext_hdr(nd, c, cap, lvl);
      for (uint32_t k = 0; k < c; k++) {
        uint8_t *p = nd + 12 + 12 * k;
        w32(p, first[i + k]);
        w32(p + 4, (uint32_t)blks[i + k]);
        w16(p + 8, (uint32_t)(blks[i + k] >> 32));
      }
      eb_ext_csum(e, ino, nd);
      nb2[nn] = b;
      nf[nn++] = first[i];
      eb_add_blocks(e, ino, 1);
    }
    memcpy(blks, nb2, nn * sizeof *blks);
    memcpy(first, nf, nn * sizeof *first);
    nodes = nn;
  }
  if (nodes > 4)
    abort();
  eb_ext_hdr(root, nodes, 4, depth);
  for (uint32_t k = 0; k < nodes; k++) {
    uint8_t *p = root + 12 + 12 * k;
    w32(p, first[k]);
    w32(p + 4, (uint32_t)blks[k]);
    w16(p + 8, (uint32_t)(blks[k] >> 32));
  }
}

static void eb_put_ptr(struct eb *e, uint8_t *table, uint32_t idx, uint32_t phys)
{
  (void)e;
  w32(table + idx * 4, phys);
}

/* Map one logical block through direct/indirect/double/triple pointers, allocating pointer blocks as needed. */
static void eb_ind_set(struct eb *e, uint32_t ino, uint64_t lblk, uint32_t phys)
{
  uint8_t *ib = eb_inode(e, ino) + 40;
  uint32_t ppb = e->bs / 4;
  if (lblk < 12) {
    eb_put_ptr(e, ib, (uint32_t)lblk, phys);
    return;
  }
  uint64_t i = lblk - 12;
  uint32_t slot, levels;
  uint64_t span = ppb;
  if (i < ppb) {
    slot = 12;
    levels = 1;
  } else if ((i -= ppb) < (uint64_t)ppb * ppb) {
    slot = 13;
    levels = 2;
  } else {
    i -= (uint64_t)ppb * ppb;
    slot = 14;
    levels = 3;
  }
  uint8_t *tbl = ib;
  uint32_t idx = slot;
  for (uint32_t lv = levels; lv > 0; lv--) {
    uint32_t p = r32(tbl + idx * 4);
    if (!p) {
      p = (uint32_t)eb_alloc(e);
      w32(tbl + idx * 4, p);
      eb_add_blocks(e, ino, 1);
    }
    tbl = eb_blk(e, p);
    span = 1;
    for (uint32_t k = 1; k < lv; k++)
      span *= ppb;
    idx = (uint32_t)(i / span);
    i %= span;
  }
  w32(tbl + idx * 4, phys);
}

/* Write data into freshly allocated contiguous blocks and map it from logical block `lblk`. */
static uint64_t eb_data_at(struct eb *e, uint32_t ino, uint32_t lblk, const void *data, uint64_t len)
{
  uint32_t nb = (uint32_t)((len + e->bs - 1) / e->bs);
  if (!nb)
    return 0;
  uint64_t phys = eb_alloc_n(e, nb);
  memcpy(eb_blk(e, phys), data, len);
  eb_add_blocks(e, ino, nb);
  if (e->ext) {
    struct eb_ext x[1] = {{lblk, nb, phys, false}};
    eb_extents(e, ino, x, 1, 0, 4, 4);
  } else {
    for (uint32_t i = 0; i < nb; i++)
      eb_ind_set(e, ino, lblk + i, (uint32_t)(phys + i));
  }
  return phys;
}

static uint32_t eb_file(struct eb *e, uint32_t ino, uint32_t mode, const void *data, uint64_t len)
{
  ino = eb_mkino(e, ino, mode);
  eb_data_at(e, ino, 0, data, len);
  eb_set_size(e, ino, len);
  return ino;
}

static uint32_t eb_symlink(struct eb *e, const char *target, bool slow)
{
  uint32_t ino = eb_mkino(e, 0, EB_LNK | 0777);
  size_t n = strlen(target);
  if (!slow) {
    memcpy(eb_inode(e, ino) + 40, target, n);
  } else {
    eb_data_at(e, ino, 0, target, n);
  }
  eb_set_size(e, ino, n);
  return ino;
}

/* ------------------------------------------------------------------ directories */

struct eb_de {
  const uint8_t *name;
  uint32_t nlen, ino;
  uint8_t ft;
};

#define EB_NAME(s) ((const uint8_t *)(s)), (uint32_t)(sizeof(s) - 1)

static uint32_t eb_enc_reclen(const struct eb *e, uint32_t len)
{
  if (e->bs == 65536 && len == 65536)
    return e->reclen_zero ? 0 : 65535;
  return len;
}

static void eb_put_dent(struct eb *e, uint8_t *p, const struct eb_de *d, uint32_t rec)
{
  w32(p, d->ino);
  w16(p + 4, eb_enc_reclen(e, rec));
  p[6] = (uint8_t)d->nlen;
  p[7] = (e->incompat & EB_INC_FILETYPE) ? d->ft : 0;
  memcpy(p + 8, d->name, d->nlen);
}

static void eb_leaf_close(struct eb *e, uint32_t ino, uint8_t *blk, uint32_t last_off, uint32_t usable)
{
  uint8_t *p = blk + last_off;
  w16(p + 4, eb_enc_reclen(e, usable - last_off));
  if (e->metacsum) {
    uint8_t *t = blk + usable;
    memset(t, 0, 12);
    w16(t + 4, 12);
    t[7] = 0xDE;
    w32(t + 8, eb_crc32c(eb_inode_seed(e, ino), blk, usable));
  }
}

/* Pack entries into leaf blocks (returned malloc'd, *nblk blocks). */
static uint8_t *eb_pack(struct eb *e, uint32_t dino, const struct eb_de *ents, uint32_t n, uint32_t *nblk)
{
  uint32_t usable = e->bs - (e->metacsum ? 12 : 0), cap = 4, b = 0, off = 0, last = 0;
  uint8_t *buf = calloc(cap, e->bs);
  for (uint32_t i = 0; i < n; i++) {
    uint32_t need = (8 + ents[i].nlen + 3) & ~3u;
    if (off + need > usable && off) {
      eb_leaf_close(e, dino, buf + (size_t)b * e->bs, last, usable);
      b++;
      off = 0;
    }
    if (b >= cap) {
      buf = realloc(buf, (size_t)cap * 2 * e->bs);
      memset(buf + (size_t)cap * e->bs, 0, (size_t)cap * e->bs);
      cap *= 2;
    }
    uint8_t *blk = buf + (size_t)b * e->bs;
    eb_put_dent(e, blk + off, &ents[i], need);
    last = off;
    off += need;
  }
  if (off == 0 && b == 0) {
    struct eb_de empty = {(const uint8_t *)"", 0, 0, 0};
    eb_put_dent(e, buf, &empty, 8);
    last = 0;
  }
  eb_leaf_close(e, dino, buf + (size_t)b * e->bs, last, usable);
  *nblk = b + 1;
  return buf;
}

static void eb_map_run(struct eb *e, uint32_t ino, uint32_t lblk, uint64_t phys, uint32_t nb)
{
  if (e->ext) {
    struct eb_ext x[1] = {{lblk, nb, phys, false}};
    eb_extents(e, ino, x, 1, 0, 4, 4);
  } else {
    for (uint32_t i = 0; i < nb; i++)
      eb_ind_set(e, ino, lblk + i, (uint32_t)(phys + i));
  }
}

/* Create directory dino with "." / ".." and ents; `gap` unmapped blocks precede the data. Returns first data block. */
static uint64_t eb_dir(struct eb *e, uint32_t dino, uint32_t parent, const struct eb_de *ents, uint32_t n,
                       uint32_t gap)
{
  struct eb_de *all = malloc((n + 2) * sizeof *all);
  all[0] = (struct eb_de){EB_NAME("."), dino, EB_FT_DIR};
  all[1] = (struct eb_de){EB_NAME(".."), parent, EB_FT_DIR};
  memcpy(all + 2, ents, n * sizeof *all);
  uint32_t nb;
  uint8_t *buf = eb_pack(e, dino, all, n + 2, &nb);
  uint64_t phys = eb_alloc_n(e, nb);
  memcpy(eb_blk(e, phys), buf, (size_t)nb * e->bs);
  free(buf);
  free(all);
  eb_map_run(e, dino, gap, phys, nb);
  eb_add_blocks(e, dino, nb);
  eb_set_size(e, dino, (uint64_t)(gap + nb) * e->bs);
  uint8_t *r = eb_inode(e, dino);
  w16(r + 26, 2);
  w32(r + 108, 0);
  return phys;
}

static uint32_t eb_dx_tail_csum(struct eb *e, uint32_t ino, uint8_t *blk, uint32_t countoff)
{
  uint32_t limit = r16(blk + countoff), count = r16(blk + countoff + 2);
  uint8_t *tail = blk + countoff + limit * 8;
  uint8_t zero[4] = {0, 0, 0, 0};
  uint32_t c = eb_crc32c(eb_inode_seed(e, ino), blk, countoff + count * 8);
  c = eb_crc32c(c, tail, 4);
  c = eb_crc32c(c, zero, 4);
  w32(tail + 4, c);
  return c;
}

/* Indexed directory: block 0 is the dx root, `levels` 1 or 2 index levels above the leaves. */
static void eb_dir_htree(struct eb *e, uint32_t dino, uint32_t parent, const struct eb_de *ents, uint32_t n,
                         uint32_t levels)
{
  uint32_t nleaf;
  uint8_t *leaves = eb_pack(e, dino, ents, n, &nleaf);
  uint32_t tail = e->metacsum ? 8 : 0;
  uint32_t node_limit = (e->bs - 8 - tail) / 8, root_limit = (e->bs - 32 - tail) / 8;
  uint32_t nnodes = levels == 2 ? 2 : 0;
  uint32_t total = 1 + nnodes + nleaf;
  uint64_t phys = eb_alloc_n(e, total);
  uint8_t *root = eb_blk(e, phys);
  struct eb_de dot = {EB_NAME("."), dino, EB_FT_DIR}, dotdot = {EB_NAME(".."), parent, EB_FT_DIR};
  eb_put_dent(e, root, &dot, 12);
  eb_put_dent(e, root + 12, &dotdot, e->bs - 12);
  root[24 + 4] = 1;
  root[24 + 5] = 8;
  root[24 + 6] = (uint8_t)(levels - 1);
  for (uint32_t i = 0; i < nleaf; i++)
    memcpy(eb_blk(e, phys + 1 + nnodes + i), leaves + (size_t)i * e->bs, e->bs);
  free(leaves);
  if (levels == 1) {
    w16(root + 32, root_limit);
    w16(root + 34, nleaf);
    for (uint32_t i = 0; i < nleaf; i++) {
      if (i)
        w32(root + 32 + i * 8, 0x1000u * i);
      w32(root + 36 + i * 8, 1 + i);
    }
  } else {
    w16(root + 32, root_limit);
    w16(root + 34, 2);
    uint32_t half = (nleaf + 1) / 2;
    for (uint32_t k = 0; k < 2; k++) {
      uint8_t *nd = eb_blk(e, phys + 1 + k);
      w32(nd, 0);
      w16(nd + 4, eb_enc_reclen(e, e->bs));
      uint32_t from = k * half, cnt = k ? nleaf - half : half;
      w16(nd + 8, node_limit);
      w16(nd + 10, cnt);
      for (uint32_t i = 0; i < cnt; i++) {
        if (i)
          w32(nd + 8 + i * 8, 0x1000u * (from + i));
        w32(nd + 12 + i * 8, 1 + nnodes + from + i);
      }
      if (e->metacsum)
        eb_dx_tail_csum(e, dino, nd, 8);
      if (k)
        w32(root + 32 + k * 8, 0x80000000u);
      w32(root + 36 + k * 8, 1 + k);
    }
  }
  if (e->metacsum)
    eb_dx_tail_csum(e, dino, root, 32);
  eb_map_run(e, dino, 0, phys, total);
  eb_add_blocks(e, dino, total);
  eb_set_size(e, dino, (uint64_t)total * e->bs);
  uint8_t *r = eb_inode(e, dino);
  w16(r + 26, 2);
  w32(r + 108, 0);
  w32(r + 32, r32(r + 32) | EB_IF_INDEX);
}

/* ------------------------------------------------------------------ inline data */

static void eb_xattr_data(struct eb *e, uint32_t ino, const void *val, uint32_t vlen)
{
  uint8_t *r = eb_inode(e, ino);
  uint32_t extra = r16(r + 128), start = 128 + extra;
  w32(r + start, 0xEA020000u);
  uint8_t *en = r + start + 4;
  en[0] = 4;
  en[1] = 7;
  w16(en + 2, 24);
  w32(en + 8, vlen);
  memcpy(en + 16, "data", 4);
  memcpy(en + 24, val, vlen);
}

static uint32_t eb_inline_file(struct eb *e, const void *data, uint32_t len)
{
  uint32_t ino = eb_mkino(e, 0, EB_REG | 0644);
  uint8_t *r = eb_inode(e, ino);
  w32(r + 32, EB_IF_INLINE);
  memcpy(r + 40, data, len < 60 ? len : 60);
  eb_xattr_data(e, ino, len > 60 ? (const uint8_t *)data + 60 : (const uint8_t *)"", len > 60 ? len - 60 : 0);
  eb_set_size(e, ino, len);
  return ino;
}

static void eb_inline_dir(struct eb *e, uint32_t dino, uint32_t parent, const struct eb_de *ents, uint32_t n)
{
  uint8_t *r = eb_inode(e, dino);
  w32(r + 32, EB_IF_INLINE);
  w32(r + 40, parent);
  uint8_t region[1024];
  uint32_t off[2] = {0, 0}, reg = 0, last[2] = {0, 0}, cap[2] = {56, 0};
  memset(region, 0, sizeof region);
  uint8_t *base[2] = {r + 44, region};
  cap[1] = e->isize > 128 + 32 + 4 + 24 ? e->isize - (128 + 32 + 4 + 24) : 0;
  cap[1] &= ~3u;
  for (uint32_t i = 0; i < n; i++) {
    uint32_t need = (8 + ents[i].nlen + 3) & ~3u;
    if (off[reg] + need > cap[reg]) {
      reg++;
      if (reg > 1 || off[reg] + need > cap[reg])
        abort();
    }
    eb_put_dent(e, base[reg] + off[reg], &ents[i], need);
    last[reg] = off[reg];
    off[reg] += need;
  }
  w16(base[0] + last[0] + 4, 56 - last[0]);
  uint32_t vlen = 0;
  if (reg == 1) {
    w16(base[1] + last[1] + 4, off[1] - last[1]);
    vlen = off[1];
  }
  eb_xattr_data(e, dino, region, vlen);
  eb_set_size(e, dino, 60 + vlen);
  w16(r + 26, 2);
}

/* ------------------------------------------------------------------ finish */

static void eb_gd_csum(struct eb *e, uint32_t g, uint8_t *gd)
{
  uint8_t g4[4];
  w32(g4, g);
  w16(gd + 0x1E, 0);
  if (e->metacsum) {
    uint32_t c = eb_crc32c(e->seed, g4, 4);
    c = eb_crc32c(c, gd, e->dsize);
    w16(gd + 0x1E, c & 0xFFFF);
  } else if (e->gdtcsum) {
    uint16_t c = eb_crc16(0xFFFF, e->uuid, 16);
    c = eb_crc16(c, g4, 4);
    c = eb_crc16(c, gd, 0x1E);
    if ((e->incompat & EB_INC_64BIT) && e->dsize > 32)
      c = eb_crc16(c, gd + 32, e->dsize - 32);
    w16(gd + 0x1E, c);
  }
}

static uint8_t *eb_finish(struct eb *e, size_t *len)
{
  for (uint32_t g = 0; g < e->groups; g++) {
    uint8_t *gd = eb_blk(e, eb_gd_block(e, g / e->dpb)) + (size_t)(g % e->dpb) * e->dsize;
    memset(gd, 0, e->dsize);
    w32(gd + 8, (uint32_t)e->itab[g]);
    if (e->dsize >= 64)
      w32(gd + 40, (uint32_t)(e->itab[g] >> 32));
    if (e->gflags[g] & EB_GD_BLOCK_UNINIT) {
      w32(gd, 0xDEADBEEFu);
      w32(gd + 4, 0xDEADBEEFu);
    }
    w16(gd + 18, e->gflags[g]);
    eb_gd_csum(e, g, gd);
  }
  if (e->metacsum)
    for (uint32_t i = 1; i <= e->groups * e->ipg; i++) {
      if (!e->ino_used[i])
        continue;
      uint8_t *r = eb_inode(e, i);
      uint32_t extra = e->isize > 128 ? r16(r + 128) : 0;
      w16(r + 0x7C, 0);
      if (e->isize > 128 && extra >= 4)
        w16(r + 0x82, 0);
      uint32_t c = eb_crc32c(eb_inode_seed(e, i), r, e->isize);
      w16(r + 0x7C, c & 0xFFFF);
      if (e->isize > 128 && extra >= 4)
        w16(r + 0x82, c >> 16);
    }
  uint8_t *sb = e->img + 1024;
  uint64_t bc = e->nblocks;
  w32(sb, e->groups * e->ipg);
  w32(sb + 4, (uint32_t)bc);
  w32(sb + 12, 100);
  w32(sb + 16, 100);
  w32(sb + 20, e->first_data);
  w32(sb + 24, __builtin_ctz(e->bs) - 10);
  w32(sb + 28, __builtin_ctz(e->bs) - 10);
  w32(sb + 32, e->bpg);
  w32(sb + 36, e->bpg);
  w32(sb + 40, e->ipg);
  w16(sb + 56, 0xEF53);
  w16(sb + 58, e->state ? e->state : 1);
  w32(sb + 76, 1);
  w32(sb + 84, 11);
  w16(sb + 88, e->isize);
  w32(sb + 92, e->compat);
  w32(sb + 96, e->incompat);
  w32(sb + 100, e->ro_compat);
  memcpy(sb + 104, e->uuid, 16);
  memcpy(sb + 120, "ebvol", 5);
  w32(sb + 0xE0, e->journal_inum);
  w32(sb + 0xE8, e->last_orphan);
  w16(sb + 0xFE, e->incompat & EB_INC_64BIT ? e->dsize : 0);
  w32(sb + 0x104, e->first_meta_bg);
  w32(sb + 0x150, (uint32_t)(bc >> 32));
  w32(sb + 0x24C, e->backup[0]);
  w32(sb + 0x250, e->backup[1]);
  sb[0x175] = e->metacsum ? 1 : 0;
  if (e->incompat & EB_INC_CSUM_SEED)
    w32(sb + 0x270, e->csum_seed_val);
  if (e->metacsum)
    w32(sb + 0x3FC, eb_crc32c(~0u, sb, 0x3FC));
  *len = (size_t)e->nblocks * e->bs;
  return e->img;
}

#endif
