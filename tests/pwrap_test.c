/* pwrap_test.c: PMIC register access (kernel/arch/aarch64/pwrap.c) against a pwrap_fake MT6765 PMIC wrapper. */
#define OWOS_SUITE_TEST
#include "../kernel/arch/aarch64/pwrap.h"
#include "pwrap_host.h"
#include "test.h"
#include <string.h>

struct fake_pwrap pwrap_fake;

static void reset(int mt6357, int mt6358)
{
  memset(&pwrap_fake, 0, sizeof pwrap_fake);
  pwrap_fake.init_done2 = 1;
  pwrap_fake.init_done0 = 1;
  if (mt6357)
    pwrap_fake.pmic[0x040c / 2] = 0x5aa5;
  if (mt6358)
    pwrap_fake.pmic[0x040e / 2] = 0x5aa5;
}

static void check_init(void)
{
  reset(1, 0);
  pwrap_fake.init_done2 = 0;
  expect_eq(pwrap_init(FAKE_PWRAP_BASE), -1);
  uint16_t v;
  expect_eq(pmic_read(0x040c, &v), -1); /* no access without a successful init */
  pwrap_fake.init_done2 = 1;
  pwrap_fake.init_done0 = 0;
  expect_eq(pwrap_init(FAKE_PWRAP_BASE), -1);
  pwrap_fake.init_done0 = 1;
  expect_eq(pwrap_init(FAKE_PWRAP_BASE), 0);
}

static void check_read_write(void)
{
  reset(1, 0);
  expect_eq(pwrap_init(FAKE_PWRAP_BASE), 0);
  uint16_t v = 0;

  expect_eq(pmic_write(0x1234, 0xbeef), 0);
  expect_eq(pwrap_fake.last_cmd, 0x80000000u | (0x91au << 16) | 0xbeef);
  expect_eq(pwrap_fake.pmic[0x1234 / 2], 0xbeef);

  expect_eq(pmic_read(0x1234, &v), 0);
  expect_eq(pwrap_fake.last_cmd, 0x91au << 16);
  expect_eq(v, 0xbeef);
  expect_eq(pwrap_fake.fsm, 0); /* result collected, VLDCLR given */

  expect_eq(pmic_write(0xfffe, 0x0001), 0);
  expect_eq(pmic_read(0xfffe, &v), 0);
  expect_eq(v, 1);
}

static void check_odd_register(void)
{
  reset(1, 0);
  expect_eq(pwrap_init(FAKE_PWRAP_BASE), 0);
  uint16_t v;
  int cmds = pwrap_fake.cmds;
  expect_eq(pmic_read(0x0401, &v), -1);
  expect_eq(pmic_write(0x0401, 1), -1);
  expect_eq(pwrap_fake.cmds, cmds);
}

static void check_timeouts(void)
{
  reset(1, 0);
  expect_eq(pwrap_init(FAKE_PWRAP_BASE), 0);
  uint16_t v = 0x1111;
  pwrap_fake.stuck_busy = 1;
  expect_eq(pmic_read(0x040c, &v), -1);
  expect_eq(v, 0x1111);
  expect_eq(pmic_write(0x040c, 1), -1); /* the bus is still busy */
}

static void check_stale_vldclr(void)
{
  reset(1, 0);
  expect_eq(pwrap_init(FAKE_PWRAP_BASE), 0);
  uint16_t v = 0;
  pwrap_fake.fsm = 6;
  expect_eq(pmic_read(0x040c, &v), -1); /* the access that finds it stuck fails but releases it */
  expect_eq(pwrap_fake.vldclr_writes, 1);
  expect_eq(pmic_read(0x040c, &v), 0);
  expect_eq(v, 0x5aa5);
}

static void check_probe(void)
{
  reset(1, 0);
  expect_eq(pwrap_init(FAKE_PWRAP_BASE), 0);
  expect_eq(strcmp(pmic_probe(), "mt6357"), 0);
  reset(0, 1);
  expect_eq(strcmp(pmic_probe(), "mt6358"), 0);
  reset(0, 0);
  expect_eq(pmic_probe() == 0, 1);
}

int main(void)
{
  test("init requires a bootloader-initialised wrapper", check_init);
  test("register read and write encoding", check_read_write);
  test("odd register addresses are refused", check_odd_register);
  test("stuck bus times out", check_timeouts);
  test("stale WFVLDCLR state is recovered", check_stale_vldclr);
  test("PMIC identified by its read-test register", check_probe);
  return 0;
}
