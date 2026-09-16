/* sched.h — preemptive scheduler for kernel threads and user processes.
 *
 * Every task is a node in a circular run queue. Kernel threads share the
 * kernel address space; user processes each have their own page directory and
 * kernel stack, and the scheduler switches CR3 and the TSS kernel stack when
 * it runs one. Dead tasks are reaped (resources freed) by the scheduler.
 */
#ifndef OWOS_SCHED_H
#define OWOS_SCHED_H

#include "file.h"
#include "signal.h"
#include <stdint.h>

struct registers;

void sched_init(void);
void sched_create_thread(const char *name, void (*entry)(void));

/* Spawn a user process from an already-built address space; returns its pid.
 * `brk` is the initial program break (page-aligned image end) for SYS_SBRK. */
int sched_create_process(const char *name, uint32_t entry, uint32_t *dir, uint32_t user_esp, uint32_t brk);

/* User-space heap. sched_sbrk moves the caller's process break by `inc` bytes
 * (mapping/freeing window pages), returning the PREVIOUS break or (uint32_t)-1
 * on failure — the malloc primitive. The heap is shared by every thread of the
 * process (they share its address space). sched_set_brk resets the caller's
 * break base, used by the image-replace syscall when it swaps in a new program.
 */
uint32_t sched_sbrk(int inc);
void sched_set_brk(uint32_t brk);

/* Spawn a thread inside the current process (shares its address space); runs
 * `entry` on the user stack `user_esp`. Returns the thread's tid, or -1. */
int sched_create_user_thread(uint32_t entry, uint32_t user_esp);

/* fork the current process from its syscall trap frame; returns child pid to
 * the parent (the child returns 0 via the duplicated frame). */
int sched_fork(struct registers *r);

void sched_exit(int status); /* terminate current task; never returns  */

/* Signals (see signal.h + sched.c). sched_signal_send is the kill(2) primitive;
 * sigaction/sigprocmask act on the caller; deliver/sigreturn are the ring-3
 * boundary hooks (isr/irq tail + the SYS_SIGRETURN trampoline).
 * sched_intr_pending is the EINTR query for blocking kernel waits — true when a
 * deliverable signal is pending on the caller. */
int sched_signal_send(int pid, int sig);
int sched_sigaction(int sig, const struct k_sigaction *act, struct k_sigaction *old);
uint32_t sched_sigprocmask(int how, uint32_t set, int have_set);
void sched_signal_deliver(struct registers *r);
int sched_fault_signal(struct registers *r, int sig); /* deliver a CPU fault as a signal */
int sched_sigreturn(struct registers *r);
int sched_intr_pending(void);             /* a signal wants delivery (blocking waits) */
int sched_waitpid(int pid);               /* block for a child; returns its status  */
int sched_try_reap(int pid, int *status); /* non-blocking reap (kernel side) */
void sched_replace(uint32_t *new_dir, uint32_t entry, uint32_t user_esp, struct registers *r);

void schedule(void);    /* invoked from the timer tick                       */
void sched_yield(void); /* voluntarily give up the CPU (stays runnable)      */
/* Block the caller until absolute timer tick `wake`, fully off the run queue
 * (so sleeping tasks don't dilate other tasks' scheduling). timer_sleep() is
 * built on this; sched_wake_due() (called each timer tick) re-wakes due ones.
 */
void sched_sleep_until(uint32_t wake);
void sched_wake_due(uint32_t now);
struct fd *sched_fds(void);           /* the current task's file descriptor table     */
uint32_t *sched_current_dir(void);    /* current task's page dir (windowing layer) */
uint32_t sched_current_id(void);      /* current task's pid                        */
const char *sched_current_name(void); /* current task's name (crash reporter) */
void sched_set_pipe_fd(int pid, int fd, int writer, int pipe_id);
void sched_set_cwd(int pid, const char *path); /* set a child's working dir */
void sched_set_cwd_self(const char *path);     /* set the caller's own cwd     */
const char *sched_cwd(void);                   /* the current task's working dir (absolute)     */

/* Per-process credentials (inherited across spawn/fork like cwd). The current
 * task's uid/gid; sched_set_creds_self switches the caller (used by login/su
 * via /proc/login -- only after the kernel has validated the password). */
uint32_t sched_uid(void);
uint32_t sched_gid(void);
void sched_set_creds_self(uint32_t uid, uint32_t gid);
void sched_set_creds(int pid, uint32_t uid, uint32_t gid); /* set a child's uid/gid */
void sched_dump(void);
int sched_count(void);

#endif /* OWOS_SCHED_H */
