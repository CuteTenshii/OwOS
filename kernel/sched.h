/* sched.h: preemptive scheduler for kernel threads and user processes (circular run queue). */
#ifndef OWOS_SCHED_H
#define OWOS_SCHED_H

#include "file.h"
#include "signal.h"
#include "syscall_nr.h"
#include <stdbool.h>
#include <stdint.h>

struct registers;

void sched_init(void);
void sched_init_cpu(int cpu);
void sched_start_cpu(int cpu);
void sched_create_thread(const char *name, void (*entry)(void));

/* Spawn a user process from a built address space; `brk` is the initial program break. A `held` process is not
 * scheduled until sched_release(pid), so its spawner can finish setting it up on an SMP machine. */
int sched_create_process(const char *name, uint32_t entry, uint32_t *dir, uint32_t user_esp, uint32_t brk, bool held);
void sched_release(int pid);

/* User heap: sched_sbrk returns the previous break or (uint32_t)-1; sched_set_brk resets it on exec. */
uint32_t sched_sbrk(int inc);
void sched_set_brk(uint32_t brk);

/* Spawn a thread in the current process running `entry` on `user_esp` with %gs based at `tls_base`; tid or -1. */
int sched_create_user_thread(uint32_t entry, uint32_t user_esp, uint32_t tls_base);

/* Point the caller's %gs segment at `base` (0 = flat). Each user thread has its own. */
void sched_set_tls(uint32_t base);

/* fork from the syscall trap frame; child pid to the parent, 0 in the child. */
int sched_fork(struct registers *r);

void sched_exit(int status); /* terminate current task; never returns */

/* Signals (see signal.h): send is kill(2); deliver/sigreturn are the ring-3 boundary hooks. */
int sched_signal_send(int pid, int sig);
int sched_sigaction(int sig, const struct k_sigaction *act, struct k_sigaction *old);
uint32_t sched_sigprocmask(int how, uint32_t set, int have_set);
void sched_signal_deliver(struct registers *r);
int sched_fault_signal(struct registers *r, int sig, uint32_t addr); /* deliver a CPU fault as a signal */
int sched_sigreturn(struct registers *r);
bool sched_intr_pending(void);             /* a signal wants delivery (blocking waits) */
int sched_waitpid(int pid);                /* block for a child; returns its status */
bool sched_try_reap(int pid, int *status); /* non-blocking reap (kernel side) */
int sched_replace(uint32_t *new_dir, uint32_t entry, uint32_t user_esp, uint32_t brk, struct registers *r);

void schedule(void); /* invoked from the timer tick */
void sched_kernel_enter(void);
void sched_kernel_leave(void);
void sched_kernel_switch_in(void);
void sched_yield(void); /* voluntarily give up the CPU (stays runnable) */
/* Block the caller off the run queue until absolute tick `wake`; sched_wake_due() wakes due tasks. */
void sched_sleep_until(uint32_t wake);
void sched_wake_due(uint32_t now);
bool sched_futex_wait(uint32_t addr, uint32_t expected, uint32_t ms); /* true: timed out/interrupted */
int sched_futex_wake(uint32_t addr, int n);
struct fd *sched_fds(void);        /* the current task's file descriptor table */
uint32_t *sched_current_dir(void); /* current task's page dir (windowing layer) */
uint32_t sched_current_id(void);   /* current task's pid */
uint32_t sched_current_process_id(void);
const char *sched_current_name(void);         /* current task's program name (crash reporter, symbols) */
const char *sched_current_thread_name(void);  /* its thread label, "" if unnamed */
void sched_set_thread_name(const char *name); /* label the current thread (truncated to THREAD_NAME_MAX - 1) */
struct env;
struct env *sched_env(void);                                        /* the running process's environment */
void sched_set_env_block(int pid, const char *block, uint32_t len); /* give a new child exactly this environment */
void sched_set_pipe_fd(int pid, int fd, int writer, int pipe_id);
void sched_set_fd(int pid, int fd, const struct fd *f); /* install a copy of an open file in a new child */
void fd_ref(const struct fd *f);                        /* take a reference for a new copy of an open file */
void sched_set_cwd(int pid, const char *path);          /* set a child's working dir */
void sched_set_cwd_self(const char *path);              /* set the caller's own cwd */
const char *sched_cwd(void);                            /* the current task's working dir (absolute) */

/* Per-process credentials, inherited across spawn/fork; sched_set_creds_self is for login/su. */
uint32_t sched_uid(void);
uint32_t sched_gid(void);
void sched_set_creds_self(uint32_t uid, uint32_t gid);
uint32_t sched_umask(void);                                /* file creation mask, inherited across spawn/fork */
uint32_t sched_set_umask(uint32_t mask);                   /* returns the previous mask */
void sched_set_creds(int pid, uint32_t uid, uint32_t gid); /* set a child's uid/gid */
void sched_dump(void);
int sched_count(void);

/* One task as /proc/ps reports it; pid is the process leader's id, so pid != tid marks a user thread. */
struct sched_task_info {
  uint32_t tid;
  uint32_t pid;
  uint32_t ppid;
  uint32_t uid;
  uint32_t runs;
  char state; /* R runnable, S waiting, Z zombie, E held by its spawner */
  char name[16];
  char tname[THREAD_NAME_MAX];
};

/* Copy up to `max` tasks into `out` in run-queue order; returns how many were copied. */
int sched_snapshot(struct sched_task_info *out, int max);

#endif /* OWOS_SCHED_H */
