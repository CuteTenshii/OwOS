#include "syscall_entropy.h"
#include "entropy.h"
#include "regs.h"
#include "uaccess.h"
#include <stdint.h>

void syscall_entropy_getrandom(struct registers *r)
{
  uint32_t len = (uint32_t)SC_ARG1(r);
  if (len > 65536 || !user_out_range(SC_ARG0(r), len)) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  uint8_t buf[256];
  uint32_t done = 0;
  while (done < len) {
    uint32_t n = len - done;
    if (n > sizeof buf)
      n = sizeof buf;
    entropy_get(buf, n);
    if (copyout(SC_ARG0(r) + done, buf, n) != 0) {
      SC_RET(r) = (uint32_t)-1;
      return;
    }
    done += n;
  }
  SC_RET(r) = len;
}
