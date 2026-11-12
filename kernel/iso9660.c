/* iso9660.c: read-only ISO 9660 / Joliet driver (see iso9660.h). Little-endian copy of both-endian fields. */

#include <stddef.h>

#include "console.h"
#include "endian.h"
#include "heap.h"
#include "iso9660.h"
#include "kstring.h"

#define ISO_SECTOR 2048u
#define VD_START 16u
#define VD_LIMIT 64u
#define VDT_PRIMARY 1
#define VDT_SUPPLEM 2
#define VDT_TERM 255

#define FLAG_DIR 0x02

struct dirrec {
  uint32_t lba, size;
  int is_dir;
};

struct iso_mount {
  blk_read_fn rd;
  uint32_t root_lba, root_size;
  bool joliet;
  char vol_label[33];
  char vol_type[20];

  uint8_t dbuf[ISO_SECTOR];
  uint8_t rbuf[ISO_SECTOR];
};

/* Decode an identifier: drop ";version" and a bare trailing '.', fold Joliet UCS-2BE to bytes. dst >= 256. */
static void decode_name(struct iso_mount *m, char *dst, const uint8_t *id, int len)
{
  int n = 0;
  if (m->joliet) {
    for (int i = 0; i + 1 < len && n < 254; i += 2)
      dst[n++] = (char)id[i + 1];
  } else {
    for (int i = 0; i < len && n < 254; i++)
      dst[n++] = (char)id[i];
  }
  dst[n] = '\0';
  for (int i = 0; i < n; i++)
    if (dst[i] == ';') {
      dst[i] = '\0';
      n = i;
      break;
    }
  if (n > 0 && dst[n - 1] == '.')
    dst[--n] = '\0';
}

struct rec_iter {
  uint32_t base_lba, size, pos;
  uint32_t cur_sec;
  bool loaded;
};

static void ri_init(struct rec_iter *it, uint32_t lba, uint32_t size)
{
  it->base_lba = lba;
  it->size = size;
  it->pos = 0;
  it->loaded = false;
}

/* Next directory record into out/name (>=256 bytes): 1, or 0 at end. A zero length skips to the next sector. */
static bool ri_next(struct iso_mount *m, struct rec_iter *it, char *name, struct dirrec *out)
{
  while (it->pos < it->size) {
    uint32_t sec = it->pos / ISO_SECTOR;
    uint32_t off = it->pos % ISO_SECTOR;

    if (!it->loaded || it->cur_sec != sec) {
      if (m->rd(it->base_lba + sec, m->dbuf) != 0)
        return false;
      it->cur_sec = sec;
      it->loaded = true;
    }

    uint8_t reclen = m->dbuf[off];
    if (reclen == 0) {
      it->pos = (sec + 1) * ISO_SECTOR;
      continue;
    }
    if (off + reclen > ISO_SECTOR) {
      it->pos = (sec + 1) * ISO_SECTOR;
      continue;
    }
    if (reclen < 34) {
      it->pos += reclen;
      continue;
    }

    const uint8_t *p = m->dbuf + off;
    it->pos += reclen;

    out->lba = load_le32(p + 2);
    out->size = load_le32(p + 10);
    out->is_dir = (p[25] & FLAG_DIR) ? 1 : 0;

    /* Clamp the untrusted identifier length to the record. */
    uint32_t fil = p[32];
    if (33 + fil > reclen)
      fil = reclen - 33;
    if (fil == 1 && p[33] == 0x00) {
      name[0] = '.';
      name[1] = '\0';
    } else if (fil == 1 && p[33] == 0x01) {
      name[0] = '.';
      name[1] = '.';
      name[2] = '\0';
    } else {
      decode_name(m, name, p + 33, (int)fil);
    }
    return true;
  }
  return false;
}

/* Find a named child in directory (lba,size). 1 + *out on hit, else 0. */
static bool dir_find(struct iso_mount *m, uint32_t lba, uint32_t size, const char *want, struct dirrec *out)
{
  struct rec_iter it;
  char name[256];
  struct dirrec r;
  ri_init(&it, lba, size);
  while (ri_next(m, &it, name, &r)) {
    if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
      continue;
    if (k_streq_ci(name, want)) {
      *out = r;
      return true;
    }
  }
  return false;
}

/* Resolve a media subpath ("" or "/a/b/c") to a directory/file record. */
static int resolve(struct iso_mount *m, const char *path, struct dirrec *out)
{
  struct dirrec cur = {m->root_lba, m->root_size, 1};

  if (path)
    while (*path == '/')
      path++;

  while (path && *path) {
    if (!cur.is_dir)
      return -1;
    char comp[256];
    int n = 0;
    while (*path && *path != '/' && n < 255)
      comp[n++] = *path++;
    comp[n] = '\0';
    while (*path == '/')
      path++;
    if (n == 0)
      continue;
    struct dirrec child;
    if (!dir_find(m, cur.lba, cur.size, comp, &child))
      return -1;
    cur = child;
  }
  *out = cur;
  return 0;
}

static void load_label(struct iso_mount *m, const uint8_t *vd)
{
  /* Volume Identifier: 32 bytes at offset 40 (Joliet: UCS-2BE, 16 chars). */
  int n = 0;
  if (m->joliet) {
    for (int i = 0; i < 32 && n < 32; i += 2)
      m->vol_label[n++] = (char)vd[40 + i + 1];
  } else {
    for (int i = 0; i < 32 && n < 32; i++)
      m->vol_label[n++] = (char)vd[40 + i];
  }
  while (n > 0 && (m->vol_label[n - 1] == ' ' || m->vol_label[n - 1] == '\0'))
    n--;
  m->vol_label[n] = '\0';
}

/* Joliet if the escape sequences (32 bytes at offset 88) start with %/@, %/C or %/E. */
static bool is_joliet_svd(const uint8_t *vd)
{
  const uint8_t *e = vd + 88;
  return e[0] == 0x25 && e[1] == 0x2F && (e[2] == 0x40 || e[2] == 0x43 || e[2] == 0x45);
}

/* Probe the volume descriptors into a fresh instance: 0, or -1 (no "CD001", I/O error, no medium). */
static int mount_probe(struct iso_mount *m, blk_read_fn read)
{
  m->joliet = false;
  m->rd = read;
  m->vol_label[0] = '\0';
  m->vol_type[0] = '\0';

  bool have_primary = false;
  uint32_t pri_lba = 0, pri_size = 0;
  char pri_label[33] = {0};

  for (uint32_t s = VD_START; s < VD_LIMIT; s++) {
    if (m->rd(s, m->rbuf) != 0)
      break;
    if (m->rbuf[1] != 'C' || m->rbuf[2] != 'D' || m->rbuf[3] != '0' || m->rbuf[4] != '0' || m->rbuf[5] != '1')
      break;

    uint8_t type = m->rbuf[0];
    if (type == VDT_TERM)
      break;

    if (type == VDT_PRIMARY) {
      /* Root directory record sits at offset 156. */
      pri_lba = load_le32(m->rbuf + 156 + 2);
      pri_size = load_le32(m->rbuf + 156 + 10);
      m->joliet = false;
      load_label(m, m->rbuf);
      for (int i = 0; (pri_label[i] = m->vol_label[i]); i++)
        ;
      have_primary = true;
    } else if (type == VDT_SUPPLEM && is_joliet_svd(m->rbuf)) {
      /* Prefer Joliet: long, mixed-case names. */
      m->root_lba = load_le32(m->rbuf + 156 + 2);
      m->root_size = load_le32(m->rbuf + 156 + 10);
      m->joliet = true;
      load_label(m, m->rbuf);
      const char *t = "ISO9660+Joliet";
      int i = 0;
      for (; t[i]; i++)
        m->vol_type[i] = t[i];
      m->vol_type[i] = '\0';
      return 0;
    }
  }

  if (!have_primary)
    return -1;

  m->root_lba = pri_lba;
  m->root_size = pri_size;
  m->joliet = false;
  for (int i = 0; (m->vol_label[i] = pri_label[i]); i++)
    ;
  const char *t = "ISO9660";
  int i = 0;
  for (; t[i]; i++)
    m->vol_type[i] = t[i];
  m->vol_type[i] = '\0';
  return 0;
}

static const char *iso_label(struct iso_mount *m) { return m->vol_label; }
static const char *iso_type(struct iso_mount *m) { return m->vol_type; }

static int iso_list(struct iso_mount *m, const char *path)
{
  struct dirrec d;
  if (resolve(m, path, &d) != 0)
    return -1;
  if (!d.is_dir)
    return -1;

  struct rec_iter it;
  char name[256];
  struct dirrec r;
  ri_init(&it, d.lba, d.size);
  while (ri_next(m, &it, name, &r)) {
    if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
      continue;
    con_write(r.is_dir ? "  <DIR>  " : "         ");
    if (!r.is_dir)
      con_write_dec(r.size);
    con_printf("  %s\n", name);
  }
  return 0;
}

static int iso_stat(struct iso_mount *m, const char *path, int *is_dir, uint32_t *size)
{
  struct dirrec d;
  if (resolve(m, path, &d) != 0)
    return -1;
  if (is_dir)
    *is_dir = d.is_dir;
  if (size)
    *size = d.is_dir ? 0 : d.size;
  return 0;
}

static int iso_read_at(struct iso_mount *m, const char *path, uint32_t offset, void *buf, uint32_t len)
{
  struct dirrec d;
  if (resolve(m, path, &d) != 0 || d.is_dir)
    return -1;
  if (offset >= d.size)
    return 0;
  if (offset + len > d.size)
    len = d.size - offset;

  uint8_t *out = buf;
  uint32_t done = 0;
  while (done < len) {
    uint32_t pos = offset + done;
    uint32_t sec = pos / ISO_SECTOR;
    uint32_t so = pos % ISO_SECTOR;
    if (m->rd(d.lba + sec, m->rbuf) != 0)
      return done ? (int)done : -1;
    uint32_t chunk = ISO_SECTOR - so;
    if (chunk > len - done)
      chunk = len - done;
    memcpy(out + done, m->rbuf + so, chunk);
    done += chunk;
  }
  return (int)done;
}

static int iso_enum_dir(struct iso_mount *m, const char *path, int idx, char *name, int *is_dir, uint32_t *size)
{
  struct dirrec d;
  if (resolve(m, path, &d) != 0 || !d.is_dir)
    return 0;

  struct rec_iter it;
  char nm[256];
  struct dirrec r;
  int seen = 0;
  ri_init(&it, d.lba, d.size);
  while (ri_next(m, &it, nm, &r)) {
    if (nm[0] == '.' && (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0')))
      continue;
    if (seen == idx) {
      fs_enum_name(name, nm, 255);
      if (is_dir)
        *is_dir = r.is_dir;
      if (size)
        *size = r.is_dir ? 0 : r.size;
      return 1;
    }
    seen++;
  }
  return 0;
}

static void *iso_probe(blk_read_fn read, blk_write_fn write)
{
  (void)write;
  struct iso_mount *m = kmalloc(sizeof(*m));
  if (!m)
    return NULL;
  if (mount_probe(m, read) != 0) {
    kfree(m);
    return NULL;
  }
  return m;
}

static void iso_close(void *inst) { kfree(inst); }

static const char *ops_label(void *inst) { return iso_label((struct iso_mount *)inst); }
static const char *ops_type(void *inst) { return iso_type((struct iso_mount *)inst); }

static int ops_list(void *inst, const char *path) { return iso_list((struct iso_mount *)inst, path); }

static int ops_stat(void *inst, const char *path, int *is_dir, uint32_t *size)
{
  return iso_stat((struct iso_mount *)inst, path, is_dir, size);
}

static int ops_read_at(void *inst, const char *path, uint32_t off, void *buf, uint32_t len)
{
  return iso_read_at((struct iso_mount *)inst, path, off, buf, len);
}

static int ops_enum_dir(void *inst, const char *path, int idx, char *name, int *is_dir, uint32_t *size)
{
  return iso_enum_dir((struct iso_mount *)inst, path, idx, name, is_dir, size);
}

const struct fs_ops *iso_ops(void)
{
  static const struct fs_ops ops = {
      .fsname = "ISO9660",
      .probe = iso_probe,
      .close = iso_close,
      .label = ops_label,
      .type = ops_type,
      .list = ops_list,
      .stat = ops_stat,
      .read_at = ops_read_at,
      .enum_dir = ops_enum_dir,
      .create = NULL,
      .del = NULL,
      .mkdir = NULL,
      .rmdir = NULL,
  };
  return &ops;
}
