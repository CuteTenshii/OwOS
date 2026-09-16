#ifndef OWOS_SYSCALL_PATH_H
#define OWOS_SYSCALL_PATH_H

#include <stdint.h>

#define SYS_PATH_MAX 256

struct rtc_time;

void join_cwd(const char *name, char *out, int outsz);
int read_with_path(const char *name, void *buf, uint32_t max);
int resolve_path(const char *name, char *out, int outcap);
int read_at_with_path(const char *name, uint32_t off, void *buf, uint32_t len);
void epoch_to_rtc(uint32_t secs, struct rtc_time *t);

#endif
