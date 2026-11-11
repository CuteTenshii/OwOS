/* tegra_test.c: the Tegra X1 clock-and-reset (tegra_car.c), GPIO (tegra_gpio.c), I2C controller (i2c_tegra.c) and
 * MAX77620 (max77620.c) drivers against the fake SoC in tegra_host.h. */
#define OWOS_SUITE_TEST
#include "../kernel/arch/aarch64/i2c_tegra.h"
#include "../kernel/arch/aarch64/max77620.h"
#include "../kernel/arch/aarch64/tegra_car.h"
#include "../kernel/arch/aarch64/tegra_gpio.h"
#include "tegra_host.h"
#include "test.h"
#include <string.h>

struct fake_tegra tegra_fake;

#define CNFG 0x000
#define TX_FIFO 0x050
#define RX_FIFO 0x054
#define FIFO_CONTROL 0x05c
#define FIFO_STATUS 0x060
#define INT_STATUS 0x068
#define CLK_DIVISOR 0x06c
#define BUS_CLEAR_CNFG 0x084
#define CONFIG_LOAD 0x08c
#define INTERFACE_TIMING_0 0x094
#define PACKET_MODE_EN (1u << 10)
#define INT_ARB_LOST (1u << 2)
#define INT_NO_ACK (1u << 3)
#define INT_XFER_COMPLETE (1u << 7)
#define INT_BUS_CLR_DONE (1u << 11)

static const uint32_t i2c_bases[6] = {0x7000c000, 0x7000c400, 0x7000c500, 0x7000c700, 0x7000d000, 0x7000d100};
static const unsigned i2c_gates[6] = {12, 54, 67, 103, 47, 166};
static const uint16_t bank_regs[7][6] = {
    {0x010, 0x320, 0x324, 0x004, 0x300, 0x304}, {0x014, 0x328, 0x32c, 0x008, 0x308, 0x30c},
    {0x018, 0x330, 0x334, 0x00c, 0x310, 0x314}, {0x360, 0x440, 0x444, 0x358, 0x430, 0x434},
    {0x364, 0x448, 0x44c, 0x35c, 0x438, 0x43c}, {0x280, 0x284, 0x288, 0x28c, 0x290, 0x294},
    {0x298, 0x29c, 0x2a0, 0x2a4, 0x2a8, 0x2ac},
};

static uint32_t *car_reg(uint32_t off) { return &tegra_fake.car[off / 4]; }

static int gate_bit(unsigned gate, int which)
{
  return (int)((*car_reg(bank_regs[gate / 32][which]) >> (gate % 32)) & 1);
}

static void i2c_hw_reset(int i) { memset(&tegra_fake.i2c[i], 0, sizeof tegra_fake.i2c[i]); }

static void car_write(uint32_t off, uint32_t v)
{
  if (tegra_fake.ncar_log < FAKE_LOG) {
    tegra_fake.car_log[tegra_fake.ncar_log].addr = off;
    tegra_fake.car_log[tegra_fake.ncar_log++].val = v;
  }
  for (int b = 0; b < 7; b++) {
    const uint16_t *r = bank_regs[b];
    if (off == r[1] || off == r[2] || off == r[4] || off == r[5]) {
      uint32_t *state = car_reg(off == r[1] || off == r[2] ? r[0] : r[3]);
      if (off == r[1] || off == r[4])
        *state |= v;
      else
        *state &= ~v;
      if (off == r[4])
        for (int i = 0; i < 6; i++)
          if (i2c_gates[i] / 32 == (unsigned)b && (v >> (i2c_gates[i] % 32)) & 1)
            i2c_hw_reset(i);
      return;
    }
  }
  *car_reg(off) = v;
}

static void gpio_write(uint32_t off, uint32_t v)
{
  uint32_t in_bank = off % 0x100;
  if (in_bank >= 0x80 && in_bank < 0xb0) {
    uint32_t *r = &tegra_fake.gpio[(off - 0x80) / 4], mask = (v >> 8) & 0xff;
    *r = (*r & ~mask) | (v & mask);
    return;
  }
  tegra_fake.gpio[off / 4] = v;
}

static int dev_xfer(uint8_t addr, int rd, uint8_t *buf, uint32_t len)
{
  uint8_t *mem, *ptr;
  if (addr == FAKE_EEPROM)
    mem = tegra_fake.eeprom, ptr = &tegra_fake.eeprom_ptr;
  else if (addr == FAKE_PMIC)
    mem = tegra_fake.pmic, ptr = &tegra_fake.pmic_ptr;
  else
    return -1;
  if (rd) {
    for (uint32_t i = 0; i < len; i++)
      buf[i] = mem[(*ptr)++];
  } else {
    *ptr = buf[0];
    for (uint32_t i = 1; i < len; i++)
      mem[(*ptr)++] = buf[i];
  }
  return 0;
}

static void packet_done(struct fake_tegra_i2c *c, int nacked)
{
  c->packets++;
  c->nhdr = 0;
  if (tegra_fake.hang)
    return;
  if (tegra_fake.arb_lost)
    c->reg[INT_STATUS / 4] |= INT_ARB_LOST;
  else if (nacked || tegra_fake.nack_all)
    c->reg[INT_STATUS / 4] |= INT_NO_ACK;
  else
    c->reg[INT_STATUS / 4] |= INT_XFER_COMPLETE;
}

static void tx_fifo(struct fake_tegra_i2c *c, uint32_t v)
{
  if (c->nhdr < 3) {
    c->hdr[c->nhdr++] = v;
    if (c->nhdr < 3)
      return;
    memcpy(c->last_hdr, c->hdr, sizeof c->hdr);
    uint32_t len = c->hdr[1] + 1;
    uint8_t addr = (uint8_t)((c->hdr[2] >> 1) & 0x7f);
    int rd = (c->hdr[2] >> 19) & 1;
    if ((c->hdr[0] & 0xf0) != 0x10 || ((c->hdr[0] >> 16) & 0xff) != 1 || !((c->hdr[2] >> 17) & 1) ||
        !(c->reg[CNFG / 4] & PACKET_MODE_EN))
      c->bad_headers++;
    c->repeat_starts += (c->hdr[2] >> 16) & 1;
    if (rd) {
      uint8_t buf[4096] = {0};
      int nack = tegra_fake.nack_all || dev_xfer(addr, 1, buf, len);
      if (!nack && !tegra_fake.hang && !tegra_fake.arb_lost)
        for (uint32_t at = 0; at < len; at += 4) {
          uint32_t w = 0;
          for (uint32_t k = 0; k < 4 && at + k < len; k++)
            w |= (uint32_t)buf[at + k] << (8 * k);
          c->rx[c->rx_tail++] = w;
        }
      packet_done(c, nack);
    } else {
      c->payload_left = len;
      c->wlen = 0;
    }
    return;
  }
  for (int k = 0; k < 4 && c->payload_left; k++, c->payload_left--)
    c->wbuf[c->wlen++] = (uint8_t)(v >> (8 * k));
  if (!c->payload_left) {
    uint8_t addr = (uint8_t)((c->hdr[2] >> 1) & 0x7f);
    int nack = tegra_fake.nack_all || dev_xfer(addr, 0, c->wbuf, c->wlen);
    packet_done(c, nack);
  }
}

static int i2c_at(uintptr_t a, uint32_t *off)
{
  for (int i = 0; i < 6; i++)
    if (a >= i2c_bases[i] && a < i2c_bases[i] + 0x100) {
      *off = (uint32_t)(a - i2c_bases[i]);
      if (!gate_bit(i2c_gates[i], 0) || gate_bit(i2c_gates[i], 3))
        tegra_fake.dead_access++;
      return i;
    }
  return -1;
}

uint32_t fake_tegra_read32(uintptr_t a)
{
  uint32_t off;
  if (a >= FAKE_CAR && a < FAKE_CAR + 0x1000)
    return *car_reg((uint32_t)(a - FAKE_CAR));
  if (a >= FAKE_GPIO && a < FAKE_GPIO + 0x1000)
    return tegra_fake.gpio[(a - FAKE_GPIO) / 4];
  int i = i2c_at(a, &off);
  if (i < 0)
    return 0xdeadbeef;
  struct fake_tegra_i2c *c = &tegra_fake.i2c[i];
  if (off == FIFO_STATUS) {
    int rx = c->rx_tail - c->rx_head;
    return 8u << 4 | (uint32_t)(rx > 8 ? 8 : rx);
  }
  if (off == RX_FIFO)
    return c->rx_head < c->rx_tail ? c->rx[c->rx_head++] : 0;
  return c->reg[off / 4];
}

void fake_tegra_write32(uintptr_t a, uint32_t v)
{
  uint32_t off;
  if (a >= FAKE_CAR && a < FAKE_CAR + 0x1000) {
    car_write((uint32_t)(a - FAKE_CAR), v);
    return;
  }
  if (a >= FAKE_GPIO && a < FAKE_GPIO + 0x1000) {
    gpio_write((uint32_t)(a - FAKE_GPIO), v);
    return;
  }
  int i = i2c_at(a, &off);
  if (i < 0)
    return;
  struct fake_tegra_i2c *c = &tegra_fake.i2c[i];
  if (off == TX_FIFO)
    tx_fifo(c, v);
  else if (off == INT_STATUS)
    c->reg[off / 4] &= ~v;
  else if (off == CONFIG_LOAD)
    c->reg[off / 4] = 0;
  else if (off == FIFO_CONTROL) {
    if (v & 1)
      c->rx_head = c->rx_tail = 0;
    if (v & 2)
      c->nhdr = 0;
    c->reg[off / 4] = v & ~3u;
  } else if (off == BUS_CLEAR_CNFG && (v & 1)) {
    c->bus_clears++;
    c->reg[INT_STATUS / 4] |= INT_BUS_CLR_DONE;
    c->reg[off / 4] = v & ~1u;
  } else {
    c->reg[off / 4] = v;
  }
}

static void reset(void)
{
  memset(&tegra_fake, 0, sizeof tegra_fake);
  tegra_car_init(FAKE_CAR);
  tegra_gpio_init(FAKE_GPIO);
}

static int logged(int from, uint32_t off, uint32_t val)
{
  for (int i = from; i < tegra_fake.ncar_log; i++)
    if (tegra_fake.car_log[i].addr == off && tegra_fake.car_log[i].val == val)
      return i;
  return -1;
}

static void check_car_banks(void)
{
  static const struct {
    unsigned gate;
    uint32_t set, clr, rst_set, bit;
  } cases[] = {
      {12, 0x320, 0x324, 0x300, 12}, {47, 0x328, 0x32c, 0x308, 15}, {67, 0x330, 0x334, 0x310, 3},
      {103, 0x440, 0x444, 0x430, 7}, {130, 0x448, 0x44c, 0x438, 2}, {166, 0x284, 0x288, 0x290, 6},
      {200, 0x29c, 0x2a0, 0x2a8, 8},
  };
  reset();
  for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
    tegra_fake.ncar_log = 0;
    expect_eq(tegra_clk_enable(cases[i].gate), 0);
    expect_eq(tegra_clk_disable(cases[i].gate), 0);
    expect_eq(tegra_rst_assert(cases[i].gate), 0);
    expect_eq(tegra_fake.ncar_log, 3);
    expect_eq(tegra_fake.car_log[0].addr, cases[i].set);
    expect_eq(tegra_fake.car_log[0].val, 1u << cases[i].bit);
    expect_eq(tegra_fake.car_log[1].addr, cases[i].clr);
    expect_eq(tegra_fake.car_log[2].addr, cases[i].rst_set);
  }
  expect_eq(tegra_clk_enable(TEGRA_CAR_GATES), -1);
  expect_eq(tegra_rst_asserted(TEGRA_CAR_GATES), -1);
  tegra_car_init(0);
  expect_eq(tegra_clk_enable(12), -1);
}

static void check_car_state(void)
{
  reset();
  expect_eq(tegra_periph_live(TEGRA_GATE_UARTB), 0);
  expect_eq(tegra_clk_enable(TEGRA_GATE_UARTB), 0);
  expect_eq(tegra_clk_enabled(TEGRA_GATE_UARTB), 1);
  expect_eq(tegra_periph_live(TEGRA_GATE_UARTB), 1);
  expect_eq(tegra_rst_assert(TEGRA_GATE_UARTB), 0);
  expect_eq(tegra_periph_live(TEGRA_GATE_UARTB), 0); /* clocked but held in reset */
  expect_eq(tegra_clk_enabled(TEGRA_GATE_UARTA), 0); /* neighbouring bit untouched */

  tegra_fake.ncar_log = 0;
  expect_eq(tegra_periph_reset(TEGRA_GATE_I2C5), 0);
  int a = logged(0, 0x308, 1u << 15), e = logged(0, 0x328, 1u << 15), d = logged(0, 0x30c, 1u << 15);
  expect(a >= 0 && e > a && d > e); /* reset held while the clock starts, released last */
  expect_eq(tegra_periph_live(TEGRA_GATE_I2C5), 1);

  uint32_t v = 0;
  expect_eq(tegra_clk_source_write(0x128, 0x1234), 0);
  expect_eq(tegra_clk_source_read(0x128, &v), 0);
  expect_eq(v, 0x1234);
  expect_eq(tegra_clk_source_write(0x1000, 0), -1);
  expect_eq(tegra_clk_source_write(0x129, 0), -1);
}

static void check_gpio(void)
{
  reset();
  unsigned pin = TEGRA_GPIO(9, 5); /* port J: bank 2, port 1, bit 5 */
  uint32_t port = 2 * 0x100 + 1 * 4;
  expect_eq(pin, 77);
  memset(tegra_fake.gpio, 0, sizeof tegra_fake.gpio);
  tegra_fake.gpio[(port + 0x20) / 4] = 0x0f; /* other pins of the port already driven */
  expect_eq(tegra_gpio_set_gpio(pin, 1), 0);
  expect_eq(tegra_gpio_set_output(pin, 1), 0);
  expect_eq(tegra_gpio_write(pin, 1), 0);
  expect_eq(tegra_fake.gpio[port / 4], 1u << 5);
  expect_eq(tegra_fake.gpio[(port + 0x10) / 4], 1u << 5);
  expect_eq(tegra_fake.gpio[(port + 0x20) / 4], 0x2fu);
  expect_eq(tegra_gpio_is_gpio(pin), 1);
  expect_eq(tegra_gpio_read(pin), 1); /* an output reads back what it drives */
  expect_eq(tegra_gpio_write(pin, 0), 0);
  expect_eq(tegra_fake.gpio[(port + 0x20) / 4], 0x0fu);

  expect_eq(tegra_gpio_set_output(pin, 0), 0);
  tegra_fake.gpio[(port + 0x30) / 4] = 1u << 5;
  expect_eq(tegra_gpio_read(pin), 1); /* an input reads the pad */
  expect_eq(tegra_gpio_set_gpio(pin, 0), 0);
  expect_eq(tegra_gpio_is_gpio(pin), 0);

  expect_eq(tegra_gpio_write(TEGRA_GPIO_COUNT - 1, 1), 0);
  expect_eq(tegra_fake.gpio[(7 * 0x100 + 3 * 4 + 0x20) / 4], 1u << 7);
  expect_eq(tegra_gpio_write(TEGRA_GPIO_COUNT, 1), -1);
  expect_eq(tegra_gpio_read(TEGRA_GPIO_COUNT), -1);
  tegra_gpio_init(0);
  expect_eq(tegra_gpio_read(0), -1);
}

static struct i2c_tegra ctl;

static uint32_t i2c_reg(int i, uint32_t off) { return tegra_fake.i2c[i].reg[off / 4]; }

static void check_i2c_timing(void)
{
  uint32_t src = 0;
  reset();
  expect_eq(i2c_tegra_init(&ctl, FAKE_I2C5, 400000), 0);
  expect_eq(ctl.index, 4);
  expect_eq(tegra_clk_source_read(0x128, &src), 0);
  expect_eq(src, 4); /* PLLP / 5 = 81.6 MHz, 208 module clocks per SCL period */
  expect_eq(ctl.bus.speed_hz, 392307);
  expect_eq(i2c_reg(4, CLK_DIVISOR), 0x00190001u);
  expect_eq(i2c_reg(4, INTERFACE_TIMING_0), 0x0204u);
  expect_eq(i2c_reg(4, CNFG), 0x2c00u);
  expect_eq(tegra_periph_live(TEGRA_GATE_I2C5), 1);

  expect_eq(i2c_tegra_init(&ctl, FAKE_I2C1, 100000), 0);
  expect_eq(tegra_clk_source_read(0x124, &src), 0);
  expect_eq(src, 19);
  expect_eq(ctl.bus.speed_hz, 98076);
  expect(ctl.bus.speed_hz <= 100000);

  expect_eq(i2c_tegra_init(&ctl, FAKE_I2C1, 1000000), 0); /* fast-mode plus */
  expect_eq(i2c_reg(0, CLK_DIVISOR), 0x00100001u);
  expect_eq(ctl.bus.speed_hz, 1000000);

  expect_eq(i2c_tegra_init(&ctl, 0x7000c100, 100000), -1);
  expect_eq(i2c_tegra_init(&ctl, FAKE_I2C1, 0), -1);
  expect_eq(i2c_tegra_init(&ctl, FAKE_I2C1, 1000001), -1);
  expect_eq(i2c_tegra_init(&ctl, FAKE_I2C1, 20), -1); /* needs a divider wider than 16 bits */
  expect_eq(i2c_tegra_index(0x7000d100), 5);
  expect_eq(tegra_fake.dead_access, 0);
}

static void start_bus(void)
{
  reset();
  for (int i = 0; i < 256; i++)
    tegra_fake.eeprom[i] = (uint8_t)(0xa0 ^ i);
  expect_eq(i2c_tegra_init(&ctl, FAKE_I2C5, 400000), 0);
}

static void check_i2c_transfers(void)
{
  start_bus();
  uint8_t w[7] = {0x10, 1, 2, 3, 4, 5, 6}, r[6] = {0};
  expect_eq(i2c_write(&ctl.bus, FAKE_EEPROM, w, 7), I2C_OK);
  expect_eq(memcmp(tegra_fake.eeprom + 0x10, w + 1, 6), 0);
  struct fake_tegra_i2c *c = &tegra_fake.i2c[4];
  expect_eq(c->last_hdr[1], 6);                       /* payload size - 1 */
  expect_eq(c->last_hdr[2] & 0xff, FAKE_EEPROM << 1); /* 7-bit address shifted up */
  expect_eq(c->repeat_starts, 0);

  expect_eq(i2c_reg_read(&ctl.bus, FAKE_EEPROM, 0x10, r, 6), I2C_OK);
  expect_eq(memcmp(r, w + 1, 6), 0);
  expect_eq(c->repeat_starts, 1); /* register pointer then data, joined by a repeated start */
  expect_eq(c->last_hdr[2] >> 19 & 1, 1);

  uint8_t big[300];
  expect_eq(i2c_reg_read(&ctl.bus, FAKE_EEPROM, 0, big, sizeof big), I2C_OK);
  for (int i = 0; i < 256; i++)
    if (big[i] != tegra_fake.eeprom[i])
      expect_eq(big[i], tegra_fake.eeprom[i]);
  expect_eq(big[256], tegra_fake.eeprom[0]); /* the fake's pointer wraps */

  expect_eq(c->bad_headers, 0);
  expect_eq(tegra_fake.dead_access, 0);
}

static void check_i2c_errors(void)
{
  start_bus();
  uint8_t r[2], w[2] = {0, 0};
  struct fake_tegra_i2c *c = &tegra_fake.i2c[4];

  expect_eq(i2c_read(&ctl.bus, 0x51, r, 2), I2C_ERR_NACK);
  expect_eq(i2c_reg(4, CNFG), 0x2c00u); /* reset and programmed again */
  expect_eq(i2c_reg_read(&ctl.bus, FAKE_EEPROM, 0, r, 2), I2C_OK);

  tegra_fake.hang = 1;
  expect_eq(i2c_write(&ctl.bus, FAKE_EEPROM, w, 2), I2C_ERR_TIMEOUT);
  tegra_fake.hang = 0;
  expect_eq(i2c_reg_read(&ctl.bus, FAKE_EEPROM, 0, r, 2), I2C_OK);

  tegra_fake.arb_lost = 1;
  expect_eq(i2c_write(&ctl.bus, FAKE_EEPROM, w, 2), I2C_ERR_BUS);
  tegra_fake.arb_lost = 0;
  expect_eq(c->bus_clears, 1);
  expect_eq(i2c_reg_read(&ctl.bus, FAKE_EEPROM, 0, r, 2), I2C_OK);

  static uint8_t huge[I2C_TEGRA_MAX_WRITE + 1];
  expect_eq(i2c_write(&ctl.bus, FAKE_EEPROM, huge, sizeof huge), I2C_ERR_INVAL);
  expect_eq(tegra_fake.dead_access, 0);
}

static void check_max77620(void)
{
  struct max77620 pmic;
  start_bus();
  tegra_fake.pmic[MAX77620_REG_CID4] = 0x35;
  tegra_fake.pmic[MAX77620_REG_CID5] = 0x82;
  tegra_fake.pmic[MAX77620_REG_ONOFFCNFG1] = 0x18;
  tegra_fake.pmic[MAX77620_REG_ONOFFCNFG2] = 0x80 | 0x04;
  expect_eq(max77620_probe(&pmic, &ctl.bus, FAKE_PMIC), I2C_OK);
  expect_eq(pmic.otp, 0x35);
  expect_eq(pmic.es, 0x82);

  uint8_t v = 0;
  expect_eq(max77620_write(&pmic, 0x20, 0x5a), I2C_OK);
  expect_eq(max77620_read(&pmic, 0x20, &v), I2C_OK);
  expect_eq(v, 0x5a);

  expect_eq(max77620_poweroff(&pmic), I2C_OK);
  expect_eq(tegra_fake.pmic[MAX77620_REG_ONOFFCNFG2], 0x04); /* no wake from the soft reset */
  expect_eq(tegra_fake.pmic[MAX77620_REG_ONOFFCNFG1], 0x98); /* SFT_RST, other bits kept */

  struct max77620 absent;
  expect_eq(max77620_probe(&absent, &ctl.bus, 0x3d), I2C_ERR_NACK);
  struct max77620 none = {0};
  expect_eq(max77620_poweroff(&none), I2C_ERR_INVAL);
}

int main(void)
{
  test("CAR gate and reset registers in every bank", check_car_banks);
  test("CAR state, liveness and reset order", check_car_state);
  test("GPIO masked writes and reads", check_gpio);
  test("I2C clock and timing for each speed", check_i2c_timing);
  test("I2C packet transfers and repeated start", check_i2c_transfers);
  test("I2C NACK, timeout and arbitration recovery", check_i2c_errors);
  test("MAX77620 probe, registers and power-off", check_max77620);
  return 0;
}
