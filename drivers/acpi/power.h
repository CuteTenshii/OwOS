/* power.h: power off, reset and sleep. */
#ifndef OWOS_POWER_H
#define OWOS_POWER_H

#include <stdbool.h>
#include <stdint.h>

void power_off(void);   /* ACPI/QEMU shutdown; returns only if it fails  */
void power_reset(void); /* reboot via the 8042 controller, then 3-fault  */

/* ACPI S3 suspend-to-RAM; returns after resume, or at once if S3 is unsupported. */
void power_sleep(void);

/* 1 if ACPI S3 is available (FADT, DSDT _S3 and FACS present). */
bool power_can_sleep(void);

/* Scan DSDT and all SSDTs for a byte sequence (e.g. an EISA _HID). 1 if found. */
bool acpi_aml_contains(const uint8_t *needle, uint32_t n);

/* Call fn(body, len, ctx) for the DSDT and each SSDT AML stream (past the header). */
void acpi_for_each_aml(void (*fn)(const uint8_t *body, uint32_t len, void *ctx), void *ctx);

/* Map a primary ACPI table by 4-char signature; NULL if absent or ACPI is unreachable. */
void *acpi_find_table(const char *sig);

/* Copy the first 20 bytes of the bootloader's RSDP (not in BIOS memory on UEFI boots). */
void acpi_set_rsdp(const void *rsdp_copy);

#endif /* OWOS_POWER_H */
