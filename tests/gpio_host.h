/* gpio_host.h: plain memory behind the GPIO register block, in place of mmio.h for gpio_test.c. */
#ifndef OWOS_ARCH_AARCH64_MMIO_H
#define OWOS_ARCH_AARCH64_MMIO_H

#include <stdint.h>

#define FAKE_GPIO_BASE 0x10005000u

struct fake_gpio {
  uint32_t mem[0x400];
};
extern struct fake_gpio gpio_fake;

static inline uint32_t mmio_read32(uintptr_t a) { return gpio_fake.mem[(a - FAKE_GPIO_BASE) / 4]; }
static inline void mmio_write32(uintptr_t a, uint32_t v) { gpio_fake.mem[(a - FAKE_GPIO_BASE) / 4] = v; }

#endif
