/* battery.c — ACPI battery detection and charge reading for the neofetch splash.
 *
 * Presence is decided cheaply: an ACPI Control Method Battery is a device whose
 * _HID is the EISA id "PNP0C0A", so acpi_aml_contains() byte-scans the DSDT and
 * every SSDT for that id (two encodings — the compressed 4-byte EisaId the ASL
 * compiler usually emits, and the plain ASCII form a few tables carry). Scanning
 * the SSDTs matters: many laptops declare the battery there, not in the DSDT.
 *
 * The actual percentage comes from aml.c, which evaluates the battery's _BST and
 * _BIF methods (see aml.h). That drives the Embedded Controller and isn't free,
 * so both queries are cached here. QEMU emulates no battery, so both are empty
 * there and the row only shows up on real machines. */

#include "battery.h"
#include "power.h"
#include "aml.h"
#include "timer.h"

int battery_present(void)
{
	/* Presence is fixed for the boot session, but the lookup byte-scans every
	 * ACPI table, so cache it — neofetch calls this on each render. */
	static int cached = -1;
	if (cached >= 0)
		return cached;

	/* EisaId("PNP0C0A") compresses to these four AML bytes: the manufacturer
	 * letters P,N,P pack into 0x41,0xD0 and the product id "0C0A" follows. */
	static const unsigned char eisa[4] = { 0x41, 0xD0, 0x0C, 0x0A };
	/* Fallback: a few tables store the _HID as a literal ASCII string. */
	static const unsigned char str[7] = { 'P', 'N', 'P', '0', 'C', '0', 'A' };
	cached = acpi_aml_contains(eisa, 4) || acpi_aml_contains(str, 7);
	return cached;
}

int battery_percent(int *charging, int *discharging)
{
	/* Evaluating _BST/_BIF drives the Embedded Controller (milliseconds), so
	 * cache the result and refresh at most once every few seconds rather than on
	 * every neofetch render. */
	static int valid = 0, pct = -1, chg = 0, dis = 0;
	static uint32_t last_t;
	static int have = 0;

	uint32_t now = timer_seconds();
	if (!have || (now - last_t) >= 5) {
		struct aml_battery b;
		valid = aml_read_battery(&b);
		if (valid) { pct = b.percent; chg = b.charging; dis = b.discharging; }
		else       { pct = -1; chg = dis = 0; }
		last_t = now;
		have = 1;
	}

	if (charging)    *charging = chg;
	if (discharging) *discharging = dis;
	return valid ? pct : -1;
}

const char *battery_error(void) { return aml_last_error(); }
