/* ahci.c — AHCI (SATA) host-bus-adapter driver, polled and single-port.
 *
 * Flow, mirroring how AHCI hardware actually works:
 *   1. Find the controller on PCI (class 0x01 / subclass 0x06) and read its
 *      MMIO register block (ABAR = BAR5). Identity-map it so the kernel can
 *      touch the registers directly (it lives above the 16 MiB identity map).
 *   2. Enable the controller (GHC.AE) and scan the implemented ports for one
 *      whose PHY is up and whose signature says "SATA disk".
 *   3. Give that port three DMA areas — a command list, a received-FIS area,
 *      and one command table — all page-aligned frames from the pool, which is
 *      identity-mapped so physical == virtual (the addresses the HBA needs).
 *   4. Per I/O: build a Register-Host-to-Device FIS carrying READ/WRITE DMA
 *      EXT, point a single PRDT entry at a bounce buffer, ring the doorbell
 *      (PxCI), and poll until the controller clears the bit.
 *
 * Polled, one sector at a time — same contract as ata.c, just over DMA. Every
 * wait is bounded so a wedged or absent drive returns -1 instead of hanging.
 *
 * Note: the MMIO mapping is cached (paging_identity_map has no uncached flag).
 * That is fine under QEMU; on real silicon AHCI registers want PCD set.
 */

#include "ahci.h"
#include "pci.h"
#include "paging.h"
#include "vga.h"

/* ------------------------------------------------------- HBA / port registers */

#define AHCI_CLASS    0x01
#define AHCI_SUBCLASS 0x06

/* Generic host control (byte offsets into ABAR). */
#define HBA_CAP   0x00
#define HBA_GHC   0x04
#define HBA_IS    0x08
#define HBA_PI    0x0C        /* ports implemented, one bit per port            */

#define GHC_AE    0x80000000u /* AHCI enable                                    */

/* Per-port register block: PORT_BASE(n) + register offset. */
#define PORT_BASE(n) (0x100 + (n) * 0x80)
#define PX_CLB    0x00        /* command-list base (1 KiB aligned)              */
#define PX_CLBU   0x04
#define PX_FB     0x08        /* received-FIS base (256 B aligned)              */
#define PX_FBU    0x0C
#define PX_IS     0x10
#define PX_IE     0x14
#define PX_CMD    0x18
#define PX_TFD    0x20        /* task-file data (ATA status/error)              */
#define PX_SIG    0x24        /* device signature                               */
#define PX_SSTS   0x28        /* SATA status (DET/IPM)                          */
#define PX_SERR   0x30
#define PX_CI     0x38        /* command issue (the doorbell)                   */

#define CMD_ST    0x0001      /* start                                          */
#define CMD_FRE   0x0010      /* FIS-receive enable                             */
#define CMD_FR    0x4000      /* FIS-receive running                            */
#define CMD_CR    0x8000      /* command-list running                           */

#define TFD_BSY   0x80
#define TFD_DRQ   0x08
#define IS_TFES   0x40000000u /* task-file error                                */

#define SIG_SATA  0x00000101u /* signature of a plain SATA disk                 */

#define ATA_READ_DMA_EX  0x25
#define ATA_WRITE_DMA_EX 0x35

#define POLL_LIMIT 5000000u

/* --------------------------------------------------------------------- state */

static volatile uint32_t *abar;   /* mapped MMIO register block                 */
static int      found;
static int      port;             /* the port we drive                          */
static uint32_t cap_sectors;
static char     model[41];        /* drive model from IDENTIFY                   */

/* DMA areas, each a page-aligned identity-mapped frame (phys == virt). */
static volatile uint32_t *clb;    /* command list (32 headers x 32 B)           */
static volatile uint8_t  *fis;    /* received-FIS area                          */
static volatile uint8_t  *ctab;   /* one command table (CFIS + PRDT)            */
static volatile uint8_t  *dbuf;   /* 512-byte DMA bounce buffer                 */

/* ----------------------------------------------------------------- MMIO + I/O */

static inline uint32_t hba_rd(uint32_t off)      { return abar[off / 4]; }
static inline void     hba_wr(uint32_t off, uint32_t v) { abar[off / 4] = v; }

static inline uint32_t port_rd(uint32_t reg)
{
	return abar[(PORT_BASE(port) + reg) / 4];
}
static inline void port_wr(uint32_t reg, uint32_t v)
{
	abar[(PORT_BASE(port) + reg) / 4] = v;
}

/* ----------------------------------------------------------- port start/stop */

static void port_stop(void)
{
	uint32_t cmd = port_rd(PX_CMD);
	cmd &= ~(CMD_ST | CMD_FRE);
	port_wr(PX_CMD, cmd);
	for (uint32_t i = 0; i < POLL_LIMIT; i++)
		if (!(port_rd(PX_CMD) & (CMD_FR | CMD_CR)))
			break;
}

static void port_start(void)
{
	for (uint32_t i = 0; i < POLL_LIMIT; i++)
		if (!(port_rd(PX_CMD) & CMD_CR))
			break;
	uint32_t cmd = port_rd(PX_CMD);
	port_wr(PX_CMD, cmd | CMD_FRE);
	port_wr(PX_CMD, port_rd(PX_CMD) | CMD_ST);
}

/* ------------------------------------------------------------- command issue */

/* Build a one-sector READ/WRITE DMA EXT command in slot 0 and ring PxCI.
 * Returns 0 on success, -1 on timeout or task-file error. */
static int issue(uint32_t lba, uint8_t ata_cmd, int write)
{
	port_wr(PX_SERR, 0xFFFFFFFFu);            /* clear sticky errors            */
	port_wr(PX_IS, 0xFFFFFFFFu);              /* clear port interrupt status    */

	/* Wait for the drive to be idle before handing it a command. */
	uint32_t i;
	for (i = 0; i < POLL_LIMIT; i++)
		if (!(port_rd(PX_TFD) & (TFD_BSY | TFD_DRQ)))
			break;
	if (i == POLL_LIMIT)
		return -1;

	/* Command header 0: 5-dword command FIS, one PRDT entry, W bit for writes. */
	clb[0] = 5 | (write ? (1u << 6) : 0) | (1u << 16);
	clb[1] = 0;                               /* PRDBC (filled by HBA)          */
	clb[2] = (uint32_t)ctab;                  /* command-table base (phys)      */
	clb[3] = 0;

	/* Command table: zero the CFIS + PRDT region we touch. */
	volatile uint8_t *ct = ctab;
	for (int b = 0; b < 0x80 + 0x10; b++)
		ct[b] = 0;

	/* Register Host-to-Device FIS carrying the ATA command. */
	ct[0]  = 0x27;                            /* FIS type: Reg H2D              */
	ct[1]  = 0x80;                            /* C=1: this is a command         */
	ct[2]  = ata_cmd;
	ct[4]  = lba & 0xFF;
	ct[5]  = (lba >> 8) & 0xFF;
	ct[6]  = (lba >> 16) & 0xFF;
	ct[7]  = 0x40;                            /* LBA mode                       */
	ct[8]  = (lba >> 24) & 0xFF;
	ct[12] = 1;                               /* sector count (low) = 1         */

	/* PRDT entry 0 (at offset 0x80): point at the bounce buffer, 512 bytes. */
	volatile uint32_t *prdt = (volatile uint32_t *)(ctab + 0x80);
	prdt[0] = (uint32_t)dbuf;                 /* data base (phys)               */
	prdt[1] = 0;
	prdt[2] = 0;
	prdt[3] = (512 - 1) & 0x3FFFFF;           /* byte count - 1, no IRQ         */

	port_wr(PX_CI, 1);                        /* ring the doorbell for slot 0   */

	for (i = 0; i < POLL_LIMIT; i++) {
		if (!(port_rd(PX_CI) & 1))
			break;
		if (port_rd(PX_IS) & IS_TFES)         /* task-file error                */
			return -1;
	}
	if (i == POLL_LIMIT)
		return -1;
	if (port_rd(PX_IS) & IS_TFES)
		return -1;
	return 0;
}

int ahci_read_sector(uint32_t lba, void *buf)
{
	if (!found)
		return -1;
	if (issue(lba, ATA_READ_DMA_EX, 0))
		return -1;
	uint8_t *d = buf;
	for (int i = 0; i < 512; i++)
		d[i] = dbuf[i];
	return 0;
}

int ahci_write_sector(uint32_t lba, const void *buf)
{
	if (!found)
		return -1;
	const uint8_t *s = buf;
	for (int i = 0; i < 512; i++)
		dbuf[i] = s[i];
	return issue(lba, ATA_WRITE_DMA_EX, 1);
}

int ahci_present(void)        { return found; }
uint32_t ahci_sectors(void)   { return cap_sectors; }
const char *ahci_model(void)  { return model; }

/* --------------------------------------------------------------------- probe */

/* Send IDENTIFY DEVICE (0xEC), then pull the 48-bit sector count (words
 * 100..103) and the model string (words 27..46) out of the 512-byte identify
 * block left in the DMA buffer. Best-effort: 0/"" means unknown. */
static uint32_t identify_drive(void)
{
	model[0] = '\0';
	if (issue(0, 0xEC, 0))
		return 0;
	const volatile uint16_t *id = (const volatile uint16_t *)dbuf;

	/* Model: 20 words, big-endian within each word; trim trailing spaces. */
	int n = 0;
	for (int i = 0; i < 20; i++) {
		model[n++] = (char)(id[27 + i] >> 8);
		model[n++] = (char)(id[27 + i] & 0xFF);
	}
	while (n > 0 && model[n - 1] == ' ')
		n--;
	model[n] = '\0';

	/* Words 100-103 = LBA48 sector count; low 32 bits is plenty here. */
	return (uint32_t)id[100] | ((uint32_t)id[101] << 16);
}

static int find_controller(uint8_t *bus, uint8_t *slot, uint8_t *func)
{
	for (int b = 0; b < 256; b++)
		for (int s = 0; s < 32; s++) {
			if ((pci_read32((uint8_t)b, (uint8_t)s, 0, 0x00) & 0xFFFF) == 0xFFFF)
				continue;
			for (int f = 0; f < 8; f++) {
				if ((pci_read32((uint8_t)b, (uint8_t)s, (uint8_t)f, 0x00) & 0xFFFF) == 0xFFFF)
					continue;
				uint32_t cls = pci_read32((uint8_t)b, (uint8_t)s, (uint8_t)f, 0x08);
				if (((cls >> 24) & 0xFF) == AHCI_CLASS &&
				    ((cls >> 16) & 0xFF) == AHCI_SUBCLASS) {
					*bus = (uint8_t)b; *slot = (uint8_t)s; *func = (uint8_t)f;
					return 1;
				}
			}
		}
	return 0;
}

void ahci_init(void)
{
	uint8_t bus, slot, func;
	if (!find_controller(&bus, &slot, &func)) {
		vga_write("ahci: no SATA controller found\n");
		return;
	}

	/* ABAR is BAR5 (config offset 0x24): the MMIO register block. */
	uint32_t bar5 = pci_read32(bus, slot, func, 0x24) & 0xFFFFFFF0u;
	if (!bar5) {
		vga_write("ahci: controller has no MMIO base\n");
		return;
	}

	/* Enable memory space + bus-mastering DMA in the PCI command register. */
	uint32_t cmd = pci_read32(bus, slot, func, 0x04);
	pci_write32(bus, slot, func, 0x04, cmd | 0x06);

	/* The register block sits above the 16 MiB identity map; map it in. */
	if (paging_identity_map(bar5, 0x2000) != 0) {
		vga_write("ahci: could not map MMIO\n");
		return;
	}
	abar = (volatile uint32_t *)bar5;

	hba_wr(HBA_GHC, hba_rd(HBA_GHC) | GHC_AE); /* take the HBA out of legacy mode */

	/* Find the first implemented port with a SATA disk whose PHY is up. */
	uint32_t pi = hba_rd(HBA_PI);
	int chosen = -1;
	for (int p = 0; p < 32; p++) {
		if (!(pi & (1u << p)))
			continue;
		port = p;
		uint32_t ssts = port_rd(PX_SSTS);
		uint8_t det = ssts & 0x0F;
		uint8_t ipm = (ssts >> 8) & 0x0F;
		if (det != 3 || ipm != 1)             /* not present / not active        */
			continue;
		if (port_rd(PX_SIG) != SIG_SATA)      /* skip ATAPI/other signatures     */
			continue;
		chosen = p;
		break;
	}
	if (chosen < 0) {
		vga_write("ahci: controller present but no SATA disk attached\n");
		return;
	}
	port = chosen;

	/* Carve the per-port DMA areas from the identity-mapped frame pool, so the
	 * physical addresses the HBA reads back are exactly these pointers. */
	clb  = palloc();
	fis  = palloc();
	ctab = palloc();
	dbuf = palloc();
	if (!clb || !fis || !ctab || !dbuf) {
		vga_write("ahci: out of DMA frames\n");
		return;
	}

	port_stop();
	port_wr(PX_CLB, (uint32_t)clb);
	port_wr(PX_CLBU, 0);
	port_wr(PX_FB, (uint32_t)fis);
	port_wr(PX_FBU, 0);
	port_wr(PX_SERR, 0xFFFFFFFFu);
	port_wr(PX_IS, 0xFFFFFFFFu);
	port_wr(PX_IE, 0);                         /* polled: no interrupts          */
	port_start();

	found = 1;
	cap_sectors = identify_drive();

	vga_write("ahci: SATA disk on port ");
	vga_write_dec((uint32_t)port);
	if (model[0]) {
		vga_write(" [");
		vga_write(model);
		vga_putc(']');
	}
	if (cap_sectors) {
		vga_write(", ");
		vga_write_dec(cap_sectors / 2048);    /* 512-B sectors -> MiB           */
		vga_write(" MiB");
	}
	vga_putc('\n');
}
