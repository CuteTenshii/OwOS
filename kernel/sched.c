/* sched.c — round-robin scheduling of kernel threads and user processes. */

#include "sched.h"
#include "heap.h"
#include "timer.h"
#include "vga.h"
#include "gdt.h"
#include "paging.h"
#include "loader.h"
#include "idt.h"
#include "file.h"
#include "pipe.h"
#include "uwin.h"
#include <stdint.h>

#define MAX_TASKS   16
#define KSTACK_SIZE 16384
#define UKSTACK_SIZE 8192   /* user task kernel stack (room for trap frames)   */
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
	int           exit_status; /* set on exit, read by waitpid                 */
	const char   *name;
	char          namebuf[16]; /* owns `name` for spawned programs (argv[0])    */
	uint32_t      runs;
	struct fd     fds[MAX_FD]; /* open file descriptors                        */
	char          cwd[TASK_CWD_MAX]; /* working dir; relative path syscalls join here */
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
		t->fds[i].type = FD_NONE;
	}
}

/* From switch.S. */
extern void context_switch(uint32_t *old_esp, uint32_t new_esp);
extern void thread_start(void);
extern void enter_user_task(void);
extern void fork_child_entry(void);

static task_t tasks[MAX_TASKS];
static int    count;
static uint32_t next_id;
static task_t *current;

static task_t *alloc_task(void)
{
	for (int i = 0; i < MAX_TASKS; i++)
		if (tasks[i].state == TASK_FREE)
			return &tasks[i];
	return 0;
}

/* Splice t into the run queue right after the main task. */
static void enqueue(task_t *t)
{
	t->next = tasks[0].next;
	tasks[0].next = t;
	count++;
}

void thread_exit(void)
{
	for (;;)
		__asm__ volatile ("hlt");
}

void sched_init(void)
{
	for (int i = 0; i < MAX_TASKS; i++)
		tasks[i].state = TASK_FREE;

	task_t *t = &tasks[0];
	t->id = next_id++;
	t->name = "main";
	t->runs = 1;
	t->esp = 0;                    /* filled in on the first switch away       */
	t->cr3 = paging_kernel_phys();
	t->dir = 0;
	t->kstack = 0;
	t->kstack_top = 0;             /* a kernel thread: never traps to ring 0   */
	t->state = TASK_RUNNABLE;
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

	uint8_t *stack = kmalloc(KSTACK_SIZE);
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
                         uint32_t user_esp)
{
	task_t *t = alloc_task();
	if (!t)
		return -1;
	t->id = next_id++;
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
	fds_init_console(t);
	cwd_set(t->cwd, current->cwd);   /* inherit the spawner's cwd by default */

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
	t->parent = current->id;
	t->state = TASK_RUNNABLE;
	fds_init_console(t);
	cwd_set(t->cwd, current->cwd);

	uint8_t *kstack = kmalloc(UKSTACK_SIZE);
	if (!kstack) { t->state = TASK_FREE; return -1; }
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
	if (!child_dir)
		return -1;

	uint8_t *kstack = kmalloc(UKSTACK_SIZE);
	if (!kstack) {
		paging_free_process_dir(child_dir);
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
	cwd_set(t->cwd, current->cwd);

	/* The child inherits the parent's open files (Unix semantics). */
	for (int i = 0; i < MAX_FD; i++) {
		t->fds[i] = current->fds[i];
		if (t->fds[i].type == FD_PIPE_RD)
			pipe_ref(t->fds[i].pipe, 0);
		else if (t->fds[i].type == FD_PIPE_WR)
			pipe_ref(t->fds[i].pipe, 1);
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
	for (int i = 0; i < MAX_TASKS; i++)
		if (tasks[i].state != TASK_FREE && tasks[i].id == id)
			return &tasks[i];
	return 0;
}

/* Unlink a (zombie) task from the run queue and free its resources. */
static void reap_task(task_t *t)
{
	task_t *p = current;
	while (p->next != t)
		p = p->next;
	p->next = t->next;
	if (t->dir)
		paging_free_process_dir(t->dir);
	if (t->kstack)
		kfree(t->kstack);
	t->state = TASK_FREE;
	count--;
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

/* The current task's working directory (an absolute path), for syscall.c. */
const char *sched_cwd(void)
{
	return current->cwd;
}

void sched_yield(void)
{
	schedule();                    /* give up the CPU; we stay RUNNABLE        */
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

/* Find an exited child of `current` matching pid (-1 = any). */
static task_t *zombie_child(int pid)
{
	for (int i = 0; i < MAX_TASKS; i++) {
		task_t *t = &tasks[i];
		if (t->state == TASK_ZOMBIE && t->parent == current->id &&
		    (pid < 0 || (int)t->id == pid))
			return t;
	}
	return 0;
}

static int has_child(int pid)
{
	for (int i = 0; i < MAX_TASKS; i++) {
		task_t *t = &tasks[i];
		if (t->state != TASK_FREE && t->parent == current->id &&
		    (pid < 0 || (int)t->id == pid))
			return 1;
	}
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
	task_t *prev = current;
	task_t *next = prev->next;
	while (next->state != TASK_RUNNABLE)
		next = next->next;         /* always finds main if nothing else        */
	if (next == prev)
		return;

	current = next;
	current->runs++;
	if (current->kstack_top)
		tss_set_kernel_stack(current->kstack_top);
	if (current->cr3 != prev->cr3)
		paging_load((uint32_t *)current->cr3);
	context_switch(&prev->esp, current->esp);
}

int sched_count(void)
{
	return count;
}

void sched_dump(void)
{
	vga_write("id  name        runs\n");
	task_t *t = &tasks[0];
	do {
		vga_write_dec(t->id);
		vga_write("   ");
		vga_write(t->name);
		vga_write("\t");
		vga_write_dec(t->runs);
		vga_putc('\n');
		t = t->next;
	} while (t != &tasks[0]);
}
