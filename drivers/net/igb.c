/* igb.c — Intel igb-family Gigabit Ethernet driver (I210 / I211 / 82576).
 *
 * Drives the Intel "igb" MAC over its memory-mapped register file. This family
 * (82575/82576/82580/I350 and the single-port I210/I211) is a different
 * architecture from the 8254x/82574/e1000e parts in e1000.c, so it gets its own
 * driver. Three things differ from e1000.c and are the whole reason for a
 * separate file:
 *
 *   1. The per-queue descriptor registers moved. e1000 had a single RX ring at
 *      0x2800 and TX ring at 0x3800; igb has 16 RX/TX queues, queue 0's window
 *      at 0xC000 (RX) / 0xE000 (TX), each queue +0x40.
 *   2. igb uses *advanced* descriptors, not the legacy 16-byte format. The read
 *      (software-owned) and write-back (hardware-owned) layouts differ and
 * share the 16 bytes as a union — see igb_rx_desc / igb_tx_desc.
 *   3. A queue is armed by setting RXDCTL/TXDCTL.ENABLE (bit 25) and polling it
 *      back; RCTL.EN / TCTL.EN alone (the e1000 way) leaves the queues dead.
 *
 * Receive is drained off the timer tick via the .poll hook (nic_poll), exactly
 * like the VMware e1000 path — no RX interrupt is wired, which avoids igb's
 * GPIE/IVAR/EICR extended-interrupt setup entirely. The MAC address is read
 * from the receive-address registers RAL/RAH[0], which firmware (and QEMU)
 * pre-load, so the per-part iNVM/EEPROM layout never has to be touched.
 *
 * The descriptor rings live in normal write-back RAM (palloc frames): x86 DMA
 * is cache-coherent, so the card's write-back of a descriptor's DD bit is
 * snooped and the CPU sees it without an uncached mapping. Only the registers
 * are UC.
 *
 * Verified in QEMU against `-device igb` (the 82576, device 0x10C9) — DHCP
 * lease
 * + ICMP/TCP over SLIRP. The I210/I211 are the same igb programming model, so
 * the same code drives them; their bring-up logs to the journal (`journal`
 * command) since a laptop shows no console at NIC-probe time.
 */

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

/* ------------------------------------------------------------------ helpers */

/* irq_save / irq_restore: snapshot EFLAGS + cli, restore the prior IF. */
#include "irqflags.h"

static void print_hex2(uint8_t v)
{
  static const char h[] = "0123456789abcdef";
  con_printf("%c%c", h[v >> 4], h[v & 0xF]);
}

/* Interrupt-free millisecond delay using PIT channel 2 in one-shot (mode 0),
 * polling its OUT bit on port 0x61. The NIC probe runs before timer_init, so
 * timer_sleep (which blocks on the timer IRQ) would hang the boot here. Channel
 * 2 is the speaker timer, independent of channel 0 the kernel timer uses. */
static void igb_mdelay(uint32_t ms)
{
  while (ms--) {
    uint8_t p = inb(0x61);
    outb(0x61, (uint8_t)((p & ~0x02u) | 0x01u)); /* speaker off, gate2 on   */
    outb(0x43, 0xB0);                            /* ch2, lo/hi, mode 0, bin */
    outb(0x42, 1193 & 0xFF);                     /* 1193182 Hz / 1000 ~ 1ms */
    outb(0x42, (1193 >> 8) & 0xFF);
    uint32_t guard = 0;
    while (!(inb(0x61) & 0x20) && ++guard < 20000000u)
      ;
  }
}

/* ----------------------------------------------------------- register file */

#define IGB_CTRL 0x00000     /* device control                              */
#define IGB_STATUS 0x00008   /* device status (link up, speed)              */
#define IGB_CTRL_EXT 0x00018 /* extended device control                     */
#define IGB_MDIC 0x00020     /* PHY register access (unused; poll link)     */
#define IGB_RCTL 0x00100     /* receive control                             */
#define IGB_TCTL 0x00400     /* transmit control                            */
#define IGB_TIPG 0x00410     /* transmit inter-packet gap                   */
#define IGB_ICR 0x01500      /* interrupt cause read (igb block, not 0xC0)  */
#define IGB_IMC 0x0150C      /* interrupt mask clear                        */
#define IGB_RDBAL 0x0C000    /* RX queue-0 descriptor base (low)            */
#define IGB_RDBAH 0x0C004    /* RX queue-0 descriptor base (high)           */
#define IGB_RDLEN 0x0C008    /* RX queue-0 ring length (bytes)              */
#define IGB_SRRCTL 0x0C00C   /* RX queue-0 split/replication + buffer size  */
#define IGB_RDH 0x0C010      /* RX queue-0 head                             */
#define IGB_RDT 0x0C018      /* RX queue-0 tail                             */
#define IGB_RXDCTL 0x0C028   /* RX queue-0 descriptor control (+ENABLE)     */
#define IGB_TDBAL 0x0E000    /* TX queue-0 descriptor base (low)            */
#define IGB_TDBAH 0x0E004    /* TX queue-0 descriptor base (high)           */
#define IGB_TDLEN 0x0E008    /* TX queue-0 ring length (bytes)              */
#define IGB_TDH 0x0E010      /* TX queue-0 head                             */
#define IGB_TDT 0x0E018      /* TX queue-0 tail                             */
#define IGB_TXDCTL 0x0E028   /* TX queue-0 descriptor control (+ENABLE)     */
#define IGB_MTA 0x05200      /* multicast table array (128 dwords)          */
#define IGB_RAL 0x05400      /* receive address low [0]                     */
#define IGB_RAH 0x05404      /* receive address high [0] (+ Address Valid)  */
#define IGB_CRCERRS 0x04000  /* CRC error count (clear-on-read)             */
#define IGB_MPC 0x04010      /* missed packets (RX overrun, clear-on-read)  */
#define IGB_GPRC 0x04074     /* good packets received (clear-on-read)       */
#define IGB_GPTC 0x04080     /* good packets transmitted (clear-on-read)    */

/* CTRL bits */
#define CTRL_FD (1u << 0)                 /* full duplex                       */
#define CTRL_GIO_MASTER_DISABLE (1u << 2) /* quiesce bus-master DMA before reset */
#define CTRL_LRST (1u << 3)               /* link reset (clear)                */
#define CTRL_ASDE (1u << 5)               /* auto-speed detect (adopt autoneg) */
#define CTRL_SLU (1u << 6)                /* set link up                       */
#define CTRL_ILOS (1u << 7)               /* invert loss-of-signal (clear)     */
#define CTRL_RST (1u << 26)               /* device (port) reset               */
#define CTRL_PHY_RST (1u << 31)           /* PHY reset (clear)                 */

/* MDIC (PHY register access over MDIO). The igb integrated copper PHY answers
 * at MDIO address 1; access is direct (no PCH software-flag arbitration). */
#define MDIC_REG_SHIFT 16
#define MDIC_PHY_SHIFT 21
#define MDIC_OP_WRITE (1u << 26)
#define MDIC_OP_READ (2u << 26)
#define MDIC_READY (1u << 28)
#define MDIC_ERROR (1u << 30)
#define IGB_PHY_ADDR 1

/* STATUS bits */
#define STATUS_LU (1u << 1)             /* link up                           */
#define STATUS_GIO_MASTER_EN (1u << 19) /* bus-master DMA still active */
#define STATUS_SPEED_SHIFT 6            /* bits 7:6: 0=10 1=100 2/3=1000      */

/* RCTL bits — on igb the receive buffer size is set in SRRCTL, not RCTL.BSIZE.
 */
#define RCTL_EN (1u << 1)     /* receiver enable                           */
#define RCTL_BAM (1u << 15)   /* accept broadcast                          */
#define RCTL_SECRC (1u << 26) /* strip Ethernet CRC (FCS) on receive       */

/* SRRCTL: receive buffer size in 1 KiB units (bits 6:0) + descriptor type. */
#define SRRCTL_BSIZE_2K 0x00000002u            /* 2 * 1 KiB = 2048-byte buffer  */
#define SRRCTL_DESCTYPE_ADV_ONEBUF 0x02000000u /* bits 27:25 = 001 (advanced)   */

/* RXDCTL / TXDCTL: per-queue enable (bit 25) + prefetch/host/write-back
 * thresholds. The thresholds below are the values Linux igb programs for the
 * single-queue case; without ENABLE the queue never fetches a descriptor. */
#define QUEUE_ENABLE 0x02000000u
#define RXDCTL_THRESH (8u | (8u << 8) | (1u << 16)) /* P=8 H=8 W=1     */
#define TXDCTL_THRESH (8u | (1u << 8) | (1u << 16)) /* P=8 H=1 W=1     */

/* TCTL bits */
#define TCTL_EN (1u << 1)    /* transmitter enable                        */
#define TCTL_PSP (1u << 3)   /* pad short packets to 60 bytes             */
#define TCTL_CT (0x0Fu << 4) /* collision threshold (igb: 15)             */
#define TCTL_COLD                                                                                                      \
  (0x3Fu << 12)              /* collision distance, full duplex (igb: 63)                                              \
                              */
#define TCTL_RTLC (1u << 24) /* re-transmit on late collision             */

/* Advanced TX data-descriptor command/type bits (igb e1000_adv_tx_desc). */
#define ADVTXD_DTYP_DATA 0x00300000u /* descriptor type = advanced data */
#define ADVTXD_DCMD_EOP 0x01000000u  /* end of packet */
#define ADVTXD_DCMD_IFCS 0x02000000u /* insert FCS (Ethernet CRC) */
#define ADVTXD_DCMD_RS 0x08000000u   /* report status (write DD when done)    */
#define ADVTXD_DCMD_DEXT 0x20000000u /* descriptor extension (1 = advanced) */
#define ADVTXD_PAYLEN_SHIFT 14       /* PAYLEN field position in olinfo_status */
#define TXD_STAT_DD 0x00000001u      /* descriptor done (written back)        */

/* Advanced RX descriptor status/error bits (in the write-back status_error). */
#define RXD_STAT_DD 0x01         /* descriptor done                       */
#define RXD_STAT_EOP 0x02        /* end of packet                         */
#define RXD_ERR_MASK 0x9F000000u /* any RX error (CE/SE/SEQ/RXE/etc.)     */

/* Advanced receive descriptor: software fills the read half (buffer addresses);
 * hardware overwrites the same 16 bytes with the write-back half on receive. */
union igb_rx_desc {
  struct {
    uint64_t pkt_addr; /* packet buffer (must be re-armed each time) */
    uint64_t hdr_addr; /* header buffer — unused, kept 0             */
  } read;
  struct {
    uint32_t info0;        /* RSS type / packet info                     */
    uint32_t info1;        /* RSS hash / checksum                        */
    uint32_t status_error; /* bit0 DD, bit1 EOP, error bits high         */
    uint16_t length;       /* received length (FCS already stripped)     */
    uint16_t vlan;
  } wb;
} __attribute__((packed));

/* Advanced transmit data descriptor (single-buffer, no offload context). */
struct igb_tx_desc {
  uint64_t addr;          /* buffer physical address                    */
  uint32_t cmd_type_len;  /* DTALEN + DTYP + DCMD bits                   */
  uint32_t olinfo_status; /* PAYLEN<<14; bit0 = DD on write-back         */
} __attribute__((packed));

#define RX_DESCS 64 /* 64 * 16 = 1024 bytes (multiple of 128)    */
#define TX_DESCS 16 /* 16 * 16 = 256 bytes  (multiple of 128)    */
#define BUF_SIZE 2048

/* ------------------------------------------------------------------- state */

#define INTEL_VENDOR 0x8086

/* The igb-family parts this driver recognizes. They all drive the per-queue
 * advanced-descriptor model identically. The I211 (0x1539) is the verified
 * real-hardware target; the 82576 (0x10C9) is QEMU's `-device igb` model, which
 * exercises the exact same code path in emulation. */
static const uint16_t igb_nics[] = {
    0x1539,                         /* I211-AT (copper)                       */
    0x1533,                         /* I210-AT (copper, with flash)           */
    0x1536, 0x1537, 0x1538,         /* I210 (SERDES / SGMII / external)       */
    0x157B, 0x157C,                 /* I210 (flashless copper / serdes)       */
    0x10C9,                         /* 82576 (QEMU -device igb)               */
    0x150A, 0x150D, 0x1518,         /* 82576 variants                         */
    0x10E6, 0x10E7, 0x10E8,         /* 82576 variants                         */
    0x150E, 0x150F, 0x1510, 0x1511, /* 82580                                  */
    0x1521, 0x1522, 0x1523, 0x1524, /* I350                                   */
};

static volatile uint8_t *mmio; /* BAR0, mapped uncached                   */
static uint8_t mac[6];
static int present;
static uint16_t device_id;

static union igb_rx_desc *rx_ring;  /* RX_DESCS entries, page-aligned         */
static struct igb_tx_desc *tx_ring; /* TX_DESCS entries, page-aligned         */
static uint8_t *rx_bufs[RX_DESCS];
static uint8_t *tx_bufs[TX_DESCS];
static uint32_t rx_cur, tx_cur;

/* MMIO accessors. The mapping is UC, so plain volatile loads/stores are not
 * cached or reordered against each other. */
static inline void reg_write(uint32_t off, uint32_t val) { *(volatile uint32_t *)(mmio + off) = val; }
static inline uint32_t reg_read(uint32_t off) { return *(volatile uint32_t *)(mmio + off); }

/* One MDIO PHY-register read (bounded). Returns the 16-bit value, or -1 on a
 * timeout / PHY error. */
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

/* ----------------------------------------------------------------- MAC read */

static void read_mac(void)
{
  uint32_t ral = reg_read(IGB_RAL);
  uint32_t rah = reg_read(IGB_RAH);

  if (rah & (1u << 31)) { /* Address Valid: firmware/QEMU loaded it   */
    mac[0] = ral & 0xFF;
    mac[1] = (ral >> 8) & 0xFF;
    mac[2] = (ral >> 16) & 0xFF;
    mac[3] = (ral >> 24) & 0xFF;
    mac[4] = rah & 0xFF;
    mac[5] = (rah >> 8) & 0xFF;
  }
}

/* --------------------------------------------------------------------- reset
 */

/* Global MAC reset (igb e1000_reset_hw_82575). Quiesce DMA, mask interrupts,
 * pulse CTRL.RST, wait for it to self-clear, then re-mask. The reset clears
 * RAL/RAH, so the caller captures the MAC afterwards. */
static void igb_reset(void)
{
  reg_write(IGB_IMC, 0xFFFFFFFF); /* mask every interrupt cause      */
  reg_write(IGB_RCTL, 0);         /* stop the receiver               */
  reg_write(IGB_TCTL, TCTL_PSP);  /* stop the transmitter            */
  (void)reg_read(IGB_STATUS);     /* flush the posted writes         */
  igb_mdelay(10);

  /* Quiesce bus-master DMA so the reset doesn't catch a transfer mid-flight. */
  reg_write(IGB_CTRL, reg_read(IGB_CTRL) | CTRL_GIO_MASTER_DISABLE);
  for (int g = 0; (reg_read(IGB_STATUS) & STATUS_GIO_MASTER_EN) && g < 1000000; g++)
    ;

  reg_write(IGB_CTRL, reg_read(IGB_CTRL) | CTRL_RST);
  igb_mdelay(10); /* RST self-clears after ~few ms   */
  for (int g = 0; (reg_read(IGB_CTRL) & CTRL_RST) && g < 1000000; g++)
    ;

  reg_write(IGB_IMC, 0xFFFFFFFF); /* reset re-arms some causes       */
  (void)reg_read(IGB_ICR);        /* read clears pending causes      */
}

/* --------------------------------------------------------------- ring setup */

static int setup_rx(void)
{
  rx_ring = palloc(); /* zeroed, page-aligned, identity-mapped     */
  if (!rx_ring)
    return -1;
  for (int i = 0; i < RX_DESCS; i++) {
    rx_bufs[i] = kmalloc(BUF_SIZE);
    if (!rx_bufs[i])
      return -1;
    rx_ring[i].read.pkt_addr = (uint32_t)rx_bufs[i];
    rx_ring[i].read.hdr_addr = 0;
  }

  reg_write(IGB_RXDCTL, 0); /* disable the queue while we set up */
  reg_write(IGB_RDBAL, (uint32_t)rx_ring);
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

  /* Enable the receiver, then hand every descriptor to the card (RDT must be
   * advanced only after the queue is live). */
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
    tx_ring[i].addr = (uint32_t)tx_bufs[i];
    tx_ring[i].olinfo_status = 0;
  }

  reg_write(IGB_TXDCTL, 0); /* disable the queue while we set up */
  reg_write(IGB_TDBAL, (uint32_t)tx_ring);
  reg_write(IGB_TDBAH, 0);
  reg_write(IGB_TDLEN, TX_DESCS * sizeof(struct igb_tx_desc));
  reg_write(IGB_TDH, 0);
  reg_write(IGB_TDT, 0);
  tx_cur = 0;

  reg_write(IGB_TIPG, 0x00602008); /* IPGT=8 IPGR1=8 IPGR2=6 (copper) */
  reg_write(IGB_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD | TCTL_RTLC);

  reg_write(IGB_TXDCTL, QUEUE_ENABLE | TXDCTL_THRESH);
  for (int g = 0; !(reg_read(IGB_TXDCTL) & QUEUE_ENABLE) && g < 1000000; g++)
    ;
  return 0;
}

/* --------------------------------------------------------------- transmit */

static void igb_send(const uint8_t *frame, uint32_t len)
{
  if (!present || len > 1514)
    return;

  uint32_t fl = irq_save();

  uint32_t i = tx_cur;
  memcpy(tx_bufs[i], frame, len);
  tx_ring[i].addr = (uint32_t)tx_bufs[i];
  /* Single-buffer advanced data descriptor. PSP pads runts to 60 in hardware,
   * so DTALEN/PAYLEN carry the real length. */
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

/* ---------------------------------------------------------------- receive */

/* Drain every complete frame the card has DMA'd in. The advanced write-back
 * overwrites the descriptor's buffer-address half, so each consumed descriptor
 * must be re-armed (pkt_addr rewritten) before it is handed back via RDT. */
static void rx_poll(void)
{
  while (rx_ring[rx_cur].wb.status_error & RXD_STAT_DD) {
    uint32_t se = rx_ring[rx_cur].wb.status_error;
    uint16_t len = rx_ring[rx_cur].wb.length;

    if ((se & RXD_STAT_EOP) && !(se & RXD_ERR_MASK) && len >= 14 && len <= BUF_SIZE)
      net_rx_frame(rx_bufs[rx_cur], len); /* SECRC already dropped FCS  */

    rx_ring[rx_cur].read.pkt_addr = (uint32_t)rx_bufs[rx_cur]; /* re-arm     */
    rx_ring[rx_cur].read.hdr_addr = 0;

    uint32_t done = rx_cur;
    rx_cur = (rx_cur + 1) % RX_DESCS;
    reg_write(IGB_RDT, done); /* hand the emptied descriptor back          */
  }
}

/* IRQ-independent RX drain, called off the timer tick (see nic_poll). No RX
 * interrupt is wired for igb, so this is the sole receive path. IF-guarded for
 * symmetry with the rest of the stack; nothing else touches rx_cur/RDT. */
static void igb_poll(void)
{
  if (!present)
    return;
  uint32_t fl = irq_save();
  rx_poll();
  irq_restore(fl);
}

/* --------------------------------------------------------------------- init */

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

/* Link state for the IP stack to gate DHCP on: real copper takes a couple of
 * seconds to autonegotiate, far longer than boot DHCP's patience. */
static int igb_link_up(void) { return present && (reg_read(IGB_STATUS) & STATUS_LU); }

static int igb_probe(void)
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
        for (unsigned k = 0; k < sizeof(igb_nics) / sizeof(igb_nics[0]); k++) {
          if (dev == igb_nics[k]) {
            bus = (uint8_t)b;
            slot = (uint8_t)s;
            func = (uint8_t)f;
            device_id = dev;
            found = 1;
            break;
          }
        }
      }
    }
  }
  if (!found)
    return 0; /* silent: another driver may own the NIC      */

  uint32_t bar0 = pci_read32(bus, slot, func, 0x10);
  uint32_t mmio_phys = bar0 & ~0xFu; /* BAR0 is a memory BAR (maybe 64-bit) */

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
  mmio = (volatile uint8_t *)mmio_phys;

  igb_reset();

  /* Bring the link up. SLU sets link-up; ASDE makes the MAC adopt the speed/
   * duplex the PHY autonegotiates. Clear the reset/loopback bits so the MAC
   * tracks the PHY rather than a forced mode. */
  uint32_t ctrl = reg_read(IGB_CTRL);
  ctrl |= CTRL_SLU | CTRL_ASDE;
  ctrl &= ~(CTRL_LRST | CTRL_PHY_RST | CTRL_ILOS);
  reg_write(IGB_CTRL, ctrl);

  /* Kick the integrated PHY: advertise gigabit, then enable + restart
   * autonegotiation. STATUS.LU only asserts once a link is negotiated — on real
   * copper and on QEMU's igb model alike, SLU alone leaves it clear, which
   * stalls the link-gated DHCP path. Best-effort: a PHY that doesn't answer
   * MDIO just leaves these no-ops and link is reported via STATUS as-is. */
  int gbcr = phy_read(9); /* 1000BASE-T control                */
  if (gbcr >= 0)
    phy_write(9, (uint16_t)((gbcr & 0xFCFF) | 0x0200)); /* advertise 1000-FD  */
  phy_write(0, 0x1200);                                 /* BMCR: autoneg enable + restart    */

  /* Clear the multicast table so stray hashes don't accept junk. */
  for (int i = 0; i < 128; i++)
    reg_write(IGB_MTA + i * 4, 0);

  read_mac();
  /* (Re)program the unicast filter from the MAC the reset cleared. */
  reg_write(IGB_RAL, mac[0] | mac[1] << 8 | mac[2] << 16 | (uint32_t)mac[3] << 24);
  reg_write(IGB_RAH, mac[4] | mac[5] << 8 | (1u << 31)); /* + Address Valid  */

  if (setup_rx() != 0 || setup_tx() != 0) {
    con_write("net: igb ring allocation failed\n");
    return 0;
  }

  present = 1;

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
