/* firmware.h — which platform firmware booted us (UEFI vs legacy BIOS).
 *
 * Detected from the Multiboot2 boot info: GRUB includes an EFI system-table tag
 * (type 11/12) only on a UEFI boot. Legacy BIOS / CSM (and QEMU's `-kernel`
 * multiboot1 path) carry no such tag. This is load-bearing for ACPI: on UEFI
 * the RSDP is not in the legacy BIOS memory area, so the firmware mode tells
 * the shutdown path where the tables can be reached from. */
#ifndef OWOS_FIRMWARE_H
#define OWOS_FIRMWARE_H

enum firmware_kind { FIRMWARE_BIOS = 0, FIRMWARE_UEFI = 1 };

/* Record the detected firmware mode; called once from kernel_main during boot.
 */
void firmware_set(enum firmware_kind kind);

/* 1 if we booted via UEFI, 0 for legacy BIOS / CSM. */
int firmware_is_uefi(void);

/* "UEFI" or "BIOS" — for banners and the neofetch splash. */
const char *firmware_name(void);

#endif /* OWOS_FIRMWARE_H */
