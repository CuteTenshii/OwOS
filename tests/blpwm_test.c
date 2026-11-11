/* blpwm_test.c: Intel backlight PWM layout selection and level math (drivers/video/blpwm.c).
 * Run through tests/run-tests.sh blpwm. */
#define OWOS_SUITE_TEST
#include "../drivers/video/blpwm.h"
#include "test.h"

static void layout_by_pch(void)
{
  /* GPU ids below are ordinary HD Graphics parts; the PCH id alone decides. */
  expect_eq(bl_layout_for(0x0046, 0x3B09), BL_PCH_SPLIT); /* Arrandale HD + HM55 (Ibex Peak)       */
  expect_eq(bl_layout_for(0x0126, 0x1C49), BL_PCH_SPLIT); /* Sandy Bridge HD 3000 + HM65            */
  expect_eq(bl_layout_for(0x0166, 0x1E59), BL_PCH_SPLIT); /* Ivy Bridge HD 4000 + HM76              */
  expect_eq(bl_layout_for(0x0416, 0x8C4B), BL_LPT);       /* Haswell HD 4600 + HM87                 */
  expect_eq(bl_layout_for(0x0A16, 0x9C43), BL_LPT);       /* Haswell-ULT HD 4400 + Lynx Point-LP    */
  expect_eq(bl_layout_for(0x1616, 0x9CC3), BL_LPT);       /* Broadwell HD 5500 + Wildcat Point-LP   */
  expect_eq(bl_layout_for(0x1916, 0x9D48), BL_LPT);       /* Skylake HD 520 + Sunrise Point-LP      */
  expect_eq(bl_layout_for(0x591B, 0xA2C9), BL_LPT);       /* Kaby Lake HD 630 + Kaby Point          */
  expect_eq(bl_layout_for(0x5917, 0x9D4E), BL_LPT);       /* Kaby Lake-R UHD 620 keeps SPT-LP        */
  expect_eq(bl_layout_for(0x3EA0, 0x9D84), BL_BXT);       /* Whiskey Lake UHD 620 + Cannon Point-LP */
  expect_eq(bl_layout_for(0x9BC5, 0x0284), BL_BXT);       /* Comet Lake UHD 630 + Comet Point       */
  expect_eq(bl_layout_for(0x9A49, 0xA082), BL_BXT);       /* Tiger Lake Iris Xe + Tiger Point       */
}

static void layout_by_gpu(void)
{
  /* SoCs: the 00:1f.0 LPC bridge must not be mistaken for a PCH. */
  expect_eq(bl_layout_for(0x0F31, 0x0F1C), BL_VLV); /* Bay Trail HD                */
  expect_eq(bl_layout_for(0x22B0, 0x229C), BL_VLV); /* Cherry Trail HD             */
  expect_eq(bl_layout_for(0x5A85, 0x5AE8), BL_BXT); /* Apollo Lake HD 500          */
  expect_eq(bl_layout_for(0x3185, 0x31E8), BL_BXT); /* Gemini Lake UHD 600         */
}

static void layout_unknown(void)
{
  expect_eq(bl_layout_for(0x0046, 0), BL_NONE);      /* no ISA bridge at 00:1f.0         */
  expect_eq(bl_layout_for(0x1234, 0x2918), BL_NONE); /* QEMU q35 ICH9 LPC, Bochs VGA     */
  expect_eq(bl_layout_for(0x2A42, 0x2919), BL_NONE); /* GM45 + ICH9M: pre-PCH, not handled */
}

static void percent_mapping(void)
{
  uint32_t period = 1000;
  expect_eq(bl_min_duty(period), 50);
  expect_eq(bl_percent_to_duty(0, period), 50);
  expect_eq(bl_percent_to_duty(100, period), 1000);
  expect_eq(bl_percent_to_duty(50, period), 525);
  expect_eq(bl_percent_to_duty(-20, period), 50);
  expect_eq(bl_percent_to_duty(250, period), 1000);

  expect_eq(bl_duty_to_percent(50, period), 0);
  expect_eq(bl_duty_to_percent(1000, period), 100);
  expect_eq(bl_duty_to_percent(10, period), 0); /* below the floor reads as 0 */
  expect_eq(bl_duty_to_percent(5000, period), 100);

  /* Every percent survives the round trip, so a 10% key step never stalls. */
  uint32_t periods[] = {0x1D4C, 0x0E8C, 0x0438, 25000000 / 200, 128};
  for (unsigned i = 0; i < sizeof periods / sizeof periods[0]; i++)
    for (int p = 0; p <= 100; p++)
      expect_eq(bl_duty_to_percent(bl_percent_to_duty(p, periods[i]), periods[i]), p);
}

static void floor_and_clamp(void)
{
  expect_eq(bl_min_duty(0), 1);
  expect_eq(bl_min_duty(19), 1);
  expect_eq(bl_min_duty(21), 2);
  expect_eq(bl_clamp_duty(0, 7500), 375);
  expect_eq(bl_clamp_duty(7501, 7500), 7500);
  expect_eq(bl_clamp_duty(4000, 7500), 4000);
}

static void polarity(void)
{
  expect_eq(bl_hw_duty(300, 1000, false), 300);
  expect_eq(bl_hw_duty(300, 1000, true), 700);
  expect_eq(bl_hw_duty(bl_hw_duty(123, 1000, true), 1000, true), 123);
  expect_eq(bl_hw_duty(2000, 1000, true), 0); /* out-of-range duty never underflows */
}

int owos_main_blpwm(void)
{
  test("layout from the PCH id", layout_by_pch);
  test("SoC layouts from the GPU id", layout_by_gpu);
  test("unknown platforms are left alone", layout_unknown);
  test("percent maps over the floor and back", percent_mapping);
  test("floor and clamp", floor_and_clamp);
  test("active-low PWM inverts the duty", polarity);
  return 0;
}
