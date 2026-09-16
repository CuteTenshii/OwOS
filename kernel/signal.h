/* signal.h — signal-subsystem ABI, shared by the kernel (sched.c/syscall.c)
 * and, mirrored, by user space (owos.h). A minimal but real POSIX-style signal
 * model: per-process pending/blocked masks + a disposition per signal,
 * delivered on the return to ring 3 by running a user handler on its own stack
 * (SIG_DFL falls back to the default action — terminate for most, ignore for a
 * few). Keep the numbers in sync with the SIG_* / SA_* mirror in
 * user/lib/owos.h.
 *
 * Phase A implements per-process signals (kill/sigaction/sigprocmask/handlers +
 * EINTR). The job-control signals (STOP/CONT/TSTP/TTIN/TTOU) have numbers
 * reserved here but default to "ignore" until Phase B wires process groups + a
 * controlling terminal. */
#ifndef OWOS_SIGNAL_H
#define OWOS_SIGNAL_H

#include <stdint.h>

#define NSIG 32 /* signals 1..31; the masks are uint32_t bitsets   */

#define SIGHUP 1
#define SIGINT 2 /* Ctrl+C                                          */
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGBUS 7
#define SIGFPE 8
#define SIGKILL 9 /* uncatchable: always terminates                  */
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17 /* default: ignore                                 */
#define SIGCONT 18 /* Phase B                                         */
#define SIGSTOP 19 /* Phase B: uncatchable stop                       */
#define SIGTSTP 20 /* Phase B                                         */
#define SIGTTIN 21 /* Phase B                                         */
#define SIGTTOU 22 /* Phase B                                         */

/* Dispositions that are values, not handler addresses (a real handler is a user
 * code pointer, which is always >= USER_BASE so it can't collide with these).
 */
#define SIG_DFL 0u /* default action for the signal                   */
#define SIG_IGN 1u /* ignore the signal                               */

/* sa_flags is stored per-signal but unused: an interrupted blocking syscall
 * always returns EINTR. */

/* sigprocmask() `how`. */
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

/* The sigaction ABI struct exchanged across the syscall boundary. `handler`
 * holds a user code pointer or SIG_DFL/SIG_IGN; `mask` is added to the blocked
 * set while the handler runs; `flags` is the SA_* set. */
struct k_sigaction {
  uint32_t handler;
  uint32_t mask;
  uint32_t flags;
};

/* Bit for signal `s` (1..31). Signal 0 is not a real signal (kill(pid,0)
 * probes). */
#define SIGBIT(s) (1u << (s))

#endif /* OWOS_SIGNAL_H */
