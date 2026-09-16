#include "uaccess.h"

#include "paging.h"

int user_range(uint32_t addr, uint32_t len) { return paging_user_range(addr, len, 0); }

int user_out_range(uint32_t addr, uint32_t len) { return paging_user_range(addr, len, 1); }

int copyin(void *dst, uint32_t src, uint32_t len)
{
  if (!user_range(src, len))
    return -1;
  for (uint32_t i = 0; i < len; i++)
    ((uint8_t *)dst)[i] = ((const uint8_t *)(uintptr_t)src)[i];
  return 0;
}

int copyout(uint32_t dst, const void *src, uint32_t len)
{
  if (!user_out_range(dst, len))
    return -1;
  for (uint32_t i = 0; i < len; i++)
    ((uint8_t *)(uintptr_t)dst)[i] = ((const uint8_t *)src)[i];
  return 0;
}

int copyin_str(uint32_t src, char *dst, uint32_t cap)
{
  for (uint32_t i = 0; i < cap; i++) {
    if (copyin(&dst[i], src + i, 1) != 0)
      return -1;
    if (dst[i] == '\0')
      return 0;
  }
  return -1;
}
