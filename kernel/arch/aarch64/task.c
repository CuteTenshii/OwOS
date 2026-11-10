/* task.c: EL0 syscalls and fault handling, plus a run-to-completion helper. */
#include "arch.h"

void task_syscall(struct trap_frame *tf)
{
  switch (tf->x[8]) {
  case SYS_WRITE: {
    uint64_t ptr = tf->x[0], len = tf->x[1];
    if (ptr < USER_BASE || ptr >= USER_END || len > USER_END - ptr) {
      tf->x[0] = (uint64_t)-1;
      return;
    }
    for (uint64_t i = 0; i < len; i++)
      uart_putc(((const char *)ptr)[i]);
    tf->x[0] = len;
    return;
  }
  case SYS_EXIT:
    thread_exit((int)tf->x[0]);
  default:
    tf->x[0] = (uint64_t)-1;
  }
}

void task_fault(struct trap_frame *tf)
{
  uart_puts("task fault esr=");
  uart_puthex(SYSREG_READ(esr_el1));
  uart_puts(" far=");
  uart_puthex(SYSREG_READ(far_el1));
  uart_puts(" elr=");
  uart_puthex(tf->elr);
  uart_putc('\n');
  thread_exit(-1);
}

int task_run(const void *code, uint32_t len) { return thread_join(thread_create_user("task", code, len)); }
