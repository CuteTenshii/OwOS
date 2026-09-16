/* syscall.h — the int 0x80 system-call interface for user programs. */
#ifndef OWOS_SYSCALL_H
#define OWOS_SYSCALL_H

struct registers;

void syscall_init(void); /* install the DPL-3 0x80 gate       */
void syscall_dispatch(struct registers *r);

#include <stdint.h>
/* Release the one system-wide PCM stream if task `id` owns it (sched_exit
 * hook), so an abnormal exit can't leak the audio latch + leave the DMA
 * running. */
void audio_task_exiting(uint32_t id);

#endif /* OWOS_SYSCALL_H */
