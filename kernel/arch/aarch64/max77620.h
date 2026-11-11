/* max77620.h: Maxim MAX77620 PMIC on an I2C bus (I2C5, address 0x3c on the Nintendo Switch): register access,
 * identification and power-off. Regulators are left as the loader set them. */
#ifndef OWOS_ARCH_AARCH64_MAX77620_H
#define OWOS_ARCH_AARCH64_MAX77620_H

#include "i2c.h"
#include <stdint.h>

#define MAX77620_REG_ONOFFCNFG1 0x41
#define MAX77620_REG_ONOFFCNFG2 0x42
#define MAX77620_REG_CID4 0x5c /* OTP version */
#define MAX77620_REG_CID5 0x5d /* ES version: DIDM in bits 7:4, DIDO in 3:0 */

struct max77620 {
  struct i2c_bus *bus;
  uint16_t addr;
  uint8_t otp, es; /* CID4 and CID5 as read at probe */
};

/* Read the chip ID registers. Returns I2C_OK, or the I2C error when nothing answers at addr. */
int max77620_probe(struct max77620 *pmic, struct i2c_bus *bus, uint16_t addr);

/* Thread context only, as for i2c_transfer. Return I2C_OK or an I2C_ERR_ code. */
int max77620_read(const struct max77620 *pmic, uint8_t reg, uint8_t *val);
int max77620_write(const struct max77620 *pmic, uint8_t reg, uint8_t val);

/* Cut power with a software reset that is not allowed to wake the system again. Bypasses the bus lock: for the last
 * moments of the system, when nothing else will use the bus. Returns only if the PMIC could not be reached. */
int max77620_poweroff(const struct max77620 *pmic);

#endif
