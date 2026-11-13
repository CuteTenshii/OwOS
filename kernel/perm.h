/* perm.h: pure Unix permission decisions, shared by the kernel (fs.c) and the host tests. */
#ifndef OWOS_PERM_H
#define OWOS_PERM_H

#include <stdbool.h>
#include <stdint.h>

#define PERM_R 4u
#define PERM_W 2u
#define PERM_X 1u

#define PERM_STICKY 01000u
#define PERM_MODE_MASK 01777u /* rwx for owner/group/other plus sticky */

#define PERM_EACCES 13

/* The rwx triplet (0..7) that applies to `uid`/`gid` on an object owned by fuid:fgid. */
static inline uint32_t perm_class(uint32_t mode, uint32_t fuid, uint32_t fgid, uint32_t uid, uint32_t gid)
{
  if (uid == fuid)
    return (mode >> 6) & 7u;
  if (gid == fgid)
    return (mode >> 3) & 7u;
  return mode & 7u;
}

/* May uid/gid access with `want` (PERM_R|W|X)? Root skips rwx but needs an x bit to exec. */
static inline bool perm_allows(uint32_t mode, uint32_t fuid, uint32_t fgid, uint32_t uid, uint32_t gid, uint32_t want,
                               bool is_dir)
{
  if (uid == 0) {
    if ((want & PERM_X) && !is_dir && !(mode & 0111u))
      return false;
    return true;
  }
  uint32_t have = perm_class(mode, fuid, fgid, uid, gid);
  return (have & want) == want;
}

/* Sticky dir: only the entry's owner, the directory's owner or root may remove/rename. */
static inline bool perm_sticky_allows(uint32_t dir_mode, uint32_t dir_uid, uint32_t entry_uid, uint32_t uid)
{
  if (!(dir_mode & PERM_STICKY))
    return true;
  return uid == 0 || uid == dir_uid || uid == entry_uid;
}

/* Apply a umask to a requested creation mode (0666 for files, 0777 for directories). */
static inline uint32_t perm_apply_umask(uint32_t requested, uint32_t umask) { return requested & ~umask & 0777u; }

/* May uid change the mode of an object owned by fuid? */
static inline bool perm_may_chmod(uint32_t fuid, uint32_t uid) { return uid == 0 || uid == fuid; }

/* chown: root sets anything; the owner may only change group to their own gid. */
static inline bool perm_may_chown(uint32_t fuid, uint32_t uid, uint32_t gid, uint32_t new_uid, uint32_t new_gid)
{
  if (uid == 0)
    return true;
  return uid == fuid && new_uid == fuid && new_gid == gid;
}

/* setuid/setgid: root may take any id; with no saved id to return to, anyone else may only keep their own. */
static inline bool perm_may_setid(uint32_t uid, uint32_t cur_id, uint32_t new_id)
{
  return uid == 0 || new_id == cur_id;
}

/* "drwxr-xr-x" style string; out needs 11 bytes. */
static inline void perm_mode_str(uint32_t mode, bool is_dir, char *out)
{
  out[0] = is_dir ? 'd' : '-';
  for (int i = 0; i < 3; i++) {
    uint32_t bits = (mode >> (6 - 3 * i)) & 7u;
    out[1 + 3 * i] = (bits & PERM_R) ? 'r' : '-';
    out[2 + 3 * i] = (bits & PERM_W) ? 'w' : '-';
    out[3 + 3 * i] = (bits & PERM_X) ? 'x' : '-';
  }
  if (mode & PERM_STICKY)
    out[9] = (mode & 1u) ? 't' : 'T';
  out[10] = '\0';
}

#endif
