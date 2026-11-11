/* blpwm.c: Intel panel-backlight layout selection and level math (see blpwm.h). */

#include "blpwm.h"

/* PCH families share the top bits of the ISA bridge id; i915 matches on the same mask. */
#define PCH_ID_MASK 0xFF80u

static bool gpu_is_vlv(uint16_t id) { return (id >= 0x0F30 && id <= 0x0F33) || (id >= 0x22B0 && id <= 0x22B3); }

static bool gpu_is_bxt(uint16_t id)
{
  switch (id) {
  case 0x0A84: /* Broxton                    */
  case 0x1A84:
  case 0x1A85:
  case 0x5A84: /* Apollo Lake HD 505         */
  case 0x5A85: /* Apollo Lake HD 500         */
  case 0x3184: /* Gemini Lake UHD 605        */
  case 0x3185: /* Gemini Lake UHD 600        */
    return true;
  default:
    return false;
  }
}

enum bl_layout bl_layout_for(uint16_t gpu_id, uint16_t pch_id)
{
  /* SoC parts have no PCH; their 00:1f.0 is an LPC bridge whose id says nothing about the PWM. */
  if (gpu_is_vlv(gpu_id))
    return BL_VLV;
  if (gpu_is_bxt(gpu_id))
    return BL_BXT;

  switch (pch_id & PCH_ID_MASK) {
  case 0x3B00: /* Ibex Peak                  */
  case 0x1C00: /* Cougar Point               */
  case 0x1E00: /* Panther Point              */
    return BL_PCH_SPLIT;
  case 0x8C00: /* Lynx Point                 */
  case 0x9C00: /* Lynx Point-LP              */
  case 0x8C80: /* Wildcat Point              */
  case 0x9C80: /* Wildcat Point-LP           */
  case 0xA100: /* Sunrise Point              */
  case 0x9D00: /* Sunrise Point-LP           */
  case 0xA280: /* Kaby Point                 */
  case 0xA380: /* Comet Lake-V (KBP-based)   */
    return BL_LPT;
  case 0xA300: /* Cannon Point               */
  case 0x9D80: /* Cannon Point-LP            */
  case 0x0280: /* Comet Point                */
  case 0x0680: /* Comet Point-H              */
  case 0x3480: /* Ice Point                  */
  case 0x3880: /* Ice Point-LP               */
  case 0x4B00: /* Mule Creek Canyon          */
  case 0xA080: /* Tiger Point                */
  case 0x4380: /* Tiger Point-H              */
  case 0x4D80: /* Jasper Point               */
  case 0x7A80: /* Alder Point                */
  case 0x5180: /* Alder Point-P              */
  case 0x7A00: /* Alder Point-S              */
  case 0x5480: /* Alder Point-N              */
    return BL_BXT;
  default:
    return BL_NONE;
  }
}

uint32_t bl_min_duty(uint32_t period)
{
  uint32_t m = (period + 19) / 20;
  return m ? m : 1;
}

uint32_t bl_clamp_duty(uint32_t duty, uint32_t period)
{
  uint32_t lo = bl_min_duty(period);
  if (duty < lo)
    return lo;
  return duty > period ? period : duty;
}

uint32_t bl_percent_to_duty(int percent, uint32_t period)
{
  if (percent < 0)
    percent = 0;
  if (percent > 100)
    percent = 100;
  uint32_t lo = bl_min_duty(period);
  if (period <= lo)
    return period;
  uint64_t span = period - lo;
  return lo + (uint32_t)((span * (uint32_t)percent + 50) / 100);
}

int bl_duty_to_percent(uint32_t duty, uint32_t period)
{
  uint32_t lo = bl_min_duty(period);
  if (period <= lo)
    return 100;
  duty = bl_clamp_duty(duty, period);
  uint64_t span = period - lo;
  return (int)(((uint64_t)(duty - lo) * 100 + span / 2) / span);
}

uint32_t bl_hw_duty(uint32_t duty, uint32_t period, bool active_low)
{
  if (duty > period)
    duty = period;
  return active_low ? period - duty : duty;
}
