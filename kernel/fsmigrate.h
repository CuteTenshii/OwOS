/* fsmigrate.h: owofs entry layouts and the pure v8 -> v9 conversion (host-tested). */
#ifndef OWOS_FSMIGRATE_H
#define OWOS_FSMIGRATE_H

#include <stdbool.h>
#include <stdint.h>

#define FSM_SECTOR_SIZE 512u
#define FSM_NAME_LEN 64
#define FSM_ROOT 0xFFFFu
#define FSM_DIR_FLAG 0x01u

#define FSM_MAGIC_V8 0x4F574F38u /* "OWO8": entries without owner/mode */
#define FSM_MAGIC_V9 0x4F574F39u /* "OWO9": entries with mode/uid/gid */

/* v8 entry, 80 bytes. */
struct owofs_dirent_v8 {
  char name[FSM_NAME_LEN];
  uint8_t flags;
  uint8_t reserved;
  uint16_t parent; /* containing dir slot, or FSM_ROOT */
  uint32_t size;
  uint32_t start;
  uint32_t mtime; /* packed wall-clock time (see pack_now)   */
};

/* v9 entry, 88 bytes. */
struct owofs_dirent {
  char name[FSM_NAME_LEN];
  uint8_t flags;
  uint8_t reserved;
  uint16_t parent;
  uint32_t size;
  uint32_t start;
  uint32_t mtime;
  uint16_t mode; /* rwx for owner/group/other plus sticky */
  uint16_t uid;
  uint16_t gid;
  uint16_t pad;
};

static inline uint32_t fsm_entries_v8(uint32_t sectors)
{
  return sectors * FSM_SECTOR_SIZE / (uint32_t)sizeof(struct owofs_dirent_v8);
}

static inline uint32_t fsm_entries_v9(uint32_t sectors)
{
  return sectors * FSM_SECTOR_SIZE / (uint32_t)sizeof(struct owofs_dirent);
}

/* Sectors a v9 extent needs to hold every slot of a v8 extent of `old_sectors`. */
static inline uint32_t fsm_v9_sectors(uint32_t old_sectors)
{
  uint64_t bytes = (uint64_t)fsm_entries_v8(old_sectors) * sizeof(struct owofs_dirent);
  return (uint32_t)((bytes + FSM_SECTOR_SIZE - 1) / FSM_SECTOR_SIZE);
}

static inline bool fsm_name_is(const char *name, const char *want)
{
  for (int i = 0; i < FSM_NAME_LEN; i++) {
    if (name[i] != want[i])
      return false;
    if (!name[i])
      return true;
  }
  return true;
}

/* Top-level ancestor slot of entry i (i if parented at root), or -1 on a broken chain. */
static inline int fsm_top_slot(const struct owofs_dirent_v8 *old, uint32_t n, uint32_t i)
{
  for (uint32_t hops = 0; hops <= n; hops++) {
    if (old[i].parent == FSM_ROOT)
      return (int)i;
    if (old[i].parent >= n || !old[old[i].parent].name[0])
      return -1;
    i = old[i].parent;
  }
  return -1;
}

/* Migrated owner/mode: root:root, dirs 0755, files 0644, /bin 0755, /etc/passwd 0600. */
static inline uint16_t fsm_default_mode(const struct owofs_dirent_v8 *old, uint32_t n, uint32_t i)
{
  if (old[i].flags & FSM_DIR_FLAG)
    return 0755;
  int top = fsm_top_slot(old, n, i);
  if (top < 0 || !(old[top].flags & FSM_DIR_FLAG))
    return 0644;
  if (fsm_name_is(old[top].name, "bin"))
    return 0755;
  if (fsm_name_is(old[top].name, "etc") && old[i].parent == (uint16_t)top && fsm_name_is(old[i].name, "passwd"))
    return 0600;
  return 0644;
}

/* Convert n_old v8 slots into out[0..n_out) with root ownership and default modes. */
static inline void fsm_convert(const struct owofs_dirent_v8 *old, uint32_t n_old, struct owofs_dirent *out,
                               uint32_t n_out)
{
  uint8_t *z = (uint8_t *)out;
  for (uint32_t b = 0; b < n_out * (uint32_t)sizeof *out; b++)
    z[b] = 0;
  for (uint32_t i = 0; i < n_old; i++) {
    if (!old[i].name[0])
      continue;
    for (int k = 0; k < FSM_NAME_LEN; k++)
      out[i].name[k] = old[i].name[k];
    out[i].flags = old[i].flags;
    out[i].parent = old[i].parent;
    out[i].size = old[i].size;
    out[i].start = old[i].start;
    out[i].mtime = old[i].mtime;
    out[i].mode = fsm_default_mode(old, n_old, i);
  }
}

#endif
