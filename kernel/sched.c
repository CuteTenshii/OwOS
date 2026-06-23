/* sched.c — round-robin scheduling of kernel threads and user processes. */

#include "sched.h"
#include "heap.h"
#include "timer.h"
#include "console.h"
#include "gdt.h"
#include "paging.h"
#include "loader.h"
#include "idt.h"
#include "file.h"
#include "pipe.h"
#include "irqflags.h"
#include "ipc.h"
#include "uwin.h"
#include <stdint.h>

/* Tasks are HEAP-ALLOCATED — a `kmalloc`'d task_t per task, linked into the
 * circular run queue. There is no fixed task-table cap: the only ceiling is the
 * kernel heap, and `alloc_task()` fails gracefully (the spawn returns -1, e.g.
 * "failed to launch ...") when it's exhausted, rather than at an arbitrary array
 * bound. Each task also kmalloc's a KSTACK_SIZE kernel stack, so kstacks are what
 * actually dominate heap use as the task count grows. */
/* Every timer tick, timer_handler() runs nic_poll() -> the full IP/TCP/UDP RX
 * path BEFORE schedule(), nested in the IRQ on whatever thread it interrupted
 * (real hosts/VMware poll the NIC off the tick because its INTx never reaches
 * our PIC). So a thread's worst-case stack is its own depth PLUS a nested
 * receive stack. The wm compositor mid-compose() + a nested RX overran 16 KiB
 * on real hardware, silently smashing the adjacent thread's stack (a GP fault
 * resuming an unrelated thread into EIP 0x0000000D). Size for both. */
#define KSTACK_SIZE 65536   /* 64K: sshd runs the SSH KEX crypto (x25519/ed25519, deep
                             * stacks) on a kernel thread, and a NIC-RX IRQ for the next
                             * handshake packet nests its full RX/TCP chain on top — 32K
                             * overflowed and corrupted an adjacent task struct. */
#define UKSTACK_SIZE 32768  /* user task kernel stack: a timer IRQ mid-syscall nests
                             * nic_poll()'s full RX/TCP chain on top of the syscall's
                             * frames (no ring0->ring0 stack switch), so it needs the
                             * same headroom as KSTACK_SIZE, not half. 16K overflowed
                             * into an adjacent task on real HW (busy LAN) -> the
                             * victim resumed into EIP 0x0000000D. */
#define TASK_CWD_MAX 128    /* must match the shell's PATH_MAX                  */

enum { TASK_FREE = 0, TASK_RUNNABLE, TASK_WAITING, TASK_ZOMBIE };

typedef struct task {
	uint32_t      esp;         /* saved kernel stack pointer (the context)     */
	uint32_t      cr3;         /* page directory physical address              */
	uint32_t     *dir;         /* process page dir to free (0 for threads)     */
	void         *kstack;      /* kernel stack to free (0 for the main task)   */
	uint32_t      kstack_top;  /* TSS esp0 for this task (0 = leave it alone)  */
	int           state;
	uint32_t      id;
	uint32_t      parent;      /* pid of the task that created it              */
	int           wait_pid;    /* pid being waited on while WAITING            */
	uint32_t      wake_tick;   /* if WAITING for a timed sleep: tick to wake at
	                            * (0 = not a timed sleep, e.g. a waitpid waiter) */
	int           exit_status; /* set on exit, read by waitpid                 */
	volatile int  intr;        /* cooperative-interrupt latch (Ctrl+C / SIGINT) */
	const char   *name;
	char          namebuf[16]; /* owns `name` for spawned programs (argv[0])    */
	uint32_t      runs;
	struct fd     fds[MAX_FD]; /* open file descriptors                        */
	char          cwd[TASK_CWD_MAX]; /* working dir; relative path syscalls join here */
	uint32_t      uid;         /* owning user id (credentials; inherited like cwd)    */
	uint32_t      gid;         /* owning primary group id                             */
	uint32_t      brk;         /* user-space heap break; 0 = no heap (thread/kthread) */
	uint32_t      brk_base;    /* heap start (image end); sbrk can't shrink below it  */
	struct task  *next;
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

/* Drop references this task held on any pipe ends. */
static void fds_close_all(task_t *t)
{
	for (int i = 0; i < MAX_FD; i++) {
		if (t->fds[i].type == FD_PIPE_RD)
			pipe_close(t->fds[i].pipe, 0);
		else if (t->fds[i].type == FD_PIPE_WR)
			pipe_close(t->fds[i].pipe, 1);
		else if (t->fds[i].type == FD_CHANNEL) {
			pipe_close(t->fds[i].pipe, 0);     /* rx reader */
			pipe_close(t->fds[i].tx, 1);       /* tx writer */
		} else if (t->fds[i].type == FD_PORT)
			ipc_port_destroy(t->fds[i].pipe);
		t->fds[i].type = FD_NONE;
	}
}

/* From switch.S. */
extern void context_switch(uint32_t *old_esp, uint32_t new_esp);
extern void thread_start(void);
extern void enter_user_task(void);
extern void fork_child_entry(void);

static task_t main_task;       /* permanent head of the circular run queue (never freed) */
static int    count;
static uint32_t next_id;
static task_t *current;

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

/* The task that owns `t`'s heap: itself if it's a process (dir != 0), else the
 * process whose address space it shares (a thread, dir == 0, same cr3). The heap
 * (break + frames) is per-address-space, so every thread sbrk's the one owner. */
static task_t *heap_owner_of(task_t *t)
{
	if (t->dir)
		return t;
	task_t *p = &main_task;
	do {
		if (p->dir && p->cr3 == t->cr3)
			return p;
		p = p->next;
	} while (p != &main_task);
	return t;
}

/* Splice t into the run queue right after the main task. The single publishing
 * write (main_task.next = t) makes t visible atomically, so a timer IRQ walking
 * the queue mid-enqueue always sees a consistent list. */
static void enqueue(task_t *t)
{
	t->next = main_task.next;
	main_task.next = t;
	count++;
}

void thread_exit(void)
{
	for (;;)
		__asm__ volatile ("hlt");
}

void sched_init(void)
{
	task_t *t = &main_task;
	t->id = next_id++;
	t->name = "main";
	t->runs = 1;
	t->esp = 0;                    /* filled in on the first switch away       */
	t->cr3 = paging_kernel_phys();
	t->dir = 0;
	t->kstack = 0;
	t->kstack_top = 0;             /* a kernel thread: never traps to ring 0   */
	t->state = TASK_RUNNABLE;
	t->wake_tick = 0;
	t->intr = 0;
	t->brk = t->brk_base = 0;      /* no user heap                             */
	t->uid = t->gid = 0;           /* the boot session runs as root            */
	cwd_set(t->cwd, "/");
	t->next = t;
	current = t;
	count = 1;

	timer_on_tick(schedule);
}

void sched_create_thread(const char *name, void (*entry)(void))
{
	task_t *t = alloc_task();
	if (!t)
		return;
	t->id = next_id++;
	t->name = name;
	t->runs = 0;
	t->cr3 = paging_kernel_phys();
	t->dir = 0;
	t->kstack_top = 0;
	t->state = TASK_RUNNABLE;
	t->brk = t->brk_base = 0;       /* kernel thread: no user heap             */

	uint8_t *stack = kmalloc(KSTACK_SIZE);
	t->intr = 0;
	t->kstack = stack;
	uint32_t *sp = (uint32_t *)(stack + KSTACK_SIZE);
	*--sp = (uint32_t)thread_exit;
	*--sp = (uint32_t)entry;
	*--sp = (uint32_t)thread_start;
	*--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;   /* ebp,ebx,esi,edi          */
	t->esp = (uint32_t)sp;

	enqueue(t);
}

int sched_create_process(const char *name, uint32_t entry, uint32_t *dir,
                         uint32_t user_esp, uint32_t brk)
{
	task_t *t = alloc_task();
	if (!t)
		return -1;
	t->id = next_id++;
	t->brk = t->brk_base = brk;     /* the process's heap (shared by its threads) */
	{	/* copy the program name so it persists (callers pass argv[0]) */
		int i = 0;
		for (; name && name[i] && i < (int)sizeof t->namebuf - 1; i++)
			t->namebuf[i] = name[i];
		t->namebuf[i] = '\0';
		t->name = t->namebuf;
	}
	t->runs = 0;
	t->cr3 = (uint32_t)dir;
	t->dir = dir;
	t->parent = current->id;
	t->state = TASK_RUNNABLE;
	t->intr = 0;
	fds_init_console(t);
	cwd_set(t->cwd, current->cwd);   /* inherit the spawner's cwd by default */
	t->uid = current->uid;           /* and its credentials (uid/gid)         */
	t->gid = current->gid;

	uint8_t *kstack = kmalloc(UKSTACK_SIZE);
	t->kstack = kstack;
	t->kstack_top = (uint32_t)(kstack + UKSTACK_SIZE);

	/* Craft the kernel stack so the first schedule restores it, rets into
	 * enter_user_task, and irets to ring 3 at `entry`. */
	uint32_t *sp = (uint32_t *)t->kstack_top;
	*--sp = 0x23;                   /* user SS                                 */
	*--sp = user_esp;               /* user ESP (points at argc)               */
	*--sp = 0x202;                  /* EFLAGS (IF set)                         */
	*--sp = 0x1B;                   /* user CS                                 */
	*--sp = entry;                  /* user EIP                                */
	*--sp = (uint32_t)enter_user_task;
	*--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;   /* ebp,ebx,esi,edi          */
	t->esp = (uint32_t)sp;

	enqueue(t);
	return (int)t->id;
}

/* Move the caller's process break by `inc` bytes (the malloc primitive), mapping
 * fresh zeroed window pages on growth or freeing whole pages on shrink. Returns
 * the PREVIOUS break, or (uint32_t)-1 on failure. Runs in the int 0x80 gate with
 * IF clear, so it is atomic against the other threads that share this heap — no
 * lock needed on the kernel side. */
uint32_t sched_sbrk(int inc)
{
	task_t *o = heap_owner_of(current);
	if (o->brk == 0)                          /* not a process with a heap */
		return (uint32_t)-1;
	uint32_t old = o->brk;
	if (inc == 0)
		return old;
	if (inc > 0) {
		uint32_t add = (uint32_t)inc;
		if (old >= USER_HEAP_MAX || add > USER_HEAP_MAX - old)
			return (uint32_t)-1;              /* would run into the user stack */
		uint32_t newbrk  = old + add;
		uint32_t old_top = (old + 0xFFFu) & ~0xFFFu;
		uint32_t new_top = (newbrk + 0xFFFu) & ~0xFFFu;
		if (new_top > old_top && paging_user_grow(o->dir, old_top, new_top) < 0)
			return (uint32_t)-1;              /* out of physical memory */
		o->brk = newbrk;
		return old;
	}
	uint32_t dec = (uint32_t)(-inc);
	if (dec > old - o->brk_base)               /* never shrink below the heap base */
		dec = old - o->brk_base;
	uint32_t newbrk  = old - dec;
	uint32_t old_top = (old + 0xFFFu) & ~0xFFFu;
	uint32_t new_top = (newbrk + 0xFFFu) & ~0xFFFu;
	if (old_top > new_top)
		paging_user_shrink(o->dir, new_top, old_top);
	o->brk = newbrk;
	return old;
}

/* Reset the caller's heap base + break (the image-replace syscall, after it swaps
 * in a new program image whose end is the new heap start). */
void sched_set_brk(uint32_t brk)
{
	task_t *o = heap_owner_of(current);
	o->brk = o->brk_base = brk;
}

/* Spawn a thread inside the CURRENT process: a new task that SHARES the
 * caller's address space (same CR3) and runs `entry` in ring 3 on the
 * caller-provided user stack. dir=0 so its reap never frees the shared address
 * space — that belongs to the process's main task. It is an ordinary child of
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
	t->cr3 = current->cr3;          /* share the creator's address space      */
	t->dir = 0;                     /* not the owner: don't free it on reap    */
	t->brk = t->brk_base = 0;       /* heap belongs to the process; sbrk redirects */
	t->parent = current->id;
	t->state = TASK_RUNNABLE;
	t->intr = 0;
	fds_init_console(t);
	cwd_set(t->cwd, current->cwd);
	t->uid = current->uid;
	t->gid = current->gid;

	uint8_t *kstack = kmalloc(UKSTACK_SIZE);
	if (!kstack) { kfree(t); return -1; }
	t->kstack = kstack;
	t->kstack_top = (uint32_t)(kstack + UKSTACK_SIZE);

	uint32_t *sp = (uint32_t *)t->kstack_top;
	*--sp = 0x23;                   /* user SS                                 */
	*--sp = user_esp;               /* user ESP                                */
	*--sp = 0x202;                  /* EFLAGS (IF set)                         */
	*--sp = 0x1B;                   /* user CS                                 */
	*--sp = entry;                  /* user EIP                                */
	*--sp = (uint32_t)enter_user_task;
	*--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;   /* ebp,ebx,esi,edi          */
	t->esp = (uint32_t)sp;

	enqueue(t);
	return (int)t->id;
}

int sched_fork(struct registers *r)
{
	task_t *t = alloc_task();
	if (!t)
		return -1;

	uint32_t *child_dir = paging_fork_dir(current->dir);
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
	t->cr3 = (uint32_t)child_dir;
	t->dir = child_dir;
	t->parent = current->id;
	t->kstack = kstack;
	t->kstack_top = (uint32_t)(kstack + UKSTACK_SIZE);
	t->state = TASK_RUNNABLE;
	t->intr = 0;
	/* The child owns a private copy of the parent's address space (paging_fork_dir
	 * duplicated every window page, heap included), so it inherits the break. */
	{	task_t *o = heap_owner_of(current);
		t->brk = o->brk; t->brk_base = o->brk_base; }
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
			pipe_ref(t->fds[i].pipe, 0);   /* rx reader */
			pipe_ref(t->fds[i].tx, 1);     /* tx writer */
		} else if (t->fds[i].type == FD_PORT)
			t->fds[i].type = FD_NONE;      /* a listen port stays single-owner */
	}

	/* Copy the parent's 16-dword syscall trap frame to the top of the child's
	 * kernel stack, forcing the child's saved eax (index 8) to 0 so fork()
	 * returns 0 in the child. Then a context_switch frame that lands in
	 * fork_child_entry. */
	uint32_t *frame = (uint32_t *)t->kstack_top - 16;
	const uint32_t *src = (const uint32_t *)r;
	for (int i = 0; i < 16; i++)
		frame[i] = src[i];
	frame[8] = 0;                   /* eax = 0 in the child                    */

	uint32_t *sp = frame;
	*--sp = (uint32_t)fork_child_entry;
	*--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;   /* ebp,ebx,esi,edi          */
	t->esp = (uint32_t)sp;

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
 * removed; the page-dir/stack/node frees then run normally (t is already off the
 * queue and is never `current`, so nothing can reach it). */
static void reap_task(task_t *t)
{
	uintptr_t fl = irq_save();
	task_t *p = &main_task;
	while (p->next != t)
		p = p->next;
	p->next = t->next;
	count--;
	if (fl & 0x200)
		__asm__ volatile ("sti");

	if (t->dir)
		paging_free_process_dir(t->dir);
	if (t->kstack)
		kfree(t->kstack);
	kfree(t);
}

struct fd *sched_fds(void)
{
	return current->fds;
}

/* The current task's page directory (for the windowing layer to map a shared
 * buffer into the calling process's user window). */
uint32_t *sched_current_dir(void)
{
	return current->dir;
}

/* The current task's pid (owner id for windowing-layer bookkeeping). */
uint32_t sched_current_id(void)
{
	return current->id;
}

/* The current task's name (for the crash reporter). */
const char *sched_current_name(void)
{
	return current->name ? current->name : "task";
}

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
void sched_set_cwd_self(const char *path)
{
	cwd_set(current->cwd, path);
}

/* The current task's working directory (an absolute path), for syscall.c. */
const char *sched_cwd(void)
{
	return current->cwd;
}

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
	if (t) { t->uid = uid; t->gid = gid; }
}

void sched_yield(void)
{
	schedule();                    /* give up the CPU; we stay RUNNABLE        */
}

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
	if (!current)                      /* called before the scheduler exists */
		return;
	uintptr_t flags = irq_save();      /* set the state + switch atomically vs the IRQ */
	current->wake_tick = wake;
	current->state = TASK_WAITING;
	schedule();                        /* entered IF=0; returns once woken + rescheduled */
	current->wake_tick = 0;
	if (flags & 0x200)                 /* restore the caller's interrupt flag */
		__asm__ volatile ("sti");
}

/* Promote every timed sleeper whose deadline has arrived back to RUNNABLE.
 * Called once per timer tick from the IRQ handler (so `now` only advances there).
 * Waiters that are not timed sleeps (waitpid: wake_tick == 0) are left alone. */
void sched_wake_due(uint32_t now)
{
	task_t *t = &main_task;
	do {
		if (t->state == TASK_WAITING && t->wake_tick &&
		    (int32_t)(now - t->wake_tick) >= 0) {
			t->wake_tick = 0;
			t->state = TASK_RUNNABLE;
		}
		t = t->next;
	} while (t != &main_task);
}

void sched_exit(int status)
{
	uwin_task_exiting(current->id);   /* tear down any GUI windows it owns */
	fds_close_all(current);
	current->exit_status = status;
	current->state = TASK_ZOMBIE;

	/* Wake the parent if it is blocked waiting for us. */
	task_t *parent = find_task(current->parent);
	if (parent && parent->state == TASK_WAITING &&
	    (parent->wait_pid < 0 || parent->wait_pid == (int)current->id))
		parent->state = TASK_RUNNABLE;

	schedule();                    /* switch away; never returns               */
	for (;;)
		;                          /* unreachable                              */
}

/* Terminate another task by pid (the streaming bridge aborts a CLI child on
 * Ctrl+C / window close). Make it a zombie so the scheduler stops running it and
 * its parent reaps it; tear down the windows and fds it owns. The task isn't
 * `current` (the killer runs in another context) and this OS is cooperative with
 * no kernel locks, so abandoning its in-kernel state is safe -- it never resumes.
 * Done with interrupts off so a tick can't schedule it mid-teardown. */
void sched_kill(int pid)
{
	uintptr_t fl = irq_save();
	task_t *t = find_task((uint32_t)pid);
	if (t && t != current && t->state != TASK_FREE && t->state != TASK_ZOMBIE) {
		t->exit_status = -1;
		t->state = TASK_ZOMBIE;       /* unschedulable from here on            */
		uwin_task_exiting(t->id);
		fds_close_all(t);
		task_t *parent = find_task(t->parent);
		if (parent && parent->state == TASK_WAITING &&
		    (parent->wait_pid < 0 || parent->wait_pid == (int)t->id))
			parent->state = TASK_RUNNABLE;
	}
	if (fl & 0x200) __asm__ volatile ("sti");
}

/* Kill a task only if it is a child of the caller -- the guard behind SYS_KILL,
 * so a process can abort a command it spawned (e.g. on window close) but
 * not an unrelated task. Returns 0 on a kill, -1 if pid isn't our live child. */
int sched_kill_child(int pid)
{
	task_t *t = find_task((uint32_t)pid);
	if (!t || t->parent != current->id ||
	    t->state == TASK_FREE || t->state == TASK_ZOMBIE)
		return -1;
	sched_kill(pid);
	return 0;
}

/* Cooperative interrupt (a minimal SIGINT). sched_set_intr raises the latch on a
 * task (Ctrl+C targets the foreground child); blocking kernel waits peek it via
 * sched_intr_pending() and bail with an EINTR sentinel; the program clears it
 * with sched_intr_take() (the SYS_INTR syscall) once it has handled the abort. */
void sched_set_intr(int pid)
{
	task_t *t = find_task((uint32_t)pid);
	if (t)
		t->intr = 1;
}

int sched_intr_pending(void)        /* peek the current task's latch          */
{
	return current->intr;
}

int sched_intr_take(void)           /* read and clear it (acknowledge)        */
{
	int v = current->intr;
	current->intr = 0;
	return v;
}

/* Find an exited child of `current` matching pid (-1 = any). */
static task_t *zombie_child(int pid)
{
	task_t *t = &main_task;
	do {
		if (t->state == TASK_ZOMBIE && t->parent == current->id &&
		    (pid < 0 || (int)t->id == pid))
			return t;
		t = t->next;
	} while (t != &main_task);
	return 0;
}

static int has_child(int pid)
{
	task_t *t = &main_task;
	do {
		if (t->parent == current->id && (pid < 0 || (int)t->id == pid))
			return 1;
		t = t->next;
	} while (t != &main_task);
	return 0;
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
		schedule();                /* yield; woken when a child exits          */
	}
}

/* Kernel-side, non-blocking reap (used by the shell's foreground wait). */
int sched_try_reap(int pid, int *status)
{
	task_t *z = zombie_child(pid);
	if (!z)
		return 0;
	*status = z->exit_status;
	reap_task(z);
	return 1;
}

/* Replace the current process's image: swap in a new address space and set up
 * the trap frame so the syscall returns into the new program's entry. */
void sched_replace(uint32_t *new_dir, uint32_t entry, uint32_t user_esp,
                   struct registers *r)
{
	uint32_t *old = current->dir;
	paging_load(new_dir);
	current->dir = new_dir;
	current->cr3 = (uint32_t)new_dir;
	if (old)
		paging_free_process_dir(old);

	r->eip = entry;
	r->cs = 0x1B;
	r->eflags = 0x202;
	r->useresp = user_esp;
	r->ss = 0x23;
	r->ds = 0x23;
	r->eax = r->ebx = r->ecx = r->edx = 0;
	r->esi = r->edi = r->ebp = 0;
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

	task_t *prev = current;
	task_t *next = prev->next;
	while (next->state != TASK_RUNNABLE)
		next = next->next;         /* always finds main if nothing else        */
	if (next != prev) {
		current = next;
		current->runs++;
		if (current->kstack_top)
			tss_set_kernel_stack(current->kstack_top);
		if (current->cr3 != prev->cr3)
			paging_load((uint32_t *)current->cr3);
		context_switch(&prev->esp, current->esp);
	}

	if (flags & 0x200)             /* restore the caller's interrupt flag       */
		__asm__ volatile ("sti");
}

int sched_count(void)
{
	return count;
}

void sched_dump(void)
{
	con_write("id  name        runs\n");
	task_t *t = &main_task;
	do {
		con_write_dec(t->id);
		con_write("   ");
		con_write(t->name);
		con_write("\t");
		con_write_dec(t->runs);
		con_putc('\n');
		t = t->next;
	} while (t != &main_task);
}
