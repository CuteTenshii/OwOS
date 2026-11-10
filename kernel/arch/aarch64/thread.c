/* thread.c: preemptive round-robin scheduler for kernel threads and EL0 tasks, SMP.
 * Threads live on the heap in one circular list, so the only limit is memory. */
#include "arch.h"
#include "heap.h"
#include "irqflags.h"
#include "kstring.h"
#include "spinlock.h"
#include <stdbool.h>

#define KSTACK_SIZE 16384
#define USER_STACK_TOP (USER_BASE + 0x2000)

enum { T_RUNNABLE, T_RUNNING, T_DEAD };

struct thread {
  struct thread *next, *prev;
  int id;
  int state;
  int exit_code;
  uint8_t *stack;
  uintptr_t sp;
  uint64_t *root; /* user address space, 0 for kernel threads */
  char name[16];
};

/* sched_lock guards the thread list and every thread's state. It is held across a context
 * switch: the switching-out side takes it and the resumed side (or the trampoline) releases
 * it, so no CPU can pick a thread whose registers are not saved yet. */
static spinlock_t sched_lock;
static struct thread main_thread, idle[MAX_CPUS];
static struct thread *cur[MAX_CPUS];
static struct thread *rr; /* last thread picked; the next scan starts after it */
static int next_id = 1;

static void idle_loop(void *arg)
{
  (void)arg;
  for (;;) {
    thread_yield();
    __asm__ volatile("wfi");
  }
}

void sched_init(void)
{
  main_thread.next = main_thread.prev = &main_thread;
  main_thread.state = T_RUNNING;
  strlcpy(main_thread.name, "main", sizeof main_thread.name);
  rr = cur[0] = &main_thread;

  for (int i = 0; i < MAX_CPUS; i++)
    idle[i].id = -(i + 1);
  idle[0].stack = kmalloc(KSTACK_SIZE + 16);
  idle[0].sp = context_init((void *)(((uintptr_t)idle[0].stack + KSTACK_SIZE) & ~(uintptr_t)15), idle_loop, 0);
}

/* Called on a secondary CPU, on its boot stack, which becomes that CPU's idle thread. */
void sched_cpu_init(int cpu) { cur[cpu] = &idle[cpu]; }

/* Releases the lock inherited from the thread that switched to a brand new one. */
void sched_new_thread(void) { __sync_lock_release(&sched_lock); }

int thread_self(void) { return cur[cpu_id()]->id; }

static int thread_spawn(const char *name, void (*entry)(void *), void *arg, uint64_t *root)
{
  struct thread *t = kmalloc(sizeof *t);
  uint8_t *stack = kmalloc(KSTACK_SIZE + 16);
  if (!t || !stack) {
    kfree(t);
    kfree(stack);
    return -1;
  }
  t->stack = stack;
  t->root = root;
  t->exit_code = 0;
  strlcpy(t->name, name, sizeof t->name);
  t->sp = context_init((void *)(((uintptr_t)stack + KSTACK_SIZE) & ~(uintptr_t)15), entry, arg);
  t->state = T_RUNNABLE;

  uintptr_t flags = spin_lock_irqsave(&sched_lock);
  t->id = next_id++;
  t->prev = main_thread.prev;
  t->next = &main_thread;
  main_thread.prev->next = t;
  main_thread.prev = t;
  spin_unlock_irqrestore(&sched_lock, flags);
  return t->id;
}

int thread_create(const char *name, void (*entry)(void *), void *arg) { return thread_spawn(name, entry, arg, 0); }

/* Pick the next thread for this CPU and switch to it. Called with sched_lock held; it is still
 * held when this returns, in whichever thread resumed here. */
static void switch_locked(void)
{
  int cpu = cpu_id();
  struct thread *prev = cur[cpu], *next = 0;

  struct thread *c = rr;
  do {
    c = c->next;
    if (c->state == T_RUNNABLE) {
      next = c;
      break;
    }
  } while (c != rr);

  if (!next && prev->id >= 0 && prev->state == T_RUNNING)
    return;
  if (!next)
    next = &idle[cpu];
  if (next == prev)
    return;

  if (prev->id >= 0 && prev->state == T_RUNNING)
    prev->state = T_RUNNABLE;
  if (next->id >= 0) {
    next->state = T_RUNNING;
    rr = next;
  }
  cur[cpu] = next;
  uint64_t *old_root = prev->root ? prev->root : kernel_l1, *new_root = next->root ? next->root : kernel_l1;
  if (new_root != old_root)
    vm_switch(new_root);
  context_switch(&prev->sp, next->sp);
}

void schedule(void)
{
  uintptr_t flags = spin_lock_irqsave(&sched_lock);
  switch_locked();
  spin_unlock_irqrestore(&sched_lock, flags);
}

void thread_yield(void) { schedule(); }

void thread_exit(int code)
{
  spin_lock_irqsave(&sched_lock);
  struct thread *t = cur[cpu_id()];
  t->exit_code = code;
  t->state = T_DEAD;
  switch_locked();
  for (;;)
    cpu_halt();
}

int thread_join(int id)
{
  struct thread *t = 0;
  for (;;) {
    uintptr_t flags = spin_lock_irqsave(&sched_lock);
    for (struct thread *c = main_thread.next; c != &main_thread && !t; c = c->next)
      if (c->id == id)
        t = c;
    if (!t) {
      spin_unlock_irqrestore(&sched_lock, flags);
      return -1;
    }
    if (t->state == T_DEAD) {
      t->prev->next = t->next;
      t->next->prev = t->prev;
      if (rr == t)
        rr = t->prev;
      spin_unlock_irqrestore(&sched_lock, flags);
      break;
    }
    spin_unlock_irqrestore(&sched_lock, flags);
    thread_yield();
  }
  int code = t->exit_code;
  if (t->root)
    vm_destroy(t->root);
  kfree(t->stack);
  kfree(t);
  return code;
}

void thread_sleep(uint32_t ticks)
{
  uint64_t wake = timer_ticks() + ticks;
  while (timer_ticks() < wake)
    __asm__ volatile("wfi");
}

static void user_launch(void *arg)
{
  (void)arg;
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
      id = thread_spawn(name, user_launch, 0, root);
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
