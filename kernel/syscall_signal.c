#include "syscall_signal.h"
#include "idt.h"
#include "sched.h"
#include "signal.h"
#include "uaccess.h"
#include <stdint.h>

void syscall_signal_action(struct registers *r)
{
  struct k_sigaction action, old;
  const struct k_sigaction *input = 0;
  if (r->ecx) {
    if (!user_range(r->ecx, sizeof action) || copyin(&action, r->ecx, sizeof action) != 0) {
      r->eax = (uint32_t)-1;
      return;
    }
    input = &action;
  }
  if (r->edx && !user_out_range(r->edx, sizeof old)) {
    r->eax = (uint32_t)-1;
    return;
  }
  int rc = sched_sigaction((int)r->ebx, input, r->edx ? &old : 0);
  if (rc == 0 && r->edx && copyout(r->edx, &old, sizeof old) != 0)
    rc = -1;
  r->eax = (uint32_t)rc;
}

void syscall_signal_procmask(struct registers *r)
{
  r->eax = sched_sigprocmask((int)r->ebx, (uint32_t)r->ecx, (int)r->edx);
}

void syscall_signal_return(struct registers *r) { sched_sigreturn(r); }
