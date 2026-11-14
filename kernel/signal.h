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

#include "uabi.h"
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

/* sa_flags is stored per-signal; an interrupted blocking syscall always returns EINTR (no SA_RESTART). */

/* sigprocmask() `how`. */
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

/* The sigaction ABI struct exchanged across the syscall boundary. `handler`
 * holds a user code pointer or SIG_DFL/SIG_IGN; `mask` is added to the blocked
 * set while the handler runs; `flags` is the SA_* set. */
struct k_sigaction {
  sys_word handler;
  uint32_t mask;
  uint32_t flags;
};

/* SA_SIGINFO: the handler is void (*)(int sig, struct k_siginfo *info, struct k_sigctx *ctx). Every handler
 * gets the three arguments; the flag only says the handler was written to read them. */
#define SA_SIGINFO 4u

/* Why a signal arrived, in the handler frame (32-bit layout under both kernels). */
#define SI_USER 0     /* sent by kill() or raise() */
#define SEGV_MAPERR 1 /* address not mapped */
#define SEGV_ACCERR 2 /* mapped, but the access isn't allowed (write to read-only, user to kernel) */
#define ILL_ILLOPC 1  /* invalid opcode */
#define FPE_INTDIV 1  /* integer divide by zero */
#define SI_TRAP_NONE 0xFFFFFFFFu
struct k_siginfo {
  int32_t signo;
  int32_t code;    /* SI_USER, or the SEGV_/ILL_/FPE_ cause of a CPU fault */
  sys_word addr;   /* the faulting address: CR2 for a page fault, the instruction for #UD/#DE, else 0 */
  uint32_t trapno; /* CPU exception vector, SI_TRAP_NONE when no fault raised it */
  uint32_t err;    /* CPU error code (page fault: bit 0 present, 1 write, 2 user, 4 instruction fetch) */
};

/* The interrupted user context, restored by sigreturn (a handler may read it, and rewrite it to resume elsewhere).
 * x86_64 keeps the 32-bit names at full width and adds r8..r15, as struct registers does; AArch64 has its own. */
#if defined(__aarch64__)
struct k_sigctx {
  uint64_t x[31], sp, pc, pstate; /* the EL0 general registers, stack pointer, resume address and flags */
};
#elif defined(__x86_64__)
struct k_sigctx {
  uint64_t eip, eflags, esp;
  uint64_t eax, ecx, edx, ebx, ebp, esi, edi;
  uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
};
#else
struct k_sigctx {
  uint32_t eip, eflags, esp;
  uint32_t eax, ecx, edx, ebx, ebp, esi, edi;
};
#endif

/* Bit for signal `s` (1..31). Signal 0 is not a real signal (kill(pid,0)
 * probes). */
#define SIGBIT(s) (1u << (s))

#endif /* OWOS_SIGNAL_H */
