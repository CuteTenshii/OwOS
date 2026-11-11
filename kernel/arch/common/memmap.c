/* memmap.c: subtract reserved ranges from a RAM range. */
#include "memmap.h"
#include <stdbool.h>

int memmap_subtract(uint64_t base, uint64_t size, const struct fdt_range *rsv, int n, struct fdt_range *out, int max)
{
  int count = 0;
  if (size && max > 0)
    out[count++] = (struct fdt_range){base, size};
  else if (size)
    return -1;

  for (int r = 0; r < n; r++) {
    uint64_t rs = rsv[r].base, re = rs + rsv[r].size;
    if (re < rs)
      re = UINT64_MAX;
    for (int i = 0; i < count; i++) {
      uint64_t ps = out[i].base, pe = ps + out[i].size;
      if (re <= ps || rs >= pe)
        continue;
      bool keep_low = rs > ps, keep_high = re < pe;
      if (keep_low && keep_high) {
        if (count == max)
          return -1;
        for (int j = count; j > i + 1; j--)
          out[j] = out[j - 1];
        count++;
        out[i + 1] = (struct fdt_range){re, pe - re};
        out[i].size = rs - ps;
        i++;
      } else if (keep_low) {
        out[i].size = rs - ps;
      } else if (keep_high) {
        out[i] = (struct fdt_range){re, pe - re};
      } else {
        for (int j = i; j < count - 1; j++)
          out[j] = out[j + 1];
        count--;
        i--;
      }
    }
  }
  return count;
}
