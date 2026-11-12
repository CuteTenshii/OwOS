#include "uaccess.h"

#include "irqflags.h"
#include "kstring.h"
#include "paging.h"

/* Check and copy run with interrupts off: a preemption between them drops the kernel lock, and a sibling thread could
 * unmap the range before the copy faults on it in ring 0. */

int user_range(uint32_t addr, uint32_t len) { return paging_user_range(addr, len, false); }

int user_out_range(uint32_t addr, uint32_t len) { return paging_user_range(addr, len, true); }

int copyin(void *dst, uint32_t src, uint32_t len)
{
  uintptr_t fl = irq_save();
  int ok = user_range(src, len);
  if (ok)
    memcpy(dst, (const void *)(uintptr_t)src, len);
  irq_restore(fl);
  return ok ? 0 : -1;
}

int copyout(uint32_t dst, const void *src, uint32_t len)
{
  uintptr_t fl = irq_save();
  int ok = user_out_range(dst, len);
  if (ok)
    memcpy((void *)(uintptr_t)dst, src, len);
  irq_restore(fl);
  return ok ? 0 : -1;
}

int copyin_str(uint32_t src, char *dst, uint32_t cap)
{
  uint32_t i = 0;
  while (i < cap) {
    uint32_t n = 4096u - ((src + i) & 0xFFFu);
    if (n > cap - i)
      n = cap - i;
    uintptr_t fl = irq_save();
    int ok = user_range(src + i, n);
    if (ok)
      memcpy(dst + i, (const void *)(uintptr_t)(src + i), n);
    irq_restore(fl);
    if (!ok)
      return -1;
    for (uint32_t j = 0; j < n; j++)
      if (dst[i + j] == '\0')
        return 0;
    i += n;
  }
  return -1;
}
