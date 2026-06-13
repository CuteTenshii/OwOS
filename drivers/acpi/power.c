/* power.c — shutdown and reboot.
 *
 * There is no firmware abstraction here, so both paths poke hardware directly.
 * Shutdown first tries real ACPI (the only method VMware and bare metal honour)
 * and falls back to the "magic" I/O ports the emulators watch for; reboot pulses
 * the CPU reset line through the legacy 8042 keyboard controller, and
 * triple-faults as a last resort. */

#include <stdint.h>
#include "io.h"
#include "power.h"
#include "paging.h"
#include "irq.h"
#include "timer.h"

/* ---- ACPI shutdown ------------------------------------------------------ *
 * The low 16 MiB is identity-mapped, and firmware parks the ACPI tables well
 * below that, so we can treat their physical addresses as plain pointers.
 * We find the FADT, pull the _S5 sleep-type value out of the DSDT's AML, then
 * write SLP_TYPa|SLP_EN into the PM1 control register(s) — the standard
 * software-off sequence that works on VMware and real hardware alike. */

struct rsdp {
	char     signature[8];          /* "RSD PTR " */
	uint8_t  checksum;
	char     oemid[6];
	uint8_t  revision;              /* 0 = ACPI 1.0 (RSDT only); >=2 adds XSDT */
	uint32_t rsdt_addr;
	/* ACPI 2.0+ extension, present when revision >= 2. UEFI firmware often sets
	 * rsdt_addr to 0 and only fills in this 64-bit XSDT pointer. */
	uint32_t length;
	uint64_t xsdt_addr;
	uint8_t  ext_checksum;
	uint8_t  reserved[3];
} __attribute__((packed));

struct sdt_header {
	char     signature[4];
	uint32_t length;
	uint8_t  revision;
	uint8_t  checksum;
	char     oemid[6];
	char     oem_table_id[8];
	uint32_t oem_revision;
	uint32_t creator_id;
	uint32_t creator_revision;
} __attribute__((packed));

/* FADT — only the prefix up to the PM1 control blocks; offsets per ACPI spec. */
struct fadt {
	struct sdt_header header;       /* 0  */
	uint32_t firmware_ctrl;         /* 36 */
	uint32_t dsdt;                  /* 40 */
	uint8_t  reserved0;             /* 44 */
	uint8_t  preferred_pm_profile;  /* 45 */
	uint16_t sci_int;               /* 46 */
	uint32_t smi_cmd;               /* 48 */
	uint8_t  acpi_enable;           /* 52 */
	uint8_t  acpi_disable;          /* 53 */
	uint8_t  s4bios_req;            /* 54 */
	uint8_t  pstate_cnt;            /* 55 */
	uint32_t pm1a_evt_blk;          /* 56 */
	uint32_t pm1b_evt_blk;          /* 60 */
	uint32_t pm1a_cnt_blk;          /* 64 */
	uint32_t pm1b_cnt_blk;          /* 68 */
} __attribute__((packed));

#define SLP_EN  (1 << 13)           /* PM1_CNT sleep-enable bit */
#define SCI_EN  (1 << 0)            /* PM1a_CNT: ACPI mode is on */

static int sig4(const char *a, const char *b)
{
	return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

/* Scan a physical range, 16-byte aligned, for the RSDP anchor string. */
static struct rsdp *scan_rsdp(uint32_t start, uint32_t end)
{
	for (uint32_t p = start; p < end; p += 16) {
		struct rsdp *r = (struct rsdp *)p;
		if (r->signature[0] == 'R' && r->signature[1] == 'S' &&
		    r->signature[2] == 'D' && r->signature[3] == ' ' &&
		    r->signature[4] == 'P' && r->signature[5] == 'T' &&
		    r->signature[6] == 'R' && r->signature[7] == ' ')
			return r;
	}
	return 0;
}

/* A copy of the RSDP the bootloader handed us via the multiboot2 ACPI tag.
 * On a UEFI boot the firmware leaves no RSDP in the legacy BIOS area, so the
 * scan below finds nothing and this copy is the only way to reach ACPI. */
static struct rsdp boot_rsdp;
static int boot_rsdp_valid;

void acpi_set_rsdp(const void *rsdp_copy)
{
	const struct rsdp *src = rsdp_copy;
	/* "RSD PTR " is the anchor; ignore anything that isn't really an RSDP. */
	if (!(src->signature[0] == 'R' && src->signature[1] == 'S' &&
	      src->signature[2] == 'D' && src->signature[3] == ' '))
		return;
	/* GRUB may hand us both an ACPI-1.0 (type 14) and a 2.0 (type 15) tag.
	 * Never let the 1.0 one clobber a 2.0 RSDP already captured — only the 2.0
	 * RSDP carries the XSDT that UEFI firmware actually populates. */
	if (boot_rsdp_valid && boot_rsdp.revision >= 2 && src->revision < 2)
		return;

	const uint8_t *s = rsdp_copy;
	uint8_t *d = (uint8_t *)&boot_rsdp;
	/* A 2.0 RSDP is 36 bytes (adds length + the 64-bit XSDT pointer); a 1.0 one
	 * is only 20, so don't read the extension fields out of a smaller tag. */
	uint32_t len = src->revision >= 2 ? sizeof boot_rsdp : 20;
	for (uint32_t i = 0; i < len; i++)
		d[i] = s[i];
	/* Reachable if it names a root table — the RSDT (1.0) or XSDT (2.0+). */
	boot_rsdp_valid = boot_rsdp.rsdt_addr != 0 ||
	                  (boot_rsdp.revision >= 2 && boot_rsdp.xsdt_addr != 0);
}

static struct rsdp *find_rsdp(void)
{
	/* Prefer the loader-provided copy: on UEFI it's the only one that exists,
	 * and on a legacy boot it's simply absent (boot_rsdp_valid == 0). */
	if (boot_rsdp_valid)
		return &boot_rsdp;

	/* The EBDA segment pointer lives at physical 0x40E (real-mode word). */
	uint32_t ebda = (uint32_t)(*(uint16_t *)0x40E) << 4;
	struct rsdp *r = 0;
	if (ebda)
		r = scan_rsdp(ebda, ebda + 1024);
	if (!r)
		r = scan_rsdp(0xE0000, 0x100000);
	return r;
}

/* Map and validate the root system description table: the XSDT (8-byte entry
 * pointers) on ACPI 2.0+ when the firmware populated it, otherwise the RSDT
 * (4-byte). Returns 0 on failure; on success *hdr is the mapped header, *count
 * the number of table pointers, and *xsdt is 1 for 8-byte entries. */
static int find_root_sdt(struct sdt_header **hdr, uint32_t *count, int *xsdt)
{
	struct rsdp *rsdp = find_rsdp();
	if (!rsdp)
		return 0;

	uint32_t phys;
	int is_xsdt;
	if (rsdp->revision >= 2 && rsdp->xsdt_addr &&
	    rsdp->xsdt_addr < 0x100000000ull) {
		phys = (uint32_t)rsdp->xsdt_addr;
		is_xsdt = 1;
	} else {
		phys = rsdp->rsdt_addr;
		is_xsdt = 0;
	}
	if (!phys)
		return 0;

	struct sdt_header *h = (struct sdt_header *)phys;
	if (paging_identity_map(phys, sizeof(*h)) != 0)
		return 0;
	if (!sig4(h->signature, is_xsdt ? "XSDT" : "RSDT"))
		return 0;
	if (paging_identity_map(phys, h->length) != 0)
		return 0;

	*hdr = h;
	*count = (h->length - sizeof(*h)) / (is_xsdt ? 8 : 4);
	*xsdt = is_xsdt;
	return 1;
}

/* Physical address of the i-th table the root SDT points at. RSDT entries are
 * 4 bytes, XSDT entries 8; either way ACPI tables live below 4 GiB, so the
 * truncation to 32 bits is safe (we only manage the low 4 GiB anyway). */
static uint32_t root_entry(struct sdt_header *root, uint32_t i, int xsdt)
{
	uint8_t *base = (uint8_t *)root + sizeof(struct sdt_header);
	if (xsdt)
		return (uint32_t)(*(uint64_t *)(base + (uint32_t)i * 8));
	return *(uint32_t *)(base + (uint32_t)i * 4);
}

/* The DSDT's physical address from the FADT. Prefer the 64-bit X_DSDT (offset
 * 140, ACPI 2.0+): UEFI firmware routinely leaves the legacy 32-bit dsdt field
 * zero. Falls back to that field. The caller must have mapped the full FADT. */
static uint32_t fadt_dsdt_phys(struct fadt *f)
{
	if (f->header.length >= 148) {
		uint64_t x = *(uint64_t *)((uint8_t *)f + 140);
		if (x && x < 0x100000000ull)
			return (uint32_t)x;
	}
	return f->dsdt;
}

/* Dig SLP_TYPa/SLP_TYPb out of one of the DSDT's sleep-state packages, named by
 * `name` ("_S5_" for off, "_S3_" for suspend-to-RAM). Returns 0 on failure; on
 * success the values are written *already shifted* into PM1_CNT position. */
static int parse_sleep(struct fadt *f, const char *name,
                       uint16_t *slp_a, uint16_t *slp_b)
{
	uint32_t dphys = fadt_dsdt_phys(f);
	struct sdt_header *dsdt = (struct sdt_header *)dphys;
	/* Map the header, then the whole table once its length is known. */
	if (paging_identity_map(dphys, sizeof(*dsdt)) != 0)
		return 0;
	if (!dsdt || !sig4(dsdt->signature, "DSDT"))
		return 0;
	if (paging_identity_map(dphys, dsdt->length) != 0)
		return 0;

	uint8_t *p   = (uint8_t *)dsdt + sizeof(*dsdt);
	uint8_t *end = (uint8_t *)dsdt + dsdt->length;
	for (; p < end - 4; p++) {
		if (sig4((char *)p, name))
			break;
	}
	if (p >= end - 4)
		return 0;

	/* Validate it's a real NameOp(name, Package): the name is preceded by an
	 * AML NameOp (0x08), optionally under the root scope ('\\' == 0x5C), and
	 * followed by a PackageOp (0x12). */
	if (!((*(p - 1) == 0x08) ||
	      (*(p - 2) == 0x08 && *(p - 1) == 0x5C)) || *(p + 4) != 0x12)
		return 0;

	p += 5;                          /* skip the name and the PackageOp byte */
	p += ((*p & 0xC0) >> 6) + 2;     /* skip PkgLength + element count       */

	if (*p == 0x0A)                  /* BytePrefix before SLP_TYPa           */
		p++;
	*slp_a = (uint16_t)(*p++) << 10;

	if (*p == 0x0A)                  /* BytePrefix before SLP_TYPb           */
		p++;
	*slp_b = (uint16_t)(*p) << 10;
	return 1;
}

/* Locate and identity-map the FADT (its ACPI signature is "FACP"). Returns 0 if
 * ACPI isn't present. Walks whichever root table the RSDP names (XSDT or RSDT).
 * Maps the *whole* FADT, not just the prefix, so fadt_dsdt_phys() can read the
 * 64-bit X_DSDT at offset 140. Shared by the shutdown path and acpi_aml_contains. */
static struct fadt *find_fadt(void)
{
	struct sdt_header *root;
	uint32_t count;
	int xsdt;
	if (!find_root_sdt(&root, &count, &xsdt))
		return 0;

	for (uint32_t i = 0; i < count; i++) {
		uint32_t phys = root_entry(root, i, xsdt);
		if (paging_identity_map(phys, sizeof(struct sdt_header)) != 0)
			continue;
		struct sdt_header *h = (struct sdt_header *)phys;
		if (sig4(h->signature, "FACP")) {
			if (paging_identity_map(phys, h->length) != 0)
				return 0;
			return (struct fadt *)h;
		}
	}
	return 0;
}

/* Map one already-located SDT (whose signature is sig) and scan its AML body for
 * a byte sequence. Returns 1 if found. The header must be reachable; we map the
 * full table once its length is known. A garbage length can't run away: it's a
 * table's own self-reported size and we only ever read inside the mapped span. */
static int sdt_contains(struct sdt_header *h, const char *sig,
                        const uint8_t *needle, uint32_t n)
{
	if (!h || paging_identity_map((uint32_t)h, sizeof(*h)) != 0)
		return 0;
	if (!sig4(h->signature, sig) || h->length <= sizeof(*h) || n == 0)
		return 0;
	if (paging_identity_map((uint32_t)h, h->length) != 0)
		return 0;

	const uint8_t *body = (const uint8_t *)h + sizeof(*h);
	uint32_t len = h->length - sizeof(*h);
	if (len < n)
		return 0;
	for (uint32_t i = 0; i + n <= len; i++) {
		uint32_t j = 0;
		while (j < n && body[i + j] == needle[j])
			j++;
		if (j == n)
			return 1;
	}
	return 0;
}

/* Scan the device AML for a byte sequence (e.g. an EISA _HID), looking in both
 * the DSDT and every SSDT. Returns 1 if found. Firmware may declare a device —
 * the battery especially — in an SSDT rather than the DSDT, so battery.c must
 * search both. Returns 0 when ACPI isn't reachable (e.g. RSDP not in the legacy
 * BIOS area, as on a pure-UEFI boot). */
int acpi_aml_contains(const uint8_t *needle, uint32_t n)
{
	struct sdt_header *root;
	uint32_t count;
	int xsdt;
	if (!find_root_sdt(&root, &count, &xsdt))
		return 0;

	/* The DSDT hangs off the FADT, not the root table, so reach it separately
	 * (and via the 64-bit X_DSDT when the firmware uses it). */
	struct fadt *f = find_fadt();
	if (f && sdt_contains((struct sdt_header *)fadt_dsdt_phys(f), "DSDT", needle, n))
		return 1;

	/* Every other table is a direct root-table entry; scan the SSDTs. */
	for (uint32_t i = 0; i < count; i++) {
		uint32_t phys = root_entry(root, i, xsdt);
		if (paging_identity_map(phys, sizeof(struct sdt_header)) != 0)
			continue;
		if (sdt_contains((struct sdt_header *)phys, "SSDT", needle, n))
			return 1;
	}
	return 0;
}

/* Hand each AML definition block (DSDT, then every SSDT) to fn as a (body, len)
 * pair past the 36-byte table header. Maps each table fully first. The AML
 * interpreter uses this to build its namespace without duplicating the table
 * walk. Mirrors acpi_aml_contains' traversal. */
void acpi_for_each_aml(void (*fn)(const uint8_t *body, uint32_t len, void *ctx),
                       void *ctx)
{
	struct sdt_header *root;
	uint32_t count;
	int xsdt;
	if (!find_root_sdt(&root, &count, &xsdt))
		return;

	struct fadt *f = find_fadt();
	if (f) {
		uint32_t dphys = fadt_dsdt_phys(f);
		struct sdt_header *d = (struct sdt_header *)dphys;
		if (paging_identity_map(dphys, sizeof(*d)) == 0 &&
		    sig4(d->signature, "DSDT") &&
		    paging_identity_map(dphys, d->length) == 0 &&
		    d->length > sizeof(*d))
			fn((const uint8_t *)d + sizeof(*d), d->length - sizeof(*d), ctx);
	}

	for (uint32_t i = 0; i < count; i++) {
		uint32_t phys = root_entry(root, i, xsdt);
		struct sdt_header *h = (struct sdt_header *)phys;
		if (paging_identity_map(phys, sizeof(*h)) != 0)
			continue;
		if (!sig4(h->signature, "SSDT"))
			continue;
		if (paging_identity_map(phys, h->length) != 0)
			continue;
		if (h->length > sizeof(*h))
			fn((const uint8_t *)h + sizeof(*h), h->length - sizeof(*h), ctx);
	}
}

/* Switch the chipset into ACPI mode if it isn't already, so the PM1 control
 * register is live. (QEMU boots in ACPI mode; VMware/real HW may not.) */
static void acpi_ensure_enabled(struct fadt *f)
{
	if (f->smi_cmd && f->acpi_enable &&
	    !(inw((uint16_t)f->pm1a_cnt_blk) & SCI_EN)) {
		outb((uint16_t)f->smi_cmd, f->acpi_enable);
		for (int i = 0; i < 300000; i++) {
			if (inw((uint16_t)f->pm1a_cnt_blk) & SCI_EN)
				break;
			io_wait();
		}
	}
}

/* Returns only if ACPI shutdown could not be performed. */
static void acpi_off(void)
{
	struct fadt *f = find_fadt();
	if (!f)
		return;

	uint16_t slp_a, slp_b;
	if (!parse_sleep(f, "_S5_", &slp_a, &slp_b))
		return;

	acpi_ensure_enabled(f);

	outw((uint16_t)f->pm1a_cnt_blk, slp_a | SLP_EN);
	if (f->pm1b_cnt_blk)
		outw((uint16_t)f->pm1b_cnt_blk, slp_b | SLP_EN);
}

void power_off(void)
{
	/* Real ACPI first — the only path VMware and bare metal honour. */
	acpi_off();

	/* Fallback: the value/port pairs emulators listen on for "power off".
	 * QEMU >= 2.0 uses 0x604; older QEMU and Bochs use 0xB004; VirtualBox
	 * uses 0x4004. Try each — the live one shuts us down and never returns. */
	outw(0x604, 0x2000);    /* QEMU            */
	outw(0xB004, 0x2000);   /* Bochs / old QEMU */
	outw(0x4004, 0x3400);   /* VirtualBox      */
}

/* ---- ACPI reset register (FADT) ---------------------------------------- *
 * ACPI 2.0+ lets the firmware name the exact register and value that reboot
 * the platform: a Generic Address Structure (RESET_REG) at FADT offset 116
 * and a one-byte RESET_VALUE at offset 128, valid only when FADT flags bit 10
 * (RESET_REG_SUP) is set. This is the spec-blessed reboot and the method most
 * UEFI laptops actually honour — where the legacy 8042 pulse and a triple
 * fault below often do nothing (the firmware/SMM swallows the fault and the
 * box just hangs, which is exactly what "restart froze the OS" looks like).
 * Returns if it couldn't issue the write; the caller falls through. */
static void acpi_reset(void)
{
	struct fadt *f = find_fadt();
	if (!f || f->header.length < 129)
		return;

	uint32_t flags = *(uint32_t *)((uint8_t *)f + 112);
	if (!(flags & (1u << 10)))      /* RESET_REG_SUP */
		return;

	/* GAS layout: [0]=address-space id, [1..3]=width/offset/access, [4..11]=addr. */
	uint8_t *gas   = (uint8_t *)f + 116;
	uint8_t  space = gas[0];        /* 0 = system memory, 1 = system I/O */
	uint64_t addr  = *(uint64_t *)(gas + 4);
	uint8_t  value = *((uint8_t *)f + 128);
	if (!addr)
		return;

	if (space == 1) {               /* system I/O space (the common case) */
		outb((uint16_t)addr, value);
	} else if (space == 0) {        /* memory-mapped reset register */
		if (addr < 0x100000000ull &&
		    paging_identity_map((uint32_t)addr, 1) == 0)
			*(volatile uint8_t *)(uint32_t)addr = value;
	}
}

void power_reset(void)
{
	/* Firmware-declared ACPI reset first — the most reliable path on real
	 * (especially UEFI) hardware. Returns only if it couldn't be issued. */
	acpi_reset();

	/* The PCI reset-control register at port 0xCF9, present on essentially
	 * every ICH/PCH-based x86 machine. Bit 2 (RST_CPU) triggers the reset and
	 * bit 1 (SYS_RST) selects a full reset; assert them in the Linux-blessed
	 * two-write order. Harmless on a box that lacks it. */
	{
		uint8_t v = inb(0xCF9) & ~0x06u;
		outb(0xCF9, v | 0x02);   /* arm: SYS_RST */
		io_wait();
		outb(0xCF9, v | 0x06);   /* fire: SYS_RST | RST_CPU */
		io_wait();
	}

	/* Pulse the reset line via the 8042: wait until the controller's input
	 * buffer is empty (status bit 1 clear), then issue command 0xFE.
	 *
	 * The wait MUST be bounded. On real hardware with no 8042 (many laptops,
	 * pure-UEFI machines) port 0x64 reads back 0xFF on the open bus, so
	 * (status & 0x02) is permanently set and an unbounded loop hangs the
	 * machine forever — which looks exactly like "restart froze the OS".
	 * Spin a finite number of times, then issue 0xFE and fall through to the
	 * triple fault below regardless: if the controller is alive the pulse
	 * resets us, and if it isn't the triple fault does. QEMU's 8042 drains
	 * immediately so this still resets cleanly there. */
	for (int i = 0; i < 100000 && (inb(0x64) & 0x02); i++)
		io_wait();
	outb(0x64, 0xFE);

	/* If that did nothing, force a triple fault: a null IDT means the next
	 * interrupt has nowhere to go, and the CPU resets. */
	struct {
		uint16_t limit;
		uint32_t base;
	} __attribute__((packed)) null_idt = { 0, 0 };
	__asm__ volatile ("lidt %0; int3" : : "m"(null_idt));
}

/* ---- ACPI S3 sleep (suspend-to-RAM) ------------------------------------- *
 * S3 cuts power to the CPU but keeps RAM refreshed. The OS saves CPU state,
 * tells the firmware where to re-enter on wake (the FACS firmware_waking_vector
 * — a real-mode address), then writes the S3 sleep type to PM1_CNT. On wake the
 * firmware bounces through our trampoline (s3_wake.S) back into the kernel.
 *
 * NOTE: this restores the *CPU*, not devices. Under QEMU, S3 is a pause so the
 * emulated devices keep their state and the desktop comes back intact. On real
 * hardware the chipset/devices lose power across S3 and would need re-init on
 * resume (NIC, USB, the panel's backlight/timing) — not done here yet. */

/* The trampoline blob and its physical landing address (must agree with the
 * .set TRAMP in s3_wake.S). 0x8000 is free conventional RAM, below the EBDA,
 * and inside the identity-mapped low 16 MiB so we can write it directly. */
#define S3_TRAMPOLINE_PHYS 0x8000u
extern uint8_t s3_trampoline_start[];
extern uint8_t s3_trampoline_end[];
extern void s3_enter(uint32_t pm1a_port, uint32_t pm1b_port,
                     uint32_t slp_a_value, uint32_t slp_b_value);

/* The FACS hangs off the FADT (32-bit FIRMWARE_CTRL at offset 36, or the 64-bit
 * X_FIRMWARE_CTRL at offset 132 on ACPI 2.0+). Map and validate it; returns the
 * mapped base or 0. This is the firmware's own FACS — the same one the BIOS
 * reads on wake — so writing firmware_waking_vector here is what gets us back. */
static uint8_t *find_facs(struct fadt *f)
{
	uint32_t phys = f->firmware_ctrl;
	if (f->header.length >= 140) {
		uint64_t x = *(uint64_t *)((uint8_t *)f + 132);
		if (x && x < 0x100000000ull)
			phys = (uint32_t)x;
	}
	if (!phys || paging_identity_map(phys, 64) != 0)
		return 0;
	uint8_t *facs = (uint8_t *)phys;
	if (!sig4((char *)facs, "FACS"))
		return 0;
	return facs;
}

/* Is ACPI S3 (suspend-to-RAM) actually available? Read-only probe: the FADT
 * must exist, the DSDT must advertise an `_S3` package, and the FACS (where we
 * stash the wake vector) must be reachable. Used to gate the Start-menu "Sleep"
 * item — VMware, for one, doesn't expose S3 to the guest, so offering it would
 * just be a dead button. Safe to call any time after paging is up. */
int power_can_sleep(void)
{
	struct fadt *f = find_fadt();
	if (!f)
		return 0;
	uint16_t a, b;
	if (!parse_sleep(f, "_S3_", &a, &b))
		return 0;
	return find_facs(f) != 0;
}

/* Enter ACPI S3. Returns (after resume, or immediately) if S3 can't be done. */
void power_sleep(void)
{
	struct fadt *f = find_fadt();
	if (!f)
		return;

	uint16_t slp_a, slp_b;
	if (!parse_sleep(f, "_S3_", &slp_a, &slp_b))
		return;                  /* firmware doesn't advertise S3 */

	uint8_t *facs = find_facs(f);
	if (!facs)
		return;

	acpi_ensure_enabled(f);

	/* Copy the wake trampoline into low memory and point the firmware at it.
	 * firmware_waking_vector is the 32-bit (real-mode) entry at FACS offset 12;
	 * clear the 64-bit x_firmware_waking_vector (offset 24) so the firmware
	 * uses the 32-bit one. */
	uint32_t len = (uint32_t)(s3_trampoline_end - s3_trampoline_start);
	uint8_t *dst = (uint8_t *)S3_TRAMPOLINE_PHYS;
	for (uint32_t i = 0; i < len; i++)
		dst[i] = s3_trampoline_start[i];

	*(volatile uint32_t *)(facs + 12) = S3_TRAMPOLINE_PHYS;
	if (facs[32] >= 1)               /* FACS version >= 1 has the 64-bit field */
		*(volatile uint64_t *)(facs + 24) = 0;

	/* Make sure the trampoline + saved state are in RAM before power drops. */
	__asm__ volatile ("wbinvd");

	/* Snapshot the PIC line masks: the firmware's wake POST resets the 8259, so
	 * we reinstate exactly what was unmasked before sleeping. */
	uint8_t pic_mask1 = inb(0x21);
	uint8_t pic_mask2 = inb(0xA1);

	/* Hand off to asm: it snapshots the CPU and writes SLP_TYP|SLP_EN. Control
	 * returns here once the firmware re-enters us through the trampoline. */
	s3_enter(f->pm1a_cnt_blk, f->pm1b_cnt_blk,
	         (uint32_t)(slp_a | SLP_EN), (uint32_t)(slp_b | SLP_EN));

	/* ---- resumed ---- The S3 wake reset left interrupts off and the chipset
	 * timers/PIC reprogrammed by firmware. Re-establish them with interrupts
	 * masked so a stray tick can't land on a BIOS vector mid-repair, then let
	 * the restored EFLAGS' IF take over. */
	__asm__ volatile ("cli");
	irq_resume(pic_mask1, pic_mask2);   /* re-map PIC, restore masks */
	timer_init();                       /* re-program the PIT tick   */
	__asm__ volatile ("sti");
}
