/* crash.h — crash reporter.
 *
 * When a user process faults (page fault, GP, ...) the IDT handler calls
 * crash_record() to capture the registers and a stack backtrace, then kills the
 * process instead of panicking the kernel. The user-space compositor drains any
 * pending report via crash_poll() (SYS_CRASH_POLL) and renders its own "<app>
 * has crashed" window.
 */
#ifndef OWOS_CRASH_H
#define OWOS_CRASH_H

#include <stdint.h>

struct registers;

/* Capture a crash from a user-program fault (called from the exception handler,
 * in the faulting task's context so its user stack is still mapped for the
 * backtrace). */
void crash_record(const char *name, struct registers *r);

/* One crash record in the shape user space reads it (SYS_CRASH_POLL): a fixed
 * layout so a ring-3 compositor can render "<app> has crashed" windows without
 * a kernel window. Mirrors the kernel's internal crash_info, but
 * flat/ABI-stable. */
#define CRASH_TRACE_MAX 12
struct crash_rec {
  char name[24];
  uint32_t vector, err, eip, cr2;
  uint32_t eax, ebx, ecx, edx, esi, edi, ebp, esp;
  uint32_t trace[CRASH_TRACE_MAX];
  int ntrace;
};

/* Drain up to `max` pending crash reports into `out` (marking them consumed, so
 * each is returned once), and return the count. The user-space compositor calls
 * this to render "<app> has crashed" windows without a kernel window. */
int crash_poll(struct crash_rec *out, int max);

#endif /* OWOS_CRASH_H */
