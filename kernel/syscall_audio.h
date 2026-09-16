#ifndef OWOS_SYSCALL_AUDIO_H
#define OWOS_SYSCALL_AUDIO_H

#include <stdint.h>

struct registers;

void syscall_audio_open(struct registers *r);
void syscall_audio_write(struct registers *r);
void syscall_audio_close(struct registers *r);
void audio_task_exiting(uint32_t id);

#endif
