/* task_arch.h: what the scheduler needs from the CPU layer (kernel/arch/<arch>/task.c). Each new task gets a kernel
 * stack holding a context that context_switch() resumes; these build those contexts and the user-visible state
 * around a trap frame. */
#ifndef OWOS_TASK_ARCH_H
#define OWOS_TASK_ARCH_H

#include "signal.h"
#include "uaccess.h"
#include <stdint.h>

struct registers;

/* Resume the context saved at *new_sp, saving the current one at *old_sp (switch_*.S). */
void context_switch(uintptr_t *old_sp, uintptr_t new_sp);

/* A kernel thread runs entry() with interrupts enabled; returning from it ends the thread (thread_exit). */
uintptr_t arch_kthread_init(uintptr_t kstack_top, void (*entry)(void));
void thread_exit(void);

/* A user task enters ring 3 / EL0 at `entry` on `user_sp`, every other register zero. */
uintptr_t arch_user_init(uintptr_t kstack_top, uaddr_t entry, uaddr_t user_sp);

/* A forked child resumes from a copy of its parent's syscall trap frame, the call returning 0. */
uintptr_t arch_fork_init(uintptr_t kstack_top, const struct registers *parent);

/* exec: point a trap frame at a new image's entry and stack, every other register zero. */
void arch_exec_regs(struct registers *r, uaddr_t entry, uaddr_t user_sp);

/* Per-CPU state that follows the running task: the stack a trap from user mode lands on, and the TLS base. */
void arch_set_kernel_stack(uintptr_t kstack_top);
void arch_set_tls(uaddr_t base);

/* Push a signal frame on the user stack (current address space) and point `r` at `handler`, saving `blocked` for
 * sigreturn; 0, or -1 if the stack can't take it. */
int arch_sigframe_setup(struct registers *r, const struct k_siginfo *info, uint32_t blocked, uaddr_t handler);

/* A user fault for the crash reporter: fill in c's CPU state (everything but name and trace) and return the frame
 * pointer the backtrace starts from. */
struct crash_rec;
uaddr_t arch_crash_capture(struct crash_rec *c, const struct registers *r);
/* The exception's name for a report ("general protection fault"), page faults aside. */
const char *arch_trap_name(unsigned vector);

/* SYS_SIGRETURN: restore the context saved by arch_sigframe_setup into `r`, its mask into *blocked; 0, or -1 if the
 * frame is unreadable or would resume somewhere invalid. */
int arch_sigreturn(struct registers *r, uint32_t *blocked);

#endif /* OWOS_TASK_ARCH_H */
