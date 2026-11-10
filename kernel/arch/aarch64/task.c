/* task.c: EL0 fault handling and a run-to-completion helper. */
#include "arch.h"

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
