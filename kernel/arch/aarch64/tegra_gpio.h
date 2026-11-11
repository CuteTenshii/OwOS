/* tegra_gpio.h: Tegra X1 (T210) GPIO controller: 8 banks of 4 ports of 8 pins. Selects GPIO or the pin's special
 * function, direction, input and output. Pinmux, pull, drive and interrupts are not covered. */
#ifndef OWOS_ARCH_AARCH64_TEGRA_GPIO_H
#define OWOS_ARCH_AARCH64_TEGRA_GPIO_H

#include <stdint.h>

#define TEGRA_GPIO_COUNT 256
/* Port 0 is A, 25 is Z, 26 is AA and so on to 31 (FF), as in the device tree's TEGRA_GPIO(port, bit). */
#define TEGRA_GPIO(port, bit) ((port) * 8 + (bit))

void tegra_gpio_init(uintptr_t base);

/* All calls return -1 for a pin outside 0..TEGRA_GPIO_COUNT-1 or before tegra_gpio_init. */
/* gpio 1: the GPIO controller drives the pin; 0: its special function (SFIO) does. */
int tegra_gpio_set_gpio(unsigned pin, int gpio);
int tegra_gpio_is_gpio(unsigned pin);
int tegra_gpio_set_output(unsigned pin, int output);
int tegra_gpio_write(unsigned pin, int level);
/* 0 or 1: the driven level for an output, the pad level for an input. */
int tegra_gpio_read(unsigned pin);

#endif
