/* fs.h — owofs: a deliberately tiny on-disk filesystem.
 *
 * Layout (512-byte sectors on the primary ATA disk):
 *   sector 0  superblock (magic + next free data sector)
 *   sector 1+ directory: 64 fixed entries (a 4-sector table)
 *   data      file data, contiguous, bump-allocated, past the directory
 *
 * Free space is tracked by an in-RAM sector bitmap rebuilt from the directory
 * at mount, so deleting or overwriting a file reclaims its sectors (files are
 * contiguous extents, so allocation hands out a contiguous run). The usable
 * size follows the backing disk's own capacity (blk_sectors), capped in fs.c.
 *
 * Files are contiguous, so a file of known size can be pre-allocated
 * (fs_prealloc) and then filled in arbitrary order (fs_write_at) -- random
 * sector math, no growth -- which is what an out-of-order torrent download
 * needs.
 */
#ifndef OWOS_FS_H
#define OWOS_FS_H

#include <stdint.h>

void fs_init(void); /* mount the disk; format it if unformatted          */
void fs_format(void);

/* owofs is now hierarchical: the 64 directory slots form a tree via a parent
 * pointer in each entry, with an implicit root ("/"). Paths may be absolute
 * ("/bin/hello") or relative to the root ("hello"); "." and ".." are honored.
 * Component names are still <= FS_NAME_LEN-1 chars. There is no per-process
 * cwd in the fs layer -- callers (the shell) build absolute paths. */

void fs_list(const char *path); /* print directory `path` (NULL/"" = root)   */

/* Create directory `path`. Returns 0, or -1 (bad path / exists / dir full). */
int fs_mkdir(const char *path);

/* Remove an *empty* directory `path`. Returns 0; -1 not found, -2 not a
 * directory, -3 not empty. */
int fs_rmdir(const char *path);

/* 1 if `path` is a directory (root counts), 0 if it is a file, -1 if absent. */
int fs_isdir(const char *path);

/* Returns bytes read into buf (capped at maxlen), or -1 if not found. */
int fs_read(const char *name, void *buf, uint32_t maxlen);

/* Size of a file in bytes, or -1 if it does not exist. */
int fs_size(const char *name);

/* Read up to len bytes from byte `offset` in a file (for streaming large
 * files). Returns bytes read (0 at EOF), or -1 if the file is not found. */
int fs_read_at(const char *name, uint32_t offset, void *buf, uint32_t len);

/* Create or overwrite a file. Returns 0 on success, -1 on failure. */
int fs_create(const char *name, const void *data, uint32_t size);

/* Remove a file's directory entry. Its data sectors are leaked (owofs never
 * reclaims; see the header note), but the name frees up and the slot can be
 * reused. Refuses directories (use fs_rmdir). Returns 0 on success, -1 if the
 * file does not exist, -2 if `name` is a directory. */
int fs_delete(const char *name);

/* Rename/move `old` to `neu` -- no data copy, just the directory entry, so it
 * can also move a file or directory to a different parent. Overwrites an
 * existing file at `neu` (leaking its sectors); refuses to clobber a directory
 * or to move a directory into its own subtree. Returns 0 on success, -1 on
 * failure (absent source, bad/empty target, or an illegal move). */
int fs_rename(const char *old, const char *neu);

/* Pre-allocate a file of exactly `size` bytes: reserve its contiguous sectors,
 * zero them, and commit the directory entry now. The caller can then fill the
 * file in any order with fs_write_at -- e.g. a torrent writing pieces as they
 * arrive out of order. Overwrites an existing file of the same name. Returns 0
 * on success, -1 on failure (disk/dir full). */
int fs_prealloc(const char *name, uint32_t size);

/* Write len bytes at byte `offset` into an existing file, doing a
 * read-modify-write at partial-sector edges. The write must lie within the
 * file's current size (owofs files don't grow). Returns bytes written, or -1
 * on failure (file absent, range past EOF, or I/O error). */
int fs_write_at(const char *name, uint32_t offset, const void *data, uint32_t len);

/* Self-test fs_prealloc + fs_write_at (out-of-order writes, partial/cross
 * sector edges, zero-fill, bounds). Prints PASS/FAIL; returns 0 iff all pass.
 * Wired to the `fstest` shell command. */
int fs_selftest(void);

/* Streaming creation, for data of unknown final size (e.g. a download). Call
 * begin, feed bytes with one or more fs_stream_write calls, then end. Only one
 * stream may be active at a time. begin/write return 0 on success, -1 on
 * failure; end returns the bytes written, or -1 on failure. */
int fs_stream_begin(const char *name);
int fs_stream_write(const void *data, uint32_t size);
int fs_stream_end(void);

int fs_file_count(void);      /* number of files                          */
uint32_t fs_used_bytes(void); /* total bytes across all files             */

/* Enumerate directory slots. For slot `idx` (0..15): if it holds a root-level
 * file (directories and nested files are skipped), copy its NUL-terminated name
 * into name[] (at least 20 bytes), store its size and packed mtime, and return
 * 1; otherwise return 0. Lets a caller (e.g. the SMB server) serve a flat share
 * of the root without knowing the on-disk layout. */
int fs_enum(int idx, char *name, uint32_t *size, uint32_t *mtime);

/* Stat a path. On success fills (if non-NULL) *is_dir, *size, *mtime and
 * returns 0; returns -1 if the path does not exist. The root ("" or "/") is a
 * directory of size 0. Lets the SMB server resolve a full path without the
 * layout. */
int fs_stat(const char *path, int *is_dir, uint32_t *size, uint32_t *mtime);

/* stat plus owner and mode; pseudo-fs report fixed modes, other volumes a mount default. */
struct fs_stat_info {
  int is_dir;
  uint32_t size, mtime, mode, uid, gid;
};
int fs_stat_ex(const char *path, struct fs_stat_info *st);

/* Set mode/owner on an owofs entry, unchecked. 0, or -1 if absent, root or not owofs. */
int fs_chmod(const char *path, uint32_t mode);
int fs_chown(const char *path, uint32_t uid, uint32_t gid);

/* Permission check for the calling task: 0, -13 (EACCES), or -1 if a component is absent. */
#define FS_R 4
#define FS_W 2
#define FS_X 1
#define FS_ACC_ENTRY 0x10
#define FS_ACC_CREATE 0x20
int fs_access(const char *path, int want);

/* Enumerate the children of directory `path` (NULL/""/"/" = root). For the
 * idx-th child (0-based, slot order) fill name (short component, >= 18 bytes),
 * *is_dir, *size, *mtime and return 1; return 0 past the last child or if
 * `path` is not a directory. */
int fs_enum_dir(const char *path, int idx, char *name, int *is_dir, uint32_t *size, uint32_t *mtime);

/* ---- removable mounts (the mount table) -----------------------------------
 */

/* Mount the block device `dev` (a /dev name like "usb0"/"cd0") at mount point
 * `point` (must be "/media/<name>"). fs.c resolves the device's sector I/O via
 * blk_dev_io and probes the filesystem drivers. Returns 0, or -1 (unknown
 * device, no recognised filesystem, point already mounted, or table full). */
int fs_mount(const char *dev, const char *point);

/* Unmount the volume at `point`. Returns 0, or -1 if nothing is mounted there.
 */
int fs_unmount(const char *point);

/* Enumerate active mounts. For the idx-th mount fill *out and return 1; return
 * 0 past the last. `type`/`label` point at driver-owned strings (valid until
 * the volume is unmounted). */
struct fs_mountinfo {
  char point[48];
  char dev[16];
  const char *type;
  const char *label;
};
int fs_mount_enum(int idx, struct fs_mountinfo *out);

#endif /* OWOS_FS_H */
