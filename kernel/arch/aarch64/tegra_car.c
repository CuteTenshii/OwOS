/* tegra_car.c: gates and resets through the per-bank SET/CLR registers, as Linux's Tegra clock driver does, so no
 * read-modify-write races another CPU. Bank offsets are from drivers/clk/tegra/clk.c. */
#include "tegra_car.h"
#include "counter.h"
#include "mmio.h"

#define RESET_SETTLE_US 2

struct car_bank {
  uint16_t enb, enb_set, enb_clr, rst, rst_set, rst_clr;
};

static const struct car_bank banks[TEGRA_CAR_GATES / 32] = {
    {0x010, 0x320, 0x324, 0x004, 0x300, 0x304}, /* L */
    {0x014, 0x328, 0x32c, 0x008, 0x308, 0x30c}, /* H */
    {0x018, 0x330, 0x334, 0x00c, 0x310, 0x314}, /* U */
    {0x360, 0x440, 0x444, 0x358, 0x430, 0x434}, /* V */
    {0x364, 0x448, 0x44c, 0x35c, 0x438, 0x43c}, /* W */
    {0x280, 0x284, 0x288, 0x28c, 0x290, 0x294}, /* X */
    {0x298, 0x29c, 0x2a0, 0x2a4, 0x2a8, 0x2ac}, /* Y */
};

static uintptr_t car;

void tegra_car_init(uintptr_t base) { car = base; }

static const struct car_bank *bank_of(unsigned gate) { return car && gate < TEGRA_CAR_GATES ? &banks[gate / 32] : 0; }

static int put(unsigned gate, uint16_t reg)
{
  mmio_write32(car + reg, 1u << (gate % 32));
  return 0;
}

static int get(unsigned gate, uint16_t reg) { return (int)((mmio_read32(car + reg) >> (gate % 32)) & 1); }

int tegra_clk_enable(unsigned gate)
{
  const struct car_bank *b = bank_of(gate);
  return b ? put(gate, b->enb_set) : -1;
}

int tegra_clk_disable(unsigned gate)
{
  const struct car_bank *b = bank_of(gate);
  return b ? put(gate, b->enb_clr) : -1;
}

int tegra_clk_enabled(unsigned gate)
{
  const struct car_bank *b = bank_of(gate);
  return b ? get(gate, b->enb) : -1;
}

int tegra_rst_assert(unsigned gate)
{
  const struct car_bank *b = bank_of(gate);
  return b ? put(gate, b->rst_set) : -1;
}

int tegra_rst_deassert(unsigned gate)
{
  const struct car_bank *b = bank_of(gate);
  return b ? put(gate, b->rst_clr) : -1;
}

int tegra_rst_asserted(unsigned gate)
{
  const struct car_bank *b = bank_of(gate);
  return b ? get(gate, b->rst) : -1;
}

int tegra_periph_live(unsigned gate) { return tegra_clk_enabled(gate) == 1 && tegra_rst_asserted(gate) == 0; }

int tegra_periph_reset(unsigned gate)
{
  if (tegra_rst_assert(gate))
    return -1;
  tegra_clk_enable(gate);
  counter_delay_us(RESET_SETTLE_US);
  tegra_rst_deassert(gate);
  return 0;
}

static int source_ok(uint32_t reg) { return car && reg < TEGRA_CAR_SIZE && !(reg & 3); }

int tegra_clk_source_write(uint32_t reg, uint32_t val)
{
  if (!source_ok(reg))
    return -1;
  mmio_write32(car + reg, val);
  return 0;
}

int tegra_clk_source_read(uint32_t reg, uint32_t *val)
{
  if (!source_ok(reg))
    return -1;
  *val = mmio_read32(car + reg);
  return 0;
}
