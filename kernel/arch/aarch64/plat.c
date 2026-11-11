/* plat.c: discover the UART, interrupt controller, PSCI conduit, CPU list and SoC blocks from the device tree. */
#include "plat.h"
#include "arch.h"
#include "tegra_car.h"

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

static int status_ok(const char *status) { return status[0] == 'o' && status[1] == 'k'; }

/* A node without a status property is enabled. */
static int node_disabled(const void *dtb, const char *compat, int idx)
{
  char status[16];
  if (fdt_compat_str_at(dtb, compat, idx, "status", status, sizeof status))
    return 0;
  return !status_ok(status);
}

struct uart_compat {
  const char *compat;
  enum uart_kind kind;
};

static const struct uart_compat uart_compat[] = {
    {"arm,pl011", UART_PL011},
    {"mediatek,mt6577-uart", UART_16550},
    {"nvidia,tegra20-uart", UART_16550},
    {"nvidia,tegra114-hsuart", UART_16550},
};

/* The loader's console when /chosen names a UART this kernel drives, else the first enabled one it knows, else the
 * first one at all: vendor trees do not always mark the console okay. */
static void find_uart(const void *dtb)
{
  uint64_t base, size;
  char path[64], status[16];
  if (!fdt_stdout_path(dtb, path, sizeof path) && !fdt_path_reg(dtb, path, 0, &base, &size) &&
      (fdt_path_str(dtb, path, "status", status, sizeof status) || status_ok(status)))
    for (unsigned i = 0; i < sizeof uart_compat / sizeof *uart_compat; i++)
      if (fdt_path_compat(dtb, path, uart_compat[i].compat) == 1) {
        plat.uart_kind = uart_compat[i].kind;
        plat.uart_base = base;
        return;
      }
  for (int any = 0; any < 2; any++)
    for (unsigned i = 0; i < sizeof uart_compat / sizeof *uart_compat; i++)
      for (int idx = 0; !fdt_compat_reg(dtb, uart_compat[i].compat, idx, 0, &base, &size); idx++)
        if (any || !node_disabled(dtb, uart_compat[i].compat, idx)) {
          plat.uart_kind = uart_compat[i].kind;
          plat.uart_base = base;
          return;
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

static void find_fb(const void *dtb)
{
  uint64_t base, size;
  uint32_t w, h, stride;
  char format[16];
  if (fdt_compat_reg(dtb, "simple-framebuffer", 0, 0, &base, &size) ||
      fdt_compat_u32(dtb, "simple-framebuffer", "width", &w) ||
      fdt_compat_u32(dtb, "simple-framebuffer", "height", &h) ||
      fdt_compat_u32(dtb, "simple-framebuffer", "stride", &stride) ||
      fdt_compat_str(dtb, "simple-framebuffer", "format", format, sizeof format))
    return;
  plat.fb = (struct fb_info){base, w, h, stride, fb_format_parse(format)};
}

static void find_soc_blocks(const void *dtb)
{
  uint64_t base, size;
  if (!fdt_compat_reg(dtb, "mediatek,mt6765-topckgen", 0, 0, &base, &size))
    plat.topckgen_base = base;
  if (!fdt_compat_reg(dtb, "mediatek,mt6765-infracfg", 0, 0, &base, &size))
    plat.infracfg_base = base;
  if (!fdt_compat_reg(dtb, "mediatek,mt6765-pinctrl", 0, 0, &base, &size))
    plat.gpio_base = base;
}

static void find_pwrap(const void *dtb)
{
  uint64_t base, size;
  if (!fdt_compat_reg(dtb, "mediatek,mt6765-pwrap", 0, 0, &base, &size))
    plat.pwrap_base = base;
}

/* Vendor (ALPS) trees use the generic string; mainline-style trees name the SoC. */
static const char *const i2c_compat[] = {"mediatek,mt6765-i2c", "mediatek,i2c"};

static int i2c_known(uintptr_t base)
{
  for (int i = 0; i < plat.ni2c; i++)
    if (plat.i2c[i].base == base)
      return 1;
  return 0;
}

static void find_i2c(const void *dtb)
{
  for (unsigned c = 0; c < sizeof i2c_compat / sizeof *i2c_compat; c++) {
    const char *compat = i2c_compat[c];
    uint64_t base, dma, size;
    for (int idx = 0; plat.ni2c < PLAT_I2C_MAX && !fdt_compat_reg(dtb, compat, idx, 0, &base, &size); idx++) {
      if (i2c_known(base) || node_disabled(dtb, compat, idx) || fdt_compat_reg(dtb, compat, idx, 1, &dma, &size))
        continue;
      struct plat_i2c *p = &plat.i2c[plat.ni2c];
      *p = (struct plat_i2c){.base = base, .dma_base = dma, .id = (uint32_t)plat.ni2c, .speed_hz = 100000};
      fdt_compat_u32_at(dtb, compat, idx, "id", &p->id);
      fdt_compat_u32_at(dtb, compat, idx, "clock-div", &p->clock_div);
      fdt_compat_u32_at(dtb, compat, idx, "clock-frequency", &p->speed_hz);
      plat.ni2c++;
    }
  }
}

static void find_tegra(const void *dtb)
{
  static const char i2c[] = "nvidia,tegra210-i2c", pmic[] = "maxim,max77620";
  uint64_t base, size;
  uint32_t addr;
  if (!fdt_compat_reg(dtb, "nvidia,tegra210-car", 0, 0, &base, &size)) {
    plat.car_base = base;
    tegra_car_init(base);
  }
  if (!fdt_compat_reg(dtb, "nvidia,tegra210-gpio", 0, 0, &base, &size))
    plat.tegra_gpio_base = base;
  if (!fdt_compat_reg(dtb, "nvidia,tegra210-pmc", 0, 0, &base, &size))
    plat.pmc_base = base;
  for (int idx = 0; plat.ntegra_i2c < PLAT_TEGRA_I2C_MAX && !fdt_compat_reg(dtb, i2c, idx, 0, &base, &size); idx++) {
    if (node_disabled(dtb, i2c, idx))
      continue;
    struct plat_tegra_i2c *p = &plat.tegra_i2c[plat.ntegra_i2c++];
    *p = (struct plat_tegra_i2c){.base = base, .speed_hz = 100000};
    fdt_compat_u32_at(dtb, i2c, idx, "clock-frequency", &p->speed_hz);
  }
  if (!fdt_compat_parent_reg(dtb, pmic, 0, 0, &base, &size) && !fdt_compat_u32(dtb, pmic, "reg", &addr)) {
    plat.max77620_bus = base;
    plat.max77620_addr = addr;
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

struct tegra_uart {
  uintptr_t base;
  unsigned gate;
};

static const struct tegra_uart tegra_uarts[] = {
    {0x70006000, TEGRA_GATE_UARTA},
    {0x70006040, TEGRA_GATE_UARTB},
    {0x70006200, TEGRA_GATE_UARTC},
    {0x70006300, TEGRA_GATE_UARTD},
};

/* A Tegra UART the loader left unclocked or in reset hangs the bus on first access, so it is not used at all. Without
 * the CAR its state is unknown and it is not touched either. */
static void drop_dead_tegra_uart(void)
{
  if (plat.uart_kind != UART_16550)
    return;
  for (unsigned i = 0; i < sizeof tegra_uarts / sizeof *tegra_uarts; i++)
    if (plat.uart_base == tegra_uarts[i].base && !(plat.car_base && tegra_periph_live(tegra_uarts[i].gate)))
      plat.uart_kind = UART_NONE;
}

void plat_init(const void *dtb)
{
  /* With a device tree, the QEMU virt PL011 default is only right if the tree names it. */
  if (fdt_totalsize(dtb))
    plat.uart_kind = UART_NONE;
  find_uart(dtb);
  find_gic(dtb);
  find_psci(dtb);
  find_pwrap(dtb);
  find_soc_blocks(dtb);
  find_i2c(dtb);
  find_tegra(dtb);
  drop_dead_tegra_uart();
  find_fb(dtb);
  find_cpus(dtb);
}
