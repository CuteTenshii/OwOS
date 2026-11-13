#include "syscall_process.h"
#include "idt.h"
#include "loader.h"
#include "paging.h"
#include "perm.h"
#include "power.h"
#include "sched.h"
#include "syscall_nr.h"
#include "timer.h"
#include "uaccess.h"
#include "users.h"
#include <stdint.h>

#define WNOHANG 1

void syscall_process_exit(struct registers *r) { sched_exit((int)r->ebx); }

void syscall_process_fork(struct registers *r) { r->eax = (uint32_t)sched_fork(r); }

void syscall_process_waitpid(struct registers *r)
{
  if ((int)r->edx & WNOHANG) {
    int status = 0;
    int got = sched_try_reap((int)r->ebx, &status);
    if (got && r->ecx && copyout(r->ecx, &status, sizeof status) != 0)
      got = -1;
    r->eax = (uint32_t)got;
  } else {
    r->eax = (uint32_t)sched_waitpid((int)r->ebx);
  }
}

void syscall_process_msec(struct registers *r) { r->eax = timer_us() / 1000u; }

/* ebx = word address, ecx = expected value, edx = timeout ms (0 = forever). */
void syscall_process_futex_wait(struct registers *r)
{
  if ((r->ebx & 3) || !user_range(r->ebx, 4) || !paging_user_range(r->ebx, 4, false)) {
    r->eax = (uint32_t)-1;
    return;
  }
  r->eax = sched_futex_wait(r->ebx, r->ecx, r->edx);
}

/* ebx = word address, ecx = max waiters to wake. */
void syscall_process_futex_wake(struct registers *r)
{
  if ((r->ebx & 3) || !user_range(r->ebx, 4)) {
    r->eax = (uint32_t)-1;
    return;
  }
  r->eax = (uint32_t)sched_futex_wake(r->ebx, (int)r->ecx);
}

void syscall_process_getuid(struct registers *r) { r->eax = sched_uid(); }

void syscall_process_setuid(struct registers *r)
{
  if (!perm_may_setid(sched_uid(), sched_uid(), r->ebx)) {
    r->eax = (uint32_t)-1;
    return;
  }
  sched_set_process_uid(r->ebx);
  r->eax = 0;
}

void syscall_process_setgid(struct registers *r)
{
  if (!perm_may_setid(sched_uid(), sched_gid(), r->ebx)) {
    r->eax = (uint32_t)-1;
    return;
  }
  sched_set_process_gid(r->ebx);
  r->eax = 0;
}

void syscall_process_thread(struct registers *r)
{
  if (!user_range(r->ebx, 1) || !user_range(r->ecx, 4)) {
    r->eax = (uint32_t)-1;
    return;
  }
  if (r->edx && !user_range(r->edx, 4)) {
    r->eax = (uint32_t)-1;
    return;
  }
  r->eax = (uint32_t)sched_create_user_thread(r->ebx, r->ecx, r->edx);
}

void syscall_process_set_tls(struct registers *r)
{
  if (r->ebx && !user_range(r->ebx, 4)) {
    r->eax = (uint32_t)-1;
    return;
  }
  sched_set_tls(r->ebx);
  r->eax = 0;
}

void syscall_process_sleep(struct registers *r)
{
  __asm__ volatile("sti");
  timer_sleep(r->ebx);
  r->eax = 0;
}

void syscall_process_kill(struct registers *r) { r->eax = (uint32_t)sched_signal_send((int)r->ebx, (int)r->ecx); }

void syscall_process_power(struct registers *r)
{
  if (r->ebx == 3) {
    r->eax = power_can_sleep();
    return;
  }
  if (sched_uid() != 0 && !users_sudo_valid(sched_uid())) {
    r->eax = (uint32_t)-1;
    return;
  }
  if (r->ebx == 2) {
    power_sleep();
    r->eax = 0;
    return;
  }
  if (r->ebx == 1)
    power_reset();
  else
    power_off();
  r->eax = (uint32_t)-1;
}

void syscall_process_sbrk(struct registers *r) { r->eax = sched_sbrk((int)r->ebx); }

void syscall_process_getpid(struct registers *r) { r->eax = sched_current_id(); }

void syscall_process_thread_name(struct registers *r)
{
  char name[THREAD_NAME_MAX * 2]; /* longer than a label holds, so a long name is truncated rather than refused */
  if (r->ebx == THREAD_NAME_SET) {
    if (!r->ecx) {
      sched_set_thread_name("");
    } else if (copyin_str(r->ecx, name, sizeof name) < 0) {
      r->eax = (uint32_t)-1; /* unreadable, or no terminator within 64 bytes */
      return;
    } else {
      sched_set_thread_name(name);
    }
    r->eax = 0;
    return;
  }
  if (r->ebx == THREAD_NAME_GET && r->edx > 0) {
    const char *t = sched_current_thread_name();
    uint32_t cap = r->edx < THREAD_NAME_MAX ? r->edx : THREAD_NAME_MAX, n = 0;
    for (; t[n] && n < cap - 1; n++)
      name[n] = t[n];
    name[n] = '\0';
    if (user_out_range(r->ecx, n + 1) && copyout(r->ecx, name, n + 1) == 0) {
      r->eax = 0;
      return;
    }
  }
  r->eax = (uint32_t)-1;
}

void syscall_process_sigprocmask(struct registers *r)
{
  r->eax = sched_sigprocmask((int)r->ebx, (uint32_t)r->ecx, (int)r->edx);
}

void syscall_process_stack_alloc(struct registers *r) { r->eax = user_stack_alloc((int)r->ebx); }

void syscall_process_stack_free(struct registers *r)
{
  user_stack_free(r->ebx);
  r->eax = 0;
}
