/* gpio.h: MT6765 GPIO: pin mode, direction, input and output (pinctrl "iocfg0" block). Pull, drive and EINT are not
 * covered. */
#ifndef OWOS_ARCH_AARCH64_GPIO_H
#define OWOS_ARCH_AARCH64_GPIO_H

#include <stdint.h>

#define GPIO_COUNT 203
#define GPIO_MODE_GPIO 0

void gpio_init(uintptr_t iocfg0);

/* Pin mux function 0..15. All calls return -1 for a pin outside 0..GPIO_COUNT-1. */
int gpio_set_mode(unsigned pin, unsigned mode);
int gpio_get_mode(unsigned pin);
int gpio_set_output(unsigned pin, int output);
int gpio_write(unsigned pin, int level);
/* Level on the pad: 0 or 1. */
int gpio_read(unsigned pin);

#endif
