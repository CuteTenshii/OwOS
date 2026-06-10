/* power.h — turn the machine off or reboot it. */
#ifndef OWOS_POWER_H
#define OWOS_POWER_H

void power_off(void);     /* ACPI/QEMU shutdown; returns only if it fails  */
void power_reset(void);   /* reboot via the 8042 controller, then 3-fault  */

#endif /* OWOS_POWER_H */
