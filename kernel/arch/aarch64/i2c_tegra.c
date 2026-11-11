/* i2c_tegra.c: T210 I2C controller. Register use, timing values and the packet format follow Linux's i2c-tegra.c
 * (tegra210_i2c_hw); completion and errors are polled from INT_STATUS instead of taken as interrupts. */
#include "i2c_tegra.h"
#include "counter.h"
#include "mmio.h"
#include "tegra_car.h"

#define REG_CNFG 0x000
#define REG_SL_CNFG 0x020
#define REG_SL_ADDR1 0x02c
#define REG_SL_ADDR2 0x030
#define REG_TX_FIFO 0x050
#define REG_RX_FIFO 0x054
#define REG_FIFO_CONTROL 0x05c
#define REG_FIFO_STATUS 0x060
#define REG_INT_MASK 0x064
#define REG_INT_STATUS 0x068
#define REG_CLK_DIVISOR 0x06c
#define REG_BUS_CLEAR_CNFG 0x084
#define REG_CONFIG_LOAD 0x08c
#define REG_INTERFACE_TIMING_0 0x094

#define CNFG_DEBOUNCE_2 (2u << 12)
#define CNFG_NEW_MASTER_FSM (1u << 11)
#define CNFG_PACKET_MODE_EN (1u << 10)
#define SL_CNFG_NACK (1u << 1)
#define SL_CNFG_NEWSL (1u << 2)
#define SL_ADDR1_DEFAULT 0xfcu

#define FIFO_RX_FLUSH (1u << 0)
#define FIFO_TX_FLUSH (1u << 1)
#define FIFO_TX_TRIG(n) (((n) - 1u) << 5)
#define FIFO_RX_TRIG(n) (((n) - 1u) << 2)
#define FIFO_STATUS_TX(v) (((v) >> 4) & 0xfu)
#define FIFO_STATUS_RX(v) ((v) & 0xfu)

#define INT_ARB_LOST (1u << 2)
#define INT_NO_ACK (1u << 3)
#define INT_PACKET_XFER_COMPLETE (1u << 7)
#define INT_BUS_CLR_DONE (1u << 11)

#define CONFIG_LOAD_MSTR (1u << 0)
#define BUS_CLEAR_ENABLE (1u << 0)
#define BUS_CLEAR_TERMINATE (1u << 1)
#define BUS_CLEAR_STOP_COND (1u << 2)
#define BUS_CLEAR_THRESHOLD(n) ((uint32_t)(n) << 16)

#define PKT_PROTOCOL_I2C (1u << 4)
#define PKT_CONT_ID(n) ((uint32_t)(n) << 12)
#define PKT_ID(n) ((uint32_t)(n) << 16)
#define HDR_REPEAT_START (1u << 16)
#define HDR_IE_ENABLE (1u << 17)
#define HDR_READ (1u << 19)

/* tegra210_i2c_hw: TLOW 4 and THIGH 2 in every mode, so one SCL period is (4 + 2 + 2) * (non_hs_div + 1) module
 * clocks. */
#define TLOW 4u
#define THIGH 2u
#define DIV_STD_FAST 0x19u
#define DIV_FAST_PLUS 0x10u
#define DIV_HS 1u
#define FAST_MODE_MAX 400000u
#define SRC_DIV_MAX 0xffffu
#define SRC_MUX_PLLP (0u << 30)

#define REG_TIMEOUT_US 100000
#define XFER_TIMEOUT_US 100000
#define BUS_CLEAR_TIMEOUT_US 50000
#define BUS_CLEAR_PULSES 9

struct controller {
  uintptr_t base;
  unsigned gate;
  uint32_t src_reg;
};

static const struct controller controllers[I2C_TEGRA_COUNT] = {
    {0x7000c000, TEGRA_GATE_I2C1, 0x124}, {0x7000c400, TEGRA_GATE_I2C2, 0x198}, {0x7000c500, TEGRA_GATE_I2C3, 0x1b8},
    {0x7000c700, TEGRA_GATE_I2C4, 0x3c4}, {0x7000d000, TEGRA_GATE_I2C5, 0x128}, {0x7000d100, TEGRA_GATE_I2C6, 0x65c},
};

int i2c_tegra_index(uintptr_t base)
{
  for (int i = 0; i < I2C_TEGRA_COUNT; i++)
    if (controllers[i].base == base)
      return i;
  return -1;
}

static uint32_t rd(const struct i2c_tegra *d, uint32_t reg) { return mmio_read32(d->base + reg); }

/* Read back so the write has reached the controller before the next step depends on it. */
static void wr(const struct i2c_tegra *d, uint32_t reg, uint32_t v)
{
  mmio_write32(d->base + reg, v);
  (void)mmio_read32(d->base + reg);
}

static uint32_t scl_clocks(uint32_t non_hs_div) { return (TLOW + THIGH + 2) * (non_hs_div + 1); }

static int poll_clear(const struct i2c_tegra *d, uint32_t reg, uint32_t bits)
{
  uint64_t end = counter_deadline_us(REG_TIMEOUT_US);
  while (rd(d, reg) & bits)
    if (counter_passed(end))
      return -1;
  return 0;
}

static int config_load(const struct i2c_tegra *d)
{
  wr(d, REG_CONFIG_LOAD, CONFIG_LOAD_MSTR);
  return poll_clear(d, REG_CONFIG_LOAD, 0xffffffffu);
}

static int flush_fifos(const struct i2c_tegra *d)
{
  wr(d, REG_FIFO_CONTROL, FIFO_RX_FLUSH | FIFO_TX_FLUSH);
  return poll_clear(d, REG_FIFO_CONTROL, FIFO_RX_FLUSH | FIFO_TX_FLUSH);
}

static int hw_init(const struct i2c_tegra *d)
{
  const struct controller *c = &controllers[d->index];
  tegra_rst_assert(c->gate);
  tegra_clk_source_write(c->src_reg, SRC_MUX_PLLP | d->src_div);
  if (tegra_periph_reset(c->gate))
    return -1;
  wr(d, REG_CNFG, CNFG_NEW_MASTER_FSM | CNFG_PACKET_MODE_EN | CNFG_DEBOUNCE_2);
  wr(d, REG_INT_MASK, 0);
  wr(d, REG_CLK_DIVISOR, d->non_hs_div << 16 | DIV_HS);
  wr(d, REG_INTERFACE_TIMING_0, THIGH << 8 | TLOW);
  wr(d, REG_SL_CNFG, rd(d, REG_SL_CNFG) | SL_CNFG_NACK | SL_CNFG_NEWSL);
  wr(d, REG_SL_ADDR1, SL_ADDR1_DEFAULT);
  wr(d, REG_SL_ADDR2, 0);
  if (flush_fifos(d))
    return -1;
  return config_load(d);
}

/* Clock the bus until a device holding SDA low lets go, then send a STOP. */
static void bus_clear(const struct i2c_tegra *d)
{
  wr(d, REG_BUS_CLEAR_CNFG, BUS_CLEAR_THRESHOLD(BUS_CLEAR_PULSES) | BUS_CLEAR_STOP_COND | BUS_CLEAR_TERMINATE);
  if (config_load(d))
    return;
  wr(d, REG_BUS_CLEAR_CNFG, rd(d, REG_BUS_CLEAR_CNFG) | BUS_CLEAR_ENABLE);
  uint64_t end = counter_deadline_us(BUS_CLEAR_TIMEOUT_US);
  while (!(rd(d, REG_INT_STATUS) & INT_BUS_CLR_DONE) && !counter_passed(end))
    ;
  wr(d, REG_INT_STATUS, rd(d, REG_INT_STATUS));
}

/* After a NACK the controller sends STOP on its own; let it finish before the reset cuts it short. */
static void recover(const struct i2c_tegra *d, int arb_lost)
{
  counter_delay_us(2u * 1000000u / d->bus.speed_hz + 1);
  hw_init(d);
  if (arb_lost)
    bus_clear(d);
}

static int status_error(uint32_t st)
{
  if (st & INT_ARB_LOST)
    return I2C_ERR_BUS;
  if (st & INT_NO_ACK)
    return I2C_ERR_NACK;
  return I2C_OK;
}

/* Wait for a free TX FIFO slot (or, for want_rx, a filled RX one). Returns I2C_OK or the error that stopped it. */
static int wait_fifo(const struct i2c_tegra *d, int want_rx, uint64_t end)
{
  for (;;) {
    uint32_t fs = rd(d, REG_FIFO_STATUS);
    if (want_rx ? FIFO_STATUS_RX(fs) : FIFO_STATUS_TX(fs))
      return I2C_OK;
    int err = status_error(rd(d, REG_INT_STATUS));
    if (err)
      return err;
    if (counter_passed(end))
      return I2C_ERR_TIMEOUT;
  }
}

static int push(const struct i2c_tegra *d, uint32_t word, uint64_t end)
{
  int err = wait_fifo(d, 0, end);
  if (!err)
    mmio_write32(d->base + REG_TX_FIFO, word);
  return err;
}

static uint64_t msg_deadline(const struct i2c_tegra *d, uint16_t len)
{
  /* Nine SCL periods a byte, doubled for clock stretching, on top of a fixed allowance. */
  return counter_deadline_us(XFER_TIMEOUT_US + (uint64_t)len * 9 * 2 * 1000000 / d->bus.speed_hz);
}

static int xfer_msg(struct i2c_tegra *d, const struct i2c_msg *m, int repeat_start)
{
  int rd_msg = (m->flags & I2C_M_RD) != 0;
  uint64_t end = msg_deadline(d, m->len);
  if (flush_fifos(d))
    return I2C_ERR_TIMEOUT;
  wr(d, REG_FIFO_CONTROL, FIFO_TX_TRIG(8) | FIFO_RX_TRIG(1));
  wr(d, REG_INT_STATUS, rd(d, REG_INT_STATUS));

  uint32_t hdr[3] = {PKT_PROTOCOL_I2C | PKT_CONT_ID(d->index) | PKT_ID(1), (uint32_t)m->len - 1,
                     (uint32_t)m->addr << 1 | HDR_IE_ENABLE | (rd_msg ? HDR_READ : 0) |
                         (repeat_start ? HDR_REPEAT_START : 0)};
  int err = I2C_OK;
  for (int i = 0; i < 3 && !err; i++)
    err = push(d, hdr[i], end);

  for (uint16_t at = 0; !err && at < m->len; at += 4) {
    uint16_t n = m->len - at < 4 ? m->len - at : 4;
    if (rd_msg) {
      err = wait_fifo(d, 1, end);
      if (err)
        break;
      uint32_t w = mmio_read32(d->base + REG_RX_FIFO);
      for (uint16_t k = 0; k < n; k++)
        m->buf[at + k] = (uint8_t)(w >> (8 * k));
    } else {
      uint32_t w = 0;
      for (uint16_t k = 0; k < n; k++)
        w |= (uint32_t)m->buf[at + k] << (8 * k);
      err = push(d, w, end);
    }
  }

  while (!err) {
    uint32_t st = rd(d, REG_INT_STATUS);
    err = status_error(st);
    if (!err && (st & INT_PACKET_XFER_COMPLETE))
      break;
    if (!err && counter_passed(end))
      err = I2C_ERR_TIMEOUT;
  }
  uint32_t st = rd(d, REG_INT_STATUS);
  wr(d, REG_INT_STATUS, st);
  if (err)
    recover(d, (st & INT_ARB_LOST) != 0);
  return err;
}

static int tegra_xfer(struct i2c_bus *bus, struct i2c_msg *msgs, int n)
{
  struct i2c_tegra *d = (struct i2c_tegra *)bus;
  for (int i = 0; i < n; i++)
    if (msgs[i].len > ((msgs[i].flags & I2C_M_RD) ? I2C_TEGRA_MAX_READ : I2C_TEGRA_MAX_WRITE))
      return I2C_ERR_INVAL;
  for (int i = 0; i < n; i++) {
    int err = xfer_msg(d, &msgs[i], i + 1 < n);
    if (err)
      return err;
  }
  return I2C_OK;
}

int i2c_tegra_init(struct i2c_tegra *dev, uintptr_t base, uint32_t speed_hz)
{
  int index = i2c_tegra_index(base);
  if (index < 0 || !speed_hz || speed_hz > I2C_TEGRA_MAX_SPEED)
    return -1;
  uint32_t non_hs = speed_hz > FAST_MODE_MAX ? DIV_FAST_PLUS : DIV_STD_FAST;
  uint64_t module_hz = (uint64_t)speed_hz * scl_clocks(non_hs);
  uint64_t div = (TEGRA_PLLP_HZ + module_hz - 1) / module_hz; /* round up so the bus never runs fast */
  if (div - 1 > SRC_DIV_MAX)
    return -1;

  *dev = (struct i2c_tegra){.base = base, .index = index, .src_div = (uint32_t)(div - 1), .non_hs_div = non_hs};
  dev->bus.xfer = tegra_xfer;
  dev->bus.speed_hz = (uint32_t)(TEGRA_PLLP_HZ / div / scl_clocks(non_hs));
  return hw_init(dev) ? -1 : 0;
}
