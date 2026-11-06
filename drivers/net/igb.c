/* igb.c: Intel igb-family Gigabit Ethernet (I210 / I211 / 82576), advanced descriptors over MMIO.
 * Queue 0 registers sit at 0xC000 (RX) / 0xE000 (TX); a queue runs only after RXDCTL/TXDCTL.ENABLE (bit 25).
 * RX is polled from the timer tick (no RX interrupt); MAC comes from RAL/RAH[0]; rings sit in cached RAM. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "console.h"
#include "heap.h"
#include "igb.h"
#include "io.h"
#include "kstring.h"
#include "net.h"
#include "paging.h"
#include "pci.h"

/* irq_save / irq_restore: snapshot EFLAGS + cli, restore the prior IF. */
#include "irqflags.h"

static void print_hex2(uint8_t v)
{
  static const char h[] = "0123456789abcdef";
  con_printf("%c%c", h[v >> 4], h[v & 0xF]);
}

/* Interrupt-free ms delay via PIT channel 2 polling port 0x61; runs before timer_init. */
static void igb_mdelay(uint32_t ms)
{
  while (ms--) {
    uint8_t p = inb(0x61);
    outb(0x61, (uint8_t)((p & ~0x02u) | 0x01u));
    outb(0x43, 0xB0);
    outb(0x42, 1193 & 0xFF);
    outb(0x42, (1193 >> 8) & 0xFF);
    uint32_t guard = 0;
    while (!(inb(0x61) & 0x20) && ++guard < 20000000u)
      ;
  }
}

#define IGB_CTRL 0x00000
#define IGB_STATUS 0x00008
#define IGB_CTRL_EXT 0x00018
#define IGB_MDIC 0x00020
#define IGB_RCTL 0x00100
#define IGB_TCTL 0x00400
#define IGB_TIPG 0x00410
#define IGB_ICR 0x01500
#define IGB_IMC 0x0150C
#define IGB_RDBAL 0x0C000
#define IGB_RDBAH 0x0C004
#define IGB_RDLEN 0x0C008
#define IGB_SRRCTL 0x0C00C
#define IGB_RDH 0x0C010
#define IGB_RDT 0x0C018
#define IGB_RXDCTL 0x0C028
#define IGB_TDBAL 0x0E000
#define IGB_TDBAH 0x0E004
#define IGB_TDLEN 0x0E008
#define IGB_TDH 0x0E010
#define IGB_TDT 0x0E018
#define IGB_TXDCTL 0x0E028
#define IGB_MTA 0x05200
#define IGB_RAL 0x05400
#define IGB_RAH 0x05404
#define IGB_CRCERRS 0x04000
#define IGB_MPC 0x04010
#define IGB_GPRC 0x04074
#define IGB_GPTC 0x04080

/* CTRL bits */
#define CTRL_FD (1u << 0)
#define CTRL_GIO_MASTER_DISABLE (1u << 2)
#define CTRL_LRST (1u << 3)
#define CTRL_ASDE (1u << 5)
#define CTRL_SLU (1u << 6)
#define CTRL_ILOS (1u << 7)
#define CTRL_RST (1u << 26)
#define CTRL_PHY_RST (1u << 31)

/* MDIC PHY access; the integrated copper PHY answers at MDIO address 1. */
#define MDIC_REG_SHIFT 16
#define MDIC_PHY_SHIFT 21
#define MDIC_OP_WRITE (1u << 26)
#define MDIC_OP_READ (2u << 26)
#define MDIC_READY (1u << 28)
#define MDIC_ERROR (1u << 30)
#define IGB_PHY_ADDR 1

/* STATUS bits */
#define STATUS_LU (1u << 1)
#define STATUS_GIO_MASTER_EN (1u << 19)
#define STATUS_SPEED_SHIFT 6

/* RCTL bits; on igb the receive buffer size is set in SRRCTL, not RCTL.BSIZE. */
#define RCTL_EN (1u << 1)
#define RCTL_BAM (1u << 15)
#define RCTL_SECRC (1u << 26)

/* SRRCTL: receive buffer size in 1 KiB units (bits 6:0) + descriptor type. */
#define SRRCTL_BSIZE_2K 0x00000002u
#define SRRCTL_DESCTYPE_ADV_ONEBUF 0x02000000u

/* RXDCTL/TXDCTL: per-queue ENABLE (bit 25) plus Linux igb's single-queue thresholds. */
#define QUEUE_ENABLE 0x02000000u
#define RXDCTL_THRESH (8u | (8u << 8) | (1u << 16))
#define TXDCTL_THRESH (8u | (1u << 8) | (1u << 16))

/* TCTL bits */
#define TCTL_EN (1u << 1)
#define TCTL_PSP (1u << 3)
#define TCTL_CT (0x0Fu << 4)
#define TCTL_COLD (0x3Fu << 12)
#define TCTL_RTLC (1u << 24)

/* Advanced TX data-descriptor command/type bits (igb e1000_adv_tx_desc). */
#define ADVTXD_DTYP_DATA 0x00300000u
#define ADVTXD_DCMD_EOP 0x01000000u
#define ADVTXD_DCMD_IFCS 0x02000000u
#define ADVTXD_DCMD_RS 0x08000000u
#define ADVTXD_DCMD_DEXT 0x20000000u
#define ADVTXD_PAYLEN_SHIFT 14
#define TXD_STAT_DD 0x00000001u

/* Advanced RX descriptor status/error bits (in the write-back status_error). */
#define RXD_STAT_DD 0x01
#define RXD_STAT_EOP 0x02
#define RXD_ERR_MASK 0x9F000000u

/* Advanced RX descriptor: the read half is software's, hardware overwrites it with write-back on receive. */
union igb_rx_desc {
  struct {
    uint64_t pkt_addr;
    uint64_t hdr_addr;
  } read;
  struct {
    uint32_t info0;
    uint32_t info1;
    uint32_t status_error;
    uint16_t length;
    uint16_t vlan;
  } wb;
} __attribute__((packed));

/* Advanced transmit data descriptor (single-buffer, no offload context). */
struct igb_tx_desc {
  uint64_t addr;
  uint32_t cmd_type_len;
  uint32_t olinfo_status;
} __attribute__((packed));

#define RX_DESCS 64
#define TX_DESCS 16
#define BUF_SIZE 2048

#define INTEL_VENDOR 0x8086

/* Recognized igb-family parts; I211 is the hardware target, 82576 is QEMU's `-device igb`. */
static const uint16_t igb_nics[] = {
    0x1539, 0x1533, 0x1536, 0x1537, 0x1538, 0x157B, 0x157C, 0x10C9, 0x150A, 0x150D, 0x1518,
    0x10E6, 0x10E7, 0x10E8, 0x150E, 0x150F, 0x1510, 0x1511, 0x1521, 0x1522, 0x1523, 0x1524,
};

static volatile uint8_t *mmio;
static uint8_t mac[6];
static bool present;
static uint16_t device_id;

static union igb_rx_desc *rx_ring;
static struct igb_tx_desc *tx_ring;
static uint8_t *rx_bufs[RX_DESCS];
static uint8_t *tx_bufs[TX_DESCS];
static uint32_t rx_cur, tx_cur;

/* MMIO accessors; the UC mapping keeps plain volatile accesses ordered. */
static inline void reg_write(uint32_t off, uint32_t val) { *(volatile uint32_t *)(mmio + off) = val; }
static inline uint32_t reg_read(uint32_t off) { return *(volatile uint32_t *)(mmio + off); }

/* One bounded MDIO PHY read; returns -1 on timeout or PHY error. */
static int phy_read(uint8_t reg)
{
  reg_write(IGB_MDIC, ((uint32_t)reg << MDIC_REG_SHIFT) | ((uint32_t)IGB_PHY_ADDR << MDIC_PHY_SHIFT) | MDIC_OP_READ);
  for (uint32_t i = 0; i < 100000u; i++) {
    uint32_t m = reg_read(IGB_MDIC);
    if (m & MDIC_READY)
      return (m & MDIC_ERROR) ? -1 : (int)(m & 0xFFFF);
  }
  return -1;
}

/* One MDIO PHY-register write (bounded). */
static void phy_write(uint8_t reg, uint16_t val)
{
  reg_write(IGB_MDIC, (uint32_t)val | ((uint32_t)reg << MDIC_REG_SHIFT) | ((uint32_t)IGB_PHY_ADDR << MDIC_PHY_SHIFT) |
                          MDIC_OP_WRITE);
  for (uint32_t i = 0; i < 100000u; i++)
    if (reg_read(IGB_MDIC) & MDIC_READY)
      break;
}

static void read_mac(void)
{
  uint32_t ral = reg_read(IGB_RAL);
  uint32_t rah = reg_read(IGB_RAH);

  if (rah & (1u << 31)) {
    mac[0] = ral & 0xFF;
    mac[1] = (ral >> 8) & 0xFF;
    mac[2] = (ral >> 16) & 0xFF;
    mac[3] = (ral >> 24) & 0xFF;
    mac[4] = rah & 0xFF;
    mac[5] = (rah >> 8) & 0xFF;
  }
}

/* Global MAC reset (e1000_reset_hw_82575); it clears RAL/RAH, so read the MAC afterwards. */
static void igb_reset(void)
{
  reg_write(IGB_IMC, 0xFFFFFFFF);
  reg_write(IGB_RCTL, 0);
  reg_write(IGB_TCTL, TCTL_PSP);
  (void)reg_read(IGB_STATUS);
  igb_mdelay(10);

  /* Quiesce bus-master DMA so the reset doesn't catch a transfer mid-flight. */
  reg_write(IGB_CTRL, reg_read(IGB_CTRL) | CTRL_GIO_MASTER_DISABLE);
  for (int g = 0; (reg_read(IGB_STATUS) & STATUS_GIO_MASTER_EN) && g < 1000000; g++)
    ;

  reg_write(IGB_CTRL, reg_read(IGB_CTRL) | CTRL_RST);
  igb_mdelay(10);
  for (int g = 0; (reg_read(IGB_CTRL) & CTRL_RST) && g < 1000000; g++)
    ;

  reg_write(IGB_IMC, 0xFFFFFFFF);
  (void)reg_read(IGB_ICR);
}

static int setup_rx(void)
{
  rx_ring = palloc();
  if (!rx_ring)
    return -1;
  for (int i = 0; i < RX_DESCS; i++) {
    rx_bufs[i] = kmalloc(BUF_SIZE);
    if (!rx_bufs[i])
      return -1;
    rx_ring[i].read.pkt_addr = (uint32_t)(uintptr_t)rx_bufs[i];
    rx_ring[i].read.hdr_addr = 0;
  }

  reg_write(IGB_RXDCTL, 0);
  reg_write(IGB_RDBAL, (uint32_t)(uintptr_t)rx_ring);
  reg_write(IGB_RDBAH, 0);
  reg_write(IGB_RDLEN, RX_DESCS * sizeof(union igb_rx_desc));
  reg_write(IGB_SRRCTL, SRRCTL_BSIZE_2K | SRRCTL_DESCTYPE_ADV_ONEBUF);
  reg_write(IGB_RDH, 0);
  reg_write(IGB_RDT, 0);
  rx_cur = 0;

  /* Arm the queue and wait for the hardware to acknowledge ENABLE. */
  reg_write(IGB_RXDCTL, QUEUE_ENABLE | RXDCTL_THRESH);
  for (int g = 0; !(reg_read(IGB_RXDCTL) & QUEUE_ENABLE) && g < 1000000; g++)
    ;

  /* Enable the receiver, then advance RDT only after the queue is live. */
  reg_write(IGB_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);
  reg_write(IGB_RDT, RX_DESCS - 1);
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
    tx_ring[i].addr = (uint32_t)(uintptr_t)tx_bufs[i];
    tx_ring[i].olinfo_status = 0;
  }

  reg_write(IGB_TXDCTL, 0);
  reg_write(IGB_TDBAL, (uint32_t)(uintptr_t)tx_ring);
  reg_write(IGB_TDBAH, 0);
  reg_write(IGB_TDLEN, TX_DESCS * sizeof(struct igb_tx_desc));
  reg_write(IGB_TDH, 0);
  reg_write(IGB_TDT, 0);
  tx_cur = 0;

  reg_write(IGB_TIPG, 0x00602008);
  reg_write(IGB_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD | TCTL_RTLC);

  reg_write(IGB_TXDCTL, QUEUE_ENABLE | TXDCTL_THRESH);
  for (int g = 0; !(reg_read(IGB_TXDCTL) & QUEUE_ENABLE) && g < 1000000; g++)
    ;
  return 0;
}

static void igb_send(const uint8_t *frame, uint32_t len)
{
  if (!present || len > 1514)
    return;

  uint32_t fl = irq_save();

  uint32_t i = tx_cur;
  memcpy(tx_bufs[i], frame, len);
  tx_ring[i].addr = (uint32_t)(uintptr_t)tx_bufs[i];
  /* Single-buffer advanced data descriptor; PSP pads runts in hardware. */
  tx_ring[i].cmd_type_len =
      len | ADVTXD_DTYP_DATA | ADVTXD_DCMD_DEXT | ADVTXD_DCMD_EOP | ADVTXD_DCMD_IFCS | ADVTXD_DCMD_RS;
  tx_ring[i].olinfo_status = len << ADVTXD_PAYLEN_SHIFT;

  tx_cur = (i + 1) % TX_DESCS;
  reg_write(IGB_TDT, tx_cur);

  /* Wait for the card to write back DD (bounded spin). */
  for (int spins = 0; spins < 2000000; spins++)
    if (tx_ring[i].olinfo_status & TXD_STAT_DD)
      break;

  irq_restore(fl);
}

/* Drain complete frames; write-back clobbers pkt_addr, so re-arm each consumed descriptor before RDT. */
static void rx_poll(void)
{
  while (rx_ring[rx_cur].wb.status_error & RXD_STAT_DD) {
    uint32_t se = rx_ring[rx_cur].wb.status_error;
    uint16_t len = rx_ring[rx_cur].wb.length;

    if ((se & RXD_STAT_EOP) && !(se & RXD_ERR_MASK) && len >= 14 && len <= BUF_SIZE)
      net_rx_frame(rx_bufs[rx_cur], len);

    rx_ring[rx_cur].read.pkt_addr = (uint32_t)(uintptr_t)rx_bufs[rx_cur];
    rx_ring[rx_cur].read.hdr_addr = 0;

    uint32_t done = rx_cur;
    rx_cur = (rx_cur + 1) % RX_DESCS;
    reg_write(IGB_RDT, done);
  }
}

/* IRQ-independent RX drain off the timer tick (nic_poll); the sole receive path. */
static void igb_poll(void)
{
  if (!present)
    return;
  uint32_t fl = irq_save();
  rx_poll();
  irq_restore(fl);
}

static const uint8_t *igb_mac(void) { return mac; }

static const char *model_name(uint16_t dev)
{
  switch (dev) {
  case 0x1539:
    return "I211";
  case 0x1533:
  case 0x1536:
  case 0x1537:
  case 0x1538:
  case 0x157B:
  case 0x157C:
    return "I210";
  case 0x1521:
  case 0x1522:
  case 0x1523:
  case 0x1524:
    return "I350";
  case 0x150E:
  case 0x150F:
  case 0x1510:
  case 0x1511:
    return "82580";
  default:
    return "82576";
  }
}

/* Link state for the IP stack to gate DHCP on. */
static int igb_link_up(void) { return present && (reg_read(IGB_STATUS) & STATUS_LU); }

static int igb_probe(void)
{
  bool found = false;
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
        for (unsigned k = 0; k < sizeof(igb_nics) / sizeof(igb_nics[0]); k++) {
          if (dev == igb_nics[k]) {
            bus = (uint8_t)b;
            slot = (uint8_t)s;
            func = (uint8_t)f;
            device_id = dev;
            found = true;
            break;
          }
        }
      }
    }
  }
  if (!found)
    return 0;

  uint32_t bar0 = pci_read32(bus, slot, func, 0x10);
  uint32_t mmio_phys = bar0 & ~0xFu;

  if (mmio_phys == 0 || mmio_phys == 0xFFFFFFF0) {
    con_printf("net: Intel %s BAR0 unassigned; skipping\n", model_name(device_id));
    return 0;
  }

  /* Enable memory-space decoding (bit 1) and bus-master DMA (bit 2). */
  uint32_t cmd = pci_read32(bus, slot, func, 0x04);
  pci_write32(bus, slot, func, 0x04, cmd | 0x06);

  /* Map the register file uncached. The igb register window is 128 KiB. */
  if (paging_identity_map(mmio_phys, 0x20000) != 0) {
    con_write("net: igb MMIO map failed\n");
    return 0;
  }
  paging_mark_uc(mmio_phys, 0x20000);
  mmio = (volatile uint8_t *)(uintptr_t)mmio_phys;

  igb_reset();

  /* Link up: SLU plus ASDE so the MAC adopts the PHY's autonegotiated speed. */
  uint32_t ctrl = reg_read(IGB_CTRL);
  ctrl |= CTRL_SLU | CTRL_ASDE;
  ctrl &= ~(CTRL_LRST | CTRL_PHY_RST | CTRL_ILOS);
  reg_write(IGB_CTRL, ctrl);

  /* Advertise gigabit and restart autoneg; SLU alone leaves STATUS.LU clear. Best-effort if the PHY is silent. */
  int gbcr = phy_read(9);
  if (gbcr >= 0)
    phy_write(9, (uint16_t)((gbcr & 0xFCFF) | 0x0200));
  phy_write(0, 0x1200);

  /* Clear the multicast table so stray hashes don't accept junk. */
  for (int i = 0; i < 128; i++)
    reg_write(IGB_MTA + i * 4, 0);

  read_mac();
  /* (Re)program the unicast filter from the MAC the reset cleared. */
  reg_write(IGB_RAL, mac[0] | mac[1] << 8 | mac[2] << 16 | (uint32_t)mac[3] << 24);
  reg_write(IGB_RAH, mac[4] | mac[5] << 8 | (1u << 31));

  if (setup_rx() != 0 || setup_tx() != 0) {
    con_write("net: igb ring allocation failed\n");
    return 0;
  }

  present = true;

  con_printf("net: Intel %s at mmio 0x%08X mac ", model_name(device_id), mmio_phys);
  for (int i = 0; i < 6; i++) {
    print_hex2(mac[i]);
    if (i < 5)
      con_putc(':');
  }
  con_printf(" link %s", (reg_read(IGB_STATUS) & STATUS_LU) ? "up\n" : "down (cable?)\n");
  return 1;
}

const struct nic_ops igb_ops = {
    .name = "igb",
    .probe = igb_probe,
    .send = igb_send,
    .mac = igb_mac,
    .poll = igb_poll,
    .link_up = igb_link_up,
};
