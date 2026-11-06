/* firmware.c — remember whether UEFI or legacy BIOS booted us.
 *
 * The detection itself lives in kernel.c's multiboot2 tag walk (an EFI
 * system-table tag means UEFI); this just holds the answer for later readers
 * such as the shutdown path and the neofetch splash. */

#include "firmware.h"

static enum firmware_kind g_firmware = FIRMWARE_BIOS;

void firmware_set(enum firmware_kind kind) { g_firmware = kind; }

bool firmware_is_uefi(void) { return g_firmware == FIRMWARE_UEFI; }

const char *firmware_name(void) { return g_firmware == FIRMWARE_UEFI ? "UEFI" : "BIOS"; }
