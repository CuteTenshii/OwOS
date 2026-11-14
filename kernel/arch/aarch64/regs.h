/* regs.h: the register frame an exception leaves for C (vectors.S), and the system call ABI on top of it. */
#ifndef OWOS_REGS_H
#define OWOS_REGS_H

#include <stdbool.h>
#include <stdint.h>

/* In memory order; vectors.S builds and restores it, and its size stays a multiple of 16 for the stack. */
struct registers {
  uint64_t x[31];
  uint64_t sp;     /* SP_EL0: the interrupted user stack */
  uint64_t pc;     /* ELR_EL1: where the exception returns */
  uint64_t pstate; /* SPSR_EL1 */
  uint64_t sa[3];  /* a system call's x0..x2 as it arrived; x0 also carries the result back */
  uint64_t esr;    /* ESR_EL1 and FAR_EL1 at entry: the cause and faulting address of a synchronous exception */
  uint64_t far;
  uint64_t pad;
};

_Static_assert(sizeof(struct registers) == 320, "vectors.S lays the frame out at 320 bytes");

/* System calls: svc #0 with the number in x8 and arguments in x0..x2, result in x0. */
#define SC_NUM(r) ((r)->x[8])
#define SC_ARG0(r) ((r)->sa[0])
#define SC_ARG1(r) ((r)->sa[1])
#define SC_ARG2(r) ((r)->sa[2])
#define SC_RET(r) ((r)->x[0])

/* Whether the frame interrupted EL0: SPSR.M is EL0t. */
static inline bool regs_from_user(const struct registers *r) { return (r->pstate & 0xF) == 0; }

#endif /* OWOS_REGS_H */
