/* plat.c: discover the UART, interrupt controller and PSCI conduit from the device tree. */
#include "plat.h"
#include "arch.h"

struct plat plat = {
    .uart_base = 0x09000000u,
    .gicd_base = 0x08000000u,
    .gicc_base = 0x08010000u,
    .psci_smc = -1,
};

static const char *const gic_compat[] = {"arm,cortex-a15-gic", "arm,cortex-a7-gic", "arm,gic-400", "arm,cortex-a9-gic"};
static const char *const psci_compat[] = {"arm,psci-1.0", "arm,psci-0.2", "arm,psci"};

/* reg entry `entry` of the first node matching compat, if this CPU can address it without LPAE. */
static int find_reg(const void *dtb, const char *compat, int entry, uintptr_t *base)
{
  uint64_t b, size;
  if (fdt_compat_reg(dtb, compat, 0, entry, &b, &size) || b > UINTPTR_MAX)
    return -1;
  *base = (uintptr_t)b;
  return 0;
}

static void find_gic(const void *dtb)
{
  for (unsigned i = 0; i < sizeof gic_compat / sizeof *gic_compat; i++) {
    uintptr_t d, c;
    if (!find_reg(dtb, gic_compat[i], 0, &d) && !find_reg(dtb, gic_compat[i], 1, &c)) {
      plat.gicd_base = d;
      plat.gicc_base = c;
      return;
    }
  }
}

static void find_psci(const void *dtb)
{
  char method[8];
  for (unsigned i = 0; i < sizeof psci_compat / sizeof *psci_compat; i++)
    if (!fdt_compat_str(dtb, psci_compat[i], "method", method, sizeof method)) {
      plat.psci_smc = method[0] == 's';
      return;
    }
}

void plat_init(const void *dtb)
{
  find_reg(dtb, "arm,pl011", 0, &plat.uart_base);
  find_gic(dtb);
  find_psci(dtb);
}
