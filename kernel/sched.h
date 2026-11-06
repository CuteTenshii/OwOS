/* sched.h: preemptive scheduler for kernel threads and user processes (circular run queue). */
#ifndef OWOS_SCHED_H
#define OWOS_SCHED_H

#include "file.h"
#include "signal.h"
#include <stdbool.h>
#include <stdint.h>

struct registers;

void sched_init(void);
void sched_init_cpu(int cpu);
void sched_start_cpu(int cpu);
void sched_create_thread(const char *name, void (*entry)(void));

/* Spawn a user process from a built address space; `brk` is the initial program break. */
int sched_create_process(const char *name, uint32_t entry, uint32_t *dir, uint32_t user_esp, uint32_t brk);

/* User heap: sched_sbrk returns the previous break or (uint32_t)-1; sched_set_brk resets it on exec. */
uint32_t sched_sbrk(int inc);
void sched_set_brk(uint32_t brk);

/* Spawn a thread in the current process running `entry` on `user_esp`; tid or -1. */
int sched_create_user_thread(uint32_t entry, uint32_t user_esp);

/* fork from the syscall trap frame; child pid to the parent, 0 in the child. */
int sched_fork(struct registers *r);

void sched_exit(int status); /* terminate current task; never returns */

/* Signals (see signal.h): send is kill(2); deliver/sigreturn are the ring-3 boundary hooks. */
int sched_signal_send(int pid, int sig);
int sched_sigaction(int sig, const struct k_sigaction *act, struct k_sigaction *old);
uint32_t sched_sigprocmask(int how, uint32_t set, int have_set);
void sched_signal_deliver(struct registers *r);
int sched_fault_signal(struct registers *r, int sig); /* deliver a CPU fault as a signal */
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
const char *sched_current_name(void); /* current task's name (crash reporter) */
void sched_set_pipe_fd(int pid, int fd, int writer, int pipe_id);
void sched_set_cwd(int pid, const char *path); /* set a child's working dir */
void sched_set_cwd_self(const char *path);     /* set the caller's own cwd */
const char *sched_cwd(void);                   /* the current task's working dir (absolute) */

/* Per-process credentials, inherited across spawn/fork; sched_set_creds_self is for login/su. */
uint32_t sched_uid(void);
uint32_t sched_gid(void);
void sched_set_creds_self(uint32_t uid, uint32_t gid);
void sched_set_creds(int pid, uint32_t uid, uint32_t gid); /* set a child's uid/gid */
void sched_dump(void);
int sched_count(void);

#endif /* OWOS_SCHED_H */
