/* shm.h: page-backed shared memory objects, granted to specific processes.
 *
 * The creating process owns an object and may grant it to other pids; the owner and its grantees map it at a
 * slot-aligned address inside one of the shm regions (SHM_* in common/uabi.h). An object's frames are freed once the
 * owner has released it and no mapping of it remains, so a grantee keeps its view alive across the owner's
 * release. New objects are zero-filled.
 */
#ifndef OWOS_SHM_H
#define OWOS_SHM_H

#include "paging.h"
#include "uaccess.h"
#include <stdint.h>

/* Create an object of `bytes` (rounded up to pages, at most SHM_SLOT_SIZE) owned by the calling process. Returns a
 * positive handle or -1. */
int shm_create(uint32_t bytes);

/* Let process `pid` map handle `h`. Owner only; 0 / -1. */
int shm_grant(int h, uint32_t pid);

/* Map handle `h` at `at` in the caller's address space, replacing any shm mapping already there. The caller must
 * own the object or hold a grant; `at` must be the start of a slot in an shm region. 0 / -1. */
int shm_map(int h, uaddr_t at);

/* Drop the caller's shm mapping at `at`. 0 / -1. */
int shm_unmap(uaddr_t at);

/* The owner gives up its reference; the frames go once the last mapping does. 0 / -1. */
int shm_release(int h);

/* Bytes in handle `h` if the caller owns it or may map it, else -1. */
int shm_size(int h);

/* Process `pid` (address space `dir`) is exiting: forget its mappings and release what it owns. */
void shm_process_exiting(uint32_t pid, pgdir_t *dir);

#endif /* OWOS_SHM_H */
