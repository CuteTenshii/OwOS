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

static int i2c_disabled(const void *dtb, const char *compat, int idx)
{
  char status[16];
  if (fdt_compat_str_at(dtb, compat, idx, "status", status, sizeof status))
    return 0;
  return !(status[0] == 'o' && status[1] == 'k');
}

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
      if (i2c_known(base) || i2c_disabled(dtb, compat, idx) || fdt_compat_reg(dtb, compat, idx, 1, &dma, &size))
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
  find_pwrap(dtb);
  find_soc_blocks(dtb);
  find_i2c(dtb);
  find_fb(dtb);
  find_cpus(dtb);
}
