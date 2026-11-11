/* plat.h: board description, read from the device tree with the QEMU virt layout as the fallback. */
#ifndef OWOS_ARCH_AARCH64_PLAT_H
#define OWOS_ARCH_AARCH64_PLAT_H

#include "cpu_defs.h"
#include "fbcon.h"
#include <stdint.h>

enum uart_kind { UART_NONE, UART_PL011, UART_16550 };
enum gic_kind { GIC_V2, GIC_V3 };

#define PLAT_I2C_MAX 8
#define PLAT_TEGRA_I2C_MAX 6

/* A MediaTek I2C controller node. clock_div is 0 and speed_hz 100 kHz when the tree leaves them out. */
struct plat_i2c {
  uintptr_t base, dma_base;
  uint32_t id, clock_div, speed_hz;
};

/* An enabled Tegra I2C controller node; speed_hz is 100 kHz when the tree leaves clock-frequency out. */
struct plat_tegra_i2c {
  uintptr_t base;
  uint32_t speed_hz;
};

struct plat {
  enum uart_kind uart_kind;
  uintptr_t uart_base;
  enum gic_kind gic_kind;
  uintptr_t gicd_base;
  uintptr_t gic_second_base; /* GICv2: CPU interface. GICv3: first redistributor. */
  uint64_t gic_second_size;
  struct fb_info fb;                      /* format FB_NONE when the loader left no framebuffer */
  uintptr_t topckgen_base, infracfg_base; /* 0 when absent */
  uintptr_t gpio_base;
  uintptr_t pwrap_base; /* 0 when the device tree has no PMIC wrapper */
  int ni2c;
  struct plat_i2c i2c[PLAT_I2C_MAX];
  uintptr_t car_base, tegra_gpio_base, pmc_base; /* Tegra clock-and-reset, GPIO and power blocks, 0 when absent */
  int ntegra_i2c;
  struct plat_tegra_i2c tegra_i2c[PLAT_TEGRA_I2C_MAX];
  uintptr_t max77620_bus; /* base of the I2C controller the MAX77620 PMIC sits on, 0 when absent */
  uint32_t max77620_addr;
  int psci_smc; /* -1: pick from the boot exception level */
  int ncpus;
  uint64_t cpu_mpidr[MAX_CPUS]; /* affinity bits, boot CPU first */
};

extern struct plat plat;

/* Fill plat from the device tree; keeps the defaults for anything it does not describe. */
void plat_init(const void *dtb);

#endif
