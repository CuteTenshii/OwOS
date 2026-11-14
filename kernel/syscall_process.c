#include "syscall_process.h"
#include "irqflags.h"
#include "loader.h"
#include "paging.h"
#include "perm.h"
#include "power.h"
#include "regs.h"
#include "sched.h"
#include "syscall_nr.h"
#include "timer.h"
#include "uaccess.h"
#include "users.h"
#include <stdint.h>

#define WNOHANG 1

void syscall_process_exit(struct registers *r) { sched_exit((int)SC_ARG0(r)); }

void syscall_process_fork(struct registers *r) { SC_RET(r) = (uint32_t)sched_fork(r); }

void syscall_process_waitpid(struct registers *r)
{
  if ((int)SC_ARG2(r) & WNOHANG) {
    int status = 0;
    int got = sched_try_reap((int)SC_ARG0(r), &status);
    if (got && SC_ARG1(r) && copyout(SC_ARG1(r), &status, sizeof status) != 0)
      got = -1;
    SC_RET(r) = (uint32_t)got;
  } else {
    SC_RET(r) = (uint32_t)sched_waitpid((int)SC_ARG0(r));
  }
}

void syscall_process_msec(struct registers *r) { SC_RET(r) = timer_us() / 1000u; }

/* ebx = word address, ecx = expected value, edx = timeout ms (0 = forever). */
void syscall_process_futex_wait(struct registers *r)
{
  if ((SC_ARG0(r) & 3) || !user_range(SC_ARG0(r), 4) || !paging_user_range(SC_ARG0(r), 4, false)) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  SC_RET(r) = sched_futex_wait(SC_ARG0(r), (uint32_t)SC_ARG1(r), (uint32_t)SC_ARG2(r));
}

/* ebx = word address, ecx = max waiters to wake. */
void syscall_process_futex_wake(struct registers *r)
{
  if ((SC_ARG0(r) & 3) || !user_range(SC_ARG0(r), 4)) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  SC_RET(r) = (uint32_t)sched_futex_wake(SC_ARG0(r), (int)SC_ARG1(r));
}

void syscall_process_getuid(struct registers *r) { SC_RET(r) = sched_uid(); }

void syscall_process_setuid(struct registers *r)
{
  if (!perm_may_setid(sched_uid(), sched_uid(), (uint32_t)SC_ARG0(r))) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  sched_set_process_uid((uint32_t)SC_ARG0(r));
  SC_RET(r) = 0;
}

void syscall_process_setgid(struct registers *r)
{
  if (!perm_may_setid(sched_uid(), sched_gid(), (uint32_t)SC_ARG0(r))) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  sched_set_process_gid((uint32_t)SC_ARG0(r));
  SC_RET(r) = 0;
}

void syscall_process_thread(struct registers *r)
{
  if (!user_range(SC_ARG0(r), 1) || !user_range(SC_ARG1(r), 4)) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  if (SC_ARG2(r) && !user_range(SC_ARG2(r), 4)) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  SC_RET(r) = (uint32_t)sched_create_user_thread(SC_ARG0(r), SC_ARG1(r), SC_ARG2(r));
}

void syscall_process_set_tls(struct registers *r)
{
  if (SC_ARG0(r) && !user_range(SC_ARG0(r), 4)) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  sched_set_tls(SC_ARG0(r));
  SC_RET(r) = 0;
}

void syscall_process_sleep(struct registers *r)
{
  irq_enable();
  timer_sleep((uint32_t)SC_ARG0(r));
  SC_RET(r) = 0;
}

void syscall_process_kill(struct registers *r)
{
  SC_RET(r) = (uint32_t)sched_signal_send((int)SC_ARG0(r), (int)SC_ARG1(r));
}

void syscall_process_power(struct registers *r)
{
  if ((uint32_t)SC_ARG0(r) == 3) {
    SC_RET(r) = power_can_sleep();
    return;
  }
  if (sched_uid() != 0 && !users_sudo_valid(sched_uid())) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  if ((uint32_t)SC_ARG0(r) == 2) {
    power_sleep();
    SC_RET(r) = 0;
    return;
  }
  if ((uint32_t)SC_ARG0(r) == 1)
    power_reset();
  else
    power_off();
  SC_RET(r) = (uint32_t)-1;
}

void syscall_process_sbrk(struct registers *r) { SC_RET(r) = sched_sbrk((int)SC_ARG0(r)); }

void syscall_process_getpid(struct registers *r) { SC_RET(r) = sched_current_id(); }

void syscall_process_thread_name(struct registers *r)
{
  char name[THREAD_NAME_MAX * 2]; /* longer than a label holds, so a long name is truncated rather than refused */
  if ((uint32_t)SC_ARG0(r) == THREAD_NAME_SET) {
    if (!SC_ARG1(r)) {
      sched_set_thread_name("");
    } else if (copyin_str(SC_ARG1(r), name, sizeof name) < 0) {
      SC_RET(r) = (uint32_t)-1; /* unreadable, or no terminator within 64 bytes */
      return;
    } else {
      sched_set_thread_name(name);
    }
    SC_RET(r) = 0;
    return;
  }
  if ((uint32_t)SC_ARG0(r) == THREAD_NAME_GET && (int)SC_ARG2(r) > 0) {
    const char *t = sched_current_thread_name();
    uint32_t cap = (uint32_t)SC_ARG2(r) < THREAD_NAME_MAX ? (uint32_t)SC_ARG2(r) : THREAD_NAME_MAX, n = 0;
    for (; t[n] && n < cap - 1; n++)
      name[n] = t[n];
    name[n] = '\0';
    if (user_out_range(SC_ARG1(r), n + 1) && copyout(SC_ARG1(r), name, n + 1) == 0) {
      SC_RET(r) = 0;
      return;
    }
  }
  SC_RET(r) = (uint32_t)-1;
}

void syscall_process_sigprocmask(struct registers *r)
{
  SC_RET(r) = sched_sigprocmask((int)SC_ARG0(r), (uint32_t)SC_ARG1(r), (int)SC_ARG2(r));
}

void syscall_process_stack_alloc(struct registers *r) { SC_RET(r) = user_stack_alloc((int)SC_ARG0(r)); }

void syscall_process_stack_free(struct registers *r)
{
  user_stack_free(SC_ARG0(r));
  SC_RET(r) = 0;
}
