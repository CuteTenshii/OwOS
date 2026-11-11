/* thread.c: preemptive round-robin scheduler for kernel threads on one CPU.
 * Threads live on the heap in one circular list, so the only limit is memory. */
#include "arch.h"
#include "heap.h"
#include "irqflags.h"
#include "kstring.h"

#define KSTACK_SIZE 16384

enum { T_RUNNABLE, T_RUNNING, T_DEAD };

struct thread {
  struct thread *next, *prev;
  int id;
  int state;
  int exit_code;
  uint8_t *stack;
  uintptr_t sp;
  char name[16];
};

/* Interrupts stay masked from picking a thread until it resumes, and the resumed side restores its own mask (or the
 * trampoline unmasks), so a tick never lands on a context that is only half saved. */
static struct thread main_thread, idle;
static struct thread *cur;
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

static uintptr_t stack_top(uint8_t *stack) { return ((uintptr_t)stack + KSTACK_SIZE) & ~(uintptr_t)15; }

int sched_init(void)
{
  main_thread.next = main_thread.prev = &main_thread;
  main_thread.state = T_RUNNING;
  strlcpy(main_thread.name, "main", sizeof main_thread.name);
  rr = cur = &main_thread;

  idle.id = -1;
  strlcpy(idle.name, "idle", sizeof idle.name);
  idle.stack = kmalloc(KSTACK_SIZE + 16);
  if (!idle.stack)
    return -1;
  idle.sp = context_init((void *)stack_top(idle.stack), idle_loop, 0);
  return 0;
}

int thread_self(void) { return cur->id; }

int thread_create(const char *name, void (*entry)(void *), void *arg)
{
  struct thread *t = kmalloc(sizeof *t);
  uint8_t *stack = kmalloc(KSTACK_SIZE + 16);
  if (!t || !stack) {
    kfree(t);
    kfree(stack);
    return -1;
  }
  t->stack = stack;
  t->exit_code = 0;
  strlcpy(t->name, name, sizeof t->name);
  t->sp = context_init((void *)stack_top(stack), entry, arg);
  t->state = T_RUNNABLE;

  uintptr_t flags = irq_save();
  t->id = next_id++;
  t->prev = main_thread.prev;
  t->next = &main_thread;
  main_thread.prev->next = t;
  main_thread.prev = t;
  irq_restore(flags);
  return t->id;
}

/* Pick the next thread and switch to it. Called with interrupts masked; they are still masked when this returns, in
 * whichever thread resumed here. */
static void switch_masked(void)
{
  struct thread *prev = cur, *next = 0;

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
    next = &idle;
  if (next == prev)
    return;

  if (prev->id >= 0 && prev->state == T_RUNNING)
    prev->state = T_RUNNABLE;
  if (next->id >= 0) {
    next->state = T_RUNNING;
    rr = next;
  }
  cur = next;
  context_switch(&prev->sp, next->sp);
}

void schedule(void)
{
  uintptr_t flags = irq_save();
  switch_masked();
  irq_restore(flags);
}

void thread_yield(void) { schedule(); }

void thread_exit(int code)
{
  irq_save();
  cur->exit_code = code;
  cur->state = T_DEAD;
  switch_masked();
  for (;;)
    cpu_halt();
}

int thread_join(int id)
{
  struct thread *t = 0;
  for (;;) {
    uintptr_t flags = irq_save();
    for (struct thread *c = main_thread.next; c != &main_thread && !t; c = c->next)
      if (c->id == id)
        t = c;
    if (!t) {
      irq_restore(flags);
      return -1;
    }
    if (t->state == T_DEAD) {
      t->prev->next = t->next;
      t->next->prev = t->prev;
      if (rr == t)
        rr = t->prev;
      irq_restore(flags);
      break;
    }
    irq_restore(flags);
    thread_yield();
  }
  int code = t->exit_code;
  kfree(t->stack);
  kfree(t);
  return code;
}

void thread_sleep(uint32_t n)
{
  uint32_t start = timer_ticks();
  while (timer_ticks() - start < n)
    __asm__ volatile("wfi");
}
