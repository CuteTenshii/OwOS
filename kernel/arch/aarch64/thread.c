/* thread.c: round-robin preemptive scheduler for kernel threads and EL0 tasks (single CPU). */
#include "arch.h"
#include "heap.h"
#include "irqflags.h"
#include "kstring.h"
#include <stdbool.h>

#define MAX_THREADS 16
#define KSTACK_SIZE 16384
#define USER_STACK_TOP (USER_BASE + 0x2000)

enum { T_FREE, T_RUNNABLE, T_DEAD };

struct thread {
  int state;
  int exit_code;
  uint8_t *stack;
  uintptr_t sp;
  uint64_t *root; /* user address space, 0 for kernel threads */
  char name[16];
};

static struct thread threads[MAX_THREADS];
static int current;

void sched_init(void)
{
  threads[0].state = T_RUNNABLE;
  strlcpy(threads[0].name, "main", sizeof threads[0].name);
}

int thread_self(void) { return current; }

static int thread_spawn(const char *name, void (*entry)(void), uint64_t *root)
{
  uintptr_t flags = irq_save();
  int id = -1;
  for (int i = 1; i < MAX_THREADS; i++)
    if (threads[i].state == T_FREE) {
      id = i;
      break;
    }
  uint8_t *stack = id < 0 ? 0 : kmalloc(KSTACK_SIZE + 16);
  if (!stack) {
    irq_restore(flags);
    return -1;
  }
  struct thread *t = &threads[id];
  t->stack = stack;
  t->root = root;
  t->exit_code = 0;
  strlcpy(t->name, name, sizeof t->name);
  t->sp = context_init((void *)(((uintptr_t)stack + KSTACK_SIZE) & ~(uintptr_t)15), entry);
  t->state = T_RUNNABLE;
  irq_restore(flags);
  return id;
}

int thread_create(const char *name, void (*entry)(void)) { return thread_spawn(name, entry, 0); }

void schedule(void)
{
  struct thread *prev = &threads[current];
  int next = -1;
  for (int i = 1; i <= MAX_THREADS; i++) {
    int c = (current + i) % MAX_THREADS;
    if (threads[c].state == T_RUNNABLE) {
      next = c;
      break;
    }
  }
  if (next < 0 || next == current)
    return;

  struct thread *n = &threads[next];
  current = next;
  uint64_t *old_root = prev->root ? prev->root : kernel_l1, *new_root = n->root ? n->root : kernel_l1;
  if (new_root != old_root)
    vm_switch(new_root);
  context_switch(&prev->sp, n->sp);
}

void thread_yield(void)
{
  uintptr_t flags = irq_save();
  schedule();
  irq_restore(flags);
}

void thread_exit(int code)
{
  irq_save();
  threads[current].exit_code = code;
  threads[current].state = T_DEAD;
  schedule();
  for (;;)
    cpu_halt();
}

int thread_join(int id)
{
  if (id <= 0 || id >= MAX_THREADS)
    return -1;
  struct thread *t = &threads[id];
  while (t->state != T_DEAD)
    thread_yield();
  int code = t->exit_code;
  if (t->root)
    vm_destroy(t->root);
  kfree(t->stack);
  t->state = T_FREE;
  return code;
}

void thread_sleep(uint32_t ticks)
{
  uint64_t wake = timer_ticks() + ticks;
  while (timer_ticks() < wake)
    __asm__ volatile("wfi");
}

static void user_launch(void)
{
  irq_save(); /* ELR/SPSR must not be clobbered by a tick before the eret */
  SYSREG_WRITE(sp_el0, USER_STACK_TOP);
  SYSREG_WRITE(elr_el1, USER_BASE);
  SYSREG_WRITE(spsr_el1, 0);
  __asm__ volatile("eret");
}

int thread_create_user(const char *name, const void *code, uint32_t len)
{
  phys_t text = pmm_alloc_frame(), stack = pmm_alloc_frame();
  uint64_t *root = vm_create();
  bool text_mapped = false, stack_mapped = false;
  int id = -1;

  if (text && stack && root && len <= PMM_FRAME_SIZE) {
    memset((void *)text, 0, PMM_FRAME_SIZE);
    memset((void *)stack, 0, PMM_FRAME_SIZE);
    memcpy((void *)text, code, len);
    for (uintptr_t a = text; a < text + len; a += 64)
      __asm__ volatile("dc cvau, %0\n\tdsb ish\n\tic ivau, %0" : : "r"(a) : "memory");
    __asm__ volatile("dsb ish\n\tisb");
    text_mapped = !vm_map(root, USER_BASE, text, VM_USER_RX);
    stack_mapped = text_mapped && !vm_map(root, USER_BASE + 0x1000, stack, VM_USER_RW);
    if (stack_mapped)
      id = thread_spawn(name, user_launch, root);
  }
  if (id < 0) {
    if (root)
      vm_destroy(root); /* frees mapped pages and the tables */
    if (text && !text_mapped)
      pmm_free_frame(text);
    if (stack && !stack_mapped)
      pmm_free_frame(stack);
  }
  return id;
}
