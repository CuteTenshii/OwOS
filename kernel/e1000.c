/* e1000.c — Intel Gigabit Ethernet driver (8254x / 82574 / I21x family).
 *
 * Drives the Intel MAC over its memory-mapped register file using the legacy
 * descriptor model: a ring of receive descriptors the card DMAs inbound frames
 * into, and a ring of transmit descriptors we hand outbound frames to. Inbound
 * frames go up via net_rx_frame(); the stack sends frames down via the nic_ops
 * .send hook. One driver covers several chips because they share this legacy
 * path: QEMU's 82540EM (-device e1000) and 82574L (-device e1000e), VMware's
 * default 82545EM, plus the real I217/I218/I219 parts on modern Intel laptops.
 *
 * Two things differ from rtl8139.c (which is port-I/O and Realtek-specific):
 *   - The register file is MMIO above the 16 MiB identity map, so we map BAR0
 *     uncached (paging_identity_map + paging_mark_uc) before touching it.
 *   - The MAC is read from the receive-address registers RAL/RAH[0], which both
 *     QEMU and platform firmware pre-load. That avoids the per-chip EEPROM
 *     (EERD) layout differences entirely.
 *
 * The descriptor rings live in normal write-back RAM (palloc frames): x86 DMA
 * is cache-coherent, so the card's writes to a descriptor are snooped and the
 * CPU sees fresh status without an uncached mapping. Only the registers are UC.
 */

#include <stdint.h>
#include <stddef.h>

#include "e1000.h"
#include "net.h"
#include "pci.h"
#include "irq.h"
#include "heap.h"
#include "paging.h"
#include "vga.h"

/* ------------------------------------------------------------------ helpers */

static void kmemcpy(void *dst, const void *src, uint32_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;
	while (n--)
		*d++ = *s++;
}

/* Save EFLAGS and clear IF; restore only re-enables if it was on. */
static inline uint32_t irq_save(void)
{
	uint32_t f;
	__asm__ volatile ("pushf\n\tpop %0\n\tcli" : "=r"(f) :: "memory");
	return f;
}
static inline void irq_restore(uint32_t f)
{
	if (f & 0x200)
		__asm__ volatile ("sti" ::: "memory");
}

static void print_hex2(uint8_t v)
{
	static const char h[] = "0123456789abcdef";
	vga_putc(h[v >> 4]);
	vga_putc(h[v & 0xF]);
}

/* ----------------------------------------------------------- register file */

#define E1000_CTRL    0x0000   /* device control                              */
#define E1000_STATUS  0x0008   /* device status (link up, speed)              */
#define E1000_EERD    0x0014   /* EEPROM read (MAC fallback)                  */
#define E1000_ICR     0x00C0   /* interrupt cause read (read clears)          */
#define E1000_IMS     0x00D0   /* interrupt mask set                          */
#define E1000_IMC     0x00D8   /* interrupt mask clear                        */
#define E1000_RCTL    0x0100   /* receive control                             */
#define E1000_TCTL    0x0400   /* transmit control                            */
#define E1000_TIPG    0x0410   /* transmit inter-packet gap                   */
#define E1000_RDBAL   0x2800   /* RX descriptor base (low)                    */
#define E1000_RDBAH   0x2804   /* RX descriptor base (high)                   */
#define E1000_RDLEN   0x2808   /* RX descriptor ring length (bytes)           */
#define E1000_RDH     0x2810   /* RX descriptor head                          */
#define E1000_RDT     0x2818   /* RX descriptor tail                          */
#define E1000_TDBAL   0x3800   /* TX descriptor base (low)                    */
#define E1000_TDBAH   0x3804   /* TX descriptor base (high)                   */
#define E1000_TDLEN   0x3808   /* TX descriptor ring length (bytes)           */
#define E1000_TDH     0x3810   /* TX descriptor head                          */
#define E1000_TDT     0x3818   /* TX descriptor tail                          */
#define E1000_MTA     0x5200   /* multicast table array (128 dwords)          */
#define E1000_RAL     0x5400   /* receive address low [0]                     */
#define E1000_RAH     0x5404   /* receive address high [0] (+ Address Valid)  */

/* CTRL bits */
#define CTRL_SLU      (1u << 6)    /* set link up                             */
#define CTRL_ILOS     (1u << 7)    /* invert loss-of-signal (clear)           */
#define CTRL_RST      (1u << 26)   /* device reset                            */
#define CTRL_PHY_RST  (1u << 31)   /* PHY reset (clear)                       */
#define CTRL_LRST     (1u << 3)    /* link reset (clear)                      */

#define RAH_AV        (1u << 31)   /* receive address valid                   */

/* RCTL bits — BSIZE=00 with BSEX=0 gives a 2048-byte receive buffer. */
#define RCTL_EN       (1u << 1)    /* receiver enable                         */
#define RCTL_BAM      (1u << 15)   /* accept broadcast                        */
#define RCTL_SECRC    (1u << 26)   /* strip Ethernet CRC (FCS) on receive     */

/* TCTL bits */
#define TCTL_EN       (1u << 1)    /* transmitter enable                      */
#define TCTL_PSP      (1u << 3)    /* pad short packets to 60 bytes           */
#define TCTL_CT       (0x10u << 4) /* collision threshold (recommended 0x10)  */
#define TCTL_COLD     (0x40u << 12)/* collision distance, full duplex (0x40)  */

/* interrupt cause / mask bits */
#define INT_LSC       (1u << 2)    /* link status change                      */
#define INT_RXDMT0    (1u << 4)    /* RX descriptor min threshold             */
#define INT_RXO       (1u << 6)    /* receiver overrun                        */
#define INT_RXT0      (1u << 7)    /* receiver timer (a frame arrived)        */

/* descriptor status / command bits */
#define RXD_DD        (1u << 0)    /* descriptor done                         */
#define RXD_EOP       (1u << 1)    /* end of packet                           */
#define TXD_DD        (1u << 0)    /* descriptor done (written back)          */
#define TXD_CMD_EOP   (1u << 0)    /* end of packet                           */
#define TXD_CMD_IFCS  (1u << 1)    /* insert FCS                              */
#define TXD_CMD_RS    (1u << 3)    /* report status (set DD when done)        */

/* Legacy descriptor formats (16 bytes each). */
struct rx_desc {
	uint64_t addr;
	uint16_t length;
	uint16_t checksum;
	uint8_t  status;
	uint8_t  errors;
	uint16_t special;
} __attribute__((packed));

struct tx_desc {
	uint64_t addr;
	uint16_t length;
	uint8_t  cso;
	uint8_t  cmd;
	uint8_t  status;
	uint8_t  css;
	uint16_t special;
} __attribute__((packed));

#define RX_DESCS  32              /* 32 * 16 = 512 bytes (a multiple of 128)   */
#define TX_DESCS  16              /* 16 * 16 = 256 bytes (a multiple of 128)   */
#define BUF_SIZE  2048

/* ------------------------------------------------------------------- state */

#define INTEL_VENDOR 0x8086

/* The Intel parts this driver recognizes. They all drive the legacy descriptor
 * ring the same way; the family span is: 82540EM/82574L (QEMU), then the
 * physical I217/I218/I219 MACs found on Haswell-era and later Intel laptops. */
static const uint16_t intel_nics[] = {
	0x100E,                         /* 82540EM         (QEMU -device e1000)   */
	0x100F,                         /* 82545EM         (VMware default e1000) */
	0x10D3,                         /* 82574L          (QEMU -device e1000e)  */
	0x153A, 0x153B,                 /* I217-LM / I217-V                       */
	0x15A0, 0x15A1, 0x15A2, 0x15A3, /* I218-LM / I218-V variants              */
	0x156F, 0x1570,                 /* I219-LM / I219-V (Skylake)             */
	0x15B7, 0x15B8, 0x15B9,         /* I219 (Skylake / Kaby Lake)             */
	0x15D6, 0x15D7, 0x15D8,         /* I219-V / I219-LM                       */
	0x15E3,                         /* I219-LM                                */
};

static volatile uint8_t *mmio;     /* BAR0, mapped uncached                   */
static uint8_t  mac[6];
static int      present;
static uint16_t device_id;

static struct rx_desc *rx_ring;    /* RX_DESCS entries, page-aligned          */
static struct tx_desc *tx_ring;    /* TX_DESCS entries, page-aligned          */
static uint8_t *rx_bufs[RX_DESCS];
static uint8_t *tx_bufs[TX_DESCS];
static uint32_t rx_cur, tx_cur;

/* MMIO accessors. The mapping is UC, so plain volatile loads/stores are not
 * cached or reordered against each other. */
static inline void reg_write(uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(mmio + off) = val;
}
static inline uint32_t reg_read(uint32_t off)
{
	return *(volatile uint32_t *)(mmio + off);
}

/* ----------------------------------------------------------------- MAC read */

/* EEPROM word read, used only if RAL/RAH was not pre-loaded. The done-bit and
 * address shift differ across chips, so try the 8254x layout then the 82574
 * one. Returns 0 and the word on success. */
static int eeprom_read(uint16_t addr, uint16_t *out)
{
	reg_write(E1000_EERD, ((uint32_t)addr << 8) | 1u);   /* 8254x: addr<<8     */
	for (int i = 0; i < 100000; i++) {
		uint32_t v = reg_read(E1000_EERD);
		if (v & (1u << 4)) { *out = (uint16_t)(v >> 16); return 0; }
	}
	reg_write(E1000_EERD, ((uint32_t)addr << 2) | 1u);   /* 82574: addr<<2     */
	for (int i = 0; i < 100000; i++) {
		uint32_t v = reg_read(E1000_EERD);
		if (v & (1u << 1)) { *out = (uint16_t)(v >> 16); return 0; }
	}
	return -1;
}

static void read_mac(void)
{
	uint32_t ral = reg_read(E1000_RAL);
	uint32_t rah = reg_read(E1000_RAH);

	if (rah & RAH_AV) {            /* firmware/QEMU already loaded the address  */
		mac[0] = ral & 0xFF;
		mac[1] = (ral >> 8) & 0xFF;
		mac[2] = (ral >> 16) & 0xFF;
		mac[3] = (ral >> 24) & 0xFF;
		mac[4] = rah & 0xFF;
		mac[5] = (rah >> 8) & 0xFF;
		return;
	}

	/* Fallback: words 0..2 of the EEPROM hold the MAC, little-endian. */
	for (int i = 0; i < 3; i++) {
		uint16_t w = 0;
		eeprom_read((uint16_t)i, &w);
		mac[i * 2]     = w & 0xFF;
		mac[i * 2 + 1] = (w >> 8) & 0xFF;
	}
}

/* --------------------------------------------------------------- ring setup */

static int setup_rx(void)
{
	rx_ring = palloc();           /* zeroed, page-aligned, identity-mapped     */
	if (!rx_ring)
		return -1;
	for (int i = 0; i < RX_DESCS; i++) {
		rx_bufs[i] = kmalloc(BUF_SIZE);
		if (!rx_bufs[i])
			return -1;
		rx_ring[i].addr   = (uint32_t)rx_bufs[i];
		rx_ring[i].status = 0;
	}

	reg_write(E1000_RDBAL, (uint32_t)rx_ring);
	reg_write(E1000_RDBAH, 0);
	reg_write(E1000_RDLEN, RX_DESCS * sizeof(struct rx_desc));
	reg_write(E1000_RDH, 0);
	reg_write(E1000_RDT, RX_DESCS - 1);   /* all but one descriptor are the card's */
	rx_cur = 0;
	reg_write(E1000_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);
	return 0;
}

static int setup_tx(void)
{
	tx_ring = palloc();
	if (!tx_ring)
		return -1;
	for (int i = 0; i < TX_DESCS; i++) {
		tx_bufs[i] = kmalloc(BUF_SIZE);
		if (!tx_bufs[i])
			return -1;
		tx_ring[i].addr   = (uint32_t)tx_bufs[i];
		tx_ring[i].status = 0;
	}

	reg_write(E1000_TDBAL, (uint32_t)tx_ring);
	reg_write(E1000_TDBAH, 0);
	reg_write(E1000_TDLEN, TX_DESCS * sizeof(struct tx_desc));
	reg_write(E1000_TDH, 0);
	reg_write(E1000_TDT, 0);
	tx_cur = 0;
	reg_write(E1000_TIPG, 0x0060200A);    /* IPGT=10, IPGR1=8, IPGR2=6 (copper) */
	reg_write(E1000_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD);
	return 0;
}

/* --------------------------------------------------------------- transmit */

static void e1000_send(const uint8_t *frame, uint32_t len)
{
	if (!present || len > 1514)
		return;

	uint32_t fl = irq_save();

	uint32_t i = tx_cur;
	kmemcpy(tx_bufs[i], frame, len);
	tx_ring[i].addr   = (uint32_t)tx_bufs[i];
	tx_ring[i].length = (uint16_t)len;    /* PSP pads runts to 60 in hardware   */
	tx_ring[i].cmd    = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;
	tx_ring[i].status = 0;

	tx_cur = (i + 1) % TX_DESCS;
	reg_write(E1000_TDT, tx_cur);

	/* Wait for the card to write back DD (bounded spin). */
	for (int spins = 0; spins < 1000000; spins++)
		if (tx_ring[i].status & TXD_DD)
			break;

	irq_restore(fl);
}

/* ---------------------------------------------------------------- receive */

/* Drain every complete frame the card has DMA'd in. Called from the IRQ. */
static void rx_poll(void)
{
	while (rx_ring[rx_cur].status & RXD_DD) {
		struct rx_desc *d = &rx_ring[rx_cur];
		uint16_t len = d->length;

		if ((d->status & RXD_EOP) && d->errors == 0 && len >= 14)
			net_rx_frame(rx_bufs[rx_cur], len);   /* SECRC already dropped FCS  */

		d->status = 0;
		uint32_t done = rx_cur;
		rx_cur = (rx_cur + 1) % RX_DESCS;
		reg_write(E1000_RDT, done);   /* hand the emptied descriptor back       */
	}
}

static void e1000_irq(struct registers *r)
{
	(void)r;
	uint32_t icr = reg_read(E1000_ICR);   /* reading the cause register clears it */
	if (icr & (INT_RXT0 | INT_RXDMT0 | INT_RXO))
		rx_poll();
}

/* IRQ-independent RX drain, called off the timer tick (see nic_poll). Some hosts
 * (VMware) route this card's INTx to a line our 8259 path never sees, so the RX
 * interrupt never fires and frames just pile up in the ring; polling drains them
 * anyway. IF-guarded so it can't race the real e1000_irq over rx_cur/RDT. The
 * descriptor ring is in cached RAM, but x86 DMA is cache-coherent, so the DD bit
 * the card writes is visible to a plain read. */
static void e1000_poll(void)
{
	if (!present)
		return;
	uint32_t fl = irq_save();
	rx_poll();
	irq_restore(fl);
}

/* --------------------------------------------------------------------- init */

static const uint8_t *e1000_mac(void)
{
	return mac;
}

static const char *model_name(uint16_t dev)
{
	switch (dev) {
	case 0x100E: return "82540EM";
	case 0x100F: return "82545EM";
	case 0x10D3: return "82574L";
	case 0x153A: case 0x153B: return "I217";
	case 0x15A0: case 0x15A1: case 0x15A2: case 0x15A3: return "I218";
	default:     return "I219";
	}
}

/* I217/I218/I219 are PCH-integrated MACs: the MAC and PHY sit behind the
 * chipset's Management Engine, and the CSR window is dead until a LANPHYPC pulse
 * + ME-handshake bring-up powers it on. We don't implement that sequence, and on
 * bare metal the *first* MMIO read of a not-yet-powered CSR window doesn't fault
 * — it never completes, hanging the CPU on the bus cycle (a frozen boot with no
 * panic). So we recognize these parts but refuse to touch their MMIO. The QEMU
 * parts (82540EM/82574L) and the VMware 82545EM are real PCI(e) devices whose
 * CSRs are live immediately, so they're driven normally. */
static int is_pch_lan(uint16_t dev)
{
	return dev != 0x100E && dev != 0x100F && dev != 0x10D3;
}

static int e1000_probe(void)
{
	int found = 0;
	uint8_t bus = 0, slot = 0, func = 0;

	for (int b = 0; b < 256 && !found; b++) {
		for (int s = 0; s < 32 && !found; s++) {
			if ((pci_read32((uint8_t)b, (uint8_t)s, 0, 0x00) & 0xFFFF) == 0xFFFF)
				continue;
			for (int f = 0; f < 8 && !found; f++) {
				uint32_t id = pci_read32((uint8_t)b, (uint8_t)s, (uint8_t)f, 0x00);
				if ((id & 0xFFFF) != INTEL_VENDOR)
					continue;
				uint16_t dev = (id >> 16) & 0xFFFF;
				for (unsigned k = 0; k < sizeof(intel_nics) / sizeof(intel_nics[0]); k++) {
					if (dev == intel_nics[k]) {
						bus = (uint8_t)b; slot = (uint8_t)s; func = (uint8_t)f;
						device_id = dev;
						found = 1;
						break;
					}
				}
			}
		}
	}
	if (!found)
		return 0;                /* silent: another driver may own the NIC      */

	/* Bail before any MMIO if this is a PCH-integrated MAC we can't safely wake
	 * — touching its CSR window would hang the CPU and freeze boot. */
	if (is_pch_lan(device_id)) {
		vga_write("net: Intel ");
		vga_write(model_name(device_id));
		vga_write(" NIC not supported on bare metal; skipping.\n");
		return 0;
	}

	uint32_t bar0 = pci_read32(bus, slot, func, 0x10);
	uint8_t  irq  = (uint8_t)(pci_read32(bus, slot, func, 0x3C) & 0xFF);
	uint32_t mmio_phys = bar0 & ~0xFu;    /* BAR0 is a memory BAR               */

	/* Enable memory-space decoding (bit 1) and bus-master DMA (bit 2). */
	uint32_t cmd = pci_read32(bus, slot, func, 0x04);
	pci_write32(bus, slot, func, 0x04, cmd | 0x06);

	/* Map the register file uncached. The e1000 register window is 128 KiB. */
	if (paging_identity_map(mmio_phys, 0x20000) != 0) {
		vga_write("e1000: MMIO map failed\n");
		return 0;
	}
	paging_mark_uc(mmio_phys, 0x20000);
	mmio = (volatile uint8_t *)mmio_phys;

	/* Reset: mask interrupts, pulse CTRL.RST, wait for it to self-clear. */
	reg_write(E1000_IMC, 0xFFFFFFFF);
	reg_write(E1000_CTRL, reg_read(E1000_CTRL) | CTRL_RST);
	for (int spins = 0; spins < 1000000; spins++)
		if (!(reg_read(E1000_CTRL) & CTRL_RST))
			break;
	reg_write(E1000_IMC, 0xFFFFFFFF);     /* reset re-arms some causes           */

	/* Bring the link up; clear the reset/loopback control bits. */
	uint32_t ctrl = reg_read(E1000_CTRL);
	ctrl |=  CTRL_SLU;
	ctrl &= ~(CTRL_LRST | CTRL_PHY_RST | CTRL_ILOS);
	reg_write(E1000_CTRL, ctrl);

	/* Clear the multicast table so stray hashes don't accept junk. */
	for (int i = 0; i < 128; i++)
		reg_write(E1000_MTA + i * 4, 0);

	read_mac();
	/* (Re)program the unicast filter from the MAC in case reset cleared it. */
	reg_write(E1000_RAL, mac[0] | mac[1] << 8 | mac[2] << 16 | (uint32_t)mac[3] << 24);
	reg_write(E1000_RAH, mac[4] | mac[5] << 8 | RAH_AV);

	if (setup_rx() != 0 || setup_tx() != 0) {
		vga_write("e1000: ring allocation failed\n");
		return 0;
	}

	/* Enable receive interrupts and clear any pending cause. */
	reg_write(E1000_IMS, INT_RXT0 | INT_RXDMT0 | INT_RXO | INT_LSC);
	reg_read(E1000_ICR);

	irq_register(irq, e1000_irq);
	irq_unmask(irq);
	if (irq >= 8)
		irq_unmask(2);            /* cascade line for the slave PIC              */

	present = 1;

	vga_write("net: Intel ");
	vga_write(model_name(device_id));
	vga_write(" at mmio ");
	vga_write_hex(mmio_phys);
	vga_write(" irq ");
	vga_write_dec(irq);
	vga_write(" mac ");
	for (int i = 0; i < 6; i++) {
		print_hex2(mac[i]);
		if (i < 5)
			vga_putc(':');
	}
	vga_putc('\n');
	return 1;
}

const struct nic_ops e1000_ops = {
	.name  = "e1000",
	.probe = e1000_probe,
	.send  = e1000_send,
	.mac   = e1000_mac,
	.poll  = e1000_poll,
};
