#ifndef OWOS_SYSCALL_PATH_H
#define OWOS_SYSCALL_PATH_H

#include <stdint.h>

#define SYS_PATH_MAX 256

struct rtc_time;

/* Resolve `name` against `base` (ignored when name is absolute) to a canonical absolute path: no "//", "." or "..",
 * and ".." stops at "/". Permission checks and mount routing must see the same string, so every syscall path goes
 * through here. 0, or -1 if the result does not fit in outsz. */
int path_join(const char *base, const char *name, char *out, int outsz);
int join_cwd(const char *name, char *out, int outsz); /* path_join against the caller's cwd */
int read_with_path(const char *name, void *buf, uint32_t max);
int resolve_path(const char *name, char *out, int outcap);
int read_at_with_path(const char *name, uint32_t off, void *buf, uint32_t len);
void epoch_to_rtc(uint32_t secs, struct rtc_time *t);

#endif
