/* fat.c - read-only-capable FAT12/16/32 driver (see fat.h, fsmount.h).
 *
 * Multi-instance: all mutable per-mount state lives in a heap-allocated
 * `struct fat_mount`, so N independent FAT volumes can be mounted at once.
 * Every operation takes the instance through a `void *inst` (a `struct
 * fat_mount *`). Everything is derived from the BIOS Parameter Block in the
 * boot sector: the reserved region, one or more FATs, a root-directory region
 * (FAT12/16) or root cluster (FAT32), and the cluster-addressed data region.
 * The FAT type is decided the canonical way -- by the count of data clusters --
 * not by the (advisory) string in the BPB.
 *
 * Sector I/O goes through a one-sector LRU-of-1 cache (per mount). Walking a
 * cluster chain re-reads the same FAT sector for every link, and listing a
 * directory re-reads its sectors; caching the last sector turns those into hits
 * and keeps the polled AHCI/NVMe back ends from thrashing.
 */

#include <stddef.h>

#include "console.h"
#include "endian.h"
#include "fat.h"
#include "heap.h"
#include "kstring.h"
#include "wallclock.h"

#define SEC 512u
#define CLINES 16
#define IOERR 0xFFFFFFFFu
#define BADCLUS 0xFFFFFFFEu /* directory cluster that must not be followed */
#define MAX_LFN 255
/* A resolved directory entry. The loc[] arrays record where the raw on-disk
 * records live -- the run of LFN slots followed by the 8.3 entry -- so a delete
 * can wipe every record that makes up the name, not just the 8.3 one. */
#define MAX_RECS 21 /* up to 20 LFN slots + 1 short entry */
#define DIR_MAX_ENTRIES 65536u
#define UNKNOWN 0xFFFFFFFFu

/* ----------------------------------------------------------------- mounting */

enum fat_kind { FAT_NONE, FAT12, FAT16, FAT32 };

struct loc {
  uint32_t sec; /* volume-relative 512-byte sector */
  uint16_t off;
};

/* One logical directory entry: LFN slots folded into the 8.3 record; loc[] ends with it. */
struct dirent {
  uint16_t name[MAX_LFN + 5]; /* UTF-16 display name */
  uint32_t nlen;
  uint8_t sfn[11];
  uint8_t attr, ntres, ctenth;
  uint16_t ctime, cdate, adate, mtime, mdate;
  uint32_t cluster; /* first cluster (0 = empty file)                  */
  uint32_t size;    /* bytes (0 for directories)                       */
  bool is_dir;
  struct loc loc[MAX_RECS];
  int loc_n;
};

struct cline {
  uint32_t lba;
  bool valid, dirty;
  uint8_t data[SEC];
};

/* Per-mount state. Heap-allocated in fat_probe, freed in fat_close. Holds every
 * field that used to be a mount-specific file-static global, including the
 * one-sector read cache, so concurrent mounts never clobber each other. */
struct fat_mount {
  blk_read_fn read;
  blk_write_fn write; /* NULL => read-only mount                      */
  uint32_t base;      /* absolute LBA of the boot sector              */
  enum fat_kind kind;
  uint32_t bps;
  uint32_t csec;   /* sectors per cluster */
  uint32_t cbytes; /* bytes per cluster */
  uint32_t fat_start, fat_secs, num_fats, fat_active;
  bool single_fat; /* FAT32 with mirroring disabled: only fat_active is updated */
  uint32_t root_start, root_secs, root_entries;
  uint32_t data_start;
  uint32_t root_cluster; /* FAT32 root cluster (0 on FAT12/16)           */
  uint32_t nclus;        /* data clusters; valid numbers are 2..nclus+1 */
  uint32_t fsi_sec;
  uint32_t free_count, next_free;
  bool free_known, fsi_dirty;
  uint32_t res_off; /* boot-sector offset of the dirty-flag byte */
  bool was_clean, flagged, io_err;
  char label[12];

  uint32_t hint_first, hint_idx, hint_clus;

  char *rc_path;
  struct dirent rc_ent;

  struct cline lines[CLINES];
};

static inline void put_le16(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static inline void put_le32(uint8_t *p, uint32_t v)
{
  put_le16(p, v);
  put_le16(p + 2, v >> 16);
}

/* ------------------------------------------------------------------ text */

static const uint16_t cp437[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE,
    0x00EC, 0x00C4, 0x00C5, 0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6,
    0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192, 0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA,
    0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB, 0x2591, 0x2592, 0x2593, 0x2502,
    0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510, 0x2514,
    0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550,
    0x256C, 0x2567, 0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C,
    0x2588, 0x2584, 0x258C, 0x2590, 0x2580, 0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229, 0x2261, 0x00B1, 0x2265, 0x2264, 0x2320,
    0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0};

/* Simple lowercase folding: ASCII, Latin-1, Latin Extended-A, Greek and Cyrillic basics. */
static uint16_t ufold(uint16_t c)
{
  if (c < 0x80)
    return (c >= 'A' && c <= 'Z') ? (uint16_t)(c + 32) : c;
  if (c >= 0xC0 && c <= 0xDE && c != 0xD7)
    return (uint16_t)(c + 32);
  if (c >= 0x100 && c <= 0x17F) {
    if (c == 0x178)
      return 0xFF;
    if (c == 0x130 || c == 0x131 || c == 0x138 || c == 0x149 || c == 0x17F)
      return c;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E))
      return (c & 1) ? (uint16_t)(c + 1) : c;
    return (c & 1) ? c : (uint16_t)(c + 1);
  }
  if (c >= 0x391 && c <= 0x3AB && c != 0x3A2)
    return (uint16_t)(c + 32);
  if (c == 0x386)
    return 0x3AC;
  if (c >= 0x388 && c <= 0x38A)
    return (uint16_t)(c + 37);
  if (c == 0x38C)
    return 0x3CC;
  if (c == 0x38E || c == 0x38F)
    return (uint16_t)(c + 63);
  if (c == 0x3C2)
    return 0x3C3;
  if (c >= 0x410 && c <= 0x42F)
    return (uint16_t)(c + 32);
  if (c >= 0x400 && c <= 0x40F)
    return (uint16_t)(c + 80);
  if ((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x52F) ||
      (c >= 0x1E00 && c <= 0x1E95) || (c >= 0x1EA0 && c <= 0x1EFF))
    return (c & 1) ? c : (uint16_t)(c + 1);
  if (c >= 0x4C1 && c <= 0x4CE)
    return (c & 1) ? (uint16_t)(c + 1) : c;
  if (c == 0x4C0)
    return 0x4CF;
  return c;
}

static bool ueq(const uint16_t *a, uint32_t an, const uint16_t *b, uint32_t bn)
{
  if (an != bn)
    return false;
  for (uint32_t i = 0; i < an; i++)
    if (a[i] != b[i] && ufold(a[i]) != ufold(b[i]))
      return false;
  return true;
}

/* UTF-8 to UTF-16. Count, -1 for malformed input, -2 if it exceeds cap units. */
static int utf8_to_u16(const char *s, uint32_t len, uint16_t *out, uint32_t cap)
{
  uint32_t o = 0, i = 0;
  while (i < len) {
    uint32_t c = (uint8_t)s[i++], need = 0, min = 0;
    if (c >= 0x80) {
      if ((c & 0xE0) == 0xC0) {
        c &= 0x1F;
        need = 1;
        min = 0x80;
      } else if ((c & 0xF0) == 0xE0) {
        c &= 0x0F;
        need = 2;
        min = 0x800;
      } else if ((c & 0xF8) == 0xF0) {
        c &= 0x07;
        need = 3;
        min = 0x10000;
      } else {
        return -1;
      }
      if (i + need > len)
        return -1;
      for (uint32_t k = 0; k < need; k++) {
        uint32_t b = (uint8_t)s[i++];
        if ((b & 0xC0) != 0x80)
          return -1;
        c = (c << 6) | (b & 0x3F);
      }
      if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF))
        return -1;
    }
    if (c >= 0x10000) {
      if (o + 2 > cap)
        return -2;
      c -= 0x10000;
      out[o++] = (uint16_t)(0xD800 | (c >> 10));
      out[o++] = (uint16_t)(0xDC00 | (c & 0x3FF));
    } else {
      if (o + 1 > cap)
        return -2;
      out[o++] = (uint16_t)c;
    }
  }
  return (int)o;
}

/* UTF-16 to UTF-8 into cap bytes; length or -1. Lone surrogates become U+FFFD. */
static int u16_to_utf8(const uint16_t *u, uint32_t n, char *out, uint32_t cap)
{
  uint32_t o = 0;
  for (uint32_t i = 0; i < n; i++) {
    uint32_t c = u[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && u[i + 1] >= 0xDC00 && u[i + 1] <= 0xDFFF) {
      c = 0x10000 + ((c - 0xD800) << 10) + (u[i + 1] - 0xDC00);
      i++;
    } else if (c >= 0xD800 && c <= 0xDFFF) {
      c = 0xFFFD;
    }
    uint32_t need = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
    if (o + need + 1 > cap)
      return -1;
    if (need == 1) {
      out[o++] = (char)c;
    } else if (need == 2) {
      out[o++] = (char)(0xC0 | (c >> 6));
      out[o++] = (char)(0x80 | (c & 0x3F));
    } else if (need == 3) {
      out[o++] = (char)(0xE0 | (c >> 12));
      out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
      out[o++] = (char)(0x80 | (c & 0x3F));
    } else {
      out[o++] = (char)(0xF0 | (c >> 18));
      out[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
      out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
      out[o++] = (char)(0x80 | (c & 0x3F));
    }
  }
  out[o] = '\0';
  return (int)o;
}

static uint16_t oem_char(uint8_t c, bool lower)
{
  if (c >= 0x80)
    return cp437[c - 0x80];
  if (lower && c >= 'A' && c <= 'Z')
    return (uint16_t)(c + 32);
  return c;
}

/* Decode an 11-byte short name; ntres holds NT lowercase flags (0x08 base, 0x10 ext). */
static uint32_t sfn_decode(const uint8_t *s, uint8_t ntres, uint16_t *out)
{
  uint32_t o = 0, bl = 8, el = 3;
  while (bl > 0 && s[bl - 1] == ' ')
    bl--;
  while (el > 0 && s[8 + el - 1] == ' ')
    el--;
  for (uint32_t i = 0; i < bl; i++)
    out[o++] = oem_char(i == 0 && s[0] == 0x05 ? 0xE5 : s[i], ntres & 0x08);
  if (el) {
    out[o++] = '.';
    for (uint32_t i = 0; i < el; i++)
      out[o++] = oem_char(s[8 + i], ntres & 0x10);
  }
  return o;
}

static bool short_legal(uint16_t c)
{
  if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
    return true;
  for (const char *ok = "!#$%&'()-@^_`{}~"; *ok; ok++)
    if (c == (uint16_t)*ok)
      return true;
  return false;
}

static uint8_t upper8(uint16_t c) { return (uint8_t)((c >= 'a' && c <= 'z') ? c - 32 : c); }

/* LFN rules: 1..255 units, no control/reserved chars, no trailing dot/space, not "."/"..". */
static bool name_valid(const uint16_t *u, uint32_t n)
{
  if (n == 0 || n > MAX_LFN)
    return false;
  for (uint32_t i = 0; i < n; i++) {
    uint16_t c = u[i];
    if (c < 0x20 || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' || c == '>' || c == '?' || c == '\\' ||
        c == '|')
      return false;
  }
  if (u[n - 1] == '.' || u[n - 1] == ' ')
    return false;
  return true;
}

static bool is_dot_name(const uint16_t *u, uint32_t n)
{
  return (n == 1 && u[0] == '.') || (n == 2 && u[0] == '.' && u[1] == '.');
}

/* True for a plain 8.3 name; NT case flags cover an all-lowercase base/extension. */
static bool fit_83(const uint16_t *u, uint32_t n, uint8_t sfn[11], uint8_t *ntres)
{
  uint32_t dot = n;
  for (uint32_t i = 0; i < n; i++) {
    if (u[i] == '.') {
      if (dot != n || i == 0)
        return false;
      dot = i;
    } else if (!short_legal(u[i])) {
      return false;
    }
  }
  uint32_t bl = dot, el = dot == n ? 0 : n - dot - 1;
  if (bl == 0 || bl > 8 || el > 3 || (dot != n && el == 0))
    return false;
  bool blow = false, bup = false, elow = false, eup = false;
  memset(sfn, ' ', 11);
  for (uint32_t i = 0; i < bl; i++) {
    blow |= u[i] >= 'a' && u[i] <= 'z';
    bup |= u[i] >= 'A' && u[i] <= 'Z';
    sfn[i] = upper8(u[i]);
  }
  for (uint32_t i = 0; i < el; i++) {
    uint16_t c = u[dot + 1 + i];
    elow |= c >= 'a' && c <= 'z';
    eup |= c >= 'A' && c <= 'Z';
    sfn[8 + i] = upper8(c);
  }
  if ((blow && bup) || (elow && eup))
    return false;
  *ntres = (uint8_t)((blow ? 0x08 : 0) | (elow ? 0x10 : 0));
  return true;
}

static uint8_t lfn_sum(const uint8_t *sfn)
{
  uint8_t s = 0;
  for (int i = 0; i < 11; i++)
    s = (uint8_t)(((s & 1) << 7) + (s >> 1) + sfn[i]);
  return s;
}

/* ------------------------------------------------------------------ time */

struct fts {
  uint16_t date, time;
  uint8_t tenth;
};

static void fat_now(struct fts *t)
{
  struct timeval64 tv;
  wallclock_get(&tv);
  long long s = tv.tv_sec;
  if (s < 315532800)
    s = 315532800;
  if (s > 4294967295LL)
    s = 4294967295LL;
  uint32_t sec = (uint32_t)s, days = sec / 86400, rem = sec % 86400;
  uint32_t z = days + 719468, era = z / 146097, doe = z - era * 146097;
  uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
  uint32_t d = doy - (153 * mp + 2) / 5 + 1, mo = mp < 10 ? mp + 3 : mp - 9, y = yoe + era * 400 + (mo <= 2);
  t->date = (uint16_t)(((y - 1980) << 9) | (mo << 5) | d);
  t->time = (uint16_t)(((rem / 3600) << 11) | (((rem / 60) % 60) << 5) | ((rem % 60) / 2));
  t->tenth = (uint8_t)((sec & 1) * 100 + (uint32_t)tv.tv_usec / 10000);
}

static uint32_t fat_to_unix(uint16_t date, uint16_t time)
{
  uint32_t y = 1980u + (date >> 9), mo = (date >> 5) & 15, d = date & 31;
  if (mo < 1 || mo > 12 || d < 1)
    return 0;
  uint32_t yy = y - (mo <= 2), era = yy / 400, yoe = yy - era * 400;
  uint32_t doy = (153 * (mo > 2 ? mo - 3 : mo + 9) + 2) / 5 + d - 1;
  uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy, days = era * 146097 + doe - 719468;
  uint64_t v = (uint64_t)days * 86400 + (time >> 11) * 3600u + ((time >> 5) & 63) * 60u + (time & 31) * 2u;
  return v > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)v;
}

/* --------------------------------------------------------- sector cache */

static int cl_flush_line(struct fat_mount *m, struct cline *l)
{
  if (l->valid && l->dirty) {
    if (m->write(l->lba, l->data) != 0) {
      m->io_err = true;
      return -1;
    }
    l->dirty = false;
  }
  return 0;
}

static int cl_flush(struct fat_mount *m)
{
  int rc = 0;
  for (int i = 0; i < CLINES; i++)
    if (cl_flush_line(m, &m->lines[i]) != 0)
      rc = -1;
  return rc;
}

static int cl_load(struct fat_mount *m, uint32_t lba, struct cline **out)
{
  struct cline *l = &m->lines[lba % CLINES];
  if (!(l->valid && l->lba == lba)) {
    if (cl_flush_line(m, l) != 0)
      return -1;
    l->valid = false;
    if (m->read(lba, l->data) != 0)
      return -1;
    l->lba = lba;
    l->valid = true;
    l->dirty = false;
  }
  *out = l;
  return 0;
}

static int meta_read(struct fat_mount *m, uint32_t sec, uint8_t *dst)
{
  struct cline *l;
  if (cl_load(m, m->base + sec, &l) != 0)
    return -1;
  memcpy(dst, l->data, SEC);
  return 0;
}

static int meta_write(struct fat_mount *m, uint32_t sec, const uint8_t *src)
{
  struct cline *l = &m->lines[(m->base + sec) % CLINES];
  if (!(l->valid && l->lba == m->base + sec) && cl_flush_line(m, l) != 0)
    return -1;
  memcpy(l->data, src, SEC);
  l->lba = m->base + sec;
  l->valid = true;
  l->dirty = true;
  return 0;
}

/* Write-through of a file-data sector, keeping any cached copy coherent. */
static int data_write(struct fat_mount *m, uint32_t sec, const uint8_t *src)
{
  uint32_t lba = m->base + sec;
  if (m->write(lba, src) != 0) {
    m->io_err = true;
    return -1;
  }
  struct cline *l = &m->lines[lba % CLINES];
  if (l->valid && l->lba == lba) {
    memcpy(l->data, src, SEC);
    l->dirty = false;
  }
  return 0;
}

static int data_read(struct fat_mount *m, uint32_t sec, uint8_t *dst)
{
  uint32_t lba = m->base + sec;
  struct cline *l = &m->lines[lba % CLINES];
  if (l->valid && l->lba == lba) {
    memcpy(dst, l->data, SEC);
    return 0;
  }
  return m->read(lba, dst);
}

/* ------------------------------------------------------------------- FAT */

static uint32_t clus_sec(const struct fat_mount *m, uint32_t c) { return m->data_start + (c - 2) * m->csec; }

static bool clus_ok(const struct fat_mount *m, uint32_t c) { return c >= 2 && c <= m->nclus + 1; }

static uint32_t root_id(const struct fat_mount *m) { return m->kind == FAT32 ? m->root_cluster : 0; }

static bool is_root(const struct fat_mount *m, uint32_t dir) { return dir == root_id(m); }

static uint32_t fat_sec_of(const struct fat_mount *m, uint32_t copy, uint32_t boff)
{
  return m->fat_start + copy * m->fat_secs + boff / SEC;
}

static uint32_t eoc_value(const struct fat_mount *m)
{
  return m->kind == FAT12 ? 0x0FFFu : m->kind == FAT16 ? 0xFFFFu : 0x0FFFFFFFu;
}

static int fat_byte(struct fat_mount *m, uint32_t boff, uint8_t *v)
{
  struct cline *l;
  if (boff / SEC >= m->fat_secs || cl_load(m, m->base + fat_sec_of(m, m->fat_active, boff), &l) != 0)
    return -1;
  *v = l->data[boff % SEC];
  return 0;
}

/* Raw FAT entry (28 bits on FAT32), or IOERR when out of range or unreadable. */
static uint32_t fat_get(struct fat_mount *m, uint32_t c)
{
  struct cline *l;
  if (c > m->nclus + 1)
    return IOERR;
  if (m->kind == FAT12) {
    uint8_t b0, b1;
    uint32_t off = c + c / 2;
    if (fat_byte(m, off, &b0) != 0 || fat_byte(m, off + 1, &b1) != 0)
      return IOERR;
    uint32_t v = (uint32_t)b0 | ((uint32_t)b1 << 8);
    return (c & 1) ? v >> 4 : v & 0xFFF;
  }
  uint32_t off = c * (m->kind == FAT16 ? 2u : 4u);
  if (off / SEC >= m->fat_secs || cl_load(m, m->base + fat_sec_of(m, m->fat_active, off), &l) != 0)
    return IOERR;
  if (m->kind == FAT16)
    return load_le16(l->data + off % SEC);
  return load_le32(l->data + off % SEC) & 0x0FFFFFFFu;
}

static int fat_put(struct fat_mount *m, uint32_t copy, uint32_t boff, uint8_t mask, uint8_t val)
{
  struct cline *l;
  if (boff / SEC >= m->fat_secs || cl_load(m, m->base + fat_sec_of(m, copy, boff), &l) != 0)
    return -1;
  uint8_t *p = &l->data[boff % SEC];
  *p = (uint8_t)((*p & ~mask) | (val & mask));
  l->dirty = true;
  return 0;
}

static bool fat_copy_used(const struct fat_mount *m, uint32_t f) { return !m->single_fat || f == m->fat_active; }

/* Set a cluster's FAT entry in every FAT copy, tracking the free count. */
static int fat_set(struct fat_mount *m, uint32_t c, uint32_t val)
{
  uint32_t old = fat_get(m, c);
  if (old == IOERR)
    return -1;
  m->hint_first = 0;
  for (uint32_t f = 0; f < m->num_fats; f++) {
    if (!fat_copy_used(m, f))
      continue;
    int rc = 0;
    if (m->kind == FAT16) {
      rc |= fat_put(m, f, c * 2, 0xFF, (uint8_t)val);
      rc |= fat_put(m, f, c * 2 + 1, 0xFF, (uint8_t)(val >> 8));
    } else if (m->kind == FAT32) {
      rc |= fat_put(m, f, c * 4, 0xFF, (uint8_t)val);
      rc |= fat_put(m, f, c * 4 + 1, 0xFF, (uint8_t)(val >> 8));
      rc |= fat_put(m, f, c * 4 + 2, 0xFF, (uint8_t)(val >> 16));
      rc |= fat_put(m, f, c * 4 + 3, 0x0F, (uint8_t)(val >> 24));
    } else {
      uint32_t off = c + c / 2;
      if (c & 1) {
        rc |= fat_put(m, f, off, 0xF0, (uint8_t)(val << 4));
        rc |= fat_put(m, f, off + 1, 0xFF, (uint8_t)(val >> 4));
      } else {
        rc |= fat_put(m, f, off, 0xFF, (uint8_t)val);
        rc |= fat_put(m, f, off + 1, 0x0F, (uint8_t)(val >> 8));
      }
    }
    if (rc)
      return -1;
  }
  if (m->free_known) {
    if (old == 0 && val != 0) {
      if (m->free_count)
        m->free_count--;
      else
        m->free_known = false; /* the FSInfo hint understated free space: recount when next needed */
    } else if (old != 0 && val == 0) {
      m->free_count++;
    }
  }
  m->fsi_dirty = true;
  return 0;
}

enum { CH_NEXT, CH_END, CH_BAD };

/* Classify c's FAT entry: next cluster, end marker, or corrupt (free/reserved/bad/I/O). */
static int chain_next(struct fat_mount *m, uint32_t c, uint32_t *next)
{
  uint32_t v = fat_get(m, c);
  uint32_t eoc = m->kind == FAT12 ? 0x0FF8u : m->kind == FAT16 ? 0xFFF8u : 0x0FFFFFF8u;
  if (v == IOERR)
    return CH_BAD;
  if (v >= eoc)
    return CH_END;
  if (!clus_ok(m, v))
    return CH_BAD;
  *next = v;
  return CH_NEXT;
}

/* Find cluster idx: 0 found; 1 chain ended (*c last, *at index); -1 corrupt or cyclic. */
static int chain_seek(struct fat_mount *m, uint32_t first, uint32_t idx, uint32_t *c, uint32_t *at)
{
  uint32_t cur = first, i = 0;
  /* Writes reset hint_first to 0 but keep hint_clus, so a cluster-0 file must not match the stale hint. */
  if (first && m->hint_first == first && m->hint_clus && m->hint_idx <= idx) {
    cur = m->hint_clus;
    i = m->hint_idx;
  }
  if (!clus_ok(m, cur))
    return -1;
  while (i < idx) {
    uint32_t n;
    int r = chain_next(m, cur, &n);
    if (r == CH_BAD)
      return -1;
    if (r == CH_END) {
      *c = cur;
      *at = i;
      return 1;
    }
    cur = n;
    if (++i > m->nclus)
      return -1;
  }
  m->hint_first = first;
  m->hint_idx = i;
  m->hint_clus = cur;
  *c = cur;
  *at = i;
  return 0;
}

/* Reserve a free cluster as end-of-chain, searching from `near`/free hint. 0 if full. */
static uint32_t alloc_cluster(struct fat_mount *m, uint32_t near, int *err)
{
  uint32_t c = near ? near + 1 : m->next_free;
  if (!clus_ok(m, c))
    c = 2;
  for (uint32_t n = 0; n < m->nclus; n++) {
    uint32_t v = fat_get(m, c);
    if (v == IOERR) {
      *err = FAT_EIO;
      return 0;
    }
    if (v == 0) {
      if (fat_set(m, c, eoc_value(m)) != 0) {
        *err = FAT_EIO;
        return 0;
      }
      m->next_free = c + 1;
      return c;
    }
    if (++c > m->nclus + 1)
      c = 2;
  }
  *err = FAT_ENOSPC;
  return 0;
}

static void chain_free(struct fat_mount *m, uint32_t c)
{
  for (uint32_t guard = 0; clus_ok(m, c) && guard <= m->nclus; guard++) {
    uint32_t n = 0;
    int r = chain_next(m, c, &n);
    if (fat_set(m, c, 0) != 0 || r != CH_NEXT)
      return;
    c = n;
  }
}

static int zero_cluster(struct fat_mount *m, uint32_t c)
{
  uint8_t z[SEC];
  memset(z, 0, SEC);
  for (uint32_t s = 0; s < m->csec; s++)
    if (data_write(m, clus_sec(m, c) + s, z) != 0)
      return -1;
  return 0;
}

static uint32_t count_free(struct fat_mount *m)
{
  uint32_t n = 0;
  for (uint32_t c = 2; c <= m->nclus + 1; c++)
    if (fat_get(m, c) == 0)
      n++;
  return n;
}

/* ------------------------------------------------------------ directories */

/* Raw 32-byte dir slot iterator; the FAT12/16 root is a fixed region, others a chain. */
struct diter {
  struct fat_mount *m;
  uint32_t clus, hops;
  uint32_t mark, mark_at; /* Brent cycle check: the cluster seen at hop mark_at/2 */
  uint32_t sec_n, ent, limit;
  uint32_t cur_sec, cur_off, index;
  bool fixed, have, done, err;
  uint8_t buf[SEC];
};

static void diter_init(struct diter *it, struct fat_mount *m, uint32_t dir)
{
  memset(it, 0, sizeof *it);
  it->m = m;
  it->clus = dir;
  it->mark = dir;
  it->mark_at = 1;
  it->fixed = dir == 0 && m->kind != FAT32;
  it->limit = it->fixed ? m->root_secs : m->csec;
  if (!it->fixed && !clus_ok(m, dir)) {
    it->done = true;
    it->err = dir != BADCLUS;
  }
}

static const uint8_t *diter_raw(struct diter *it)
{
  struct fat_mount *m = it->m;
  for (;;) {
    if (it->done)
      return NULL;
    if (!it->have) {
      if (it->sec_n == it->limit) {
        uint32_t n;
        int r = it->fixed ? CH_END : chain_next(m, it->clus, &n);
        /* A cyclic chain, or one longer than any directory may be, would repeat entries without end. */
        if (r != CH_NEXT || ++it->hops > m->nclus || it->hops >= DIR_MAX_ENTRIES * 32u / m->cbytes || n == it->mark) {
          it->done = true;
          it->err = r == CH_BAD || r == CH_NEXT;
          return NULL;
        }
        if (it->hops == it->mark_at) {
          it->mark = n;
          it->mark_at *= 2;
        }
        it->clus = n;
        it->sec_n = 0;
      }
      uint32_t sec = it->fixed ? m->root_start + it->sec_n : clus_sec(m, it->clus) + it->sec_n;
      if (meta_read(m, sec, it->buf) != 0) {
        it->done = true;
        it->err = true;
        return NULL;
      }
      it->cur_sec = sec;
      it->have = true;
      it->ent = 0;
      it->sec_n++;
    }
    if (it->ent == SEC / 32) {
      it->have = false;
      continue;
    }
    it->cur_off = it->ent * 32;
    it->index++;
    return it->buf + it->ent++ * 32;
  }
}

/* Next slot up to the end marker (0x00), deleted slots included. */
static const uint8_t *diter_next(struct diter *it)
{
  const uint8_t *e = diter_raw(it);
  if (e && e[0] == 0x00) {
    it->done = true;
    return NULL;
  }
  return e;
}

static bool is_dotdot_sfn(const uint8_t *s) { return s[0] == '.' && s[1] == '.' && s[2] == ' '; }

/* Next logical entry (LFN slots verified by checksum). 1 if yielded, 0 at the end. */
static bool dir_read(struct fat_mount *m, struct diter *it, struct dirent *d)
{
  static const uint8_t lo[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
  struct loc run[MAX_RECS];
  uint32_t seq = 0, total = 0, run_n = 0;
  uint8_t crc = 0;

  memset(d, 0, sizeof *d);
  for (;;) {
    const uint8_t *e = diter_next(it);
    if (!e)
      return false;
    if (e[0] == 0xE5) {
      seq = 0;
      continue;
    }
    uint8_t attr = e[11];
    if ((attr & 0x3F) == 0x0F) {
      uint32_t ord = e[0] & 0x3F;
      bool start = e[0] & 0x40;
      if (e[12] != 0 || load_le16(e + 26) != 0 || ord < 1 || ord > 20) {
        seq = 0;
        continue;
      }
      if (start) {
        total = seq = ord;
        crc = e[13];
        run_n = 0;
      } else if (seq <= 1 || ord != seq - 1 || e[13] != crc) {
        seq = 0;
        continue;
      } else {
        seq = ord;
      }
      for (int i = 0; i < 13; i++)
        d->name[(ord - 1) * 13 + i] = load_le16(e + lo[i]);
      run[run_n].sec = it->cur_sec;
      run[run_n].off = (uint16_t)it->cur_off;
      run_n++;
      continue;
    }
    if (attr & 0x08) {
      seq = 0;
      continue;
    }

    memcpy(d->sfn, e, 11);
    d->attr = attr;
    d->ntres = e[12];
    d->ctenth = e[13];
    d->ctime = load_le16(e + 14);
    d->cdate = load_le16(e + 16);
    d->adate = load_le16(e + 18);
    d->mtime = load_le16(e + 22);
    d->mdate = load_le16(e + 24);
    d->cluster = ((uint32_t)load_le16(e + 20) << 16) | load_le16(e + 26);
    d->is_dir = (attr & 0x10) != 0;
    d->size = d->is_dir ? 0 : load_le32(e + 28);
    if (m->kind != FAT32)
      d->cluster &= 0xFFFF;
    if (d->is_dir && d->cluster == 0)
      d->cluster = is_dotdot_sfn(e) ? root_id(m) : BADCLUS;

    d->nlen = 0;
    d->loc_n = 0;
    bool lfn = seq == 1 && lfn_sum(e) == crc;
    if (lfn) {
      uint32_t n = 0;
      while (n < total * 13 && d->name[n] != 0)
        n++;
      lfn = n > 0 && n <= MAX_LFN;
      d->nlen = n;
    }
    if (lfn) {
      for (uint32_t i = 0; i < run_n; i++)
        d->loc[d->loc_n++] = run[i];
    } else {
      d->nlen = sfn_decode(e, d->ntres, d->name);
    }
    d->loc[d->loc_n].sec = it->cur_sec;
    d->loc[d->loc_n].off = (uint16_t)it->cur_off;
    d->loc_n++;
    return true;
  }
}

static bool dent_match(const struct dirent *d, const uint16_t *key, uint32_t klen)
{
  if (ueq(d->name, d->nlen, key, klen))
    return true;
  uint16_t alias[13];
  return ueq(alias, sfn_decode(d->sfn, 0, alias), key, klen);
}

static int dir_lookup(struct fat_mount *m, uint32_t dir, const uint16_t *key, uint32_t klen, struct dirent *out)
{
  struct diter it;
  struct dirent d;
  diter_init(&it, m, dir);
  while (dir_read(m, &it, &d)) {
    if (dent_match(&d, key, klen)) {
      memcpy(out, &d, sizeof d);
      return 0;
    }
  }
  return it.err ? FAT_EIO : FAT_ENOENT;
}

static bool is_dot_entry(const struct dirent *d) { return is_dot_name(d->name, d->nlen); }

/* Resolve path[0..plen) to an entry; the root gives cluster root_id(). */
static int resolve_n(struct fat_mount *m, const char *path, uint32_t plen, struct dirent *out)
{
  memset(out, 0, sizeof *out);
  out->is_dir = true;
  out->attr = FAT_ATTR_DIR;
  out->cluster = root_id(m);

  const char *p = path, *end = path + plen;
  while (p < end) {
    while (p < end && *p == '/')
      p++;
    if (p >= end)
      break;
    const char *c = p;
    while (p < end && *p != '/')
      p++;
    uint32_t len = (uint32_t)(p - c);
    if (len == 1 && c[0] == '.')
      continue;
    if (!out->is_dir)
      return FAT_ENOTDIR;
    if (len == 2 && c[0] == '.' && c[1] == '.' && is_root(m, out->cluster))
      continue;
    uint16_t key[MAX_LFN];
    int kn = utf8_to_u16(c, len, key, MAX_LFN);
    if (kn <= 0)
      return FAT_ENOENT;
    struct dirent child;
    int rc = dir_lookup(m, out->cluster, key, (uint32_t)kn, &child);
    if (rc != 0)
      return rc;
    memcpy(out, &child, sizeof child);
  }
  return 0;
}

static void rc_drop(struct fat_mount *m)
{
  if (m->rc_path) {
    kfree(m->rc_path);
    m->rc_path = NULL;
  }
}

/* Resolve `path` to a directory entry. On the root, returns is_dir with
 * cluster = root (0 for FAT12/16). Returns 0 on success, -1 if absent. */
static int resolve(struct fat_mount *m, const char *path, struct dirent *out)
{
  if (!path)
    path = "";
  if (m->rc_path && streq(m->rc_path, path)) {
    memcpy(out, &m->rc_ent, sizeof *out);
    return 0;
  }
  int rc = resolve_n(m, path, strlen(path), out);
  if (rc == 0) {
    rc_drop(m);
    m->rc_path = kmalloc(strlen(path) + 1);
    if (m->rc_path) {
      memcpy(m->rc_path, path, strlen(path) + 1);
      memcpy(&m->rc_ent, out, sizeof *out);
    }
  }
  return rc;
}

/* Split off the last component: leaf/llen name it, plen is the parent path length. */
static int path_split(const char *path, uint32_t *plen, const char **leaf, uint32_t *llen)
{
  uint32_t n = strlen(path);
  while (n > 0 && path[n - 1] == '/')
    n--;
  uint32_t e = n;
  while (n > 0 && path[n - 1] != '/')
    n--;
  if (e == n)
    return FAT_EINVAL;
  *leaf = path + n;
  *llen = e - n;
  *plen = n;
  if ((*llen == 1 && (*leaf)[0] == '.') || (*llen == 2 && (*leaf)[0] == '.' && (*leaf)[1] == '.'))
    return FAT_EINVAL;
  return 0;
}

/* ------------------------------------------------------------- volume info */

static void fsinfo_flush(struct fat_mount *m)
{
  uint8_t b[SEC];
  if (!m->fsi_sec || !m->fsi_dirty)
    return;
  m->fsi_dirty = false;
  if (meta_read(m, m->fsi_sec, b) != 0)
    return;
  put_le32(b + 488, m->free_known ? m->free_count : UNKNOWN);
  put_le32(b + 492, clus_ok(m, m->next_free) ? m->next_free : 2);
  meta_write(m, m->fsi_sec, b);
}

/* Set/clear the "volume in use" markers: FAT[1] clean bit and boot-sector flag bit 0. */
static void vol_flag(struct fat_mount *m, bool dirty)
{
  uint8_t b[SEC];
  for (uint32_t f = 0; f < m->num_fats; f++) {
    if (!fat_copy_used(m, f))
      continue;
    if (m->kind == FAT16)
      fat_put(m, f, 3, 0x80, dirty ? 0x00 : 0x80);
    else if (m->kind == FAT32)
      fat_put(m, f, 7, 0x08, dirty ? 0x00 : 0x08);
  }
  if (meta_read(m, 0, b) == 0) {
    b[m->res_off] = (uint8_t)(dirty ? (b[m->res_off] | 1) : (b[m->res_off] & ~1));
    meta_write(m, 0, b);
  }
  cl_flush(m);
}

static int op_begin(struct fat_mount *m, bool write)
{
  if (m->kind == FAT_NONE)
    return FAT_EROFS;
  if (write) {
    if (!m->write)
      return FAT_EROFS;
    rc_drop(m);
    m->hint_first = 0;
    if (m->was_clean && !m->flagged) {
      vol_flag(m, true);
      m->flagged = true;
    }
  }
  return 0;
}

static int op_end(struct fat_mount *m, int rc)
{
  if (!m->write)
    return rc;
  rc_drop(m);
  fsinfo_flush(m);
  int f = cl_flush(m);
  if (m->flagged && !m->io_err) {
    vol_flag(m, false);
    m->flagged = false;
  }
  return rc ? rc : (f ? FAT_EIO : 0);
}

/* ------------------------------------------------------------- mounting */

/* Validate a candidate boot sector as a FAT BPB and, if good, populate `*m`.
 * Returns 0 on success. */
static int parse_bpb(struct fat_mount *m, const uint8_t *bs)
{
  if (load_le16(bs + 510) != 0xAA55)
    return -1;
  uint32_t bps = load_le16(bs + 11), spc = bs[13], rsvd = load_le16(bs + 14), nfat = bs[16];
  uint32_t rootent = load_le16(bs + 17), fatsz16 = load_le16(bs + 22);
  uint32_t totsec = load_le16(bs + 19) ? load_le16(bs + 19) : load_le32(bs + 32);
  uint32_t fatsz = fatsz16 ? fatsz16 : load_le32(bs + 36);
  bool fat32 = fatsz16 == 0;

  if (bps < 512 || bps > 4096 || (bps & (bps - 1)))
    return -1;
  if (spc == 0 || (spc & (spc - 1)) || spc > 128)
    return -1;
  if (nfat == 0 || nfat > 2 || rsvd == 0 || fatsz == 0 || totsec == 0)
    return -1;
  if (fat32 ? (rootent != 0 || load_le16(bs + 42) != 0) : rootent == 0)
    return -1;

  uint32_t ss = bps / SEC, root_units = (rootent * 32 + bps - 1) / bps;
  uint64_t meta = (uint64_t)rsvd + (uint64_t)nfat * fatsz + root_units;
  if (meta >= totsec || (uint64_t)totsec * ss > 0xFFFFFFFFu)
    return -1;
  uint32_t nclus = (totsec - (uint32_t)meta) / spc;

  m->kind = fat32 ? FAT32 : nclus >= 4085 ? FAT16 : FAT12;
  uint32_t maxc = m->kind == FAT12 ? 4084u : m->kind == FAT16 ? 65524u : 0x0FFFFFF4u;
  uint64_t cap;
  if (m->kind == FAT12) {
    uint64_t fb = (uint64_t)fatsz * bps;
    cap = (fb > 0xFFFF ? 0xFFFF : (uint32_t)fb) * 2 / 3;
  } else {
    cap = (uint64_t)fatsz * (bps / (m->kind == FAT16 ? 2 : 4));
  }
  if (cap < 3)
    return -1;
  if (nclus > maxc)
    nclus = maxc;
  if (nclus > cap - 2)
    nclus = (uint32_t)(cap - 2);

  m->bps = bps;
  m->csec = spc * ss;
  m->cbytes = m->csec * SEC;
  m->fat_start = rsvd * ss;
  m->fat_secs = fatsz * ss;
  m->num_fats = nfat;
  m->root_entries = rootent;
  m->root_start = m->fat_start + nfat * m->fat_secs;
  m->root_secs = root_units * ss;
  m->data_start = m->root_start + m->root_secs;
  m->nclus = nclus;
  m->root_cluster = 0;
  m->fat_active = 0;
  m->single_fat = false;
  m->fsi_sec = 0;
  m->res_off = 37;

  /* Root location and volume label live in different places per type. */
  const uint8_t *lbl = NULL;
  if (fat32) {
    m->root_cluster = load_le32(bs + 44);
    if (!clus_ok(m, m->root_cluster))
      return -1;
    uint32_t flags = load_le16(bs + 40);
    if ((flags & 0x80) && (flags & 0x0F) < nfat) {
      m->single_fat = true;
      m->fat_active = flags & 0x0F;
    }
    uint32_t fsi = load_le16(bs + 48);
    if (fsi >= 1 && fsi < rsvd)
      m->fsi_sec = fsi * ss;
    m->res_off = 65;
    if (bs[66] == 0x29 || bs[66] == 0x28)
      lbl = bs + 71;
  } else if (bs[38] == 0x29 || bs[38] == 0x28) {
    lbl = bs + 43;
  }

  m->label[0] = '\0';
  if (lbl) {
    uint32_t n = 11;
    while (n > 0 && lbl[n - 1] == ' ') /* trim trailing pad */
      n--;
    for (uint32_t i = 0; i < n; i++)
      m->label[i] = lbl[i] < 0x80 ? (char)lbl[i] : '?';
    m->label[n] = '\0';
    if (streq(m->label, "NO NAME"))
      m->label[0] = '\0';
  }
  return 0;
}

static void mount_finish(struct fat_mount *m, const uint8_t *bs)
{
  uint32_t f1 = m->kind == FAT12 ? 0xFFFFFFFFu : fat_get(m, 1);
  bool clean = m->kind == FAT12 ? true : m->kind == FAT16 ? (f1 & 0x8000u) != 0 : (f1 & 0x08000000u) != 0;
  if (f1 == IOERR)
    clean = false;
  m->was_clean = clean && !(bs[m->res_off] & 1);
  m->next_free = 2;
  m->free_count = 0;
  m->free_known = false;
  if (m->fsi_sec) {
    uint8_t b[SEC];
    if (meta_read(m, m->fsi_sec, b) == 0 && load_le32(b) == 0x41615252u && load_le32(b + 484) == 0x61417272u &&
        load_le32(b + 508) == 0xAA550000u) {
      uint32_t fc = load_le32(b + 488), nf = load_le32(b + 492);
      if (fc <= m->nclus && m->was_clean) {
        m->free_count = fc;
        m->free_known = true;
      }
      if (clus_ok(m, nf))
        m->next_free = nf;
    }
  }

  struct diter it;
  diter_init(&it, m, root_id(m));
  const uint8_t *e;
  while ((e = diter_next(&it))) {
    if (e[0] == 0xE5 || (e[11] & 0x3F) == 0x0F || !(e[11] & 0x08) || (e[11] & 0x10))
      continue;
    uint32_t n = 11;
    while (n > 0 && e[n - 1] == ' ')
      n--;
    for (uint32_t i = 0; i < n; i++)
      m->label[i] = e[i] < 0x80 ? (char)e[i] : '?';
    m->label[n] = '\0';
    break;
  }
}

/* Mount the FAT volume whose boot sector sits at absolute LBA `base_lba`.
 * Returns 0 on success, -1 on failure (bad BPB, unsupported geometry, I/O). */
static int fat_mount_at(struct fat_mount *m, blk_read_fn read, blk_write_fn write, uint32_t base_lba)
{
  uint8_t bs[SEC];
  memset(m, 0, sizeof *m);
  m->kind = FAT_NONE;
  if (!read || read(base_lba, bs) != 0)
    return -1;

  m->read = read;
  m->write = write;
  m->base = base_lba;
  if (parse_bpb(m, bs) != 0) {
    m->kind = FAT_NONE;
    return -1;
  }
  mount_finish(m, bs);
  return 0;
}

/* --------------------------------------------------------------- file data */

/* Rewrite d's 8.3 record; touch stamps the modify time and archive bit. */
static int dirent_commit(struct fat_mount *m, struct dirent *d, bool touch)
{
  if (touch) {
    struct fts t;
    fat_now(&t);
    d->mdate = t.date;
    d->mtime = t.time;
    d->adate = t.date;
    d->attr |= FAT_ATTR_ARCHIVE;
  }
  uint8_t b[SEC];
  struct loc *l = &d->loc[d->loc_n - 1];
  if (meta_read(m, l->sec, b) != 0)
    return FAT_EIO;
  uint8_t *e = b + l->off;
  uint32_t cl = d->cluster == BADCLUS ? 0 : d->cluster; /* BADCLUS stands in for an on-disk 0 */
  memcpy(e, d->sfn, 11);
  e[11] = d->attr;
  e[12] = d->ntres;
  e[13] = d->ctenth;
  put_le16(e + 14, d->ctime);
  put_le16(e + 16, d->cdate);
  put_le16(e + 18, d->adate);
  put_le16(e + 20, cl >> 16);
  put_le16(e + 22, d->mtime);
  put_le16(e + 24, d->mdate);
  put_le16(e + 26, cl);
  put_le32(e + 28, d->is_dir ? 0 : d->size);
  return meta_write(m, l->sec, b) == 0 ? 0 : FAT_EIO;
}

static int file_read(struct fat_mount *m, const struct dirent *d, uint32_t off, uint8_t *dst, uint32_t len)
{
  if (off >= d->size)
    return 0;
  if (len > d->size - off)
    len = d->size - off;
  if (!len)
    return 0;

  uint32_t idx = off / m->cbytes, in = off % m->cbytes, c = 0, at = 0, got = 0;
  if (chain_seek(m, d->cluster, idx, &c, &at) != 0)
    return FAT_EIO;
  while (got < len) {
    uint32_t span = len - got < 0x40000000u ? len - got : 0x40000000u;
    uint32_t need = (in + span - 1) / m->cbytes + 1, run = 1, last = c, nxt = 0;
    int r = CH_END;
    while (run < need) {
      r = chain_next(m, last, &nxt);
      if (r == CH_NEXT && nxt == last + 1) {
        last = nxt;
        run++;
        continue;
      }
      break;
    }
    uint32_t avail = run * m->cbytes - in, take = avail < len - got ? avail : len - got;
    uint32_t sec = clus_sec(m, c) + in / SEC, soff = in % SEC, done = 0;
    while (done < take) {
      uint32_t n = SEC - soff < take - done ? SEC - soff : take - done;
      if (n == SEC) {
        if (data_read(m, sec, dst + got + done) != 0)
          return got ? (int)got : FAT_EIO;
      } else {
        uint8_t tmp[SEC];
        if (data_read(m, sec, tmp) != 0)
          return got ? (int)got : FAT_EIO;
        memcpy(dst + got + done, tmp + soff, n);
      }
      done += n;
      sec++;
      soff = 0;
    }
    got += take;
    m->hint_first = d->cluster;
    m->hint_idx = at + run - 1;
    m->hint_clus = last;
    at += run;
    in = 0;
    if (got >= len)
      break;
    if (r != CH_NEXT)
      break;
    c = nxt;
  }
  return (int)got;
}

/* Write src (NULL = zeros) at off, extending the chain; leaves size and directory alone. */
static int chain_write(struct fat_mount *m, struct dirent *d, uint32_t off, const uint8_t *src, uint32_t len)
{
  if (!len)
    return 0;
  uint32_t end = off + len, cb = m->cbytes, idx = off / cb, c = 0, at = 0, k;
  int err = 0;

  if (d->cluster < 2) {
    c = alloc_cluster(m, 0, &err);
    if (!c)
      return err;
    d->cluster = c;
    at = 0;
    if (!(off == 0 && len >= cb) && zero_cluster(m, c) != 0)
      return FAT_EIO;
  } else if (chain_seek(m, d->cluster, idx, &c, &at) < 0) {
    return FAT_EIO;
  }
  while (at < idx) {
    uint32_t nc = alloc_cluster(m, c, &err);
    if (!nc)
      return err;
    if (fat_set(m, c, nc) != 0 || zero_cluster(m, nc) != 0)
      return FAT_EIO;
    c = nc;
    at++;
  }

  uint8_t zero[SEC];
  memset(zero, 0, SEC);
  uint32_t done = 0, pos = off % cb;
  for (k = idx;; k++) {
    uint32_t take = cb - pos < len - done ? cb - pos : len - done;
    uint32_t sec = clus_sec(m, c) + pos / SEC, soff = pos % SEC, w = 0;
    while (w < take) {
      uint32_t n = SEC - soff < take - w ? SEC - soff : take - w;
      const uint8_t *from = src ? src + done + w : zero;
      if (n == SEC) {
        if (data_write(m, sec, from) != 0)
          return FAT_EIO;
      } else {
        uint8_t tmp[SEC];
        if (data_read(m, sec, tmp) != 0)
          return FAT_EIO;
        memcpy(tmp + soff, from, n);
        if (data_write(m, sec, tmp) != 0)
          return FAT_EIO;
      }
      w += n;
      sec++;
      soff = 0;
    }
    done += take;
    pos = 0;
    if (done >= len)
      break;

    uint32_t nc;
    int r = chain_next(m, c, &nc);
    if (r == CH_BAD)
      return FAT_EIO;
    if (r == CH_END) {
      nc = alloc_cluster(m, c, &err);
      if (!nc)
        return err;
      if (fat_set(m, c, nc) != 0)
        return FAT_EIO;
      bool covered = end - (k + 1) * cb >= cb;
      if (!covered && zero_cluster(m, nc) != 0)
        return FAT_EIO;
    }
    c = nc;
  }
  m->hint_first = d->cluster;
  m->hint_idx = k;
  m->hint_clus = c;
  return 0;
}

/* Clusters that hold `bytes`; (bytes + cbytes - 1) would wrap for sizes within a cluster of 4 GiB. */
static uint32_t clusters_for(const struct fat_mount *m, uint32_t bytes)
{
  return bytes / m->cbytes + (bytes % m->cbytes != 0);
}

/* Drop clusters beyond what newsize needs. */
static int chain_shrink(struct fat_mount *m, struct dirent *d, uint32_t newsize)
{
  uint32_t keep = clusters_for(m, newsize);
  if (d->cluster < 2)
    return 0;
  if (keep == 0) {
    chain_free(m, d->cluster);
    d->cluster = 0;
    return 0;
  }
  uint32_t c, at, n;
  if (chain_seek(m, d->cluster, keep - 1, &c, &at) != 0)
    return FAT_EIO;
  int r = chain_next(m, c, &n);
  if (r == CH_END)
    return 0;
  if (r == CH_BAD || fat_set(m, c, eoc_value(m)) != 0)
    return FAT_EIO;
  chain_free(m, n);
  return 0;
}

/* Fail with ENOSPC up front if growing old_size to new_size needs more clusters than free. */
static int grow_check(struct fat_mount *m, uint32_t old_size, uint32_t new_size)
{
  uint32_t have = clusters_for(m, old_size), need = clusters_for(m, new_size);
  if (need <= have)
    return 0;
  if (!m->free_known || need - have > m->free_count) { /* FSInfo is only a hint: count before refusing */
    m->free_count = count_free(m);
    m->free_known = true;
  }
  return need - have > m->free_count ? FAT_ENOSPC : 0;
}

/* Write into a file, growing it (a gap reads as zeros); rolls the chain back on failure. */
static int file_write(struct fat_mount *m, struct dirent *d, uint32_t off, const uint8_t *src, uint32_t len)
{
  if (len > 0xFFFFFFFFu - off)
    return FAT_EFBIG;
  int rc = grow_check(m, d->size, off + len);
  if (rc)
    return rc;
  uint32_t old_size = d->size, old_clus = d->cluster;
  if (off > old_size)
    rc = chain_write(m, d, old_size, NULL, off - old_size);
  if (!rc)
    rc = chain_write(m, d, off, src, len);
  if (rc) {
    chain_shrink(m, d, old_size);
    if (d->loc_n && d->cluster != old_clus)
      dirent_commit(m, d, false);
    return rc;
  }
  if (off + len > d->size)
    d->size = off + len;
  return 0;
}

static int file_resize(struct fat_mount *m, struct dirent *d, uint32_t size)
{
  if (size == d->size)
    return 0;
  if (size > d->size) {
    int rc = grow_check(m, d->size, size);
    if (!rc)
      rc = chain_write(m, d, d->size, NULL, size - d->size);
    if (rc) {
      chain_shrink(m, d, d->size);
      return rc;
    }
    d->size = size;
    return 0;
  }
  d->size = size;
  return chain_shrink(m, d, size);
}

/* ------------------------------------------------------- entry creation */

static bool sfn_exists(struct fat_mount *m, uint32_t dir, const uint8_t sfn[11], const struct loc *ignore, int *err)
{
  struct diter it;
  const uint8_t *e;
  diter_init(&it, m, dir);
  while ((e = diter_next(&it))) {
    if (e[0] == 0xE5 || (e[11] & 0x3F) == 0x0F || (e[11] & 0x08))
      continue;
    if (ignore && ignore->sec == it.cur_sec && ignore->off == it.cur_off)
      continue;
    if (memcmp(e, sfn, 11) == 0)
      return true;
  }
  *err = it.err ? FAT_EIO : 0;
  return false;
}

static void make_sfn(const uint8_t *base, uint32_t bl, const uint8_t *ext, uint32_t el, uint32_t tail, uint8_t sfn[11])
{
  memset(sfn, ' ', 11);
  uint32_t digits = 0, o = 0;
  for (uint32_t t = tail; t; t /= 10)
    digits++;
  if (tail && bl > 7 - digits)
    bl = 7 - digits;
  memcpy(sfn, base, bl);
  o = bl;
  if (tail) {
    sfn[o++] = '~';
    for (uint32_t i = digits; i > 0; i--) {
      uint32_t p = 1;
      for (uint32_t j = 1; j < i; j++)
        p *= 10;
      sfn[o++] = (uint8_t)('0' + (tail / p) % 10);
    }
  }
  memcpy(sfn + 8, ext, el);
}

/* Pick the 8.3 alias; *lfn says whether long-name slots are needed for the original. */
static int alias_for(struct fat_mount *m, uint32_t dir, const uint16_t *name, uint32_t nlen, uint8_t sfn[11],
                     uint8_t *ntres, bool *lfn, const struct loc *ignore)
{
  int err = 0;
  *ntres = 0;
  *lfn = false;
  if (fit_83(name, nlen, sfn, ntres)) {
    bool taken = sfn_exists(m, dir, sfn, ignore, &err);
    if (err || !taken)
      return err;
  }
  *ntres = 0;
  *lfn = true;

  uint8_t base[8], ext[3];
  uint32_t bl = 0, el = 0, start = 0, last_dot = nlen;
  bool lossy = false;
  while (start < nlen && name[start] == '.') {
    start++;
    lossy = true;
  }
  for (uint32_t i = nlen; i > start; i--)
    if (name[i - 1] == '.') {
      last_dot = i - 1;
      break;
    }
  for (uint32_t i = start; i < nlen; i++) {
    uint16_t ch = name[i];
    if (i == last_dot)
      continue;
    if (ch == ' ' || ch == '.' || !short_legal(ch))
      lossy = true;
    if (ch == ' ' || ch == '.')
      continue;
    uint8_t out = short_legal(ch) ? upper8(ch) : '_';
    if (last_dot != nlen && i > last_dot) {
      if (el < 3)
        ext[el++] = out;
      else
        lossy = true;
    } else if (bl < 8) {
      base[bl++] = out;
    } else {
      lossy = true;
    }
  }
  if (bl == 0) {
    base[bl++] = '_';
    lossy = true;
  }
  if (!lossy) {
    make_sfn(base, bl, ext, el, 0, sfn);
    bool taken = sfn_exists(m, dir, sfn, ignore, &err);
    if (err || !taken)
      return err;
  }
  for (uint32_t n = 1; n < 1000000; n++) {
    make_sfn(base, bl, ext, el, n, sfn);
    bool taken = sfn_exists(m, dir, sfn, ignore, &err);
    if (err || !taken)
      return err;
  }
  return FAT_ENOSPC;
}

/* Find `need` consecutive free slots in a directory, growing a chain directory when full. */
static int dir_find_free(struct fat_mount *m, uint32_t dir, uint32_t need, struct loc *out)
{
  struct diter it;
  const uint8_t *e;
  uint32_t run = 0;
  bool end_seen = false;

  diter_init(&it, m, dir);
  while ((e = diter_raw(&it))) {
    if (e[0] == 0x00)
      end_seen = true;
    if (end_seen || e[0] == 0xE5) {
      out[run].sec = it.cur_sec;
      out[run].off = (uint16_t)it.cur_off;
      if (++run == need)
        return 0;
    } else {
      run = 0;
    }
  }
  if (it.err)
    return FAT_EIO;
  if (it.fixed)
    return FAT_ENOSPC;

  uint32_t more = need - run, per = m->cbytes / 32, last = it.clus, first_new = 0;
  if (it.index + more > DIR_MAX_ENTRIES)
    return FAT_ENOSPC;
  uint32_t orig_last = last;
  while (run < need) {
    int err = 0;
    uint32_t nc = alloc_cluster(m, last, &err);
    int rc = nc ? 0 : err;
    if (nc && (zero_cluster(m, nc) != 0 || fat_set(m, last, nc) != 0))
      rc = FAT_EIO;
    if (rc) {
      if (first_new) {
        fat_set(m, orig_last, eoc_value(m));
        chain_free(m, first_new);
      } else if (nc) {
        chain_free(m, nc);
      }
      return rc;
    }
    if (!first_new)
      first_new = nc;
    last = nc;
    for (uint32_t k = 0; k < per && run < need; k++, run++) {
      out[run].sec = clus_sec(m, nc) + k * 32 / SEC;
      out[run].off = (uint16_t)(k * 32 % SEC);
    }
  }
  return 0;
}

static void lfn_slot(uint8_t *e, uint32_t ord, bool last, const uint16_t *name, uint32_t nlen, uint8_t crc)
{
  static const uint8_t lo[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
  memset(e, 0, 32);
  e[0] = (uint8_t)(ord | (last ? 0x40 : 0));
  e[11] = 0x0F;
  e[13] = crc;
  for (uint32_t i = 0; i < 13; i++) {
    uint32_t p = (ord - 1) * 13 + i;
    put_le16(e + lo[i], p < nlen ? name[p] : p == nlen ? 0x0000 : 0xFFFF);
  }
}

static int slot_write(struct fat_mount *m, const struct loc *l, const uint8_t *e)
{
  uint8_t b[SEC];
  if (meta_read(m, l->sec, b) != 0)
    return FAT_EIO;
  memcpy(b + l->off, e, 32);
  return meta_write(m, l->sec, b) == 0 ? 0 : FAT_EIO;
}

/* Add a directory entry `name` with d's attributes, times, cluster and size; fills d. */
static int dir_add(struct fat_mount *m, uint32_t dir, const uint16_t *name, uint32_t nlen, struct dirent *d,
                   const struct loc *ignore)
{
  uint8_t sfn[11], ntres;
  bool lfn;
  int rc = alias_for(m, dir, name, nlen, sfn, &ntres, &lfn, ignore);
  if (rc)
    return rc;
  uint32_t nl = lfn ? (nlen + 12) / 13 : 0;
  struct loc locs[MAX_RECS];
  rc = dir_find_free(m, dir, nl + 1, locs);
  if (rc)
    return rc;

  uint8_t crc = lfn_sum(sfn), e[32];
  for (uint32_t i = 0; i < nl; i++) {
    lfn_slot(e, nl - i, i == 0, name, nlen, crc);
    if ((rc = slot_write(m, &locs[i], e)) != 0)
      return rc;
  }
  memcpy(d->sfn, sfn, 11);
  d->ntres = ntres;
  memcpy(d->name, name, nlen * sizeof name[0]);
  d->nlen = nlen;
  for (uint32_t i = 0; i <= nl; i++)
    d->loc[i] = locs[i];
  d->loc_n = (int)nl + 1;
  return dirent_commit(m, d, false);
}

static void dirent_new(struct dirent *d, uint8_t attr, uint32_t cluster, uint32_t size)
{
  struct fts t;
  fat_now(&t);
  memset(d, 0, sizeof *d);
  d->attr = attr;
  d->is_dir = (attr & FAT_ATTR_DIR) != 0;
  d->cluster = cluster;
  d->size = size;
  d->cdate = d->mdate = d->adate = t.date;
  d->ctime = d->mtime = t.time;
  d->ctenth = t.tenth;
}

static int wipe_entry(struct fat_mount *m, const struct dirent *d)
{
  uint8_t b[SEC];
  for (int i = 0; i < d->loc_n; i++) {
    if (meta_read(m, d->loc[i].sec, b) != 0)
      return FAT_EIO;
    b[d->loc[i].off] = 0xE5;
    if (meta_write(m, d->loc[i].sec, b) != 0)
      return FAT_EIO;
  }
  return 0;
}

/* Resolve the parent directory of path and convert its leaf to UTF-16. */
static int parent_of(struct fat_mount *m, const char *path, struct dirent *parent, uint16_t *key, uint32_t *klen)
{
  const char *leaf;
  uint32_t plen, llen;
  int rc = path_split(path, &plen, &leaf, &llen);
  if (rc)
    return rc;
  int kn = utf8_to_u16(leaf, llen, key, MAX_LFN);
  if (kn < 0 || !name_valid(key, (uint32_t)kn))
    return FAT_EINVAL;
  *klen = (uint32_t)kn;
  rc = resolve_n(m, path, plen, parent);
  if (rc)
    return rc;
  return parent->is_dir ? 0 : FAT_ENOTDIR;
}

/* ------------------------------------------------------------ operations */

static int create_impl(struct fat_mount *m, const char *path, const void *data, uint32_t size)
{
  struct dirent parent, ex, nd;
  uint16_t key[MAX_LFN];
  uint32_t klen;
  int rc = parent_of(m, path, &parent, key, &klen);
  if (rc)
    return rc;

  rc = dir_lookup(m, parent.cluster, key, klen, &ex);
  if (rc == 0) {
    if (ex.is_dir)
      return FAT_EISDIR;
    if (ex.attr & FAT_ATTR_RO)
      return FAT_EACCES;
    uint32_t old = ex.size;
    rc = file_write(m, &ex, 0, data, size);
    if (rc)
      return rc;
    if (size < old)
      rc = file_resize(m, &ex, size);
    if (!rc)
      rc = dirent_commit(m, &ex, true);
    return rc;
  }
  if (rc != FAT_ENOENT)
    return rc;

  dirent_new(&nd, FAT_ATTR_ARCHIVE, 0, 0);
  rc = file_write(m, &nd, 0, data, size);
  if (rc)
    return rc;
  rc = dir_add(m, parent.cluster, key, klen, &nd, NULL);
  if (rc && nd.cluster)
    chain_free(m, nd.cluster);
  return rc;
}

static int delete_impl(struct fat_mount *m, const char *path)
{
  const char *leaf;
  uint32_t plen, llen;
  struct dirent d;
  int rc = path_split(path, &plen, &leaf, &llen);
  if (rc)
    return rc;
  if ((rc = resolve_n(m, path, plen + llen, &d)) != 0)
    return rc;
  if (d.is_dir)
    return FAT_EISDIR;
  if (d.attr & FAT_ATTR_RO)
    return FAT_EACCES;
  if ((rc = wipe_entry(m, &d)) != 0)
    return rc;
  if (d.cluster >= 2)
    chain_free(m, d.cluster);
  return 0;
}

static int mkdir_impl(struct fat_mount *m, const char *path)
{
  struct dirent parent, ex, nd;
  uint16_t key[MAX_LFN];
  uint32_t klen;
  int rc = parent_of(m, path, &parent, key, &klen);
  if (rc)
    return rc;
  rc = dir_lookup(m, parent.cluster, key, klen, &ex);
  if (rc == 0)
    return FAT_EEXIST;
  if (rc != FAT_ENOENT)
    return rc;

  int err = 0;
  uint32_t dc = alloc_cluster(m, 0, &err);
  if (!dc)
    return err;
  if (zero_cluster(m, dc) != 0) {
    chain_free(m, dc);
    return FAT_EIO;
  }
  dirent_new(&nd, FAT_ATTR_DIR, dc, 0);

  uint8_t b[SEC];
  memset(b, 0, SEC);
  for (int i = 0; i < 2; i++) {
    uint8_t *e = b + i * 32;
    memset(e, ' ', 11);
    e[0] = '.';
    if (i)
      e[1] = '.';
    e[11] = FAT_ATTR_DIR;
    e[13] = nd.ctenth;
    put_le16(e + 14, nd.ctime);
    put_le16(e + 16, nd.cdate);
    put_le16(e + 18, nd.adate);
    put_le16(e + 22, nd.mtime);
    put_le16(e + 24, nd.mdate);
    uint32_t cl = i ? (is_root(m, parent.cluster) ? 0 : parent.cluster) : dc;
    put_le16(e + 20, cl >> 16);
    put_le16(e + 26, cl);
  }
  if (data_write(m, clus_sec(m, dc), b) != 0) {
    chain_free(m, dc);
    return FAT_EIO;
  }
  rc = dir_add(m, parent.cluster, key, klen, &nd, NULL);
  if (rc)
    chain_free(m, dc);
  return rc;
}

static bool dir_empty(struct fat_mount *m, uint32_t dir, int *err)
{
  struct diter it;
  struct dirent e;
  diter_init(&it, m, dir);
  while (dir_read(m, &it, &e))
    if (!is_dot_entry(&e))
      return false;
  *err = it.err ? FAT_EIO : 0;
  return true;
}

static int rmdir_impl(struct fat_mount *m, const char *path)
{
  const char *leaf;
  uint32_t plen, llen;
  struct dirent d;
  int rc = path_split(path, &plen, &leaf, &llen);
  if (rc)
    return rc;
  if ((rc = resolve_n(m, path, plen + llen, &d)) != 0)
    return rc;
  if (!d.is_dir)
    return FAT_ENOTDIR;
  if (d.attr & FAT_ATTR_RO)
    return FAT_EACCES;
  int err = 0;
  if (!dir_empty(m, d.cluster, &err))
    return FAT_ENOTEMPTY;
  if (err)
    return err;
  if ((rc = wipe_entry(m, &d)) != 0)
    return rc;
  if (d.cluster >= 2 && d.cluster != BADCLUS)
    chain_free(m, d.cluster);
  return 0;
}

/* True if `dir` is `anc` or lies below it, following ".." entries up to the root. */
static bool dir_under(struct fat_mount *m, uint32_t dir, uint32_t anc)
{
  for (uint32_t guard = 0; guard <= m->nclus; guard++) {
    if (dir == anc)
      return true;
    if (is_root(m, dir))
      return false;
    struct dirent dd;
    uint16_t dots[2] = {'.', '.'};
    if (dir_lookup(m, dir, dots, 2, &dd) != 0)
      return true;
    dir = dd.cluster;
  }
  return true;
}

static int rename_impl(struct fat_mount *m, const char *from, const char *to)
{
  const char *leaf;
  uint32_t plen, llen;
  struct dirent *buf = kmalloc(3 * sizeof *buf);
  if (!buf)
    return FAT_ENOMEM;
  struct dirent *src = &buf[0], *dp = &buf[1], *ex = &buf[2];
  uint16_t key[MAX_LFN];
  uint32_t klen;

  int rc = path_split(from, &plen, &leaf, &llen);
  if (!rc)
    rc = resolve_n(m, from, plen + llen, src);
  if (!rc)
    rc = parent_of(m, to, dp, key, &klen);
  if (rc)
    goto out;
  if (src->attr & FAT_ATTR_RO && !src->is_dir) {
    rc = FAT_EACCES;
    goto out;
  }
  if (src->is_dir && dir_under(m, dp->cluster, src->cluster)) {
    rc = FAT_EINVAL;
    goto out;
  }

  rc = dir_lookup(m, dp->cluster, key, klen, ex);
  if (rc == 0) {
    const struct loc *a = &ex->loc[ex->loc_n - 1], *b = &src->loc[src->loc_n - 1];
    if (a->sec != b->sec || a->off != b->off) {
      if (ex->is_dir || src->is_dir) {
        rc = FAT_EEXIST;
        goto out;
      }
      if (ex->attr & FAT_ATTR_RO) {
        rc = FAT_EACCES;
        goto out;
      }
      if ((rc = wipe_entry(m, ex)) != 0)
        goto out;
      if (ex->cluster >= 2)
        chain_free(m, ex->cluster);
    } else if (ex->nlen == klen && memcmp(ex->name, key, klen * sizeof key[0]) == 0 && ex->loc_n == src->loc_n) {
      rc = 0;
      goto out;
    }
  } else if (rc != FAT_ENOENT) {
    goto out;
  }

  struct dirent nd;
  memcpy(&nd, src, sizeof nd);
  nd.loc_n = 0;
  if ((rc = dir_add(m, dp->cluster, key, klen, &nd, &src->loc[src->loc_n - 1])) != 0)
    goto out;
  if (src->is_dir && clus_ok(m, src->cluster)) {
    uint8_t b[SEC];
    uint32_t up = is_root(m, dp->cluster) ? 0 : dp->cluster;
    if (data_read(m, clus_sec(m, src->cluster), b) == 0 && b[32] == '.' && b[33] == '.') {
      put_le16(b + 32 + 20, up >> 16);
      put_le16(b + 32 + 26, up);
      if (data_write(m, clus_sec(m, src->cluster), b) != 0)
        rc = FAT_EIO;
    }
  }
  if (!rc)
    rc = wipe_entry(m, src);
out:
  kfree(buf);
  return rc;
}

static int write_at_impl(struct fat_mount *m, const char *path, uint32_t off, const uint8_t *buf, uint32_t len)
{
  struct dirent d;
  int rc = resolve(m, path, &d);
  if (rc)
    return rc;
  if (d.is_dir || d.loc_n == 0)
    return FAT_EISDIR;
  if (d.attr & FAT_ATTR_RO)
    return FAT_EACCES;
  if ((rc = file_write(m, &d, off, buf, len)) != 0)
    return rc;
  if ((rc = dirent_commit(m, &d, true)) != 0)
    return rc;
  return (int)len;
}

static int truncate_impl(struct fat_mount *m, const char *path, uint32_t size)
{
  struct dirent d;
  int rc = resolve(m, path, &d);
  if (rc)
    return rc;
  if (d.is_dir || d.loc_n == 0)
    return FAT_EISDIR;
  if (d.attr & FAT_ATTR_RO)
    return FAT_EACCES;
  if ((rc = file_resize(m, &d, size)) != 0)
    return rc;
  return dirent_commit(m, &d, true);
}

static int setattr_impl(struct fat_mount *m, const char *path, uint8_t set, uint8_t clear)
{
  struct dirent d;
  int rc = resolve(m, path, &d);
  if (rc)
    return rc;
  if (d.loc_n == 0)
    return FAT_EINVAL;
  uint8_t mask = FAT_ATTR_RO | FAT_ATTR_HIDDEN | FAT_ATTR_SYSTEM | FAT_ATTR_ARCHIVE;
  d.attr = (uint8_t)((d.attr & ~(clear & mask)) | (set & mask));
  return dirent_commit(m, &d, false);
}

/* ---------------------------------------------------------- public ops */

static int fat_stat_op(struct fat_mount *m, const char *path, int *is_dir, uint32_t *size)
{
  struct dirent d;
  if (op_begin(m, false) != 0)
    return -2;
  int rc = resolve(m, path, &d);
  if (rc)
    return rc;
  if (is_dir)
    *is_dir = d.is_dir;
  if (size)
    *size = d.size;
  return 0;
}

int fat_getattr(void *inst, const char *path, struct fat_stat *st)
{
  struct fat_mount *m = inst;
  struct dirent d;
  int rc = op_begin(m, false);
  if (!rc)
    rc = resolve(m, path, &d);
  if (rc)
    return rc;
  st->size = d.size;
  st->cluster = d.cluster == BADCLUS ? 0 : d.cluster;
  st->attr = d.attr;
  st->is_dir = d.is_dir;
  st->ctime = fat_to_unix(d.cdate, d.ctime);
  st->mtime = fat_to_unix(d.mdate, d.mtime);
  st->atime = fat_to_unix(d.adate, 0);
  return 0;
}

int fat_statfs(void *inst, struct fat_statfs *out)
{
  struct fat_mount *m = inst;
  int rc = op_begin(m, false);
  if (rc)
    return rc;
  if (!m->free_known) {
    m->free_count = count_free(m);
    m->free_known = true;
  }
  out->cluster_bytes = m->cbytes;
  out->total_clusters = m->nclus;
  out->free_clusters = m->free_count;
  out->bits = m->kind == FAT12 ? 12 : m->kind == FAT16 ? 16 : 32;
  return 0;
}

static int fat_list_op(struct fat_mount *m, const char *path)
{
  struct dirent d, e;
  char name[4 * MAX_LFN + 1];
  if (op_begin(m, false) != 0)
    return -2;
  if (resolve(m, path, &d) != 0 || !d.is_dir)
    return -1;
  struct diter it;
  diter_init(&it, m, d.cluster);
  while (dir_read(m, &it, &e)) {
    if (is_dot_entry(&e))
      continue;
    u16_to_utf8(e.name, e.nlen, name, sizeof name);
    con_write(e.is_dir ? "  <DIR>  " : "         ");
    if (!e.is_dir)
      con_printf("%u  ", e.size);
    else
      con_write("  ");
    con_printf("%s\n", name);
  }
  return 0;
}

static int fat_read_at_op(struct fat_mount *m, const char *path, uint32_t off, void *buf, uint32_t len)
{
  struct dirent d;
  if (op_begin(m, false) != 0)
    return -2;
  if (resolve(m, path, &d) != 0 || d.is_dir)
    return -1;
  return file_read(m, &d, off, buf, len);
}

int fat_enum_dir_cap(void *inst, const char *path, int idx, char *name, uint32_t cap, int *is_dir, uint32_t *size)
{
  struct fat_mount *m = inst;
  struct dirent d, e;
  if (op_begin(m, false) != 0 || idx < 0 || resolve(m, path, &d) != 0 || !d.is_dir)
    return 0;
  struct diter it;
  int count = 0;
  diter_init(&it, m, d.cluster);
  while (dir_read(m, &it, &e)) {
    if (is_dot_entry(&e))
      continue;
    if (count++ != idx)
      continue;
    if (u16_to_utf8(e.name, e.nlen, name, cap) < 0) {
      uint16_t alias[13];
      u16_to_utf8(alias, sfn_decode(e.sfn, e.ntres, alias), name, cap);
    }
    if (is_dir)
      *is_dir = e.is_dir;
    if (size)
      *size = e.size;
    return 1;
  }
  return 0;
}

#define MUTATE(m, call)                                                                                                \
  do {                                                                                                                 \
    int _rc = op_begin((m), true);                                                                                     \
    if (_rc)                                                                                                           \
      return _rc;                                                                                                      \
    return op_end((m), (call));                                                                                        \
  } while (0)

int fat_setattr(void *inst, const char *path, uint8_t set, uint8_t clear)
{
  MUTATE((struct fat_mount *)inst, setattr_impl(inst, path, set, clear));
}

int fat_rename(void *inst, const char *from, const char *to)
{
  MUTATE((struct fat_mount *)inst, rename_impl(inst, from, to));
}

int fat_truncate(void *inst, const char *path, uint32_t size)
{
  MUTATE((struct fat_mount *)inst, truncate_impl(inst, path, size));
}

int fat_write_at(void *inst, const char *path, uint32_t off, const void *buf, uint32_t len)
{
  struct fat_mount *m = inst;
  int rc = op_begin(m, true);
  if (rc)
    return rc;
  rc = write_at_impl(m, path, off, buf, len);
  int e = op_end(m, rc < 0 ? rc : 0);
  return e ? e : rc;
}

/* ---------------------------------------------------------------- format */

/* Write one bps-sized unit (volume-relative, in bps units) as ss device sectors. */
static int fmt_unit(const struct fat_format_opts *o, uint32_t ss, uint32_t unit, const uint8_t *buf)
{
  for (uint32_t s = 0; s < ss; s++)
    if (o->write(o->base_lba + unit * ss + s, buf + s * SEC) != 0)
      return FAT_EIO;
  return 0;
}

static int fmt_zero(const struct fat_format_opts *o, uint32_t ss, uint32_t unit, uint32_t n, uint8_t *z)
{
  memset(z, 0, 4096);
  for (uint32_t i = 0; i < n; i++)
    if (fmt_unit(o, ss, unit + i, z) != 0)
      return FAT_EIO;
  return 0;
}

/* Microsoft's cluster-size tables by volume size (in 512-byte sectors). */
static uint32_t default_spc(int kind, uint32_t sectors)
{
  if (kind == 32)
    return sectors <= 532480 ? 1 : sectors <= 16777216 ? 8 : sectors <= 33554432 ? 16 : sectors <= 67108864 ? 32 : 64;
  if (kind == 16)
    return sectors <= 32680     ? 2
           : sectors <= 262144  ? 4
           : sectors <= 524288  ? 8
           : sectors <= 1048576 ? 16
           : sectors <= 2097152 ? 32
                                : 64;
  uint32_t spc = 1;
  while (sectors / spc > 4084 && spc < 128)
    spc *= 2;
  return spc;
}

struct fmt_geom {
  uint32_t spc, rsvd, rootent, root_units, fatsz, clusters;
};

/* Lay out `total` sectors for one kind; false if the cluster count is out of range. */
static bool fmt_geometry(int kind, uint32_t total, uint32_t bps, uint32_t spc, uint32_t sectors, struct fmt_geom *g)
{
  g->spc = spc;
  g->rsvd = kind == 32 ? 32 : 1;
  g->rootent = kind == 32 ? 0 : (sectors <= 5760 ? 224 : 512);
  g->root_units = (g->rootent * 32 + bps - 1) / bps;
  g->fatsz = 1;
  for (int iter = 0; iter < 64; iter++) {
    uint64_t meta = (uint64_t)g->rsvd + 2 * (uint64_t)g->fatsz + g->root_units;
    if (meta >= total)
      return false;
    g->clusters = (total - (uint32_t)meta) / spc;
    if (g->clusters > (kind == 12 ? 4084u : kind == 16 ? 65524u : 0x0FFFFFF4u))
      return false;
    uint32_t need = kind == 12 ? ((g->clusters + 2) * 3 + 1) / 2 : (g->clusters + 2) * ((uint32_t)kind / 8);
    uint32_t want = (need + bps - 1) / bps;
    if (want == g->fatsz)
      break;
    g->fatsz = want;
  }
  if (kind == 12)
    return g->clusters >= 1 && g->clusters <= 4084;
  if (kind == 16)
    return g->clusters >= 4085 && g->clusters <= 65524;
  return g->clusters >= 65525 && g->clusters <= 0x0FFFFFF4u;
}

int fat_format(const struct fat_format_opts *o)
{
  uint32_t bps = o->bytes_per_sec ? o->bytes_per_sec : 512;
  if (!o->write || bps < 512 || bps > 4096 || (bps & (bps - 1)) || o->sectors < 8)
    return FAT_EINVAL;
  uint32_t ss = bps / SEC, total = o->sectors / ss;
  int kind = o->kind;
  if (kind == 0)
    kind = o->sectors <= 16384 ? 12 : o->sectors <= 4194304 ? 16 : 32;
  if (kind != 12 && kind != 16 && kind != 32)
    return FAT_EINVAL;

  struct fmt_geom g;
  if (o->sec_per_clus) {
    if ((o->sec_per_clus & (o->sec_per_clus - 1)) || o->sec_per_clus > 128 ||
        !fmt_geometry(kind, total, bps, o->sec_per_clus, o->sectors, &g))
      return FAT_EINVAL;
  } else {
    uint32_t want = default_spc(kind, o->sectors) / ss ? default_spc(kind, o->sectors) / ss : 1;
    bool ok = fmt_geometry(kind, total, bps, want, o->sectors, &g);
    for (uint32_t s = want / 2; !ok && s >= 1; s /= 2)
      ok = fmt_geometry(kind, total, bps, s, o->sectors, &g);
    for (uint32_t s = want * 2; !ok && s <= 128; s *= 2)
      ok = fmt_geometry(kind, total, bps, s, o->sectors, &g);
    if (!ok)
      return FAT_EINVAL;
  }

  struct fts t;
  fat_now(&t);
  uint32_t serial = o->serial;
  if (!serial) {
    struct timeval64 tv;
    wallclock_get(&tv);
    serial = (uint32_t)tv.tv_sec * 2654435761u ^ ((uint32_t)tv.tv_usec << 12) ^ o->sectors;
  }
  char lbl[11];
  memset(lbl, ' ', 11);
  const char *label = o->label && o->label[0] ? o->label : NULL;
  if (label)
    for (int i = 0; i < 11 && label[i]; i++)
      lbl[i] = (char)upper8((uint8_t)label[i]);

  uint32_t nfat = 2, root0 = g.rsvd + nfat * g.fatsz;
  uint32_t ext = kind == 32 ? 64 : 36;
  bool small = kind != 32 && total < 65536;
  uint8_t b[4096];
  memset(b, 0, sizeof b);
  b[0] = 0xEB;
  b[1] = kind == 32 ? 0x58 : 0x3C;
  b[2] = 0x90;
  memcpy(b + 3, "OWOS    ", 8);
  put_le16(b + 11, bps);
  b[13] = (uint8_t)g.spc;
  put_le16(b + 14, g.rsvd);
  b[16] = (uint8_t)nfat;
  put_le16(b + 17, g.rootent);
  put_le16(b + 19, small ? total : 0);
  b[21] = 0xF8;
  put_le16(b + 22, kind == 32 ? 0 : g.fatsz);
  put_le16(b + 24, 63);
  put_le16(b + 26, 255);
  put_le32(b + 28, o->base_lba);
  put_le32(b + 32, small ? 0 : total);
  if (kind == 32) {
    put_le32(b + 36, g.fatsz);
    put_le32(b + 44, 2);
    put_le16(b + 48, 1);
    put_le16(b + 50, 6);
  }
  b[ext] = 0x80;
  b[ext + 2] = 0x29;
  put_le32(b + ext + 3, serial);
  memcpy(b + ext + 7, label ? lbl : "NO NAME    ", 11);
  memcpy(b + ext + 18, kind == 32 ? "FAT32   " : kind == 16 ? "FAT16   " : "FAT12   ", 8);
  b[ext + 26] = 0xFA;
  b[ext + 27] = 0xF4;
  b[ext + 28] = 0xEB;
  b[ext + 29] = 0xFD;
  b[510] = 0x55;
  b[511] = 0xAA;
  b[bps - 2] = 0x55;
  b[bps - 1] = 0xAA;

  uint8_t w[4096];
  int rc = fmt_unit(o, ss, 0, b);
  if (kind == 32 && !rc) {
    memset(w, 0, sizeof w);
    put_le32(w, 0x41615252u);
    put_le32(w + 484, 0x61417272u);
    put_le32(w + 488, g.clusters - 1);
    put_le32(w + 492, 3);
    put_le32(w + 508, 0xAA550000u);
    w[bps - 2] = 0x55;
    w[bps - 1] = 0xAA;
    rc = fmt_unit(o, ss, 1, w) ?: fmt_unit(o, ss, 7, w) ?: fmt_unit(o, ss, 6, b);
    memset(w, 0, sizeof w);
    put_le32(w + 508, 0xAA550000u);
    w[bps - 2] = 0x55;
    w[bps - 1] = 0xAA;
    rc = rc ?: fmt_unit(o, ss, 2, w) ?: fmt_unit(o, ss, 8, w);
    for (uint32_t u = 3; u < g.rsvd && !rc; u++)
      if (u < 6 || u > 8)
        rc = fmt_zero(o, ss, u, 1, w);
  } else {
    rc = rc ?: fmt_zero(o, ss, 1, g.rsvd - 1, w);
  }

  for (uint32_t f = 0; f < nfat && !rc; f++) {
    memset(w, 0, sizeof w);
    if (kind == 12) {
      w[0] = 0xF8;
      w[1] = w[2] = 0xFF;
    } else if (kind == 16) {
      put_le32(w, 0xFFFFFFF8u);
    } else {
      put_le32(w, 0x0FFFFFF8u);
      put_le32(w + 4, 0x0FFFFFFFu);
      put_le32(w + 8, 0x0FFFFFFFu);
    }
    rc = fmt_unit(o, ss, g.rsvd + f * g.fatsz, w);
    if (!rc && g.fatsz > 1)
      rc = fmt_zero(o, ss, g.rsvd + f * g.fatsz + 1, g.fatsz - 1, w);
  }
  uint32_t zero_units = kind == 32 ? g.spc : g.root_units;
  if (!rc)
    rc = fmt_zero(o, ss, root0, zero_units, w);

  if (!rc && label) {
    memset(w, 0, sizeof w);
    memcpy(w, lbl, 11);
    w[11] = 0x08;
    put_le16(w + 22, t.time);
    put_le16(w + 24, t.date);
    rc = fmt_unit(o, ss, root0, w);
  }
  return rc;
}

/* ----------------------------------------------------------------- probe */

static bool part_type_fat(uint8_t type)
{
  return type == 0x01 || type == 0x04 || type == 0x06 || type == 0x0B || type == 0x0C || type == 0x0E || type == 0x11 ||
         type == 0x14 || type == 0x16 || type == 0x1B || type == 0x1C || type == 0x1E;
}

static const uint8_t guid_basic[16] = {0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
                                       0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7};
static const uint8_t guid_esp[16] = {0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
                                     0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B};

static bool gpt_try(struct fat_mount *m, blk_read_fn read, blk_write_fn write)
{
  uint8_t hdr[SEC], sec[SEC];
  if (read(1, hdr) != 0 || memcmp(hdr, "EFI PART", 8) != 0)
    return false;
  uint64_t lba = load_le64(hdr + 72);
  uint32_t count = load_le32(hdr + 80), esz = load_le32(hdr + 84);
  if (lba == 0 || lba > 0xFFFFFFFFu || esz < 128 || esz > SEC || SEC % esz != 0 || count > 1024)
    return false;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t byte = i * esz;
    if (byte % SEC == 0 || i == 0) {
      if (read((uint32_t)lba + byte / SEC, sec) != 0)
        return false;
    }
    const uint8_t *e = sec + byte % SEC;
    if (memcmp(e, guid_basic, 16) != 0 && memcmp(e, guid_esp, 16) != 0)
      continue;
    uint64_t first = load_le64(e + 32);
    if (first == 0 || first > 0xFFFFFFFFu)
      continue;
    if (fat_mount_at(m, read, write, (uint32_t)first) == 0)
      return true;
  }
  return false;
}

/* Whole disk (BPB at LBA 0), then MBR primary partitions, then GPT FAT partitions. */
/* ----------------------------------------------------------------- probe + ops
 *
 * Probe one block device for a mountable FAT volume -- whole-disk first (BPB at
 * LBA 0), then each primary MBR partition -- and build a mount instance for the
 * first found. Device-agnostic: the caller (blk.c) supplies a specific disk's
 * read/write; this code never names a controller. `write` may be NULL for a
 * read-only mount. */
static void *fat_probe(blk_read_fn read, blk_write_fn write)
{
  struct fat_mount *m = kmalloc(sizeof *m);
  if (!m)
    return NULL;

  if (fat_mount_at(m, read, write, 0) == 0) /* whole disk: BPB at LBA 0 */
    return m;

  uint8_t mbr[SEC];
  if (read && read(0, mbr) == 0 && load_le16(mbr + 510) == 0xAA55) {
    for (int i = 0; i < 4; i++) {
      const uint8_t *p = mbr + 446 + i * 16;
      uint32_t lba = load_le32(p + 8);
      if (p[4] == 0xEE) {
        if (gpt_try(m, read, write))
          return m;
      } else if (part_type_fat(p[4]) && lba != 0 && fat_mount_at(m, read, write, lba) == 0) {
        return m;
      }
    }
  }
  kfree(m);
  return NULL;
}

static void fat_close(void *inst)
{
  struct fat_mount *m = inst;
  if (m->write && m->kind != FAT_NONE)
    op_end(m, 0);
  rc_drop(m);
  kfree(m);
}

#define M(inst) ((struct fat_mount *)(inst))

static const char *fat_op_label(void *inst) { return M(inst)->label; }

static const char *fat_op_type(void *inst)
{
  switch (M(inst)->kind) {
  case FAT12:
    return "FAT12";
  case FAT16:
    return "FAT16";
  case FAT32:
    return "FAT32";
  default:
    return "none";
  }
}

static int fat_op_list(void *inst, const char *path) { return fat_list_op(M(inst), path); }
static int fat_op_stat(void *inst, const char *path, int *is_dir, uint32_t *size)
{
  return fat_stat_op(M(inst), path, is_dir, size);
}
static int fat_op_read_at(void *inst, const char *path, uint32_t off, void *buf, uint32_t len)
{
  return fat_read_at_op(M(inst), path, off, buf, len);
}
static int fat_op_enum_dir(void *inst, const char *path, int idx, char *name, int *is_dir, uint32_t *size)
{
  return fat_enum_dir_cap(inst, path, idx, name, FAT_ENUM_NAME_MAX, is_dir, size);
}
static int fat_op_create(void *inst, const char *path, const void *data, uint32_t size)
{
  MUTATE(M(inst), create_impl(M(inst), path, data, size));
}
static int fat_op_del(void *inst, const char *path) { MUTATE(M(inst), delete_impl(M(inst), path)); }
static int fat_op_mkdir(void *inst, const char *path) { MUTATE(M(inst), mkdir_impl(M(inst), path)); }
static int fat_op_rmdir(void *inst, const char *path) { MUTATE(M(inst), rmdir_impl(M(inst), path)); }
static int fat_op_mtime(void *inst, const char *path, uint32_t *mtime)
{
  struct fat_stat st;
  int r = fat_getattr(inst, path, &st);
  if (r == 0)
    *mtime = st.mtime;
  return r;
}

static const struct fs_ops fat_fs_ops = {
    .fsname = "FAT",
    .probe = fat_probe,
    .close = fat_close,
    .label = fat_op_label,
    .type = fat_op_type,
    .list = fat_op_list,
    .stat = fat_op_stat,
    .read_at = fat_op_read_at,
    .enum_dir = fat_op_enum_dir,
    .create = fat_op_create,
    .del = fat_op_del,
    .mkdir = fat_op_mkdir,
    .rmdir = fat_op_rmdir,
    .rename = fat_rename,
    .write_at = fat_write_at,
    .mtime = fat_op_mtime,
};

const struct fs_ops *fat_ops(void) { return &fat_fs_ops; }
