/* clk.c: gate and mux control through the SET/CLR registers, as the Linux mt6765 clock driver does. */
#include "clk.h"
#include "mmio.h"
#include <stdbool.h>

static uintptr_t top_base, ifr_base;
static bool reparented[CLK_TOP_MUX_COUNT];

void clk_init(uintptr_t topckgen, uintptr_t infracfg)
{
  top_base = topckgen;
  ifr_base = infracfg;
}

/* A set gate bit means the clock is off. */
void clk_ifr_enable(enum clk_ifr g) { mmio_write32(ifr_base + clk_ifr_gates[g].clr, 1u << clk_ifr_gates[g].bit); }
void clk_ifr_disable(enum clk_ifr g) { mmio_write32(ifr_base + clk_ifr_gates[g].set, 1u << clk_ifr_gates[g].bit); }
int clk_ifr_enabled(enum clk_ifr g)
{
  return !(mmio_read32(ifr_base + clk_ifr_gates[g].sta) & (1u << clk_ifr_gates[g].bit));
}

void clk_mux_enable(enum clk_top_mux m)
{
  const struct clk_mux_def *d = &clk_top_muxes[m];
  if (d->gate >= 0)
    mmio_write32(top_base + d->clr, 1u << d->gate);
  if (reparented[m] && d->upd >= 0) {
    mmio_write32(top_base + CLK_CFG_UPDATE, 1u << d->upd);
    reparented[m] = false;
  }
}

void clk_mux_disable(enum clk_top_mux m)
{
  const struct clk_mux_def *d = &clk_top_muxes[m];
  if (d->gate >= 0)
    mmio_write32(top_base + d->set, 1u << d->gate);
}

int clk_mux_enabled(enum clk_top_mux m)
{
  const struct clk_mux_def *d = &clk_top_muxes[m];
  return d->gate < 0 || !(mmio_read32(top_base + d->cfg) & (1u << d->gate));
}

int clk_mux_set_parent(enum clk_top_mux m, unsigned index)
{
  const struct clk_mux_def *d = &clk_top_muxes[m];
  uint32_t mask = (1u << d->width) - 1;
  if (index > mask)
    return -1;
  if (((mmio_read32(top_base + d->cfg) >> d->shift) & mask) == index)
    return 0;
  mmio_write32(top_base + d->clr, mask << d->shift);
  mmio_write32(top_base + d->set, index << d->shift);
  if (d->upd >= 0) {
    mmio_write32(top_base + CLK_CFG_UPDATE, 1u << d->upd);
    reparented[m] = true;
  }
  return 0;
}

unsigned clk_mux_parent(enum clk_top_mux m)
{
  const struct clk_mux_def *d = &clk_top_muxes[m];
  return (mmio_read32(top_base + d->cfg) >> d->shift) & ((1u << d->width) - 1);
}
