/* battery.h — best-effort ACPI battery detection for the neofetch splash. */
#ifndef OWOS_BATTERY_H
#define OWOS_BATTERY_H

int battery_present(void);  /* 1 if the firmware exposes an ACPI battery */

/* Remaining charge as a percentage (0..100), or -1 if it can't be read (no
 * battery, or the _BST/_BIF AML couldn't be evaluated). On success, *charging
 * and *discharging (if non-NULL) are set from the _BST state bits. */
int battery_percent(int *charging, int *discharging);

/* Why the last battery_percent() returned -1 (e.g. "_BST not found",
 * "ext op 0x..."), for diagnostics. Empty string after a successful read. */
const char *battery_error(void);

#endif /* OWOS_BATTERY_H */
