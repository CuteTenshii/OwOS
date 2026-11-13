/* install.c: bring the boot disk's system files in line with the payload embedded in the kernel. */
#include "install.h"
#include "console.h"
#include "fs.h"
#include "heap.h"
#include "kstring.h"
#include "pkgdb.h"

#ifndef OWOS_VERSION
#define OWOS_VERSION "dev"
#endif

#define PKGDB_FILE "/var/lib/installed"
#define DIR_MODE 0755

uint32_t install_payload_bytes(void)
{
  uint32_t total = 0;
  for (unsigned i = 0; i < payload_count; i++)
    total += (uint32_t)(payload[i].end - payload[i].start);
  return total;
}

/* The whole file in a fresh buffer, its length in *len; NULL if absent or unreadable. */
static uint8_t *read_file(const char *path, uint32_t *len)
{
  int sz = fs_size(path);
  uint8_t *buf = sz > 0 ? kmalloc((uint32_t)sz) : 0;
  if (!buf)
    return 0;
  int n = fs_read(path, buf, (uint32_t)sz);
  if (n != sz) {
    kfree(buf);
    return 0;
  }
  *len = (uint32_t)n;
  return buf;
}

static void make_parents(const char *path)
{
  char dir[128];
  uint32_t len = strlen(path);
  if (len >= sizeof dir)
    return;
  for (uint32_t i = 1; i < len; i++) {
    if (path[i] != '/')
      continue;
    memcpy(dir, path, i);
    dir[i] = '\0';
    if (fs_mkdir(dir) == 0)
      fs_chmod(dir, DIR_MODE);
  }
}

static int write_file(const char *path, const void *data, uint32_t len, uint32_t mode)
{
  make_parents(path);
  /* fs_create writes the new extent before it swaps the directory entry, so an interrupted write keeps the old copy. */
  if (fs_create(path, data, len) != 0)
    return -1;
  fs_chmod(path, mode);
  return 0;
}

static bool in_payload(const char *path)
{
  for (unsigned i = 0; i < payload_count; i++)
    if (streq(payload[i].path, path))
      return true;
  return false;
}

static void record(uint8_t *db, uint32_t cap, uint32_t *len, const struct pkg_rec *r)
{
  if (pkgdb_append(db, cap, len, r) != 0)
    con_printf("install: cannot record %s\n", r->path);
}

void install_payload(void)
{
  uint32_t old_len = 0;
  uint8_t *old = read_file(PKGDB_FILE, &old_len);
  struct pkg_rec *recs = 0;
  int nrec = old ? pkgdb_count(old, old_len) : -1;
  if (nrec > 0) {
    recs = kmalloc((uint32_t)nrec * (uint32_t)sizeof *recs);
    nrec = recs ? pkgdb_parse(old, old_len, recs, nrec) : -1;
  }
  if (nrec < 0)
    nrec = 0; /* missing or unreadable: no file has a known owner, so every one is rewritten */
  pkgdb_sort(recs, nrec);

  uint32_t cap = PKGDB_HEADER_SIZE;
  for (unsigned i = 0; i < payload_count; i++) {
    struct pkg_rec r = {0, 0, PKG_BASE, OWOS_VERSION, payload[i].path};
    cap += pkgdb_record_size(&r);
  }
  for (int i = 0; i < nrec; i++)
    cap += pkgdb_record_size(&recs[i]);
  uint8_t *db = kmalloc(cap);
  if (!db) {
    con_write("install: out of memory, system files not checked\n");
    kfree(recs);
    kfree(old);
    return;
  }
  uint32_t len = pkgdb_init(db, cap);

  uint32_t written = 0, failed = 0, removed = 0;
  for (unsigned i = 0; i < payload_count; i++) {
    const struct payload_file *f = &payload[i];
    uint32_t size = (uint32_t)(f->end - f->start);
    const struct pkg_rec *rec = pkgdb_find(recs, nrec, f->path);
    struct pkg_rec now = {f->crc, size, PKG_BASE, OWOS_VERSION, f->path};
    if (pkgdb_decide(rec, fs_size(f->path), f->crc, size, OWOS_VERSION) == PKG_KEEP) {
      record(db, cap, &len, rec->origin == PKG_UPDATE ? rec : &now);
    } else if (write_file(f->path, f->start, size, f->mode) == 0) {
      record(db, cap, &len, &now);
      written++;
    } else {
      /* Left out of the manifest, so the next boot tries again. */
      con_printf("install: could not write %s\n", f->path);
      failed++;
    }
  }

  for (int i = 0; i < nrec; i++) {
    if (in_payload(recs[i].path))
      continue;
    if (recs[i].origin == PKG_UPDATE)
      record(db, cap, &len, &recs[i]);
    else if (fs_delete(recs[i].path) == 0)
      removed++;
  }

  bool same = old && old_len == len && memcmp(old, db, len) == 0;
  if (!same && write_file(PKGDB_FILE, db, len, 0644) != 0)
    con_write("install: could not save " PKGDB_FILE "\n");
  con_printf("install: %u of %u system files written, %u removed", written, payload_count, removed);
  if (failed)
    con_printf(", %u failed", failed);
  con_putc('\n');

  kfree(db);
  kfree(recs);
  kfree(old);
}
