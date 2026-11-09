/* hid_pad.h: game controllers that need more than their HID descriptor: identification, DualShock 4 rumble and
 * Bluetooth reports, Switch Pro Controller USB start-up. Pure logic so host tests can link it. */
#ifndef OWOS_HID_PAD_H
#define OWOS_HID_PAD_H

#include <stdbool.h>
#include <stdint.h>

enum pad_kind { PAD_GENERIC, PAD_DS4, PAD_SWITCH_PRO };

enum pad_kind pad_kind_usb(uint16_t vid, uint16_t pid);
/* Bluetooth device name as the controller advertises it. */
enum pad_kind pad_kind_name(const char *name);
const char *pad_kind_token(enum pad_kind k); /* "generic", "ds4", "switchpro" */
const char *pad_kind_label(enum pad_kind k);

#define DS4_USB_OUT_ID 0x05
#define DS4_USB_OUT_LEN 32
#define DS4_BT_OUT_ID 0x11
#define DS4_BT_OUT_LEN 78

/* Output report (ID byte first) that sets the two rumble motors, 0..65535 each; returns its length. The Bluetooth
 * form carries the CRC-32 the controller checks. */
int ds4_pack_rumble(bool bt, uint16_t strong, uint16_t weak, uint8_t *buf);

/* A DualShock 4 Bluetooth input report with ID 0x11 (which the HID descriptor does not describe) in the layout the
 * USB descriptor yields: buttons bit n = button n + 1, hat 0 or 1..8, axes X Y Z Rx Ry Rz. */
#define DS4_BT_IN_ID 0x11
bool ds4_decode_bt(const uint8_t *data, int len, uint32_t *buttons, uint8_t *hat, int32_t axes[6]);

/* Output reports (ID byte first) that put a Switch Pro Controller on USB into plain HID mode, in order. */
#define SWITCH_USB_STEPS 4
int switch_usb_step(int step, uint8_t *buf);

#endif
