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
	uint8_t  revision;
	uint32_t rsdt_addr;
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

static struct rsdp *find_rsdp(void)
{
	/* The EBDA segment pointer lives at physical 0x40E (real-mode word). */
	uint32_t ebda = (uint32_t)(*(uint16_t *)0x40E) << 4;
	struct rsdp *r = 0;
	if (ebda)
		r = scan_rsdp(ebda, ebda + 1024);
	if (!r)
		r = scan_rsdp(0xE0000, 0x100000);
	return r;
}

/* Dig SLP_TYPa/SLP_TYPb out of the DSDT's \_S5_ package. Returns 0 on failure;
 * on success the values are written *already shifted* into PM1_CNT position. */
static int parse_s5(struct fadt *f, uint16_t *slp_a, uint16_t *slp_b)
{
	struct sdt_header *dsdt = (struct sdt_header *)f->dsdt;
	/* Map the header, then the whole table once its length is known. */
	if (paging_identity_map(f->dsdt, sizeof(*dsdt)) != 0)
		return 0;
	if (!dsdt || !sig4(dsdt->signature, "DSDT"))
		return 0;
	if (paging_identity_map(f->dsdt, dsdt->length) != 0)
		return 0;

	uint8_t *p   = (uint8_t *)dsdt + sizeof(*dsdt);
	uint8_t *end = (uint8_t *)dsdt + dsdt->length;
	for (; p < end - 4; p++) {
		if (sig4((char *)p, "_S5_"))
			break;
	}
	if (p >= end - 4)
		return 0;

	/* Validate it's a real NameOp("_S5_", Package): the name is preceded by an
	 * AML NameOp (0x08), optionally under the root scope ('\\' == 0x5C), and
	 * followed by a PackageOp (0x12). */
	if (!((*(p - 1) == 0x08) ||
	      (*(p - 2) == 0x08 && *(p - 1) == 0x5C)) || *(p + 4) != 0x12)
		return 0;

	p += 5;                          /* skip "_S5_" and the PackageOp byte */
	p += ((*p & 0xC0) >> 6) + 2;     /* skip PkgLength + element count       */

	if (*p == 0x0A)                  /* BytePrefix before SLP_TYPa           */
		p++;
	*slp_a = (uint16_t)(*p++) << 10;

	if (*p == 0x0A)                  /* BytePrefix before SLP_TYPb           */
		p++;
	*slp_b = (uint16_t)(*p) << 10;
	return 1;
}

/* Returns only if ACPI shutdown could not be performed. */
static void acpi_off(void)
{
	struct rsdp *rsdp = find_rsdp();
	if (!rsdp)
		return;

	/* The RSDT (and the tables it points to) usually sit near the top of RAM,
	 * above the 16 MiB identity map, so map each before dereferencing it. */
	struct sdt_header *rsdt = (struct sdt_header *)rsdp->rsdt_addr;
	if (paging_identity_map(rsdp->rsdt_addr, sizeof(*rsdt)) != 0)
		return;
	if (!rsdt || !sig4(rsdt->signature, "RSDT"))
		return;
	if (paging_identity_map(rsdp->rsdt_addr, rsdt->length) != 0)
		return;

	/* Walk the RSDT's array of 32-bit table pointers looking for the FADT. */
	uint32_t count = (rsdt->length - sizeof(*rsdt)) / 4;
	uint32_t *entry = (uint32_t *)((uint8_t *)rsdt + sizeof(*rsdt));
	struct fadt *f = 0;
	for (uint32_t i = 0; i < count; i++) {
		struct sdt_header *h = (struct sdt_header *)entry[i];
		if (paging_identity_map(entry[i], sizeof(struct fadt)) != 0)
			continue;
		if (h && sig4(h->signature, "FACP")) {  /* FADT's signature is "FACP" */
			f = (struct fadt *)h;
			break;
		}
	}
	if (!f)
		return;

	uint16_t slp_a, slp_b;
	if (!parse_s5(f, &slp_a, &slp_b))
		return;

	/* Switch the chipset into ACPI mode if it isn't already, so the PM1
	 * control register is live. (QEMU boots in ACPI mode; VMware/HW may not.) */
	if (f->smi_cmd && f->acpi_enable &&
	    !(inw((uint16_t)f->pm1a_cnt_blk) & SCI_EN)) {
		outb((uint16_t)f->smi_cmd, f->acpi_enable);
		for (int i = 0; i < 300000; i++) {
			if (inw((uint16_t)f->pm1a_cnt_blk) & SCI_EN)
				break;
			io_wait();
		}
	}

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

void power_reset(void)
{
	/* Pulse the reset line via the 8042: wait until the controller's input
	 * buffer is empty (status bit 1 clear), then issue command 0xFE. */
	uint8_t status;
	do {
		status = inb(0x64);
	} while (status & 0x02);
	outb(0x64, 0xFE);

	/* If that did nothing, force a triple fault: a null IDT means the next
	 * interrupt has nowhere to go, and the CPU resets. */
	struct {
		uint16_t limit;
		uint32_t base;
	} __attribute__((packed)) null_idt = { 0, 0 };
	__asm__ volatile ("lidt %0; int3" : : "m"(null_idt));
}
