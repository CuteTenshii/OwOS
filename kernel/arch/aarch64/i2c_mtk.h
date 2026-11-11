/* i2c_mtk.h: MediaTek MT6765 I2C controller (register layout v2), polled, with AP-DMA for transfers over 8 bytes. */
#ifndef OWOS_ARCH_AARCH64_I2C_MTK_H
#define OWOS_ARCH_AARCH64_I2C_MTK_H

#include "i2c.h"
#include <stdint.h>

#define I2C_MTK_FIFO_LEN 8
#define I2C_MTK_DMA_MAX 4096 /* longest message, and the size of each DMA buffer */
#define I2C_MTK_MAX_SPEED 400000

struct i2c_mtk_config {
  uintptr_t base, dma_base;
  uint32_t src_hz;    /* clock into the block, before clock_div */
  uint32_t clock_div; /* the device tree's clock-div, 1..8 */
  uint32_t speed_hz;  /* requested SCL rate; above I2C_MTK_MAX_SPEED it is lowered to that */
  /* I2C_MTK_DMA_MAX bytes each, identity-mapped below 4 GiB, not sharing a cache line with anything else. */
  uint8_t *tx_buf, *rx_buf;
};

struct i2c_mtk {
  struct i2c_bus bus;
  uintptr_t base, dma_base;
  uint8_t *tx_buf, *rx_buf;
  uint16_t timing, ltiming, clock_div_reg, ext_conf;
};

/* Work out the bus timing and reset the controller. Returns 0, or -1 if the clock or speed cannot be met. The bus
 * is ready for i2c_register(&dev->bus, id). */
int i2c_mtk_init(struct i2c_mtk *dev, const struct i2c_mtk_config *cfg);

#endif
