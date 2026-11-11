/* backlight.h: laptop panel backlight driven through the Intel integrated GPU's PWM (real hardware only). */
#ifndef OWOS_BACKLIGHT_H
#define OWOS_BACKLIGHT_H

#include <stdbool.h>
#include <stdint.h>

/* Probe once at boot, after pci_init. Adopts the PWM the firmware already enabled and leaves the level as is.
 * Returns true if a controllable backlight was found. */
bool backlight_init(void);

bool backlight_present(void);

/* Level as 0..100 over [dimmest readable, full]; set clamps. IRQ-safe. */
int backlight_percent(void);
void backlight_set_percent(int percent);

/* Raw PWM units, as /sys/class/backlight exposes them: max is the PWM period, raw the requested duty cycle,
 * actual the duty read back from the hardware. All 0 when no backlight is present. set_raw clamps. IRQ-safe. */
uint32_t backlight_max(void);
uint32_t backlight_raw(void);
uint32_t backlight_actual(void);
void backlight_set_raw(uint32_t duty);

/* Re-apply the level after S3, where firmware reprograms the PWM to its own default. */
void backlight_resume(void);

#endif /* OWOS_BACKLIGHT_H */
