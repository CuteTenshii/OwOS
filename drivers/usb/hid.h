/* hid.h: USB HID class driver services beyond the host-core hooks in usbhost.h. */
#ifndef OWOS_HID_H
#define OWOS_HID_H

/* Text state of the gamepad/joystick, backing /sys/class/input/js0/state: one line
 * "SEQ BUTTONS HAT X Y Z RX RY RZ SLIDER DIAL\n" in decimal. SEQ bumps on every report, BUTTONS is a bit mask (bit n =
 * button n + 1), HAT is 0 centered or 1 (up) to 8 clockwise, axes run -32768..32767. Returns the byte count, or -1
 * when no gamepad is attached. */
int hid_gamepad_state(char *buf, int max);

#endif /* OWOS_HID_H */
