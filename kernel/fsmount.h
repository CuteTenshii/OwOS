/* fsmount.h — the multi-instance mounted-filesystem interface.
 *
 * Every removable filesystem driver (fat/hfs/ext/iso9660) exposes a `struct
 * fs_ops` whose calls take an opaque per-mount instance the driver allocates in
 * probe() and frees in close(). Because no driver keeps global mount state any
 * more, N volumes -- including two of the same type -- can be mounted at once.
 * fs.c keeps a mount table mapping a mount point to {ops, instance} and routes
 * every path to the owning mount (longest-prefix) or to owofs.
 */
#ifndef OWOS_FSMOUNT_H
#define OWOS_FSMOUNT_H

#include <stdint.h>

/* The backing block device, as seen by a mounted fs. LBA is in the device's own
 * sector size (512 for disks/USB/SD, 2048 for an ATAPI CD). */
typedef int (*blk_read_fn)(uint32_t lba, void *buf);
typedef int (*blk_write_fn)(uint32_t lba, const void *buf);

/* A driver's operations. Read-only filesystems leave the create/delete/mkdir/
 * rmdir pointers NULL. Every op takes the instance from probe(). Paths are
 * relative to the volume root ("" or "/" is the root, "/dir/file" a child). */
struct fs_ops {
  const char *fsname; /* "FAT"/"HFS+"/"ext"/"ISO9660"  */

  /* Probe the device for this filesystem. Returns a freshly allocated mount
   * instance on a match, or NULL if the device isn't this filesystem. `write`
   * is NULL for a read-only mount (or an unwritable medium like a CD). */
  void *(*probe)(blk_read_fn read, blk_write_fn write);
  void (*close)(void *inst); /* free the instance             */

  const char *(*label)(void *inst); /* volume label, or ""           */
  const char *(*type)(void *inst);  /* "FAT32"/"ext4"/... or "none"  */

  int (*list)(void *inst, const char *path);
  int (*stat)(void *inst, const char *path, int *is_dir, uint32_t *size);
  int (*read_at)(void *inst, const char *path, uint32_t off, void *buf, uint32_t len);
  int (*enum_dir)(void *inst, const char *path, int idx, char *name, int *is_dir, uint32_t *size);

  /* Write path -- NULL on a read-only driver. */
  int (*create)(void *inst, const char *path, const void *data, uint32_t size);
  int (*del)(void *inst, const char *path);
  int (*mkdir)(void *inst, const char *path);
  int (*rmdir)(void *inst, const char *path);
  int (*rename)(void *inst, const char *from, const char *to);
  int (*write_at)(void *inst, const char *path, uint32_t off, const void *buf, uint32_t len);

  /* Optional: modification time as Unix seconds. */
  int (*mtime)(void *inst, const char *path, uint32_t *mtime);
};

const struct fs_ops *fat_ops(void);
const struct fs_ops *hfs_ops(void);
const struct fs_ops *ext_ops(void);
const struct fs_ops *iso_ops(void);
const struct fs_ops *ntfs_ops(void);

#endif /* OWOS_FSMOUNT_H */
