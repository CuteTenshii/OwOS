/* gpio_test.c: MT6765 GPIO register layout (kernel/arch/aarch64/gpio.c) against plain memory standing in for the pin
 * block. */
#define OWOS_SUITE_TEST
#include "../kernel/arch/aarch64/gpio.h"
#include "gpio_host.h"
#include "test.h"
#include <string.h>

struct fake_gpio gpio_fake;

static void reset(void)
{
  memset(&gpio_fake, 0, sizeof gpio_fake);
  gpio_init(FAKE_GPIO_BASE);
}

static uint32_t reg(uint32_t off) { return gpio_fake.mem[off / 4]; }

static void check_mode(void)
{
  reset();
  expect_eq(gpio_set_mode(0, 1), 0);
  expect_eq(reg(0x300), 0x1);
  expect_eq(gpio_set_mode(9, 5), 0); /* eight pins per register: pin 9 is bits 7:4 of the second */
  expect_eq(reg(0x310), 0x5u << 4);
  expect_eq(gpio_set_mode(202, 7), 0); /* 202 / 8 = 25 registers on, 2 * 4 bits in */
  expect_eq(reg(0x300 + 0x10 * 25), 0x7u << 8);
  expect_eq(gpio_get_mode(9), 5);
  expect_eq(gpio_set_mode(9, 2), 0);
  expect_eq(reg(0x310), 0x2u << 4); /* the old value is replaced, not or'ed */
  expect_eq(gpio_set_mode(3, 16), -1);
}

static void check_dir_out_in(void)
{
  reset();
  expect_eq(gpio_set_output(33, 1), 0); /* 32 pins per register: 33 is bit 1 of the second */
  expect_eq(reg(0x010), 1u << 1);
  expect_eq(gpio_write(33, 1), 0);
  expect_eq(reg(0x110), 1u << 1);
  expect_eq(gpio_write(33, 0), 0);
  expect_eq(reg(0x110), 0);
  expect_eq(gpio_set_output(33, 0), 0);
  expect_eq(reg(0x010), 0);

  gpio_fake.mem[0x200 / 4] = 1u << 31;
  gpio_fake.mem[0x210 / 4] = 1u << 7;
  expect_eq(gpio_read(31), 1);
  expect_eq(gpio_read(30), 0);
  expect_eq(gpio_read(39), 1);
}

static void check_neighbours_untouched(void)
{
  reset();
  memset(gpio_fake.mem, 0xff, sizeof gpio_fake.mem);
  expect_eq(gpio_write(5, 0), 0);
  expect_eq(reg(0x100), 0xffffffffu & ~(1u << 5));
  expect_eq(reg(0x110), 0xffffffffu);
  expect_eq(gpio_set_mode(1, 0), 0);
  expect_eq(reg(0x300), 0xffffff0fu);
}

static void check_bounds(void)
{
  reset();
  expect_eq(gpio_read(GPIO_COUNT), -1);
  expect_eq(gpio_write(GPIO_COUNT, 1), -1);
  expect_eq(gpio_set_mode(GPIO_COUNT, 0), -1);
  expect_eq(gpio_get_mode(1000), -1);
  expect_eq(gpio_set_output(GPIO_COUNT + 5, 1), -1);
  expect_eq(gpio_read(GPIO_COUNT - 1) >= 0, 1);
  gpio_init(0);
  expect_eq(gpio_read(0), -1); /* no block discovered */
}

int main(void)
{
  test("mode field", check_mode);
  test("direction, output and input bits", check_dir_out_in);
  test("read-modify-write leaves neighbours alone", check_neighbours_untouched);
  test("pin range and missing block", check_bounds);
  return 0;
}
