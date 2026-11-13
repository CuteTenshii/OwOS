/* pkgdb.h: the record of who owns each installed system file, kept on disk at /var/lib/installed.
 *
 * Binary, little-endian. The 16-byte header holds "OWPK", a u16 format (1), a u16 zero, the u32 record count and
 * the u32 CRC-32C of every byte after the header. Each record holds a u32 crc, a u32 size, a u8 origin, a u8
 * version length and a u16 path length, then the version and path. Both strings are stored with their NUL and the
 * lengths count it, so parsed records point straight into the buffer.
 * origin is PKG_BASE for a file the kernel copied out of its own image and PKG_UPDATE for one an updater wrote; crc
 * (POSIX cksum) and size describe what was written, so deciding whether a file is current never reads it back.
 */
#ifndef OWOS_PKGDB_H
#define OWOS_PKGDB_H

#include <stdbool.h>
#include <stdint.h>

#define PKGDB_HEADER_SIZE 16u

enum pkg_origin { PKG_BASE, PKG_UPDATE };

struct pkg_rec {
  uint32_t crc, size;
  enum pkg_origin origin;
  const char *version, *path;
};

enum pkg_action { PKG_KEEP, PKG_INSTALL };

/* Validate buf (header, checksum, every record's bounds and strings) and fill up to max records pointing into it.
 * Returns the record count, or -1 if the manifest is damaged, truncated, of another format, or holds more than
 * max records. pkgdb_count gives the count to size recs with, or -1 under the same header checks. */
int pkgdb_count(const uint8_t *buf, uint32_t len);
int pkgdb_parse(const uint8_t *buf, uint32_t len, struct pkg_rec *recs, int max);

/* Bytes one record takes on disk. */
uint32_t pkgdb_record_size(const struct pkg_rec *r);

/* Build a manifest in out: init writes an empty one and returns its length (0 if cap is too small); append adds a
 * record, growing *len, or returns -1 and leaves out untouched when it does not fit or a string is too long. */
uint32_t pkgdb_init(uint8_t *out, uint32_t cap);
int pkgdb_append(uint8_t *out, uint32_t cap, uint32_t *len, const struct pkg_rec *r);

/* Sort by path so pkgdb_find can binary-search. */
void pkgdb_sort(struct pkg_rec *recs, int n);
const struct pkg_rec *pkgdb_find(const struct pkg_rec *recs, int n, const char *path);

/* Compare two `git describe` versions ("v1.2.3", "v1.2.3-4-gabcdef[-dirty]"): *cmp gets <0, 0 or >0.
 * False when either has no leading version number, so the two cannot be ordered. */
bool pkgdb_vercmp(const char *a, const char *b, int *cmp);

/* Whether the kernel should write its built-in copy (crc, size) of a file whose manifest record is rec (NULL if
 * none) and whose on-disk size is disk_size (-1 if absent). An update survives only while it is at least as new
 * as base_version, the running kernel's own version. */
enum pkg_action pkgdb_decide(const struct pkg_rec *rec, int disk_size, uint32_t crc, uint32_t size,
                             const char *base_version);

#endif /* OWOS_PKGDB_H */
