#ifndef OWOS_SYSCALL_WINDOW_H
#define OWOS_SYSCALL_WINDOW_H

struct registers;

void syscall_window_create(struct registers *r);
void syscall_window_present(struct registers *r);
void syscall_window_poll(struct registers *r);
void syscall_window_close(struct registers *r);
void syscall_window_title(struct registers *r);
void syscall_window_resizable(struct registers *r);
void syscall_window_cursor(struct registers *r);
void syscall_window_set_parent(struct registers *r);
void syscall_window_fb(struct registers *r);
void syscall_window_owns(struct registers *r);

#endif
