#ifndef OWOS_SYSCALL_GRAPHICS_H
#define OWOS_SYSCALL_GRAPHICS_H

#include <stdbool.h>
#include <stdint.h>

struct registers;

/* True while a process (the compositor) owns the framebuffer back buffer. */
bool graphics_fb_owned(void);
/* Task `id` is exiting; releases the framebuffer if it owned it. */
void graphics_task_exiting(uint32_t id);

void syscall_graphics_fb_info(struct registers *r);
void syscall_graphics_fb_map(struct registers *r);
void syscall_graphics_fb_present(struct registers *r);
void syscall_graphics_fb_cursor(struct registers *r);
void syscall_graphics_input_poll(struct registers *r);
void syscall_graphics_input_bounds(struct registers *r);
void syscall_graphics_crash_poll(struct registers *r);
void syscall_graphics_symbolize(struct registers *r);

#endif
