/* tegra_host.h: a fake Tegra X1 for tegra_test.c, in place of mmio.h and counter.h: the CAR with its SET/CLR
 * semantics, the GPIO block with its masked writes, and packet-mode I2C controllers with an EEPROM (0x50) and a
 * MAX77620 (0x3c) on every bus. Touching an I2C controller whose clock is off or that is held in reset counts as a
 * dead access, which on hardware hangs the bus. */
#ifndef OWOS_ARCH_AARCH64_MMIO_H
#define OWOS_ARCH_AARCH64_MMIO_H
#define OWOS_ARCH_AARCH64_COUNTER_H

#include <stdint.h>

#define FAKE_CAR 0x60006000u
#define FAKE_GPIO 0x6000d000u
#define FAKE_I2C1 0x7000c000u
#define FAKE_I2C5 0x7000d000u
#define FAKE_EEPROM 0x50
#define FAKE_PMIC 0x3c
#define FAKE_LOG 64

struct fake_tegra_i2c {
  uint32_t reg[0x100 / 4];
  uint32_t hdr[3];
  int nhdr;              /* header words received for the packet in flight */
  uint32_t payload_left; /* bytes still expected from the TX FIFO */
  uint8_t wbuf[4096];
  uint32_t wlen;
  uint32_t rx[1024];
  int rx_head, rx_tail;
  int packets, repeat_starts, bad_headers, bus_clears;
  uint32_t last_hdr[3];
};

struct fake_tegra {
  uint32_t car[0x1000 / 4];
  struct {
    uint32_t addr, val;
  } car_log[FAKE_LOG];
  int ncar_log;
  uint32_t gpio[0x1000 / 4];
  struct fake_tegra_i2c i2c[6];
  uint8_t eeprom[256], eeprom_ptr;
  uint8_t pmic[256], pmic_ptr;
  int nack_all, hang, arb_lost; /* fault injection for the next packet */
  int dead_access;
  uint64_t now; /* 1 MHz counter */
};
extern struct fake_tegra tegra_fake;

uint32_t fake_tegra_read32(uintptr_t a);
void fake_tegra_write32(uintptr_t a, uint32_t v);
#define mmio_read32 fake_tegra_read32
#define mmio_write32 fake_tegra_write32

static inline uint64_t counter_now(void) { return tegra_fake.now += 10; }
static inline uint64_t counter_hz(void) { return 1000000; }
static inline uint64_t counter_deadline_us(uint64_t us) { return counter_now() + us; }
static inline int counter_passed(uint64_t deadline) { return (int64_t)(counter_now() - deadline) >= 0; }
static inline void counter_delay_us(uint64_t us) { tegra_fake.now += us; }

#endif
