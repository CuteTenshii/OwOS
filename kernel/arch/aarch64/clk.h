/* clk.h: MT6765 clock gates (infracfg) and gated muxes (topckgen). Rates are left as the bootloader set them. */
#ifndef OWOS_ARCH_AARCH64_CLK_H
#define OWOS_ARCH_AARCH64_CLK_H

#include "mt6765_clk.h"
#include <stdint.h>

void clk_init(uintptr_t topckgen, uintptr_t infracfg);

void clk_ifr_enable(enum clk_ifr gate);
void clk_ifr_disable(enum clk_ifr gate);
int clk_ifr_enabled(enum clk_ifr gate);

void clk_mux_enable(enum clk_top_mux mux);
void clk_mux_disable(enum clk_top_mux mux);
int clk_mux_enabled(enum clk_top_mux mux);
/* Select parent `index` (0 is the 26 MHz crystal on every mux listed here). Returns -1 if index does not fit. */
int clk_mux_set_parent(enum clk_top_mux mux, unsigned index);
unsigned clk_mux_parent(enum clk_top_mux mux);

#endif
