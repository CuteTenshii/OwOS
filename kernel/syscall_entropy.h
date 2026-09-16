#ifndef OWOS_SYSCALL_ENTROPY_H
#define OWOS_SYSCALL_ENTROPY_H

struct registers;

void syscall_entropy_getrandom(struct registers *r);

#endif
