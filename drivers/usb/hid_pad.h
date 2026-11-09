/* hid_pad.h: game controllers that need more than their HID descriptor: identification, DualShock 4 rumble and
 * Bluetooth reports, Switch Pro Controller USB start-up. Pure logic so host tests can link it. */
#ifndef OWOS_HID_PAD_H
#define OWOS_HID_PAD_H

#include <stdbool.h>
#include <stdint.h>

enum pad_kind { PAD_GENERIC, PAD_DS4, PAD_DS5, PAD_SWITCH_PRO, PAD_XBOX };

enum pad_kind pad_kind_usb(uint16_t vid, uint16_t pid);
/* Bluetooth device name as the controller advertises it. */
enum pad_kind pad_kind_name(const char *name);
const char *pad_kind_token(enum pad_kind k); /* "generic", "ds4", "ds5", "switchpro", "xbox" */
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

/* DualSense: rumble report (ID byte first) and the Bluetooth input report with ID 0x31, in the same canonical layout.
 * seq is the Bluetooth output sequence number, 0..15. */
#define DS5_USB_OUT_ID 0x02
#define DS5_USB_OUT_LEN 63
#define DS5_BT_OUT_ID 0x31
#define DS5_BT_OUT_LEN 78
#define DS5_BT_IN_ID 0x31
int ds5_pack_rumble(bool bt, uint8_t seq, uint16_t strong, uint16_t weak, uint8_t *buf);
bool ds5_decode_bt(const uint8_t *data, int len, uint32_t *buttons, uint8_t *hat, int32_t axes[6]);

/* Xbox One / Series controllers on USB speak GIP: power-on packet, 0x20 input packets, 0x07 guide button packets, 0x09
 * rumble. Buttons: A 0, B 1, X 2, Y 3, LB 4, RB 5, View 6, Menu 7, L3 8, R3 9, Guide 10; axes as for the DualShock
 * (X Y Z Rx Ry Rz) with the triggers on Rx and Ry. */
#define XBOX_BTN_GUIDE (1u << 10)
#define XBOX_INIT_LEN 5
#define XBOX_RUMBLE_LEN 13
int xbox_pack_init(uint8_t seq, uint8_t *buf);
int xbox_pack_rumble(uint8_t seq, uint16_t strong, uint16_t weak, uint8_t *buf);
bool xbox_decode(const uint8_t *data, int len, uint32_t *buttons, uint8_t *hat, int32_t axes[6]);
/* A guide button packet: true and the new state in *down. */
bool xbox_decode_guide(const uint8_t *data, int len, bool *down);

/* Output reports (ID byte first) that put a Switch Pro Controller on USB into plain HID mode, in order. */
#define SWITCH_USB_STEPS 4
int switch_usb_step(int step, uint8_t *buf);

#endif
