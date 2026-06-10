/* syscall.h — the int 0x80 system-call interface for user programs. */
#ifndef OWOS_SYSCALL_H
#define OWOS_SYSCALL_H

struct registers;

void syscall_init(void);                 /* install the DPL-3 0x80 gate       */
void syscall_dispatch(struct registers *r);

#endif /* OWOS_SYSCALL_H */
