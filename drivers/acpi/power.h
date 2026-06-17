/* power.h — turn the machine off or reboot it. */
#ifndef OWOS_POWER_H
#define OWOS_POWER_H

void power_off(void);     /* ACPI/QEMU shutdown; returns only if it fails  */
void power_reset(void);   /* reboot via the 8042 controller, then 3-fault  */

/* ACPI S3 suspend-to-RAM: save CPU state, point the firmware at a wake
 * trampoline, and write the S3 sleep type to PM1_CNT. Returns after the machine
 * resumes, or immediately if the firmware doesn't advertise S3. Restores the
 * CPU only — on real hardware devices lose power across S3 and aren't re-inited
 * yet (fine under QEMU, where S3 is a pause that keeps device state). */
void power_sleep(void);

/* 1 if ACPI S3 is available (FADT + DSDT _S3 + FACS all present). Read-only;
 * use it to gate UI that offers Sleep so an unsupported host (e.g. VMware,
 * which doesn't expose S3 to the guest) doesn't show a dead button. */
int power_can_sleep(void);

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

/* Locate and identity-map a primary ACPI table by its 4-character signature
 * (e.g. "APIC" for the MADT). Walks whichever root table the RSDP names (XSDT or
 * RSDT) and maps the whole table. Returns the mapped table base (an sdt_header:
 * the u32 at offset 4 is its total length), or NULL if ACPI is unreachable or
 * the table is absent. */
void *acpi_find_table(const char *sig);

/* Stash a copy of the firmware RSDP the bootloader handed us (multiboot2 ACPI
 * tag). On a UEFI boot the RSDP is NOT in the legacy BIOS memory area, so this
 * is the only way the ACPI shutdown/AML code can find the tables. Pass the raw
 * RSDP bytes; the first 20 (signature..rsdt_addr) are copied. No-op-safe. */
void acpi_set_rsdp(const void *rsdp_copy);

#endif /* OWOS_POWER_H */
