/* crash.h: crash reporter.
 *
 * When a user process faults (page fault, GP, ...) the IDT handler calls
 * crash_record() to capture the registers and a stack backtrace, then kills the
 * process instead of panicking the kernel. The user-space compositor drains any
 * pending report via crash_poll() (SYS_CRASH_POLL) and renders its own "<app>
 * has crashed" window.
 */
#ifndef OWOS_CRASH_H
#define OWOS_CRASH_H

#include "crashrec.h"
#include "uaccess.h"
#include <stdint.h>

struct registers;

/* Capture a crash from a user-program fault (called from the exception handler,
 * in the faulting task's context so its user stack is still mapped for the
 * backtrace). */
void crash_record(const char *name, struct registers *r);

/* Drain up to `max` pending crash reports into `out` (marking them consumed, so
 * each is returned once), and return the count. The user-space compositor calls
 * this to render "<app> has crashed" windows without a kernel window. */
int crash_poll(struct crash_rec *out, int max);

/* Name `addr` in the current task's own program as "func+0xNN" (its symbols are no secret to it); 1 on success. */
int crash_symbolize_self(uaddr_t addr, char *out, int cap);

#endif /* OWOS_CRASH_H */
