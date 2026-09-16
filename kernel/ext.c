/* ext.c — read-only ext2/3/4 driver (see ext.h).
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
#include "endian.h"
#include "ext.h"
#include "heap.h"
#include "kstring.h"

/* ---------------------------------------------------------------- state */
#define EXT_MAGIC 0xEF53u
#define EXT_ROOT_INO 2u
#define MAX_BLOCK 4096
#define INCOMPAT_FILETYPE 0x0002u
#define INCOMPAT_EXTENTS 0x0040u
#define INCOMPAT_64BIT 0x0080u
#define INCOMPAT_ENCRYPT 0x10000u
#define INODE_EXTENTS_FL 0x80000u
#define EXT_EXTENT_MAGIC 0xF30Au

/* Every per-mount mutable field. One heap-allocated instance per mounted ext
 * volume; ops cast their void *inst to this. The scratch buffers live here too
 * so two concurrent mounts never share them. */
struct ext_mount {
  blk_read_fn read;
  uint32_t base; /* LBA of byte 0 of the volume */
  uint32_t block_size;
  uint32_t inodes_per_group;
  uint32_t inode_size;
  uint32_t desc_size; /* group-descriptor size (32 or 64) */
  uint32_t gd_block;  /* first block of the group-descriptor table */
  const char *type;
  char label[20];

  /* Scratch: one inode under inspection, one for the directory being walked,
   * and two block buffers (one for block-mapping internals, one for data). */
  uint8_t cur_inode[256];
  uint8_t dir_inode[256];
  uint8_t blk_scratch[MAX_BLOCK];
  uint8_t data_blk[MAX_BLOCK];
};

/* ------------------------------------------------------------- device I/O */
static int dev_read(struct ext_mount *m, uint64_t byteoff, uint8_t *buf, uint32_t len)
{
  uint8_t s[512];
  while (len) {
    uint32_t lba = m->base + (uint32_t)(byteoff / 512);
    uint32_t soff = (uint32_t)(byteoff % 512);
    if (m->read(lba, s) != 0)
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

static int read_block(struct ext_mount *m, uint32_t block, uint8_t *buf)
{
  return dev_read(m, (uint64_t)block * m->block_size, buf, m->block_size);
}

/* Straddle-safe read of `len` bytes from an absolute device byte offset using a
 * raw callback (used while probing, before an ext_mount's read/base are set).
 */
static int raw_read(blk_read_fn read, uint64_t byteoff, uint8_t *buf, uint32_t len)
{
  uint8_t s[512];
  while (len) {
    uint32_t lba = (uint32_t)(byteoff / 512);
    uint32_t soff = (uint32_t)(byteoff % 512);
    if (read(lba, s) != 0)
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

/* The inode-table start block of a given block group. */
static uint64_t group_inode_table(struct ext_mount *m, uint32_t group)
{
  uint8_t gd[64];
  uint64_t off = (uint64_t)m->gd_block * m->block_size + (uint64_t)group * m->desc_size;
  if (dev_read(m, off, gd, m->desc_size > 64 ? 64 : m->desc_size) != 0)
    return 0;
  uint64_t t = load_le32(gd + 8); /* bg_inode_table_lo */
  if (m->desc_size >= 64)
    t |= (uint64_t)load_le32(gd + 40) << 32; /* bg_inode_table_hi */
  return t;
}

/* Read inode `ino` (1-based) into `ibuf` (>= 256 bytes). */
static int read_inode(struct ext_mount *m, uint32_t ino, uint8_t *ibuf)
{
  if (ino == 0)
    return -1;
  uint32_t group = (ino - 1) / m->inodes_per_group;
  uint32_t index = (ino - 1) % m->inodes_per_group;
  uint64_t table = group_inode_table(m, group);
  if (!table)
    return -1;
  uint64_t off = table * m->block_size + (uint64_t)index * m->inode_size;
  uint32_t n = m->inode_size < 256 ? m->inode_size : 256;
  return dev_read(m, off, ibuf, n);
}

static int inode_is_dir(const uint8_t *ibuf) { return (load_le16(ibuf) & 0xF000) == 0x4000; }
static uint64_t inode_size(const uint8_t *ibuf)
{
  uint64_t sz = load_le32(ibuf + 4);
  if (!inode_is_dir(ibuf))
    sz |= (uint64_t)load_le32(ibuf + 108) << 32; /* i_size_high (files) */
  return sz;
}

/* ------------------------------------------------- logical -> physical block
 */

/* Walk an extent tree to map logical block `lblk` -> physical block (0 = hole).
 * `node` points at an ext4_extent_header (inline in the inode, or a tree node
 * read into blk_scratch); `node_bytes` is the usable size of that buffer, so an
 * untrusted eh_entries can't drive the entry scan past it. A guard counter caps
 * tree depth against a cyclic/self-referential tree. */
static uint32_t extent_lookup(struct ext_mount *m, const uint8_t *node, uint32_t node_bytes, uint32_t lblk)
{
  for (int guard = 0; guard < 32; guard++) {
    if (node_bytes < 12 || load_le16(node) != EXT_EXTENT_MAGIC)
      return 0;
    uint16_t entries = load_le16(node + 2);
    uint16_t depth = load_le16(node + 6);
    uint32_t maxent = (node_bytes - 12) / 12; /* entries that fit */
    if (entries > maxent)
      entries = (uint16_t)maxent;

    if (depth == 0) { /* leaf: ext4_extent */
      for (int i = 0; i < entries; i++) {
        const uint8_t *e = node + 12 + i * 12;
        uint32_t eb = load_le32(e);
        uint32_t len = load_le16(e + 4);
        if (len > 32768)
          len -= 32768; /* uninitialized */
        if (lblk >= eb && lblk < eb + len) {
          uint32_t phys = load_le32(e + 8); /* ee_start_lo */
          return phys + (lblk - eb);
        }
      }
      return 0;
    }
    /* interior: pick the last ext4_extent_idx whose ei_block <= lblk */
    const uint8_t *chosen = NULL;
    for (int i = 0; i < entries; i++) {
      const uint8_t *e = node + 12 + i * 12;
      if (load_le32(e) <= lblk)
        chosen = e;
      else
        break;
    }
    if (!chosen)
      return 0;
    uint32_t leaf = load_le32(chosen + 4); /* ei_leaf_lo */
    if (read_block(m, leaf, m->blk_scratch) != 0)
      return 0;
    node = m->blk_scratch;
    node_bytes = m->block_size;
  }
  return 0; /* depth guard tripped */
}

/* Classic ext2/3 block map: 12 direct + single/double/triple indirect. */
static uint32_t indirect_lookup(struct ext_mount *m, const uint8_t *ibuf, uint32_t lblk)
{
  const uint8_t *blk = ibuf + 40;   /* i_block[15] */
  uint32_t ppb = m->block_size / 4; /* pointers per block */

  if (lblk < 12)
    return load_le32(blk + lblk * 4);
  lblk -= 12;
  if (lblk < ppb) {
    if (read_block(m, load_le32(blk + 12 * 4), m->blk_scratch) != 0)
      return 0;
    return load_le32(m->blk_scratch + lblk * 4);
  }
  lblk -= ppb;
  if (lblk < ppb * ppb) {
    if (read_block(m, load_le32(blk + 13 * 4), m->blk_scratch) != 0)
      return 0;
    uint32_t b = load_le32(m->blk_scratch + (lblk / ppb) * 4);
    if (read_block(m, b, m->blk_scratch) != 0)
      return 0;
    return load_le32(m->blk_scratch + (lblk % ppb) * 4);
  }
  lblk -= ppb * ppb;
  if (read_block(m, load_le32(blk + 14 * 4), m->blk_scratch) != 0)
    return 0;
  uint32_t b = load_le32(m->blk_scratch + (lblk / (ppb * ppb)) * 4);
  if (read_block(m, b, m->blk_scratch) != 0)
    return 0;
  b = load_le32(m->blk_scratch + ((lblk / ppb) % ppb) * 4);
  if (read_block(m, b, m->blk_scratch) != 0)
    return 0;
  return load_le32(m->blk_scratch + (lblk % ppb) * 4);
}

static uint32_t map_block(struct ext_mount *m, const uint8_t *ibuf, uint32_t lblk)
{
  if (load_le32(ibuf + 32) & INODE_EXTENTS_FL)
    return extent_lookup(m, ibuf + 40, 60, lblk); /* i_block[15] = 60 bytes */
  return indirect_lookup(m, ibuf, lblk);
}

/* ----------------------------------------------------- directory + resolve */

/* Find child `name` (exact, case-sensitive) in directory inode `dirbuf`. On a
 * hit returns the child inode number and sets *is_dir (best-effort from the
 * dir_entry file_type); 0 if not found. Uses data_blk (so the caller must not
 * hold a data_blk it needs). */
static uint32_t dir_find(struct ext_mount *m, const uint8_t *dirbuf, const char *name, int *is_dir)
{
  uint64_t size = inode_size(dirbuf);
  uint32_t nlen = 0;
  while (name[nlen])
    nlen++;

  for (uint32_t lblk = 0; (uint64_t)lblk * m->block_size < size; lblk++) {
    uint32_t phys = map_block(m, dirbuf, lblk);
    if (!phys || read_block(m, phys, m->data_blk) != 0)
      continue;
    uint32_t off = 0;
    while (off + 8 <= m->block_size) {
      const uint8_t *e = m->data_blk + off;
      uint32_t ino = load_le32(e);
      uint16_t rec = load_le16(e + 4);
      uint8_t nl = e[6];
      if (rec < 8)
        break;
      if (off + 8 + nl > m->block_size)
        break; /* name runs past the block */
      if (ino && nl == nlen) {
        int match = 1;
        for (uint32_t i = 0; i < nlen; i++)
          if (e[8 + i] != (uint8_t)name[i]) {
            match = 0;
            break;
          }
        if (match) {
          if (is_dir)
            *is_dir = (e[7] == 2); /* file_type dir */
          return ino;
        }
      }
      off += rec;
    }
  }
  return 0;
}

/* Resolve `path` to an inode, filling ino_out/is_dir_out/size_out. 0 / -1. */
static int resolve(struct ext_mount *m, const char *path, uint32_t *ino_out, int *is_dir_out, uint64_t *size_out)
{
  uint32_t ino = EXT_ROOT_INO;
  if (read_inode(m, ino, m->cur_inode) != 0)
    return -1;

  if (path) {
    while (*path == '/')
      path++;
    while (*path) {
      const char *comp = path;
      int len = 0;
      while (path[len] && path[len] != '/')
        len++;
      char name[256];
      int k = 0;
      for (; k < len && k < 255; k++)
        name[k] = comp[k];
      name[k] = '\0';
      path += len;
      while (*path == '/')
        path++;

      if (k == 0 || (k == 1 && name[0] == '.'))
        continue;
      if (!inode_is_dir(m->cur_inode))
        return -1;
      int cd = 0;
      uint32_t child = dir_find(m, m->cur_inode, name, &cd);
      if (!child || read_inode(m, child, m->cur_inode) != 0)
        return -1;
      ino = child;
    }
  }
  if (ino_out)
    *ino_out = ino;
  if (is_dir_out)
    *is_dir_out = inode_is_dir(m->cur_inode);
  if (size_out)
    *size_out = inode_size(m->cur_inode);
  return 0;
}

/* ------------------------------------------------------------- mounting */

static int parse_superblock(struct ext_mount *m, const uint8_t *sb)
{
  if (load_le16(sb + 56) != EXT_MAGIC)
    return -1;
  uint32_t bs = 1024u << load_le32(sb + 24);
  if (bs < 1024 || bs > MAX_BLOCK)
    return -1;
  uint32_t incompat = load_le32(sb + 96);
  if (incompat & INCOMPAT_ENCRYPT) /* can't decrypt */
    return -1;
  uint32_t ipg = load_le32(sb + 40);
  if (ipg == 0)
    return -1;

  m->block_size = bs;
  m->inodes_per_group = ipg;
  m->inode_size = load_le16(sb + 88);
  if (m->inode_size < 128)
    m->inode_size = 128;
  m->desc_size = (incompat & INCOMPAT_64BIT) ? load_le16(sb + 254) : 32;
  if (m->desc_size < 32)
    m->desc_size = 32;
  m->gd_block = (bs == 1024) ? 2 : 1;

  uint32_t compat = load_le32(sb + 92);
  m->type = (incompat & INCOMPAT_EXTENTS) ? "ext4" : (compat & 0x4) ? "ext3" : "ext2"; /* has_journal */

  int n = 0; /* s_volume_name @120 */
  for (; n < 16 && sb[120 + n]; n++)
    m->label[n] = (char)sb[120 + n];
  m->label[n] = '\0';
  return 0;
}

/* Try to mount the ext volume whose Superblock is at byte 1024 of `base_lba`.
 * 0 on success (m populated), -1 on failure. */
static int try_mount(struct ext_mount *m, blk_read_fn read, uint32_t base_lba)
{
  uint8_t sb[1024];
  m->read = read;
  m->base = base_lba;
  m->label[0] = '\0';
  if (!read || dev_read(m, 1024, sb, 1024) != 0)
    return -1;
  if (parse_superblock(m, sb) != 0)
    return -1;
  /* Sanity: the root inode must read and be a directory. */
  if (read_inode(m, EXT_ROOT_INO, m->cur_inode) != 0 || !inode_is_dir(m->cur_inode))
    return -1;
  return 0;
}

/* Whole disk, then GPT, then MBR partitions (ext often lives in a partition).
 */
static int probe_dev(struct ext_mount *m, blk_read_fn read)
{
  if (try_mount(m, read, 0) == 0)
    return 0;
  uint8_t sec[512];

  if (read(1, sec) == 0 && sec[0] == 'E' && sec[1] == 'F' && sec[2] == 'I' && sec[3] == ' ' &&
      sec[4] == 'P') { /* GPT */
    uint64_t ent_lba = (uint64_t)load_le32(sec + 72) | ((uint64_t)load_le32(sec + 76) << 32);
    uint32_t ent_cnt = load_le32(sec + 80), ent_sz = load_le32(sec + 84);
    if (ent_sz < 128)
      ent_sz = 128;
    if (ent_cnt > 128)
      ent_cnt = 128;
    uint8_t ent[128];
    for (uint32_t i = 0; i < ent_cnt; i++) {
      /* ent_sz is untrusted (only floored to 128), so an entry can straddle
       * a sector boundary and its tail runs past a single 512-byte read.
       * Read exactly one 128-byte entry straddle-safe and index that. */
      if (raw_read(read, ent_lba * 512 + (uint64_t)i * ent_sz, ent, 128) != 0)
        break;
      int empty = 1;
      for (int k = 0; k < 16; k++)
        if (ent[k]) {
          empty = 0;
          break;
        }
      if (empty)
        continue;
      uint64_t first = (uint64_t)load_le32(ent + 32) | ((uint64_t)load_le32(ent + 36) << 32);
      if (first && try_mount(m, read, (uint32_t)first) == 0)
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

/* ------------------------------------------------------------------ ops */

static const char *ext_label(struct ext_mount *m) { return m->label; }
static const char *ext_type(struct ext_mount *m) { return m->type; }

static int ext_stat(struct ext_mount *m, const char *path, int *is_dir, uint32_t *size)
{
  uint64_t sz;
  int d;
  if (resolve(m, path, NULL, &d, &sz) != 0)
    return -1;
  if (is_dir)
    *is_dir = d;
  if (size)
    *size = (uint32_t)sz;
  return 0;
}

/* Shared directory walk for list (idx<0 -> print) and enum (idx -> fill). */
static int dir_walk(struct ext_mount *m, const char *path, int want_idx, char *name, int *is_dir_out,
                    uint32_t *size_out)
{
  int d;
  uint64_t sz;
  if (resolve(m, path, NULL, &d, &sz) != 0 || !d)
    return want_idx < 0 ? -1 : 0;
  memcpy(m->dir_inode, m->cur_inode, sizeof m->dir_inode); /* hold the dir inode */

  uint64_t dsize = inode_size(m->dir_inode);
  int seen = 0;
  for (uint32_t lblk = 0; (uint64_t)lblk * m->block_size < dsize; lblk++) {
    uint32_t phys = map_block(m, m->dir_inode, lblk);
    if (!phys || read_block(m, phys, m->data_blk) != 0)
      continue;
    uint32_t off = 0;
    while (off + 8 <= m->block_size) {
      const uint8_t *e = m->data_blk + off;
      uint32_t ino = load_le32(e);
      uint16_t rec = load_le16(e + 4);
      uint8_t nl = e[6];
      if (rec < 8)
        break;
      if (off + 8 + nl > m->block_size)
        break; /* name runs past the block */
      if (ino && nl) {
        char nm[256];
        int j = 0;
        for (; j < nl && j < 255; j++)
          nm[j] = (char)e[8 + j];
        nm[j] = '\0';
        int skip = (nm[0] == '.' && (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0')));
        if (!skip) {
          int isd = (e[7] == 2);
          uint32_t fsz = 0;
          if (want_idx < 0) { /* print mode */
            uint32_t s = 0;
            if (read_inode(m, ino, m->cur_inode) == 0) {
              isd = inode_is_dir(m->cur_inode);
              s = (uint32_t)inode_size(m->cur_inode);
            }
            con_write(isd ? "  <DIR>  " : "         ");
            if (!isd) {
              con_printf("%u  ", s);
            } else {
              con_write("  ");
            }
            con_printf("%s\n", nm);
          } else if (seen == want_idx) { /* index mode */
            if (read_inode(m, ino, m->cur_inode) == 0) {
              isd = inode_is_dir(m->cur_inode);
              fsz = (uint32_t)inode_size(m->cur_inode);
            }
            int mm = 0;
            for (; mm < 255 && nm[mm]; mm++)
              name[mm] = nm[mm];
            name[mm] = '\0';
            if (is_dir_out)
              *is_dir_out = isd;
            if (size_out)
              *size_out = fsz;
            return 1;
          }
          seen++;
        }
      }
      off += rec;
    }
  }
  return want_idx < 0 ? 0 : 0;
}

static int ext_list(struct ext_mount *m, const char *path) { return dir_walk(m, path, -1, NULL, NULL, NULL); }

static int ext_enum_dir(struct ext_mount *m, const char *path, int idx, char *name, int *is_dir, uint32_t *size)
{
  if (idx < 0)
    return 0;
  return dir_walk(m, path, idx, name, is_dir, size);
}

static int ext_read_at(struct ext_mount *m, const char *path, uint32_t offset, void *buf, uint32_t len)
{
  uint64_t sz;
  int d;
  if (resolve(m, path, NULL, &d, &sz) != 0 || d)
    return -1;                                             /* absent / a dir */
  memcpy(m->dir_inode, m->cur_inode, sizeof m->dir_inode); /* the file inode */

  if (offset >= sz)
    return 0;
  if (offset + len > sz)
    len = (uint32_t)(sz - offset);

  uint8_t *dst = buf;
  uint32_t got = 0;
  while (got < len) {
    uint32_t pos = offset + got;
    uint32_t lblk = pos / m->block_size;
    uint32_t boff = pos % m->block_size;
    uint32_t phys = map_block(m, m->dir_inode, lblk);
    uint32_t chunk = m->block_size - boff;
    if (chunk > len - got)
      chunk = len - got;
    if (!phys) { /* sparse hole */
      for (uint32_t i = 0; i < chunk; i++)
        dst[got + i] = 0;
    } else {
      if (read_block(m, phys, m->data_blk) != 0)
        return got ? (int)got : -1;
      memcpy(dst + got, m->data_blk + boff, chunk);
    }
    got += chunk;
  }
  return (int)got;
}

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

static void ext_close(void *inst) { kfree(inst); }

static const char *op_label(void *inst) { return ext_label((struct ext_mount *)inst); }
static const char *op_type(void *inst) { return ext_type((struct ext_mount *)inst); }

static int op_list(void *inst, const char *path) { return ext_list((struct ext_mount *)inst, path); }
static int op_stat(void *inst, const char *path, int *is_dir, uint32_t *size)
{
  return ext_stat((struct ext_mount *)inst, path, is_dir, size);
}
static int op_read_at(void *inst, const char *path, uint32_t off, void *buf, uint32_t len)
{
  return ext_read_at((struct ext_mount *)inst, path, off, buf, len);
}
static int op_enum_dir(void *inst, const char *path, int idx, char *name, int *is_dir, uint32_t *size)
{
  return ext_enum_dir((struct ext_mount *)inst, path, idx, name, is_dir, size);
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
  };
  return &ops;
}
