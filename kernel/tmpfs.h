/* tmpfs.h: the volatile filesystem mounted at /tmp.
 *
 * Files and directories live in the kernel heap and vanish on reboot. Each node keeps the creator's uid, gid and
 * umask-reduced mode; the root is 01777 so /tmp gets sticky-directory semantics. An instance may hold at most a
 * quarter of the heap that was free when it was created, so filling /tmp cannot starve the kernel.
 */
#ifndef OWOS_TMPFS_H
#define OWOS_TMPFS_H

#include "fsmount.h"

const struct fs_ops *tmpfs_ops(void);

/* A fresh instance holding only the root directory, or NULL when the heap is exhausted. */
void *tmpfs_new(void);

#endif /* OWOS_TMPFS_H */
