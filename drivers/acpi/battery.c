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
	/* Does a battery *device* exist in the tables? Fixed for the session and the
	 * lookup byte-scans every ACPI table, so cache it. */
	static int has_device = -1;
	if (has_device < 0) {
		/* EisaId("PNP0C0A") compresses to these four AML bytes: the manufacturer
		 * letters P,N,P pack into 0x41,0xD0 and the product id "0C0A" follows. */
		static const unsigned char eisa[4] = { 0x41, 0xD0, 0x0C, 0x0A };
		/* Fallback: a few tables store the _HID as a literal ASCII string. */
		static const unsigned char str[7] = { 'P', 'N', 'P', '0', 'C', '0', 'A' };
		has_device = acpi_aml_contains(eisa, 4) || acpi_aml_contains(str, 7);
	}
	if (!has_device)
		return 0;

	/* A device exists. A real, populated battery reports an actual charge via
	 * _BST/_BIF — trust that outright (it's how this worked on real laptops, and
	 * real _STA methods are quirky/EC-driven and not always evaluable by our
	 * best-effort interpreter, so we must not gate on _STA when we have a real
	 * reading). Only when the charge reads 0/unknown do we consult _STA to weed
	 * out a phantom: VMware advertises a PNP0C0A whose _BST reports a flat 0% and
	 * whose _STA says the battery is absent. */
	if (battery_percent(0, 0, 0) > 0)
		return 1;
	return aml_battery_present();
}

int battery_percent(int *charging, int *discharging, int *minutes)
{
	/* Evaluating _BST/_BIF drives the Embedded Controller (milliseconds), so
	 * cache the result and refresh at most once every few seconds rather than on
	 * every neofetch render. */
	static int valid = 0, pct = -1, chg = 0, dis = 0, mins = -1;
	static uint32_t last_t;
	static int have = 0;

	uint32_t now = timer_seconds();
	if (!have || (now - last_t) >= 5) {
		struct aml_battery b;
		valid = aml_read_battery(&b);
		if (valid) { pct = b.percent; chg = b.charging; dis = b.discharging; mins = b.minutes; }
		else       { pct = -1; chg = dis = 0; mins = -1; }
		last_t = now;
		have = 1;
	}

	if (charging)    *charging = chg;
	if (discharging) *discharging = dis;
	if (minutes)     *minutes = mins;
	return valid ? pct : -1;
}

const char *battery_error(void) { return aml_last_error(); }

/* battery_diag — a one-line account of where detection stands, for debugging on
 * a real laptop (QEMU has no battery, so this path can't be exercised locally).
 * Reports, in order: whether the PNP0C0A _HID byte-pattern exists in the ACPI
 * tables at all (hid=0/1); the namespace-walk stats (objects recorded n=, and
 * the opcode any term-list walk first bailed on b=, or b=- for none); and either
 * the read percentage (pct=) or the aml interpreter's failure note (err=). Read
 * it with `cat /proc/battery`. */
const char *battery_diag(void)
{
	static char buf[192];
	int i = 0;

	/* Stage 1: does a battery *device* exist in the tables? (Same two encodings
	 * battery_present() scans for.) hid=0 means the firmware exposes no PNP0C0A
	 * at all — nothing downstream can help. */
	static const unsigned char eisa[4] = { 0x41, 0xD0, 0x0C, 0x0A };
	static const unsigned char str[7]  = { 'P', 'N', 'P', '0', 'C', '0', 'A' };
	int hid = acpi_aml_contains(eisa, 4) || acpi_aml_contains(str, 7);

	const char *h = hid ? "hid=1 " : "hid=0 ";
	for (int k = 0; h[k] && i < (int)sizeof buf - 1; k++) buf[i++] = h[k];

	/* Stage 2: namespace-walk stats. */
	int nnodes = 0, bail = -1;
	aml_ns_stats(&nnodes, &bail);
	buf[i++] = 'n'; buf[i++] = '=';
	{
		char t[12]; int m = 0, v = nnodes;
		if (!v) t[m++] = '0';
		while (v) { t[m++] = (char)('0' + v % 10); v /= 10; }
		while (m && i < (int)sizeof buf - 1) buf[i++] = t[--m];
	}
	buf[i++] = ' '; buf[i++] = 'b'; buf[i++] = '=';
	if (bail < 0) {
		buf[i++] = '-';
	} else {
		static const char hx[] = "0123456789ABCDEF";
		buf[i++] = '0'; buf[i++] = 'x';
		for (int sh = 12; sh >= 0; sh -= 4) buf[i++] = hx[(bail >> sh) & 0xF];
	}
	buf[i++] = ' ';

	/* Stage 3: actually read it, and report the percentage or the failure note. */
	struct aml_battery b;
	if (aml_read_battery(&b)) {
		const char *p = "pct=";
		for (int k = 0; p[k]; k++) buf[i++] = p[k];
		int v = b.percent; char t[4]; int m = 0;
		if (!v) t[m++] = '0';
		while (v) { t[m++] = (char)('0' + v % 10); v /= 10; }
		while (m) buf[i++] = t[--m];
	} else {
		const char *e = "err=";
		for (int k = 0; e[k]; k++) buf[i++] = e[k];
		const char *m = aml_last_error();
		for (int k = 0; m[k] && i < (int)sizeof buf - 1; k++) buf[i++] = m[k];
	}
	buf[i] = 0;
	return buf;
}
