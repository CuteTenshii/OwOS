#ifndef OWOS_SYSCALL_IO_H
#define OWOS_SYSCALL_IO_H

struct registers;

void syscall_io_write(struct registers *r);
void syscall_io_read(struct registers *r);
void syscall_io_pipe(struct registers *r);
void syscall_io_close(struct registers *r);
void syscall_io_read_nb(struct registers *r);
void syscall_io_port_open(struct registers *r);
void syscall_io_port_accept(struct registers *r);

#endif
