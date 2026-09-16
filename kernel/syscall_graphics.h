#ifndef OWOS_SYSCALL_GRAPHICS_H
#define OWOS_SYSCALL_GRAPHICS_H

struct registers;

void syscall_graphics_fb_info(struct registers *r);
void syscall_graphics_fb_map(struct registers *r);
void syscall_graphics_fb_present(struct registers *r);
void syscall_graphics_input_poll(struct registers *r);
void syscall_graphics_input_bounds(struct registers *r);
void syscall_graphics_win_enum(struct registers *r);
void syscall_graphics_win_map(struct registers *r);
void syscall_graphics_win_unmap(struct registers *r);
void syscall_graphics_win_geom(struct registers *r);
void syscall_graphics_win_deliver(struct registers *r);
void syscall_graphics_win_key_space(struct registers *r);
void syscall_graphics_crash_poll(struct registers *r);

#endif
