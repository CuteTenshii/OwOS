#ifndef OWOS_SYSCALL_SIGNAL_H
#define OWOS_SYSCALL_SIGNAL_H

struct registers;

void syscall_signal_action(struct registers *r);
void syscall_signal_procmask(struct registers *r);
void syscall_signal_return(struct registers *r);

#endif
