/* hfs.c: read-only HFS+ driver (see hfs.h). Big-endian; scans catalog leaf nodes linearly. */

#include <stddef.h>

#include "console.h"
#include "endian.h"
#include "heap.h"
#include "hfs.h"
#include "kstring.h"

#define HFS_SIG 0x482Bu  /* 'H+' */
#define HFSX_SIG 0x4858u /* 'HX' */
#define ROOT_CNID 2u
#define MAX_NODE 8192 /* catalog node-size cap we support */

struct extent {
  uint32_t start, count;
}; /* in allocation blocks */

/* All per-mount state. One per mounted volume, heap-allocated in hfs_probe. */
struct hfs_mount {
  blk_read_fn read;
  uint32_t base;       /* absolute LBA of byte 0 of the volume */
  uint32_t block_size; /* allocation block size (bytes) */
  uint32_t total_blocks;
  struct extent cat_ext[8]; /* catalog file fork extents */
  uint32_t node_size;       /* catalog B-tree node size */
  uint32_t first_leaf;      /* first leaf node index */
  uint32_t max_nodes;       /* nodes the catalog can hold: bounds a leaf-chain walk */
  char label[64];
  uint8_t nodebuf[MAX_NODE]; /* one catalog node, reused per-mount */
};

/* A resolved catalog object. */
struct obj {
  int is_dir;
  uint32_t cnid;        /* folder id (dirs) */
  uint64_t size;        /* data fork logical size (files) */
  struct extent ext[8]; /* data fork inline extents (files) */
};

/* Read `len` bytes from an absolute device byte offset. */
static int dev_read(struct hfs_mount *m, uint64_t byteoff, uint8_t *buf, uint32_t len)
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

/* Map a catalog-file offset to a device byte offset via the fork extents. 0 on success. */
static int cat_dev_off(struct hfs_mount *m, uint64_t logoff, uint64_t *devoff)
{
  uint64_t pos = 0;
  for (int i = 0; i < 8 && m->cat_ext[i].count; i++) {
    uint64_t span = (uint64_t)m->cat_ext[i].count * m->block_size;
    if (logoff < pos + span) {
      *devoff = (uint64_t)m->cat_ext[i].start * m->block_size + (logoff - pos);
      return 0;
    }
    pos += span;
  }
  return -1;
}

/* Read catalog node `idx` into m->nodebuf. */
static int read_node(struct hfs_mount *m, uint32_t idx)
{
  uint64_t devoff;
  if (cat_dev_off(m, (uint64_t)idx * m->node_size, &devoff) != 0)
    return -1;
  return dev_read(m, devoff, m->nodebuf, m->node_size);
}

/* Byte offset of record `i` (from the node's trailing offset array). */
static uint32_t rec_off(struct hfs_mount *m, int i) { return load_be16(m->nodebuf + m->node_size - 2 * (i + 1)); }

/* Convert an HFSUniStr255 name (UTF-16BE) to ASCII into out[cap]. */
static void uni_ascii(const uint8_t *p, int n, char *out, int cap)
{
  int o = 0;
  for (int i = 0; i < n && o < cap - 1; i++) {
    uint16_t u = load_be16(p + i * 2);
    out[o++] = (u >= 0x20 && u < 0x7F) ? (char)u : '?';
  }
  out[o] = '\0';
}

/* A catalog record, located within the current node. */
struct catrec {
  uint32_t parent;     /* key: parent CNID */
  const uint8_t *name; /* key: UTF-16BE name units */
  int namelen;         /* in UTF-16 units */
  uint16_t type;       /* 1 folder, 2 file, 3/4 thread */
  const uint8_t *data; /* record body (after the key) */
};

/* Parse record `i`, bounds-checking against the node size. 0, or -1 if out of range. */
static int parse_rec(struct hfs_mount *m, int i, struct catrec *c)
{
  uint32_t ns = m->node_size;
  if ((uint32_t)(2 * (i + 1)) > ns)
    return -1;
  uint32_t ro = rec_off(m, i);
  if (ro + 8 > ns) /* keyLen(2)+parent(4)+namelen(2) */
    return -1;
  uint16_t keyLen = load_be16(m->nodebuf + ro);
  c->parent = load_be32(m->nodebuf + ro + 2);
  c->namelen = load_be16(m->nodebuf + ro + 6);
  c->name = m->nodebuf + ro + 8;
  if ((uint32_t)ro + 8 + (uint32_t)c->namelen * 2 > ns) /* name units */
    return -1;
  uint32_t body = (uint32_t)ro + 2 + keyLen; /* key length excludes its field */
  if (body + 2 > ns)                         /* at least the 2-byte type */
    return -1;
  c->data = m->nodebuf + body;
  c->type = load_be16(c->data);
  /* rec_to_obj reads a folder body to +12 and a file body to +168. */
  uint32_t need = (c->type == 1) ? 12u : (c->type == 2) ? 168u : 0u;
  if (need && body + need > ns)
    return -1;
  return 0;
}

/* Hidden if the name is empty or starts with a control char (private/hardlink dirs). */
static bool rec_hidden(const struct catrec *c)
{
  if (c->namelen == 0)
    return true;
  return load_be16(c->name) < 0x20;
}

/* Fill an obj from a folder/file record body. */
static void rec_to_obj(const struct catrec *c, struct obj *o)
{
  if (c->type == 1) { /* folder */
    o->is_dir = 1;
    o->cnid = load_be32(c->data + 8);
    o->size = 0;
  } else { /* file */
    o->is_dir = 0;
    o->cnid = load_be32(c->data + 8);
    const uint8_t *fork = c->data + 88; /* HFSPlusForkData (data fork) */
    o->size = load_be64(fork);
    for (int i = 0; i < 8; i++) {
      o->ext[i].start = load_be32(fork + 16 + i * 8);
      o->ext[i].count = load_be32(fork + 16 + i * 8 + 4);
    }
  }
}

/* Call visit() on every leaf record; a non-zero return stops the scan and is returned. Reuses nodebuf. */
static int for_each_leaf(struct hfs_mount *m, int (*visit)(struct hfs_mount *, const struct catrec *, void *),
                         void *ctx)
{
  uint32_t node = m->first_leaf, mark = node, mark_at = 1, hops = 0;
  while (node && node < m->max_nodes) {
    if (read_node(m, node) != 0)
      return 0;
    if (m->nodebuf[8] != 0xFF) /* kind: leaf == 0xFF */
      return 0;
    int nrec = load_be16(m->nodebuf + 10);
    uint32_t fLink = load_be32(m->nodebuf + 0);
    /* cap the untrusted record count by the offset array's capacity */
    if (nrec < 0 || (uint32_t)nrec > m->node_size / 2)
      nrec = (int)(m->node_size / 2);
    for (int i = 0; i < nrec; i++) {
      struct catrec c;
      if (parse_rec(m, i, &c) != 0)
        continue; /* malformed record: skip it */
      int r = visit(m, &c, ctx);
      if (r)
        return r;
    }
    /* Untrusted links: a chain longer than the tree, or one that returns to a node (Brent), is corrupt. */
    if (++hops >= m->max_nodes || fLink == mark)
      return 0;
    if (hops == mark_at) {
      mark = fLink;
      mark_at *= 2;
    }
    node = fLink;
  }
  return 0;
}

struct find_ctx {
  uint32_t parent;
  const char *name;
  bool found;
  struct obj out;
};

/* Full case-insensitive ASCII compare of two NUL-terminated names. */
static bool names_match(const char *want, const char *have)
{
  int i = 0;
  for (; want[i] && have[i]; i++)
    if (k_lower(want[i]) != k_lower(have[i]))
      return false;
  return want[i] == '\0' && have[i] == '\0';
}

static int visit_find2(struct hfs_mount *m, const struct catrec *c, void *vp)
{
  (void)m;
  struct find_ctx *f = vp;
  if (c->parent != f->parent || (c->type != 1 && c->type != 2))
    return 0;
  char nm[256];
  uni_ascii(c->name, c->namelen, nm, sizeof nm);
  if (names_match(f->name, nm)) {
    rec_to_obj(c, &f->out);
    f->found = true;
    return 1; /* stop scanning */
  }
  return 0;
}

static int find_child(struct hfs_mount *m, uint32_t parent, const char *name, struct obj *out)
{
  struct find_ctx f;
  f.parent = parent;
  f.name = name;
  f.found = false;
  for_each_leaf(m, visit_find2, &f);
  if (!f.found)
    return -1;
  *out = f.out;
  return 0;
}

/* Resolve `path` to an object. The root ("" / "/") is the folder CNID 2. */
static int resolve(struct hfs_mount *m, const char *path, struct obj *out)
{
  out->is_dir = 1;
  out->cnid = ROOT_CNID;
  out->size = 0;

  if (!path)
    return 0;
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
    if (!out->is_dir)
      return -1;
    struct obj child;
    if (find_child(m, out->cnid, name, &child) != 0)
      return -1;
    *out = child;
  }
  return 0;
}

static int parse_volume_header(struct hfs_mount *m, const uint8_t *vh)
{
  uint16_t sig = load_be16(vh);
  if (sig != HFS_SIG && sig != HFSX_SIG)
    return -1;
  m->block_size = load_be32(vh + 40);
  m->total_blocks = load_be32(vh + 44);
  if (m->block_size < 512 || (m->block_size % 512) != 0)
    return -1;

  const uint8_t *cf = vh + 272; /* catalogFile fork */
  for (int i = 0; i < 8; i++) {
    m->cat_ext[i].start = load_be32(cf + 16 + i * 8);
    m->cat_ext[i].count = load_be32(cf + 16 + i * 8 + 4);
  }
  if (m->cat_ext[0].count == 0)
    return -1;
  return 0;
}

/* The label is the folder record whose parent CNID is 1. */
static int visit_label(struct hfs_mount *m, const struct catrec *c, void *vp)
{
  (void)vp;
  if (c->type == 1 && c->parent == 1) {
    uni_ascii(c->name, c->namelen, m->label, sizeof m->label);
    return 1;
  }
  return 0;
}

/* Mount the volume whose header sits at byte 1024 of `base_lba`. 0, or -1. */
static int hfs_mount(struct hfs_mount *m, uint32_t base_lba)
{
  uint8_t vh[512];
  m->base = base_lba;

  if (!m->read || m->read(base_lba + 2, vh) != 0) /* volume header @ byte 1024 */
    return -1;
  if (parse_volume_header(m, vh) != 0)
    return -1;

  /* Catalog B-tree header record (treeDepth/rootNode/firstLeafNode/nodeSize at node+14). */
  uint64_t devoff;
  if (cat_dev_off(m, 0, &devoff) != 0)
    return -1;
  uint8_t hdr[512];
  if (dev_read(m, devoff, hdr, 512) != 0)
    return -1;
  const uint8_t *hr = hdr + 14; /* BTHeaderRec */
  m->first_leaf = load_be32(hr + 10);
  m->node_size = load_be16(hr + 18);
  if (m->node_size < 512 || m->node_size > MAX_NODE || (m->node_size % 512) != 0)
    return -1;
  uint64_t cat_bytes = 0;
  for (int i = 0; i < 8; i++)
    cat_bytes += (uint64_t)m->cat_ext[i].count * m->block_size;
  uint64_t fit = cat_bytes / m->node_size, total = load_be32(hr + 22); /* BTHeaderRec.totalNodes */
  m->max_nodes = (uint32_t)(total < fit ? total : fit);

  m->label[0] = '\0';
  for_each_leaf(m, visit_label, NULL);
  if (!m->label[0]) {
    const char *d = "HFS+ Volume";
    int i = 0;
    for (; d[i]; i++)
      m->label[i] = d[i];
    m->label[i] = '\0';
  }
  return 0;
}

/* Raw-callback variant of the above, used before m->read is set. */
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

/* Try a whole-disk mount, then GPT partitions, then an Apple Partition Map. */
static int probe(struct hfs_mount *m, blk_read_fn read)
{
  if (hfs_mount(m, 0) == 0)
    return 0;

  uint8_t sec[512];

  /* GPT: header at LBA 1 ("EFI PART"), entry array described in LE fields. */
  if (read(1, sec) == 0 && sec[0] == 'E' && sec[1] == 'F' && sec[2] == 'I' && sec[3] == ' ' && sec[4] == 'P') {
    uint64_t ent_lba = (uint64_t)load_le32(sec + 72) | ((uint64_t)load_le32(sec + 76) << 32);
    uint32_t ent_cnt = load_le32(sec + 80);
    uint32_t ent_sz = load_le32(sec + 84);
    if (ent_sz < 128)
      ent_sz = 128;
    if (ent_cnt > 128)
      ent_cnt = 128;
    uint8_t ent[128];
    for (uint32_t i = 0; i < ent_cnt; i++) {
      if (raw_read(read, ent_lba * 512 + (uint64_t)i * ent_sz, ent, 128) != 0)
        break;
      bool empty = true; /* zero type GUID => unused slot */
      for (int b = 0; b < 16; b++)
        if (ent[b]) {
          empty = false;
          break;
        }
      if (empty)
        continue;
      uint64_t first = (uint64_t)load_le32(ent + 32) | ((uint64_t)load_le32(ent + 36) << 32);
      if (first && hfs_mount(m, (uint32_t)first) == 0)
        return 0;
    }
  }

  /* Apple Partition Map: block 1 starts with 'PM' (0x504D), big-endian. */
  if (read(1, sec) == 0 && sec[0] == 0x50 && sec[1] == 0x4D) {
    uint32_t mapblks = load_be32(sec + 4); /* pmMapBlkCnt */
    if (mapblks > 64)
      mapblks = 64;
    for (uint32_t i = 1; i <= mapblks; i++) {
      if (read(i, sec) != 0)
        break;
      if (sec[0] != 0x50 || sec[1] != 0x4D)
        continue;
      uint32_t pstart = load_be32(sec + 8); /* pmPyPartStart */
      if (pstart && hfs_mount(m, pstart) == 0)
        return 0;
    }
  }
  return -1;
}

static int hfs_stat(struct hfs_mount *m, const char *path, int *is_dir, uint32_t *size)
{
  struct obj o;
  if (resolve(m, path, &o) != 0)
    return -1;
  if (is_dir)
    *is_dir = o.is_dir;
  if (size)
    *size = (uint32_t)o.size;
  return 0;
}

struct list_ctx {
  uint32_t parent;
  int want_idx; /* -1 => print all; else fill *out for that index */
  int seen;
  char *name;
  int *is_dir;
  uint32_t *size;
  int hit;
};

static int visit_list(struct hfs_mount *m, const struct catrec *c, void *vp)
{
  (void)m;
  struct list_ctx *L = vp;
  if (c->parent != L->parent || (c->type != 1 && c->type != 2))
    return 0;
  if (rec_hidden(c))
    return 0;
  char nm[256];
  uni_ascii(c->name, c->namelen, nm, sizeof nm);

  if (L->want_idx < 0) { /* print mode */
    struct obj o;
    rec_to_obj(c, &o);
    con_write(o.is_dir ? "  <DIR>  " : "         ");
    if (!o.is_dir) {
      con_printf("%u  ", (uint32_t)o.size);
    } else {
      con_write("  ");
    }
    con_printf("%s\n", nm);
    return 0;
  }
  /* index mode */
  if (L->seen == L->want_idx) {
    struct obj o;
    rec_to_obj(c, &o);
    fs_enum_name(L->name, nm, 255);
    if (L->is_dir)
      *L->is_dir = o.is_dir;
    if (L->size)
      *L->size = (uint32_t)o.size;
    L->hit = 1;
    return 1;
  }
  L->seen++;
  return 0;
}

static int hfs_list(struct hfs_mount *m, const char *path)
{
  struct obj d;
  if (resolve(m, path, &d) != 0)
    return -1;
  if (!d.is_dir)
    return -1;
  struct list_ctx L = {d.cnid, -1, 0, 0, 0, 0, 0};
  for_each_leaf(m, visit_list, &L);
  return 0;
}

static int hfs_enum_dir(struct hfs_mount *m, const char *path, int idx, char *name, int *is_dir, uint32_t *size)
{
  if (idx < 0)
    return 0;
  struct obj d;
  if (resolve(m, path, &d) != 0 || !d.is_dir)
    return 0;
  struct list_ctx L = {d.cnid, idx, 0, name, is_dir, size, 0};
  for_each_leaf(m, visit_list, &L);
  return L.hit;
}

static int hfs_read_at(struct hfs_mount *m, const char *path, uint32_t offset, void *buf, uint32_t len)
{
  struct obj o;
  if (resolve(m, path, &o) != 0 || o.is_dir)
    return -1;
  if (offset >= o.size)
    return 0;
  if (offset + len > o.size)
    len = (uint32_t)(o.size - offset);

  uint8_t *dst = buf;
  uint32_t got = 0;
  while (got < len) {
    uint32_t cur = offset + got;
    uint64_t pos = 0;
    int fi = -1;
    uint64_t within = 0;
    for (int i = 0; i < 8 && o.ext[i].count; i++) {
      uint64_t span = (uint64_t)o.ext[i].count * m->block_size;
      if (cur < pos + span) {
        fi = i;
        within = cur - pos;
        break;
      }
      pos += span;
    }
    if (fi < 0)
      break; /* beyond inline extents */
    uint64_t devoff = (uint64_t)o.ext[fi].start * m->block_size + within;
    uint64_t remain = (uint64_t)o.ext[fi].count * m->block_size - within;
    uint32_t chunk = len - got;
    if (chunk > remain)
      chunk = (uint32_t)remain;
    if (dev_read(m, devoff, dst + got, chunk) != 0)
      return got ? (int)got : -1;
    got += chunk;
  }
  return (int)got;
}

static void *hfs_op_probe(blk_read_fn read, blk_write_fn write)
{
  (void)write; /* read-only */
  if (!read)
    return NULL;
  struct hfs_mount *m = kmalloc(sizeof *m);
  if (!m)
    return NULL;
  m->read = read;
  if (probe(m, read) != 0) {
    kfree(m);
    return NULL;
  }
  return m;
}

static void hfs_op_close(void *inst) { kfree(inst); }

static const char *hfs_op_label(void *inst) { return ((struct hfs_mount *)inst)->label; }

static const char *hfs_op_type(void *inst)
{
  (void)inst;
  return "HFS+";
}

static int hfs_op_list(void *inst, const char *path) { return hfs_list(inst, path); }

static int hfs_op_stat(void *inst, const char *path, int *is_dir, uint32_t *size)
{
  return hfs_stat(inst, path, is_dir, size);
}

static int hfs_op_read_at(void *inst, const char *path, uint32_t off, void *buf, uint32_t len)
{
  return hfs_read_at(inst, path, off, buf, len);
}

static int hfs_op_enum_dir(void *inst, const char *path, int idx, char *name, int *is_dir, uint32_t *size)
{
  return hfs_enum_dir(inst, path, idx, name, is_dir, size);
}

static const struct fs_ops hfs_fs_ops = {
    .fsname = "HFS+",
    .probe = hfs_op_probe,
    .close = hfs_op_close,
    .label = hfs_op_label,
    .type = hfs_op_type,
    .list = hfs_op_list,
    .stat = hfs_op_stat,
    .read_at = hfs_op_read_at,
    .enum_dir = hfs_op_enum_dir,
    .create = NULL,
    .del = NULL,
    .mkdir = NULL,
    .rmdir = NULL,
};

const struct fs_ops *hfs_ops(void) { return &hfs_fs_ops; }
