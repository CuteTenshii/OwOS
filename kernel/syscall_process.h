#ifndef OWOS_SYSCALL_PROCESS_H
#define OWOS_SYSCALL_PROCESS_H

struct registers;

void syscall_process_exit(struct registers *r);
void syscall_process_fork(struct registers *r);
void syscall_process_waitpid(struct registers *r);
void syscall_process_futex_wait(struct registers *r);
void syscall_process_futex_wake(struct registers *r);
void syscall_process_msec(struct registers *r);
void syscall_process_getuid(struct registers *r);
void syscall_process_thread(struct registers *r);
void syscall_process_set_tls(struct registers *r);
void syscall_process_sleep(struct registers *r);
void syscall_process_kill(struct registers *r);
void syscall_process_power(struct registers *r);
void syscall_process_sbrk(struct registers *r);
void syscall_process_getpid(struct registers *r);
void syscall_process_thread_name(struct registers *r);
void syscall_process_sigprocmask(struct registers *r);
void syscall_process_stack_alloc(struct registers *r);
void syscall_process_stack_free(struct registers *r);

#endif
