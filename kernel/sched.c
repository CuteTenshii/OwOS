/* sched.c — round-robin scheduling of kernel threads and user processes. */

#include "sched.h"
#include "console.h"
#include "file.h"
#include "gdt.h"
#include "heap.h"
#include "idt.h"
#include "ipc.h"
#include "irqflags.h"
#include "loader.h"
#include "net.h"
#include "paging.h"
#include "pipe.h"
#include "signal.h"
#include "smp.h"
#include "syscall.h"
#include "syscall_nr.h"
#include "timer.h"
#include "uwin.h"
#include <stdint.h>

/* Tasks are HEAP-ALLOCATED — a `kmalloc`'d task_t per task, linked into the
 * circular run queue. There is no fixed task-table cap: the only ceiling is the
 * kernel heap, and `alloc_task()` fails gracefully (the spawn returns -1, e.g.
 * "failed to launch ...") when it's exhausted, rather than at an arbitrary
 * array bound. Each task also kmalloc's a KSTACK_SIZE kernel stack, so kstacks
 * are what actually dominate heap use as the task count grows. */
/* Every timer tick, timer_handler() runs nic_poll() -> the full IP/TCP/UDP RX
 * path BEFORE schedule(), nested in the IRQ on whatever thread it interrupted
 * (real hosts/VMware poll the NIC off the tick because its INTx never reaches
 * our PIC). So a thread's worst-case stack is its own depth PLUS a nested
 * receive stack. The wm compositor mid-compose() + a nested RX overran 16 KiB
 * on real hardware, silently smashing the adjacent thread's stack (a GP fault
 * resuming an unrelated thread into EIP 0x0000000D). Size for both. */
#define KSTACK_SIZE                                                                                                    \
  65536 /* 64K: sshd runs the SSH KEX crypto (x25519/ed25519, deep                                                     \
         * stacks) on a kernel thread, and a NIC-RX IRQ for the next                                                   \
         * handshake packet nests its full RX/TCP chain on top — 32K                                                 \
         * overflowed and corrupted an adjacent task struct. */
#define UKSTACK_SIZE                                                                                                   \
  32768                  /* user task kernel stack: a timer IRQ mid-syscall nests                                      \
                          * nic_poll()'s full RX/TCP chain on top of the syscall's                                     \
                          * frames (no ring0->ring0 stack switch), so it needs the                                     \
                          * same headroom as KSTACK_SIZE, not half. 16K overflowed                                     \
                          * into an adjacent task on real HW (busy LAN) -> the                                         \
                          * victim resumed into EIP 0x0000000D. */
#define TASK_CWD_MAX 128 /* must match the shell's PATH_MAX */

enum { TASK_FREE = 0, TASK_RUNNABLE, TASK_WAITING, TASK_ZOMBIE };

typedef struct address_space {
  uint32_t *dir;
  uint32_t refs;
  uint32_t brk;
  uint32_t brk_base;
} address_space_t;

typedef struct task {
  uintptr_t esp;        /* saved kernel stack pointer (the context)     */
  address_space_t *as;  /* shared by a process and its user threads      */
  bool as_owner;        /* this task owns the address-space object          */
  void *kstack;         /* kernel stack to free (0 for the main task)   */
  uintptr_t kstack_top; /* TSS stack top (0 = leave it alone)          */
  int state;
  uint32_t id;
  struct task *process;       /* process leader for user threads             */
  uint32_t parent;            /* pid of the task that created it              */
  bool self_reap;             /* detached kernel thread: no parent waitpid's it, so
                               * schedule() frees its zombie instead of leaking it */
  int wait_pid;               /* pid being waited on while WAITING            */
  uint32_t futex_key;         /* user address a futex waiter sleeps on (0 = none) */
  uint32_t wake_tick;         /* if WAITING for a timed sleep: tick to wake at
                               * (0 = not a timed sleep, e.g. a waitpid waiter) */
  int exit_status;            /* set on exit, read by waitpid                 */
  uint32_t sig_pending;       /* bitset of posted-but-undelivered signals       */
  uint32_t sig_blocked;       /* bitset currently masked from delivery          */
  uint32_t sig_handler[NSIG]; /* disposition per signal (SIG_DFL/IGN/addr) */
  uint32_t sig_mask[NSIG];    /* extra signals blocked while a handler runs */
  uint32_t sig_flags[NSIG];   /* per-signal SA_* flags                     */
  const char *name;
  char namebuf[16]; /* owns `name` for spawned programs (argv[0])    */
  uint32_t runs;
  struct fd fds[MAX_FD];  /* open file descriptors                        */
  char cwd[TASK_CWD_MAX]; /* working dir; relative path syscalls join here */
  uint32_t uid;           /* owning user id (credentials; inherited like cwd)    */
  uint32_t gid;           /* owning primary group id                             */
  struct task *next;
  int running_cpu;
  uint8_t kernel_lock_depth;
  uint8_t kernel_lock_reacquire;
} task_t;

/* Copy a path into a fixed buffer, NUL-terminating (truncates if too long). */
static void cwd_set(char *dst, const char *src)
{
  int i = 0;
  if (src)
    for (; src[i] && i < TASK_CWD_MAX - 1; i++)
      dst[i] = src[i];
  dst[i] = '\0';
}

/* fd 0/1/2 = console; the rest closed. */
static void fds_init_console(task_t *t)
{
  for (int i = 0; i < MAX_FD; i++)
    t->fds[i].type = FD_NONE;
  t->fds[0].type = t->fds[1].type = t->fds[2].type = FD_CONSOLE;
}

/* Reset a task's signal state to the fresh-program default: nothing pending,
 * nothing blocked, every disposition SIG_DFL (0). Also the exec-reset. */
static void sig_init(task_t *t)
{
  t->sig_pending = 0;
  t->sig_blocked = 0;
  for (int i = 0; i < NSIG; i++) {
    t->sig_handler[i] = SIG_DFL;
    t->sig_mask[i] = 0;
    t->sig_flags[i] = 0;
  }
}

/* Drop references this task held on any pipe ends. */
static void fds_close_all(task_t *t)
{
  for (int i = 0; i < MAX_FD; i++) {
    if (t->fds[i].type == FD_PIPE_RD)
      pipe_close(t->fds[i].pipe, 0);
    else if (t->fds[i].type == FD_PIPE_WR)
      pipe_close(t->fds[i].pipe, 1);
    else if (t->fds[i].type == FD_CHANNEL) {
      pipe_close(t->fds[i].pipe, 0); /* rx reader */
      pipe_close(t->fds[i].tx, 1);   /* tx writer */
    } else if (t->fds[i].type == FD_PORT)
      ipc_port_destroy(t->fds[i].pipe);
    t->fds[i].type = FD_NONE;
  }
}

/* From switch.S. */
extern void context_switch(uintptr_t *old_esp, uintptr_t new_esp);
extern void thread_start(void);
extern void enter_user_task(void);
extern void fork_child_entry(void);

static task_t main_task; /* permanent head of the circular run queue (never freed) */
static task_t idle_tasks[SMP_MAX_CPUS];
static task_t *current_tasks[SMP_MAX_CPUS];
static task_t *switching_tasks[SMP_MAX_CPUS];
static volatile uint32_t scheduler_lock;
static volatile uint32_t kernel_lock;
static volatile int kernel_lock_cpu = -1;
static int count;
static uint32_t next_id;
static int detached_zombies; /* self_reap threads that have exited, awaiting free */

#define current (current_tasks[smp_this_cpu()])

static bool task_running(const task_t *task)
{
  for (int i = 0; i < SMP_MAX_CPUS; i++)
    if (current_tasks[i] == task || switching_tasks[i] == task)
      return true;
  return false;
}

static void sched_lock(void)
{
  while (__sync_lock_test_and_set(&scheduler_lock, 1))
    __asm__ volatile("pause");
}

static void sched_unlock(void) { __sync_lock_release(&scheduler_lock); }

void sched_kernel_enter(void)
{
  int cpu = smp_this_cpu();
  if (kernel_lock_cpu == cpu) {
    current->kernel_lock_depth++;
    return;
  }
  while (__sync_lock_test_and_set(&kernel_lock, 1))
    __asm__ volatile("pause");
  kernel_lock_cpu = cpu;
  current->kernel_lock_depth = 1;
}

void sched_kernel_leave(void)
{
  if (!current || current->kernel_lock_depth == 0)
    return;
  if (--current->kernel_lock_depth == 0) {
    kernel_lock_cpu = -1;
    __sync_lock_release(&kernel_lock);
  }
}

static void sched_kernel_switch_out(void)
{
  if (!current || current->kernel_lock_depth == 0)
    return;
  current->kernel_lock_reacquire = current->kernel_lock_depth;
  current->kernel_lock_depth = 0;
  kernel_lock_cpu = -1;
  __sync_lock_release(&kernel_lock);
}

void sched_kernel_switch_in(void)
{
  uint8_t depth = current ? current->kernel_lock_reacquire : 0;
  if (!depth)
    return;
  while (__sync_lock_test_and_set(&kernel_lock, 1))
    __asm__ volatile("pause");
  kernel_lock_cpu = smp_this_cpu();
  current->kernel_lock_depth = depth;
  current->kernel_lock_reacquire = 0;
}

/* Allocate a zeroed task node off the kernel heap (NOT yet on the run queue —
 * the creator fills it in and calls enqueue()). Returns 0 if the heap is full,
 * so every spawn path degrades gracefully. Zeroing reproduces what a fresh
 * static array slot used to give (all-FREE fds, cleared flags). */
static task_t *alloc_task(void)
{
  task_t *t = kmalloc(sizeof *t);
  if (!t)
    return 0;
  uint8_t *p = (uint8_t *)t;
  for (unsigned i = 0; i < sizeof *t; i++)
    p[i] = 0;
  return t;
}

/* Return the active page directory for a task. Kernel threads have no address
 * space object and run under the shared kernel directory. */
static uintptr_t task_cr3(const task_t *t) { return t->as ? (uintptr_t)t->as->dir : paging_kernel_phys(); }

static void address_space_get(address_space_t *as)
{
  if (as)
    as->refs++;
}

static void address_space_put(address_space_t *as)
{
  if (!as || as->refs == 0)
    return;
  as->refs--;
  if (as->refs == 0) {
    paging_free_process_dir(as->dir);
    kfree(as);
  }
}

/* Splice t into the run queue right after the main task. The single publishing
 * write (main_task.next = t) makes t visible atomically, so a timer IRQ walking
 * the queue mid-enqueue always sees a consistent list. */
static void enqueue(task_t *t)
{
  uintptr_t flags = irq_save();
  sched_lock();
  t->next = main_task.next;
  t->running_cpu = -1;
  main_task.next = t;
  count++;
  sched_unlock();
  irq_restore(flags);
}

/* Return trampoline for a kernel thread whose body returned. Exit cleanly
 * rather than spinning on a bare hlt: sched_exit marks us a zombie and switches
 * away, so the task is descheduled (never round-robined, never burning a tick)
 * and we can't halt the CPU forever if the body happened to return with IF
 * clear. */
void thread_exit(void)
{
  sched_exit(0);
  for (;;)
    __asm__ volatile("sti\n\thlt"); /* unreachable; sti keeps hlt safe */
}

void sched_init(void)
{
  task_t *t = &main_task;
  t->id = next_id++;
  t->name = "main";
  t->runs = 1;
  t->esp = 0; /* filled in on the first switch away       */
  t->as = 0;
  t->as_owner = false;
  t->process = t;
  t->kstack = 0;
  t->kstack_top = 0; /* a kernel thread: never traps to ring 0   */
  t->state = TASK_RUNNABLE;
  t->wake_tick = 0;
  sig_init(t);
  t->uid = t->gid = 0; /* the boot session runs as root            */
  cwd_set(t->cwd, "/");
  t->next = t;
  t->running_cpu = 0;
  for (int i = 0; i < SMP_MAX_CPUS; i++)
    current_tasks[i] = 0;
  current_tasks[0] = t;
  count = 1;

  timer_on_tick(schedule);
}

void sched_init_cpu(int cpu)
{
  task_t *idle;
  if (cpu <= 0 || cpu >= SMP_MAX_CPUS)
    return;
  idle = &idle_tasks[cpu];
  for (unsigned i = 0; i < sizeof *idle; i++)
    ((uint8_t *)idle)[i] = 0;
  idle->name = "idle";
  idle->state = TASK_RUNNABLE;
  idle->running_cpu = cpu;
  idle->process = idle;
  current_tasks[cpu] = idle;
}

void sched_start_cpu(int cpu)
{
  if (cpu <= 0 || cpu >= SMP_MAX_CPUS)
    return;
  for (;;) {
    schedule();
    __asm__ volatile("hlt");
  }
}

void sched_create_thread(const char *name, void (*entry)(void))
{
  task_t *t = alloc_task();
  if (!t)
    return;
  t->id = next_id++;
  t->name = name;
  t->runs = 0;
  t->as = 0;
  t->as_owner = false;
  t->process = t;
  t->kstack_top = 0;
  t->state = TASK_RUNNABLE;
  t->self_reap = true; /* main_task never waitpid's us: self-reap on exit */

  uint8_t *stack = kmalloc(KSTACK_SIZE);
  if (!stack) { /* heap exhausted: don't enqueue a bogus stack */
    kfree(t);
    return;
  }
  sig_init(t);
  t->kstack = stack;
#ifdef __x86_64__
  /* context_switch rets into thread_start, which rets into `entry`; when the
   * thread body returns it rets into thread_exit. Six zeroed callee slots
   * (rbp,rbx,r12-r15) feed context_switch's pop sequence. 8-byte slots. */
  uint64_t *sp = (uint64_t *)(stack + KSTACK_SIZE);
  *--sp = (uint64_t)(uintptr_t)thread_exit;
  *--sp = (uint64_t)(uintptr_t)entry;
  *--sp = (uint64_t)(uintptr_t)thread_start;
  for (int i = 0; i < 6; i++)
    *--sp = 0;
#else
  uint32_t *sp = (uint32_t *)(stack + KSTACK_SIZE);
  *--sp = (uint32_t)thread_exit;
  *--sp = (uint32_t)entry;
  *--sp = (uint32_t)thread_start;
  *--sp = 0;
  *--sp = 0;
  *--sp = 0;
  *--sp = 0; /* ebp,ebx,esi,edi          */
#endif
  t->esp = (uintptr_t)sp;

  enqueue(t);
}

int sched_create_process(const char *name, uint32_t entry, uint32_t *dir, uint32_t user_esp, uint32_t brk)
{
  task_t *t = alloc_task();
  address_space_t *as;
  if (!t)
    return -1;
  t->id = next_id++;
  as = kmalloc(sizeof *as);
  if (!as) {
    kfree(t);
    return -1;
  }
  as->dir = dir;
  as->refs = 1;
  as->brk = as->brk_base = brk;
  { /* copy the program name so it persists (callers pass argv[0]) */
    int i = 0;
    for (; name && name[i] && i < (int)sizeof t->namebuf - 1; i++)
      t->namebuf[i] = name[i];
    t->namebuf[i] = '\0';
    t->name = t->namebuf;
  }
  t->runs = 0;
  t->as = as;
  t->as_owner = true;
  t->process = t;
  t->parent = current->id;
  t->state = TASK_RUNNABLE;
  sig_init(t);
  fds_init_console(t);
  cwd_set(t->cwd, current->cwd); /* inherit the spawner's cwd by default */
  t->uid = current->uid;         /* and its credentials (uid/gid)         */
  t->gid = current->gid;

  uint8_t *kstack = kmalloc(UKSTACK_SIZE);
  if (!kstack) { /* heap exhausted: fail rather than enqueue a
                  * task whose saved esp points into low memory */
    address_space_put(as);
    kfree(t);
    return -1;
  }
  t->kstack = kstack;
  t->kstack_top = (uintptr_t)(kstack + UKSTACK_SIZE);

  /* Craft the kernel stack so the first schedule restores it, rets into
   * enter_user_task, and irets to ring 3 at `entry`. */
#ifdef __x86_64__
  uint64_t *sp = (uint64_t *)(uintptr_t)t->kstack_top;
  *--sp = 0x23;     /* user SS                                 */
  *--sp = user_esp; /* user RSP (points at argc)               */
  *--sp = 0x202;    /* RFLAGS (IF set)                         */
  *--sp = 0x1B;     /* user CS (32-bit compat, DPL3)           */
  *--sp = entry;    /* user RIP                                */
  *--sp = (uint64_t)(uintptr_t)enter_user_task;
  for (int i = 0; i < 6; i++)
    *--sp = 0; /* rbp,rbx,r12-r15           */
#else
  uint32_t *sp = (uint32_t *)t->kstack_top;
  *--sp = 0x23;     /* user SS                                 */
  *--sp = user_esp; /* user ESP (points at argc)               */
  *--sp = 0x202;    /* EFLAGS (IF set)                         */
  *--sp = 0x1B;     /* user CS                                 */
  *--sp = entry;    /* user EIP                                */
  *--sp = (uint32_t)enter_user_task;
  *--sp = 0;
  *--sp = 0;
  *--sp = 0;
  *--sp = 0; /* ebp,ebx,esi,edi          */
#endif
  t->esp = (uintptr_t)sp;

  enqueue(t);
  return (int)t->id;
}

/* Move the caller's process break by `inc` bytes (the malloc primitive),
 * mapping fresh zeroed window pages on growth or freeing whole pages on shrink.
 * Returns the PREVIOUS break, or (uint32_t)-1 on failure. Runs in the int 0x80
 * gate with IF clear, so it is atomic against the other threads that share this
 * heap — no lock needed on the kernel side. */
uint32_t sched_sbrk(int inc)
{
  address_space_t *as = current->as;
  if (!as || as->brk == 0) /* not a process with a heap */
    return (uint32_t)-1;
  uint32_t old = as->brk;
  if (inc == 0)
    return old;
  if (inc > 0) {
    uint32_t add = (uint32_t)inc;
    if (old >= USER_HEAP_MAX || add > USER_HEAP_MAX - old)
      return (uint32_t)-1; /* would run into the user stack */
    uint32_t newbrk = old + add;
    uint32_t old_top = (old + 0xFFFu) & ~0xFFFu;
    uint32_t new_top = (newbrk + 0xFFFu) & ~0xFFFu;
    if (new_top > old_top && paging_user_grow(as->dir, old_top, new_top) < 0)
      return (uint32_t)-1; /* out of physical memory */
    as->brk = newbrk;
    return old;
  }
  uint32_t dec = (uint32_t)(-inc);
  if (dec > old - as->brk_base) /* never shrink below the heap base */
    dec = old - as->brk_base;
  uint32_t newbrk = old - dec;
  uint32_t old_top = (old + 0xFFFu) & ~0xFFFu;
  uint32_t new_top = (newbrk + 0xFFFu) & ~0xFFFu;
  if (old_top > new_top)
    paging_user_shrink(as->dir, new_top, old_top);
  as->brk = newbrk;
  return old;
}

/* Reset the caller's heap base + break (the image-replace syscall, after it
 * swaps in a new program image whose end is the new heap start). */
void sched_set_brk(uint32_t brk)
{
  if (current->as)
    current->as->brk = current->as->brk_base = brk;
}

/* Spawn a thread inside the CURRENT process: a new task that SHARES the
 * caller's address space and runs `entry` in ring 3 on the caller-provided user
 * stack. Its address-space reference keeps the shared tables alive. It is an ordinary child of
 * the caller (parent = current->id), so the caller joins it with waitpid()
 * before exiting (which is required: the threads must be gone before the main
 * task's reap tears down the address space they run in). Returns its tid. */
int sched_create_user_thread(uint32_t entry, uint32_t user_esp)
{
  task_t *t = alloc_task();
  if (!t)
    return -1;
  t->id = next_id++;
  t->name = "thread";
  t->runs = 0;
  t->as = current->as;
  t->as_owner = false;
  t->process = current->process;
  address_space_get(t->as);
  t->parent = current->id;
  t->state = TASK_RUNNABLE;
  sig_init(t);
  fds_init_console(t);
  cwd_set(t->cwd, current->cwd);
  t->uid = current->uid;
  t->gid = current->gid;

  uint8_t *kstack = kmalloc(UKSTACK_SIZE);
  if (!kstack) {
    address_space_put(t->as);
    kfree(t);
    return -1;
  }
  t->kstack = kstack;
  t->kstack_top = (uintptr_t)(kstack + UKSTACK_SIZE);

#ifdef __x86_64__
  uint64_t *sp = (uint64_t *)(uintptr_t)t->kstack_top;
  *--sp = 0x23;     /* user SS                                 */
  *--sp = user_esp; /* user RSP                                */
  *--sp = 0x202;    /* RFLAGS (IF set)                         */
  *--sp = 0x1B;     /* user CS (32-bit compat, DPL3)           */
  *--sp = entry;    /* user RIP                                */
  *--sp = (uint64_t)(uintptr_t)enter_user_task;
  for (int i = 0; i < 6; i++)
    *--sp = 0; /* rbp,rbx,r12-r15           */
#else
  uint32_t *sp = (uint32_t *)t->kstack_top;
  *--sp = 0x23;     /* user SS                                 */
  *--sp = user_esp; /* user ESP                                */
  *--sp = 0x202;    /* EFLAGS (IF set)                         */
  *--sp = 0x1B;     /* user CS                                 */
  *--sp = entry;    /* user EIP                                */
  *--sp = (uint32_t)enter_user_task;
  *--sp = 0;
  *--sp = 0;
  *--sp = 0;
  *--sp = 0; /* ebp,ebx,esi,edi          */
#endif
  t->esp = (uintptr_t)sp;

  enqueue(t);
  return (int)t->id;
}

int sched_fork(struct registers *r)
{
  task_t *t = alloc_task();
  if (!t || !current->as) {
    if (t)
      kfree(t);
    return -1;
  }

  uint32_t *child_dir = paging_fork_dir(current->as ? current->as->dir : 0);
  if (!child_dir) {
    kfree(t);
    return -1;
  }

  uint8_t *kstack = kmalloc(UKSTACK_SIZE);
  if (!kstack) {
    paging_free_process_dir(child_dir);
    kfree(t);
    return -1;
  }

  t->id = next_id++;
  t->name = current->name;
  t->runs = 0;
  t->as = kmalloc(sizeof *t->as);
  if (!t->as) {
    paging_free_process_dir(child_dir);
    kfree(kstack);
    kfree(t);
    return -1;
  }
  t->as->dir = child_dir;
  t->as->refs = 1;
  t->as_owner = true;
  t->process = t;
  t->parent = current->id;
  t->kstack = kstack;
  t->kstack_top = (uintptr_t)(kstack + UKSTACK_SIZE);
  t->state = TASK_RUNNABLE;
  sig_init(t);
  /* fork inherits the parent's signal dispositions + blocked mask (POSIX); the
   * pending set does NOT carry over (sig_init already cleared it). */
  t->sig_blocked = current->sig_blocked;
  for (int i = 0; i < NSIG; i++) {
    t->sig_handler[i] = current->sig_handler[i];
    t->sig_mask[i] = current->sig_mask[i];
    t->sig_flags[i] = current->sig_flags[i];
  }
  /* The child shares the parent's user pages copy-on-write, so it inherits the
   * same heap break without copying the address space. */
  if (current->as) {
    t->as->brk = current->as->brk;
    t->as->brk_base = current->as->brk_base;
  }
  cwd_set(t->cwd, current->cwd);
  t->uid = current->uid;
  t->gid = current->gid;

  /* The child inherits the parent's open files (Unix semantics). */
  for (int i = 0; i < MAX_FD; i++) {
    t->fds[i] = current->fds[i];
    if (t->fds[i].type == FD_PIPE_RD)
      pipe_ref(t->fds[i].pipe, 0);
    else if (t->fds[i].type == FD_PIPE_WR)
      pipe_ref(t->fds[i].pipe, 1);
    else if (t->fds[i].type == FD_CHANNEL) {
      pipe_ref(t->fds[i].pipe, 0); /* rx reader */
      pipe_ref(t->fds[i].tx, 1);   /* tx writer */
    } else if (t->fds[i].type == FD_PORT)
      t->fds[i].type = FD_NONE; /* a listen port stays single-owner */
  }

  /* Copy the parent's syscall trap frame to the top of the child's kernel
   * stack, forcing the child's saved eax to 0 so fork() returns 0 in the child.
   * Then a context_switch frame that lands in fork_child_entry. */
#ifdef __x86_64__
  struct registers *cf = (struct registers *)((uint8_t *)(uintptr_t)t->kstack_top - sizeof *cf);
  *cf = *r;
  cf->eax = 0; /* rax = 0 in the child                    */
  uint64_t *sp = (uint64_t *)cf;
  *--sp = (uint64_t)(uintptr_t)fork_child_entry;
  for (int i = 0; i < 6; i++)
    *--sp = 0; /* rbp,rbx,r12-r15           */
#else
  uint32_t *frame = (uint32_t *)t->kstack_top - 16;
  const uint32_t *src = (const uint32_t *)r;
  for (int i = 0; i < 16; i++)
    frame[i] = src[i];
  frame[8] = 0; /* eax = 0 in the child                    */

  uint32_t *sp = frame;
  *--sp = (uint32_t)fork_child_entry;
  *--sp = 0;
  *--sp = 0;
  *--sp = 0;
  *--sp = 0; /* ebp,ebx,esi,edi          */
#endif
  t->esp = (uintptr_t)sp;

  enqueue(t);
  return (int)t->id;
}

static task_t *find_task(uint32_t id)
{
  task_t *t = &main_task;
  do {
    if (t->id == id)
      return t;
    t = t->next;
  } while (t != &main_task);
  return 0;
}

/* Unlink a (zombie) task from the run queue and free its resources, including
 * the task node itself (now heap-allocated). The unlink is done with interrupts
 * off so a timer tick can't walk the queue mid-splice or touch the node as it's
 * removed; the page-dir/stack/node frees then run normally (t is already off
 * the queue and is never `current`, so nothing can reach it). */
static void reap_task_locked(task_t *t)
{
  uintptr_t fl = irq_save();

  /* An address-space owner shares its page directory with its
   * threads. Releasing the directory below frees
   * those page tables and user frames; a thread still on the run queue would
   * then be scheduled onto a freed CR3 -> triple fault. Mark every such
   * sibling zombie now, under the same lock, so no tick can pick one before
   * we drain them. (Kernel threads/main_task use the kernel CR3 and forked
   * children own their own dir, so neither matches.) */
  if (t->as_owner) {
    task_t *s = t->next;
    while (s != t) {
      if (s != &main_task && !task_running(s) && s->as == t->as && !s->as_owner && s->state != TASK_FREE)
        s->state = TASK_ZOMBIE;
      s = s->next;
    }
  }

  task_t *p = &main_task;
  while (p->next != t)
    p = p->next;
  p->next = t->next;
  count--;
  if (fl & 0x200)
    __asm__ volatile("sti");

  if (t->as_owner) {
    /* Drain the owner's threads before releasing the shared address space:
     * close their fds (a fetch worker's socket/TCB would otherwise leak),
     * unlink, and free their nodes -- all while the dir is still valid. */
    for (;;) {
      task_t *th = 0, *s = main_task.next;
      while (s != &main_task) {
        if (!task_running(s) && s->as == t->as && !s->as_owner) {
          th = s;
          break;
        }
        s = s->next;
      }
      if (!th)
        break;
      fds_close_all(th);
      fl = irq_save();
      task_t *q = &main_task;
      while (q->next != th)
        q = q->next;
      q->next = th->next;
      count--;
      if (fl & 0x200)
        __asm__ volatile("sti");
      if (th->kstack)
        kfree(th->kstack);
      if (th->self_reap && th->state == TASK_ZOMBIE && detached_zombies > 0)
        detached_zombies--;
      address_space_put(th->as);
      kfree(th);
    }
  }
  address_space_put(t->as);
  if (t->kstack)
    kfree(t->kstack);
  kfree(t);
}

static void reap_task(task_t *t)
{
  uintptr_t flags = irq_save();
  sched_lock();
  reap_task_locked(t);
  sched_unlock();
  irq_restore(flags);
}

/* Free any detached kernel thread that has exited. Such a thread's parent is
 * main_task, which never waitpid's it, so its node + 64 KiB kstack would leak
 * as a permanent zombie. It can't free itself (it runs on that kstack), so the
 * exiting thread only marks itself and the NEXT task to run schedule() reaps
 * it. `current` is skipped — the exiting thread is still current until it
 * switches away. Called from schedule() with interrupts already off. */
static void reap_detached(void)
{
  while (detached_zombies > 0) {
    task_t *z = 0, *t = main_task.next;
    while (t != &main_task) {
      if (!task_running(t) && t->self_reap && t->state == TASK_ZOMBIE) {
        z = t;
        break;
      }
      t = t->next;
    }
    if (!z)
      break;
    reap_task_locked(z);
    detached_zombies--;
  }
}

struct fd *sched_fds(void) { return current->fds; }

/* The active address space's page directory, shared by a process and its user
 * threads. */
uint32_t *sched_current_dir(void) { return (uint32_t *)(uintptr_t)task_cr3(current); }

/* The current task's pid (owner id for windowing-layer bookkeeping). */
uint32_t sched_current_id(void) { return current->id; }

uint32_t sched_current_process_id(void) { return current->process ? current->process->id : current->id; }

/* The current task's name (for the crash reporter). */
const char *sched_current_name(void) { return current->name ? current->name : "task"; }

/* Point a (not-yet-running) process's fd at a pipe end -- used by the shell to
 * wire up `a | b` before the children are scheduled. */
void sched_set_pipe_fd(int pid, int fd, int writer, int pipe_id)
{
  task_t *t = find_task((uint32_t)pid);
  if (!t || fd < 0 || fd >= MAX_FD)
    return;
  t->fds[fd].type = writer ? FD_PIPE_WR : FD_PIPE_RD;
  t->fds[fd].pipe = (uint8_t)pipe_id;
}

/* Set a (not-yet-running) process's working directory -- the shell calls this
 * after spawn() so the child resolves relative path syscalls against the
 * directory the user is actually in, not the root. */
void sched_set_cwd(int pid, const char *path)
{
  task_t *t = find_task((uint32_t)pid);
  if (t)
    cwd_set(t->cwd, path);
}

/* Set the calling task's own cwd. The streaming-command runner thread uses this
 * to adopt the terminal's cwd before running a bridged command, so relative
 * paths (curl -o, torrent get) resolve where the synchronous bridge did. */
void sched_set_cwd_self(const char *path) { cwd_set(current->cwd, path); }

/* The current task's working directory (an absolute path), for syscall.c. */
const char *sched_cwd(void) { return current->cwd; }

/* Per-process credentials. The current task's uid/gid, and a setter that
 * switches the caller's identity (login/su, after the password check). */
uint32_t sched_uid(void) { return current->uid; }
uint32_t sched_gid(void) { return current->gid; }
void sched_set_creds_self(uint32_t uid, uint32_t gid)
{
  current->uid = uid;
  current->gid = gid;
}

/* Set a not-yet-running child's credentials (sshd spawns the login shell as the
 * authenticated user). */
void sched_set_creds(int pid, uint32_t uid, uint32_t gid)
{
  task_t *t = find_task((uint32_t)pid);
  if (t) {
    t->uid = uid;
    t->gid = gid;
  }
}

void sched_yield(void) { schedule(); /* give up the CPU; we stay RUNNABLE        */ }

/* Block the current task until absolute timer tick `wake`, fully off the run
 * queue (state TASK_WAITING) so it is NOT round-robined while asleep. This is
 * the fix for the "many windows -> whole system lags" scaling: previously
 * timer_sleep() kept the task RUNNABLE and `hlt`ed, so every sleeping thread
 * still got picked each cycle and burned a whole timer tick — N sleeping
 * threads diluted every other thread's wakeup latency to N ticks. Now a sleeper
 * costs nothing; the timer IRQ promotes it via sched_wake_due() when due.
 * The main task never sleeps (it polls + hlt), so it is the always-runnable
 * fallback and schedule() can never starve. */
void sched_sleep_until(uint32_t wake)
{
  if (!current) /* called before the scheduler exists */
    return;
  if (wake == 0) /* 0 is the "not a timed sleep" sentinel; a
                  * deadline that wrapped to exactly 0 would never
                  * wake, so nudge it to the next tick */
    wake = 1;
  uintptr_t flags = irq_save(); /* set the state + switch atomically vs the IRQ */
  current->wake_tick = wake;
  current->state = TASK_WAITING;
  schedule(); /* entered IF=0; returns once woken + rescheduled */
  current->wake_tick = 0;
  if (flags & 0x200) /* restore the caller's interrupt flag */
    __asm__ volatile("sti");
}

/* Sleep until futex_wake(addr) unless *addr != expected on entry. A timeout of
 * 0 ms waits forever. Returns 1 if the wait timed out or was interrupted. */
bool sched_futex_wait(uint32_t addr, uint32_t expected, uint32_t ms)
{
  uint32_t wake = 0;
  if (ms) {
    wake = timer_ticks() + (uint32_t)(((uint64_t)ms * TIMER_HZ + 999) / 1000);
    if (wake == 0)
      wake = 1;
  }
  uintptr_t flags = irq_save();
  sched_lock();
  if (*(volatile uint32_t *)(uintptr_t)addr != expected) {
    sched_unlock();
    irq_restore(flags);
    return false;
  }
  current->futex_key = addr;
  current->wake_tick = wake;
  current->state = TASK_WAITING;
  sched_unlock();
  schedule();
  bool unwoken = current->futex_key != 0;
  current->futex_key = 0;
  current->wake_tick = 0;
  irq_restore(flags);
  return unwoken;
}

/* Wake up to n tasks of the caller's address space sleeping on addr. */
int sched_futex_wake(uint32_t addr, int n)
{
  int woken = 0;
  uintptr_t flags = irq_save();
  sched_lock();
  task_t *t = &main_task;
  do {
    if (woken < n && t->state == TASK_WAITING && t->futex_key == addr && t->as == current->as) {
      t->futex_key = 0;
      t->wake_tick = 0;
      t->state = TASK_RUNNABLE;
      woken++;
    }
    t = t->next;
  } while (t != &main_task);
  sched_unlock();
  irq_restore(flags);
  return woken;
}

/* Promote every timed sleeper whose deadline has arrived back to RUNNABLE.
 * Called once per timer tick from the IRQ handler (so `now` only advances
 * there). Waiters that are not timed sleeps (waitpid: wake_tick == 0) are left
 * alone. */
void sched_wake_due(uint32_t now)
{
  sched_lock();
  task_t *t = &main_task;
  do {
    if (t->state == TASK_WAITING && t->wake_tick && (int32_t)(now - t->wake_tick) >= 0) {
      t->wake_tick = 0;
      t->state = TASK_RUNNABLE;
    }
    t = t->next;
  } while (t != &main_task);
  sched_unlock();
}

void sched_exit(int status)
{
  uwin_task_exiting(current->id);  /* tear down any GUI windows it owns */
  audio_task_exiting(current->id); /* release the PCM stream if it held it */
  if (!current->as || current->as_owner)
    net_sock_close_owner(sched_current_process_id());
  fds_close_all(current);
  current->exit_status = status;
  current->state = TASK_ZOMBIE;
  if (current->self_reap) /* nobody waitpid's us: queue for schedule() to free */
    detached_zombies++;

  /* Reparent live children to the permanent main task. Detached children are
   * marked for scheduler-side reaping because main never waits on them. */
  task_t *child = &main_task;
  do {
    if (child != current && child->parent == current->id) {
      child->parent = main_task.id;
      if (child->self_reap == 0) {
        child->self_reap = true;
        if (child->state == TASK_ZOMBIE)
          detached_zombies++;
      }
    }
    child = child->next;
  } while (child != &main_task);

  /* Notify the parent of our death. Post SIGCHLD only if the parent installed a
   * real handler (default disposition is ignore — posting it then would just
   * leave a stale pending bit); wake it if it's blocked in waitpid for us, or
   * if that SIGCHLD handler now wants delivery. */
  task_t *parent = find_task(current->parent);
  if (parent && parent->state != TASK_FREE && parent->state != TASK_ZOMBIE) {
    uint32_t h = parent->sig_handler[SIGCHLD];
    int chld = (h != SIG_DFL && h != SIG_IGN); /* a caught SIGCHLD */
    int chld_wake = chld && !(parent->sig_blocked & SIGBIT(SIGCHLD));
    if (chld)
      parent->sig_pending |= SIGBIT(SIGCHLD);
    if (parent->state == TASK_WAITING && (parent->wait_pid < 0 || parent->wait_pid == (int)current->id || chld_wake))
      parent->state = TASK_RUNNABLE;
  }

  schedule(); /* switch away; never returns               */
  for (;;)
    ; /* unreachable                              */
}

/* ---- signals ------------------------------------------------------------- *
 * A minimal POSIX signal model, and the ONLY interrupt mechanism for user
 * processes (it replaced the old cooperative intr latch). Signals are POSTED to
 * a task's sig_pending bitset (sched_signal_send) and DELIVERED when that task
 * is next about to return to ring 3 (sched_signal_deliver, from the isr/irq
 * tail): a caught signal runs its user handler on a kernel-built stack frame
 * whose return address is a tiny trampoline that invokes SYS_SIGRETURN to
 * restore the interrupted context; SIG_DFL falls back to the default action
 * (terminate for most, ignore for a few). A blocking kernel wait bails with
 * EINTR whenever a deliverable signal is pending (sched_intr_pending). */

/* Default action for a SIG_DFL disposition: 1 = ignore, 0 = terminate. The job-
 * control stops default to ignore until Phase B wires process groups + a tty.
 */
static bool sig_default_ignore(int sig)
{
  switch (sig) {
  case SIGCHLD:
  case SIGCONT:
  case SIGSTOP:
  case SIGTSTP:
  case SIGTTIN:
  case SIGTTOU:
    return true;
  default:
    return false;
  }
}

/* Whether task `t`'s current disposition for `sig` is "ignore" (explicit
 * SIG_IGN, or a default action that is ignore). SIGKILL/SIGSTOP are never
 * ignorable. POSIX: a signal generated while ignored is discarded, not left
 * pending. */
static bool sig_ignored(task_t *t, int sig)
{
  if (sig == SIGKILL || sig == SIGSTOP)
    return false;
  uint32_t h = t->sig_handler[sig];
  return h == SIG_IGN || (h == SIG_DFL && sig_default_ignore(sig));
}

/* Lowest-numbered signal that is pending, unblocked, and would actually act (a
 * handler, or a terminating default) — 0 if none. Drives both delivery and the
 * EINTR decision (an ignored-by-default signal never interrupts a blocking
 * wait). */
static int sig_next(task_t *t)
{
  uint32_t d = t->sig_pending & ~t->sig_blocked;
  for (int s = 1; s < NSIG; s++) {
    if (!(d & SIGBIT(s)))
      continue;
    if (s == SIGKILL)
      return s; /* forced terminate, never masked  */
    uint32_t h = t->sig_handler[s];
    if (h == SIG_IGN)
      continue;
    if (h == SIG_DFL && sig_default_ignore(s))
      continue;
    return s;
  }
  return 0;
}

/* A blocking kernel wait (tcp/net reads, waitpid, timer sleep, audio drain)
 * polls this and bails with an EINTR sentinel when a signal wants delivery, so
 * control reaches a return-to-ring-3 boundary where sched_signal_deliver runs.
 */
bool sched_intr_pending(void) { return sig_next(current) != 0; }

/* Post signal `sig` to task `pid` (the kill(2) primitive). Permission: root,
 * the caller itself, or a child of the caller. sig 0 only probes
 * existence/perm. Returns 0, or -1 (no such task / not permitted / bad signal).
 */
int sched_signal_send(int pid, int sig)
{
  if (sig < 0 || sig >= NSIG)
    return -1;
  task_t *t = find_task((uint32_t)pid);
  if (!t || t->state == TASK_FREE || t->state == TASK_ZOMBIE)
    return -1;
  if (current->uid != 0 && t->id != current->id && t->parent != current->id)
    return -1;
  if (sig == 0)
    return 0;
  if (sig_ignored(t, sig))
    return 0; /* POSIX: an ignored signal is discarded, not queued */
  t->sig_pending |= SIGBIT(sig);
  /* A blocked/sleeping target must become runnable to reach a delivery boundary
   * (this also interrupts a timer sleep, like a signal does on Unix). Only a
   * deliverable (non-ignored, handled above) signal gets here, so no ignored
   * signal ever spuriously wakes a sleeper. */
  if (t->state == TASK_WAITING)
    t->state = TASK_RUNNABLE;
  return 0;
}

/* sigaction for the current task. SIGKILL/SIGSTOP can't be caught or ignored.
 */
int sched_sigaction(int sig, const struct k_sigaction *act, struct k_sigaction *old)
{
  if (sig <= 0 || sig >= NSIG || sig == SIGKILL || sig == SIGSTOP)
    return -1;
  if (old) {
    old->handler = current->sig_handler[sig];
    old->mask = current->sig_mask[sig];
    old->flags = current->sig_flags[sig];
  }
  if (act) {
    current->sig_handler[sig] = act->handler;
    current->sig_mask[sig] = act->mask & ~(SIGBIT(SIGKILL) | SIGBIT(SIGSTOP));
    current->sig_flags[sig] = act->flags;
    if (sig_ignored(current, sig))
      current->sig_pending &= ~SIGBIT(sig); /* POSIX: setting SIG_IGN discards pending */
  }
  return 0;
}

/* sigprocmask for the current task; returns the previous blocked set. When
 * have_set is 0 it just reports the mask (how: SIG_BLOCK/UNBLOCK/SETMASK). */
uint32_t sched_sigprocmask(int how, uint32_t set, int have_set)
{
  uint32_t old = current->sig_blocked;
  if (have_set) {
    set &= ~(SIGBIT(SIGKILL) | SIGBIT(SIGSTOP)); /* never blockable */
    if (how == SIG_BLOCK)
      current->sig_blocked |= set;
    else if (how == SIG_UNBLOCK)
      current->sig_blocked &= ~set;
    else if (how == SIG_SETMASK)
      current->sig_blocked = set;
  }
  return old;
}

/* The user context saved for sigreturn — always 32-bit (user mode is 32-bit
 * even under an x86_64 kernel), so this is arch-neutral. */
struct sig_uctx {
  uint32_t eip, eflags, esp;
  uint32_t eax, ecx, edx, ebx, ebp, esi, edi;
};

/* What the kernel pushes on the user stack to run a handler. cdecl entry sees
 * [pretcode][sig], so pretcode is the handler's return address and sig its arg;
 * the handler ret's into `tramp`, which invokes SYS_SIGRETURN. */
struct sigframe {
  uint32_t pretcode;      /* -> tramp                                */
  uint32_t sig;           /* handler argument                        */
  uint32_t saved_blocked; /* mask to restore on sigreturn            */
  struct sig_uctx uc;     /* interrupted context to resume           */
  uint8_t tramp[8];       /* mov $SYS_SIGRETURN,%eax ; int $0x80      */
};

/* Build a signal-handler frame on the user stack (current CR3 is this
 * process's, so the user window is directly writable). Returns 0, or -1 if the
 * stack won't hold it. */
static int sigframe_setup(task_t *t, struct registers *r, int sig)
{
  uint32_t sp = (uint32_t)r->useresp;
  uint32_t base = (sp - sizeof(struct sigframe)) & ~3u;
  if (base < USER_BASE || base + sizeof(struct sigframe) > USER_REGION_END || base > sp) /* underflow guard        */
    return -1;

  struct sigframe *f = (struct sigframe *)(uintptr_t)base;
  uint32_t tramp = base + (uint32_t)__builtin_offsetof(struct sigframe, tramp);
  f->pretcode = tramp;
  f->sig = (uint32_t)sig;
  f->saved_blocked = t->sig_blocked;
  f->uc.eip = (uint32_t)r->eip;
  f->uc.eflags = (uint32_t)r->eflags;
  f->uc.esp = (uint32_t)r->useresp;
  f->uc.eax = (uint32_t)r->eax;
  f->uc.ecx = (uint32_t)r->ecx;
  f->uc.edx = (uint32_t)r->edx;
  f->uc.ebx = (uint32_t)r->ebx;
  f->uc.ebp = (uint32_t)r->ebp;
  f->uc.esi = (uint32_t)r->esi;
  f->uc.edi = (uint32_t)r->edi;
  f->tramp[0] = 0xB8; /* mov $imm32, %eax        */
  f->tramp[1] = (uint8_t)(SYS_SIGRETURN);
  f->tramp[2] = (uint8_t)(SYS_SIGRETURN >> 8);
  f->tramp[3] = (uint8_t)(SYS_SIGRETURN >> 16);
  f->tramp[4] = (uint8_t)(SYS_SIGRETURN >> 24);
  f->tramp[5] = 0xCD; /* int $0x80               */
  f->tramp[6] = 0x80;
  f->tramp[7] = 0x90; /* nop pad                 */

  r->useresp = base;
  r->eip = t->sig_handler[sig];
  return 0;
}

/* Deliver at most one pending signal to the current task as it returns to
 * ring 3. A default-terminate never returns (sched_exit); a handler is set up
 * and iret runs it. Called from the isr/irq tail. */
void sched_signal_deliver(struct registers *r)
{
  if ((r->cs & 3) != 3) /* only when returning to a user context */
    return;
  task_t *t = current;
  int sig = sig_next(t);
  if (!sig)
    return;
  t->sig_pending &= ~SIGBIT(sig);

  uint32_t h = t->sig_handler[sig];
  if (sig == SIGKILL || h == SIG_DFL) {
    /* sig_next already dropped ignore-by-default signals, so a SIG_DFL that
     * reaches here terminates (128 + sig, the shell's "killed by" convention).
     */
    sched_exit(128 + sig); /* never returns */
  }
  if (sigframe_setup(t, r, sig) < 0)
    sched_exit(128 + sig); /* unusable stack: fall back to terminate */
  /* Mask this signal (+ the handler's own mask) while the handler runs;
   * sigreturn restores the previous mask. One handler per boundary — the rest
   * deliver on the next return to ring 3. */
  t->sig_blocked |= SIGBIT(sig) | t->sig_mask[sig];
}

/* Turn a synchronous CPU fault (page fault, GP fault, #DE, #UD …) into a
 * delivered signal when the faulting task has a real handler installed for it,
 * so a program can recover from its own fault instead of being killed. Returns
 * 1 when the frame was built (isr_handler iret's into the handler), or 0 when
 * the fault is uncaught (no handler, or blocked, or an unusable stack) and the
 * caller must crash+exit.
 *
 * Unlike an asynchronous kill(2) signal this delivers immediately in the
 * fault's own context; the saved EIP points AT the faulting instruction, so a
 * handler that returns via sigreturn re-runs it and re-faults. A crash handler
 * must therefore not return normally — it longjmp()s out to a recovery point.
 */
int sched_fault_signal(struct registers *r, int sig)
{
  task_t *t = current;
  uint32_t h = t->sig_handler[sig];
  if (h == SIG_DFL || h == SIG_IGN) /* no user handler: fatal, as before   */
    return 0;
  if (t->sig_blocked & SIGBIT(sig))  /* fault while the signal is blocked    */
    return 0;                        /* -> can't deliver safely: terminate   */
  if (sigframe_setup(t, r, sig) < 0) /* stack won't hold the frame           */
    return 0;
  t->sig_blocked |= SIGBIT(sig) | t->sig_mask[sig];
  return 1;
}

/* SYS_SIGRETURN: the trampoline lands here after the handler returned. The
 * handler's ret popped the 4-byte pretcode, so useresp points just past it —
 * the frame base is useresp-4. Restore the saved context + mask so the
 * interrupted code resumes exactly where it was. */
int sched_sigreturn(struct registers *r)
{
  uint32_t base = (uint32_t)r->useresp - 4;
  if (base < USER_BASE || base + sizeof(struct sigframe) > USER_REGION_END)
    return -1;
  struct sigframe *f = (struct sigframe *)(uintptr_t)base;

  /* The frame is user-writable: strip SIGKILL/SIGSTOP so a crafted saved mask
   * can't make the process unkillable (same guarantee as sched_sigprocmask). */
  current->sig_blocked = f->saved_blocked & ~(SIGBIT(SIGKILL) | SIGBIT(SIGSTOP));
  r->eip = f->uc.eip;
  r->useresp = f->uc.esp;
  /* Restore user flags but sanitize: keep only arithmetic/DF/TF bits, force IF
   * on
   * + reserved bit1 — a crafted frame must not gain IOPL/NT. */
  r->eflags = (f->uc.eflags & 0x00000DD5u) | 0x00000202u;
  r->eax = f->uc.eax;
  r->ecx = f->uc.ecx;
  r->edx = f->uc.edx;
  r->ebx = f->uc.ebx;
  r->ebp = f->uc.ebp;
  r->esi = f->uc.esi;
  r->edi = f->uc.edi;
  return 0;
}

/* Find an exited child of `current` matching pid (-1 = any). */
static task_t *zombie_child(int pid)
{
  task_t *t = &main_task;
  do {
    if (t->state == TASK_ZOMBIE && !task_running(t) && t->parent == current->id && (pid < 0 || (int)t->id == pid))
      return t;
    t = t->next;
  } while (t != &main_task);
  return 0;
}

static bool has_child(int pid)
{
  task_t *t = &main_task;
  do {
    if (t->parent == current->id && (pid < 0 || (int)t->id == pid))
      return true;
    t = t->next;
  } while (t != &main_task);
  return false;
}

/* Block until a child exits; reap it and return its status (-1 if none). */
int sched_waitpid(int pid)
{
  for (;;) {
    task_t *z = zombie_child(pid);
    if (z) {
      int status = z->exit_status;
      reap_task(z);
      return status;
    }
    if (!has_child(pid))
      return -1;
    current->state = TASK_WAITING;
    current->wait_pid = pid;
    schedule();               /* yield; woken when a child exits          */
    if (sched_intr_pending()) /* a signal wants delivery: bail (EINTR)    */
      return -1;
  }
}

/* Kernel-side, non-blocking reap (used by the shell's foreground wait). */
bool sched_try_reap(int pid, int *status)
{
  task_t *z = zombie_child(pid);
  if (!z)
    return false;
  *status = z->exit_status;
  reap_task(z);
  return true;
}

/* Replace the current process's image: swap in a new address space and set up
 * the trap frame so the syscall returns into the new program's entry. */
int sched_replace(uint32_t *new_dir, uint32_t entry, uint32_t user_esp, uint32_t brk, struct registers *r)
{
  address_space_t *next = kmalloc(sizeof *next);
  address_space_t *old = current->as;
  if (!next)
    return -1;
  next->dir = new_dir;
  next->refs = 1;
  next->brk = next->brk_base = brk;
  paging_load(new_dir);
  current->as = next;
  current->as_owner = true;
  current->process = current;
  address_space_put(old);

  /* exec resets caught handlers to SIG_DFL and drops pending signals; the
   * blocked mask is preserved (POSIX). The new image can't keep the old code's
   * handlers — their addresses point into the freed address space. */
  current->sig_pending = 0;
  for (int i = 0; i < NSIG; i++) {
    current->sig_handler[i] = SIG_DFL;
    current->sig_mask[i] = 0;
    current->sig_flags[i] = 0;
  }

  r->eip = entry;
  r->cs = 0x1B;
  r->eflags = 0x202;
  r->useresp = user_esp;
  r->ss = 0x23;
  r->ds = 0x23;
  r->eax = r->ebx = r->ecx = r->edx = 0;
  r->esi = r->edi = r->ebp = 0;
  return 0;
}

void schedule(void)
{
  /* Pick-next + context_switch must be ATOMIC against the timer IRQ. The
   * cooperative entry — sched_yield() from timer_sleep/tcp_read/tcp_accept/...
   * — runs with IF=1 (waiters do `sti; hlt`). Without disabling interrupts
   * here, a timer tick landing in the window between choosing `next` and the
   * stack swap inside context_switch() re-enters schedule()/context_switch()
   * on the SAME task, crossing the saved kernel ESPs and later resuming a
   * thread into a garbage return address — seen as a GP fault to EIP
   * 0x0000000D in whatever thread happened to be yielding, with the rest of
   * its register state reflecting that victim. The IRQ entry already runs
   * IF=0, so it is unaffected. We restore the CALLER's IF after we are resumed
   * (possibly many switches later, on this thread's own stack): IF=0 callers
   * stay IF=0, cooperative IF=1 waiters resume with interrupts back on. The
   * saved `flags` is a local, so it rides this thread's stack across switches.
   * context_switch does not touch EFLAGS, so IF stays cleared across the swap
   * until this restore (or thread_start's own `sti` for a first run). */
  uintptr_t flags = irq_save();
  int cpu = smp_this_cpu();

  sched_lock();
  switching_tasks[cpu] = 0;
  reap_detached(); /* free any exited detached kernel thread    */

  task_t *prev = current_tasks[cpu];
  task_t *idle = cpu == 0 ? &main_task : &idle_tasks[cpu];
  prev->running_cpu = -1;
  task_t *next = prev == idle ? main_task.next : prev->next;
  while (next != &main_task && (next->state != TASK_RUNNABLE || task_running(next)))
    next = next->next; /* always finds main if nothing else        */
  if (next == &main_task && (cpu != 0 || main_task.state != TASK_RUNNABLE || task_running(&main_task)))
    next = 0;
  if (!next)
    next = idle;
  next->running_cpu = cpu;
  if (next != prev) {
    sched_kernel_switch_out();
    switching_tasks[cpu] = prev;
    current_tasks[cpu] = next;
    current->runs++;
    if (current->kstack_top)
      tss_set_kernel_stack(current->kstack_top);
    if (task_cr3(current) != task_cr3(prev))
      paging_load((uint32_t *)(uintptr_t)task_cr3(current));
    sched_unlock();
    context_switch(&prev->esp, current->esp);
    sched_kernel_switch_in();
  } else {
    sched_unlock();
  }

  if (flags & 0x200) /* restore the caller's interrupt flag       */
    __asm__ volatile("sti");
}

int sched_count(void) { return count; }

void sched_dump(void)
{
  con_write("id  name        runs\n");
  task_t *t = &main_task;
  do {
    con_printf("%u   %s\t%u\n", t->id, t->name, t->runs);
    t = t->next;
  } while (t != &main_task);
}
