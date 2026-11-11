/* i2c_mtk.c: MT6765 I2C controller. Register use and the timing search follow MediaTek's Linux i2c-mtk driver for
 * this SoC (ver 2 layout, cnt_constraint, separate low timing, 0x1801 fast-mode start). */
#include "i2c_mtk.h"
#include "cache.h"
#include "counter.h"
#include "mmio.h"

#define REG_DATA_PORT 0x00
#define REG_SLAVE_ADDR 0x04
#define REG_INTR_MASK 0x08
#define REG_INTR_STAT 0x0c
#define REG_CONTROL 0x10
#define REG_TRANSFER_LEN 0x14
#define REG_TRANSAC_LEN 0x18
#define REG_DELAY_LEN 0x1c
#define REG_TIMING 0x20
#define REG_START 0x24
#define REG_EXT_CONF 0x28
#define REG_LTIMING 0x2c
#define REG_HS 0x30
#define REG_IO_CONFIG 0x34
#define REG_FIFO_ADDR_CLR 0x38
#define REG_MCU_INTR 0x40
#define REG_TRANSFER_LEN_AUX 0x44
#define REG_CLOCK_DIV 0x48
#define REG_SOFTRESET 0x50

#define DMA_INT_FLAG 0x00
#define DMA_EN 0x08
#define DMA_RST 0x0c
#define DMA_CON 0x18
#define DMA_TX_MEM_ADDR 0x1c
#define DMA_RX_MEM_ADDR 0x20
#define DMA_TX_LEN 0x24
#define DMA_RX_LEN 0x28
#define DMA_TX_MEM_ADDR2 0x54
#define DMA_RX_MEM_ADDR2 0x58

#define INTR_TRANSAC_COMP (1u << 0)
#define INTR_ACKERR (1u << 1)
#define INTR_HS_NACKERR (1u << 2)
#define INTR_ARB_LOST (1u << 3)
#define INTR_SCL_TIMEOUT (1u << 5)
#define INTR_BUS_ERR (1u << 8)
#define INTR_ALL 0x01ffu
#define INTR_WAIT (INTR_TRANSAC_COMP | INTR_ACKERR | INTR_HS_NACKERR | INTR_ARB_LOST | INTR_SCL_TIMEOUT | INTR_BUS_ERR)

#define CONTROL_RS (1u << 1)
#define CONTROL_DMA_EN (1u << 2)
#define CONTROL_CLK_EXT_EN (1u << 3)
#define CONTROL_DIR_CHANGE (1u << 4)
#define CONTROL_ACKERR_DET_EN (1u << 5)
#define CONTROL_DMAACK_EN (1u << 8)
#define CONTROL_ASYNC_MODE (1u << 9)

#define TRANSAC_START 1u
#define SOFT_RST 1u
#define FIFO_ADDR_CLR 1u
#define MCU_INTR_EN 1u
#define IO_CONFIG_OPEN_DRAIN 3u
#define DELAY_LEN 0x0au
#define ST_START_CON 0x8001u
#define FS_START_CON_MT6765 0x1801u

#define DMA_CON_TX 0u
#define DMA_CON_RX 1u
#define DMA_START_EN 1u
#define DMA_WARM_RST 1u

#define STANDARD_SPEED 100000
#define MAX_CLOCK_DIV 8
#define MAX_SAMPLE_CNT 8
#define MAX_STEP_CNT 64
#define MAX_HS_STEP_CNT 8
#define DMA_RESET_US 100

static void w16(const struct i2c_mtk *d, uint32_t reg, uint16_t v) { mmio_write16(d->base + reg, v); }
static uint16_t r16(const struct i2c_mtk *d, uint32_t reg) { return mmio_read16(d->base + reg); }
static void dma_w32(const struct i2c_mtk *d, uint32_t reg, uint32_t v) { mmio_write32(d->dma_base + reg, v); }
static uint32_t dma_r32(const struct i2c_mtk *d, uint32_t reg) { return mmio_read32(d->dma_base + reg); }

/* Smallest sample * step divider whose SCL rate does not exceed speed_hz; hardware counts are stored one less, except
 * that MT6765 keeps step as is when sample is 1. */
static int calc_speed(uint32_t src_hz, uint32_t speed_hz, unsigned *step_out, unsigned *sample_out)
{
  unsigned khz = speed_hz / 1000, hclk = src_hz / 1000;
  if (!khz || !hclk)
    return -1;
  unsigned max_step = speed_hz > I2C_MTK_MAX_SPEED ? MAX_HS_STEP_CNT : MAX_STEP_CNT;
  unsigned min_div = ((hclk >> 1) + khz - 1) / khz;
  unsigned best_mul = MAX_SAMPLE_CNT * max_step, sample = MAX_SAMPLE_CNT, step = max_step;
  for (unsigned s = 1; s <= MAX_SAMPLE_CNT; s++) {
    unsigned st = (min_div + s - 1) / s;
    if (st > max_step || st * s >= best_mul)
      continue;
    best_mul = st * s;
    sample = s;
    step = st;
    if (best_mul == min_div)
      break;
  }
  if (hclk / (2 * sample * step) > khz)
    return -1;
  if (--sample)
    step--;
  *step_out = step;
  *sample_out = sample;
  return 0;
}

/* Fast mode stretches the low phase to 55% of the period, as the vendor driver does. */
static int set_speed(struct i2c_mtk *d, uint32_t src_hz, uint32_t speed_hz)
{
  unsigned duty = speed_hz > STANDARD_SPEED ? 45 : 50, step, sample;
  if (calc_speed(src_hz, speed_hz * 50 / duty, &step, &sample))
    return -1;
  d->timing = (uint16_t)((sample & 7u) << 8 | (step & 0x3fu));
  if (calc_speed(src_hz, speed_hz * 50 / (100 - duty), &step, &sample))
    return -1;
  d->ltiming = (uint16_t)((sample & 7u) << 6 | (step & 0x3fu));
  d->ext_conf = speed_hz > STANDARD_SPEED ? FS_START_CON_MT6765 : ST_START_CON;
  return 0;
}

static void write_timing(const struct i2c_mtk *d)
{
  w16(d, REG_IO_CONFIG, IO_CONFIG_OPEN_DRAIN);
  w16(d, REG_TIMING, d->timing);
  w16(d, REG_LTIMING, d->ltiming);
  w16(d, REG_HS, 0);
  w16(d, REG_CLOCK_DIV, d->clock_div_reg);
}

static void reset_hw(const struct i2c_mtk *d)
{
  w16(d, REG_INTR_MASK, 0);
  w16(d, REG_INTR_STAT, r16(d, REG_INTR_STAT));
  w16(d, REG_FIFO_ADDR_CLR, FIFO_ADDR_CLR);
  w16(d, REG_SOFTRESET, SOFT_RST);
  write_timing(d);
  dma_w32(d, DMA_RST, DMA_WARM_RST);
  uint64_t end = counter_deadline_us(DMA_RESET_US);
  while (((dma_r32(d, DMA_RST) & DMA_WARM_RST) || dma_r32(d, DMA_EN)) && !counter_passed(end))
    ;
  dma_w32(d, DMA_RST, 0);
}

static void dma_addr(const struct i2c_mtk *d, uint32_t lo_reg, uint32_t hi_reg, const uint8_t *buf)
{
  uint64_t a = (uintptr_t)buf; /* identity map: the bus address is the pointer */
  dma_w32(d, lo_reg, (uint32_t)a);
  dma_w32(d, hi_reg, (uint32_t)(a >> 32));
}

static void dma_start(const struct i2c_mtk *d, const struct i2c_msg *w, const struct i2c_msg *r)
{
  dma_w32(d, DMA_INT_FLAG, 0);
  dma_w32(d, DMA_CON, w ? DMA_CON_TX : DMA_CON_RX); /* write-then-read starts as TX */
  if (w) {
    for (uint16_t i = 0; i < w->len; i++)
      d->tx_buf[i] = w->buf[i];
    dcache_clean(d->tx_buf, w->len);
    dma_addr(d, DMA_TX_MEM_ADDR, DMA_TX_MEM_ADDR2, d->tx_buf);
    dma_w32(d, DMA_TX_LEN, w->len);
  }
  if (r) {
    dcache_inval(d->rx_buf, r->len);
    dma_addr(d, DMA_RX_MEM_ADDR, DMA_RX_MEM_ADDR2, d->rx_buf);
    dma_w32(d, DMA_RX_LEN, r->len);
  }
  dma_w32(d, DMA_EN, DMA_START_EN);
}

/* Twice the time the bytes take on the wire, plus slack for clock stretching. */
static uint64_t timeout_us(const struct i2c_mtk *d, const struct i2c_msg *w, const struct i2c_msg *r)
{
  uint64_t bytes = (w ? w->len + 1u : 0) + (r ? r->len + 1u : 0);
  return 2 * bytes * 9 * 1000000 / d->bus.speed_hz + 20000;
}

static uint16_t wait_done(const struct i2c_mtk *d, uint64_t us)
{
  uint64_t end = counter_deadline_us(us);
  for (;;) {
    uint16_t stat = r16(d, REG_INTR_STAT) & INTR_WAIT;
    if (stat)
      return stat;
    if (counter_passed(end))
      return 0;
  }
}

static int mtk_xfer(struct i2c_bus *bus, struct i2c_msg *msgs, int n)
{
  struct i2c_mtk *d = (struct i2c_mtk *)bus;
  const struct i2c_msg *w = 0, *r = 0;
  if (n == 2) {
    w = &msgs[0];
    r = &msgs[1];
  } else if (msgs[0].flags & I2C_M_RD) {
    r = &msgs[0];
  } else {
    w = &msgs[0];
  }
  if ((w && w->len > I2C_MTK_DMA_MAX) || (r && r->len > I2C_MTK_DMA_MAX))
    return I2C_ERR_INVAL;
  int dma = (w && w->len > I2C_MTK_FIFO_LEN) || (r && r->len > I2C_MTK_FIFO_LEN);

  uint16_t control = CONTROL_ACKERR_DET_EN | CONTROL_CLK_EXT_EN;
  if (dma)
    control |= CONTROL_DMA_EN | CONTROL_DMAACK_EN | CONTROL_ASYNC_MODE;
  if (w && r)
    control |= CONTROL_DIR_CHANGE | CONTROL_RS;
  w16(d, REG_CONTROL, control);
  w16(d, REG_EXT_CONF, d->ext_conf);
  w16(d, REG_DELAY_LEN, DELAY_LEN);
  write_timing(d);
  w16(d, REG_SLAVE_ADDR, (uint16_t)((unsigned)msgs[0].addr << 1 | (w ? 0u : 1u)));
  w16(d, REG_INTR_STAT, INTR_ALL);
  w16(d, REG_FIFO_ADDR_CLR, FIFO_ADDR_CLR);
  w16(d, REG_INTR_MASK, INTR_WAIT);
  if (w && r) {
    w16(d, REG_TRANSFER_LEN, w->len);
    w16(d, REG_TRANSFER_LEN_AUX, r->len);
    w16(d, REG_TRANSAC_LEN, 2);
  } else {
    w16(d, REG_TRANSFER_LEN, msgs[0].len);
    w16(d, REG_TRANSAC_LEN, 1);
  }

  if (dma)
    dma_start(d, w, r);
  else if (w)
    for (uint16_t i = 0; i < w->len; i++)
      w16(d, REG_DATA_PORT, w->buf[i]);
  w16(d, REG_MCU_INTR, MCU_INTR_EN);
  w16(d, REG_START, TRANSAC_START);

  uint16_t stat = wait_done(d, timeout_us(d, w, r));
  w16(d, REG_INTR_MASK, 0);
  w16(d, REG_INTR_STAT, stat);
  if (stat != INTR_TRANSAC_COMP) {
    reset_hw(d);
    if (!stat)
      return I2C_ERR_TIMEOUT;
    return stat & (INTR_ACKERR | INTR_HS_NACKERR) ? I2C_ERR_NACK : I2C_ERR_BUS;
  }

  if (r && dma) {
    dcache_inval(d->rx_buf, r->len);
    for (uint16_t i = 0; i < r->len; i++)
      r->buf[i] = d->rx_buf[i];
  } else if (r) {
    for (uint16_t i = 0; i < r->len; i++)
      r->buf[i] = (uint8_t)r16(d, REG_DATA_PORT);
  }
  return I2C_OK;
}

int i2c_mtk_init(struct i2c_mtk *d, const struct i2c_mtk_config *cfg)
{
  if (!cfg->base || !cfg->dma_base || !cfg->tx_buf || !cfg->rx_buf || !cfg->clock_div ||
      cfg->clock_div > MAX_CLOCK_DIV || !cfg->src_hz)
    return -1;
  uint32_t speed = cfg->speed_hz ? cfg->speed_hz : STANDARD_SPEED;
  if (speed > I2C_MTK_MAX_SPEED)
    speed = I2C_MTK_MAX_SPEED;

  *d = (struct i2c_mtk){0};
  d->base = cfg->base;
  d->dma_base = cfg->dma_base;
  d->tx_buf = cfg->tx_buf;
  d->rx_buf = cfg->rx_buf;
  d->clock_div_reg = (uint16_t)((cfg->clock_div - 1u) << 8 | (cfg->clock_div - 1u));
  if (set_speed(d, cfg->src_hz / cfg->clock_div, speed))
    return -1;
  d->bus.xfer = mtk_xfer;
  d->bus.speed_hz = speed;
  reset_hw(d);
  return 0;
}
