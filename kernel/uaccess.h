#ifndef OWOS_UACCESS_H
#define OWOS_UACCESS_H

#include "uabi.h"
#include <stdint.h>

/* A user virtual address: a syscall word wide (common/uabi.h), so 32 bits on x86 and 64 on AArch64. */
typedef sys_word uaddr_t;

int user_range(uaddr_t addr, uint32_t len);
int user_out_range(uaddr_t addr, uint32_t len);
int copyin(void *dst, uaddr_t src, uint32_t len);
int copyout(uaddr_t dst, const void *src, uint32_t len);
int copyin_str(uaddr_t src, char *dst, uint32_t cap);

#endif
