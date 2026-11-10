/* syscall.c: EL0 system calls, using the same numbers and argument order as the x86 ABI
 * (common/syscall_nr.h). x8 = number, x0..x2 = ebx..edx, result in x0. */
#include "arch.h"
#include "syscall_nr.h"

typedef uint64_t (*syscall_handler_t)(uint64_t a, uint64_t b, uint64_t c);

#define ERR ((uint64_t)-1)

static uint64_t sys_write(uint64_t fd, uint64_t buf, uint64_t len)
{
  if ((fd != 1 && fd != 2) || !user_range(buf, len))
    return ERR;
  for (uint64_t i = 0; i < len; i++)
    uart_putc(((const char *)buf)[i]);
  return len;
}

static uint64_t sys_exit(uint64_t code, uint64_t b, uint64_t c)
{
  (void)b;
  (void)c;
  thread_exit((int)code);
}

static uint64_t sys_sleep(uint64_t ms, uint64_t b, uint64_t c)
{
  (void)b;
  (void)c;
  thread_sleep((uint32_t)((ms * TICK_HZ + 999) / 1000));
  return 0;
}

static uint64_t sys_msec(uint64_t a, uint64_t b, uint64_t c)
{
  (void)a;
  (void)b;
  (void)c;
  return timer_ticks() * 1000 / TICK_HZ;
}

static uint64_t sys_getpid(uint64_t a, uint64_t b, uint64_t c)
{
  (void)a;
  (void)b;
  (void)c;
  return (uint64_t)thread_self();
}

static const syscall_handler_t handlers[SYS_GETPID + 1] = {
    [SYS_WRITE] = sys_write, [SYS_EXIT] = sys_exit,     [SYS_SLEEP] = sys_sleep,
    [SYS_MSEC] = sys_msec,   [SYS_GETPID] = sys_getpid,
};

void syscall_dispatch(struct trap_frame *tf)
{
  uint64_t n = tf->x[8];
  __asm__ volatile("msr daifclr, #2"); /* calls may block; the eret restores the caller's mask */
  tf->x[0] = n < sizeof handlers / sizeof handlers[0] && handlers[n] ? handlers[n](tf->x[0], tf->x[1], tf->x[2]) : ERR;
}
