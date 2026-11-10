/* task.c: run a single EL0 task to completion, with write/exit syscalls. */
#include "arch.h"
#include "kstring.h"

#define USER_STACK_TOP (USER_BASE + 0x2000)

static struct {
  uint64_t *root;
  uintptr_t sp, caller_sp;
  int exit_code;
  bool active;
} task;

static uint8_t kstack[8192] __attribute__((aligned(16)));

static void user_launch(void)
{
  SYSREG_WRITE(sp_el0, USER_STACK_TOP);
  SYSREG_WRITE(elr_el1, USER_BASE);
  SYSREG_WRITE(spsr_el1, 0); /* EL0t, interrupts unmasked */
  __asm__ volatile("eret");
}

static void task_leave(int code)
{
  task.exit_code = code;
  task.active = false;
  context_switch(&task.sp, task.caller_sp);
}

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
    task_leave((int)tf->x[0]);
    return;
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
  task_leave(-1);
}

int task_run(const void *code, uint32_t len)
{
  uintptr_t text = pmm_alloc_frame(), stack = pmm_alloc_frame();
  task.root = vm_create();
  if (!text || !stack || !task.root || len > PMM_FRAME_SIZE)
    return -1;
  memset((void *)text, 0, PMM_FRAME_SIZE);
  memset((void *)stack, 0, PMM_FRAME_SIZE);
  memcpy((void *)text, code, len);
  for (uintptr_t a = text; a < text + len; a += 64)
    __asm__ volatile("dc cvau, %0\n\tdsb ish\n\tic ivau, %0" : : "r"(a) : "memory");
  __asm__ volatile("dsb ish\n\tisb");
  if (vm_map(task.root, USER_BASE, text, VM_USER_RX) || vm_map(task.root, USER_BASE + 0x1000, stack, VM_USER_RW))
    return -1;

  task.sp = context_init(kstack + sizeof kstack, user_launch);
  task.active = true;
  vm_switch(task.root);
  context_switch(&task.caller_sp, task.sp);
  vm_switch(kernel_l1);
  __asm__ volatile("msr daifclr, #2");
  vm_destroy(task.root);
  return task.exit_code;
}
