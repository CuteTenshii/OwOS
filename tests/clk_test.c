/* clk_test.c: MT6765 clock gates and muxes (kernel/arch/aarch64/clk.c) against a write-logging fake register block. */
#define OWOS_SUITE_TEST
#include "../kernel/arch/aarch64/clk.h"
#include "clk_host.h"
#include "test.h"
#include <string.h>

struct fake_clk clk_fake;

static void reset(void)
{
  memset(&clk_fake, 0, sizeof clk_fake);
  clk_init(FAKE_TOP, FAKE_IFR);
}

static void expect_write(int i, uint32_t addr, uint32_t val)
{
  expect_eq(clk_fake.log[i].addr, addr);
  expect_eq(clk_fake.log[i].val, val);
}

static void check_ifr_gates(void)
{
  reset();
  clk_ifr_enable(CLK_IFR_MSDC0); /* infracfg 0x8c clears, bit 2 */
  clk_ifr_disable(CLK_IFR_MSDC0);
  clk_ifr_enable(CLK_IFR_UART0);
  clk_ifr_enable(CLK_IFR_MSDC0_SRC);
  expect_eq(clk_fake.nlog, 4);
  expect_write(0, FAKE_IFR + 0x8c, 1u << 2);
  expect_write(1, FAKE_IFR + 0x88, 1u << 2);
  expect_write(2, FAKE_IFR + 0x84, 1u << 22);
  expect_write(3, FAKE_IFR + 0xc4, 1u << 9);

  clk_fake.mem[1][0x94 / 4] = 1u << 2; /* STA: gated */
  expect_eq(clk_ifr_enabled(CLK_IFR_MSDC0), 0);
  expect_eq(clk_ifr_enabled(CLK_IFR_MSDC1), 1);
}

static void check_mux_gate(void)
{
  reset();
  clk_mux_enable(CLK_TOP_MSDC50_0_SEL);
  clk_mux_disable(CLK_TOP_MSDC50_0_SEL);
  expect_eq(clk_fake.nlog, 2);
  expect_write(0, FAKE_TOP + 0x78, 1u << 15);
  expect_write(1, FAKE_TOP + 0x74, 1u << 15);

  clk_fake.mem[0][0x70 / 4] = 1u << 15;
  expect_eq(clk_mux_enabled(CLK_TOP_MSDC50_0_SEL), 0);
  clk_fake.mem[0][0x70 / 4] = 0;
  expect_eq(clk_mux_enabled(CLK_TOP_MSDC50_0_SEL), 1);
}

static void check_mux_parent(void)
{
  reset();
  clk_fake.mem[0][0x70 / 4] = 5u << 8 | 1u << 15; /* msdc50_0 on parent 5, gated */
  expect_eq(clk_mux_parent(CLK_TOP_MSDC50_0_SEL), 5);

  expect_eq(clk_mux_set_parent(CLK_TOP_MSDC50_0_SEL, 5), 0); /* no change, no writes */
  expect_eq(clk_fake.nlog, 0);

  expect_eq(clk_mux_set_parent(CLK_TOP_MSDC50_0_SEL, 0), 0);
  expect_eq(clk_fake.nlog, 3);
  expect_write(0, FAKE_TOP + 0x78, 7u << 8);
  expect_write(1, FAKE_TOP + 0x74, 0);
  expect_write(2, FAKE_TOP + 0x4, 1u << 13); /* CLK_CFG_UPDATE bit for this mux */

  clk_mux_enable(CLK_TOP_MSDC50_0_SEL); /* the reparent is applied on enable too */
  expect_eq(clk_fake.nlog, 5);
  expect_write(3, FAKE_TOP + 0x78, 1u << 15);
  expect_write(4, FAKE_TOP + 0x4, 1u << 13);
  clk_mux_enable(CLK_TOP_MSDC50_0_SEL);
  expect_eq(clk_fake.nlog, 6); /* only the gate this time */

  expect_eq(clk_mux_set_parent(CLK_TOP_UART_SEL, 2), -1); /* uart_sel is one bit wide */
  expect_eq(clk_mux_set_parent(CLK_TOP_MSDC50_0_SEL, 8), -1);
}

int main(void)
{
  test("infracfg gates use the SET and CLR registers", check_ifr_gates);
  test("mux gate", check_mux_gate);
  test("mux parent selection and update bit", check_mux_parent);
  return 0;
}
