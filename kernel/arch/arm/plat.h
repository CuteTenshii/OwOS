/* plat.h: board description, read from the device tree with the QEMU virt layout as the fallback. */
#ifndef OWOS_ARCH_ARM_PLAT_H
#define OWOS_ARCH_ARM_PLAT_H

#include <stdint.h>

struct plat {
  uintptr_t uart_base; /* PL011 */
  uintptr_t gicd_base, gicc_base;
  int psci_smc; /* -1: pick from the boot mode */
};

extern struct plat plat;

/* Fill plat from the device tree; keeps the defaults for anything it does not describe or that lies above 4 GiB. */
void plat_init(const void *dtb);

#endif
