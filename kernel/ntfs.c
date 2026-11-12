/* ntfs.c: read-only NTFS driver (see ntfs.h). Resident/simple non-resident $DATA and index directories; fails closed.
 */

#include <stddef.h>

#include "console.h"
#include "endian.h"
#include "heap.h"
#include "kstring.h"
#include "ntfs.h"

#define NTFS_MAX_REC 4096u      /* largest MFT record we handle           */
#define NTFS_MAX_CLUSTER 65536u /* largest cluster we buffer (64 KiB cap) */
#define NTFS_MAX_INDX 65536u    /* largest INDX block we buffer           */
#define NTFS_MAX_NAME 255u      /* UTF-16 units in a name                 */

#define ATTR_END 0xFFFFFFFFu
#define ATTR_VOLUME_NAME 0x60u
#define ATTR_FILE_NAME 0x30u
#define ATTR_DATA 0x80u
#define ATTR_INDEX_ROOT 0x90u
#define ATTR_INDEX_ALLOC 0xA0u

#define MFT_REC_VOLUME 3u
#define MFT_REC_ROOT 5u
#define MFT_FIRST_USER 16u /* records below this are system metafiles */

#define FNAME_DIR_FLAG 0x10000000u /* $FILE_NAME file-flags directory bit */
#define INDEX_ENTRY_NODE 0x01u     /* index-entry flag: has a sub-node    */
#define INDEX_ENTRY_END 0x02u      /* index-entry flag: end-of-list entry */

/* Per-mount state, heap-allocated (scratch buffers included). */
struct ntfs_mount {
  blk_read_fn read;
  uint32_t bytes_per_sector;
  uint32_t sectors_per_cluster;
  uint32_t bytes_per_cluster;
  uint32_t mft_lcn;      /* $MFT start cluster (LCN)             */
  uint32_t mft_rec_size; /* bytes per MFT record                 */

  /* $MFT's $DATA runlist (vcn_start, length, lcn), decoded at probe time. */
  uint32_t mft_run_n;
  uint64_t mft_run_vcn[64]; /* first VCN of the run                 */
  uint64_t mft_run_len[64]; /* run length in clusters               */
  int64_t mft_run_lcn[64];  /* run start LCN (negative = absent)    */

  uint8_t rec[NTFS_MAX_REC];       /* one MFT record being inspected       */
  uint8_t dir_rec[NTFS_MAX_REC];   /* the directory record being walked    */
  uint8_t clbuf[NTFS_MAX_CLUSTER]; /* one cluster, private to read_runlist  */
  uint8_t ixbuf[NTFS_MAX_INDX];    /* one INDX block of a directory index  */
  char label[64];
};

/* Read `len` bytes at absolute byte offset `byteoff`. 0 / -1. */
static int dev_read(struct ntfs_mount *m, uint64_t byteoff, uint8_t *buf, uint32_t len)
{
  uint8_t s[512];
  while (len) {
    uint32_t lba = (uint32_t)(byteoff / 512);
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

/* Read one cluster (LCN) into `buf` (must be >= bytes_per_cluster). */
static int read_cluster(struct ntfs_mount *m, uint64_t lcn, uint8_t *buf)
{
  return dev_read(m, lcn * m->bytes_per_cluster, buf, m->bytes_per_cluster);
}

/* Apply the update-sequence array: its words replace the last 2 bytes of each 512-byte stride. 0 / -1. */
static int apply_fixup(uint8_t *buf, uint32_t total, uint16_t usa_off, uint16_t usa_cnt)
{
  if (usa_cnt < 1)
    return -1;
  /* usa_cnt-1 strides of 512 bytes must fit in the record. */
  if ((uint32_t)(usa_cnt - 1) * 512u > total)
    return -1;
  if ((uint32_t)usa_off + (uint32_t)usa_cnt * 2u > total)
    return -1;

  const uint8_t *usa = buf + usa_off;
  for (uint32_t i = 1; i < usa_cnt; i++) {
    uint32_t pos = i * 512u - 2u; /* last 2 bytes of stride i-1 */
    if (pos + 2u > total)
      return -1;
    if (buf[pos] != usa[0] || buf[pos + 1] != usa[1]) /* torn or corrupt sector */
      return -1;
    buf[pos] = usa[i * 2];
    buf[pos + 1] = usa[i * 2 + 1];
  }
  return 0;
}

/* Decode one run header (low nibble length bytes, high nibble signed LCN delta bytes). Next pointer or NULL. */
static const uint8_t *decode_run(const uint8_t *p, const uint8_t *end, uint64_t *len_out, int64_t *delta_out)
{
  uint8_t hdr = *p++;
  uint32_t lbytes = hdr & 0x0F;
  uint32_t dbytes = (hdr >> 4) & 0x0F;
  if (lbytes == 0 || lbytes > 8 || dbytes > 8)
    return NULL;
  if (p + lbytes + dbytes > end)
    return NULL;

  uint64_t len = 0;
  for (uint32_t i = 0; i < lbytes; i++)
    len |= (uint64_t)p[i] << (8 * i);
  p += lbytes;

  int64_t delta = 0;
  if (dbytes) {
    uint64_t raw = 0;
    for (uint32_t i = 0; i < dbytes; i++)
      raw |= (uint64_t)p[i] << (8 * i);
    /* sign-extend the dbytes-wide value */
    if (p[dbytes - 1] & 0x80) {
      uint64_t mask = ~0ull;
      if (dbytes < 8)
        mask <<= (8 * dbytes);
      raw |= mask;
    }
    delta = (int64_t)raw;
    p += dbytes;
  }
  *len_out = len;
  *delta_out = delta;
  return p;
}

/* Read from a non-resident attribute with mapping pairs [mp, mp_end), via clbuf. Bytes read or -1; holes are zero. */
static int read_runlist(struct ntfs_mount *m, const uint8_t *mp, const uint8_t *mp_end, uint64_t off, uint8_t *dst,
                        uint32_t len)
{
  uint32_t cl = m->bytes_per_cluster;
  uint32_t got = 0;
  uint64_t vcn = 0;
  int64_t lcn = 0;
  const uint8_t *p = mp;
  int guard = 0;

  while (got < len) {
    /* Rescan the runlist for the run holding the next VCN. */
    uint64_t want_vcn = (off + got) / cl;
    const uint8_t *q = mp;
    uint64_t cur_vcn = 0;
    int64_t cur_lcn = 0;
    int found = 0;
    int hole = 0;
    int steps = 0;
    while (q < mp_end && *q != 0x00) {
      if (++steps > 4096)
        return -1;
      uint64_t rlen;
      int64_t delta;
      const uint8_t *nx = decode_run(q, mp_end, &rlen, &delta);
      if (!nx)
        return -1;
      cur_lcn += delta;
      int is_hole = ((q[0] >> 4) & 0x0F) == 0; /* no LCN delta = sparse */
      if (want_vcn >= cur_vcn && want_vcn < cur_vcn + rlen) {
        found = 1;
        hole = is_hole;
        vcn = cur_vcn;
        lcn = cur_lcn;
        break;
      }
      cur_vcn += rlen;
      q = nx;
    }
    (void)p;
    (void)vcn;
    if (!found)
      break;

    uint32_t boff = (uint32_t)((off + got) % cl);
    uint32_t chunk = cl - boff;
    if (chunk > len - got)
      chunk = len - got;

    if (hole) {
      memset(dst + got, 0, chunk);
    } else {
      uint64_t this_lcn = (uint64_t)lcn + (want_vcn - vcn);
      if (read_cluster(m, this_lcn, m->clbuf) != 0)
        return got ? (int)got : -1;
      memcpy(dst + got, m->clbuf + boff, chunk);
    }
    got += chunk;
    if (++guard > 1000000)
      return -1;
  }
  return (int)got;
}

/* Read an MFT record at `byteoff`, check "FILE" magic and apply fixup. 0 / -1. */
static int read_mft_record_at(struct ntfs_mount *m, uint64_t byteoff, uint8_t *buf)
{
  uint32_t rs = m->mft_rec_size;
  if (rs > NTFS_MAX_REC)
    return -1;
  if (dev_read(m, byteoff, buf, rs) != 0)
    return -1;
  if (buf[0] != 'F' || buf[1] != 'I' || buf[2] != 'L' || buf[3] != 'E')
    return -1;
  uint16_t usa_off = load_le16(buf + 0x04);
  uint16_t usa_cnt = load_le16(buf + 0x06);
  if (apply_fixup(buf, rs, usa_off, usa_cnt) != 0)
    return -1;
  return 0;
}

/* Map MFT record `recno` to its byte offset via $MFT's runlist. 0 / -1. */
static int mft_record_offset(struct ntfs_mount *m, uint64_t recno, uint64_t *off_out)
{
  uint64_t rec_byte = recno * m->mft_rec_size;
  uint64_t vcn = rec_byte / m->bytes_per_cluster;
  uint64_t inclu = rec_byte % m->bytes_per_cluster;

  for (uint32_t i = 0; i < m->mft_run_n; i++) {
    if (vcn >= m->mft_run_vcn[i] && vcn < m->mft_run_vcn[i] + m->mft_run_len[i]) {
      if (m->mft_run_lcn[i] < 0)
        return -1; /* sparse MFT run -- unsupported */
      uint64_t lcn = (uint64_t)m->mft_run_lcn[i] + (vcn - m->mft_run_vcn[i]);
      *off_out = lcn * m->bytes_per_cluster + inclu;
      return 0;
    }
  }
  return -1;
}

/* Read MFT record `recno` into `buf` (>= NTFS_MAX_REC). Returns 0 / -1. */
static int read_mft_record(struct ntfs_mount *m, uint64_t recno, uint8_t *buf)
{
  uint64_t off;
  if (mft_record_offset(m, recno, &off) != 0)
    return -1;
  return read_mft_record_at(m, off, buf);
}

/* Find the first attribute of `type` in `rec`, or NULL. */
static const uint8_t *find_attr(const uint8_t *rec, uint32_t rec_size, uint32_t type)
{
  uint16_t aoff = load_le16(rec + 0x14);
  uint32_t used = load_le32(rec + 0x18);
  if (used > rec_size)
    used = rec_size;
  if (aoff < 0x18 || aoff + 8u > used)
    return NULL;

  uint32_t off = aoff;
  int guard = 0;
  while (off + 8u <= used) {
    if (++guard > 256)
      return NULL;
    const uint8_t *a = rec + off;
    uint32_t atype = load_le32(a);
    if (atype == ATTR_END)
      return NULL;
    uint32_t alen = load_le32(a + 4);
    if (alen < 8 || off + alen > used)
      return NULL;
    if (atype == type)
      return a;
    off += alen;
  }
  return NULL;
}

/* Resident attribute value pointer and length. 0 / -1 (non-resident or malformed). */
static int resident_value(const uint8_t *a, uint32_t arec_room, const uint8_t **vp, uint32_t *vlen)
{
  if (a[8] != 0)
    return -1;
  uint32_t alen = load_le32(a + 4);
  if (alen < 0x18) /* the resident header itself must fit before its fields are read */
    return -1;
  uint32_t vlength = load_le32(a + 0x10);
  uint16_t voff = load_le16(a + 0x14);
  if (voff + (uint64_t)vlength > alen)
    return -1;
  if (alen > arec_room)
    return -1;
  *vp = a + voff;
  *vlen = vlength;
  return 0;
}

/* UTF-16LE to ASCII (non-ASCII becomes '?'); writes at most cap-1 chars + NUL, returns the count. */
static int utf16_to_ascii(const uint8_t *src, uint32_t units, char *dst, uint32_t cap)
{
  if (units > NTFS_MAX_NAME)
    units = NTFS_MAX_NAME;
  uint32_t n = 0;
  for (uint32_t i = 0; i < units && n + 1 < cap; i++) {
    uint16_t u = load_le16(src + i * 2);
    dst[n++] = (u < 0x80) ? (char)u : '?';
  }
  dst[n] = '\0';
  return (int)n;
}

/* Locate the unnamed $DATA of `rec`: resident ptr/len, or non-resident mapping pairs [*mp,*mp_end). 0 / -1. */
static int find_data(struct ntfs_mount *m, const uint8_t *rec, int *resident, const uint8_t **res_ptr,
                     uint32_t *res_len, const uint8_t **mp, const uint8_t **mp_end, uint64_t *size)
{
  uint32_t used = load_le32(rec + 0x18);
  if (used > m->mft_rec_size)
    used = m->mft_rec_size;

  /* Walk by hand to require an unnamed $DATA. */
  uint16_t aoff = load_le16(rec + 0x14);
  uint32_t off = aoff;
  int guard = 0;
  while (off + 8u <= used) {
    if (++guard > 256)
      return -1;
    const uint8_t *a = rec + off;
    uint32_t atype = load_le32(a);
    if (atype == ATTR_END)
      break;
    uint32_t alen = load_le32(a + 4);
    if (alen < 8 || off + alen > used)
      return -1;
    if (alen >= 0x10 && atype == ATTR_DATA && a[9] == 0) {
      if (a[8] == 0) {
        if (alen < 0x18)
          return -1;
        uint32_t vlen = load_le32(a + 0x10);
        uint16_t voff = load_le16(a + 0x14);
        if (voff + (uint64_t)vlen > alen)
          return -1;
        *resident = 1;
        *res_ptr = a + voff;
        *res_len = vlen;
        *size = vlen;
        return 0;
      } else {
        if (alen < 0x40)
          return -1;
        uint16_t mp_off = load_le16(a + 0x20);
        uint64_t real_sz = load_le64(a + 0x30);
        if (mp_off < 0x40 || mp_off >= alen)
          return -1;
        *resident = 0;
        *mp = a + mp_off;
        *mp_end = a + alen;
        *size = real_sz;
        return 0;
      }
    }
    off += alen;
  }
  return -1;
}

/* Size of a record's $DATA (0 if none). */
static uint64_t data_size(struct ntfs_mount *m, const uint8_t *rec)
{
  int resident;
  const uint8_t *rp, *mp, *mpe;
  uint32_t rl;
  uint64_t sz;
  if (find_data(m, rec, &resident, &rp, &rl, &mp, &mpe, &sz) != 0)
    return 0;
  return sz;
}

/* Read [off, off+len) of `rec`'s $DATA into dst. Returns bytes read or -1. */
static int read_data(struct ntfs_mount *m, const uint8_t *rec, uint64_t off, uint8_t *dst, uint32_t len)
{
  int resident;
  const uint8_t *rp, *mp, *mpe;
  uint32_t rl;
  uint64_t sz;
  if (find_data(m, rec, &resident, &rp, &rl, &mp, &mpe, &sz) != 0)
    return -1;
  if (off >= sz)
    return 0;
  if (off + len > sz)
    len = (uint32_t)(sz - off);

  if (resident) {
    if (off >= rl)
      return 0;
    uint32_t avail = rl - (uint32_t)off;
    if (len > avail)
      len = avail;
    memcpy(dst, rp + off, len);
    return (int)len;
  }
  return read_runlist(m, mp, mpe, off, dst, len);
}

/* A decoded index entry; `nspace` is the filename namespace byte. */
struct index_entry {
  uint64_t child;
  int is_dir;
  uint32_t size;
  int nspace;
  char name[NTFS_MAX_NAME + 1];
};

/* Parse the $FILE_NAME at `fn` (bounded by `room`) into `e`. 0 / -1. */
static int parse_filename(const uint8_t *fn, uint32_t room, struct index_entry *e)
{
  if (room < 0x42)
    return -1;
  uint32_t flags = load_le32(fn + 0x38);
  uint8_t nlen = fn[0x40];
  uint8_t nspace = fn[0x41];
  if (0x42u + (uint32_t)nlen * 2u > room)
    return -1;
  e->is_dir = (flags & FNAME_DIR_FLAG) ? 1 : 0;
  e->nspace = nspace;
  utf16_to_ascii(fn + 0x42, nlen, e->name, sizeof e->name);
  return 0;
}

/* Walk index entries in [entries, end), calling cb on each leaf; nonzero stops the walk (set in *stop). 0 / -1. */
typedef int (*entry_cb)(void *ctx, const struct index_entry *e);

static int walk_index_entries(const uint8_t *entries, const uint8_t *end, entry_cb cb, void *ctx, int *stopped)
{
  const uint8_t *p = entries;
  int guard = 0;
  while (p + 0x10 <= end) {
    if (++guard > 100000)
      return -1;
    uint16_t entry_len = load_le16(p + 0x08);
    uint16_t fn_len = load_le16(p + 0x0A); /* content (FILE_NAME) len */
    uint8_t iflags = p[0x0C];
    if (entry_len < 0x10 || p + entry_len > end)
      return -1;

    if (!(iflags & INDEX_ENTRY_END)) {
      uint64_t ref = load_le64(p) & 0x0000FFFFFFFFFFFFull;
      /* the $FILE_NAME content sits right after the 16-byte header */
      if (0x10u + (uint32_t)fn_len <= entry_len) {
        struct index_entry e;
        e.child = ref;
        e.size = 0;
        if (parse_filename(p + 0x10, fn_len, &e) == 0) {
          if (cb(ctx, &e)) {
            if (stopped)
              *stopped = 1;
            return 0;
          }
        }
      }
    }
    if (iflags & INDEX_ENTRY_END)
      break;
    p += entry_len;
  }
  return 0;
}

/* Run `cb` over every entry of directory `rec` ($INDEX_ROOT, then INDX blocks via ixbuf). 0/-1; *stopped if cb stopped.
 */
static int for_each_dir_entry(struct ntfs_mount *m, const uint8_t *rec, entry_cb cb, void *ctx, int *stopped)
{
  if (stopped)
    *stopped = 0;
  uint32_t used = load_le32(rec + 0x18);
  if (used > m->mft_rec_size)
    used = m->mft_rec_size;
  const uint8_t *ir = find_attr(rec, m->mft_rec_size, ATTR_INDEX_ROOT);
  if (!ir)
    return -1;
  const uint8_t *irval;
  uint32_t irlen;
  if (resident_value(ir, m->mft_rec_size, &irval, &irlen) != 0)
    return -1;
  /* INDEX_ROOT: 0x10 header, then an INDEX_HEADER; entries_off is relative to it (irval+0x10). */
  if (irlen < 0x20)
    return -1;
  uint32_t blk = load_le32(irval + 0x08); /* INDX block size; usually 4 KiB whatever the cluster size */
  uint32_t ih_off = 0x10;
  uint32_t entries_off = load_le32(irval + ih_off + 0x00);
  uint32_t total_used = load_le32(irval + ih_off + 0x04);
  const uint8_t *estart = irval + ih_off + entries_off;
  const uint8_t *eend = irval + ih_off + total_used;
  if (estart < irval || eend > irval + irlen || estart > eend)
    return -1;
  if (walk_index_entries(estart, eend, cb, ctx, stopped) != 0)
    return -1;
  if (stopped && *stopped)
    return 0;
  const uint8_t *ia = find_attr(rec, m->mft_rec_size, ATTR_INDEX_ALLOC);
  if (!ia)
    return 0;
  uint32_t alen = load_le32(ia + 4);
  if (alen < 0x40) /* non-resident header must fit */
    return 0;
  if (ia[8] == 0)
    return 0;
  uint16_t mp_off = load_le16(ia + 0x20);
  uint64_t alloc_sz = load_le64(ia + 0x28); /* allocated size of the attr */
  if (mp_off < 0x40 || mp_off >= alen)
    return 0;
  const uint8_t *mp = ia + mp_off, *mp_end = ia + alen;

  if (blk < 512 || blk > NTFS_MAX_INDX || (blk & (blk - 1)))
    return 0;

  uint64_t pos = 0;
  int guard = 0;
  while (pos + blk <= alloc_sz) {
    if (++guard > 100000)
      return -1;
    int got = read_runlist(m, mp, mp_end, pos, m->ixbuf, blk);
    if (got <= 0)
      break;
    pos += blk;

    if (m->ixbuf[0] != 'I' || m->ixbuf[1] != 'N' || m->ixbuf[2] != 'D' || m->ixbuf[3] != 'X')
      continue;
    uint16_t usa_off = load_le16(m->ixbuf + 0x04);
    uint16_t usa_cnt = load_le16(m->ixbuf + 0x06);
    if (apply_fixup(m->ixbuf, blk, usa_off, usa_cnt) != 0)
      continue;
    /* INDX: 0x18 header then an INDEX_HEADER at 0x18. */
    uint32_t ih = 0x18;
    if (ih + 8u > blk)
      continue;
    uint32_t entries_off2 = load_le32(m->ixbuf + ih + 0x00);
    uint32_t used2 = load_le32(m->ixbuf + ih + 0x04);
    const uint8_t *s = m->ixbuf + ih + entries_off2;
    const uint8_t *e = m->ixbuf + ih + used2;
    if (s < m->ixbuf || e > m->ixbuf + blk || s > e)
      continue;
    if (walk_index_entries(s, e, cb, ctx, stopped) != 0)
      continue;
    if (stopped && *stopped)
      return 0;
  }
  return 0;
}

/* Context for the lookup-by-name callback. */
struct lookup_ctx {
  const char *name; /* component to match (case-insensitive)         */
  uint32_t nlen;
  uint64_t found; /* matched child recno                           */
  int is_dir;
  bool have;    /* set once a match (preferring Win32 namespace) */
  int best_dos; /* the current match was a DOS-namespace name    */
};

static int lookup_cb(void *vctx, const struct index_entry *e)
{
  struct lookup_ctx *c = vctx;
  uint32_t enl = strlen(e->name);
  if (enl != c->nlen)
    return 0;
  if (!k_eq_ci_n(e->name, c->name, c->nlen) || c->name[c->nlen] != '\0')
    return 0;
  int is_dos = (e->nspace == 2);
  /* Prefer a non-DOS (Win32/POSIX) name when both exist. */
  if (c->have && c->best_dos == 0 && is_dos)
    return 0;
  c->found = e->child;
  c->is_dir = e->is_dir;
  c->have = true;
  c->best_dos = is_dos;
  /* Keep scanning only if we matched on a DOS name (might find a Win32 one). */
  return is_dos ? 0 : 1;
}

/* Look up `name` in directory `rec`: 1 hit (child, is_dir set), 0 absent, -1 error. */
static int dir_lookup(struct ntfs_mount *m, const uint8_t *rec, const char *name, uint32_t nlen, uint64_t *child,
                      int *is_dir)
{
  struct lookup_ctx c;
  c.name = name;
  c.nlen = nlen;
  c.found = 0;
  c.is_dir = 0;
  c.have = false;
  c.best_dos = 0;
  int stopped = 0;
  if (for_each_dir_entry(m, rec, lookup_cb, &c, &stopped) != 0)
    return -1;
  if (!c.have)
    return 0;
  if (child)
    *child = c.found;
  if (is_dir)
    *is_dir = c.is_dir;
  return 1;
}

/* Resolve `path` to an MFT record in `out` (>= NTFS_MAX_REC), with is_dir and size. 0 / -1. */
static int resolve(struct ntfs_mount *m, const char *path, uint8_t *out, int *is_dir_out, uint64_t *size_out)
{
  if (read_mft_record(m, MFT_REC_ROOT, out) != 0)
    return -1;
  int cur_is_dir = 1;

  if (path) {
    while (*path == '/')
      path++;
    while (*path) {
      const char *comp = path;
      uint32_t len = 0;
      while (path[len] && path[len] != '/')
        len++;
      char name[NTFS_MAX_NAME + 1];
      uint32_t k = 0;
      for (; k < len && k < NTFS_MAX_NAME; k++)
        name[k] = comp[k];
      name[k] = '\0';
      path += len;
      while (*path == '/')
        path++;

      if (k == 0 || (k == 1 && name[0] == '.'))
        continue;
      if (!cur_is_dir)
        return -1;

      uint64_t child;
      int cd = 0;
      int r = dir_lookup(m, out, name, k, &child, &cd);
      if (r <= 0)
        return -1;
      if (read_mft_record(m, child, out) != 0)
        return -1;
      cur_is_dir = cd;
    }
  }

  if (is_dir_out)
    *is_dir_out = cur_is_dir;
  if (size_out)
    *size_out = cur_is_dir ? 0 : data_size(m, out);
  return 0;
}

/* Decode $MFT's $DATA runlist from record 0 (`mft_rec`, already fixed up). 0 / -1. */
static int load_mft_runlist(struct ntfs_mount *m, const uint8_t *mft_rec)
{
  const uint8_t *a = find_attr(mft_rec, m->mft_rec_size, ATTR_DATA);
  if (!a || a[8] == 0) /* $MFT $DATA must be non-resident */
    return -1;
  uint32_t alen = load_le32(a + 4);
  if (alen < 0x40) /* the non-resident header must fit before the runlist offset is read */
    return -1;
  uint16_t mp_off = load_le16(a + 0x20);
  if (mp_off < 0x40 || mp_off >= alen)
    return -1;

  const uint8_t *p = a + mp_off, *end = a + alen;
  uint64_t vcn = 0;
  int64_t lcn = 0;
  uint32_t n = 0;
  int guard = 0;
  while (p < end && *p != 0x00) {
    if (++guard > 4096)
      return -1;
    if (n >= 64)
      break;
    int is_hole = ((p[0] >> 4) & 0x0F) == 0;
    uint64_t rlen;
    int64_t delta;
    const uint8_t *nx = decode_run(p, end, &rlen, &delta);
    if (!nx)
      return -1;
    lcn += delta;
    m->mft_run_vcn[n] = vcn;
    m->mft_run_len[n] = rlen;
    m->mft_run_lcn[n] = is_hole ? -1 : lcn;
    n++;
    vcn += rlen;
    p = nx;
  }
  if (n == 0)
    return -1;
  m->mft_run_n = n;
  return 0;
}

/* Parse the volume label from $Volume (MFT record 3, $VOLUME_NAME attr). */
static void load_label(struct ntfs_mount *m)
{
  m->label[0] = '\0';
  if (read_mft_record(m, MFT_REC_VOLUME, m->rec) != 0)
    return;
  const uint8_t *a = find_attr(m->rec, m->mft_rec_size, ATTR_VOLUME_NAME);
  if (!a)
    return;
  const uint8_t *v;
  uint32_t vlen;
  if (resident_value(a, m->mft_rec_size, &v, &vlen) != 0)
    return;
  utf16_to_ascii(v, vlen / 2, m->label, sizeof m->label);
}

/* Validate the boot sector and fill the instance. Returns 0 / -1. */
static int parse_boot(struct ntfs_mount *m, const uint8_t *bs)
{
  if (bs[3] != 'N' || bs[4] != 'T' || bs[5] != 'F' || bs[6] != 'S' || bs[7] != ' ' || bs[8] != ' ' || bs[9] != ' ' ||
      bs[10] != ' ')
    return -1;

  uint32_t bps = load_le16(bs + 0x0B);
  if (bps < 256 || bps > 4096 || (bps & (bps - 1)))
    return -1;

  uint32_t spc;
  uint8_t spc_raw = bs[0x0D];
  if (spc_raw == 0)
    return -1;
  if (spc_raw <= 0x80) {
    spc = spc_raw;
    if (spc & (spc - 1))
      return -1;
  } else {
    uint32_t shift = 256u - spc_raw;
    if (shift > 24)
      return -1;
    spc = 1u << shift;
  }

  uint64_t bpc = (uint64_t)bps * spc;
  if (bpc == 0 || bpc > NTFS_MAX_CLUSTER)
    return -1;

  uint64_t mft_lcn = load_le64(bs + 0x30);
  if (mft_lcn >> 32) /* assume <32 bits */
    return -1;

  int8_t clpr = (int8_t)bs[0x40]; /* clusters/MFT rec */
  uint32_t recsz;
  if (clpr > 0) {
    recsz = (uint32_t)clpr * (uint32_t)bpc;
  } else {
    uint32_t shift = (uint32_t)(-clpr);
    if (shift > 20)
      return -1;
    recsz = 1u << shift;
  }
  if (recsz < 512 || recsz > NTFS_MAX_REC || (recsz & (recsz - 1)))
    return -1;

  m->bytes_per_sector = bps;
  m->sectors_per_cluster = spc;
  m->bytes_per_cluster = (uint32_t)bpc;
  m->mft_lcn = (uint32_t)mft_lcn;
  m->mft_rec_size = recsz;
  m->mft_run_n = 0;
  return 0;
}

static void *ntfs_probe(blk_read_fn read, blk_write_fn write)
{
  (void)write;
  if (!read)
    return NULL;

  uint8_t bs[512];
  if (read(0, bs) != 0)
    return NULL;

  struct ntfs_mount *m = kmalloc(sizeof *m);
  if (!m)
    return NULL;
  m->read = read;

  if (parse_boot(m, bs) != 0) {
    kfree(m);
    return NULL;
  }

  /* Read $MFT record 0 from the boot-sector LCN, then decode its runlist for the rest. */
  uint64_t mft0_off = (uint64_t)m->mft_lcn * m->bytes_per_cluster;
  if (read_mft_record_at(m, mft0_off, m->rec) != 0) {
    kfree(m);
    return NULL;
  }
  if (load_mft_runlist(m, m->rec) != 0) {
    kfree(m);
    return NULL;
  }

  /* Sanity: the root directory record must read and carry an $INDEX_ROOT. */
  if (read_mft_record(m, MFT_REC_ROOT, m->rec) != 0 || !find_attr(m->rec, m->mft_rec_size, ATTR_INDEX_ROOT)) {
    kfree(m);
    return NULL;
  }

  load_label(m);
  return m;
}

static void ntfs_close(void *inst) { kfree(inst); }

static const char *op_label(void *inst)
{
  struct ntfs_mount *m = inst;
  return m->label;
}

static const char *op_type(void *inst)
{
  (void)inst;
  return "NTFS";
}

/* The VFS sizes are 32-bit: report a larger file as 4 GiB - 1, not its size modulo 4 GiB. */
static uint32_t clamp32(uint64_t v) { return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)v; }

static int op_stat(void *inst, const char *path, int *is_dir, uint32_t *size)
{
  struct ntfs_mount *m = inst;
  int d;
  uint64_t sz;
  if (resolve(m, path, m->rec, &d, &sz) != 0)
    return -1;
  if (is_dir)
    *is_dir = d;
  if (size)
    *size = clamp32(sz);
  return 0;
}

static int op_read_at(void *inst, const char *path, uint32_t off, void *buf, uint32_t len)
{
  struct ntfs_mount *m = inst;
  int d;
  uint64_t sz;
  if (resolve(m, path, m->dir_rec, &d, &sz) != 0 || d)
    return -1;
  return read_data(m, m->dir_rec, off, buf, len);
}

/* enum_dir and list share one walk (index or print mode); skips ".", DOS 8.3 aliases and system metafiles. */
struct list_ctx {
  int want_idx; /* >=0 = the index to return; -1 = print mode    */
  int seen;
  char *name;
  int *is_dir_out;
  uint32_t *size_out;
  bool hit; /* set when want_idx matched                     */
  struct ntfs_mount *m;
};

static int list_cb(void *vctx, const struct index_entry *e)
{
  struct list_ctx *c = vctx;
  if (e->name[0] == '\0')
    return 0;
  if (e->name[0] == '.' && e->name[1] == '\0')
    return 0;
  /* a namespace-2 entry is the short alias of a long name already listed */
  if (e->nspace == 2)
    return 0;
  if (e->name[0] == '$' && e->child < MFT_FIRST_USER)
    return 0;

  uint32_t fsz = 0;
  int isd = e->is_dir;
  if (!isd) {
    /* fetch the real $DATA size from the child record */
    if (read_mft_record(c->m, e->child, c->m->rec) == 0)
      fsz = clamp32(data_size(c->m, c->m->rec));
  }

  if (c->want_idx < 0) {
    con_write(isd ? "  <DIR>  " : "         ");
    if (!isd) {
      con_printf("%u  ", fsz);
    } else {
      con_write("  ");
    }
    con_printf("%s\n", e->name);
    return 0;
  }
  if (c->seen == c->want_idx) {
    fs_enum_name(c->name, e->name, sizeof e->name);
    if (c->is_dir_out)
      *c->is_dir_out = isd;
    if (c->size_out)
      *c->size_out = fsz;
    c->hit = true;
    return 1;
  }
  c->seen++;
  return 0;
}

static int op_list(void *inst, const char *path)
{
  struct ntfs_mount *m = inst;
  int d;
  uint64_t sz;
  if (resolve(m, path, m->dir_rec, &d, &sz) != 0 || !d)
    return -1;
  struct list_ctx c;
  c.want_idx = -1;
  c.seen = 0;
  c.name = NULL;
  c.is_dir_out = NULL;
  c.size_out = NULL;
  c.hit = false;
  c.m = m;
  int stopped = 0;
  if (for_each_dir_entry(m, m->dir_rec, list_cb, &c, &stopped) != 0)
    return -1;
  return 0;
}

static int op_enum_dir(void *inst, const char *path, int idx, char *name, int *is_dir, uint32_t *size)
{
  struct ntfs_mount *m = inst;
  if (idx < 0)
    return 0;
  int d;
  uint64_t sz;
  if (resolve(m, path, m->dir_rec, &d, &sz) != 0 || !d)
    return 0;
  struct list_ctx c;
  c.want_idx = idx;
  c.seen = 0;
  c.name = name;
  c.is_dir_out = is_dir;
  c.size_out = size;
  c.hit = false;
  c.m = m;
  int stopped = 0;
  if (for_each_dir_entry(m, m->dir_rec, list_cb, &c, &stopped) != 0)
    return 0;
  return c.hit ? 1 : 0;
}

const struct fs_ops *ntfs_ops(void)
{
  static const struct fs_ops ops = {
      .fsname = "NTFS",
      .probe = ntfs_probe,
      .close = ntfs_close,
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
