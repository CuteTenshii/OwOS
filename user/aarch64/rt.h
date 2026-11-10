/* rt.h: minimal user runtime for AArch64: raw syscalls (numbers from common/syscall_nr.h) and a few helpers. */
#ifndef OWOS_USER_AARCH64_RT_H
#define OWOS_USER_AARCH64_RT_H

#include "syscall_nr.h"
#include <stddef.h>
#include <stdint.h>

static inline long syscall3(long n, long a, long b, long c)
{
  register long x8 __asm__("x8") = n;
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
  return x0;
}

static inline long write(int fd, const void *buf, size_t len) { return syscall3(SYS_WRITE, fd, (long)buf, (long)len); }
static inline void exit(int code)
{
  syscall3(SYS_EXIT, code, 0, 0);
  for (;;)
    ;
}
static inline int getpid(void) { return (int)syscall3(SYS_GETPID, 0, 0, 0); }
static inline void sleep_ms(unsigned ms) { syscall3(SYS_SLEEP, ms, 0, 0); }
static inline uint64_t msec(void) { return (uint64_t)syscall3(SYS_MSEC, 0, 0, 0); }

size_t strlen(const char *s);
void puts(const char *s);
void put_u64(uint64_t v);

#endif
