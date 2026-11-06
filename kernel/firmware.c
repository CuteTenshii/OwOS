/* firmware.c: records whether UEFI or legacy BIOS booted us (detected in kernel.c's multiboot2 walk). */

#include "firmware.h"

static enum firmware_kind g_firmware = FIRMWARE_BIOS;

void firmware_set(enum firmware_kind kind) { g_firmware = kind; }

bool firmware_is_uefi(void) { return g_firmware == FIRMWARE_UEFI; }

const char *firmware_name(void) { return g_firmware == FIRMWARE_UEFI ? "UEFI" : "BIOS"; }
