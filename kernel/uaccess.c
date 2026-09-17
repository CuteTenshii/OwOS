#include "uaccess.h"

#include "kstring.h"
#include "paging.h"

int user_range(uint32_t addr, uint32_t len) { return paging_user_range(addr, len, 0); }

int user_out_range(uint32_t addr, uint32_t len) { return paging_user_range(addr, len, 1); }

int copyin(void *dst, uint32_t src, uint32_t len)
{
  if (!user_range(src, len))
    return -1;
  memcpy(dst, (const void *)(uintptr_t)src, len);
  return 0;
}

int copyout(uint32_t dst, const void *src, uint32_t len)
{
  if (!user_out_range(dst, len))
    return -1;
  memcpy((void *)(uintptr_t)dst, src, len);
  return 0;
}

int copyin_str(uint32_t src, char *dst, uint32_t cap)
{
  uint32_t i = 0;
  while (i < cap) {
    uint32_t n = 4096u - ((src + i) & 0xFFFu);
    if (n > cap - i)
      n = cap - i;
    if (!user_range(src + i, n))
      return -1;
    memcpy(dst + i, (const void *)(uintptr_t)(src + i), n);
    for (uint32_t j = 0; j < n; j++)
      if (dst[i + j] == '\0')
        return 0;
    i += n;
  }
  return -1;
}
