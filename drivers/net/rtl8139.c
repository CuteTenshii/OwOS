/* rtl8139.c — RealTek RTL8139 NIC driver.
 *
 * Owns the hardware only: PCI probe, the 8 KiB receive ring, four transmit
 * descriptors, and the receive interrupt. Inbound frames go up to the stack
 * through net_rx_frame(); the stack sends complete Ethernet frames back down
 * through nic_send(). Transmit briefly clears IF so the card's own receive
 * interrupt can't grab the same TX descriptor mid-send.
 */

#include <stdint.h>

#include "rtl8139.h"
#include "kstring.h"
#include "net.h"
#include "io.h"
#include "pci.h"
#include "irq.h"
#include "heap.h"
#include "console.h"

/* ------------------------------------------------------------------ helpers */

/* irq_save / irq_restore: snapshot EFLAGS + cli, restore the prior IF. */
#include "irqflags.h"

static void print_hex2(uint8_t v)
{
	static const char h[] = "0123456789abcdef";
	con_printf("%c%c", h[v >> 4], h[v & 0xF]);
}

/* -------------------------------------------------------------- RTL8139 regs */

#define RTL_VENDOR  0x10EC
#define RTL_DEVICE  0x8139

#define REG_IDR0    0x00   /* MAC address, 6 bytes                            */
#define REG_TSD0    0x10   /* transmit status/command, 4 descriptors x 4B     */
#define REG_TSAD0   0x20   /* transmit start address (phys), 4 x 4B           */
#define REG_RBSTART 0x30   /* receive buffer start (phys)                     */
#define REG_CR      0x37   /* command register                               */
#define REG_CAPR    0x38   /* current address of packet read                 */
#define REG_IMR     0x3C   /* interrupt mask                                  */
#define REG_ISR     0x3E   /* interrupt status                               */
#define REG_RCR     0x44   /* receive configuration                          */
#define REG_CONFIG1 0x52

#define CR_RST      0x10
#define CR_RE       0x08
#define CR_TE       0x04
#define CR_BUFE     0x01   /* receive buffer empty (1 = nothing to read)      */

#define ISR_ROK     0x0001
#define ISR_TOK     0x0004
#define TSD_TOK     0x8000 /* transmit OK                                     */

#define RX_BUF_SIZE  8192          /* the wrap modulus (RBLEN = 8K)           */
#define RX_BUF_ALLOC (RX_BUF_SIZE + 16 + 1536) /* + header pad + WRAP slack   */
#define TX_BUFS      4

/* ------------------------------------------------------------------- state */

static int      nic_found;
static uint16_t io_base;
static uint8_t  mac[6];

static uint8_t *rx_buf;
static uint32_t rx_cur;             /* read offset into the 8K ring           */

static uint8_t *tx_buf[TX_BUFS];
static int      tx_cur;

/* ----------------------------------------------------------------- exported */

static const uint8_t *rtl8139_mac(void)
{
	return mac;
}

static void rtl8139_send(const uint8_t *frame, uint32_t len)
{
	if (!nic_found || len > 1514)
		return;

	uint32_t fl = irq_save();

	int i = tx_cur;
	uint8_t *b = tx_buf[i];
	memcpy(b, frame, len);
	while (len < 60)                /* pad runt frames to the 60-byte minimum  */
		b[len++] = 0;

	outl(io_base + REG_TSAD0 + i * 4, (uint32_t)b);
	outl(io_base + REG_TSD0  + i * 4, len);   /* OWN=0 starts transmission     */

	/* Wait for the card to report the frame out (bounded spin). */
	for (int spins = 0; spins < 2000000; spins++)
		if (inl(io_base + REG_TSD0 + i * 4) & TSD_TOK)
			break;

	tx_cur = (i + 1) % TX_BUFS;
	irq_restore(fl);
}

/* -------------------------------------------------------------- receive path */

/* Drain every complete frame sitting in the RX ring. Called from the IRQ. */
static void rx_poll(void)
{
	while (!(inb(io_base + REG_CR) & CR_BUFE)) {
		const uint8_t *p = rx_buf + rx_cur;
		uint16_t status = (uint16_t)(p[0] | (p[1] << 8));
		uint16_t len    = (uint16_t)(p[2] | (p[3] << 8));

		/* Bound to a real (VLAN-tagged) Ethernet frame. The old 2000 cap
		 * exceeded the ring's WRAP slack, so an oversized DMA'd length read
		 * past the buffer allocation. */
		if (len < 14 || len > 1522 || !(status & 0x01))
			break;                  /* runt / oversize / error / DMA-in-progress */

		net_rx_frame(p + 4, (uint32_t)len - 4);   /* drop the 4-byte FCS */

		rx_cur = (rx_cur + len + 4 + 3) & ~3u;
		rx_cur %= RX_BUF_SIZE;
		outw(io_base + REG_CAPR, (uint16_t)(rx_cur - 16));
	}
}

static void rtl_irq(struct registers *r)
{
	(void)r;
	uint16_t st = inw(io_base + REG_ISR);
	outw(io_base + REG_ISR, st);            /* acknowledge                     */
	if (st & ISR_ROK)
		rx_poll();
}

/* --------------------------------------------------------------------- init */

static int rtl8139_probe(void)
{
	int found = 0;
	uint8_t bus = 0, slot = 0, func = 0;

	for (int b = 0; b < 256 && !found; b++) {
		for (int s = 0; s < 32 && !found; s++) {
			if ((pci_read32((uint8_t)b, (uint8_t)s, 0, 0x00) & 0xFFFF) == 0xFFFF)
				continue;
			for (int f = 0; f < 8; f++) {
				uint32_t id = pci_read32((uint8_t)b, (uint8_t)s, (uint8_t)f, 0x00);
				if ((id & 0xFFFF) != RTL_VENDOR ||
				    ((id >> 16) & 0xFFFF) != RTL_DEVICE)
					continue;
				bus = (uint8_t)b; slot = (uint8_t)s; func = (uint8_t)f;
				found = 1;
				break;
			}
		}
	}
	if (!found)
		return 0;               /* silent: the dispatch layer reports "no NIC" */

	uint32_t bar0 = pci_read32(bus, slot, func, 0x10);
	uint8_t  irq  = (uint8_t)(pci_read32(bus, slot, func, 0x3C) & 0xFF);
	io_base = (uint16_t)(bar0 & 0xFFFC);

	/* Enable I/O space and bus-mastering DMA in the PCI command register. */
	uint32_t cmd = pci_read32(bus, slot, func, 0x04);
	pci_write32(bus, slot, func, 0x04, cmd | 0x05);

	rx_buf = kmalloc(RX_BUF_ALLOC);
	for (int i = 0; i < TX_BUFS; i++)
		tx_buf[i] = kmalloc(2048);
	if (!rx_buf) {
		con_write("net: RX buffer allocation failed\n");
		return 0;
	}
	for (int i = 0; i < TX_BUFS; i++) {
		if (!tx_buf[i]) {
			con_write("net: TX buffer allocation failed\n");
			return 0;
		}
	}

	outb(io_base + REG_CONFIG1, 0x00);      /* power on                        */
	outb(io_base + REG_CR, CR_RST);         /* software reset                  */
	for (int spins = 0; spins < 1000000; spins++)
		if (!(inb(io_base + REG_CR) & CR_RST))
			break;

	outl(io_base + REG_RBSTART, (uint32_t)rx_buf);
	rx_cur = 0;
	outw(io_base + REG_CAPR, 0xFFF0);       /* read pointer = -16              */
	outw(io_base + REG_IMR, ISR_ROK | ISR_TOK);
	/* RCR: accept broadcast/multicast/phys-match/all, WRAP, max DMA burst,
	 * no RX FIFO threshold (whole frame), 8K+16 ring. */
	outl(io_base + REG_RCR, 0x0000000F | (1 << 7) | (7 << 8) | (7 << 13));
	outb(io_base + REG_CR, CR_RE | CR_TE);  /* enable receive + transmit       */

	for (int i = 0; i < 6; i++)
		mac[i] = inb(io_base + REG_IDR0 + i);

	irq_register(irq, rtl_irq);
	irq_unmask(irq);
	if (irq >= 8)
		irq_unmask(2);                  /* cascade line for the slave PIC  */

	nic_found = 1;

	con_printf("net: RTL8139 at io 0x%08X irq %u mac ", io_base, irq);
	for (int i = 0; i < 6; i++) {
		print_hex2(mac[i]);
		if (i < 5)
			con_putc(':');
	}
	con_putc('\n');
	return 1;
}

const struct nic_ops rtl8139_ops = {
	.name  = "RTL8139",
	.probe = rtl8139_probe,
	.send  = rtl8139_send,
	.mac   = rtl8139_mac,
};
