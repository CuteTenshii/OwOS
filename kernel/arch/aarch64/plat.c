/* plat.c: discover the UART, interrupt controller, PSCI conduit and CPU list from the device tree. */
#include "plat.h"
#include "arch.h"

struct plat plat = {
    .uart_kind = UART_PL011,
    .uart_base = 0x09000000UL,
    .gic_kind = GIC_V2,
    .gicd_base = 0x08000000UL,
    .gic_second_base = 0x08010000UL,
    .psci_smc = -1,
    .ncpus = 1,
};

static const char *const gic_v2_compat[] = {"arm,cortex-a15-gic", "arm,gic-400", "arm,cortex-a9-gic"};
static const char *const psci_compat[] = {"arm,psci-1.0", "arm,psci-0.2", "arm,psci"};

static void find_uart(const void *dtb)
{
  uint64_t base, size;
  if (!fdt_compat_reg(dtb, "arm,pl011", 0, 0, &base, &size)) {
    plat.uart_kind = UART_PL011;
    plat.uart_base = base;
  } else if (!fdt_compat_reg(dtb, "mediatek,mt6577-uart", 0, 0, &base, &size)) {
    plat.uart_kind = UART_MTK;
    plat.uart_base = base;
  }
}

static void find_gic(const void *dtb)
{
  uint64_t d, dsize, s, ssize;
  if (!fdt_compat_reg(dtb, "arm,gic-v3", 0, 0, &d, &dsize) && !fdt_compat_reg(dtb, "arm,gic-v3", 0, 1, &s, &ssize)) {
    plat.gic_kind = GIC_V3;
    plat.gicd_base = d;
    plat.gic_second_base = s;
    plat.gic_second_size = ssize;
    return;
  }
  for (unsigned i = 0; i < sizeof gic_v2_compat / sizeof *gic_v2_compat; i++)
    if (!fdt_compat_reg(dtb, gic_v2_compat[i], 0, 0, &d, &dsize) &&
        !fdt_compat_reg(dtb, gic_v2_compat[i], 0, 1, &s, &ssize)) {
      plat.gic_kind = GIC_V2;
      plat.gicd_base = d;
      plat.gic_second_base = s;
      return;
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

static void find_cpus(const void *dtb)
{
  uint64_t self = SYSREG_READ(mpidr_el1) & FDT_MPIDR_AFFINITY;
  int n = 0, boot = -1;
  uint64_t m;
  while (n < MAX_CPUS && !fdt_cpu_mpidr(dtb, n, &m)) {
    plat.cpu_mpidr[n] = m;
    if (m == self)
      boot = n;
    n++;
  }
  if (boot < 0) {
    plat.cpu_mpidr[0] = self;
    plat.ncpus = 1;
    return;
  }
  plat.cpu_mpidr[boot] = plat.cpu_mpidr[0];
  plat.cpu_mpidr[0] = self;
  plat.ncpus = n;
}

void plat_init(const void *dtb)
{
  find_uart(dtb);
  find_gic(dtb);
  find_psci(dtb);
  find_cpus(dtb);
}
