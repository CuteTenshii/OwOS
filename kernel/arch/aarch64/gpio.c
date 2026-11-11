/* gpio.c: MT6765 pin registers: DIR 0x000, DO 0x100, DI 0x200 hold one bit per pin and MODE 0x300 four, in 0x10 byte
 * strides. */
#include "gpio.h"
#include "mmio.h"
#include "spinlock.h"

#define REG_DIR 0x000
#define REG_DO 0x100
#define REG_DI 0x200
#define REG_MODE 0x300
#define REG_STRIDE 0x10

static uintptr_t base;
static spinlock_t lock;

void gpio_init(uintptr_t iocfg0) { base = iocfg0; }

static int field_get(uint32_t reg, unsigned pin, unsigned width)
{
  if (!base || pin >= GPIO_COUNT)
    return -1;
  unsigned bit = pin * width;
  return (int)((mmio_read32(base + reg + REG_STRIDE * (bit / 32)) >> (bit % 32)) & ((1u << width) - 1));
}

static int field_set(uint32_t reg, unsigned pin, unsigned width, unsigned value)
{
  if (!base || pin >= GPIO_COUNT)
    return -1;
  unsigned bit = pin * width, shift = bit % 32;
  uintptr_t a = base + reg + REG_STRIDE * (bit / 32);
  uint32_t mask = ((1u << width) - 1) << shift;
  uintptr_t f = spin_lock_irqsave(&lock);
  mmio_write32(a, (mmio_read32(a) & ~mask) | ((value << shift) & mask));
  spin_unlock_irqrestore(&lock, f);
  return 0;
}

int gpio_set_mode(unsigned pin, unsigned mode) { return mode > 15 ? -1 : field_set(REG_MODE, pin, 4, mode); }
int gpio_get_mode(unsigned pin) { return field_get(REG_MODE, pin, 4); }
int gpio_set_output(unsigned pin, int output) { return field_set(REG_DIR, pin, 1, output != 0); }
int gpio_write(unsigned pin, int level) { return field_set(REG_DO, pin, 1, level != 0); }
int gpio_read(unsigned pin) { return field_get(REG_DI, pin, 1); }
