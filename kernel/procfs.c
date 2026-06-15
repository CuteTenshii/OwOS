/* procfs.c — a tiny read-only /proc: kernel introspection through the ordinary
 * file interface, one concern per file, generated on read.
 *
 * This replaces the old grab-bag SYS_SYSINFO / SYS_PCIDEV syscalls:
 * programs (neofetch, lspci) just readfile("/proc/<x>") like any other file, and
 * `cat /proc/uptime` works too. Each file is a single fact, never a bundle.
 *
 *   /proc/cpuinfo   model + MHz          /proc/host      SMBIOS product
 *   /proc/meminfo   used / total KiB     /proc/battery   present/pct/state
 *   /proc/uptime    seconds              /proc/display   framebuffer + EDID
 *   /proc/firmware  BIOS / UEFI          /proc/storage   one line per disk
 *   /proc/date      wall clock           /proc/owofs     file/byte usage
 *   /proc/tasks     scheduler task count /proc/pci       one line per PCI device
 */

#include <stdint.h>
#include "procfs.h"
#include "kstring.h"
#include "cpu.h"
#include "pmm.h"
#include "timer.h"
#include "rtc.h"
#include "fs.h"
#include "sched.h"
#include "firmware.h"
#include "smbios.h"
#include "battery.h"
#include "fb.h"
#include "edid.h"
#include "pci.h"
#include "ata.h"
#include "ahci.h"
#include "nvme.h"
#include "log.h"
#include "net.h"
#include "nic.h"
#include "vmtools.h"
#include "keyboard.h"
#include "mouse.h"
#include "power.h"
#include "services.h"
#include "xhci.h"
#include "usb.h"
#include "console.h"

/* bounded text builder */
struct pb { char *p; char *end; };
static void ps(struct pb *b, const char *s) { while (s && *s && b->p < b->end) *b->p++ = *s++; }
static void pc(struct pb *b, char c)        { if (b->p < b->end) *b->p++ = c; }
static void pu(struct pb *b, uint32_t v)
{
	char t[12]; int n = 0;
	if (!v) { pc(b, '0'); return; }
	while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
	while (n) pc(b, t[--n]);
}
static void p2(struct pb *b, uint32_t v) { pc(b, '0' + (char)(v / 10 % 10)); pc(b, '0' + (char)(v % 10)); }


/* /proc/cpuinfo */
static void gen_cpuinfo(struct pb *b)
{
	static uint32_t mhz;                    /* cpu_mhz() busy-waits ~200 ms     */
	char brand[52];
	cpu_brand(brand);
	if (!mhz) mhz = cpu_mhz();
	ps(b, "model "); ps(b, brand[0] ? brand : "unknown"); pc(b, '\n');
	ps(b, "mhz ");   pu(b, mhz); pc(b, '\n');
}

static void gen_meminfo(struct pb *b)
{
	ps(b, "used ");  pu(b, pmm_ram_used_kib());  pc(b, '\n');
	ps(b, "total "); pu(b, pmm_installed_kib()); pc(b, '\n');
}

static void gen_uptime(struct pb *b)  { pu(b, timer_seconds()); pc(b, '\n'); }
static void gen_firmware(struct pb *b){ ps(b, firmware_name()); pc(b, '\n'); }
static void gen_tasks(struct pb *b)   { pu(b, (uint32_t)sched_count()); pc(b, '\n'); }

static void gen_date(struct pb *b)
{
	struct rtc_time t;
	rtc_now(&t);
	pu(b, t.year); pc(b, '-'); p2(b, t.month); pc(b, '-'); p2(b, t.day);
	pc(b, ' '); p2(b, t.hour); pc(b, ':'); p2(b, t.min); pc(b, ':'); p2(b, t.sec);
	pc(b, '\n');
}

static void gen_owofs(struct pb *b)
{
	ps(b, "files "); pu(b, (uint32_t)fs_file_count()); pc(b, '\n');
	ps(b, "bytes "); pu(b, fs_used_bytes()); pc(b, '\n');
}

static void gen_host(struct pb *b)
{
	const char *h = smbios_host();
	ps(b, (h && h[0]) ? h : "");
	pc(b, '\n');
}

static void gen_battery(struct pb *b)
{
	int present = battery_present();
	ps(b, "present "); pc(b, present ? '1' : '0'); pc(b, '\n');
	if (present) {
		int chg = 0, dis = 0, mins = -1;
		int pct = battery_percent(&chg, &dis, &mins);
		ps(b, "pct ");         pu(b, pct < 0 ? 0 : (uint32_t)pct); pc(b, '\n');
		ps(b, "charging ");    pc(b, chg ? '1' : '0'); pc(b, '\n');
		ps(b, "discharging "); pc(b, dis ? '1' : '0'); pc(b, '\n');
		ps(b, "minutes ");     pu(b, mins < 0 ? 0 : (uint32_t)mins); pc(b, '\n');
	}
	/* Always include the detection diagnostic — this whole path is real-hardware
	 * only (QEMU has no battery), so `cat /proc/battery` is the way to see why a
	 * laptop battery isn't being detected. */
	ps(b, "diag "); ps(b, battery_diag()); pc(b, '\n');
}

static void gen_display(struct pb *b)
{
	int hz = edid_refresh_hz();
	if (hz <= 0) hz = fb_active() ? 60 : 70;
	ps(b, "fbw ");      pu(b, fb_active() ? fb_width() : 0);  pc(b, '\n');
	ps(b, "fbh ");      pu(b, fb_active() ? fb_height() : 0); pc(b, '\n');
	ps(b, "hz ");       pu(b, (uint32_t)hz); pc(b, '\n');
	char mon[48]; edid_model(mon, (int)sizeof mon);
	ps(b, "monitor ");  ps(b, mon); pc(b, '\n');
	ps(b, "internal "); pc(b, edid_internal() ? '1' : '0'); pc(b, '\n');
}

/* /proc/storage: one line per present disk -> "<type> <sectors> <model>" */
static void gen_storage(struct pb *b)
{
	if (ata_present())  { ps(b, "IDE/ATA ");   pu(b, ata_sectors());  pc(b, ' '); ps(b, ata_model());  pc(b, '\n'); }
	if (ahci_present()) { ps(b, "SATA/AHCI "); pu(b, ahci_sectors()); pc(b, ' '); ps(b, ahci_model()); pc(b, '\n'); }
	if (nvme_present()) { ps(b, "NVMe ");      pu(b, nvme_sectors()); pc(b, ' '); ps(b, nvme_model()); pc(b, '\n'); }
}

/* /proc/pci: "bus:slot.fn vendor:device class name" per device (lspci reads it;
 * neofetch filters class 03xx for the GPU list and counts the lines). */
static void hex2(struct pb *b, unsigned v) { static const char h[] = "0123456789abcdef"; pc(b, h[(v>>4)&0xF]); pc(b, h[v&0xF]); }
static void hex4(struct pb *b, unsigned v) { hex2(b, v >> 8); hex2(b, v); }
static void gen_pci(struct pb *b)
{
	struct pci_dev_info d;
	for (int i = 0; pci_dev_info(i, &d) > 0; i++) {
		hex2(b, d.bus); pc(b, ':'); hex2(b, d.slot); pc(b, '.'); pc(b, "0123456789abcdef"[d.func & 0xF]);
		pc(b, ' '); hex4(b, d.vendor); pc(b, ':'); hex4(b, d.device);
		pc(b, ' '); hex2(b, d.class_code); hex2(b, d.subclass);
		pc(b, ' '); ps(b, d.name); pc(b, '\n');
	}
}

static void gen_journal(struct pb *b)
{
	b->p += journal_copy(b->p, (int)(b->end - b->p));
}

/* /proc/net: link + IPv4 status. Writing to it re-runs DHCP or an NTP sync (see
 * procfs_write), so `dhcp`/`ntp` are a write-then-read of this one node. */
static void gen_net(struct pb *b)
{
	const uint8_t *ip = net_my_ip();
	ps(b, "ip ");
	if (ip) { pu(b, ip[0]); pc(b, '.'); pu(b, ip[1]); pc(b, '.'); pu(b, ip[2]); pc(b, '.'); pu(b, ip[3]); }
	else    ps(b, "0.0.0.0");
	pc(b, '\n');
	ps(b, "link "); ps(b, nic_present() ? "up" : "down"); pc(b, '\n');
}

static void gen_clipboard(struct pb *b)
{
	b->p += vmtools_clip_get(b->p, (int)(b->end - b->p));
}

static void gen_keyboard(struct pb *b)
{
	ps(b, keyboard_layout_name()); pc(b, '\n');
}

/* /proc/mouse: the pointer-speed level (1..5). Writing a level re-scales PS/2
 * motion (see procfs_write) — the Settings app's Mouse/Touchpad control. */
static void gen_mouse(struct pb *b)
{
	pu(b, (uint32_t)mouse_speed()); pc(b, '\n');
}

/* services and usb only know how to print to the console, so capture that into
 * the /proc buffer. con_capture is global but procfs_read is called
 * synchronously by the reader, so the window is tiny. */
static void gen_services(struct pb *b)
{
	con_capture_begin(b->p, (uint32_t)(b->end - b->p));
	services_cmd("status");
	b->p += con_capture_end();
}

static void gen_usb(struct pb *b)
{
	con_capture_begin(b->p, (uint32_t)(b->end - b->p));
	usb_list(-1);
	b->p += con_capture_end();
}

/* The entry names, in listing order. Single source of truth for read + list. */
static const char *const entries[] = {
	"cpuinfo", "meminfo", "uptime", "firmware", "tasks", "date",
	"owofs", "host", "battery", "display", "storage", "pci",
	"journal", "net", "clipboard", "keyboard", "mouse",
	"services", "usb",
};
#define NENTRIES ((int)(sizeof entries / sizeof entries[0]))

/* List the idx-th /proc entry name into `name`; 1 if it exists, 0 past the end. */
int procfs_list(int idx, char *name, int max)
{
	if (idx < 0 || idx >= NENTRIES)
		return 0;
	const char *s = entries[idx];
	int i = 0;
	for (; s[i] && i < max - 1; i++) name[i] = s[i];
	name[i] = '\0';
	return 1;
}

/* Byte length a /proc file would produce (generate into scratch + measure), so
 * stat/ls report a real size and the files behave like ordinary ones. */
int procfs_size(const char *sub)
{
	static char scratch[8192];
	int n = procfs_read(sub, scratch, (int)sizeof scratch);
	return n < 0 ? 0 : n;
}

/* 1 if `sub` ("" or "/name") names the /proc dir, 0 a known file, -1 unknown. */
int procfs_kind(const char *sub)
{
	if (sub[0] == '/') sub++;
	if (!sub[0]) return 1;                 /* /proc itself                      */
	for (int i = 0; i < NENTRIES; i++)
		if (streq(sub, entries[i])) return 0;
	return -1;
}

/* Path is the part after "/proc" (e.g. "/cpuinfo"). Returns bytes written, or
 * -1 for an unknown entry. */
int procfs_read(const char *sub, char *buf, int max)
{
	/* Some generators wait on the timer (cpu_mhz calibration) or ACPI; the
	 * int 0x80 gate left IF=0, so enable interrupts or they'd spin forever. */
	__asm__ volatile ("sti");
	if (sub[0] == '/') sub++;
	struct pb b = { buf, buf + max };

	if      (streq(sub, "cpuinfo"))  gen_cpuinfo(&b);
	else if (streq(sub, "meminfo"))  gen_meminfo(&b);
	else if (streq(sub, "uptime"))   gen_uptime(&b);
	else if (streq(sub, "firmware")) gen_firmware(&b);
	else if (streq(sub, "tasks"))    gen_tasks(&b);
	else if (streq(sub, "date"))     gen_date(&b);
	else if (streq(sub, "owofs"))    gen_owofs(&b);
	else if (streq(sub, "host"))     gen_host(&b);
	else if (streq(sub, "battery"))  gen_battery(&b);
	else if (streq(sub, "display"))  gen_display(&b);
	else if (streq(sub, "storage"))  gen_storage(&b);
	else if (streq(sub, "pci"))      gen_pci(&b);
	else if (streq(sub, "journal"))  gen_journal(&b);
	else if (streq(sub, "net"))      gen_net(&b);
	else if (streq(sub, "clipboard")) gen_clipboard(&b);
	else if (streq(sub, "keyboard")) gen_keyboard(&b);
	else if (streq(sub, "mouse"))    gen_mouse(&b);
	else if (streq(sub, "services")) gen_services(&b);
	else if (streq(sub, "usb"))      gen_usb(&b);
	else                             return -1;

	return (int)(b.p - buf);
}

/* The write side of /proc: control nodes through the ordinary file interface, so
 * the shell triggers kernel actions with writefile() and never needs a
 * per-action syscall or a "run a kernel command" bridge. Returns len on success
 * (so writefile reports the bytes written), -1 for an unknown/!writable node. */
int procfs_write(const char *sub, const char *buf, int len)
{
	__asm__ volatile ("sti");                 /* dhcp/ntp wait on the timer/IRQs   */
	if (sub[0] == '/') sub++;

	/* The clipboard is the one node that carries arbitrary text (a whole editor
	 * selection -- multi-line, embedded newlines, far over the 255-byte command
	 * cap below), so it takes the raw payload verbatim; vmtools_clip_set bounds
	 * it to the clipboard capacity and mirrors it to the VMware host. */
	if (streq(sub, "clipboard")) {
		vmtools_clip_set(buf, len);
		return len;
	}

	/* Trim a trailing newline and bound the payload into a NUL-terminated copy. */
	char arg[256];
	int n = len;
	if (n > (int)sizeof arg - 1) n = (int)sizeof arg - 1;
	for (int i = 0; i < n; i++) arg[i] = buf[i];
	arg[n] = '\0';
	while (n > 0 && (arg[n - 1] == '\n' || arg[n - 1] == '\r')) arg[--n] = '\0';

	if (streq(sub, "keyboard"))
		return keyboard_set_layout(arg) == 0 ? len : -1;
	if (streq(sub, "mouse")) {               /* pointer-speed level "1".."5"      */
		int v = 0;
		for (const char *p = arg; *p >= '0' && *p <= '9'; p++)
			v = v * 10 + (*p - '0');
		mouse_set_speed(v);
		return len;
	}
	if (streq(sub, "services")) {            /* "start X" / "stop X" / "restart X" */
		services_cmd(arg);
		return len;
	}
	return -1;
}

/* The part of `path` after a leading "/proc", or NULL if it isn't under /proc.
 * "/proc" -> "", "/proc/cpuinfo" -> "/cpuinfo". */
const char *procfs_subpath(const char *path)
{
	if (!path) return 0;
	while (*path == '/') path++;
	const char *q = "proc";
	while (*q && *path == *q) { path++; q++; }
	if (*q) return 0;                     /* didn't match "proc"               */
	if (*path && *path != '/') return 0;  /* "procfoo" is not /proc            */
	return path;                          /* "" or "/cpuinfo"                  */
}
