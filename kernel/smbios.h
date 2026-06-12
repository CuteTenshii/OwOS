/* smbios.h — best-effort SMBIOS/DMI system-info reader for the neofetch splash.
 *
 * Recovers the machine's manufacturer + product name from DMI type 1 ("System
 * Information") so the splash can show a "Host:" line like real neofetch does
 * (it reads the same fields out of /sys/.../dmi/id on Linux). The SMBIOS tables
 * are located one of two ways:
 *   - legacy BIOS: scan the f-segment (0xF0000..0xFFFFF) for the "_SM_"/"_SM3_"
 *     entry-point anchor — QEMU's SeaBIOS and most BIOSes put it there;
 *   - UEFI: the legacy area carries no anchor, so kernel.c hands us the entry
 *     point captured from the multiboot2 SMBIOS tag (smbios_set_entry).
 * Best-effort like battery.c/edid.c: it probes once, copies the strings out of
 * the tables into static buffers while they're reachable, and fails gracefully
 * to "" (so QEMU, which advertises "QEMU" / a board name, still shows a Host).
 */
#ifndef OWOS_SMBIOS_H
#define OWOS_SMBIOS_H

#include <stdint.h>

/* Hand the kernel the SMBIOS entry-point structure's physical address, captured
 * from the multiboot2 SMBIOS tag (the only route to it under UEFI). Call before
 * smbios_init(); a 0 address is ignored and the legacy scan is used instead. */
void smbios_set_entry(uint32_t phys);

/* Hand over the firmware's EFI system-table pointer (from the multiboot2 EFI
 * tag), the canonical route to SMBIOS under UEFI where there's no legacy area
 * to scan. `is64` selects the 64- vs 32-bit EFI table layout. Call before
 * smbios_init(); a 0 pointer is ignored. */
void smbios_set_efi(uint64_t systab, int is64);

/* Parse the tables once and cache the vendor/product strings. Idempotent. */
void smbios_init(void);

/* System manufacturer / product name from DMI type 1, or "" if unreadable. */
const char *smbios_vendor(void);
const char *smbios_product(void);

/* The combined "<vendor> <product>" the splash prints, or "" if neither is
 * known (so neofetch can show a diagnostic instead). */
const char *smbios_host(void);

/* Why the Host string is empty (e.g. "no SMBIOS in EFI cfgtab", "EFI systab
 * >4GB"), for on-hardware debugging. Empty string after a successful read. */
const char *smbios_error(void);

#endif /* OWOS_SMBIOS_H */
