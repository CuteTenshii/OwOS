/* i2c_mtk_host.h: a fake MT6765 I2C controller and AP-DMA channel with an EEPROM on the bus, in place of mmio.h,
 * cache.h, counter.h and sched.h, for i2c_mtk_test.c. The fake cache is not coherent: DMA reads only what was cleaned
 * and the CPU sees DMA writes only after an invalidate. */
#ifndef OWOS_ARCH_AARCH64_MMIO_H
#define OWOS_ARCH_AARCH64_MMIO_H
#define OWOS_ARCH_AARCH64_CACHE_H
#define OWOS_ARCH_AARCH64_COUNTER_H
#define OWOS_SCHED_H

#include <stddef.h>
#include <stdint.h>

#define FAKE_I2C_BASE 0x11007000u
#define FAKE_DMA_BASE 0x11000080u
#define FAKE_EEPROM_ADDR 0x50
#define FAKE_BUF_LEN 4096

struct fake_i2c {
  uint16_t reg[0x80]; /* controller, by offset / 2 */
  uint32_t dma[0x20]; /* AP-DMA channel, by offset / 4 */
  uint8_t tx_fifo[8], rx_fifo[8];
  int ntx, nrx, rx_pos;
  int dma_armed;
  uint8_t eeprom[256];
  uint8_t eeprom_ptr;
  uint8_t *cpu_tx, *cpu_rx;     /* the driver's DMA buffers, as the CPU sees them */
  uint8_t mem_tx[FAKE_BUF_LEN]; /* the same buffers as memory, which is what DMA sees */
  uint8_t mem_rx[FAKE_BUF_LEN];
  int hang;        /* START never completes */
  int bad_program; /* a START found registers that do not describe the transfer */
  int starts, fifo_xfers, dma_xfers, soft_resets, cleans, invals;
  int restart;  /* the last transfer joined write and read with a repeated start */
  uint64_t now; /* counter ticks at 1 MHz */
};
extern struct fake_i2c i2c_fake;

uint16_t fake_i2c_read16(uintptr_t a);
void fake_i2c_write16(uintptr_t a, uint16_t v);
uint32_t fake_i2c_read32(uintptr_t a);
void fake_i2c_write32(uintptr_t a, uint32_t v);
#define mmio_read16 fake_i2c_read16
#define mmio_write16 fake_i2c_write16
#define mmio_read32 fake_i2c_read32
#define mmio_write32 fake_i2c_write32

void dcache_clean(const void *p, size_t len);
void dcache_inval(void *p, size_t len);
void sched_yield(void);

static inline uint64_t counter_now(void) { return i2c_fake.now += 10; }
static inline uint64_t counter_hz(void) { return 1000000; }
static inline uint64_t counter_deadline_us(uint64_t us) { return counter_now() + us; }
static inline int counter_passed(uint64_t deadline) { return (int64_t)(counter_now() - deadline) >= 0; }

#endif
