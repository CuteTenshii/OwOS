/* backlight.c: Intel integrated-GPU panel backlight PWM (register layouts as in i915's intel_backlight.c). */

#include "backlight.h"
#include "blpwm.h"
#include "paging.h"
#include "pci.h"
#include "spinlock.h"

#define PWM_ENABLE (1u << 31)
#define PWM_DUTY_MASK 0xFFFFu

#define BLC_PWM_CPU_CTL2 0x48250
#define BLC_PWM_CPU_CTL 0x48254
#define BLC_PWM_PCH_CTL1 0xC8250
#define BLC_PWM_PCH_CTL2 0xC8254
#define BLM_PCH_POLARITY (1u << 29)

#define BXT_BLC_PWM_CTL(c) (0xC8250u + 0x100u * (c))
#define BXT_BLC_PWM_FREQ(c) (0xC8254u + 0x100u * (c))
#define BXT_BLC_PWM_DUTY(c) (0xC8258u + 0x100u * (c))
#define BXT_BLC_PWM_POLARITY (1u << 29)

#define VLV_DISPLAY_BASE 0x180000u
#define VLV_BLC_PWM_CTL2(p) (VLV_DISPLAY_BASE + 0x61250u + 0x100u * (p))
#define VLV_BLC_PWM_CTL(p) (VLV_DISPLAY_BASE + 0x61254u + 0x100u * (p))
#define BLM_POLARITY_I965 (1u << 28)

/* Bay Trail's display block sits at 0x180000, past the 1 MiB that covers every PCH register. */
#define MMIO_MAP_PCH 0x100000u
#define MMIO_MAP_VLV 0x200000u

/* Below ~100 duty steps above the floor, neighbouring percent levels collide; no real panel PWM is that coarse. */
#define MIN_PERIOD 128u

#define INTEL_VENDOR 0x8086
#define PCH_SLOT 0x1F

static volatile uint8_t *mmio;
static bool present;
static uint32_t duty_reg; /* register holding the duty cycle                    */
static bool duty_field16; /* duty is bits 15:0 of duty_reg, period shares it   */
static uint32_t period;
static bool active_low;
static uint32_t duty; /* requested level in brightness terms (not inverted) */
static spinlock_t lock;

static inline uint32_t rd(uint32_t off) { return *(volatile uint32_t *)(mmio + off); }
static inline void wr(uint32_t off, uint32_t val) { *(volatile uint32_t *)(mmio + off) = val; }

static uint32_t read_duty(void)
{
  uint32_t v = rd(duty_reg);
  return bl_hw_duty(duty_field16 ? (v & PWM_DUTY_MASK) : v, period, active_low);
}

static void write_duty(uint32_t d)
{
  uint32_t hw = bl_hw_duty(d, period, active_low);
  if (duty_field16)
    wr(duty_reg, (rd(duty_reg) & ~PWM_DUTY_MASK) | hw);
  else
    wr(duty_reg, hw);
}

/* First Intel display controller on bus 0: device id and BAR0, which must sit below 4 GiB to be mapped. */
static bool find_gpu(uint16_t *gpu_id, uint32_t *bar)
{
  struct pci_dev_info d;
  for (int i = 0; pci_dev_info(i, &d); i++) {
    if (d.vendor != INTEL_VENDOR || d.class_code != 0x03 || d.bus != 0)
      continue;
    uint32_t bar0 = pci_read32(d.bus, d.slot, d.func, 0x10);
    if (((bar0 >> 1) & 0x3) == 0x2 && pci_read32(d.bus, d.slot, d.func, 0x14) != 0)
      return false;
    if (bar0 & 1) /* an I/O BAR, not MMIO */
      return false;
    *gpu_id = d.device;
    *bar = bar0 & ~0xFu;
    return *bar != 0;
  }
  return false;
}

/* The PCH is the ISA bridge at 00:1f.0; 0 when that slot holds something else. */
static uint16_t pch_id(void)
{
  uint32_t id = pci_read32(0, PCH_SLOT, 0, 0x00);
  if ((id & 0xFFFF) != INTEL_VENDOR)
    return 0;
  if ((pci_read32(0, PCH_SLOT, 0, 0x08) >> 16) != 0x0601)
    return 0;
  return (uint16_t)(id >> 16);
}

/* Locate the enabled PWM for the layout and read its period and polarity. False if firmware left it off. */
static bool adopt_pwm(enum bl_layout layout)
{
  switch (layout) {
  case BL_PCH_SPLIT: {
    uint32_t ctl1 = rd(BLC_PWM_PCH_CTL1);
    if (!(ctl1 & PWM_ENABLE) || !(rd(BLC_PWM_CPU_CTL2) & PWM_ENABLE))
      return false;
    period = rd(BLC_PWM_PCH_CTL2) >> 16;
    active_low = (ctl1 & BLM_PCH_POLARITY) != 0;
    duty_reg = BLC_PWM_CPU_CTL;
    duty_field16 = true;
    return true;
  }
  case BL_LPT: {
    /* With the PCH PWM off, a Haswell desktop part drives the panel from the CPU PWM; not handled. */
    uint32_t ctl1 = rd(BLC_PWM_PCH_CTL1);
    if (!(ctl1 & PWM_ENABLE))
      return false;
    period = rd(BLC_PWM_PCH_CTL2) >> 16;
    active_low = (ctl1 & BLM_PCH_POLARITY) != 0;
    duty_reg = BLC_PWM_PCH_CTL2;
    duty_field16 = true;
    return true;
  }
  case BL_BXT:
    /* The VBT names the controller; the one firmware enabled is the one wired to the panel. */
    for (uint32_t c = 0; c < 2; c++) {
      uint32_t ctl = rd(BXT_BLC_PWM_CTL(c));
      uint32_t freq = rd(BXT_BLC_PWM_FREQ(c));
      if (!(ctl & PWM_ENABLE) || freq == 0)
        continue;
      period = freq;
      active_low = (ctl & BXT_BLC_PWM_POLARITY) != 0;
      duty_reg = BXT_BLC_PWM_DUTY(c);
      duty_field16 = false;
      return true;
    }
    return false;
  case BL_VLV:
    for (uint32_t p = 0; p < 2; p++) {
      uint32_t ctl2 = rd(VLV_BLC_PWM_CTL2(p));
      uint32_t ctl = rd(VLV_BLC_PWM_CTL(p));
      if (!(ctl2 & PWM_ENABLE) || (ctl >> 16) == 0)
        continue;
      period = ctl >> 16;
      active_low = (ctl2 & BLM_POLARITY_I965) != 0;
      duty_reg = VLV_BLC_PWM_CTL(p);
      duty_field16 = true;
      return true;
    }
    return false;
  case BL_NONE:
    break;
  }
  return false;
}

bool backlight_init(void)
{
  uint16_t gpu;
  uint32_t bar;
  if (present || !find_gpu(&gpu, &bar))
    return present;

  enum bl_layout layout = bl_layout_for(gpu, pch_id());
  if (layout == BL_NONE)
    return false;

  uint32_t len = layout == BL_VLV ? MMIO_MAP_VLV : MMIO_MAP_PCH;
  if (paging_identity_map(bar, len) != 0)
    return false;
  paging_mark_uc(bar, len);
  mmio = (volatile uint8_t *)(uintptr_t)bar;

  if (!adopt_pwm(layout) || period < MIN_PERIOD)
    return false;
  duty = read_duty();
  present = true;
  return true;
}

bool backlight_present(void) { return present; }

int backlight_percent(void) { return present ? bl_duty_to_percent(duty, period) : 0; }

void backlight_set_percent(int percent)
{
  if (present)
    backlight_set_raw(bl_percent_to_duty(percent, period));
}

uint32_t backlight_max(void) { return present ? period : 0; }

uint32_t backlight_raw(void) { return present ? duty : 0; }

uint32_t backlight_actual(void)
{
  if (!present)
    return 0;
  uintptr_t f = spin_lock_irqsave(&lock);
  uint32_t d = read_duty();
  spin_unlock_irqrestore(&lock, f);
  return d;
}

void backlight_set_raw(uint32_t d)
{
  if (!present)
    return;
  uintptr_t f = spin_lock_irqsave(&lock);
  duty = bl_clamp_duty(d, period);
  write_duty(duty);
  spin_unlock_irqrestore(&lock, f);
}

void backlight_resume(void)
{
  if (!present)
    return;
  uintptr_t f = spin_lock_irqsave(&lock);
  write_duty(duty);
  spin_unlock_irqrestore(&lock, f);
}
