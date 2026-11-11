/* max77620.c: register access over the I2C core. Power-off clears SFT_RST_WK before setting SFT_RST, as hekate does:
 * with the wake bit set the same write reboots, and PWR_OFF alone leaves the regulators running. */
#include "max77620.h"

#define ONOFFCNFG1_SFT_RST (1u << 7)
#define ONOFFCNFG2_SFT_RST_WK (1u << 7)

int max77620_read(const struct max77620 *pmic, uint8_t reg, uint8_t *val)
{
  return i2c_reg_read(pmic->bus, pmic->addr, reg, val, 1);
}

int max77620_write(const struct max77620 *pmic, uint8_t reg, uint8_t val)
{
  return i2c_reg_write(pmic->bus, pmic->addr, reg, &val, 1);
}

int max77620_probe(struct max77620 *pmic, struct i2c_bus *bus, uint16_t addr)
{
  *pmic = (struct max77620){.bus = bus, .addr = addr};
  uint8_t cid[2];
  int rc = i2c_reg_read(bus, addr, MAX77620_REG_CID4, cid, 2);
  if (rc)
    return rc;
  pmic->otp = cid[0];
  pmic->es = cid[1];
  return I2C_OK;
}

/* The locked path could yield forever behind a transfer another CPU abandoned, so go to the controller directly. */
static int raw_rmw(const struct max77620 *pmic, uint8_t reg, uint8_t clear, uint8_t set)
{
  uint8_t v;
  struct i2c_msg rd[2] = {{pmic->addr, 0, 1, &reg}, {pmic->addr, I2C_M_RD, 1, &v}};
  int rc = pmic->bus->xfer(pmic->bus, rd, 2);
  if (rc)
    return rc;
  uint8_t out[2] = {reg, (uint8_t)((v & ~clear) | set)};
  struct i2c_msg wr = {pmic->addr, 0, 2, out};
  return pmic->bus->xfer(pmic->bus, &wr, 1);
}

int max77620_poweroff(const struct max77620 *pmic)
{
  if (!pmic->bus)
    return I2C_ERR_INVAL;
  int rc = raw_rmw(pmic, MAX77620_REG_ONOFFCNFG2, ONOFFCNFG2_SFT_RST_WK, 0);
  if (rc)
    return rc;
  return raw_rmw(pmic, MAX77620_REG_ONOFFCNFG1, 0, ONOFFCNFG1_SFT_RST);
}
