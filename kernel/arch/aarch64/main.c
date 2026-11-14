/* main.c: AArch64 boot: console, MMU, memory and the board's devices, then the shared kernel (scheduler, live
 * filesystem, the embedded programs and init) and the recovery console on the UART. */
#include "arch.h"
#include "blk.h"
#include "clk.h"
#include "console.h"
#include "entropy.h"
#include "fpu.h"
#include "fs.h"
#include "gpio.h"
#include "i2c_mtk.h"
#include "i2c_tegra.h"
#include "install.h"
#include "irqflags.h"
#include "keyboard.h"
#include "kstring.h"
#include "max77620.h"
#include "net.h"
#include "pwrap.h"
#include "sched.h"
#include "shell.h"
#include "tegra_car.h"
#include "tegra_gpio.h"
#include "timer.h"
#include "userland.h"
#include "wallclock.h"

#define I2C_SRC_HZ 26000000 /* i2c_sel parent 0: the crystal */

_Static_assert(I2C_MTK_DMA_MAX <= PMM_FRAME_SIZE, "an I2C DMA buffer is one frame");

static struct i2c_mtk i2c_ctl[PLAT_I2C_MAX];

static int i2c_start_one(struct i2c_mtk *ctl, const struct plat_i2c *p)
{
  phys_t tx = pmm_alloc_frame(), rx = pmm_alloc_frame();
  struct i2c_mtk_config cfg = {.base = p->base,
                               .dma_base = p->dma_base,
                               .src_hz = I2C_SRC_HZ,
                               .clock_div = p->clock_div,
                               .speed_hz = p->speed_hz,
                               .tx_buf = PHYS_TO_PTR(tx),
                               .rx_buf = PHYS_TO_PTR(rx)};
  if (tx && rx && !i2c_mtk_init(ctl, &cfg) && !i2c_register(&ctl->bus, (int)p->id))
    return 0;
  if (tx)
    pmm_free_frame(tx);
  if (rx)
    pmm_free_frame(rx);
  return -1;
}

/* Bus timing is computed from a known source rate, so the I2C clock is moved to the crystal first. */
static void i2c_start(void)
{
  if (!plat.ni2c)
    return;
  if (!plat.topckgen_base || !plat.infracfg_base) {
    uart_puts("I2C: no clock controller in the DTB, buses left off\n");
    return;
  }
  clk_mux_set_parent(CLK_TOP_I2C_SEL, 0);
  clk_mux_enable(CLK_TOP_I2C_SEL);
  clk_ifr_enable(CLK_IFR_I2C_AP);
  clk_ifr_enable(CLK_IFR_AP_DMA);
  for (int i = 0; i < plat.ni2c; i++) {
    const struct plat_i2c *p = &plat.i2c[i];
    char line[64];
    if (i2c_start_one(&i2c_ctl[i], p))
      ksnprintf(line, sizeof line, "I2C%d at %x: not started (clock-div %d, or id in use)\n", (int)p->id,
                (unsigned)p->base, (int)p->clock_div);
    else
      ksnprintf(line, sizeof line, "I2C%d at %x: %d kHz\n", (int)p->id, (unsigned)p->base,
                (int)(i2c_ctl[i].bus.speed_hz / 1000));
    uart_puts(line);
  }
}

#define PMC_PWRGATE_STATUS 0x38
#define PMC_PARTITION_SOR 17
#define I2C6_INDEX 5

static struct i2c_tegra tegra_i2c[PLAT_TEGRA_I2C_MAX];

/* I2C6 drives the DPAUX pads in the SOR partition; with the partition gated its registers hang the bus. */
static int tegra_i2c_powered(int index)
{
  if (index != I2C6_INDEX)
    return 1;
  return plat.pmc_base && (mmio_read32(plat.pmc_base + PMC_PWRGATE_STATUS) & (1u << PMC_PARTITION_SOR));
}
static struct max77620 pmic77620;

static void max77620_off(void) { max77620_poweroff(&pmic77620); }

static void tegra_pmic_start(void)
{
  struct i2c_bus *bus = 0;
  char line[64];
  for (int i = 0; i < plat.ntegra_i2c; i++)
    if (tegra_i2c[i].bus.xfer && tegra_i2c[i].base == plat.max77620_bus)
      bus = &tegra_i2c[i].bus;
  if (!bus) {
    uart_puts("PMIC max77620: its I2C bus is not running\n");
    return;
  }
  if (max77620_probe(&pmic77620, bus, (uint16_t)plat.max77620_addr)) {
    uart_puts("PMIC max77620 not responding\n");
    return;
  }
  arch_poweroff_fallback = max77620_off;
  ksnprintf(line, sizeof line, "PMIC max77620, OTP %x, ES %x\n", (unsigned)pmic77620.otp, (unsigned)pmic77620.es);
  uart_puts(line);
}

static void tegra_start(void)
{
  if (!plat.car_base) {
    if (plat.tegra_gpio_base || plat.ntegra_i2c)
      uart_puts("Tegra: no clock-and-reset controller in the DTB, SoC drivers left off\n");
    return;
  }
  if (plat.tegra_gpio_base) {
    tegra_clk_enable(TEGRA_GATE_GPIO);
    tegra_rst_deassert(TEGRA_GATE_GPIO);
    tegra_gpio_init(plat.tegra_gpio_base);
  }
  for (int i = 0; i < plat.ntegra_i2c; i++) {
    const struct plat_tegra_i2c *p = &plat.tegra_i2c[i];
    struct i2c_tegra *d = &tegra_i2c[i];
    int index = i2c_tegra_index(p->base);
    char line[64];
    if (index >= 0 && !tegra_i2c_powered(index)) {
      ksnprintf(line, sizeof line, "I2C%d at %x: power partition off, not started\n", index + 1, (unsigned)p->base);
    } else if (index < 0 || i2c_tegra_init(d, p->base, p->speed_hz) || i2c_register(&d->bus, d->index)) {
      d->bus.xfer = 0;
      ksnprintf(line, sizeof line, "I2C at %x: not started\n", (unsigned)p->base);
    } else {
      ksnprintf(line, sizeof line, "I2C%d at %x: %d kHz\n", index + 1, (unsigned)p->base,
                (int)(d->bus.speed_hz / 1000));
    }
    uart_puts(line);
  }
  if (plat.max77620_bus)
    tegra_pmic_start();
}

/* Started after interrupts are on, so the console comes up at once. */
static void boot_bringup(void)
{
  userland_start();
  sched_exit(0);
}

static const char *uart_name(void)
{
  return plat.uart_kind == UART_PL011   ? ", uart pl011, "
         : plat.uart_kind == UART_16550 ? ", uart 16550, "
                                        : ", no uart, ";
}

void arch_main(void)
{
  plat_init((const void *)boot_dtb);
  uart_init();
  uart_puts("OwOS aarch64 boot, EL");
  uart_putc('0' + (char)boot_el);
  uart_puts(" -> EL1, MIDR ");
  uart_puthex(SYSREG_READ(midr_el1));
  uart_putc('\n');
  uart_puts(plat.gic_kind == GIC_V3 ? "GICv3, cpus " : "GICv2, cpus ");
  uart_puthex((uint64_t)plat.ncpus);
  uart_puts(uart_name());
  uart_puts(plat.psci_smc < 0 ? "psci by exception level" : plat.psci_smc ? "psci smc" : "psci hvc");
  uart_putc('\n');

  mmu_init();
  fbcon_init(&plat.fb);
  if (arch_mem_init((const void *)boot_dtb)) {
    uart_puts("no usable memory map in the DTB\n");
    arch_poweroff();
  }
  if (plat.topckgen_base && plat.infracfg_base)
    clk_init(plat.topckgen_base, plat.infracfg_base);
  if (plat.gpio_base)
    gpio_init(plat.gpio_base);
  if (plat.pwrap_base) {
    const char *pmic = pwrap_init(plat.pwrap_base) ? 0 : pmic_probe();
    uart_puts("PMIC ");
    uart_puts(pmic ? pmic : "not responding");
    uart_putc('\n');
  }
  i2c_start();
  tegra_start();
  con_printf("RAM free %u KiB\n", pmm_free_kib());

  fpu_cpu_init();
  entropy_init();
  gic_init();
  timer_init();
  sched_init();
  net_init();
  blk_init();
  /* Live owofs adds per-file cluster rounding and its directory on top of the payload itself. */
  fs_init(install_payload_bytes() / 512u * 9u / 8u + 8u * 2048u);
  /* No daemons yet: the compositor, network and audio services have no devices to drive here. */
  userland_prepare("");
  con_write("usermode: ready.\n");
  wallclock_init();

  irq_enable();
  con_printf("smp: %d/%d cores online.\n", smp_start(), plat.ncpus);
  sched_create_thread("bootup", boot_bringup);
  shell_init();

  for (;;) {
    char c;
    if (keyboard_take_interrupt())
      console_interrupt();
    while (keyboard_poll_text(&c, 0))
      console_feed(c);
    cpu_wait();
  }
}
