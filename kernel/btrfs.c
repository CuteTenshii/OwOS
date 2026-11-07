/* btrfs.c: read-only btrfs driver (see btrfs.h).
 * Little-endian; logical addresses go through the chunk map, tree nodes through an LRU cache, and every offset,
 * count and length read from disk is bounds-checked. Tree walks are iterative with strictly increasing keys. */

#include <stdbool.h>
#include <stddef.h>

#include "btrfs.h"
#include "console.h"
#include "crc32c.h"
#include "endian.h"
#include "gzip.h"
#include "heap.h"
#include "kstring.h"
#include "lzo.h"
#include "zstd.h"

#define BT_SB_OFFSET 0x10000ull
#define BT_SB_SIZE 4096u
#define BT_MAX_LEVEL 8u
#define BT_HDR_SIZE 101u
#define BT_ITEM_SIZE 25u
#define BT_KEYPTR_SIZE 33u
#define BT_DIR_ITEM_SIZE 30u
#define BT_MIN_NODESIZE 4096u
#define BT_MAX_NODESIZE 65536u
#define BT_MAX_NAME 255u
#define BT_MAX_LINK 4095u
#define BT_MAX_SYMLINKS 40u
#define BT_MAX_COMPONENTS 2048u
#define BT_MAX_RAM (128u * 1024u)
#define BT_MAX_COMP (256u * 1024u)
#define BT_MAX_COPIES 4u
#define BT_MAX_CHUNKS (1u << 22)
#define BT_MAX_STRIPES (1u << 23)
#define BT_CACHE_BUDGET (2u * 1024u * 1024u)
#define BT_HASH_BUCKETS 256u

#define BT_FS_TREE 5ull
#define BT_ROOT_TREE_DIR 6ull
#define BT_FIRST_FREE_INO 256ull

#define BT_K_INODE_ITEM 1u
#define BT_K_INODE_REF 12u
#define BT_K_INODE_EXTREF 13u
#define BT_K_DIR_ITEM 84u
#define BT_K_DIR_INDEX 96u
#define BT_K_EXTENT_DATA 108u
#define BT_K_ROOT_ITEM 132u
#define BT_K_ROOT_BACKREF 144u
#define BT_K_CHUNK_ITEM 228u

#define BT_BG_RAID0 0x08ull
#define BT_BG_RAID1 0x10ull
#define BT_BG_DUP 0x20ull
#define BT_BG_RAID10 0x40ull
#define BT_BG_RAID5 0x80ull
#define BT_BG_RAID6 0x100ull
#define BT_BG_RAID1C3 0x200ull
#define BT_BG_RAID1C4 0x400ull
#define BT_BG_PROFILE_MASK                                                                                             \
  (BT_BG_RAID0 | BT_BG_RAID1 | BT_BG_DUP | BT_BG_RAID10 | BT_BG_RAID5 | BT_BG_RAID6 | BT_BG_RAID1C3 | BT_BG_RAID1C4)

#define BT_INCOMPAT_METADATA_UUID 0x400ull
#define BT_INCOMPAT_SUPPORTED                                                                                          \
  (0x1ull | 0x2ull | 0x4ull | 0x8ull | 0x10ull | 0x20ull | 0x40ull | 0x80ull | 0x100ull | 0x200ull | 0x400ull |        \
   0x800ull | 0x8000ull)

#define BT_S_FMT 0170000u
#define BT_S_DIR 0040000u
#define BT_S_REG 0100000u
#define BT_S_LNK 0120000u

#define BT_FT_DIR 2u

#define BT_FE_INLINE 0u
#define BT_FE_REG 1u
#define BT_FE_PREALLOC 2u

enum { BT_MAP_OK = 0, BT_MAP_NOCOPY, BT_MAP_MISSING };
enum { BT_X_ZERO = 0, BT_X_REG, BT_X_INLINE };

struct bt_key {
  uint64_t objectid;
  uint8_t type;
  uint64_t offset;
};

struct bt_stripe {
  uint64_t devid, offset;
};

struct bt_chunk {
  uint64_t logical, length, type, stripe_len;
  uint32_t first, nstripes, sub_stripes;
};

struct bt_node {
  struct bt_node *next;
  uint64_t bytenr;
  uint32_t stamp;
  uint8_t data[];
};

struct bt_pl {
  uint64_t bytenr;
  uint32_t slot, nritems;
};

struct bt_path {
  uint32_t nlevels;
  struct bt_pl l[BT_MAX_LEVEL];
};

struct bt_vol {
  uint64_t id, bytenr, dirid;
};

struct bt_ref {
  struct bt_vol vol;
  uint64_t ino;
  struct btrfs_attr attr;
};

struct bt_extent {
  uint64_t start, end;
  uint64_t disk_bytenr, disk_len, eoff, ram;
  uint8_t kind, comp;
};

struct bt_dent {
  struct bt_key loc;
  uint32_t nlen;
  uint8_t ftype;
  char name[BT_MAX_NAME + 1];
};

struct bt_mount {
  blk_read_fn read;
  uint64_t base;
  uint32_t sectorsize, nodesize;
  uint8_t fsid[16];
  uint64_t devid;
  bool csum_crc32c, warned_csum;
  char label[256];

  struct bt_chunk *chunks;
  struct bt_stripe *stripes;
  uint32_t nchunks, cchunks, nstripes, cstripes;

  struct bt_node *hash[BT_HASH_BUCKETS];
  uint32_t cached_nodes, stamp;

  uint64_t root_tree;
  struct bt_vol top;

  bool xc_valid;
  uint64_t xc_vol, xc_ino;
  struct bt_extent xc;

  uint8_t *dec;
  bool dec_valid;
  uint64_t dec_disk, dec_len, dec_ram;
  uint8_t dec_comp;

  bool cur_valid;
  uint64_t cur_vol, cur_dir, cur_idx;
  uint32_t cur_n;
};

static uint64_t bt_divmod(uint64_t n, uint64_t d, uint64_t *rem)
{
  uint64_t q = 0, r = 0;
  for (int i = 63; i >= 0; i--) {
    r = (r << 1) | ((n >> i) & 1);
    if (r >= d) {
      r -= d;
      q |= 1ull << i;
    }
  }
  *rem = r;
  return q;
}

static int bt_grow(void **p, uint32_t *cap, uint32_t need, uint32_t limit, size_t esz)
{
  if (need <= *cap)
    return 0;
  if (need > limit)
    return -1;
  uint32_t ncap = *cap ? *cap : 16;
  while (ncap < need)
    ncap *= 2;
  if (ncap > limit)
    ncap = limit;
  void *n = kmalloc((size_t)ncap * esz);
  if (!n)
    return -1;
  if (*p) {
    memcpy(n, *p, (size_t)*cap * esz);
    kfree(*p);
  }
  *p = n;
  *cap = ncap;
  return 0;
}

static int bt_dev_read(struct bt_mount *m, uint64_t off, void *buf, uint32_t len)
{
  uint8_t *dst = buf;
  uint8_t s[512];
  while (len) {
    uint64_t lba = m->base + (off >> 9);
    uint32_t soff = (uint32_t)(off & 511), n = 512 - soff;
    if (lba > 0xFFFFFFFFull)
      return -1;
    if (n > len)
      n = len;
    if (n == 512) {
      if (m->read((uint32_t)lba, dst) != 0)
        return -1;
    } else {
      if (m->read((uint32_t)lba, s) != 0)
        return -1;
      memcpy(dst, s + soff, n);
    }
    dst += n;
    off += n;
    len -= n;
  }
  return 0;
}

static void bt_load_key(const uint8_t *p, struct bt_key *k)
{
  k->objectid = load_le64(p);
  k->type = p[8];
  k->offset = load_le64(p + 9);
}

static int bt_key_cmp(const struct bt_key *a, const struct bt_key *b)
{
  if (a->objectid != b->objectid)
    return a->objectid < b->objectid ? -1 : 1;
  if (a->type != b->type)
    return a->type < b->type ? -1 : 1;
  if (a->offset != b->offset)
    return a->offset < b->offset ? -1 : 1;
  return 0;
}

static int bt_disk_key_cmp(const uint8_t *dk, const struct bt_key *k)
{
  struct bt_key a;
  bt_load_key(dk, &a);
  return bt_key_cmp(&a, k);
}

/* ---------------------------------------------------------------- chunk map */

static uint32_t bt_ncopies(const struct bt_chunk *c)
{
  switch (c->type & BT_BG_PROFILE_MASK) {
  case BT_BG_RAID1:
  case BT_BG_DUP:
    return 2;
  case BT_BG_RAID1C3:
    return 3;
  case BT_BG_RAID1C4:
    return 4;
  case BT_BG_RAID10:
    return c->sub_stripes;
  default:
    return 1;
  }
}

/* Index of the last chunk with logical <= addr, or -1. */
static int64_t bt_chunk_floor(const struct bt_mount *m, uint64_t addr)
{
  uint32_t lo = 0, hi = m->nchunks;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (m->chunks[mid].logical <= addr)
      lo = mid + 1;
    else
      hi = mid;
  }
  return (int64_t)lo - 1;
}

/* c points at a btrfs_chunk with avail bytes behind it. 0 on success, -1 when the chunk is unusable. */
static int bt_chunk_add(struct bt_mount *m, uint64_t logical, const uint8_t *c, uint32_t avail)
{
  if (avail < 48)
    return -1;
  uint64_t length = load_le64(c), stripe_len = load_le64(c + 16), type = load_le64(c + 24);
  uint32_t ns = load_le16(c + 44), sub = load_le16(c + 46);
  uint64_t profile = type & BT_BG_PROFILE_MASK;

  if (profile & (BT_BG_RAID5 | BT_BG_RAID6)) {
    con_printf("btrfs: RAID5/6 is not supported\n");
    return -1;
  }
  if (ns == 0 || 48 + 32ull * ns > avail || length == 0 || logical + length < logical)
    return -1;
  if (profile & (profile - 1))
    return -1;
  if ((profile == BT_BG_RAID0 && ns < 2) || ((profile == BT_BG_RAID1 || profile == BT_BG_DUP) && ns < 2) ||
      (profile == BT_BG_RAID1C3 && ns < 3) || (profile == BT_BG_RAID1C4 && ns < 4))
    return -1;
  if (profile == BT_BG_RAID10 && (sub < 2 || ns < sub || ns % sub != 0))
    return -1;
  if ((profile == BT_BG_RAID0 || profile == BT_BG_RAID10) && (stripe_len == 0 || stripe_len > 0xFFFFFFFFull))
    return -1;

  int64_t at = bt_chunk_floor(m, logical);
  if (at >= 0 && m->chunks[at].logical == logical)
    return m->chunks[at].length == length ? 0 : -1;
  if (at >= 0 && m->chunks[at].logical + m->chunks[at].length > logical)
    return -1;
  if ((uint32_t)(at + 1) < m->nchunks && logical + length > m->chunks[at + 1].logical)
    return -1;

  if (bt_grow((void **)&m->chunks, &m->cchunks, m->nchunks + 1, BT_MAX_CHUNKS, sizeof *m->chunks) != 0 ||
      bt_grow((void **)&m->stripes, &m->cstripes, m->nstripes + ns, BT_MAX_STRIPES, sizeof *m->stripes) != 0)
    return -1;
  for (uint32_t i = 0; i < ns; i++) {
    m->stripes[m->nstripes + i].devid = load_le64(c + 48 + 32 * i);
    m->stripes[m->nstripes + i].offset = load_le64(c + 48 + 32 * i + 8);
  }
  uint32_t pos = (uint32_t)(at + 1);
  memmove(&m->chunks[pos + 1], &m->chunks[pos], (m->nchunks - pos) * sizeof *m->chunks);
  struct bt_chunk *ch = &m->chunks[pos];
  ch->logical = logical;
  ch->length = length;
  ch->type = type;
  ch->stripe_len = stripe_len;
  ch->first = m->nstripes;
  ch->nstripes = ns;
  ch->sub_stripes = sub;
  m->nstripes += ns;
  m->nchunks++;
  return 0;
}

/* Physical byte offset of logical address addr on copy `copy`; *run is how many bytes stay contiguous. */
static int bt_map(const struct bt_mount *m, uint64_t addr, uint32_t copy, uint64_t *phys, uint64_t *run)
{
  int64_t at = bt_chunk_floor(m, addr);
  if (at < 0)
    return BT_MAP_NOCOPY;
  const struct bt_chunk *c = &m->chunks[at];
  uint64_t off = addr - c->logical;
  if (off >= c->length || copy >= bt_ncopies(c))
    return BT_MAP_NOCOPY;
  const struct bt_stripe *st = &m->stripes[c->first];
  uint64_t profile = c->type & BT_BG_PROFILE_MASK, sidx, soff, rem;

  if (profile == BT_BG_RAID0 || profile == BT_BG_RAID10) {
    uint64_t nr = bt_divmod(off, c->stripe_len, &rem), q;
    uint32_t groups = profile == BT_BG_RAID0 ? c->nstripes : c->nstripes / c->sub_stripes;
    q = bt_divmod(nr, groups, &sidx);
    soff = q * c->stripe_len + rem;
    if (profile == BT_BG_RAID10)
      sidx = sidx * c->sub_stripes + copy;
    *run = c->stripe_len - rem;
  } else {
    sidx = copy;
    soff = off;
    *run = c->length - off;
  }
  if (sidx >= c->nstripes)
    return BT_MAP_NOCOPY;
  st += sidx;
  if (st->devid != m->devid)
    return BT_MAP_MISSING;
  if (st->offset + soff < st->offset)
    return BT_MAP_NOCOPY;
  *phys = st->offset + soff;
  return BT_MAP_OK;
}

/* Read len bytes at a logical address, trying every copy of each run. */
static int bt_read_logical(struct bt_mount *m, uint64_t logical, uint8_t *buf, uint32_t len)
{
  while (len) {
    bool ok = false;
    uint32_t n = 0;
    for (uint32_t copy = 0; copy < BT_MAX_COPIES; copy++) {
      uint64_t phys, run;
      int r = bt_map(m, logical, copy, &phys, &run);
      if (r == BT_MAP_NOCOPY)
        break;
      if (r == BT_MAP_MISSING)
        continue;
      n = run < len ? (uint32_t)run : len;
      if (bt_dev_read(m, phys, buf, n) == 0) {
        ok = true;
        break;
      }
    }
    if (!ok)
      return -1;
    logical += n;
    buf += n;
    len -= n;
  }
  return 0;
}

/* ---------------------------------------------------------------- node cache */

static bool bt_node_csum_ok(const struct bt_mount *m, const uint8_t *d)
{
  return !m->csum_crc32c || load_le32(d) == ~crc32c_raw(0xFFFFFFFFu, d + 32, m->nodesize - 32);
}

static bool bt_node_header_ok(const struct bt_mount *m, const uint8_t *d, uint64_t bytenr)
{
  if (load_le64(d + 48) != bytenr || memcmp(d + 32, m->fsid, 16) != 0)
    return false;
  uint32_t n = load_le32(d + 96), level = d[100];
  if (level >= BT_MAX_LEVEL)
    return false;
  if (level == 0)
    return (uint64_t)BT_HDR_SIZE + (uint64_t)n * BT_ITEM_SIZE <= m->nodesize;
  return n != 0 && (uint64_t)BT_HDR_SIZE + (uint64_t)n * BT_KEYPTR_SIZE <= m->nodesize;
}

static void bt_cache_evict(struct bt_mount *m)
{
  struct bt_node **best = NULL;
  uint32_t best_age = 0;
  for (uint32_t b = 0; b < BT_HASH_BUCKETS; b++) {
    for (struct bt_node **pp = &m->hash[b]; *pp; pp = &(*pp)->next) {
      uint32_t age = m->stamp - (*pp)->stamp;
      if (!best || age > best_age) {
        best = pp;
        best_age = age;
      }
    }
  }
  if (!best)
    return;
  struct bt_node *victim = *best;
  *best = victim->next;
  kfree(victim);
  m->cached_nodes--;
}

/* The validated node at logical bytenr (level checked when expect >= 0), or NULL. The pointer is valid until the
 * next cache call. */
static const uint8_t *bt_node_get(struct bt_mount *m, uint64_t bytenr, int expect)
{
  uint32_t h = (uint32_t)(((bytenr / BT_MIN_NODESIZE) * 2654435761u) >> 8) % BT_HASH_BUCKETS;
  for (struct bt_node *n = m->hash[h]; n; n = n->next) {
    if (n->bytenr == bytenr) {
      n->stamp = ++m->stamp;
      return expect >= 0 && n->data[100] != expect ? NULL : n->data;
    }
  }
  if (bytenr & (m->sectorsize - 1))
    return NULL;

  while (m->cached_nodes && (uint64_t)(m->cached_nodes + 1) * m->nodesize > BT_CACHE_BUDGET)
    bt_cache_evict(m);
  struct bt_node *n = kmalloc(sizeof *n + m->nodesize);
  if (!n)
    return NULL;

  int good = -1, fallback = -1;
  for (uint32_t copy = 0; copy < BT_MAX_COPIES && good < 0; copy++) {
    uint64_t phys, run;
    int r = bt_map(m, bytenr, copy, &phys, &run);
    if (r == BT_MAP_NOCOPY)
      break;
    if (r == BT_MAP_MISSING || run < m->nodesize || bt_dev_read(m, phys, n->data, m->nodesize) != 0)
      continue;
    if (!bt_node_header_ok(m, n->data, bytenr))
      continue;
    if (bt_node_csum_ok(m, n->data))
      good = (int)copy;
    else if (fallback < 0)
      fallback = (int)copy;
  }
  if (good < 0 && fallback >= 0) {
    uint64_t phys, run;
    if (bt_map(m, bytenr, (uint32_t)fallback, &phys, &run) == BT_MAP_OK &&
        bt_dev_read(m, phys, n->data, m->nodesize) == 0) {
      good = fallback;
      if (!m->warned_csum) {
        m->warned_csum = true;
        con_printf("btrfs: metadata checksum mismatch, continuing\n");
      }
    }
  }
  if (good < 0 || (expect >= 0 && n->data[100] != expect)) {
    kfree(n);
    return NULL;
  }
  n->bytenr = bytenr;
  n->stamp = ++m->stamp;
  n->next = m->hash[h];
  m->hash[h] = n;
  m->cached_nodes++;
  return n->data;
}

/* ---------------------------------------------------------------- tree paths */

/* Position on the first item >= key. 0 exact match, 1 positioned past it (slot may equal nritems), -1 error. */
static int bt_search(struct bt_mount *m, uint64_t root, const struct bt_key *key, struct bt_path *p)
{
  const uint8_t *node = bt_node_get(m, root, -1);
  if (!node)
    return -1;
  uint32_t level = node[100];
  uint64_t bytenr = root;
  p->nlevels = level + 1;

  for (;;) {
    uint32_t n = load_le32(node + 96), lo = 0, hi = n;
    if (level == 0) {
      while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (bt_disk_key_cmp(node + BT_HDR_SIZE + mid * BT_ITEM_SIZE, key) < 0)
          lo = mid + 1;
        else
          hi = mid;
      }
      p->l[0].bytenr = bytenr;
      p->l[0].slot = lo;
      p->l[0].nritems = n;
      return lo < n && bt_disk_key_cmp(node + BT_HDR_SIZE + lo * BT_ITEM_SIZE, key) == 0 ? 0 : 1;
    }
    while (lo < hi) {
      uint32_t mid = lo + (hi - lo) / 2;
      if (bt_disk_key_cmp(node + BT_HDR_SIZE + mid * BT_KEYPTR_SIZE, key) <= 0)
        lo = mid + 1;
      else
        hi = mid;
    }
    uint32_t slot = lo ? lo - 1 : 0;
    p->l[level].bytenr = bytenr;
    p->l[level].slot = slot;
    p->l[level].nritems = n;
    bytenr = load_le64(node + BT_HDR_SIZE + slot * BT_KEYPTR_SIZE + 17);
    level--;
    node = bt_node_get(m, bytenr, (int)level);
    if (!node)
      return -1;
  }
}

/* The current item of a path; key, data and size are optional. Data is valid until the next cache call. */
static int bt_cur(struct bt_mount *m, const struct bt_path *p, struct bt_key *key, const uint8_t **data, uint32_t *size)
{
  const uint8_t *node = bt_node_get(m, p->l[0].bytenr, 0);
  if (!node || p->l[0].slot >= load_le32(node + 96))
    return -1;
  const uint8_t *it = node + BT_HDR_SIZE + p->l[0].slot * BT_ITEM_SIZE;
  uint32_t off = load_le32(it + 17), sz = load_le32(it + 21);
  if ((uint64_t)BT_HDR_SIZE + off + sz > m->nodesize)
    return -1;
  if (key)
    bt_load_key(it, key);
  if (data)
    *data = node + BT_HDR_SIZE + off;
  if (size)
    *size = sz;
  return 0;
}

static int bt_down(struct bt_mount *m, struct bt_path *p, uint32_t lvl, bool leftmost)
{
  for (; lvl > 0; lvl--) {
    const uint8_t *node = bt_node_get(m, p->l[lvl].bytenr, (int)lvl);
    if (!node || p->l[lvl].slot >= load_le32(node + 96))
      return -1;
    uint64_t child = load_le64(node + BT_HDR_SIZE + p->l[lvl].slot * BT_KEYPTR_SIZE + 17);
    const uint8_t *c = bt_node_get(m, child, (int)lvl - 1);
    if (!c)
      return -1;
    uint32_t n = load_le32(c + 96);
    if (lvl == 1 && n == 0)
      return -1;
    p->l[lvl - 1].bytenr = child;
    p->l[lvl - 1].nritems = n;
    p->l[lvl - 1].slot = leftmost ? 0 : n - 1;
  }
  return 0;
}

/* Advance to the next item. 0 moved, 1 end of tree, -1 error (including keys that fail to increase). */
static int bt_next_item(struct bt_mount *m, struct bt_path *p)
{
  struct bt_key prev, cur;
  if (bt_cur(m, p, &prev, NULL, NULL) != 0)
    return -1;
  if (p->l[0].slot + 1 < p->l[0].nritems) {
    p->l[0].slot++;
  } else {
    uint32_t lvl = 1;
    while (lvl < p->nlevels && p->l[lvl].slot + 1 >= p->l[lvl].nritems)
      lvl++;
    if (lvl >= p->nlevels)
      return 1;
    p->l[lvl].slot++;
    if (bt_down(m, p, lvl, true) != 0)
      return -1;
  }
  if (bt_cur(m, p, &cur, NULL, NULL) != 0)
    return -1;
  return bt_key_cmp(&cur, &prev) > 0 ? 0 : -1;
}

static int bt_prev_item(struct bt_mount *m, struct bt_path *p)
{
  if (p->l[0].slot > 0) {
    p->l[0].slot--;
    return 0;
  }
  uint32_t lvl = 1;
  while (lvl < p->nlevels && p->l[lvl].slot == 0)
    lvl++;
  if (lvl >= p->nlevels)
    return 1;
  p->l[lvl].slot--;
  return bt_down(m, p, lvl, false);
}

/* Position on the first item >= key. 0 positioned, 1 no such item, -1 error. */
static int bt_seek_ge(struct bt_mount *m, uint64_t root, const struct bt_key *key, struct bt_path *p)
{
  int r = bt_search(m, root, key, p);
  if (r <= 0)
    return r;
  if (p->l[0].slot < p->l[0].nritems)
    return 0;
  if (p->l[0].nritems == 0)
    return 1;
  p->l[0].slot = p->l[0].nritems - 1;
  return bt_next_item(m, p);
}

/* Position on the last item <= key. 0 positioned, 1 no such item, -1 error. */
static int bt_seek_le(struct bt_mount *m, uint64_t root, const struct bt_key *key, struct bt_path *p)
{
  int r = bt_search(m, root, key, p);
  if (r <= 0)
    return r;
  if (p->l[0].slot > 0) {
    p->l[0].slot--;
    return 0;
  }
  return p->l[0].nritems == 0 ? 1 : bt_prev_item(m, p);
}

/* ---------------------------------------------------------------- inodes, roots, directories */

static int bt_inode_get(struct bt_mount *m, const struct bt_vol *v, uint64_t ino, struct btrfs_attr *a)
{
  struct bt_path p;
  struct bt_key key = {ino, BT_K_INODE_ITEM, 0};
  const uint8_t *d;
  uint32_t sz;
  if (bt_search(m, v->bytenr, &key, &p) != 0 || bt_cur(m, &p, NULL, &d, &sz) != 0 || sz < 56)
    return -1;
  memset(a, 0, sizeof *a);
  a->size = load_le64(d + 16);
  a->nlink = load_le32(d + 40);
  a->uid = load_le32(d + 44);
  a->gid = load_le32(d + 48);
  a->mode = load_le32(d + 52);
  if (sz >= 160) {
    a->atime = (int64_t)load_le64(d + 112);
    a->atime_ns = load_le32(d + 120);
    a->ctime = (int64_t)load_le64(d + 124);
    a->ctime_ns = load_le32(d + 132);
    a->mtime = (int64_t)load_le64(d + 136);
    a->mtime_ns = load_le32(d + 144);
    a->otime = (int64_t)load_le64(d + 148);
    a->otime_ns = load_le32(d + 156);
  }
  return 0;
}

static int bt_vol_open(struct bt_mount *m, uint64_t id, struct bt_vol *v)
{
  struct bt_path p;
  struct bt_key key = {id, BT_K_ROOT_ITEM, ~0ull}, k;
  const uint8_t *d;
  uint32_t sz;
  if (bt_seek_le(m, m->root_tree, &key, &p) != 0 || bt_cur(m, &p, &k, &d, &sz) != 0)
    return -1;
  if (k.objectid != id || k.type != BT_K_ROOT_ITEM || sz < 184)
    return -1;
  v->id = id;
  v->dirid = load_le64(d + 168) ? load_le64(d + 168) : BT_FIRST_FREE_INO;
  v->bytenr = load_le64(d + 176);
  return v->bytenr ? 0 : -1;
}

static bool bt_name_ok(const uint8_t *name, uint32_t len)
{
  if (len == 0 || len > BT_MAX_NAME)
    return false;
  for (uint32_t i = 0; i < len; i++)
    if (name[i] == 0 || name[i] == '/')
      return false;
  return !(name[0] == '.' && (len == 1 || (len == 2 && name[1] == '.')));
}

/* Parse the first dir_item at d. */
static bool bt_dir_item(const uint8_t *d, uint32_t sz, struct bt_dent *e)
{
  if (sz < BT_DIR_ITEM_SIZE)
    return false;
  uint32_t dl = load_le16(d + 25), nl = load_le16(d + 27);
  if ((uint64_t)BT_DIR_ITEM_SIZE + nl + dl > sz || !bt_name_ok(d + BT_DIR_ITEM_SIZE, nl))
    return false;
  bt_load_key(d, &e->loc);
  if (e->loc.type != BT_K_INODE_ITEM && e->loc.type != BT_K_ROOT_ITEM)
    return false;
  e->ftype = d[29];
  e->nlen = nl;
  memcpy(e->name, d + BT_DIR_ITEM_SIZE, nl);
  e->name[nl] = '\0';
  return true;
}

/* Find name in directory dir by hash. 0 found, 1 absent, -1 error. */
static int bt_dir_find(struct bt_mount *m, const struct bt_vol *v, uint64_t dir, const char *name, uint32_t nlen,
                       struct bt_dent *out)
{
  struct bt_path p;
  struct bt_key key = {dir, BT_K_DIR_ITEM, crc32c_raw(~1u, name, nlen)};
  const uint8_t *d;
  uint32_t sz;
  int r = bt_search(m, v->bytenr, &key, &p);
  if (r < 0)
    return -1;
  if (r > 0)
    return 1;
  if (bt_cur(m, &p, NULL, &d, &sz) != 0)
    return -1;
  for (uint32_t off = 0; off + BT_DIR_ITEM_SIZE <= sz;) {
    uint32_t dl = load_le16(d + off + 25), nl = load_le16(d + off + 27);
    if ((uint64_t)off + BT_DIR_ITEM_SIZE + nl + dl > sz)
      return -1;
    if (nl == nlen && memcmp(d + off + BT_DIR_ITEM_SIZE, name, nlen) == 0)
      return bt_dir_item(d + off, sz - off, out) ? 0 : -1;
    off += BT_DIR_ITEM_SIZE + nl + dl;
  }
  return 1;
}

/* The n-th entry of a directory in DIR_INDEX order, resuming from the last position when possible.
 * 1 found, 0 no more entries, -1 error. */
static int bt_dir_read(struct bt_mount *m, const struct bt_ref *dir, uint32_t n, struct bt_dent *out)
{
  uint32_t skip = n;
  uint64_t start = 0;
  if (m->cur_valid && m->cur_vol == dir->vol.id && m->cur_dir == dir->ino && m->cur_n <= n) {
    skip = n - m->cur_n;
    start = m->cur_idx;
  }
  struct bt_path p;
  struct bt_key key = {dir->ino, BT_K_DIR_INDEX, start}, k;
  int r = bt_seek_ge(m, dir->vol.bytenr, &key, &p);
  while (r == 0) {
    const uint8_t *d;
    uint32_t sz;
    if (bt_cur(m, &p, &k, &d, &sz) != 0)
      return -1;
    if (k.objectid != dir->ino || k.type != BT_K_DIR_INDEX)
      return 0;
    if (bt_dir_item(d, sz, out)) {
      if (skip == 0) {
        m->cur_valid = k.offset != ~0ull;
        m->cur_vol = dir->vol.id;
        m->cur_dir = dir->ino;
        m->cur_n = n + 1;
        m->cur_idx = k.offset + 1;
        return 1;
      }
      skip--;
    }
    r = bt_next_item(m, &p);
  }
  return r < 0 ? -1 : 0;
}

/* ---------------------------------------------------------------- file data */

static int bt_decompress(const struct bt_mount *m, uint8_t comp, const uint8_t *src, uint32_t slen, uint8_t *dst,
                         uint32_t ram)
{
  int64_t out = -1;
  if (comp == 1) {
    out = zlib_inflate(src, slen, dst, ram);
  } else if (comp == 3) {
    out = zstd_decompress(src, slen, dst, ram);
  } else if (comp == 2) {
    uint32_t total = slen >= 4 ? load_le32(src) : 0, in = 4, produced = 0;
    if (total < 4 || total > slen)
      return -1;
    while (in < total) {
      if (total - in < 4)
        return -1;
      uint32_t seg = load_le32(src + in), dl;
      in += 4;
      if (seg == 0 || seg > total - in)
        return -1;
      uint32_t cap = ram - produced < 65536 ? ram - produced : 65536;
      if (lzo1x_unpack(src + in, seg, dst + produced, cap, &dl) != 0)
        return -1;
      produced += dl;
      in += seg;
      uint32_t left = m->sectorsize - (in & (m->sectorsize - 1));
      if (left < 4)
        in += left;
    }
    out = produced;
  }
  if (out < 0 || (uint64_t)out > ram)
    return -1;
  memset(dst + out, 0, ram - (uint32_t)out);
  return 0;
}

static int bt_parse_extent(uint64_t koff, const uint8_t *d, uint32_t sz, struct bt_extent *e)
{
  if (sz < 21)
    return -1;
  uint8_t type = d[20], comp = d[16];
  uint64_t ram = load_le64(d + 8);
  if (d[17] || load_le16(d + 18) || comp > 3)
    return -1;
  memset(e, 0, sizeof *e);
  e->start = koff;
  e->comp = comp;
  e->ram = ram;

  if (type == BT_FE_INLINE) {
    if (comp && ram > BT_MAX_RAM)
      return -1;
    uint64_t len = comp ? ram : sz - 21;
    if (koff + len < koff)
      return -1;
    e->end = koff + len;
    e->kind = BT_X_INLINE;
    return 0;
  }
  if (type > BT_FE_PREALLOC || sz < 53)
    return -1;
  uint64_t disk = load_le64(d + 21), dlen = load_le64(d + 29), eoff = load_le64(d + 37), nb = load_le64(d + 45);
  if (nb == 0 || koff + nb < koff)
    return -1;
  e->end = koff + nb;
  if (type == BT_FE_PREALLOC || disk == 0) {
    e->kind = BT_X_ZERO;
    return 0;
  }
  if (eoff + nb < eoff || disk + dlen < disk)
    return -1;
  if (comp == 0 ? eoff + nb > dlen : (ram > BT_MAX_RAM || dlen == 0 || dlen > BT_MAX_COMP || eoff + nb > ram))
    return -1;
  e->kind = BT_X_REG;
  e->disk_bytenr = disk;
  e->disk_len = dlen;
  e->eoff = eoff;
  return 0;
}

/* The extent covering pos, or the hole up to the next extent. 0 or -1. */
static int bt_find_extent(struct bt_mount *m, const struct bt_vol *v, uint64_t ino, uint64_t pos, struct bt_extent *e)
{
  if (m->xc_valid && m->xc_vol == v->id && m->xc_ino == ino && pos >= m->xc.start && pos < m->xc.end) {
    *e = m->xc;
    return 0;
  }
  struct bt_path p;
  struct bt_key key = {ino, BT_K_EXTENT_DATA, pos}, k;
  const uint8_t *d;
  uint32_t sz;
  int r = bt_seek_le(m, v->bytenr, &key, &p);
  if (r < 0)
    return -1;
  if (r == 0) {
    if (bt_cur(m, &p, &k, &d, &sz) != 0)
      return -1;
    if (k.objectid == ino && k.type == BT_K_EXTENT_DATA) {
      if (bt_parse_extent(k.offset, d, sz, e) != 0)
        return -1;
      if (pos < e->end) {
        if (e->kind != BT_X_INLINE) {
          m->xc_valid = true;
          m->xc_vol = v->id;
          m->xc_ino = ino;
          m->xc = *e;
        }
        return 0;
      }
    }
  }
  uint64_t gap_end = ~0ull;
  r = bt_seek_ge(m, v->bytenr, &key, &p);
  if (r < 0)
    return -1;
  if (r == 0) {
    if (bt_cur(m, &p, &k, NULL, NULL) != 0)
      return -1;
    if (k.objectid == ino && k.type == BT_K_EXTENT_DATA) {
      if (k.offset <= pos)
        return -1;
      gap_end = k.offset;
    }
  }
  memset(e, 0, sizeof *e);
  e->start = pos;
  e->end = gap_end;
  e->kind = BT_X_ZERO;
  return 0;
}

static int bt_decode_into_cache(struct bt_mount *m, uint8_t comp, const uint8_t *src, uint32_t slen, uint32_t ram)
{
  m->dec_valid = false;
  if (!m->dec) {
    m->dec = kmalloc(BT_MAX_RAM);
    if (!m->dec)
      return -1;
  }
  return bt_decompress(m, comp, src, slen, m->dec, ram);
}

/* Copy n bytes of extent e starting at file position pos into buf. */
static int bt_extent_read(struct bt_mount *m, const struct bt_vol *v, uint64_t ino, const struct bt_extent *e,
                          uint64_t pos, uint8_t *buf, uint32_t n)
{
  uint64_t rel = pos - e->start;

  if (e->kind == BT_X_ZERO) {
    memset(buf, 0, n);
    return 0;
  }
  if (e->kind == BT_X_REG) {
    if (e->comp == 0)
      return bt_read_logical(m, e->disk_bytenr + e->eoff + rel, buf, n);
    if (!(m->dec_valid && m->dec_disk == e->disk_bytenr && m->dec_len == e->disk_len && m->dec_ram == e->ram &&
          m->dec_comp == e->comp)) {
      uint8_t *tmp = kmalloc((size_t)e->disk_len);
      if (!tmp)
        return -1;
      int r = bt_read_logical(m, e->disk_bytenr, tmp, (uint32_t)e->disk_len);
      if (r == 0)
        r = bt_decode_into_cache(m, e->comp, tmp, (uint32_t)e->disk_len, (uint32_t)e->ram);
      kfree(tmp);
      if (r != 0)
        return -1;
      m->dec_valid = true;
      m->dec_disk = e->disk_bytenr;
      m->dec_len = e->disk_len;
      m->dec_ram = e->ram;
      m->dec_comp = e->comp;
    }
    memcpy(buf, m->dec + e->eoff + rel, n);
    return 0;
  }

  struct bt_path p;
  struct bt_key key = {ino, BT_K_EXTENT_DATA, e->start};
  const uint8_t *d;
  uint32_t sz;
  if (bt_search(m, v->bytenr, &key, &p) != 0 || bt_cur(m, &p, NULL, &d, &sz) != 0 || sz < 21)
    return -1;
  if (e->comp == 0) {
    if (rel + n > sz - 21)
      return -1;
    memcpy(buf, d + 21 + rel, n);
    return 0;
  }
  if (bt_decode_into_cache(m, e->comp, d + 21, sz - 21, (uint32_t)e->ram) != 0)
    return -1;
  memcpy(buf, m->dec + rel, n);
  return 0;
}

/* Read len bytes at off of a file (the caller clamps to the file size). Returns bytes read, or -1 if none. */
static int bt_file_read(struct bt_mount *m, const struct bt_vol *v, uint64_t ino, uint64_t off, uint8_t *buf,
                        uint32_t len)
{
  uint32_t done = 0;
  while (done < len) {
    uint64_t pos = off + done;
    struct bt_extent e;
    if (bt_find_extent(m, v, ino, pos, &e) != 0)
      return done ? (int)done : -1;
    uint64_t span = e.end - pos;
    uint32_t n = span < len - done ? (uint32_t)span : len - done;
    if (bt_extent_read(m, v, ino, &e, pos, buf + done, n) != 0)
      return done ? (int)done : -1;
    done += n;
  }
  return (int)done;
}

/* ---------------------------------------------------------------- path resolution */

/* Step '..' from cur, staying inside the mounted volume. */
static int bt_parent(struct bt_mount *m, struct bt_ref *cur)
{
  struct bt_path p;
  struct bt_key key, k;
  const uint8_t *d;
  uint32_t sz;
  struct bt_ref up = *cur;

  if (cur->ino == cur->vol.dirid) {
    if (cur->vol.id == m->top.id)
      return 0;
    key = (struct bt_key){cur->vol.id, BT_K_ROOT_BACKREF, 0};
    if (bt_seek_ge(m, m->root_tree, &key, &p) != 0 || bt_cur(m, &p, &k, &d, &sz) != 0)
      return -1;
    if (k.objectid != cur->vol.id || k.type != BT_K_ROOT_BACKREF || sz < 18)
      return -1;
    up.ino = load_le64(d);
    if (bt_vol_open(m, k.offset, &up.vol) != 0)
      return -1;
  } else {
    key = (struct bt_key){cur->ino, BT_K_INODE_REF, 0};
    if (bt_seek_ge(m, cur->vol.bytenr, &key, &p) != 0 || bt_cur(m, &p, &k, &d, &sz) != 0)
      return -1;
    if (k.objectid != cur->ino)
      return -1;
    if (k.type == BT_K_INODE_REF)
      up.ino = k.offset;
    else if (k.type == BT_K_INODE_EXTREF && sz >= 8)
      up.ino = load_le64(d);
    else
      return -1;
  }
  if (bt_inode_get(m, &up.vol, up.ino, &up.attr) != 0)
    return -1;
  *cur = up;
  return 0;
}

static int bt_root_ref(struct bt_mount *m, struct bt_ref *r)
{
  r->vol = m->top;
  r->ino = m->top.dirid;
  return bt_inode_get(m, &r->vol, r->ino, &r->attr);
}

/* Resolve path to an object; symlinks (the last one only when follow_last) are expanded, at most 40 deep. */
static int bt_resolve(struct bt_mount *m, const char *path, bool follow_last, struct bt_ref *out)
{
  struct bt_ref cur;
  char *buf = NULL;
  const char *p = path ? path : "";
  uint32_t depth = 0, steps = 0;
  int ret = -1;

  if (bt_root_ref(m, &cur) != 0)
    return -1;
  for (;;) {
    while (*p == '/')
      p++;
    if (!*p)
      break;
    const char *name = p;
    uint32_t len = 0;
    while (p[len] && p[len] != '/')
      len++;
    p += len;
    if (++steps > BT_MAX_COMPONENTS || (cur.attr.mode & BT_S_FMT) != BT_S_DIR || len > BT_MAX_NAME)
      goto out;
    if (len == 1 && name[0] == '.')
      continue;
    if (len == 2 && name[0] == '.' && name[1] == '.') {
      if (bt_parent(m, &cur) != 0)
        goto out;
      continue;
    }

    struct bt_dent ent;
    if (bt_dir_find(m, &cur.vol, cur.ino, name, len, &ent) != 0)
      goto out;
    struct bt_ref child = cur;
    if (ent.loc.type == BT_K_ROOT_ITEM) {
      if (bt_vol_open(m, ent.loc.objectid, &child.vol) != 0)
        goto out;
      child.ino = child.vol.dirid;
    } else {
      child.ino = ent.loc.objectid;
    }
    if (bt_inode_get(m, &child.vol, child.ino, &child.attr) != 0)
      goto out;

    const char *q = p;
    while (*q == '/')
      q++;
    if ((child.attr.mode & BT_S_FMT) == BT_S_LNK && (*q || follow_last)) {
      uint64_t tl = child.attr.size;
      if (++depth > BT_MAX_SYMLINKS || tl == 0 || tl > BT_MAX_LINK)
        goto out;
      size_t rl = strlen(q);
      char *nb = kmalloc(tl + 1 + rl + 1);
      if (!nb)
        goto out;
      if (bt_file_read(m, &child.vol, child.ino, 0, (uint8_t *)nb, (uint32_t)tl) != (int)tl) {
        kfree(nb);
        goto out;
      }
      nb[tl] = '/';
      memcpy(nb + tl + 1, q, rl + 1);
      kfree(buf);
      buf = nb;
      p = nb;
      if (nb[0] == '/' && bt_root_ref(m, &cur) != 0)
        goto out;
      continue;
    }
    cur = child;
  }
  *out = cur;
  ret = 0;
out:
  kfree(buf);
  return ret;
}

/* ---------------------------------------------------------------- mounting */

static void bt_destroy(struct bt_mount *m)
{
  for (uint32_t b = 0; b < BT_HASH_BUCKETS; b++) {
    for (struct bt_node *n = m->hash[b], *next; n; n = next) {
      next = n->next;
      kfree(n);
    }
  }
  kfree(m->chunks);
  kfree(m->stripes);
  kfree(m->dec);
  kfree(m);
}

static bool bt_sb_valid(struct bt_mount *m, uint64_t off, uint8_t *sb)
{
  if (bt_dev_read(m, off, sb, BT_SB_SIZE) != 0 || memcmp(sb + 64, "_BHRfS_M", 8) != 0)
    return false;
  if (load_le64(sb + 48) != off)
    return false;
  return load_le16(sb + 196) != 0 || load_le32(sb) == ~crc32c_raw(0xFFFFFFFFu, sb + 32, BT_SB_SIZE - 32);
}

/* 0 mounted, -1 not btrfs here, -2 btrfs but unusable. */
static int bt_try_mount(struct bt_mount *m, uint64_t base)
{
  static const uint64_t mirrors[] = {BT_SB_OFFSET, 64ull << 20, 256ull << 30};
  uint8_t magic[8];
  m->base = base;
  if (bt_dev_read(m, BT_SB_OFFSET + 64, magic, 8) != 0 || memcmp(magic, "_BHRfS_M", 8) != 0)
    return -1;

  uint8_t *sb = kmalloc(BT_SB_SIZE), *alt = kmalloc(BT_SB_SIZE);
  int ret = -2;
  if (!sb || !alt)
    goto out;
  bool primary = bt_sb_valid(m, mirrors[0], sb), have = primary;
  for (int i = 1; i < 3 && !primary; i++) {
    if (bt_sb_valid(m, mirrors[i], alt) && (!have || load_le64(alt + 72) > load_le64(sb + 72))) {
      memcpy(sb, alt, BT_SB_SIZE);
      have = true;
    }
  }
  if (!have) {
    con_printf("btrfs: no valid superblock\n");
    goto out;
  }

  uint64_t incompat = load_le64(sb + 188);
  if (incompat & ~BT_INCOMPAT_SUPPORTED) {
    con_printf("btrfs: unsupported incompat features %x\n", (unsigned)(incompat & ~BT_INCOMPAT_SUPPORTED));
    goto out;
  }
  uint32_t ss = load_le32(sb + 144), ns = load_le32(sb + 148);
  if (ss < 512 || ss > BT_MAX_NODESIZE || (ss & (ss - 1)) || ns < BT_MIN_NODESIZE || ns > BT_MAX_NODESIZE ||
      (ns & (ns - 1)) || ns < ss || load_le32(sb + 152) != ns) {
    con_printf("btrfs: bad sector or node size\n");
    goto out;
  }
  m->sectorsize = ss;
  m->nodesize = ns;
  m->csum_crc32c = load_le16(sb + 196) == 0;
  m->devid = load_le64(sb + 201);
  memcpy(m->fsid, sb + ((incompat & BT_INCOMPAT_METADATA_UUID) ? 571 : 32), 16);
  memcpy(m->label, sb + 299, 255);
  m->label[255] = '\0';

  uint32_t asz = load_le32(sb + 160);
  if (asz > 2048)
    goto out;
  for (uint32_t off = 0; off < asz;) {
    if (asz - off < 17 + 48 || sb[811 + off + 8] != BT_K_CHUNK_ITEM)
      goto out;
    uint64_t logical = load_le64(sb + 811 + off + 9);
    off += 17;
    uint32_t nstripes = load_le16(sb + 811 + off + 44);
    if (bt_chunk_add(m, logical, sb + 811 + off, asz - off) != 0)
      goto out;
    off += 48 + 32 * nstripes;
  }

  struct bt_path p;
  struct bt_key key = {0, 0, 0}, k;
  const uint8_t *d;
  uint32_t sz;
  uint64_t chunk_root = load_le64(sb + 88);
  m->root_tree = load_le64(sb + 80);
  int r = bt_seek_ge(m, chunk_root, &key, &p);
  while (r == 0) {
    if (bt_cur(m, &p, &k, &d, &sz) != 0)
      goto out;
    if (k.type == BT_K_CHUNK_ITEM && bt_chunk_add(m, k.offset, d, sz) != 0)
      goto out;
    r = bt_next_item(m, &p);
  }
  if (r < 0 || m->nchunks == 0) {
    con_printf("btrfs: cannot read the chunk tree\n");
    goto out;
  }

  uint64_t default_id = BT_FS_TREE, rdir = load_le64(sb + 128) ? load_le64(sb + 128) : BT_ROOT_TREE_DIR;
  struct bt_dent ent;
  struct bt_vol root_dir = {0, m->root_tree, 0};
  if (bt_dir_find(m, &root_dir, rdir, "default", 7, &ent) == 0 && ent.loc.type == BT_K_ROOT_ITEM)
    default_id = ent.loc.objectid;
  struct btrfs_attr a;
  if ((bt_vol_open(m, default_id, &m->top) != 0 && bt_vol_open(m, BT_FS_TREE, &m->top) != 0) ||
      bt_inode_get(m, &m->top, m->top.dirid, &a) != 0 || (a.mode & BT_S_FMT) != BT_S_DIR) {
    con_printf("btrfs: cannot open the default subvolume\n");
    goto out;
  }
  ret = 0;
out:
  kfree(sb);
  kfree(alt);
  return ret;
}

static int bt_probe_dev(struct bt_mount *m)
{
  uint8_t sec[512];
  int r = bt_try_mount(m, 0);
  if (r != -1)
    return r;

  m->base = 0;
  if (m->read(1, sec) == 0 && memcmp(sec, "EFI PART", 8) == 0) {
    uint64_t ent_lba = load_le64(sec + 72);
    uint32_t cnt = load_le32(sec + 80), esz = load_le32(sec + 84);
    if (esz < 128)
      esz = 128;
    if (cnt > 128)
      cnt = 128;
    for (uint32_t i = 0; i < cnt; i++) {
      uint8_t ent[128];
      m->base = 0;
      if (bt_dev_read(m, ent_lba * 512 + (uint64_t)i * esz, ent, 128) != 0)
        break;
      uint64_t first = load_le64(ent + 32);
      bool empty = true;
      for (int b = 0; b < 16; b++)
        empty = empty && ent[b] == 0;
      if (empty || first == 0 || first > 0xFFFFFFFFull)
        continue;
      if ((r = bt_try_mount(m, first)) != -1)
        return r;
    }
  }
  m->base = 0;
  if (m->read(0, sec) == 0 && sec[510] == 0x55 && sec[511] == 0xAA) {
    for (int i = 0; i < 4; i++) {
      const uint8_t *e = sec + 446 + i * 16;
      uint32_t first = load_le32(e + 8);
      if (e[4] && first && (r = bt_try_mount(m, first)) != -1)
        return r;
    }
  }
  return -1;
}

static void *btrfs_probe(blk_read_fn read, blk_write_fn write)
{
  (void)write;
  if (!read)
    return NULL;
  struct bt_mount *m = kmalloc(sizeof *m);
  if (!m)
    return NULL;
  memset(m, 0, sizeof *m);
  m->read = read;
  if (bt_probe_dev(m) != 0) {
    bt_destroy(m);
    return NULL;
  }
  return m;
}

static void btrfs_close(void *inst) { bt_destroy(inst); }

static const char *btrfs_label(void *inst) { return ((struct bt_mount *)inst)->label; }

static const char *btrfs_type(void *inst)
{
  (void)inst;
  return "btrfs";
}

/* ---------------------------------------------------------------- operations */

static uint32_t bt_clamp32(uint64_t v) { return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)v; }

int btrfs_getinfo(void *inst, const char *path, struct btrfs_attr *out)
{
  struct bt_ref r;
  if (bt_resolve(inst, path, true, &r) != 0)
    return -1;
  *out = r.attr;
  return 0;
}

int btrfs_read64(void *inst, const char *path, uint64_t off, void *buf, uint32_t len)
{
  struct bt_mount *m = inst;
  struct bt_ref r;
  if (bt_resolve(m, path, true, &r) != 0 || (r.attr.mode & BT_S_FMT) != BT_S_REG)
    return -1;
  if (off >= r.attr.size || len == 0)
    return 0;
  if (len > 0x7FFFFFFFu)
    len = 0x7FFFFFFFu;
  if (r.attr.size - off < len)
    len = (uint32_t)(r.attr.size - off);
  return bt_file_read(m, &r.vol, r.ino, off, buf, len);
}

static int op_stat(void *inst, const char *path, int *is_dir, uint32_t *size)
{
  struct bt_ref r;
  if (bt_resolve(inst, path, true, &r) != 0)
    return -1;
  bool dir = (r.attr.mode & BT_S_FMT) == BT_S_DIR;
  if (is_dir)
    *is_dir = dir;
  if (size)
    *size = dir ? 0 : bt_clamp32(r.attr.size);
  return 0;
}

static int op_read_at(void *inst, const char *path, uint32_t off, void *buf, uint32_t len)
{
  return btrfs_read64(inst, path, off, buf, len);
}

static int op_getattr(void *inst, const char *path, uint32_t *mode, uint32_t *uid, uint32_t *gid)
{
  struct bt_ref r;
  if (bt_resolve(inst, path, true, &r) != 0)
    return -1;
  *mode = r.attr.mode & 01777u;
  *uid = r.attr.uid;
  *gid = r.attr.gid;
  return 0;
}

/* Kind and size of a directory entry as stat would report them; symlinks are followed when they resolve. */
static void bt_entry_info(struct bt_mount *m, const struct bt_ref *dir, const char *dirpath, const struct bt_dent *e,
                          bool *is_dir, uint32_t *size)
{
  struct bt_ref c = *dir;
  *is_dir = e->ftype == BT_FT_DIR;
  *size = 0;
  if (e->loc.type == BT_K_ROOT_ITEM) {
    if (bt_vol_open(m, e->loc.objectid, &c.vol) != 0)
      return;
    c.ino = c.vol.dirid;
  } else {
    c.ino = e->loc.objectid;
  }
  if (bt_inode_get(m, &c.vol, c.ino, &c.attr) != 0)
    return;
  if ((c.attr.mode & BT_S_FMT) == BT_S_LNK) {
    size_t dl = strlen(dirpath);
    char *full = kmalloc(dl + 1 + e->nlen + 1);
    if (full) {
      memcpy(full, dirpath, dl);
      full[dl] = '/';
      memcpy(full + dl + 1, e->name, e->nlen + 1);
      struct bt_ref t;
      if (bt_resolve(m, full, true, &t) == 0)
        c = t;
      kfree(full);
    }
  }
  *is_dir = (c.attr.mode & BT_S_FMT) == BT_S_DIR;
  *size = *is_dir ? 0 : bt_clamp32(c.attr.size);
}

static int op_enum_dir(void *inst, const char *path, int idx, char *name, int *is_dir, uint32_t *size)
{
  struct bt_mount *m = inst;
  struct bt_ref dir;
  struct bt_dent e;
  if (idx < 0 || bt_resolve(m, path, true, &dir) != 0 || (dir.attr.mode & BT_S_FMT) != BT_S_DIR)
    return 0;
  if (bt_dir_read(m, &dir, (uint32_t)idx, &e) != 1)
    return 0;
  bool d;
  uint32_t sz;
  bt_entry_info(m, &dir, path ? path : "", &e, &d, &sz);
  memcpy(name, e.name, e.nlen + 1);
  if (is_dir)
    *is_dir = d;
  if (size)
    *size = sz;
  return 1;
}

static int op_list(void *inst, const char *path)
{
  struct bt_mount *m = inst;
  struct bt_ref dir;
  struct bt_dent e;
  if (bt_resolve(m, path, true, &dir) != 0 || (dir.attr.mode & BT_S_FMT) != BT_S_DIR)
    return -1;
  for (uint32_t i = 0; bt_dir_read(m, &dir, i, &e) == 1; i++) {
    bool d;
    uint32_t sz;
    bt_entry_info(m, &dir, path ? path : "", &e, &d, &sz);
    con_write(d ? "  <DIR>  " : "         ");
    if (d)
      con_write("  ");
    else
      con_printf("%u  ", sz);
    con_printf("%s\n", e.name);
  }
  return 0;
}

const struct fs_ops *btrfs_ops(void)
{
  static const struct fs_ops ops = {
      .fsname = "btrfs",
      .probe = btrfs_probe,
      .close = btrfs_close,
      .label = btrfs_label,
      .type = btrfs_type,
      .list = op_list,
      .stat = op_stat,
      .read_at = op_read_at,
      .enum_dir = op_enum_dir,
      .create = NULL,
      .del = NULL,
      .mkdir = NULL,
      .rmdir = NULL,
      .getattr = op_getattr,
  };
  return &ops;
}
