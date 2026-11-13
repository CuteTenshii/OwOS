/* shm.c: shared memory objects (see shm.h). Each object is one contiguous PMM run; the big kernel lock serialises
 * every entry point, so the tables need no lock of their own. */
#include "shm.h"
#include "irqflags.h"
#include "loader.h"
#include "paging.h"
#include "pmm.h"
#include "sched.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SHM_MAX 256
#define SHM_MAPS_MAX 512
#define SHM_GRANTS 4
#define SHM_IDX_BITS 8 /* handle = gen << SHM_IDX_BITS | index, so a stale handle never names a reused slot */

struct shm_obj {
  bool used;
  bool owner_ref; /* the owner has not released it yet */
  uint32_t gen;
  uint32_t owner;
  uint32_t phys, bytes;
  int npages;
  int nmaps;
  int ngrants;
  uint32_t grants[SHM_GRANTS];
};

struct shm_mapping {
  bool used;
  int obj;
  uint32_t pid;
  uint32_t *dir;
  uint32_t at;
};

static struct shm_obj objs[SHM_MAX];
static struct shm_mapping maps[SHM_MAPS_MAX];

static int handle_of(int idx) { return (int)((objs[idx].gen << SHM_IDX_BITS) | (uint32_t)idx); }

static struct shm_obj *lookup(int h, int *idx_out)
{
  if (h <= 0)
    return 0;
  int idx = h & ((1 << SHM_IDX_BITS) - 1);
  struct shm_obj *o = &objs[idx];
  if (!o->used || (uint32_t)h >> SHM_IDX_BITS != o->gen)
    return 0;
  if (idx_out)
    *idx_out = idx;
  return o;
}

static bool may_map(const struct shm_obj *o, uint32_t pid)
{
  if (o->owner == pid && o->owner_ref)
    return true;
  for (int i = 0; i < o->ngrants; i++)
    if (o->grants[i] == pid)
      return true;
  return false;
}

static bool slot_ok(uint32_t at)
{
  if (at % SHM_SLOT_BYTES)
    return false;
  return (at >= SHM_LOW_BASE && at < SHM_LOW_END) || at >= SHM_HIGH_BASE;
}

static void zero_frames(uint32_t phys, int npages)
{
  for (int pg = 0; pg < npages; pg++) {
    uintptr_t fl = irq_save();
    uint32_t *v = paging_scratch_map(0, phys + (uint32_t)pg * 4096u);
    for (int k = 0; k < 1024; k++)
      v[k] = 0;
    paging_scratch_unmap(0);
    irq_restore(fl);
  }
}

/* Free the frames once nobody can reach them any more. */
static void maybe_free(struct shm_obj *o)
{
  if (o->owner_ref || o->nmaps > 0)
    return;
  pmm_free_contig(o->phys, o->bytes);
  o->used = false;
  o->phys = 0;
}

static void drop_mapping(struct shm_mapping *m)
{
  struct shm_obj *o = &objs[m->obj];
  paging_unmap_user_at(m->dir, m->at, SHM_SLOT_BYTES);
  m->used = false;
  o->nmaps--;
  maybe_free(o);
}

static struct shm_mapping *mapping_at(uint32_t *dir, uint32_t at)
{
  for (int i = 0; i < SHM_MAPS_MAX; i++)
    if (maps[i].used && maps[i].dir == dir && maps[i].at == at)
      return &maps[i];
  return 0;
}

int shm_create(uint32_t bytes)
{
  if (bytes == 0 || bytes > SHM_SLOT_BYTES)
    return -1;
  int idx = -1;
  for (int i = 0; i < SHM_MAX; i++)
    if (!objs[i].used) {
      idx = i;
      break;
    }
  if (idx < 0)
    return -1;
  int npages = (int)((bytes + 4095u) / 4096u);
  uint32_t run = (uint32_t)npages * 4096u;
  uint32_t phys = (uint32_t)pmm_alloc_contig(run);
  if (!phys)
    return -1;
  zero_frames(phys, npages); /* a recycled frame must never leak another process's pixels */

  struct shm_obj *o = &objs[idx];
  o->used = true;
  o->owner_ref = true;
  o->gen = (o->gen + 1) & 0x7FFFFFu;
  if (!o->gen)
    o->gen = 1;
  o->owner = sched_current_process_id();
  o->phys = phys;
  o->bytes = run;
  o->npages = npages;
  o->nmaps = 0;
  o->ngrants = 0;
  return handle_of(idx);
}

int shm_grant(int h, uint32_t pid)
{
  struct shm_obj *o = lookup(h, 0);
  if (!o || !o->owner_ref || o->owner != sched_current_process_id())
    return -1;
  for (int i = 0; i < o->ngrants; i++)
    if (o->grants[i] == pid)
      return 0;
  if (o->ngrants >= SHM_GRANTS)
    return -1;
  o->grants[o->ngrants++] = pid;
  return 0;
}

int shm_map(int h, uint32_t at)
{
  int idx;
  struct shm_obj *o = lookup(h, &idx);
  uint32_t pid = sched_current_process_id();
  if (!o || !may_map(o, pid) || !slot_ok(at))
    return -1;
  uint32_t *dir = sched_current_dir();
  struct shm_mapping *m = mapping_at(dir, at);
  if (m && m->obj == idx)
    return 0;
  struct shm_mapping *slot = m;
  if (!slot)
    for (int i = 0; i < SHM_MAPS_MAX && !slot; i++)
      if (!maps[i].used)
        slot = &maps[i];
  if (!slot)
    return -1;
  /* Pin the new object before the old one can be freed, in case both are the last refs of the same owner. */
  o->nmaps++;
  if (m)
    drop_mapping(m);
  if (paging_map_user_at(dir, at, o->phys, o->npages) < 0) {
    paging_unmap_user_at(dir, at, SHM_SLOT_BYTES);
    o->nmaps--;
    maybe_free(o);
    return -1;
  }
  slot->used = true;
  slot->obj = idx;
  slot->pid = pid;
  slot->dir = dir;
  slot->at = at;
  return 0;
}

int shm_unmap(uint32_t at)
{
  struct shm_mapping *m = mapping_at(sched_current_dir(), at);
  if (!m)
    return -1;
  drop_mapping(m);
  return 0;
}

int shm_release(int h)
{
  struct shm_obj *o = lookup(h, 0);
  if (!o || !o->owner_ref || o->owner != sched_current_process_id())
    return -1;
  o->owner_ref = false;
  o->ngrants = 0; /* no new mappings once released; existing ones keep the frames alive */
  maybe_free(o);
  return 0;
}

int shm_size(int h)
{
  struct shm_obj *o = lookup(h, 0);
  if (!o || !may_map(o, sched_current_process_id()))
    return -1;
  return (int)o->bytes;
}

void shm_process_exiting(uint32_t pid, uint32_t *dir)
{
  /* Unmap rather than forget: a sibling thread may still run until the leader's reap, and the frames may go now. */
  for (int i = 0; i < SHM_MAPS_MAX; i++)
    if (maps[i].used && maps[i].dir == dir)
      drop_mapping(&maps[i]);
  for (int i = 0; i < SHM_MAX; i++) {
    struct shm_obj *o = &objs[i];
    if (!o->used)
      continue;
    for (int g = 0; g < o->ngrants;) {
      if (o->grants[g] == pid)
        o->grants[g] = o->grants[--o->ngrants];
      else
        g++;
    }
    if (o->owner == pid && o->owner_ref) {
      o->owner_ref = false;
      o->ngrants = 0;
      maybe_free(o);
    }
  }
}
