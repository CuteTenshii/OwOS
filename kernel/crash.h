/* crash.h — ring-3 crash reporter.
 *
 * When a user process faults (page fault, GP, ...) the IDT handler calls
 * crash_record() to capture the registers and a stack backtrace, then kills the
 * process instead of panicking the kernel. The window manager calls crash_pump()
 * each frame to turn any pending report into an "<app> has crashed" window.
 */
#ifndef OWOS_CRASH_H
#define OWOS_CRASH_H

struct registers;

/* Capture a crash from a ring-3 fault (called from the exception handler, in the
 * faulting task's context so its user stack is still mapped for the backtrace). */
void crash_record(const char *name, struct registers *r);

/* Create windows for any pending crash reports (called from the WM thread). */
void crash_pump(void);

#endif /* OWOS_CRASH_H */
