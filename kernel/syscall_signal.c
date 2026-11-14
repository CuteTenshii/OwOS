#include "syscall_signal.h"
#include "regs.h"
#include "sched.h"
#include "signal.h"
#include "uaccess.h"
#include <stdint.h>

void syscall_signal_action(struct registers *r)
{
  struct k_sigaction action, old;
  const struct k_sigaction *input = 0;
  if (SC_ARG1(r)) {
    if (!user_range(SC_ARG1(r), sizeof action) || copyin(&action, SC_ARG1(r), sizeof action) != 0) {
      SC_RET(r) = (uint32_t)-1;
      return;
    }
    input = &action;
  }
  if (SC_ARG2(r) && !user_out_range(SC_ARG2(r), sizeof old)) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  int rc = sched_sigaction((int)SC_ARG0(r), input, SC_ARG2(r) ? &old : 0);
  if (rc == 0 && SC_ARG2(r) && copyout(SC_ARG2(r), &old, sizeof old) != 0)
    rc = -1;
  SC_RET(r) = (uint32_t)rc;
}

void syscall_signal_procmask(struct registers *r)
{
  SC_RET(r) = sched_sigprocmask((int)SC_ARG0(r), (uint32_t)SC_ARG1(r), (int)SC_ARG2(r));
}

void syscall_signal_return(struct registers *r)
{
  if (sched_sigreturn(r) != 0)
    sched_exit(128 + SIGSEGV); /* no readable frame: nothing to resume */
}
