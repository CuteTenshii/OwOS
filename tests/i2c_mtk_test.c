/* i2c_mtk_test.c: the I2C core (kernel/arch/aarch64/i2c.c) and the MT6765 controller driver (i2c_mtk.c) against
 * the i2c_fake controller, AP-DMA channel and EEPROM in i2c_mtk_host.h. */
#define OWOS_SUITE_TEST
#include "../kernel/arch/aarch64/i2c_mtk.h"
#include "i2c_mtk_host.h"
#include "test.h"
#include <string.h>

#define REG_DATA_PORT 0x00
#define REG_SLAVE_ADDR 0x04
#define REG_INTR_MASK 0x08
#define REG_INTR_STAT 0x0c
#define REG_CONTROL 0x10
#define REG_TRANSFER_LEN 0x14
#define REG_TRANSAC_LEN 0x18
#define REG_TIMING 0x20
#define REG_START 0x24
#define REG_EXT_CONF 0x28
#define REG_LTIMING 0x2c
#define REG_FIFO_ADDR_CLR 0x38
#define REG_TRANSFER_LEN_AUX 0x44
#define REG_CLOCK_DIV 0x48
#define REG_SOFTRESET 0x50
#define DMA_EN 0x08
#define DMA_RST 0x0c
#define DMA_TX_MEM_ADDR 0x1c
#define DMA_RX_MEM_ADDR 0x20
#define DMA_TX_LEN 0x24
#define DMA_RX_LEN 0x28
#define DMA_TX_MEM_ADDR2 0x54
#define DMA_RX_MEM_ADDR2 0x58

#define CONTROL_RS 0x02
#define CONTROL_DMA_EN 0x04
#define CONTROL_DIR_CHANGE 0x10
#define CONTROL_ACKERR_DET_EN 0x20
#define INTR_TRANSAC_COMP 0x01
#define INTR_ACKERR 0x02

struct fake_i2c i2c_fake;
static uint8_t tx_buf[I2C_MTK_DMA_MAX] __attribute__((aligned(64)));
static uint8_t rx_buf[I2C_MTK_DMA_MAX] __attribute__((aligned(64)));
static struct i2c_mtk ctl;

/* The core only yields when a bus is contended, which a single-threaded test never makes it. */
void thread_yield(void) {}

static uint16_t *reg(uint32_t off) { return &i2c_fake.reg[off / 2]; }
static uint32_t *dreg(uint32_t off) { return &i2c_fake.dma[off / 4]; }

static uint8_t *dma_target(uint32_t lo, uint32_t hi)
{
  return (uint8_t *)(uintptr_t)((uint64_t)*dreg(hi) << 32 | *dreg(lo));
}

static void run_transfer(void)
{
  uint16_t ctl_reg = *reg(REG_CONTROL), sa = *reg(REG_SLAVE_ADDR);
  int dma = ctl_reg & CONTROL_DMA_EN, wrrd = ctl_reg & CONTROL_DIR_CHANGE, rd = sa & 1;
  uint16_t len = *reg(REG_TRANSFER_LEN), aux = *reg(REG_TRANSFER_LEN_AUX);
  unsigned wlen = wrrd ? len : rd ? 0 : len, rlen = wrrd ? aux : rd ? len : 0;
  uint8_t out[FAKE_BUF_LEN];

  i2c_fake.restart = wrrd && (ctl_reg & CONTROL_RS);
  if (*reg(REG_TRANSAC_LEN) != (wrrd ? 2 : 1) || (wrrd && rd) || !(ctl_reg & CONTROL_ACKERR_DET_EN) ||
      !(*reg(REG_INTR_MASK) & INTR_TRANSAC_COMP) || (!dma && (wlen > 8 || rlen > 8)))
    i2c_fake.bad_program++;
  if (dma) {
    if (!i2c_fake.dma_armed ||
        (wlen && (dma_target(DMA_TX_MEM_ADDR, DMA_TX_MEM_ADDR2) != i2c_fake.cpu_tx || *dreg(DMA_TX_LEN) != wlen)) ||
        (rlen && (dma_target(DMA_RX_MEM_ADDR, DMA_RX_MEM_ADDR2) != i2c_fake.cpu_rx || *dreg(DMA_RX_LEN) != rlen)))
      i2c_fake.bad_program++;
    memcpy(out, i2c_fake.mem_tx, wlen);
    i2c_fake.dma_xfers++;
  } else {
    if (i2c_fake.ntx != (int)wlen)
      i2c_fake.bad_program++;
    memcpy(out, i2c_fake.tx_fifo, wlen <= 8 ? wlen : 8);
    i2c_fake.fifo_xfers++;
  }
  i2c_fake.dma_armed = 0;
  *dreg(DMA_EN) = 0;

  if ((sa >> 1) != FAKE_EEPROM_ADDR) {
    *reg(REG_INTR_STAT) |= INTR_ACKERR | INTR_TRANSAC_COMP;
    return;
  }
  if (wlen) {
    i2c_fake.eeprom_ptr = out[0];
    for (unsigned i = 1; i < wlen; i++)
      i2c_fake.eeprom[i2c_fake.eeprom_ptr++] = out[i];
  }
  uint8_t *dst = dma ? i2c_fake.mem_rx : i2c_fake.rx_fifo;
  for (unsigned i = 0; i < rlen && (dma || i < 8); i++)
    dst[i] = i2c_fake.eeprom[i2c_fake.eeprom_ptr++];
  if (!dma) {
    i2c_fake.nrx = (int)rlen;
    i2c_fake.rx_pos = 0;
  }
  *reg(REG_INTR_STAT) |= INTR_TRANSAC_COMP;
}

uint16_t fake_i2c_read16(uintptr_t a)
{
  uint32_t off = (uint32_t)(a - FAKE_I2C_BASE);
  if (off == REG_DATA_PORT)
    return i2c_fake.rx_pos < i2c_fake.nrx ? i2c_fake.rx_fifo[i2c_fake.rx_pos++] : 0;
  return *reg(off);
}

void fake_i2c_write16(uintptr_t a, uint16_t v)
{
  uint32_t off = (uint32_t)(a - FAKE_I2C_BASE);
  switch (off) {
  case REG_DATA_PORT:
    if (i2c_fake.ntx == 8)
      i2c_fake.bad_program++;
    else
      i2c_fake.tx_fifo[i2c_fake.ntx++] = (uint8_t)v;
    return;
  case REG_INTR_STAT:
    *reg(off) &= (uint16_t)~v;
    return;
  case REG_FIFO_ADDR_CLR:
    i2c_fake.ntx = i2c_fake.nrx = i2c_fake.rx_pos = 0;
    return;
  case REG_SOFTRESET:
    i2c_fake.soft_resets += v & 1;
    return;
  case REG_START:
    if (!(v & 1))
      return;
    i2c_fake.starts++;
    if (i2c_fake.hang)
      *reg(off) = v;
    else
      run_transfer();
    return;
  }
  *reg(off) = v;
}

uint32_t fake_i2c_read32(uintptr_t a) { return *dreg((uint32_t)(a - FAKE_DMA_BASE)); }

void fake_i2c_write32(uintptr_t a, uint32_t v)
{
  uint32_t off = (uint32_t)(a - FAKE_DMA_BASE);
  if (off == DMA_EN)
    i2c_fake.dma_armed = v & 1;
  *dreg(off) = off == DMA_RST ? v & ~1u : v; /* the warm reset bit clears itself */
}

static int in_buf(const void *p, size_t len, const uint8_t *buf)
{
  return (const uint8_t *)p >= buf && (const uint8_t *)p + len <= buf + FAKE_BUF_LEN;
}

void dcache_clean(const void *p, size_t len)
{
  i2c_fake.cleans++;
  if (in_buf(p, len, i2c_fake.cpu_tx))
    memcpy(i2c_fake.mem_tx + ((const uint8_t *)p - i2c_fake.cpu_tx), p, len);
}

void dcache_inval(void *p, size_t len)
{
  i2c_fake.invals++;
  if (in_buf(p, len, i2c_fake.cpu_rx))
    memcpy(p, i2c_fake.mem_rx + ((uint8_t *)p - i2c_fake.cpu_rx), len);
}

static struct i2c_mtk_config config(uint32_t speed_hz)
{
  return (struct i2c_mtk_config){FAKE_I2C_BASE, FAKE_DMA_BASE, 26000000, 5, speed_hz, tx_buf, rx_buf};
}

static struct i2c_bus *setup(uint32_t speed_hz)
{
  memset(&i2c_fake, 0, sizeof i2c_fake);
  memset(tx_buf, 0, sizeof tx_buf);
  memset(rx_buf, 0, sizeof rx_buf);
  i2c_fake.cpu_tx = tx_buf;
  i2c_fake.cpu_rx = rx_buf;
  for (int i = 0; i < 256; i++)
    i2c_fake.eeprom[i] = (uint8_t)(i ^ 0x5a);
  struct i2c_mtk_config cfg = config(speed_hz);
  expect_eq(i2c_mtk_init(&ctl, &cfg), 0);
  return &ctl.bus;
}

static void check_timing(void)
{
  struct i2c_bus *bus = setup(100000);
  /* 26 MHz / clock-div 5: SCL needs 26 counts per half period, stored as is because sample is 1. */
  expect_eq(*reg(REG_TIMING), 0x001a);
  expect_eq(*reg(REG_LTIMING), 0x001a);
  expect_eq(*reg(REG_CLOCK_DIV), 0x0404);
  expect_eq(bus->speed_hz, 100000);
  expect(i2c_fake.soft_resets >= 1);
  uint8_t v;
  expect_eq(i2c_reg_read(bus, FAKE_EEPROM_ADDR, 0, &v, 1), I2C_OK);
  expect_eq(*reg(REG_EXT_CONF), 0x8001);

  bus = setup(400000);
  expect_eq(*reg(REG_TIMING), 0x0006); /* high phase at 45% duty */
  expect_eq(*reg(REG_LTIMING), 0x0008);
  expect_eq(i2c_reg_read(bus, FAKE_EEPROM_ADDR, 0, &v, 1), I2C_OK);
  expect_eq(*reg(REG_EXT_CONF), 0x1801);

  bus = setup(1000000);
  expect_eq(bus->speed_hz, 400000);
  expect_eq(*reg(REG_TIMING), 0x0006);
  bus = setup(0);
  expect_eq(bus->speed_hz, 100000);
  expect_eq(i2c_fake.bad_program, 0);
}

static void check_bad_config(void)
{
  struct i2c_mtk dev;
  struct i2c_mtk_config cfg = config(100000);
  cfg.clock_div = 0;
  expect_eq(i2c_mtk_init(&dev, &cfg), -1);
  cfg.clock_div = 9;
  expect_eq(i2c_mtk_init(&dev, &cfg), -1);
  cfg = config(100000);
  cfg.src_hz = 0;
  expect_eq(i2c_mtk_init(&dev, &cfg), -1);
  cfg = config(100000);
  cfg.rx_buf = 0;
  expect_eq(i2c_mtk_init(&dev, &cfg), -1);
  cfg = config(100000);
  cfg.dma_base = 0;
  expect_eq(i2c_mtk_init(&dev, &cfg), -1);
  cfg = config(500); /* below what the divider can slow SCL to */
  expect_eq(i2c_mtk_init(&dev, &cfg), -1);
}

static void check_fifo(void)
{
  struct i2c_bus *bus = setup(400000);
  static const uint8_t data[3] = {0xde, 0xad, 0xbe};
  expect_eq(i2c_reg_write(bus, FAKE_EEPROM_ADDR, 0x10, data, 3), I2C_OK);
  expect_eq(memcmp(&i2c_fake.eeprom[0x10], data, 3), 0);
  expect_eq(*reg(REG_SLAVE_ADDR), FAKE_EEPROM_ADDR << 1);
  expect_eq(*reg(REG_TRANSFER_LEN), 4);
  expect_eq(i2c_fake.restart, 0);

  uint8_t got[8] = {0};
  expect_eq(i2c_reg_read(bus, FAKE_EEPROM_ADDR, 0x10, got, 3), I2C_OK);
  expect_eq(memcmp(got, data, 3), 0);
  expect_eq(i2c_fake.restart, 1);
  expect_eq(*reg(REG_TRANSFER_LEN), 1);
  expect_eq(*reg(REG_TRANSFER_LEN_AUX), 3);

  expect_eq(i2c_read(bus, FAKE_EEPROM_ADDR, got, 8), I2C_OK); /* continues from 0x13 */
  expect_eq(*reg(REG_SLAVE_ADDR), FAKE_EEPROM_ADDR << 1 | 1);
  for (int i = 0; i < 8; i++)
    expect_eq(got[i], (0x13 + i) ^ 0x5a);

  expect_eq(i2c_fake.fifo_xfers, 3);
  expect_eq(i2c_fake.dma_xfers, 0);
  expect_eq(i2c_fake.bad_program, 0);
}

static void check_dma(void)
{
  struct i2c_bus *bus = setup(400000);
  uint8_t out[201];
  out[0] = 0x20;
  for (int i = 1; i < 201; i++)
    out[i] = (uint8_t)(i * 7);
  expect_eq(i2c_write(bus, FAKE_EEPROM_ADDR, out, sizeof out), I2C_OK);
  expect_eq(memcmp(&i2c_fake.eeprom[0x20], out + 1, 200), 0);
  expect_eq(i2c_fake.dma_xfers, 1);
  expect(i2c_fake.cleans >= 1);

  uint8_t got[200] = {0};
  expect_eq(i2c_reg_read(bus, FAKE_EEPROM_ADDR, 0x20, got, 200), I2C_OK);
  expect_eq(memcmp(got, out + 1, 200), 0);
  expect_eq(i2c_fake.restart, 1);
  expect_eq(i2c_fake.dma_xfers, 2);
  expect(i2c_fake.invals >= 1);
  expect_eq(*dreg(DMA_TX_MEM_ADDR2), (uint32_t)((uint64_t)(uintptr_t)tx_buf >> 32));

  expect_eq(i2c_read(bus, FAKE_EEPROM_ADDR, got, 9), I2C_OK); /* a plain read just past the FIFO, from 0xe8 on */
  for (int i = 0; i < 9; i++)
    expect_eq(got[i], (0xe8 + i) ^ 0x5a);
  expect_eq(i2c_fake.dma_xfers, 3);
  expect_eq(i2c_fake.bad_program, 0);
}

static void check_mixed_lengths(void)
{
  struct i2c_bus *bus = setup(100000);
  uint8_t got[9];
  expect_eq(i2c_reg_read(bus, FAKE_EEPROM_ADDR, 0x40, got, 9), I2C_OK); /* 1-byte write, 9-byte read */
  for (int i = 0; i < 9; i++)
    expect_eq(got[i], (0x40 + i) ^ 0x5a);
  expect_eq(i2c_fake.dma_xfers, 1);
  expect_eq(*reg(REG_CONTROL) & CONTROL_DMA_EN, CONTROL_DMA_EN);

  expect_eq(i2c_reg_read(bus, FAKE_EEPROM_ADDR, 0x40, got, 8), I2C_OK);
  expect_eq(i2c_fake.fifo_xfers, 1);
  expect_eq(*reg(REG_CONTROL) & CONTROL_DMA_EN, 0);
  expect_eq(i2c_fake.bad_program, 0);
}

static void check_nack(void)
{
  struct i2c_bus *bus = setup(100000);
  int resets = i2c_fake.soft_resets;
  uint8_t v = 0x77;
  expect_eq(i2c_reg_read(bus, 0x51, 0, &v, 1), I2C_ERR_NACK);
  expect_eq(v, 0x77);
  expect_eq(i2c_fake.soft_resets, resets + 1);
  expect_eq(*reg(REG_INTR_STAT), 0);
  expect_eq(i2c_reg_read(bus, FAKE_EEPROM_ADDR, 1, &v, 1), I2C_OK); /* the bus recovered */
  expect_eq(v, 1 ^ 0x5a);
  expect_eq(i2c_fake.bad_program, 0);
}

static void check_timeout(void)
{
  struct i2c_bus *bus = setup(100000);
  int resets = i2c_fake.soft_resets;
  uint8_t v;
  i2c_fake.hang = 1;
  expect_eq(i2c_reg_read(bus, FAKE_EEPROM_ADDR, 0, &v, 1), I2C_ERR_TIMEOUT);
  expect_eq(i2c_fake.soft_resets, resets + 1);
  i2c_fake.hang = 0;
  expect_eq(i2c_reg_read(bus, FAKE_EEPROM_ADDR, 2, &v, 1), I2C_OK);
  expect_eq(v, 2 ^ 0x5a);
  expect_eq(bus->busy, 0);
}

static void check_requests_refused(void)
{
  struct i2c_bus *bus = setup(100000);
  static uint8_t big[I2C_MTK_DMA_MAX + 1];
  uint8_t b[2] = {0};
  expect_eq(i2c_write(bus, FAKE_EEPROM_ADDR, big, sizeof big), I2C_ERR_INVAL);
  expect_eq(i2c_write(bus, FAKE_EEPROM_ADDR, b, 0), I2C_ERR_INVAL);
  expect_eq(i2c_write(bus, 0x80, b, 1), I2C_ERR_INVAL);
  expect_eq(i2c_write(bus, FAKE_EEPROM_ADDR, 0, 1), I2C_ERR_INVAL);
  expect_eq(i2c_reg_write(bus, FAKE_EEPROM_ADDR, 0, big, I2C_REG_WRITE_MAX + 1), I2C_ERR_INVAL);
  struct i2c_msg two_writes[2] = {{FAKE_EEPROM_ADDR, 0, 1, b}, {FAKE_EEPROM_ADDR, 0, 1, b}};
  expect_eq(i2c_transfer(bus, two_writes, 2), I2C_ERR_INVAL);
  struct i2c_msg other_addr[2] = {{FAKE_EEPROM_ADDR, 0, 1, b}, {0x51, I2C_M_RD, 1, b}};
  expect_eq(i2c_transfer(bus, other_addr, 2), I2C_ERR_INVAL);
  struct i2c_msg read_first[2] = {{FAKE_EEPROM_ADDR, I2C_M_RD, 1, b}, {FAKE_EEPROM_ADDR, 0, 1, b}};
  expect_eq(i2c_transfer(bus, read_first, 2), I2C_ERR_INVAL);
  expect_eq(i2c_transfer(bus, two_writes, 3), I2C_ERR_INVAL);
  expect_eq(i2c_transfer(0, two_writes, 1), I2C_ERR_INVAL);
  expect_eq(i2c_fake.starts, 0);
}

static void check_registry(void)
{
  struct i2c_bus *bus = setup(100000);
  expect_eq(i2c_bus_count(), 0);
  expect_eq(i2c_register(bus, 3), 0);
  expect_eq(bus->id, 3);
  expect(i2c_bus_get(3) == bus);
  expect(i2c_bus_get(0) == 0);
  expect(i2c_bus_get(-1) == 0);
  expect(i2c_bus_get(I2C_MAX_BUSES) == 0);
  expect_eq(i2c_register(bus, 3), -1);
  expect_eq(i2c_register(bus, I2C_MAX_BUSES), -1);
  struct i2c_bus no_xfer = {0};
  expect_eq(i2c_register(&no_xfer, 4), -1);
  expect_eq(i2c_bus_count(), 1);
}

int main(void)
{
  test("bus timing from the source clock and speed", check_timing);
  test("unusable clock settings refused", check_bad_config);
  test("transfers up to 8 bytes go through the FIFO", check_fifo);
  test("longer transfers go through AP-DMA with cache maintenance", check_dma);
  test("one long half of a write-then-read moves both to DMA", check_mixed_lengths);
  test("missing device reports NACK and the controller recovers", check_nack);
  test("hung transfer times out and the controller recovers", check_timeout);
  test("requests the bus cannot express are refused", check_requests_refused);
  test("bus registry", check_registry);
  return 0;
}
