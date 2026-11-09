/* hid.h: USB HID class driver services beyond the host-core hooks in usbhost.h. */
#ifndef OWOS_HID_H
#define OWOS_HID_H

#include <stdbool.h>
#include <stdint.h>

/* Text state of the gamepad/joystick, backing /sys/class/input/js0/state: one line
 * "SEQ BUTTONS HAT X Y Z RX RY RZ SLIDER DIAL\n" in decimal. SEQ bumps on every report, BUTTONS is a bit mask (bit n =
 * button n + 1), HAT is 0 centered or 1 (up) to 8 clockwise, axes run -32768..32767. Returns the byte count, or -1
 * when no gamepad is attached. */
int hid_gamepad_state(char *buf, int max);

/* Longest rumble a single request may ask for. */
#define HID_RUMBLE_MAX_MS 5000

bool hid_gamepad_can_rumble(void);

/* Vibrate every attached gamepad that has a haptics report: strengths 0..65535, 0 ms or 0/0 stops. Returns how
 * many pads took it; 0 when none can vibrate. */
int hid_gamepad_rumble(uint16_t strong, uint16_t weak, uint32_t ms);

/* HID devices that are not on USB (Bluetooth): the transport parses nothing itself, it hands over the report
 * descriptor and every input report. send writes one output report (ID byte first when id != 0). */
struct hid_dev;
struct hid_dev *hid_bt_add(const uint8_t *rd, int len, int (*send)(void *ctx, uint8_t id, const uint8_t *buf, int n),
                           void *ctx);
void hid_bt_report(struct hid_dev *h, const uint8_t *data, int len);
void hid_bt_remove(struct hid_dev *h);

#endif /* OWOS_HID_H */
