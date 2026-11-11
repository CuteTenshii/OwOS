/* main.c: AArch64 bring-up: console, MMU, memory, scheduler, interrupts and the other CPUs. */
#include "arch.h"
#include "clk.h"
#include "gpio.h"
#include "i2c_mtk.h"
#include "kstring.h"
#include "pwrap.h"

#define I2C_SRC_HZ 26000000 /* i2c_sel parent 0: the crystal */

_Static_assert(I2C_MTK_DMA_MAX <= PMM_FRAME_SIZE, "an I2C DMA buffer is one frame");

int selftest_run(void);

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
  uart_puts(plat.uart_kind == UART_PL011 ? ", uart pl011, " : ", uart mtk, ");
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
  uart_puts("RAM free KiB ");
  uart_puthex(pmm_free_kib());
  uart_putc('\n');
  sched_init();
  gic_init();
  timer_init();
  __asm__ volatile("msr daifclr, #2");
  smp_start();

  selftest_run();
  arch_poweroff();
}
