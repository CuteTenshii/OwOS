/* ext.c - read-only ext2/3/4 driver (see ext.h).
 *
 * Little-endian throughout. The Superblock (byte 1024) gives the block size,
 * inode size, inodes-per-group and feature flags; block-group descriptors (just
 * after it) locate each group's inode table. An inode's data blocks are mapped
 * logical->physical by an extent tree (ext4) or classic indirect blocks
 * (ext2/3). Directories are files of dir_entry_2 records; we scan them linearly
 * (htree indexes are transparent to a linear read). Root is inode 2.
 *
 * Multi-instance: all per-mount state -- including the sector/block scratch
 * buffers -- lives in a heap-allocated `struct ext_mount`, so concurrent mounts
 * don't clobber each other. Each op takes the instance through a void *inst.
 */

#include <stddef.h>

#include "console.h"
#include "crc32c.h"
#include "endian.h"
#include "ext.h"
#include "heap.h"
#include "kstring.h"

/* ---------------------------------------------------------------- state */
#define EXT_MAGIC 0xEF53u
#define ROOT_INO 2u
#define MAX_LOG_BLOCK 6u
#define MAX_SYMLINK 4095u
#define MAX_LINKS 40
#define MAX_NAME 255u
#define CACHE_BUDGET (1u << 20)
#define CACHE_MIN_SLOTS 4u
#define CACHE_HASH 256u
#define EXT_VFY_RING 16u
#define JMAP_MAX (1u << 18)
#define MAX_EXTENT_DEPTH 16

#define COMPAT_HAS_JOURNAL 0x4u
#define COMPAT_SPARSE_SUPER2 0x200u
#define RO_SPARSE_SUPER 0x1u
#define RO_HUGE_FILE 0x8u
#define RO_GDT_CSUM 0x10u
#define RO_METADATA_CSUM 0x400u
#define INC_FILETYPE 0x2u
#define INC_RECOVER 0x4u
#define INC_JOURNAL_DEV 0x8u
#define INC_META_BG 0x10u
#define INC_EXTENTS 0x40u
#define INC_64BIT 0x80u
#define INC_MMP 0x100u
#define INC_FLEX_BG 0x200u
#define INC_EA_INODE 0x400u
#define INC_CSUM_SEED 0x2000u
#define INC_LARGEDIR 0x4000u
#define INC_INLINE_DATA 0x8000u
#define INC_ENCRYPT 0x10000u
#define INC_CASEFOLD 0x20000u
#define INC_KNOWN                                                                                                      \
  (INC_FILETYPE | INC_RECOVER | INC_JOURNAL_DEV | INC_META_BG | INC_EXTENTS | INC_64BIT | INC_MMP | INC_FLEX_BG |      \
   INC_EA_INODE | INC_CSUM_SEED | INC_LARGEDIR | INC_INLINE_DATA | INC_ENCRYPT | INC_CASEFOLD)

#define IF_ENCRYPT 0x800u
#define IF_HUGE 0x40000u
#define IF_EXTENTS 0x80000u
#define IF_INLINE 0x10000000u
#define IF_CASEFOLD 0x40000000u

#define S_FMT 0xF000u
#define S_SOCK 0xC000u
#define S_LNK 0xA000u
#define S_REG 0x8000u
#define S_BLK 0x6000u
#define S_DIR 0x4000u
#define S_CHR 0x2000u
#define S_FIFO 0x1000u

#define GD_INODE_UNINIT 0x1u
#define EXTENT_MAGIC 0xF30Au
#define XATTR_MAGIC 0xEA020000u

#define JMAGIC 0xC03B3998u
#define J_DESC 1u
#define J_COMMIT 2u
#define J_SB_V1 3u
#define J_SB_V2 4u
#define J_REVOKE 5u
#define JI_REVOKE 0x1u
#define JI_64BIT 0x2u
#define JI_ASYNC 0x4u
#define JI_CSUM_V2 0x8u
#define JI_CSUM_V3 0x10u
#define JI_FAST_COMMIT 0x20u
#define JC_CHECKSUM 0x1u
#define TAG_ESCAPE 1u
#define TAG_SAME_UUID 2u
#define TAG_LAST 8u
#define JESC (1ull << 63)
#define JPRESENT (1ull << 62)

#define SL_DIR 0
#define SL_CHILD 1
#define SL_TMP 2
#define SL_FILE 3
#define SL_R0 4
#define SL_R1 5
#define NSLOT 6

struct ext_ino {
  uint32_t ino, mode, uid, gid, nlink, flags;
  uint64_t size, blocks512;
  int64_t atime, mtime, ctime, crtime;
  uint32_t atime_ns, mtime_ns, ctime_ns, crtime_ns;
  uint32_t seed;
  uint32_t inl_off, inl_len; /* system.data value inside raw */
  uint8_t iblock[60];
  uint8_t *raw;
};

struct hent {
  uint64_t key, val; /* val 0 = empty slot */
};

struct htab {
  struct hent *e;
  uint32_t cap, n;
};

struct cslot {
  uint64_t blk;
  uint8_t *data;
  uint32_t stamp;
  int32_t next;
};

struct ext_dirpos {
  uint64_t blk;
  uint32_t off;
};

struct ext_dent {
  uint32_t ino;
  uint8_t ftype, nlen;
  char name[256];
};

struct ext_map {
  uint64_t phys; /* 0 = hole */
  uint32_t len;  /* blocks in this run */
  bool uninit;
};

struct gdesc {
  uint64_t itable;
  uint32_t flags;
};

/* Every per-mount mutable field. One heap-allocated instance per mounted ext
 * volume; ops cast their void *inst to this. The scratch buffers live here too
 * so two concurrent mounts never share them. */
struct ext_mount {
  blk_read_fn read;
  uint32_t base;
  uint32_t block_size, block_shift;
  uint32_t inodes_count, ipg, ngroups, first_ino, inode_size, desc_size, bpg, first_data_block, first_meta_bg;
  uint64_t blocks_count;
  uint32_t compat, incompat, ro_compat, state;
  uint32_t csum_seed, backup_bgs[2];
  uint8_t uuid[16];
  bool metacsum, gdt_csum;
  const char *type;
  char label[17];

  uint8_t *spare;
  struct cslot *cs;
  uint32_t cs_n, cs_max, clock;
  int32_t chash[CACHE_HASH];
  uint8_t *gd_ok;
  uint8_t *gd_bad;
  struct ext_ino *sl[NSLOT];
  struct htab jmap;

  uint64_t ev[EXT_VFY_RING];
  uint32_t ev_pos;
  uint32_t csum_errs, corrupt_errs;
  bool enc_logged;
  char *memo;
  uint32_t memo_ino;
  struct {
    bool ok;
    uint32_t dir;
    int idx;
    struct ext_dirpos pos;
  } ec;
};

static void note_csum(struct ext_mount *m, const char *what)
{
  if (m->csum_errs++ == 0)
    con_printf("ext: %s checksum mismatch, returning data unverified\n", what);
}

static void note_corrupt(struct ext_mount *m, const char *what)
{
  if (m->corrupt_errs++ == 0)
    con_printf("ext: corrupt %s, skipping\n", what);
}

/* ------------------------------------------------------------------ device and block cache */

/* ------------------------------------------------------------- device I/O */
static int dev_read(struct ext_mount *m, uint64_t byteoff, uint8_t *buf, uint32_t len)
{
  uint8_t s[512];
  while (len) {
    uint64_t lba = (uint64_t)m->base + (byteoff >> 9);
    uint32_t soff = (uint32_t)(byteoff & 511);
    if (lba > 0xFFFFFFFFull || m->read((uint32_t)lba, s) != 0)
      return -1;
    uint32_t n = 512 - soff;
    if (n > len)
      n = len;
    memcpy(buf, s + soff, n);
    buf += n;
    byteoff += n;
    len -= n;
  }
  return 0;
}

static uint32_t hash64(uint64_t k) { return (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> 32); }

static struct hent *htab_find(const struct htab *h, uint64_t key)
{
  if (!h->cap)
    return NULL;
  for (uint32_t i = hash64(key) & (h->cap - 1);; i = (i + 1) & (h->cap - 1)) {
    if (!h->e[i].val)
      return NULL;
    if (h->e[i].key == key)
      return &h->e[i];
  }
}

static bool htab_grow(struct htab *h)
{
  uint32_t ncap = h->cap ? h->cap * 2 : 64;
  struct hent *ne = kmalloc((size_t)ncap * sizeof *ne);
  if (!ne)
    return false;
  memset(ne, 0, (size_t)ncap * sizeof *ne);
  for (uint32_t i = 0; i < h->cap; i++) {
    if (!h->e[i].val)
      continue;
    uint32_t j = hash64(h->e[i].key) & (ncap - 1);
    while (ne[j].val)
      j = (j + 1) & (ncap - 1);
    ne[j] = h->e[i];
  }
  kfree(h->e);
  h->e = ne;
  h->cap = ncap;
  return true;
}

/* Slot for key (existing or new, new has val 0 and the caller must set a nonzero val). NULL on OOM or over the cap. */
static struct hent *htab_put(struct htab *h, uint64_t key)
{
  struct hent *f = htab_find(h, key);
  if (f)
    return f;
  if (h->n >= JMAP_MAX || ((h->n + 1) * 2 > h->cap && !htab_grow(h)))
    return NULL;
  uint32_t i = hash64(key) & (h->cap - 1);
  while (h->e[i].val)
    i = (i + 1) & (h->cap - 1);
  h->e[i].key = key;
  h->n++;
  return &h->e[i];
}

static void htab_free(struct htab *h)
{
  kfree(h->e);
  memset(h, 0, sizeof *h);
}

static int read_block_into(struct ext_mount *m, uint64_t blk, uint8_t *buf)
{
  if (blk >= m->blocks_count)
    return -1;
  if (m->jmap.n) {
    struct hent *h = htab_find(&m->jmap, blk);
    if (h) {
      if (dev_read(m, (h->val & ~(JESC | JPRESENT)) << m->block_shift, buf, m->block_size) != 0)
        return -1;
      if (h->val & JESC) {
        buf[0] = 0xC0;
        buf[1] = 0x3B;
        buf[2] = 0x39;
        buf[3] = 0x98;
      }
      return 0;
    }
  }
  return dev_read(m, blk << m->block_shift, buf, m->block_size);
}

static void cache_unlink(struct ext_mount *m, uint32_t i)
{
  struct cslot *s = &m->cs[i];
  if (s->blk == ~0ull)
    return;
  int32_t *p = &m->chash[hash64(s->blk) & (CACHE_HASH - 1)];
  while (*p >= 0 && *p != (int32_t)i)
    p = &m->cs[*p].next;
  if (*p >= 0)
    *p = s->next;
  s->blk = ~0ull;
  s->stamp = 0;
}

static void cache_flush(struct ext_mount *m)
{
  for (uint32_t i = 0; i < CACHE_HASH; i++)
    m->chash[i] = -1;
  for (uint32_t i = 0; i < m->cs_n; i++) {
    m->cs[i].blk = ~0ull;
    m->cs[i].stamp = 0;
  }
  memset(m->ev, 0xFF, sizeof m->ev);
}

static struct cslot *cache_victim(struct ext_mount *m)
{
  if (m->cs_n < m->cs_max) {
    uint8_t *d = kmalloc(m->block_size);
    if (d) {
      struct cslot *s = &m->cs[m->cs_n++];
      s->data = d;
      s->blk = ~0ull;
      s->stamp = 0;
      return s;
    }
  }
  if (!m->cs_n)
    return NULL;
  uint32_t best = 0;
  for (uint32_t i = 1; i < m->cs_n; i++)
    if (m->cs[i].stamp < m->cs[best].stamp)
      best = i;
  cache_unlink(m, best);
  return &m->cs[best];
}

/* The returned pointer is valid until the next get_block call. */
static const uint8_t *get_block(struct ext_mount *m, uint64_t blk)
{
  if (blk >= m->blocks_count)
    return NULL;
  uint32_t h = hash64(blk) & (CACHE_HASH - 1);
  for (int32_t i = m->chash[h]; i >= 0; i = m->cs[i].next)
    if (m->cs[i].blk == blk) {
      m->cs[i].stamp = ++m->clock;
      return m->cs[i].data;
    }
  struct cslot *s = cache_victim(m);
  uint8_t *dst = s ? s->data : m->spare;
  if (read_block_into(m, blk, dst) != 0)
    return NULL;
  if (s) {
    s->blk = blk;
    s->stamp = ++m->clock;
    s->next = m->chash[h];
    m->chash[h] = (int32_t)(s - m->cs);
  }
  return dst;
}

/* ------------------------------------------------------------------ checksums */

static uint16_t crc16(uint16_t crc, const uint8_t *p, uint32_t n)
{
  while (n--) {
    crc ^= *p++;
    for (int k = 0; k < 8; k++)
      crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
  }
  return crc;
}

static uint32_t crc32_be(uint32_t crc, const uint8_t *p, uint32_t n)
{
  static uint32_t t[256];
  if (!t[1])
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i << 24;
      for (int k = 0; k < 8; k++)
        c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : c << 1;
      t[i] = c;
    }
  while (n--)
    crc = (crc << 8) ^ t[(crc >> 24) ^ *p++];
  return crc;
}

static uint32_t csum_le32(uint32_t seed, uint32_t v)
{
  uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
  return crc32c_raw(seed, b, 4);
}

static bool gd_csum_ok(struct ext_mount *m, uint32_t group, const uint8_t *gd)
{
  static const uint8_t zero[2];
  uint32_t ds = m->desc_size;
  if (m->metacsum) {
    uint32_t c = csum_le32(m->csum_seed, group);
    c = crc32c_raw(c, gd, 0x1E);
    c = crc32c_raw(c, zero, 2);
    if (ds > 0x20)
      c = crc32c_raw(c, gd + 0x20, ds - 0x20);
    return (c & 0xFFFF) == load_le16(gd + 0x1E);
  }
  uint8_t g4[4] = {(uint8_t)group, (uint8_t)(group >> 8), (uint8_t)(group >> 16), (uint8_t)(group >> 24)};
  uint16_t c = crc16(0xFFFF, m->uuid, 16);
  c = crc16(c, g4, 4);
  c = crc16(c, gd, 0x1E);
  if ((m->incompat & INC_64BIT) && ds > 0x20)
    c = crc16(c, gd + 0x20, ds - 0x20);
  return c == load_le16(gd + 0x1E);
}

/* ------------------------------------------------------------------ group descriptors and inodes */

static bool is_power_of(uint32_t g, uint32_t b)
{
  while (g > 1 && g % b == 0)
    g /= b;
  return g == 1;
}

static bool bg_has_super(const struct ext_mount *m, uint64_t g)
{
  if (g == 0)
    return true;
  if (m->compat & COMPAT_SPARSE_SUPER2)
    return g == m->backup_bgs[0] || g == m->backup_bgs[1];
  if (!(m->ro_compat & RO_SPARSE_SUPER))
    return true;
  if (g > 0xFFFFFFFFull)
    return false;
  return g == 1 ||
         (g & 1 && (is_power_of((uint32_t)g, 3) || is_power_of((uint32_t)g, 5) || is_power_of((uint32_t)g, 7)));
}

static int group_desc(struct ext_mount *m, uint32_t group, struct gdesc *g)
{
  if (group >= m->ngroups)
    return -EXT_ERR_CORRUPT;
  uint32_t dpb = m->block_size / m->desc_size;
  uint32_t dg = group / dpb, di = group % dpb;
  uint64_t blk;
  if (!(m->incompat & INC_META_BG) || dg < m->first_meta_bg) {
    blk = (m->block_size == 1024 ? 1u : 0u) + (uint64_t)dg + 1;
  } else {
    uint64_t bg = (uint64_t)dg * dpb;
    blk = (uint64_t)m->first_data_block + bg * m->bpg + (bg_has_super(m, bg) ? 1 : 0);
    if (m->block_size == 1024 && dg == 0 && m->first_data_block == 0)
      blk++;
  }
  const uint8_t *d = get_block(m, blk);
  if (!d)
    return -EXT_ERR_IO;
  d += (size_t)di * m->desc_size;
  uint8_t bit = (uint8_t)(1u << (group & 7));
  bool checked = m->gd_ok && (m->gd_ok[group >> 3] & bit);
  bool good = !(checked && (m->gd_bad[group >> 3] & bit));
  if ((m->metacsum || m->gdt_csum) && !checked) {
    good = gd_csum_ok(m, group, d);
    if (!good)
      note_csum(m, "group descriptor");
    if (m->gd_ok) {
      m->gd_ok[group >> 3] |= bit;
      if (!good)
        m->gd_bad[group >> 3] |= bit;
    }
  }
  g->itable = load_le32(d + 8);
  if (m->desc_size >= 64)
    g->itable |= (uint64_t)load_le32(d + 40) << 32;
  g->flags = (m->metacsum || m->gdt_csum) && good ? load_le16(d + 18) : 0;
  return 0;
}

static void decode_time(const uint8_t *raw, uint32_t lo, uint32_t extra_off, uint32_t extra_isize, int64_t *sec,
                        uint32_t *ns)
{
  *sec = (int32_t)load_le32(raw + lo);
  *ns = 0;
  if (extra_off + 4 <= 128 + extra_isize) {
    uint32_t x = load_le32(raw + extra_off);
    *sec += (int64_t)(x & 3) << 32;
    *ns = x >> 2;
    if (*ns >= 1000000000u)
      *ns = 0;
  }
}

static void find_inline(const struct ext_mount *m, struct ext_ino *s, uint32_t extra)
{
  s->inl_off = s->inl_len = 0;
  uint32_t start = 128 + extra, end = m->inode_size;
  if (start + 8 > end || load_le32(s->raw + start) != XATTR_MAGIC)
    return;
  uint32_t ebeg = start + 4;
  for (uint32_t o = ebeg; o + 16 <= end && load_le32(s->raw + o) != 0;) {
    const uint8_t *e = s->raw + o;
    uint32_t nl = e[0], voff = load_le16(e + 2), vsz = load_le32(e + 8);
    if (o + 16 + nl > end)
      return;
    if (e[1] == 7 && nl == 4 && !memcmp(e + 16, "data", 4) && load_le32(e + 4) == 0) {
      if (vsz <= end && ebeg + voff <= end - vsz) {
        s->inl_off = ebeg + voff;
        s->inl_len = vsz;
      }
      return;
    }
    o += 16 + ((nl + 3) & ~3u);
  }
}

static bool inode_csum_ok(const struct ext_mount *m, const uint8_t *raw, uint32_t extra, uint32_t seed)
{
  static const uint8_t zero[4];
  uint32_t c = crc32c_raw(seed, raw, 0x7C);
  c = crc32c_raw(c, zero, 2);
  c = crc32c_raw(c, raw + 0x7E, 128 - 0x7E);
  uint32_t provided = load_le16(raw + 0x7C);
  if (m->inode_size > 128) {
    c = crc32c_raw(c, raw + 128, 2);
    if (extra >= 4) {
      c = crc32c_raw(c, zero, 2);
      c = crc32c_raw(c, raw + 0x84, m->inode_size - 0x84);
      provided |= (uint32_t)load_le16(raw + 0x82) << 16;
    } else {
      c = crc32c_raw(c, raw + 130, m->inode_size - 130);
    }
  }
  if (m->inode_size == 128 || extra < 4)
    c &= 0xFFFF;
  return c == provided;
}

static int ino_decode(struct ext_mount *m, uint32_t ino, struct ext_ino *s)
{
  const uint8_t *r = s->raw;
  uint32_t extra = 0;
  if (m->inode_size > 128) {
    extra = load_le16(r + 128);
    if (extra > m->inode_size - 128)
      extra = 0;
  }
  s->ino = ino;
  s->mode = load_le16(r);
  s->nlink = load_le16(r + 26);
  s->flags = load_le32(r + 32);
  if ((s->mode & S_FMT) == 0)
    return -EXT_ERR_CORRUPT;
  if (s->nlink == 0 && load_le32(r + 20) != 0)
    return -EXT_ERR_NOENT;
  if (m->metacsum) {
    s->seed = csum_le32(csum_le32(m->csum_seed, ino), load_le32(r + 100));
    if (!inode_csum_ok(m, r, extra, s->seed))
      note_csum(m, "inode");
  }
  s->uid = load_le16(r + 2) | ((uint32_t)load_le16(r + 120) << 16);
  s->gid = load_le16(r + 24) | ((uint32_t)load_le16(r + 122) << 16);
  s->size = load_le32(r + 4);
  if ((s->mode & S_FMT) == S_REG || (m->incompat & INC_LARGEDIR))
    s->size |= (uint64_t)load_le32(r + 108) << 32;
  uint64_t blocks = load_le32(r + 28);
  if (m->ro_compat & RO_HUGE_FILE)
    blocks |= (uint64_t)load_le16(r + 116) << 32;
  if ((m->ro_compat & RO_HUGE_FILE) && (s->flags & IF_HUGE))
    blocks <<= m->block_shift - 9;
  s->blocks512 = blocks;
  decode_time(r, 8, 0x8C, extra, &s->atime, &s->atime_ns);
  decode_time(r, 12, 0x84, extra, &s->ctime, &s->ctime_ns);
  decode_time(r, 16, 0x88, extra, &s->mtime, &s->mtime_ns);
  s->crtime = 0;
  s->crtime_ns = 0;
  if (0x94 <= 128 + extra)
    decode_time(r, 0x90, 0x94, extra, &s->crtime, &s->crtime_ns);
  memcpy(s->iblock, r + 40, 60);
  s->inl_off = s->inl_len = 0;
  if (s->flags & IF_INLINE)
    find_inline(m, s, extra);
  return 0;
}

static int ino_load(struct ext_mount *m, uint32_t ino, struct ext_ino *s)
{
  if (ino == 0 || ino > m->inodes_count)
    return -EXT_ERR_NOENT;
  struct gdesc g;
  int e = group_desc(m, (ino - 1) / m->ipg, &g);
  if (e)
    return e;
  if (g.flags & GD_INODE_UNINIT)
    return -EXT_ERR_NOENT;
  if (g.itable >= m->blocks_count)
    return -EXT_ERR_CORRUPT;
  uint64_t off = (uint64_t)((ino - 1) % m->ipg) * m->inode_size;
  const uint8_t *b = get_block(m, g.itable + (off >> m->block_shift));
  if (!b)
    return -EXT_ERR_IO;
  memcpy(s->raw, b + (off & (m->block_size - 1)), m->inode_size);
  return ino_decode(m, ino, s);
}

static void ino_copy(struct ext_ino *dst, const struct ext_ino *src, uint32_t inode_size)
{
  uint8_t *raw = dst->raw;
  *dst = *src;
  dst->raw = raw;
  memcpy(raw, src->raw, inode_size);
}

/* ------------------------------------------------------------------ block mapping */

static uint32_t type_of(const struct ext_ino *s) { return s->mode & S_FMT; }

static bool verified(const struct ext_mount *m, uint64_t blk)
{
  for (uint32_t i = 0; i < EXT_VFY_RING; i++)
    if (m->ev[i] == blk)
      return true;
  return false;
}

static int map_extents(struct ext_mount *m, const struct ext_ino *ino, uint64_t lblk, struct ext_map *r)
{
  r->phys = 0;
  r->uninit = false;
  r->len = 0xFFFFFFFFu;
  if (lblk > 0xFFFFFFFFull)
    return 0;
  uint32_t l = (uint32_t)lblk;
  const uint8_t *node = ino->iblock;
  uint32_t nbytes = 60;
  uint64_t limit = 0x100000000ull;
  int expect = -1;
  for (int lvl = 0; lvl < MAX_EXTENT_DEPTH; lvl++) {
    if (nbytes < 12 || load_le16(node) != EXTENT_MAGIC) {
      note_corrupt(m, "extent node");
      return -EXT_ERR_CORRUPT;
    }
    uint32_t entries = load_le16(node + 2), max = load_le16(node + 4), depth = load_le16(node + 6);
    if (max > (nbytes - 12) / 12 || entries > max || depth >= MAX_EXTENT_DEPTH ||
        (expect >= 0 && (int)depth != expect)) {
      note_corrupt(m, "extent node");
      return -EXT_ERR_CORRUPT;
    }
    const uint8_t *ents = node + 12;
    uint32_t lo = 0, hi = entries;
    while (lo < hi) {
      uint32_t mid = (lo + hi) / 2;
      if (load_le32(ents + mid * 12) > l)
        hi = mid;
      else
        lo = mid + 1;
    }
    uint64_t next = lo < entries ? load_le32(ents + lo * 12) : limit;
    if (next > limit)
      next = limit;
    if (depth == 0) {
      if (lo > 0) {
        const uint8_t *e = ents + (lo - 1) * 12;
        uint32_t start = load_le32(e), len = load_le16(e + 4);
        bool uninit = len > 32768;
        if (uninit)
          len -= 32768;
        if (len && l - start < len) {
          uint64_t phys = ((uint64_t)load_le16(e + 6) << 32 | load_le32(e + 8)) + (l - start);
          uint32_t rem = len - (l - start);
          if (phys >= m->blocks_count) {
            note_corrupt(m, "extent");
            return -EXT_ERR_CORRUPT;
          }
          if (phys + rem > m->blocks_count)
            rem = (uint32_t)(m->blocks_count - phys);
          r->phys = phys;
          r->len = rem;
          r->uninit = uninit;
          return 0;
        }
      }
      uint64_t hole = next > l ? next - l : 1;
      r->len = hole > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)hole;
      return 0;
    }
    if (lo == 0) {
      uint64_t hole = next > l ? next - l : 1;
      r->len = hole > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)hole;
      return 0;
    }
    const uint8_t *idx = ents + (lo - 1) * 12;
    limit = next;
    uint64_t leaf = (uint64_t)load_le16(idx + 8) << 32 | load_le32(idx + 4);
    node = get_block(m, leaf);
    if (!node)
      return -EXT_ERR_IO;
    nbytes = m->block_size;
    expect = (int)depth - 1;
    uint32_t tail = 12 + 12 * (uint32_t)load_le16(node + 4);
    if (m->metacsum && tail + 4 <= nbytes && !verified(m, leaf)) {
      if (crc32c_raw(ino->seed, node, tail) != load_le32(node + tail))
        note_csum(m, "extent block");
      m->ev[m->ev_pos++ % EXT_VFY_RING] = leaf;
    }
  }
  note_corrupt(m, "extent tree");
  return -EXT_ERR_CORRUPT;
}

static int scan_ptrs(struct ext_mount *m, const uint8_t *tbl, uint32_t n, uint32_t pos, struct ext_map *r)
{
  uint32_t p = load_le32(tbl + pos * 4), run = 1;
  r->uninit = false;
  if (!p) {
    while (pos + run < n && !load_le32(tbl + (pos + run) * 4))
      run++;
  } else {
    if (p >= m->blocks_count) {
      note_corrupt(m, "block pointer");
      return -EXT_ERR_CORRUPT;
    }
    while (pos + run < n && p + run < m->blocks_count && load_le32(tbl + (pos + run) * 4) == p + run)
      run++;
  }
  r->phys = p;
  r->len = run;
  return 0;
}

static int map_indirect(struct ext_mount *m, const struct ext_ino *ino, uint64_t lblk, struct ext_map *r)
{
  uint32_t ps = m->block_shift - 2, ppb = 1u << ps;
  r->phys = 0;
  r->uninit = false;
  if (lblk < 12)
    return scan_ptrs(m, ino->iblock, 12, (uint32_t)lblk, r);
  uint64_t idx = lblk - 12;
  int levels = 1;
  while (levels <= 3 && idx >= (1ull << (ps * levels))) {
    idx -= 1ull << (ps * levels);
    levels++;
  }
  if (levels > 3) {
    r->len = 0xFFFFFFFFu;
    return 0;
  }
  uint32_t ptr = load_le32(ino->iblock + (11 + levels) * 4);
  for (int lv = levels; lv >= 1; lv--) {
    if (!ptr) {
      uint64_t hole = (1ull << (ps * lv)) - idx;
      r->len = hole > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)hole;
      return 0;
    }
    const uint8_t *tbl = get_block(m, ptr);
    if (!tbl)
      return -EXT_ERR_IO;
    uint32_t pos = (uint32_t)(idx >> (ps * (lv - 1)));
    idx &= (1ull << (ps * (lv - 1))) - 1;
    if (lv == 1)
      return scan_ptrs(m, tbl, ppb, pos, r);
    ptr = load_le32(tbl + pos * 4);
  }
  return -EXT_ERR_CORRUPT;
}

static int map_block(struct ext_mount *m, const struct ext_ino *ino, uint64_t lblk, struct ext_map *r)
{
  return (ino->flags & IF_EXTENTS) ? map_extents(m, ino, lblk, r) : map_indirect(m, ino, lblk, r);
}

/* ------------------------------------------------------------------ file data */

static uint64_t eff_size(const struct ext_ino *f)
{
  if (f->flags & IF_INLINE) {
    uint64_t cap = 60 + (uint64_t)f->inl_len;
    return f->size < cap ? f->size : cap;
  }
  return f->size;
}

static void inline_read(const struct ext_ino *f, uint64_t off, uint8_t *dst, uint32_t len)
{
  while (len) {
    const uint8_t *src = off < 60 ? f->iblock + off : f->raw + f->inl_off + (off - 60);
    uint32_t n = off < 60 ? 60 - (uint32_t)off : len;
    if (n > len)
      n = len;
    memcpy(dst, src, n);
    dst += n;
    off += n;
    len -= n;
  }
}

/* Caller guarantees off + len <= eff_size(f). Returns bytes read or -err. */
static int file_read(struct ext_mount *m, const struct ext_ino *f, uint64_t off, uint8_t *dst, uint32_t len)
{
  if (f->flags & IF_INLINE) {
    inline_read(f, off, dst, len);
    return (int)len;
  }
  uint32_t bs = m->block_size, got = 0;
  while (got < len) {
    uint64_t pos = off + got;
    uint32_t boff = (uint32_t)(pos & (bs - 1));
    struct ext_map r;
    int err = map_block(m, f, pos >> m->block_shift, &r);
    if (err)
      return got ? (int)got : err;
    uint64_t span = (uint64_t)r.len * bs - boff;
    uint32_t chunk = (len - got) < span ? len - got : (uint32_t)span;
    if (!r.phys || r.uninit) {
      memset(dst + got, 0, chunk);
    } else {
      for (uint32_t done = 0; done < chunk;) {
        uint32_t o = (boff + done) & (bs - 1), n = bs - o;
        uint64_t b = r.phys + ((boff + done) >> m->block_shift);
        if (n > chunk - done)
          n = chunk - done;
        if (n == bs) {
          if (read_block_into(m, b, dst + got + done) != 0)
            return got ? (int)got : -EXT_ERR_IO;
        } else {
          const uint8_t *blk = get_block(m, b);
          if (!blk)
            return got ? (int)got : -EXT_ERR_IO;
          memcpy(dst + got + done, blk + o, n);
        }
        done += n;
      }
    }
    got += chunk;
  }
  return (int)got;
}

static int read_link(struct ext_mount *m, const struct ext_ino *s, char **out)
{
  if (s->flags & IF_ENCRYPT)
    return -EXT_ERR_ENCRYPTED;
  uint64_t size = eff_size(s);
  if (size == 0 || size > MAX_SYMLINK)
    return -EXT_ERR_CORRUPT;
  char *buf = kmalloc((size_t)size + 1);
  if (!buf)
    return -EXT_ERR_NOMEM;
  uint32_t ea = (load_le32(s->raw + 104) || load_le16(s->raw + 118)) ? m->block_size >> 9 : 0;
  bool fast = !(s->flags & (IF_EXTENTS | IF_INLINE)) && size <= 60 && s->blocks512 == ea;
  int n;
  if (fast) {
    memcpy(buf, s->iblock, (size_t)size);
    n = (int)size;
  } else {
    n = file_read(m, s, 0, (uint8_t *)buf, (uint32_t)size);
  }
  if (n < 0) {
    kfree(buf);
    return n;
  }
  buf[n] = '\0';
  *out = buf;
  return 0;
}

/* ------------------------------------------------------------------ directories */

static uint32_t rec_len_decode(uint32_t v, uint32_t bs)
{
  if (bs < 65536)
    return v;
  if (v == 65535 || v == 0)
    return bs;
  return (v & 65532) | ((v & 3) << 16);
}

static bool ent_ino_ok(const struct ext_mount *m, uint32_t ino)
{
  return ino == ROOT_INO || (ino >= m->first_ino && ino <= m->inodes_count);
}

static bool dir_csum_ok(const struct ext_mount *m, const struct ext_ino *d, const uint8_t *b)
{
  static const uint8_t zero[4];
  uint32_t bs = m->block_size;
  const uint8_t *t = b + bs - 12;
  if (!load_le32(t) && load_le16(t + 4) == 12 && !t[6] && t[7] == 0xDE)
    return crc32c_raw(d->seed, b, bs - 12) == load_le32(t + 8);
  uint32_t rlen = rec_len_decode(load_le16(b + 4), bs), off;
  if (rlen == bs) {
    off = 8;
  } else if (rlen == 12 && rec_len_decode(load_le16(b + 16), bs) == bs - 12) {
    off = 32;
  } else {
    return false;
  }
  uint32_t limit = load_le16(b + off), count = load_le16(b + off + 2);
  if (limit != (bs - off - 8) / 8 || count > limit)
    return false;
  const uint8_t *tail = b + off + limit * 8;
  uint32_t c = crc32c_raw(d->seed, b, off + count * 8);
  c = crc32c_raw(c, tail, 4);
  c = crc32c_raw(c, zero, 4);
  return c == load_le32(tail + 4);
}

/* Next live entry of region buf[0..len) from *off. 1 found, 0 region done, -1 corrupt. */
static int region_next(const struct ext_mount *m, const uint8_t *buf, uint32_t len, bool block, uint32_t *off,
                       struct ext_dent *e, bool enc)
{
  while (*off + 8 <= len) {
    const uint8_t *d = buf + *off;
    uint32_t rec = block ? rec_len_decode(load_le16(d + 4), m->block_size) : load_le16(d + 4);
    uint32_t nl = d[6], ino = load_le32(d);
    if (rec < 8 || (rec & 3) || rec > len - *off || 8 + nl > rec)
      return -1;
    *off += rec;
    if (!ino || !nl || !ent_ino_ok(m, ino))
      continue;
    bool ok = true;
    for (uint32_t i = 0; i < nl; i++) {
      char c = (char)d[8 + i];
      if (c == '\0' || c == '/') {
        if (!enc) {
          ok = false;
          break;
        }
        c = '_';
      }
      e->name[i] = c;
    }
    if (!ok)
      continue;
    e->name[nl] = '\0';
    e->nlen = (uint8_t)nl;
    e->ino = ino;
    e->ftype = (m->incompat & INC_FILETYPE) ? d[7] : 0;
    return 1;
  }
  return 0;
}

/* 1 entry, 0 end, <0 error. Includes "." and "..". */
static int dir_next(struct ext_mount *m, const struct ext_ino *d, struct ext_dirpos *p, struct ext_dent *e)
{
  bool enc = (d->flags & IF_ENCRYPT) != 0;
  if (d->flags & IF_INLINE) {
    for (;; p->blk++, p->off = 0) {
      const uint8_t *buf;
      uint32_t len;
      if (p->blk == 0) {
        buf = d->iblock + 4;
        len = 56;
      } else if (p->blk == 1 && d->inl_len) {
        buf = d->raw + d->inl_off;
        len = d->inl_len;
      } else {
        return 0;
      }
      int r = region_next(m, buf, len, false, &p->off, e, enc);
      if (r > 0)
        return 1;
      if (r < 0)
        note_corrupt(m, "inline directory");
    }
  }
  for (;;) {
    if (p->blk > 0xFFFFFFFFull || (p->blk << m->block_shift) >= d->size)
      return 0;
    struct ext_map r;
    int err = map_block(m, d, p->blk, &r);
    if (err)
      return err;
    if (!r.phys || r.uninit) {
      p->blk += r.len;
      p->off = 0;
      continue;
    }
    const uint8_t *b = get_block(m, r.phys);
    if (!b)
      return -EXT_ERR_IO;
    if (p->off == 0 && m->metacsum && !dir_csum_ok(m, d, b))
      note_csum(m, "directory block");
    int rc = region_next(m, b, m->block_size, true, &p->off, e, enc);
    if (rc > 0)
      return 1;
    if (rc < 0)
      note_corrupt(m, "directory block");
    p->blk++;
    p->off = 0;
  }
}

static bool is_dot(const struct ext_dent *e)
{
  return e->name[0] == '.' && (e->nlen == 1 || (e->nlen == 2 && e->name[1] == '.'));
}

static int dir_next_visible(struct ext_mount *m, const struct ext_ino *d, struct ext_dirpos *p, struct ext_dent *e)
{
  int r;
  while ((r = dir_next(m, d, p, e)) > 0)
    if (!is_dot(e))
      return 1;
  return r;
}

static bool name_eq(const struct ext_mount *m, const struct ext_ino *d, const struct ext_dent *e, const char *name,
                    uint32_t len)
{
  if (e->nlen != len)
    return false;
  if (!memcmp(e->name, name, len))
    return true;
  if (!(m->incompat & INC_CASEFOLD) || !(d->flags & IF_CASEFOLD))
    return false;
  for (uint32_t i = 0; i < len; i++) {
    unsigned char a = (unsigned char)e->name[i], b = (unsigned char)name[i];
    if (a >= 0x80 || b >= 0x80 || k_lower((char)a) != k_lower((char)b))
      return false;
  }
  return true;
}

static int dir_lookup(struct ext_mount *m, const struct ext_ino *d, const char *name, uint32_t len, uint32_t *ino)
{
  if (len > MAX_NAME)
    return -EXT_ERR_NAMETOOLONG;
  if ((d->flags & IF_INLINE) && len == 2 && name[0] == '.' && name[1] == '.') {
    *ino = load_le32(d->iblock);
    return *ino ? 0 : -EXT_ERR_CORRUPT;
  }
  struct ext_dirpos p = {0, 0};
  struct ext_dent e;
  int r;
  while ((r = dir_next(m, d, &p, &e)) > 0)
    if (name_eq(m, d, &e, name, len)) {
      *ino = e.ino;
      return 0;
    }
  return r < 0 ? r : -EXT_ERR_NOENT;
}

/* ------------------------------------------------------------------ path resolution */

static int resolve(struct ext_mount *m, uint32_t start, const char *path, bool follow_last, struct ext_ino *out)
{
  struct ext_ino *cur = m->sl[SL_R0], *nx = m->sl[SL_R1];
  char *heap = NULL;
  const char *p = path;
  int links = 0, err = ino_load(m, start, cur);
  while (!err) {
    while (*p == '/')
      p++;
    if (!*p)
      break;
    uint32_t len = 0;
    while (p[len] && p[len] != '/')
      len++;
    const char *rest = p + len;
    while (*rest == '/')
      rest++;
    if (len == 1 && p[0] == '.') {
      p = rest;
      continue;
    }
    if (type_of(cur) != S_DIR) {
      err = -EXT_ERR_NOTDIR;
      break;
    }
    if (len == 2 && p[0] == '.' && p[1] == '.' && cur->ino == ROOT_INO) {
      p = rest;
      continue;
    }
    uint32_t child;
    err = dir_lookup(m, cur, p, len, &child);
    if (!err)
      err = ino_load(m, child, nx);
    if (err)
      break;
    if (type_of(nx) == S_LNK && (*rest || follow_last)) {
      char *t;
      if (++links > MAX_LINKS) {
        err = -EXT_ERR_LOOP;
        break;
      }
      err = read_link(m, nx, &t);
      if (err)
        break;
      size_t tl = strlen(t), rl = strlen(rest);
      char *nb = kmalloc(tl + 1 + rl + 1);
      if (!nb) {
        kfree(t);
        err = -EXT_ERR_NOMEM;
        break;
      }
      memcpy(nb, t, tl);
      nb[tl] = '/';
      memcpy(nb + tl + 1, rest, rl + 1);
      kfree(t);
      kfree(heap);
      heap = nb;
      p = heap;
      if (heap[0] == '/')
        err = ino_load(m, ROOT_INO, cur);
      continue;
    }
    struct ext_ino *t = cur;
    cur = nx;
    nx = t;
    p = rest;
  }
  if (!err)
    ino_copy(out, cur, m->inode_size);
  kfree(heap);
  return err;
}

static int lookup_path(struct ext_mount *m, const char *path, struct ext_ino *out)
{
  if (!path)
    path = "";
  if (m->memo && !strcmp(m->memo, path))
    return ino_load(m, m->memo_ino, out);
  int e = resolve(m, ROOT_INO, path, true, out);
  uint32_t n = strlen(path);
  if (!e && n < 4096) {
    char *c = kmalloc(n + 1);
    if (c) {
      memcpy(c, path, n + 1);
      kfree(m->memo);
      m->memo = c;
      m->memo_ino = out->ino;
    }
  }
  return e;
}

/* ------------------------------------------------------------------ jbd2 replay */

struct jctx {
  struct ext_mount *m;
  struct ext_ino *jino;
  uint32_t first, last, start, seq0, end_id;
  bool v2, v3, b64, csum1;
  uint32_t csum23, tag_bytes, seed;
  uint8_t *hdr, *dat;
  uint32_t run_l, run_n;
  uint64_t run_p;
  struct htab rev, map;
  uint32_t ntrans, nblocks;
};

enum { P_SCAN, P_REVOKE, P_REPLAY };

static bool tid_gt(uint32_t x, uint32_t y) { return (int32_t)(x - y) > 0; }

static int jphys(struct jctx *jc, uint32_t jblk, uint64_t *phys)
{
  if (jc->run_n && jblk >= jc->run_l && jblk - jc->run_l < jc->run_n) {
    *phys = jc->run_p + (jblk - jc->run_l);
    return 0;
  }
  struct ext_map r;
  if (map_block(jc->m, jc->jino, jblk, &r) != 0 || !r.phys)
    return -1;
  jc->run_l = jblk;
  jc->run_n = r.len;
  jc->run_p = r.phys;
  *phys = r.phys;
  return 0;
}

static int jread(struct jctx *jc, uint32_t jblk, uint8_t *buf)
{
  uint64_t ph;
  struct ext_mount *m = jc->m;
  if (jphys(jc, jblk, &ph) != 0 || ph >= m->blocks_count)
    return -1;
  return dev_read(m, ph << m->block_shift, buf, m->block_size);
}

static uint32_t jnext(const struct jctx *jc, uint32_t b)
{
  b++;
  return b >= jc->last ? b - (jc->last - jc->first) : b;
}

static bool jtail_ok(const struct jctx *jc, const uint8_t *h)
{
  static const uint8_t zero[4];
  uint32_t bs = jc->m->block_size;
  uint32_t c = crc32c_raw(jc->seed, h, bs - 4);
  c = crc32c_raw(c, zero, 4);
  return c == load_be32(h + bs - 4);
}

static bool jtag_ok(const struct jctx *jc, const uint8_t *tag, const uint8_t *data, uint32_t id)
{
  uint8_t s[4] = {(uint8_t)(id >> 24), (uint8_t)(id >> 16), (uint8_t)(id >> 8), (uint8_t)id};
  uint32_t c = crc32c_raw(jc->seed, s, 4);
  c = crc32c_raw(c, data, jc->m->block_size);
  return jc->v3 ? c == load_be32(tag + 12) : (c & 0xFFFF) == load_be16(tag + 4);
}

static bool jcommit_ok(const struct jctx *jc, const uint8_t *h, uint32_t crc)
{
  static const uint8_t zero[4];
  if (jc->csum23) {
    uint32_t c = crc32c_raw(jc->seed, h, 16);
    c = crc32c_raw(c, zero, 4);
    c = crc32c_raw(c, h + 20, jc->m->block_size - 20);
    if (c != load_be32(h + 16))
      return false;
  }
  if (jc->csum1) {
    uint32_t found = load_be32(h + 16);
    bool match = found == crc && h[12] == 1 && h[13] == 4;
    if (!match && !(h[12] == 0 && h[13] == 0 && found == 0))
      return false;
  }
  return true;
}

static int jpass(struct jctx *jc, int pass)
{
  struct ext_mount *m = jc->m;
  uint32_t bs = m->block_size, id = jc->seq0, blk = jc->start, crc = ~0u, steps = 0;
  uint32_t limit = (jc->last - jc->first) * 2 + 8;
  for (;;) {
    if (pass != P_SCAN && !tid_gt(jc->end_id, id))
      break;
    if (++steps > limit || jread(jc, blk, jc->hdr) != 0)
      break;
    const uint8_t *h = jc->hdr;
    if (load_be32(h) != JMAGIC || load_be32(h + 8) != id)
      break;
    uint32_t type = load_be32(h + 4), next = jnext(jc, blk);
    if (type == J_DESC) {
      if (pass == P_SCAN && jc->csum23 && !jtail_ok(jc, h))
        break;
      if (pass == P_SCAN && jc->csum1)
        crc = crc32_be(crc, h, bs);
      uint32_t size = bs - (jc->csum23 ? 4 : 0), off = 12;
      bool torn = false;
      while (off + jc->tag_bytes <= size) {
        const uint8_t *t = h + off;
        uint32_t flags = jc->v3 ? load_be32(t + 4) : load_be16(t + 6);
        uint64_t fsblk = load_be32(t);
        if (jc->b64)
          fsblk |= (uint64_t)load_be32(t + 8) << 32;
        off += jc->tag_bytes + ((flags & TAG_SAME_UUID) ? 0 : 16);
        uint32_t dblk = next;
        next = jnext(jc, next);
        if (pass == P_SCAN && (jc->csum23 || jc->csum1)) {
          if (jread(jc, dblk, jc->dat) != 0 || (jc->csum23 && !jtag_ok(jc, t, jc->dat, id))) {
            torn = true;
            break;
          }
          if (jc->csum1)
            crc = crc32_be(crc, jc->dat, bs);
        } else if (pass == P_REPLAY && fsblk < m->blocks_count) {
          struct hent *rv = htab_find(&jc->rev, fsblk);
          uint64_t ph;
          if (!(rv && !tid_gt(id, (uint32_t)rv->val)) && jphys(jc, dblk, &ph) == 0 && ph < m->blocks_count) {
            struct hent *s = htab_put(&jc->map, fsblk);
            if (!s)
              return -EXT_ERR_NOMEM;
            if (!s->val)
              jc->nblocks++;
            s->val = JPRESENT | ph | ((flags & TAG_ESCAPE) ? JESC : 0);
          }
        }
        if (flags & TAG_LAST)
          break;
      }
      if (torn)
        break;
      blk = next;
    } else if (type == J_COMMIT) {
      if (pass == P_SCAN) {
        if (!jcommit_ok(jc, h, crc))
          break;
        crc = ~0u;
        jc->ntrans++;
      }
      id++;
      blk = next;
    } else if (type == J_REVOKE) {
      if (pass == P_SCAN && jc->csum23 && !jtail_ok(jc, h))
        break;
      if (pass == P_REVOKE) {
        uint32_t rc = load_be32(h + 12), rec = jc->b64 ? 8 : 4;
        if (rc < 16 || rc > bs - (jc->csum23 ? 4 : 0))
          break;
        for (uint32_t o = 16; o + rec <= rc; o += rec) {
          uint64_t b = jc->b64 ? load_be64(h + o) : load_be32(h + o);
          struct hent *s = htab_put(&jc->rev, b);
          if (!s)
            return -EXT_ERR_NOMEM;
          if (!s->val || tid_gt(id, (uint32_t)s->val))
            s->val = JPRESENT | id;
        }
      }
      blk = next;
    } else {
      break;
    }
  }
  if (pass == P_SCAN)
    jc->end_id = id;
  return 0;
}

static void journal_recover(struct ext_mount *m, const uint8_t *sb)
{
  uint32_t jnum = load_le32(sb + 0xE0);
  if (!(m->compat & COMPAT_HAS_JOURNAL) || !jnum) {
    con_printf("ext: needs recovery but has no internal journal, mounted without replay\n");
    return;
  }
  struct jctx jc;
  memset(&jc, 0, sizeof jc);
  jc.m = m;
  jc.jino = m->sl[SL_FILE];
  const char *why = NULL;
  uint32_t bs = m->block_size;
  jc.hdr = kmalloc(bs);
  jc.dat = kmalloc(bs);
  if (!jc.hdr || !jc.dat) {
    why = "out of memory";
    goto out;
  }
  if (ino_load(m, jnum, jc.jino) != 0 || type_of(jc.jino) != S_REG || (jc.jino->flags & (IF_INLINE | IF_ENCRYPT))) {
    why = "journal inode unreadable";
    goto out;
  }
  uint64_t jblocks = jc.jino->size >> m->block_shift;
  if (jblocks > 0xFFFFFFFFull)
    jblocks = 0xFFFFFFFFull;
  if (jread(&jc, 0, jc.hdr) != 0) {
    why = "journal superblock unreadable";
    goto out;
  }
  const uint8_t *js = jc.hdr;
  uint32_t jtype = load_be32(js + 4);
  if (load_be32(js) != JMAGIC || (jtype != J_SB_V1 && jtype != J_SB_V2) || load_be32(js + 12) != bs) {
    why = "bad journal superblock";
    goto out;
  }
  uint32_t jincompat = 0, jcompat = 0;
  if (jtype == J_SB_V2) {
    jcompat = load_be32(js + 36);
    jincompat = load_be32(js + 40);
  }
  if ((jincompat & ~(JI_REVOKE | JI_64BIT | JI_ASYNC | JI_CSUM_V2 | JI_CSUM_V3 | JI_FAST_COMMIT)) ||
      ((jincompat & JI_CSUM_V2) && (jincompat & JI_CSUM_V3))) {
    why = "unsupported journal features";
    goto out;
  }
  jc.v2 = jincompat & JI_CSUM_V2;
  jc.v3 = jincompat & JI_CSUM_V3;
  jc.csum23 = (jc.v2 || jc.v3) ? 4 : 0;
  jc.csum1 = (jcompat & JC_CHECKSUM) && !jc.csum23;
  jc.b64 = jincompat & JI_64BIT;
  if (jc.csum23) {
    static const uint8_t zero[4];
    uint32_t c = crc32c_raw(~0u, js, 0xFC);
    c = crc32c_raw(c, zero, 4);
    c = crc32c_raw(c, js + 0x100, 1024 - 0x100);
    if (c != load_be32(js + 0xFC)) {
      why = "journal superblock checksum mismatch";
      goto out;
    }
    jc.seed = crc32c_raw(~0u, js + 48, 16);
  }
  jc.tag_bytes = jc.v3 ? 16 : (12 + (jc.v2 ? 2 : 0) - (jc.b64 ? 0 : 4));
  uint32_t maxlen = load_be32(js + 16);
  if (maxlen > jblocks)
    maxlen = (uint32_t)jblocks;
  jc.first = load_be32(js + 20);
  jc.last = maxlen;
  if (jincompat & JI_FAST_COMMIT) {
    uint32_t nfc = load_be32(js + 84);
    nfc = nfc ? nfc : 256;
    jc.last = nfc < maxlen ? maxlen - nfc : 0;
  }
  jc.seq0 = load_be32(js + 24);
  jc.start = load_be32(js + 28);
  if (jc.start == 0) {
    con_printf("ext: journal is empty, nothing to replay\n");
    goto done;
  }
  if (jc.first == 0 || jc.first >= jc.last || jc.start < jc.first || jc.start >= jc.last) {
    why = "journal geometry invalid";
    goto out;
  }
  if (jpass(&jc, P_SCAN) != 0 || jpass(&jc, P_REVOKE) != 0 || jpass(&jc, P_REPLAY) != 0) {
    why = "out of memory";
    goto out;
  }
  m->jmap = jc.map;
  memset(&jc.map, 0, sizeof jc.map);
  cache_flush(m);
  con_printf("ext: replayed journal in RAM (%u transactions, %u blocks), volume is read-only\n", jc.ntrans, jc.nblocks);
  goto done;
out:
  con_printf("ext: needs recovery but %s, mounted without replay, data may be stale\n",
             why ? why : "journal unsupported");
done:
  htab_free(&jc.map);
  htab_free(&jc.rev);
  kfree(jc.hdr);
  kfree(jc.dat);
}

/* ------------------------------------------------------------------ mount */

static void mount_release(struct ext_mount *m)
{
  for (uint32_t i = 0; i < m->cs_n; i++)
    kfree(m->cs[i].data);
  for (int i = 0; i < NSLOT; i++)
    kfree(m->sl[i]);
  htab_free(&m->jmap);
  kfree(m->cs);
  kfree(m->spare);
  kfree(m->gd_ok);
  kfree(m->gd_bad);
  kfree(m->memo);
  memset(m, 0, sizeof *m);
}
/* ------------------------------------------------------------- mounting */

static int parse_superblock(struct ext_mount *m, const uint8_t *sb)
{
  if (load_le16(sb + 56) != EXT_MAGIC)
    return -1;
  uint32_t lbs = load_le32(sb + 24);
  if (lbs > MAX_LOG_BLOCK)
    return -1;
  uint32_t bs = 1024u << lbs, rev = load_le32(sb + 76);
  m->compat = rev ? load_le32(sb + 92) : 0;
  m->incompat = rev ? load_le32(sb + 96) : 0;
  m->ro_compat = rev ? load_le32(sb + 100) : 0;
  if ((m->incompat & ~INC_KNOWN) || (m->incompat & INC_JOURNAL_DEV))
    return -1;
  m->block_size = bs;
  m->block_shift = 10 + lbs;
  m->ipg = load_le32(sb + 40);
  m->inodes_count = load_le32(sb + 0);
  m->bpg = load_le32(sb + 32);
  m->first_data_block = load_le32(sb + 20);
  m->blocks_count = load_le32(sb + 4);
  if (m->incompat & INC_64BIT)
    m->blocks_count |= (uint64_t)load_le32(sb + 0x150) << 32;
  if (m->ipg == 0 || m->ipg > bs * 8 || m->inodes_count == 0 || m->blocks_count == 0 ||
      m->first_data_block >= m->blocks_count)
    return -1;
  if (m->blocks_count >> (64 - m->block_shift))
    return -1;
  m->ngroups = (m->inodes_count - 1) / m->ipg + 1;
  m->inode_size = rev ? load_le16(sb + 88) : 128;
  if (m->inode_size < 128 || m->inode_size > bs || (m->inode_size & (m->inode_size - 1)))
    return -1;
  m->first_ino = rev ? load_le32(sb + 84) : 11;
  if (m->first_ino < 11)
    m->first_ino = 11;
  m->desc_size = 32;
  if (m->incompat & INC_64BIT) {
    m->desc_size = load_le16(sb + 0xFE);
    if (m->desc_size < 64 || m->desc_size > 1024 || (m->desc_size & (m->desc_size - 1)) || m->desc_size > bs)
      return -1;
  }
  m->first_meta_bg = load_le32(sb + 0x104);
  if ((m->incompat & INC_META_BG) && m->bpg == 0)
    return -1;
  m->backup_bgs[0] = load_le32(sb + 0x24C);
  m->backup_bgs[1] = load_le32(sb + 0x250);
  m->state = load_le16(sb + 58);
  memcpy(m->uuid, sb + 104, 16);
  m->metacsum = (m->ro_compat & RO_METADATA_CSUM) && sb[0x175] == 1;
  m->gdt_csum = (m->ro_compat & RO_GDT_CSUM) && !m->metacsum;
  m->csum_seed = (m->incompat & INC_CSUM_SEED) ? load_le32(sb + 0x270) : crc32c_raw(~0u, m->uuid, 16);
  m->type = ((m->incompat & (INC_EXTENTS | INC_64BIT | INC_FLEX_BG | INC_INLINE_DATA)) || m->metacsum) ? "ext4"
            : (m->compat & COMPAT_HAS_JOURNAL)                                                         ? "ext3"
                                                                                                       : "ext2";
  int n = 0; /* s_volume_name @120 */
  for (; n < 16 && sb[120 + n]; n++)
    m->label[n] = (char)sb[120 + n];
  m->label[n] = '\0';
  return 0;
}

static bool mount_alloc(struct ext_mount *m)
{
  m->cs_max = CACHE_BUDGET / m->block_size;
  if (m->cs_max < CACHE_MIN_SLOTS)
    m->cs_max = CACHE_MIN_SLOTS;
  m->spare = kmalloc(m->block_size);
  m->cs = kmalloc(m->cs_max * sizeof *m->cs);
  if (!m->spare || !m->cs)
    return false;
  cache_flush(m);
  for (int i = 0; i < NSLOT; i++) {
    m->sl[i] = kmalloc(sizeof(struct ext_ino) + m->inode_size);
    if (!m->sl[i])
      return false;
    m->sl[i]->raw = (uint8_t *)(m->sl[i] + 1);
  }
  if (m->ngroups <= (1u << 22) && (m->metacsum || m->gdt_csum)) {
    uint32_t n = (m->ngroups + 7) / 8;
    m->gd_ok = kmalloc(n);
    m->gd_bad = kmalloc(n);
    if (m->gd_ok && m->gd_bad) {
      memset(m->gd_ok, 0, n);
      memset(m->gd_bad, 0, n);
    } else {
      /* without both bitmaps every lookup re-verifies, which stays correct */
      kfree(m->gd_ok);
      kfree(m->gd_bad);
      m->gd_ok = m->gd_bad = NULL;
    }
  }
  return true;
}

/* Try to mount the ext volume whose Superblock is at byte 1024 of `base_lba`.
 * 0 on success (m populated), -1 on failure. */
static int try_mount(struct ext_mount *m, blk_read_fn read, uint32_t base_lba)
{
  uint8_t sb[1024];
  memset(m, 0, sizeof *m);
  m->read = read;
  m->base = base_lba;
  if (dev_read(m, 1024, sb, 1024) != 0 || parse_superblock(m, sb) != 0)
    return -1;
  if (!mount_alloc(m)) {
    mount_release(m);
    return -1;
  }
  if (m->metacsum && crc32c_raw(~0u, sb, 0x3FC) != load_le32(sb + 0x3FC))
    note_csum(m, "superblock");
  if (m->state & 2)
    con_printf("ext: filesystem has recorded errors\n");
  if (m->incompat & INC_RECOVER)
    journal_recover(m, sb);
  else if (!(m->state & 1))
    con_printf("ext: not cleanly unmounted\n");
  if (ino_load(m, ROOT_INO, m->sl[SL_TMP]) != 0 || type_of(m->sl[SL_TMP]) != S_DIR) {
    mount_release(m);
    return -1;
  }
  return 0;
}

/* Whole disk, then GPT, then MBR partitions (ext often lives in a partition).
 */
static int probe_dev(struct ext_mount *m, blk_read_fn read)
{
  if (try_mount(m, read, 0) == 0)
    return 0;
  uint8_t sec[512];

  if (read(1, sec) == 0 && sec[0] == 'E' && sec[1] == 'F' && sec[2] == 'I' && sec[3] == ' ' && sec[4] == 'P') {
    uint64_t ent_lba = load_le64(sec + 72);
    uint32_t ent_cnt = load_le32(sec + 80), ent_sz = load_le32(sec + 84);
    if (ent_sz < 128)
      ent_sz = 128;
    if (ent_cnt > 128)
      ent_cnt = 128;
    for (uint32_t i = 0; i < ent_cnt && ent_lba < 0xFFFFFFFFull; i++) {
      uint64_t off = ent_lba * 512 + (uint64_t)i * ent_sz;
      uint8_t ent[128], s[512];
      uint32_t soff = (uint32_t)(off & 511);
      uint64_t lba = off >> 9;
      if (lba > 0xFFFFFFFFull || soff + 128 > 512 || read((uint32_t)lba, s) != 0)
        break;
      memcpy(ent, s + soff, 128);
      bool empty = true;
      for (int k = 0; k < 16; k++)
        if (ent[k]) {
          empty = false;
          break;
        }
      uint64_t first = load_le64(ent + 32);
      if (!empty && first && first <= 0xFFFFFFFFull && try_mount(m, read, (uint32_t)first) == 0)
        return 0;
    }
  }

  if (read(0, sec) == 0 && sec[510] == 0x55 && sec[511] == 0xAA) { /* MBR */
    for (int i = 0; i < 4; i++) {
      const uint8_t *p = sec + 446 + i * 16;
      uint32_t first = load_le32(p + 8);
      if (p[4] && first && try_mount(m, read, first) == 0)
        return 0;
    }
  }
  return -1;
}

/* ------------------------------------------------------------------ operations */

static void to_attr(const struct ext_ino *s, struct ext_attr *a)
{
  a->ino = s->ino;
  a->mode = s->mode;
  a->uid = s->uid;
  a->gid = s->gid;
  a->nlink = s->nlink;
  a->flags = s->flags;
  a->size = s->size;
  a->blocks512 = s->blocks512;
  a->atime = s->atime;
  a->mtime = s->mtime;
  a->ctime = s->ctime;
  a->crtime = s->crtime;
  a->atime_ns = s->atime_ns;
  a->mtime_ns = s->mtime_ns;
  a->ctime_ns = s->ctime_ns;
  a->crtime_ns = s->crtime_ns;
}

int ext_getinfo(void *inst, const char *path, struct ext_attr *out)
{
  struct ext_mount *m = inst;
  struct ext_ino *s = m->sl[SL_FILE];
  int e = lookup_path(m, path, s);
  if (e)
    return e;
  to_attr(s, out);
  return 0;
}

int ext_read64(void *inst, const char *path, uint64_t off, void *buf, uint32_t len)
{
  struct ext_mount *m = inst;
  struct ext_ino *f = m->sl[SL_FILE];
  int e = lookup_path(m, path, f);
  if (e)
    return e;
  if (type_of(f) == S_DIR)
    return -EXT_ERR_ISDIR;
  if (type_of(f) != S_REG)
    return -EXT_ERR_NOTSUP;
  if (f->flags & IF_ENCRYPT) {
    if (!m->enc_logged) {
      m->enc_logged = true;
      con_printf("ext: encrypted file, contents cannot be read\n");
    }
    return -EXT_ERR_ENCRYPTED;
  }
  uint64_t size = eff_size(f);
  if (off >= size)
    return 0;
  if (len > 0x7FFFFFFFu)
    len = 0x7FFFFFFFu;
  if (len > size - off)
    len = (uint32_t)(size - off);
  return file_read(m, f, off, buf, len);
}

/* Type and size of a directory entry; symlinks report their target when it resolves. */
static void ent_attrs(struct ext_mount *m, const struct ext_ino *dir, const struct ext_dent *e, uint32_t *type,
                      uint64_t *size)
{
  struct ext_ino *c = m->sl[SL_CHILD];
  *type = e->ftype == 2 ? S_DIR : S_REG;
  *size = 0;
  if (ino_load(m, e->ino, c) != 0)
    return;
  if (type_of(c) == S_LNK) {
    char *t;
    if (read_link(m, c, &t) == 0) {
      if (resolve(m, dir->ino, t, true, m->sl[SL_TMP]) == 0)
        c = m->sl[SL_TMP];
      kfree(t);
    }
  }
  *type = type_of(c);
  *size = c->size;
}

static const char *type_tag(uint32_t type)
{
  switch (type) {
  case S_DIR:
    return "  <DIR>  ";
  case S_CHR:
    return "  <CHR>  ";
  case S_BLK:
    return "  <BLK>  ";
  case S_FIFO:
    return " <FIFO>  ";
  case S_SOCK:
    return " <SOCK>  ";
  default:
    return "         ";
  }
}

static uint32_t clamp32(uint64_t v) { return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)v; }
/* --------------------------------------------------------------- vtable */

static void *ext_probe(blk_read_fn read, blk_write_fn write)
{
  (void)write; /* read-only */
  if (!read)
    return NULL;
  struct ext_mount *m = kmalloc(sizeof *m);
  if (!m)
    return NULL;
  if (probe_dev(m, read) != 0) {
    kfree(m);
    return NULL;
  }
  return m;
}

static void ext_close(void *inst)
{
  mount_release(inst);
  kfree(inst);
}

static const char *op_label(void *inst) { return ((struct ext_mount *)inst)->label; }
static const char *op_type(void *inst) { return ((struct ext_mount *)inst)->type; }

static int op_list(void *inst, const char *path)
{
  struct ext_mount *m = inst;
  struct ext_ino *d = m->sl[SL_DIR];
  if (lookup_path(m, path, d) != 0 || type_of(d) != S_DIR)
    return -1;
  struct ext_dirpos p = {0, 0};
  struct ext_dent e;
  while (dir_next_visible(m, d, &p, &e) > 0) {
    uint32_t type;
    uint64_t size;
    ent_attrs(m, d, &e, &type, &size);
    con_write(type_tag(type));
    if (type == S_DIR)
      con_write("  ");
    else
      con_printf("%u  ", clamp32(type == S_REG ? size : 0));
    con_printf("%s\n", e.name);
  }
  return 0;
}

static int op_stat(void *inst, const char *path, int *is_dir, uint32_t *size)
{
  struct ext_attr a;
  if (ext_getinfo(inst, path, &a) != 0)
    return -1;
  if (is_dir)
    *is_dir = (a.mode & S_FMT) == S_DIR;
  if (size)
    *size = clamp32(a.size);
  return 0;
}

static int op_read_at(void *inst, const char *path, uint32_t off, void *buf, uint32_t len)
{
  int n = ext_read64(inst, path, off, buf, len);
  return n < 0 ? -1 : n;
}

static int op_enum_dir(void *inst, const char *path, int idx, char *name, int *is_dir, uint32_t *size)
{
  struct ext_mount *m = inst;
  struct ext_ino *d = m->sl[SL_DIR];
  if (idx < 0 || lookup_path(m, path, d) != 0 || type_of(d) != S_DIR)
    return 0;
  struct ext_dirpos p = {0, 0};
  struct ext_dent e;
  int seen = 0;
  if (m->ec.ok && m->ec.dir == d->ino && m->ec.idx == idx) {
    p = m->ec.pos;
    seen = idx;
  }
  m->ec.ok = false;
  while (dir_next_visible(m, d, &p, &e) > 0) {
    if (seen++ != idx)
      continue;
    uint32_t type;
    uint64_t sz;
    ent_attrs(m, d, &e, &type, &sz);
    fs_enum_name(name, e.name, e.nlen);
    if (is_dir)
      *is_dir = type == S_DIR;
    if (size)
      *size = type == S_REG ? clamp32(sz) : 0;
    m->ec.ok = true;
    m->ec.dir = d->ino;
    m->ec.idx = idx + 1;
    m->ec.pos = p;
    return 1;
  }
  return 0;
}

static int op_getattr(void *inst, const char *path, uint32_t *mode, uint32_t *uid, uint32_t *gid)
{
  struct ext_attr a;
  if (ext_getinfo(inst, path, &a) != 0)
    return -1;
  *mode = a.mode & 01777u;
  *uid = a.uid;
  *gid = a.gid;
  return 0;
}

const struct fs_ops *ext_ops(void)
{
  static const struct fs_ops ops = {
      .fsname = "ext",
      .probe = ext_probe,
      .close = ext_close,
      .label = op_label,
      .type = op_type,
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
