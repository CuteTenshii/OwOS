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

static int run_image(const struct user_image *img) { return thread_join(thread_create_user("task", img)); }

int task_run(const void *code, uint32_t len)
{
  struct user_image img;
  return user_image_code(&img, code, len) ? -1 : run_image(&img);
}

int task_run_elf(const void *file, uint64_t size, int *code)
{
  struct user_image img;
  if (elf_load(&img, file, size))
    return -1;
  *code = run_image(&img);
  return 0;
}
