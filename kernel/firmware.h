/* firmware.h: which platform firmware booted us (UEFI, legacy BIOS, or a device-tree loader on ARM).
 * On x86 it is detected from the Multiboot2 EFI system-table tag; on UEFI the ACPI RSDP is not in the legacy BIOS
 * area. */
#ifndef OWOS_FIRMWARE_H
#define OWOS_FIRMWARE_H

#include <stdbool.h>

enum firmware_kind { FIRMWARE_BIOS = 0, FIRMWARE_UEFI = 1, FIRMWARE_DEVICETREE = 2 };

/* Record the detected firmware mode; called once from kernel_main. */
void firmware_set(enum firmware_kind kind);

/* 1 if we booted via UEFI, 0 for legacy BIOS / CSM. */
bool firmware_is_uefi(void);

/* "UEFI", "BIOS" or "Device tree", for banners and the neofetch splash. */
const char *firmware_name(void);

#endif /* OWOS_FIRMWARE_H */
