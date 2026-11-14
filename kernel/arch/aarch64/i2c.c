/* i2c.c: bus registry, request checks and the per-bus lock; controllers only see transfers they can run. */
#include "i2c.h"
#include "arch.h"
#include "sched.h"

static struct i2c_bus *buses[I2C_MAX_BUSES];

int i2c_register(struct i2c_bus *bus, int id)
{
  if (!bus || !bus->xfer || id < 0 || id >= I2C_MAX_BUSES || buses[id])
    return -1;
  bus->id = id;
  bus->busy = 0;
  buses[id] = bus;
  return 0;
}

struct i2c_bus *i2c_bus_get(int id) { return id >= 0 && id < I2C_MAX_BUSES ? buses[id] : 0; }

int i2c_bus_count(void)
{
  int n = 0;
  for (int i = 0; i < I2C_MAX_BUSES; i++)
    n += buses[i] != 0;
  return n;
}

static int msg_ok(const struct i2c_msg *m) { return m->addr <= 0x7f && m->len && m->buf; }

static int request_ok(const struct i2c_msg *msgs, int n)
{
  if (!msgs || n < 1 || n > 2 || !msg_ok(&msgs[0]))
    return 0;
  if (n == 1)
    return 1;
  return msg_ok(&msgs[1]) && !(msgs[0].flags & I2C_M_RD) && (msgs[1].flags & I2C_M_RD) && msgs[0].addr == msgs[1].addr;
}

int i2c_transfer(struct i2c_bus *bus, struct i2c_msg *msgs, int n)
{
  if (!bus || !request_ok(msgs, n))
    return I2C_ERR_INVAL;
  /* A transfer can take hundreds of milliseconds, too long to hold interrupts off or spin. */
  while (__atomic_exchange_n(&bus->busy, 1, __ATOMIC_ACQUIRE))
    sched_yield();
  int rc = bus->xfer(bus, msgs, n);
  __atomic_store_n(&bus->busy, 0, __ATOMIC_RELEASE);
  return rc;
}

int i2c_write(struct i2c_bus *bus, uint16_t addr, const uint8_t *buf, uint16_t len)
{
  struct i2c_msg m = {addr, 0, len, (uint8_t *)buf};
  return i2c_transfer(bus, &m, 1);
}

int i2c_read(struct i2c_bus *bus, uint16_t addr, uint8_t *buf, uint16_t len)
{
  struct i2c_msg m = {addr, I2C_M_RD, len, buf};
  return i2c_transfer(bus, &m, 1);
}

int i2c_write_read(struct i2c_bus *bus, uint16_t addr, const uint8_t *wbuf, uint16_t wlen, uint8_t *rbuf, uint16_t rlen)
{
  struct i2c_msg m[2] = {{addr, 0, wlen, (uint8_t *)wbuf}, {addr, I2C_M_RD, rlen, rbuf}};
  return i2c_transfer(bus, m, 2);
}

int i2c_reg_read(struct i2c_bus *bus, uint16_t addr, uint8_t reg, uint8_t *buf, uint16_t len)
{
  return i2c_write_read(bus, addr, &reg, 1, buf, len);
}

int i2c_reg_write(struct i2c_bus *bus, uint16_t addr, uint8_t reg, const uint8_t *buf, uint16_t len)
{
  uint8_t out[1 + I2C_REG_WRITE_MAX];
  if (len > I2C_REG_WRITE_MAX || (len && !buf))
    return I2C_ERR_INVAL;
  out[0] = reg;
  for (uint16_t i = 0; i < len; i++)
    out[1 + i] = buf[i];
  return i2c_write(bus, addr, out, (uint16_t)(1 + len));
}
