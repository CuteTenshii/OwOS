#ifndef OWOS_UACCESS_H
#define OWOS_UACCESS_H

#include <stdint.h>

int user_range(uint32_t addr, uint32_t len);
int user_out_range(uint32_t addr, uint32_t len);
int copyin(void *dst, uint32_t src, uint32_t len);
int copyout(uint32_t dst, const void *src, uint32_t len);
int copyin_str(uint32_t src, char *dst, uint32_t cap);

#endif
