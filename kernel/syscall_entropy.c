#include "syscall_entropy.h"
#include "entropy.h"
#include "idt.h"
#include "uaccess.h"
#include <stdint.h>

void syscall_entropy_getrandom(struct registers *r)
{
  uint32_t len = r->ecx;
  if (len > 65536 || !user_out_range(r->ebx, len)) {
    r->eax = (uint32_t)-1;
    return;
  }
  uint8_t buf[256];
  uint32_t done = 0;
  while (done < len) {
    uint32_t n = len - done;
    if (n > sizeof buf)
      n = sizeof buf;
    entropy_get(buf, n);
    if (copyout(r->ebx + done, buf, n) != 0) {
      r->eax = (uint32_t)-1;
      return;
    }
    done += n;
  }
  r->eax = len;
}
