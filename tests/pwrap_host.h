/* pwrap_host.h: a pwrap_fake MT6765 PMIC wrapper in place of kernel/arch/aarch64/mmio.h, for pwrap_test.c. */
#ifndef OWOS_ARCH_AARCH64_MMIO_H
#define OWOS_ARCH_AARCH64_MMIO_H

#include <stdint.h>

#include "spinlock_host.h"

#define FAKE_PWRAP_BASE 0x1000d000u

struct fake_pwrap {
  uint32_t init_done2;
  uint32_t init_done0;
  uint16_t pmic[0x8000]; /* 16-bit registers, indexed by address / 2 */
  uint32_t fsm;          /* WACS2 state machine, as in RDATA[18:16] */
  uint32_t rdata;
  int stuck_busy; /* the command never completes */
  uint32_t last_cmd;
  int vldclr_writes;
  int cmds;
};
extern struct fake_pwrap pwrap_fake;

static inline uint32_t mmio_read32(uintptr_t a)
{
  switch (a - FAKE_PWRAP_BASE) {
  case 0xa0:
    return pwrap_fake.init_done2;
  case 0xc24:
    return (pwrap_fake.rdata & 0xffff) | pwrap_fake.fsm << 16 | (1u << 20) | (pwrap_fake.init_done0 ? 1u << 21 : 0);
  }
  return 0;
}

static inline void mmio_write32(uintptr_t a, uint32_t v)
{
  switch (a - FAKE_PWRAP_BASE) {
  case 0xc20:
    pwrap_fake.last_cmd = v;
    pwrap_fake.cmds++;
    if (pwrap_fake.stuck_busy) {
      pwrap_fake.fsm = 2;
    } else if (v >> 31) {
      pwrap_fake.pmic[(v >> 16) & 0x7fff] = (uint16_t)v;
    } else {
      pwrap_fake.rdata = pwrap_fake.pmic[(v >> 16) & 0x7fff];
      pwrap_fake.fsm = 6;
    }
    break;
  case 0xc28:
    pwrap_fake.vldclr_writes++;
    pwrap_fake.fsm = 0;
    break;
  }
}

static inline uint64_t mmio_read64(uintptr_t a) { return mmio_read32(a); }

#endif
