/* blpwm.h: Intel panel-backlight PWM register layouts and level math, free of MMIO so the host tests can run it. */
#ifndef OWOS_BLPWM_H
#define OWOS_BLPWM_H

#include <stdbool.h>
#include <stdint.h>

/* Where a platform keeps the backlight PWM period (the "max") and duty cycle (the level). */
enum bl_layout {
  BL_NONE,      /* unknown platform: leave the backlight alone                              */
  BL_PCH_SPLIT, /* Ironlake..Ivy Bridge: period in PCH_CTL2[31:16], duty in CPU_CTL[15:0]    */
  BL_LPT,       /* Haswell..Kaby Lake (LPT/WPT/SPT/KBP PCH): both halves of PCH_CTL2         */
  BL_BXT,       /* Apollo/Gemini Lake and Cannon Point+ PCHs: separate FREQ and DUTY regs    */
  BL_VLV,       /* Bay Trail / Cherry Trail: per-pipe CTL in the display block               */
};

/* Pick the layout from the GPU's PCI device id and the PCH's ISA-bridge device id (00:1f.0, 0 if none). */
enum bl_layout bl_layout_for(uint16_t gpu_id, uint16_t pch_id);

/* Dimmest duty cycle a level maps to, so 0% stays readable instead of switching the panel off. */
uint32_t bl_min_duty(uint32_t period);

/* Clamp a raw duty cycle into [bl_min_duty(period), period]. */
uint32_t bl_clamp_duty(uint32_t duty, uint32_t period);

/* 0..100 (clamped) to a duty cycle over [bl_min_duty, period], and back (rounded, clamped). */
uint32_t bl_percent_to_duty(int percent, uint32_t period);
int bl_duty_to_percent(uint32_t duty, uint32_t period);

/* Convert between brightness duty and the register value; an active-low PWM counts the off time. Self-inverse. */
uint32_t bl_hw_duty(uint32_t duty, uint32_t period, bool active_low);

#endif /* OWOS_BLPWM_H */
