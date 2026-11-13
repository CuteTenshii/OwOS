/* osinstall.h: install OwOS onto a disk; the kernel side of SYS_INSTALL (common/diskinstall.h). */
#ifndef OWOS_OSINSTALL_H
#define OWOS_OSINSTALL_H

#include "diskinstall.h"

/* Fill *out with the index-th disk OwOS could be installed on, and why not if it can't. 1, or 0 past the end. */
int osinstall_disk(int index, struct install_disk *out);

/* Begin installing onto `dev`, erasing it. INSTALL_OK, or the problem refusing it. */
enum install_problem osinstall_start(const char *dev);

/* Do the next bounded chunk of the running install and describe where it stands. 0 running, 1 finished, -1 failed
 * (or nothing running). */
int osinstall_step(struct install_progress *out);

#endif /* OWOS_OSINSTALL_H */
