/* plat.h: board description, read from the device tree with the QEMU virt layout as the fallback. */
#ifndef OWOS_ARCH_AARCH64_PLAT_H
#define OWOS_ARCH_AARCH64_PLAT_H

#include "cpu_defs.h"
#include "fbcon.h"
#include <stdint.h>

enum uart_kind { UART_PL011, UART_MTK };
enum gic_kind { GIC_V2, GIC_V3 };

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
  int psci_smc;         /* -1: pick from the boot exception level */
  int ncpus;
  uint64_t cpu_mpidr[MAX_CPUS]; /* affinity bits, boot CPU first */
};

extern struct plat plat;

/* Fill plat from the device tree; keeps the defaults for anything it does not describe. */
void plat_init(const void *dtb);

#endif
