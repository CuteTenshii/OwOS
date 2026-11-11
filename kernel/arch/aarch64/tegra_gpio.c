/* tegra_gpio.c: per-port registers at bank * 0x100 + port * 4. Writes go through the masked aliases 0x80 above
 * CNF/OE/OUT, where bits 15:8 select the pins to change and bits 7:0 hold their values, so no lock is needed. Layout
 * from Linux drivers/gpio/gpio-tegra.c (tegra210 config). */
#include "tegra_gpio.h"
#include "mmio.h"

#define REG_CNF 0x00
#define REG_OE 0x10
#define REG_OUT 0x20
#define REG_IN 0x30
#define REG_MASKED 0x80
#define BANK_STRIDE 0x100
#define PORT_STRIDE 4

static uintptr_t base;

void tegra_gpio_init(uintptr_t gpio_base) { base = gpio_base; }

static uintptr_t port_reg(unsigned pin, uint32_t reg)
{
  return base + (uintptr_t)(pin / 32) * BANK_STRIDE + (uintptr_t)((pin / 8) % 4) * PORT_STRIDE + reg;
}

static int masked_write(unsigned pin, uint32_t reg, int value)
{
  if (!base || pin >= TEGRA_GPIO_COUNT)
    return -1;
  unsigned bit = pin % 8;
  mmio_write32(port_reg(pin, reg + REG_MASKED), (0x100u << bit) | ((uint32_t)(value != 0) << bit));
  return 0;
}

static int read_bit(unsigned pin, uint32_t reg)
{
  if (!base || pin >= TEGRA_GPIO_COUNT)
    return -1;
  return (int)((mmio_read32(port_reg(pin, reg)) >> (pin % 8)) & 1);
}

int tegra_gpio_set_gpio(unsigned pin, int gpio) { return masked_write(pin, REG_CNF, gpio); }
int tegra_gpio_is_gpio(unsigned pin) { return read_bit(pin, REG_CNF); }
int tegra_gpio_set_output(unsigned pin, int output) { return masked_write(pin, REG_OE, output); }
int tegra_gpio_write(unsigned pin, int level) { return masked_write(pin, REG_OUT, level); }

int tegra_gpio_read(unsigned pin)
{
  int out = read_bit(pin, REG_OE);
  if (out < 0)
    return -1;
  return read_bit(pin, out ? REG_OUT : REG_IN);
}
