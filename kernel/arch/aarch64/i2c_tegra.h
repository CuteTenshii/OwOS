/* i2c_tegra.h: Tegra X1 (T210) I2C controller in packet mode, polled, without DMA. Needs tegra_car_init first: the
 * driver resets the block and clocks it from PLLP. */
#ifndef OWOS_ARCH_AARCH64_I2C_TEGRA_H
#define OWOS_ARCH_AARCH64_I2C_TEGRA_H

#include "i2c.h"
#include <stdint.h>

#define I2C_TEGRA_COUNT 6
#define I2C_TEGRA_MAX_SPEED 1000000
#define I2C_TEGRA_MAX_READ 4096
#define I2C_TEGRA_MAX_WRITE 4084

struct i2c_tegra {
  struct i2c_bus bus;
  uintptr_t base;
  int index;           /* 0..5 for I2C1..I2C6 */
  uint32_t src_div;    /* CLK_SOURCE divider field: module clock = PLLP / (src_div + 1) */
  uint32_t non_hs_div; /* CLK_DIVISOR standard/fast mode field */
};

/* Which controller sits at base: 0..5 for I2C1..I2C6, -1 for none. */
int i2c_tegra_index(uintptr_t base);

/* Work out the clocks for speed_hz (at most I2C_TEGRA_MAX_SPEED), reset and program the controller. Returns 0, or -1
 * for an unknown base, a speed it cannot run, or a controller that will not load its configuration. The bus is ready
 * for i2c_register(&dev->bus, dev->index). */
int i2c_tegra_init(struct i2c_tegra *dev, uintptr_t base, uint32_t speed_hz);

#endif
