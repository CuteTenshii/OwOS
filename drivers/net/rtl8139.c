/* rtl8139.c: RealTek RTL8139 driver: PCI probe, 8 KiB RX ring, four TX descriptors, RX interrupt.
 * TX clears IF so the card's RX interrupt can't grab the same TX descriptor mid-send. */

#include <stdbool.h>
#include <stdint.h>

#include "console.h"
#include "heap.h"
#include "io.h"
#include "kstring.h"
#include "net.h"
#include "pci.h"
#include "rtl8139.h"

/* irq_save / irq_restore: snapshot EFLAGS + cli, restore the prior IF. */
#include "irqflags.h"

static void print_hex2(uint8_t v)
{
  static const char h[] = "0123456789abcdef";
  con_printf("%c%c", h[v >> 4], h[v & 0xF]);
}

#define RTL_VENDOR 0x10EC
#define RTL_DEVICE 0x8139

#define REG_IDR0 0x00
#define REG_TSD0 0x10
#define REG_TSAD0 0x20
#define REG_RBSTART 0x30
#define REG_CR 0x37
#define REG_CAPR 0x38
#define REG_IMR 0x3C
#define REG_ISR 0x3E
#define REG_RCR 0x44
#define REG_CONFIG1 0x52

#define CR_RST 0x10
#define CR_RE 0x08
#define CR_TE 0x04
#define CR_BUFE 0x01

#define ISR_ROK 0x0001
#define ISR_TOK 0x0004
#define TSD_TOK 0x8000

#define RX_BUF_SIZE 8192
#define RX_BUF_ALLOC (RX_BUF_SIZE + 16 + 1536)
#define TX_BUFS 4

static bool nic_found;
static uint16_t io_base;
static uint8_t mac[6];

static uint8_t *rx_buf;
static uint32_t rx_cur;

static uint8_t *tx_buf[TX_BUFS];
static int tx_cur;

static const uint8_t *rtl8139_mac(void) { return mac; }

static void rtl8139_send(const uint8_t *frame, uint32_t len)
{
  if (!nic_found || len > 1514)
    return;

  uint32_t fl = irq_save();

  int i = tx_cur;
  uint8_t *b = tx_buf[i];
  memcpy(b, frame, len);
  while (len < 60)
    b[len++] = 0;

  outl(io_base + REG_TSAD0 + i * 4, (uint32_t)(uintptr_t)b);
  outl(io_base + REG_TSD0 + i * 4, len);

  /* Wait for the card to report the frame out (bounded spin). */
  for (int spins = 0; spins < 2000000; spins++)
    if (inl(io_base + REG_TSD0 + i * 4) & TSD_TOK)
      break;

  tx_cur = (i + 1) % TX_BUFS;
  irq_restore(fl);
}

/* Drain every complete frame sitting in the RX ring. Called from the IRQ. */
static void rx_poll(void)
{
  while (!(inb(io_base + REG_CR) & CR_BUFE)) {
    const uint8_t *p = rx_buf + rx_cur;
    uint16_t status = (uint16_t)(p[0] | (p[1] << 8));
    uint16_t len = (uint16_t)(p[2] | (p[3] << 8));

    /* Bound to a real (VLAN-tagged) frame; a larger cap read past the buffer's WRAP slack. */
    if (len < 14 || len > 1522 || !(status & 0x01))
      break;

    net_rx_frame(p + 4, (uint32_t)len - 4);

    rx_cur = (rx_cur + len + 4 + 3) & ~3u;
    rx_cur %= RX_BUF_SIZE;
    outw(io_base + REG_CAPR, (uint16_t)(rx_cur - 16));
  }
}

static void rtl_poll(void)
{
  if (!nic_found)
    return;
  uint32_t fl = irq_save();
  rx_poll();
  irq_restore(fl);
}

static int rtl8139_probe(void)
{
  bool found = false;
  uint8_t bus = 0, slot = 0, func = 0;

  for (int b = 0; b < 256 && !found; b++) {
    for (int s = 0; s < 32 && !found; s++) {
      if ((pci_read32((uint8_t)b, (uint8_t)s, 0, 0x00) & 0xFFFF) == 0xFFFF)
        continue;
      for (int f = 0; f < 8; f++) {
        uint32_t id = pci_read32((uint8_t)b, (uint8_t)s, (uint8_t)f, 0x00);
        if ((id & 0xFFFF) != RTL_VENDOR || ((id >> 16) & 0xFFFF) != RTL_DEVICE)
          continue;
        bus = (uint8_t)b;
        slot = (uint8_t)s;
        func = (uint8_t)f;
        found = true;
        break;
      }
    }
  }
  if (!found)
    return 0;

  uint32_t bar0 = pci_read32(bus, slot, func, 0x10);
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

  outb(io_base + REG_CONFIG1, 0x00);
  outb(io_base + REG_CR, CR_RST);
  for (int spins = 0; spins < 1000000; spins++)
    if (!(inb(io_base + REG_CR) & CR_RST))
      break;

  outl(io_base + REG_RBSTART, (uint32_t)(uintptr_t)rx_buf);
  rx_cur = 0;
  outw(io_base + REG_CAPR, 0xFFF0);
  outw(io_base + REG_IMR, 0);
  /* RCR: accept broadcast/multicast/phys-match/all, WRAP, max DMA burst, no FIFO threshold, 8K+16 ring. */
  outl(io_base + REG_RCR, 0x0000000F | (1 << 7) | (7 << 8) | (7 << 13));
  outb(io_base + REG_CR, CR_RE | CR_TE);

  for (int i = 0; i < 6; i++)
    mac[i] = inb(io_base + REG_IDR0 + i);

  nic_found = true;

  con_printf("net: RTL8139 at io 0x%08X mac ", io_base);
  for (int i = 0; i < 6; i++) {
    print_hex2(mac[i]);
    if (i < 5)
      con_putc(':');
  }
  con_putc('\n');
  return 1;
}

const struct nic_ops rtl8139_ops = {
    .name = "RTL8139",
    .probe = rtl8139_probe,
    .send = rtl8139_send,
    .mac = rtl8139_mac,
    .poll = rtl_poll,
};
