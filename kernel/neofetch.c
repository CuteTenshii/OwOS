/* neofetch.c — a little system-info splash, OwO logo beside the stats. */

#include "neofetch.h"
#include "vga.h"
#include "cpu.h"
#include "rtc.h"
#include "timer.h"
#include "pmm.h"
#include "fs.h"
#include "pci.h"
#include "sched.h"
#include "ata.h"
#include "ahci.h"
#include "nvme.h"
#include "battery.h"
#include "firmware.h"
#include "fb.h"
#include "edid.h"
#include "smbios.h"

#define LOGO_WIDTH 16

static const char *logo[] = {
	"   /\\___/\\",
	"  /       \\",
	"  ( O w O )",
	"  (   v   )",
	"   \\     /",
	"    \\___/",
};
#define LOGO_LINES ((int)(sizeof(logo) / sizeof(logo[0])))

static void wr2(uint32_t v)   /* two-digit zero-padded                       */
{
	vga_putc('0' + (char)((v / 10) % 10));
	vga_putc('0' + (char)(v % 10));
}

/* CPU speed (MHz), calibrated once and cached: cpu_mhz() busy-waits ~200 ms, so
 * we never want to pay that again on a re-run of the splash. */
static uint32_t cpu_speed_mhz;

static void label(const char *s)
{
	vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
	vga_write(s);
	vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

/* The physical storage devices the kernel found, gathered for the splash. */
struct storage_dev {
	const char *type;
	const char *model;         /* manufacturer/model string ("" if unknown)      */
	uint32_t    sectors;       /* 512-byte sectors (0 = capacity unknown)        */
};

static int collect_storage(struct storage_dev *out)
{
	int n = 0;
	if (ata_present()) {
		out[n].type = "IDE/ATA";
		out[n].model = ata_model();
		out[n++].sectors = ata_sectors();
	}
	if (ahci_present()) {
		out[n].type = "SATA/AHCI";
		out[n].model = ahci_model();
		out[n++].sectors = ahci_sectors();
	}
	if (nvme_present()) {
		out[n].type = "NVMe";
		out[n].model = nvme_model();
		out[n++].sectors = nvme_sectors();
	}
	return n;
}

static void info_row(int i, const char *brand, const struct rtc_time *t)
{
	switch (i) {
	case 0:
		vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
		vga_write("OwOS");
		vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
		break;
	case 1: vga_write("----------------"); break;
	case 2: label("OS:      "); vga_write("OwOS 0.1"); break;
	case 3: label("Kernel:  "); vga_write("owos (i386, 32-bit)"); break;
	case 11: label("Firmware:"); vga_putc(' '); vga_write(firmware_name()); break;
	case 4:
		label("CPU:     ");
		vga_write(brand);
		if (cpu_speed_mhz) {
			/* MHz -> GHz with two decimals, rounded in hundredths so a value
			 * like 2995 MHz carries cleanly to 3.00 GHz rather than 2.99. */
			uint32_t cghz = (cpu_speed_mhz + 5) / 10;   /* hundredths of a GHz */
			vga_write(" @ ");
			vga_write_dec(cghz / 100);
			vga_putc('.');
			wr2(cghz % 100);
			vga_write(" GHz");
		}
		break;
	case 5: label("Uptime:  "); vga_write_dec(timer_seconds()); vga_write(" s"); break;
	case 6:
		label("Date:    ");
		vga_write_dec(t->year); vga_putc('-'); wr2(t->month); vga_putc('-'); wr2(t->day);
		vga_putc(' '); wr2(t->hour); vga_putc(':'); wr2(t->min); vga_putc(':'); wr2(t->sec);
		break;
	case 7:
		label("Memory:  ");
		vga_write_dec(pmm_ram_used_kib() / 1024); vga_write(" / ");
		vga_write_dec(pmm_installed_kib() / 1024); vga_write(" MiB");
		break;
	case 8:
		label("Disk:    ");
		vga_write_dec((uint32_t)fs_file_count()); vga_write(" files, ");
		vga_write_dec(fs_used_bytes()); vga_write(" bytes");
		break;
	case 9: label("PCI:     "); vga_write_dec((uint32_t)pci_count()); vga_write(" devices"); break;
	case 10: label("Tasks:   "); vga_write_dec((uint32_t)sched_count()); break;
	}
}

/* Print a 512-byte-sector capacity as "N MiB", or "N.N GiB" once it reaches
 * 1 GiB so large disks read naturally. */
static void write_capacity(uint32_t sectors)
{
	uint32_t mib = sectors / 2048;
	if (mib >= 1024) {
		uint32_t tenths = (mib * 10) / 1024;   /* MiB -> tenths of a GiB        */
		vga_write_dec(tenths / 10);
		vga_putc('.');
		vga_write_dec(tenths % 10);
		vga_write(" GiB");
	} else {
		vga_write_dec(mib);
		vga_write(" MiB");
	}
}

/* Render one "Storage:" row: the first carries the label, the rest indent to
 * line up under it. idx selects which detected device to print. */
static void storage_row(int idx, const struct storage_dev *sd, int nsd)
{
	label(idx == 0 ? "Storage: " : "         ");
	if (nsd == 0) {
		vga_write("none detected");
		return;
	}
	vga_write(sd[idx].type);
	if (sd[idx].model && sd[idx].model[0]) {
		vga_putc(' ');
		vga_write(sd[idx].model);
	}
	if (sd[idx].sectors) {
		vga_write(" (");
		write_capacity(sd[idx].sectors);
		vga_putc(')');
	}
}

/* Render one "Display:" row: the first carries the label and the active text
 * mode, the rest indent under it. idx selects which display controller. */
static void display_row(int idx, const struct pci_display *dd, int ndd)
{
	label(idx == 0 ? "Display: " : "         ");
	if (ndd == 0) {
		vga_write("80x25 VGA text");
		return;
	}
	if (dd[idx].name && dd[idx].name[0])
		vga_write(dd[idx].name);
	else
		vga_write("VGA-compatible controller");
	if (idx == 0) {
		/* Prefer the monitor's real refresh from EDID (real hardware, Intel
		 * GMBUS). Failing that there is no refresh register to read, so report
		 * the standard mode timing: a linear framebuffer (VBE/DISPI or adopted)
		 * runs at 60 Hz, VGA text mode 3 (720x400) at its native 70 Hz. */
		int hz = edid_refresh_hz();
		if (hz <= 0)
			hz = fb_active() ? 60 : 70;
		vga_write(" (80x25 text, ");
		vga_write_dec((uint32_t)hz);
		vga_write("Hz)");
	}
}

void neofetch(void)
{
	char brand[49];
	cpu_brand(brand);
	if (!cpu_speed_mhz)
		cpu_speed_mhz = cpu_mhz();   /* calibrate once; cached for re-runs    */
	struct rtc_time t;
	rtc_now(&t);

	struct storage_dev sd[3];
	int nsd = collect_storage(sd);

	struct pci_display dd[8];
	int ndd = pci_displays(dd, 8);

	const int fixed = 12;   /* info_row cases 0..11 (last is Firmware)            */
	/* "Host:" is the machine's DMI vendor+product (QEMU advertises "QEMU ...",
	 * most real boards a model name). Always shown: when SMBIOS can't be read
	 * the row carries the reason, the way the Battery row does. */
	const char *host = smbios_host();
	const int host_rows = 1;
	const int batt_rows = battery_present() ? 1 : 0;  /* only when detected     */
	const int storage_rows = nsd > 0 ? nsd : 1;   /* always show one "Storage:" */
	const int display_rows = ndd > 0 ? ndd : 1;   /* always show one "Display:" */
	const int rows = fixed + host_rows + batt_rows + storage_rows + display_rows;
	for (int i = 0; i < rows; i++) {
		/* logo column (cyan), padded to a fixed width */
		vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
		const char *l = (i < LOGO_LINES) ? logo[i] : "";
		int w = 0;
		for (const char *p = l; *p; p++) {
			vga_putc(*p);
			w++;
		}
		for (; w < LOGO_WIDTH; w++)
			vga_putc(' ');

		vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
		if (i < fixed)
			info_row(i, brand, &t);
		else if (i < fixed + host_rows) {
			label("Host:    ");
			if (host[0]) {
				vga_write(host);
			} else {
				/* No DMI: show why, for on-hardware diagnosis. */
				const char *e = smbios_error();
				vga_write("unknown (");
				vga_write((e && e[0]) ? e : "no SMBIOS");
				vga_putc(')');
			}
		} else if (i < fixed + host_rows + batt_rows) {
			label("Battery: ");
			/* Evaluate _BST/_BIF for the real charge; fall back to bare
			 * presence if the AML can't be read (e.g. a silent EC). */
			int chg = 0, dis = 0;
			int pct = battery_percent(&chg, &dis);
			if (pct >= 0) {
				vga_write_dec((uint32_t)pct);
				vga_putc('%');
				if (chg)      vga_write(" (charging)");
				else if (dis) vga_write(" (discharging)");
			} else {
				/* Couldn't read _BST/_BIF; show why, for diagnosis. */
				const char *e = battery_error();
				vga_write("present (");
				vga_write((e && e[0]) ? e : "ACPI");
				vga_putc(')');
			}
		} else if (i < fixed + host_rows + batt_rows + storage_rows)
			storage_row(i - fixed - host_rows - batt_rows, sd, nsd);
		else
			display_row(i - fixed - host_rows - batt_rows - storage_rows, dd, ndd);
		vga_putc('\n');
	}
	vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}
