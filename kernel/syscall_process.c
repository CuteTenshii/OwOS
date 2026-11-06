#include "syscall_process.h"
#include "idt.h"
#include "loader.h"
#include "paging.h"
#include "power.h"
#include "sched.h"
#include "timer.h"
#include "uaccess.h"
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

void syscall_process_thread(struct registers *r)
{
  if (!user_range(r->ebx, 1) || !user_range(r->ecx, 4)) {
    r->eax = (uint32_t)-1;
    return;
  }
  r->eax = (uint32_t)sched_create_user_thread(r->ebx, r->ecx);
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
