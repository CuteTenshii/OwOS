/* crashrec.h: one crash report as SYS_CRASH_POLL hands it to the compositor. Shared by the kernel and user space. */
#ifndef OWOS_COMMON_CRASHREC_H
#define OWOS_COMMON_CRASHREC_H

#include "uabi.h"

#define CRASH_TRACE_MAX 12
#define CRASH_REGS 8

/* Names of crash_rec.regs, in order. */
#ifdef __aarch64__
#define CRASH_REG_NAMES {"x0", "x1", "x2", "x3", "x4", "x5", "fp", "lr"}
#else
#define CRASH_REG_NAMES {"eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "flags"}
#endif

struct crash_rec {
  char name[24];
  unsigned vector; /* the CPU's exception number: the vector on x86, the ESR exception class on AArch64 */
  unsigned err;    /* its error code: for a page fault bit 0 present, 1 write, 2 user, 4 instruction fetch */
  int page_fault;  /* addr is the faulting address */
  sys_word pc, addr, sp;
  sys_word regs[CRASH_REGS];
  sys_word trace[CRASH_TRACE_MAX]; /* the faulting pc, then the return addresses up the frame-pointer chain */
  int ntrace;
};

#endif /* OWOS_COMMON_CRASHREC_H */
