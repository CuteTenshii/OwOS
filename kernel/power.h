/* power.h — turn the machine off or reboot it. */
#ifndef OWOS_POWER_H
#define OWOS_POWER_H

void power_off(void);     /* ACPI/QEMU shutdown; returns only if it fails  */
void power_reset(void);   /* reboot via the 8042 controller, then 3-fault  */

/* Scan the ACPI device AML (DSDT + every SSDT) for a byte sequence such as an
 * EISA _HID. Returns 1 if found, 0 if absent or ACPI is unreachable. Used by
 * battery.c to detect an ACPI battery device wherever the firmware declared it. */
int acpi_aml_contains(const unsigned char *needle, unsigned int n);

/* Invoke fn(body, len, ctx) for each ACPI AML definition block — the DSDT, then
 * every SSDT — where body/len is the table's AML byte stream (past the header).
 * Used by the AML interpreter (aml.c) to build the namespace. No-op if ACPI is
 * unreachable. */
void acpi_for_each_aml(void (*fn)(const unsigned char *body, unsigned int len,
                                  void *ctx), void *ctx);

/* Stash a copy of the firmware RSDP the bootloader handed us (multiboot2 ACPI
 * tag). On a UEFI boot the RSDP is NOT in the legacy BIOS memory area, so this
 * is the only way the ACPI shutdown/AML code can find the tables. Pass the raw
 * RSDP bytes; the first 20 (signature..rsdt_addr) are copied. No-op-safe. */
void acpi_set_rsdp(const void *rsdp_copy);

#endif /* OWOS_POWER_H */
