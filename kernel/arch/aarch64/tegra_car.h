/* tegra_car.h: Tegra X1 (T210) clock-and-reset controller: peripheral clock gates, resets and clock source registers.
 * A peripheral is named by its gate index, bank * 32 + bit over the banks L H U V W X Y. That equals the tegra210-car
 * device tree clock and reset ID for every gate except UARTB, whose clock ID is 224 but whose gate is bit 7. Rates the
 * loader set are kept unless a driver rewrites a clock source. */
#ifndef OWOS_ARCH_AARCH64_TEGRA_CAR_H
#define OWOS_ARCH_AARCH64_TEGRA_CAR_H

#include <stdint.h>

#define TEGRA_CAR_GATES (7 * 32)
#define TEGRA_CAR_SIZE 0x1000

#define TEGRA_GATE_UARTA 6
#define TEGRA_GATE_UARTB 7
#define TEGRA_GATE_GPIO 8
#define TEGRA_GATE_I2C1 12
#define TEGRA_GATE_I2C5 47
#define TEGRA_GATE_I2C2 54
#define TEGRA_GATE_UARTC 55
#define TEGRA_GATE_UARTD 65
#define TEGRA_GATE_I2C3 67
#define TEGRA_GATE_I2C4 103
#define TEGRA_GATE_I2C6 166

/* PLLP_OUT0 runs at a fixed 408 MHz on T210; peripheral clock sources select it as mux input 0. */
#define TEGRA_PLLP_HZ 408000000u

void tegra_car_init(uintptr_t base);

/* All return -1 for a gate at or past TEGRA_CAR_GATES or before tegra_car_init, else 0 (or the state asked for). */
int tegra_clk_enable(unsigned gate);
int tegra_clk_disable(unsigned gate);
int tegra_clk_enabled(unsigned gate);
int tegra_rst_assert(unsigned gate);
int tegra_rst_deassert(unsigned gate);
int tegra_rst_asserted(unsigned gate);

/* Registers of a running clock and out of reset: only then may its registers be touched without hanging the bus. */
int tegra_periph_live(unsigned gate);

/* Put the block in reset, run its clock, then release it: the controller restarts from its power-on state. */
int tegra_periph_reset(unsigned gate);

/* A CLK_SOURCE_* register at byte offset reg. Return -1 for an offset outside the block or not word aligned. */
int tegra_clk_source_write(uint32_t reg, uint32_t val);
int tegra_clk_source_read(uint32_t reg, uint32_t *val);

#endif
