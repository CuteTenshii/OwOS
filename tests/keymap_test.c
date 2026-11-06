/* keymap_test.c: PS/2 E0 and USB HID usage to KEY_* tables (drivers/input/keymap.h). */
#define OWOS_SUITE_TEST
#include "../drivers/input/keymap.h"
#include "test.h"

int main(void)
{
	test("PS/2 E0 media codes");
	expect(kbd_e0_key(0x10) == KEY_MEDIA_PREV);
	expect(kbd_e0_key(0x19) == KEY_MEDIA_NEXT);
	expect(kbd_e0_key(0x20) == KEY_MUTE);
	expect(kbd_e0_key(0x21) == KEY_LAUNCH_CALC);
	expect(kbd_e0_key(0x22) == KEY_MEDIA_PLAY);
	expect(kbd_e0_key(0x24) == KEY_MEDIA_STOP);
	expect(kbd_e0_key(0x2E) == KEY_VOL_DOWN);
	expect(kbd_e0_key(0x30) == KEY_VOL_UP);

	test("PS/2 E0 system, browser and brightness codes");
	expect(kbd_e0_key(0x32) == KEY_LAUNCH_BROWSER);
	expect(kbd_e0_key(0x5E) == KEY_POWER);
	expect(kbd_e0_key(0x5F) == KEY_SLEEP);
	expect(kbd_e0_key(0x65) == KEY_SEARCH);
	expect(kbd_e0_key(0x67) == KEY_WWW_REFRESH);
	expect(kbd_e0_key(0x69) == KEY_WWW_FORWARD);
	expect(kbd_e0_key(0x6A) == KEY_WWW_BACK);
	expect(kbd_e0_key(0x6B) == KEY_BRIGHT_DOWN);
	expect(kbd_e0_key(0x6C) == KEY_BRIGHT_UP);

	test("PS/2 E0 ignored and non-media codes");
	expect(kbd_e0_key(0x66) == 0);
	expect(kbd_e0_key(0x68) == 0);
	expect(kbd_e0_key(0x6D) == 0);
	expect(kbd_e0_key(0x1D) == 0);
	expect(kbd_e0_key(0x48) == 0);
	expect(kbd_e0_key(0x5B) == 0);
	expect(kbd_e0_key(0xA0) == 0);

	test("HID consumer usages");
	expect(hid_consumer_key(0xE9) == KEY_VOL_UP);
	expect(hid_consumer_key(0xEA) == KEY_VOL_DOWN);
	expect(hid_consumer_key(0xE2) == KEY_MUTE);
	expect(hid_consumer_key(0xCD) == KEY_MEDIA_PLAY);
	expect(hid_consumer_key(0xB5) == KEY_MEDIA_NEXT);
	expect(hid_consumer_key(0xB6) == KEY_MEDIA_PREV);
	expect(hid_consumer_key(0xB7) == KEY_MEDIA_STOP);
	expect(hid_consumer_key(0x6F) == KEY_BRIGHT_UP);
	expect(hid_consumer_key(0x70) == KEY_BRIGHT_DOWN);
	expect(hid_consumer_key(0x192) == KEY_LAUNCH_CALC);
	expect(hid_consumer_key(0x196) == KEY_LAUNCH_BROWSER);
	expect(hid_consumer_key(0x223) == KEY_LAUNCH_BROWSER);
	expect(hid_consumer_key(0x18A) == KEY_LAUNCH_MAIL);
	expect(hid_consumer_key(0x194) == KEY_LAUNCH_FILES);
	expect(hid_consumer_key(0x221) == KEY_SEARCH);
	expect(hid_consumer_key(0x224) == KEY_WWW_BACK);
	expect(hid_consumer_key(0x225) == KEY_WWW_FORWARD);
	expect(hid_consumer_key(0x227) == KEY_WWW_REFRESH);
	expect(hid_consumer_key(0x00) == 0);
	expect(hid_consumer_key(0x1E9) == 0);

	test("HID keyboard and system usages");
	expect(hid_kbd_key(0x66) == KEY_POWER);
	expect(hid_kbd_key(0x04) == 0);
	expect(hid_system_key(0x81) == KEY_POWER);
	expect(hid_system_key(0x82) == KEY_SLEEP);
	expect(hid_system_key(0x83) == 0);
	return 0;
}
