/* battery.h — best-effort ACPI battery detection for the neofetch splash. */
#ifndef OWOS_BATTERY_H
#define OWOS_BATTERY_H

int battery_present(void); /* 1 if the firmware exposes an ACPI battery */

/* Remaining charge as a percentage (0..100), or -1 if it can't be read (no
 * battery, or the _BST/_BIF AML couldn't be evaluated). On success, *charging
 * and *discharging (if non-NULL) are set from the _BST state bits, and *minutes
 * (if non-NULL) gets the estimated minutes to empty while discharging, else -1.
 */
int battery_percent(int *charging, int *discharging, int *minutes);

/* Why the last battery_percent() returned -1 (e.g. "_BST not found",
 * "ext op 0x..."), for diagnostics. Empty string after a successful read. */
const char *battery_error(void);

/* A one-line diagnostic of where battery detection stands — the PNP0C0A _HID
 * presence, the AML namespace-walk stats, and either the read percentage or the
 * interpreter's failure note. Surfaced via /proc/battery for debugging on real
 * hardware, where QEMU's absent battery makes this path untestable locally. */
const char *battery_diag(void);

#endif /* OWOS_BATTERY_H */
