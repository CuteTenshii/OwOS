#ifndef OWOS_SYSCALL_EXEC_H
#define OWOS_SYSCALL_EXEC_H

struct registers;

void syscall_exec_replace(struct registers *r);
void syscall_exec_spawn(struct registers *r);
void syscall_exec_spawn_cap(struct registers *r);

#endif
