/* i2c.h: I2C bus registry and transfers. Controller drivers register a bus; device drivers address it by number. */
#ifndef OWOS_ARCH_AARCH64_I2C_H
#define OWOS_ARCH_AARCH64_I2C_H

#include <stdint.h>

#define I2C_MAX_BUSES 8
#define I2C_M_RD 0x0001u /* message reads from the device; otherwise it writes */
#define I2C_REG_WRITE_MAX 32

enum {
  I2C_OK = 0,
  I2C_ERR_INVAL = -1,   /* request the bus or controller cannot express */
  I2C_ERR_NACK = -2,    /* no device acknowledged the address, or it refused a byte */
  I2C_ERR_TIMEOUT = -3, /* the controller never finished; it has been reset */
  I2C_ERR_BUS = -4,     /* arbitration lost or a bus error; the controller has been reset */
};

/* addr is the 7-bit device address. len must be at least 1. */
struct i2c_msg {
  uint16_t addr;
  uint16_t flags;
  uint16_t len;
  uint8_t *buf;
};

struct i2c_bus {
  /* Run one write, one read, or a write then a read to the same address joined by a repeated start. */
  int (*xfer)(struct i2c_bus *bus, struct i2c_msg *msgs, int n);
  uint32_t speed_hz; /* SCL rate the controller was set up for */
  int id;            /* set by i2c_register */
  volatile uint32_t busy;
};

/* Make bus number `id` available. Returns -1 if id is out of range or taken. Call before other CPUs use I2C. */
int i2c_register(struct i2c_bus *bus, int id);
/* NULL if no bus has that number. */
struct i2c_bus *i2c_bus_get(int id);
int i2c_bus_count(void);

/* Thread context only: a busy bus makes the caller yield until it is free. Return I2C_OK or an I2C_ERR_ code. */
int i2c_transfer(struct i2c_bus *bus, struct i2c_msg *msgs, int n);
int i2c_write(struct i2c_bus *bus, uint16_t addr, const uint8_t *buf, uint16_t len);
int i2c_read(struct i2c_bus *bus, uint16_t addr, uint8_t *buf, uint16_t len);
int i2c_write_read(struct i2c_bus *bus, uint16_t addr, const uint8_t *wbuf, uint16_t wlen, uint8_t *rbuf,
                   uint16_t rlen);

/* Devices with 8-bit register numbers: read len bytes from reg on, or write len bytes (at most I2C_REG_WRITE_MAX)
 * starting at reg. */
int i2c_reg_read(struct i2c_bus *bus, uint16_t addr, uint8_t reg, uint8_t *buf, uint16_t len);
int i2c_reg_write(struct i2c_bus *bus, uint16_t addr, uint8_t reg, const uint8_t *buf, uint16_t len);

#endif
