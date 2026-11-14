/* exc.h: exception syndrome helpers shared by exc.c and task.c. */
#ifndef OWOS_ARCH_AARCH64_EXC_H
#define OWOS_ARCH_AARCH64_EXC_H

#include <stdint.h>

/* ESR_EL1 exception classes. */
#define EC_UNKNOWN 0x00
#define EC_ILLEGAL_STATE 0x0E
#define EC_SVC64 0x15
#define EC_SYSREG 0x18
#define EC_INSN_ABORT_EL0 0x20
#define EC_INSN_ABORT_EL1 0x21
#define EC_PC_ALIGN 0x22
#define EC_DATA_ABORT_EL0 0x24
#define EC_DATA_ABORT_EL1 0x25
#define EC_SP_ALIGN 0x26
#define EC_FP_EXC64 0x2C
#define EC_BRK64 0x3C

static inline unsigned esr_class(uint64_t esr) { return (unsigned)(esr >> 26) & 0x3F; }

static inline int esr_is_abort(uint64_t esr)
{
  unsigned ec = esr_class(esr);
  return ec == EC_INSN_ABORT_EL0 || ec == EC_INSN_ABORT_EL1 || ec == EC_DATA_ABORT_EL0 || ec == EC_DATA_ABORT_EL1;
}

/* An abort's syndrome as the x86 page-fault error bits the rest of the kernel and user space read: 1 the page was
 * present (a permission or access-flag fault, not a translation fault), 2 a write, 4 from user mode, 16 an
 * instruction fetch. */
uint32_t abort_err_bits(uint64_t esr);

#endif /* OWOS_ARCH_AARCH64_EXC_H */
