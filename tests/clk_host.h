/* clk_host.h: two fake register blocks that log every write, in place of mmio.h for clk_test.c. */
#ifndef OWOS_ARCH_AARCH64_MMIO_H
#define OWOS_ARCH_AARCH64_MMIO_H

#include <stdint.h>

#define FAKE_TOP 0x10000000u
#define FAKE_IFR 0x10001000u
#define FAKE_MAX_LOG 64

struct fake_clk {
  uint32_t mem[2][0x400]; /* word-indexed: [0] topckgen, [1] infracfg */
  struct {
    uint32_t addr, val;
  } log[FAKE_MAX_LOG];
  int nlog;
};
extern struct fake_clk clk_fake;

static inline uint32_t *fake_slot(uintptr_t a)
{
  uintptr_t off = a >= FAKE_IFR ? a - FAKE_IFR : a - FAKE_TOP;
  return &clk_fake.mem[a >= FAKE_IFR][off / 4];
}

static inline uint32_t mmio_read32(uintptr_t a) { return *fake_slot(a); }

static inline void mmio_write32(uintptr_t a, uint32_t v)
{
  if (clk_fake.nlog < FAKE_MAX_LOG) {
    clk_fake.log[clk_fake.nlog].addr = (uint32_t)a;
    clk_fake.log[clk_fake.nlog++].val = v;
  }
}

#endif
