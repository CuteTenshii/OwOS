/* e1000.c — Intel Gigabit Ethernet driver (8254x / 82574 / I217-I219 family).
 *
 * NOTE: the I210/I211 are NOT this family — they are the "igb" architecture
 * (per-queue advanced descriptors) and live in igb.c. "I21x" here means the
 * PCH-integrated I217/I218/I219 (ich8lan/e1000e), not I210/I211.
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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "console.h"
#include "e1000.h"
#include "fb.h"
#include "heap.h"
#include "io.h"
#include "irq.h"
#include "kstring.h"
#include "net.h"
#include "paging.h"
#include "pci.h"

/* Attempt to wake and drive PCH-integrated MACs (I217/I218/I219). Set to 0 to
 * fall back to the old behaviour (recognize-but-skip) if the wake sequence ever
 * hangs boot on a particular machine — the only recovery is a rebuild, so the
 * escape hatch lives here. */
#define E1000_PCH_WAKE 1

/* ------------------------------------------------------------------ helpers */

/* irq_save / irq_restore: snapshot EFLAGS + cli, restore the prior IF. */
#include "irqflags.h"

static void print_hex2(uint8_t v)
{
  static const char h[] = "0123456789abcdef";
  con_printf("%c%c", h[v >> 4], h[v & 0xF]);
}

/* ----------------------------------------------------------- register file */

#define E1000_CTRL 0x0000     /* device control                              */
#define E1000_STATUS 0x0008   /* device status (link up, speed)              */
#define E1000_EERD 0x0014     /* EEPROM read (MAC fallback)                  */
#define E1000_ICR 0x00C0      /* interrupt cause read (read clears)          */
#define E1000_IMS 0x00D0      /* interrupt mask set                          */
#define E1000_IMC 0x00D8      /* interrupt mask clear                        */
#define E1000_RCTL 0x0100     /* receive control                             */
#define E1000_TCTL 0x0400     /* transmit control                            */
#define E1000_TIPG 0x0410     /* transmit inter-packet gap                   */
#define E1000_RDBAL 0x2800    /* RX descriptor base (low)                    */
#define E1000_RDBAH 0x2804    /* RX descriptor base (high)                   */
#define E1000_RDLEN 0x2808    /* RX descriptor ring length (bytes)           */
#define E1000_RDH 0x2810      /* RX descriptor head                          */
#define E1000_RDT 0x2818      /* RX descriptor tail                          */
#define E1000_RXDCTL 0x2828   /* RX descriptor control (thresholds)          */
#define E1000_TDBAL 0x3800    /* TX descriptor base (low)                    */
#define E1000_TDBAH 0x3804    /* TX descriptor base (high)                   */
#define E1000_TDLEN 0x3808    /* TX descriptor ring length (bytes)           */
#define E1000_TDH 0x3810      /* TX descriptor head                          */
#define E1000_TDT 0x3818      /* TX descriptor tail                          */
#define E1000_TXDCTL 0x3828   /* TX descriptor control (prefetch/writeback)  */
#define E1000_TXDCTL1 0x3928  /* TX descriptor control queue 1 (magic bit)   */
#define E1000_TARC0 0x3840    /* TX arbitration counter 0 (SPT TX errata)    */
#define E1000_TARC1 0x3940    /* TX arbitration counter 1 (magic init bits)  */
#define E1000_RFCTL 0x5008    /* receive filter control (NFS-disable WA)     */
#define E1000_PBECCSTS 0x100C /* packet-buffer ECC status (pch_lpt+ enable) */
#define E1000_GCR 0x5B00      /* PCIe control: DMA cache-snoop enables       */
#define E1000_IOSFPC 0x0F28   /* IOSF primary channel control (SPT TX errata)*/
#define E1000_CRCERRS 0x4000  /* CRC error count (clear-on-read)             */
#define E1000_MPC 0x4010      /* missed packets (RX overrun, clear-on-read)  */
#define E1000_GPRC 0x4074     /* good packets received (clear-on-read)       */
#define E1000_GPTC 0x4080     /* good packets transmitted (clear-on-read)    */
#define E1000_TPR 0x40D0      /* total packets received (clear-on-read)      */
#define E1000_TPT 0x40D4      /* total packets transmitted (clear-on-read)   */
#define E1000_MTA 0x5200      /* multicast table array (128 dwords)          */
#define E1000_RAL 0x5400      /* receive address low [0]                     */
#define E1000_RAH 0x5404      /* receive address high [0] (+ Address Valid)  */
#define E1000_MDIC 0x0020     /* PHY register access over MDIO (PCH)         */
#define E1000_CTRL_EXT                                                                                                 \
  0x0018                      /* extended device control (FORCE_SMBUS, LPCD)                                           \
                               */
#define E1000_FEXTNVM3 0x003C /* PHY config counter (LANPHYPC timing) */
#define E1000_EXTCNF_CTRL                                                                                              \
  0x0F00                         /* extended config control (PHY SW/FW flag)                                           \
                                  */
#define E1000_H2ME 0x5B50        /* host -> ME mailbox (request ULP unconfig)   */
#define E1000_FWSM 0x5B54        /* firmware status (ME valid + ULP cfg done)   */
#define E1000_KMRNCTRLSTA 0x0034 /* Kumeran (MAC<->PHY internal link) control  */

/* CTRL bits */
#define CTRL_FD (1u << 0)                 /* full duplex                              */
#define CTRL_GIO_MASTER_DISABLE (1u << 2) /* quiesce bus-master DMA before reset */
#define CTRL_ASDE (1u << 5)               /* auto-speed detect (adopt PHY's autoneg)  */
#define CTRL_SLU (1u << 6)                /* set link up                             */
#define CTRL_SPEED_SHIFT 8                /* speed select: 0=10 1=100 2=1000          */
#define CTRL_FRCSPD (1u << 11)            /* force speed (don't track the PHY)        */
#define CTRL_FRCDPLX (1u << 12)           /* force duplex                             */
#define CTRL_ILOS (1u << 7)               /* invert loss-of-signal (clear)           */
#define CTRL_RST (1u << 26)               /* device reset                            */
#define CTRL_PHY_RST (1u << 31)           /* PHY reset (clear)                       */
#define CTRL_LRST (1u << 3)               /* link reset (clear)                      */
#define CTRL_LANPHYPC_OVERRIDE                                                                                         \
  (1u << 16)                           /* drive the LANPHYPC pin manually                                              \
                                        */
#define CTRL_LANPHYPC_VALUE (1u << 17) /* value driven onto LANPHYPC       */

/* STATUS bits */
#define STATUS_LU (1u << 1)             /* link up                                 */
#define STATUS_GIO_MASTER_EN (1u << 19) /* bus-master DMA still active          */

/* EXTCNF_CTRL: gate automatic PHY config across a MAC reset (else the reset can
 * hang the bus reconfiguring a parked PHY — the original frozen-boot cause). */
#define EXTCNF_GATE_PHY_CFG (1u << 7)

/* MDIC (PHY register access over MDIO) and the EXTCNF_CTRL software flag */
#define MDIC_REG_SHIFT 16
#define MDIC_PHY_SHIFT 21
#define MDIC_OP_READ (2u << 26)
#define MDIC_OP_WRITE (1u << 26)
#define MDIC_READY (1u << 28)
#define MDIC_ERROR (1u << 30)
#define EXTCNF_SWFLAG (1u << 5) /* software (vs. ME firmware) owns the PHY  */

/* PHY ULP / SMBus exit (PCH parts), values from Linux e1000e ich8lan. */
#define CTRL_EXT_FORCE_SMBUS 0x00000800u
#define CTRL_EXT_LPCD 0x00000004u /* LANPHYPC power-up done            */
#define CTRL_EXT_DRV_LOAD                                                                                              \
  0x10000000u                         /* tell the ME the host driver owns us                                           \
                                       */
#define CTRL_EXT_PHYPDEN 0x00100000u  /* PHY power-down enable at D3        */
#define CTRL_EXT_SPD_BYPS 0x00008000u /* speed-select bypass (K1 config)   */
#define CTRL_MEHE 0x00080000u         /* memory error handling enable      */
#define TCTL_MULR 0x10000000u         /* multiple request support          */
#define RFCTL_NFSW_DIS 0x00000040u
#define RFCTL_NFSR_DIS 0x00000080u
#define PBECCSTS_ECC_ENABLE 0x00010000u
#define CTRL_EXT_RO_DIS 0x00020000u   /* disable relaxed ordering          */
#define PCIE_NO_SNOOP_ALL 0x0000003Fu /* GCR bits 0-5: per-DMA no-snoop    */
#define CTRL_SPD_100 0x00000100u
#define CTRL_SPD_1000 0x00000200u

/* Kumeran register access (KMRNCTRLSTA): offset in bits 16-20, REN to read. */
#define KMRN_OFFSET_SHIFT 16
#define KMRN_OFFSET_MASK 0x001F0000u
#define KMRN_REN 0x00200000u
#define KMRN_K1_CONFIG 0x07
#define KMRN_K1_ENABLE 0x0002
#define FEXTNVM3_PHY_CFG_COUNTER_MASK 0x0C000000u
#define FEXTNVM3_PHY_CFG_COUNTER_50MSEC 0x08000000u
#define ICH_FWSM_FW_VALID 0x00008000u     /* ME firmware present              */
#define FWSM_ULP_CFG_DONE 0x00000400u     /* ME still (re)configuring ULP     */
#define H2ME_ULP 0x00000800u              /* request: leave ULP               */
#define H2ME_ENFORCE_SETTINGS 0x00001000u /* our settings override the ME's */
#define FWSM_PCIM2PCI 0x01000000u         /* ME mid-transaction; TDT writes drop */

#define RAH_AV (1u << 31) /* receive address valid                   */

/* RCTL bits — BSIZE=00 with BSEX=0 gives a 2048-byte receive buffer. */
#define RCTL_EN (1u << 1)     /* receiver enable                         */
#define RCTL_BAM (1u << 15)   /* accept broadcast                        */
#define RCTL_SECRC (1u << 26) /* strip Ethernet CRC (FCS) on receive     */

/* TCTL bits */
#define TCTL_EN (1u << 1)       /* transmitter enable                      */
#define TCTL_PSP (1u << 3)      /* pad short packets to 60 bytes           */
#define TCTL_CT (0x10u << 4)    /* collision threshold (recommended 0x10)  */
#define TCTL_COLD (0x40u << 12) /* collision distance, full duplex (0x40)  */
#define TCTL_RTLC (1u << 24)    /* re-transmit on late collision           */

/* Sunrise-Point / Kaby-Lake (I219) TX errata: the on-chip IOSF arbiter hangs
 * transmit with 3 outstanding requests; drop it to 2. Without this the I219
 * transmits *nothing* (GPTC/TPT stay 0) while receive works fine. */
#define RCTL_RDMTS_HEX 0x00010000u
#define TARC0_CB_MULTIQ_3_REQ 0x30000000u
#define TARC0_CB_MULTIQ_2_REQ 0x20000000u

/* e1000e's TXDCTL value for PCH parts: descriptor granularity (bit24) + count
 * (bit22), HTHRESH=1, WTHRESH=1 (writeback per descriptor, avoids TX stalls),
 * PTHRESH=0x1f. Without a sane prefetch threshold the transmit engine can sit
 * idle and never advance TDH. */
#define TXDCTL_DMA_BURST 0x0141011Fu

/* interrupt cause / mask bits */
#define INT_LSC (1u << 2)    /* link status change                      */
#define INT_RXDMT0 (1u << 4) /* RX descriptor min threshold             */
#define INT_RXO (1u << 6)    /* receiver overrun                        */
#define INT_RXT0 (1u << 7)   /* receiver timer (a frame arrived)        */

/* descriptor status / command bits */
#define RXD_DD (1u << 0)       /* descriptor done                         */
#define RXD_EOP (1u << 1)      /* end of packet                           */
#define TXD_DD (1u << 0)       /* descriptor done (written back)          */
#define TXD_CMD_EOP (1u << 0)  /* end of packet                           */
#define TXD_CMD_IFCS (1u << 1) /* insert FCS                              */
#define TXD_CMD_RS (1u << 3)   /* report status (set DD when done)        */

/* Legacy descriptor formats (16 bytes each). */
struct rx_desc {
  uint64_t addr;
  uint16_t length;
  uint16_t checksum;
  uint8_t status;
  uint8_t errors;
  uint16_t special;
} __attribute__((packed));

struct tx_desc {
  uint64_t addr;
  uint16_t length;
  uint8_t cso;
  uint8_t cmd;
  uint8_t status;
  uint8_t css;
  uint16_t special;
} __attribute__((packed));

#define RX_DESCS 32 /* 32 * 16 = 512 bytes (a multiple of 128)   */
#define TX_DESCS 16 /* 16 * 16 = 256 bytes (a multiple of 128)   */
#define BUF_SIZE 2048

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

static volatile uint8_t *mmio; /* BAR0, mapped uncached                   */
static uint8_t mac[6];
static bool present;
static bool pch_part; /* 1 for I217/I218/I219 (needs PCH WAs)     */
static uint16_t device_id;

static struct rx_desc *rx_ring; /* RX_DESCS entries, page-aligned          */
static struct tx_desc *tx_ring; /* TX_DESCS entries, page-aligned          */
static uint8_t *rx_bufs[RX_DESCS];
static uint8_t *tx_bufs[TX_DESCS];
static uint32_t rx_cur, tx_cur;

/* MMIO accessors. The mapping is UC, so plain volatile loads/stores are not
 * cached or reordered against each other. */
static inline void reg_write(uint32_t off, uint32_t val) { *(volatile uint32_t *)(mmio + off) = val; }
static inline uint32_t reg_read(uint32_t off) { return *(volatile uint32_t *)(mmio + off); }

/* ========================================================== PCH bring-up ==
 * I217/I218/I219 are PCH-integrated MACs. Unlike the discrete 8254x parts,
 * firmware can leave their CSR window powered down (PCI D3) and their PHY
 * parked in a low-power state, so they need an explicit wake: PCI power state
 * D0, then a LANPHYPC pin pulse to restart the PHY clock. None of this can be
 * exercised under QEMU (no I219 model), so each step is bounded by a timeout
 * and appended to a diagnostic log readable after boot via `ethdiag` — the
 * laptop has no serial port and the early console isn't visible before the
 * window manager comes up. */

static char diag[1024];
static unsigned diag_len;
static unsigned line_start; /* index in diag where this line began   */

/* Append to the diagnostic log (kept for the `ethdiag` command) and mirror to
 * the console funnel. con_putc handles on-screen visibility for every mode: the
 * 0xB8000 text grid in text mode, and the framebuffer boot console
 * (fb_con_putc) in graphics mode when verbose — so the NIC bring-up shows on a
 * laptop panel the same single way every other subsystem's log does, with no
 * NIC-specific framebuffer drawing of its own. */
static void dlog(const char *s)
{
  while (*s) {
    char c = *s++;
    con_putc(c);
    if (c == '\n') {
      if (diag_len < sizeof(diag) - 1)
        diag[diag_len++] = '\n';
      line_start = diag_len;
    } else if (diag_len < sizeof(diag) - 1) {
      diag[diag_len++] = c;
    }
  }
  diag[diag_len] = 0;
}
static void dlogx(uint32_t v)
{
  static const char h[] = "0123456789abcdef";
  char t[11] = "0x00000000";
  for (int i = 0; i < 8; i++)
    t[2 + i] = h[(v >> ((7 - i) * 4)) & 0xF];
  dlog(t);
}
static void dlogb(uint8_t v) /* two hex digits, no "0x" (MAC bytes)  */
{
  static const char h[] = "0123456789abcdef";
  char t[3] = {h[v >> 4], h[v & 0xF], 0};
  dlog(t);
}

/* Byte/word PCI config access, derived from the dword primitives in pci.c. */
static uint8_t pci_read8(uint8_t b, uint8_t s, uint8_t f, uint8_t off)
{
  return (pci_read32(b, s, f, off & ~3u) >> ((off & 3) * 8)) & 0xFF;
}
static uint16_t pci_read16(uint8_t b, uint8_t s, uint8_t f, uint8_t off)
{
  return (pci_read32(b, s, f, off & ~3u) >> ((off & 3) * 8)) & 0xFFFF;
}
static void pci_write16(uint8_t b, uint8_t s, uint8_t f, uint8_t off, uint16_t v)
{
  uint8_t aligned = off & ~3u;
  unsigned shift = (off & 3) * 8;
  uint32_t d = pci_read32(b, s, f, aligned);
  d = (d & ~(0xFFFFu << shift)) | ((uint32_t)v << shift);
  pci_write32(b, s, f, aligned, d);
}

/* Interrupt-free millisecond delay using PIT channel 2 in one-shot (mode 0),
 * polling its OUT bit on port 0x61. The NIC probe runs long before timer_init,
 * so timer_sleep (which blocks on the timer IRQ) would hang the boot here. */
static void pch_mdelay(uint32_t ms)
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

/* Bring the function to PCI power state D0 via its Power Management capability.
 * A NIC firmware parked in D3 ignores MMIO until it's powered back up. This is
 * pure config-space, so it's safe to run before the CSR window is alive. */
static void pci_set_d0(uint8_t b, uint8_t s, uint8_t f)
{
  if (!(pci_read16(b, s, f, 0x06) & (1u << 4))) /* capability list present? */
    return;
  uint8_t cap = pci_read8(b, s, f, 0x34) & 0xFC;
  for (int i = 0; cap && i < 48; i++) {
    if (pci_read8(b, s, f, cap) == 0x01) { /* PM capability id         */
      uint16_t pmcsr = pci_read16(b, s, f, cap + 4);
      if ((pmcsr & 0x3) != 0) {
        pci_write16(b, s, f, cap + 4, pmcsr & ~0x3u);
        pch_mdelay(10); /* D3 -> D0 recovery time   */
        dlog("  powered up from D3\n");
      }
      return;
    }
    cap = pci_read8(b, s, f, cap + 1) & 0xFC;
  }
}

/* Pulse the LANPHYPC pin: drive it low briefly, then release the override, to
 * restart the PCH PHY's clock out of its low-power state (the heart of
 * e1000e's e1000_toggle_lanphypc_pch_lpt). */
static void pch_lanphypc(void)
{
  /* Program the PHY config counter to 50ms before pulsing (e1000e order). */
  uint32_t f3 = reg_read(E1000_FEXTNVM3);
  f3 &= ~FEXTNVM3_PHY_CFG_COUNTER_MASK;
  f3 |= FEXTNVM3_PHY_CFG_COUNTER_50MSEC;
  reg_write(E1000_FEXTNVM3, f3);

  /* Drive LANPHYPC low briefly, then release the override. */
  uint32_t c = reg_read(E1000_CTRL);
  c |= CTRL_LANPHYPC_OVERRIDE;
  c &= ~CTRL_LANPHYPC_VALUE;
  reg_write(E1000_CTRL, c);
  (void)reg_read(E1000_STATUS); /* flush the posted write           */
  pch_mdelay(1);                /* hold low (spec: ~10us, longer ok)*/
  c &= ~CTRL_LANPHYPC_OVERRIDE; /* release; pin returns to hardware */
  reg_write(E1000_CTRL, c);
  (void)reg_read(E1000_STATUS);

  /* Wait for the PHY power-up to actually complete (CTRL_EXT.LPCD), up to
   * ~100ms, then a final settle. Without this the PHY is still mid-power-up
   * when we move on — link/MDIO come up degraded or not at all. */
  for (int count = 0; count < 20 && !(reg_read(E1000_CTRL_EXT) & CTRL_EXT_LPCD); count++)
    pch_mdelay(5);
  pch_mdelay(30);
}

/* PHY/MDIO ownership: take the EXTCNF_CTRL software flag, arbitrating against
 * the ME (e1000_acquire_swflag_ich8lan). The flag MUST be held across the whole
 * SMBus->PCIe transition — grabbing/releasing per read lets the ME flip the PHY
 * back to SMBus between steps. */
static int phy_acquire(void)
{
  int i;
  for (i = 0; (reg_read(E1000_EXTCNF_CTRL) & EXTCNF_SWFLAG) && i < 100; i++)
    pch_mdelay(1); /* wait for the ME to release it           */
  reg_write(E1000_EXTCNF_CTRL, reg_read(E1000_EXTCNF_CTRL) | EXTCNF_SWFLAG);
  for (i = 0; i < 1000; i++) {
    if (reg_read(E1000_EXTCNF_CTRL) & EXTCNF_SWFLAG)
      return 0; /* we own it                               */
    pch_mdelay(1);
  }
  return -1;
}
static void phy_release(void) { reg_write(E1000_EXTCNF_CTRL, reg_read(E1000_EXTCNF_CTRL) & ~EXTCNF_SWFLAG); }

/* One MDIC PHY-register read; caller must already hold the flag. -1 on error.
 */
static int phy_read_locked(uint8_t addr, uint8_t reg)
{
  reg_write(E1000_MDIC, ((uint32_t)reg << MDIC_REG_SHIFT) | ((uint32_t)addr << MDIC_PHY_SHIFT) | MDIC_OP_READ);
  for (uint32_t i = 0; i < 100000u; i++) {
    uint32_t m = reg_read(E1000_MDIC);
    if (m & MDIC_READY)
      return (m & MDIC_ERROR) ? -1 : (int)(m & 0xFFFF);
  }
  return -1;
}

/* One MDIC PHY-register write; caller must already hold the flag. */
static void phy_write_locked(uint8_t addr, uint8_t reg, uint16_t val)
{
  reg_write(E1000_MDIC,
            (uint32_t)val | ((uint32_t)reg << MDIC_REG_SHIFT) | ((uint32_t)addr << MDIC_PHY_SHIFT) | MDIC_OP_WRITE);
  for (uint32_t i = 0; i < 100000u; i++)
    if (reg_read(E1000_MDIC) & MDIC_READY)
      break;
}

/* Self-contained read for the diagnostic scan (acquires + releases the flag).
 */
static int phy_read(uint8_t addr, uint8_t reg)
{
  if (phy_acquire() != 0)
    return -1;
  int v = phy_read_locked(addr, reg);
  phy_release();
  return v;
}

static uint8_t phy_addr; /* MDIO address the PHY answered at        */
static uint32_t phy_id;  /* full 32-bit PHY identity, once found    */

/* Is the PHY answering MDIO? Reads PHYID1/2 at address 1 then 2 (caller holds
 * the flag). Records the address + id on success (e1000_phy_is_accessible). */
static bool phy_accessible_locked(void)
{
  for (uint8_t a = 1; a <= 2; a++) {
    int id1 = phy_read_locked(a, 2);
    if (id1 <= 0 || id1 == 0xFFFF)
      continue;
    int id2 = phy_read_locked(a, 3);
    if (id2 < 0 || id2 == 0xFFFF)
      continue;
    phy_addr = a;
    phy_id = ((uint32_t)id1 << 16) | (uint16_t)id2;
    return true;
  }
  return false;
}

/* Bring the MAC-PHY interconnect out of SMBus into PCIe mode so MDIO reaches
 * the PHY (e1000e e1000_init_phy_workarounds_pchlan). Faithful version: keep
 * PHY config gated the whole time, hold the MDIO flag across every step, do the
 * force-SMBus -> LANPHYPC toggle -> unforce-SMBus as ONE linear sequence
 * (checking accessibility after each), then a PHY hardware reset. Without a
 * working interconnect the PHY only does a degraded 10Mb link and TX never runs
 * (observed: phy scan reads 0 at every address). */
static void pch_phy_workarounds(void)
{
  /* Gate hardware PHY auto-config for the whole transition, or the ME/hardware
   * keeps flipping the PHY back to SMBus underneath us. */
  reg_write(E1000_EXTCNF_CTRL, reg_read(E1000_EXTCNF_CTRL) | EXTCNF_GATE_PHY_CFG);

  if (phy_acquire() != 0) {
    dlog("  PHY: cannot take MDIO flag\n");
    return;
  }

  bool ok = phy_accessible_locked();
  if (!ok) {
    dlog("  PHY: force SMBus\n");
    reg_write(E1000_CTRL_EXT, reg_read(E1000_CTRL_EXT) | CTRL_EXT_FORCE_SMBUS);
    pch_mdelay(50);
    ok = phy_accessible_locked();
  }
  if (!ok) {
    dlog("  PHY: LANPHYPC toggle\n");
    pch_lanphypc();
    ok = phy_accessible_locked();
  }
  if (!ok) {
    dlog("  PHY: unforce SMBus\n");
    reg_write(E1000_CTRL_EXT, reg_read(E1000_CTRL_EXT) & ~CTRL_EXT_FORCE_SMBUS);
    ok = phy_accessible_locked();
  }
  phy_release();

  if (!ok) {
    dlog("  PHY still inaccessible (SMBus stuck)\n");
    return;
  }

  dlog("  PHY accessible at addr ");
  dlogx(phy_addr);
  dlog(" id=");
  dlogx(phy_id);
  dlog("\n");

  /* PHY hardware reset for a known-good state (toggle CTRL.PHY_RST). */
  uint32_t c = reg_read(E1000_CTRL);
  reg_write(E1000_CTRL, c | CTRL_PHY_RST);
  (void)reg_read(E1000_STATUS);
  pch_mdelay(1);
  reg_write(E1000_CTRL, c & ~CTRL_PHY_RST);
  (void)reg_read(E1000_STATUS);
  pch_mdelay(10);
}

/* Now that MDIO reaches the PHY, renegotiate a clean copper link: advertise
 * gigabit and restart autoneg (the link inherited from SMBus mode was stuck at
 * 10Mb). Then wait (bounded ~4s) for the PHY to report link, so the MAC has a
 * real negotiated speed to transmit at by the time DHCP runs. No-op if the PHY
 * never became accessible. */
static void pch_phy_link_setup(void)
{
  if (phy_id == 0) {
    dlog("  no PHY; skipping link setup\n");
    return;
  }
  if (phy_acquire() != 0)
    return;
  int gbcr = phy_read_locked(phy_addr, 9); /* 1000BASE-T control       */
  if (gbcr >= 0)
    phy_write_locked(phy_addr, 9, (uint16_t)((gbcr & 0xFCFF) | 0x0200));
  phy_write_locked(phy_addr, 0, 0x1200); /* AN enable + AN restart   */
  phy_release();
  dlog("  PHY autoneg restarted; waiting for link\n");

  bool up = false;
  for (int i = 0; i < 400; i++) { /* up to ~4s                */
    if (phy_acquire() == 0) {
      int bmsr = phy_read_locked(phy_addr, 1);
      phy_release();
      if (bmsr > 0 && (bmsr & 0x0004)) {
        up = true;
        break;
      } /* link up   */
    }
    pch_mdelay(10);
  }
  dlog(up ? "  PHY link up\n" : "  PHY link timeout\n");
}

/* Un-gate hardware PHY config and let the MAC track the PHY for link/speed. We
 * kept PHY config GATED through the SMBus->PCIe transition (so the hardware
 * couldn't revert the PHY mid-sequence) — but while it stays gated the MAC
 * can't sync link/speed from the PHY over the internal interface, so STATUS
 * never sees a real link and the transmit engine never gets a clock (forcing
 * CTRL.SLU/speed made STATUS lie but didn't fool the TX engine — it never
 * fetched a descriptor). Un-gate and clear the force bits so the MAC
 * establishes the real internal link.*/
static void pch_force_mac_speed(void)
{
  if (phy_id == 0)
    return;

  reg_write(E1000_EXTCNF_CTRL, reg_read(E1000_EXTCNF_CTRL) & ~EXTCNF_GATE_PHY_CFG);

  uint32_t ctrl = reg_read(E1000_CTRL);
  ctrl |= CTRL_SLU | CTRL_ASDE; /* link up + auto-speed-detect */
  ctrl &= ~(CTRL_FRCSPD | CTRL_FRCDPLX | CTRL_LRST | CTRL_PHY_RST | CTRL_ILOS);
  reg_write(E1000_CTRL, ctrl);

  dlog("  ungated PHY cfg; MAC tracking link\n");
}

/* Kumeran (MAC<->PHY internal link) register read/write; caller holds the flag.
 * Offset goes in bits 16-20; reads set REN then read back the low 16 bits. */
static int kmrn_read_locked(uint8_t off)
{
  reg_write(E1000_KMRNCTRLSTA, (((uint32_t)off << KMRN_OFFSET_SHIFT) & KMRN_OFFSET_MASK) | KMRN_REN);
  (void)reg_read(E1000_STATUS); /* flush */
  pch_mdelay(1);
  return (int)(reg_read(E1000_KMRNCTRLSTA) & 0xFFFF);
}
static void kmrn_write_locked(uint8_t off, uint16_t data)
{
  reg_write(E1000_KMRNCTRLSTA, (((uint32_t)off << KMRN_OFFSET_SHIFT) & KMRN_OFFSET_MASK) | data);
  (void)reg_read(E1000_STATUS);
  pch_mdelay(1);
}

/* Configure the Kumeran K1 power state (e1000e e1000_configure_k1_ich8lan).
 * Disabling K1 takes the MAC<->PHY link out of its low-power idle — the
 * suspected reason the link passes RX but the transmit engine never starts. */
static void pch_configure_k1(bool k1_enable)
{
  if (phy_acquire() != 0)
    return;
  int kmrn = kmrn_read_locked(KMRN_K1_CONFIG);
  uint16_t kr = (uint16_t)kmrn;
  if (k1_enable)
    kr |= KMRN_K1_ENABLE;
  else
    kr &= ~KMRN_K1_ENABLE;
  kmrn_write_locked(KMRN_K1_CONFIG, kr);
  phy_release();
  pch_mdelay(1);

  /* Pulse FRCSPD + CTRL_EXT.SPD_BYPS so the new K1 setting takes effect, then
   * restore CTRL/CTRL_EXT to what the caller set. */
  uint32_t ctrl_ext = reg_read(E1000_CTRL_EXT);
  uint32_t ctrl_reg = reg_read(E1000_CTRL);
  reg_write(E1000_CTRL, (ctrl_reg & ~(CTRL_SPD_1000 | CTRL_SPD_100)) | CTRL_FRCSPD);
  reg_write(E1000_CTRL_EXT, ctrl_ext | CTRL_EXT_SPD_BYPS);
  (void)reg_read(E1000_STATUS);
  pch_mdelay(1);
  reg_write(E1000_CTRL, ctrl_reg);
  reg_write(E1000_CTRL_EXT, ctrl_ext);
  (void)reg_read(E1000_STATUS);
  pch_mdelay(1);
  dlog(k1_enable ? "  K1 enabled\n" : "  K1 disabled\n");
}

/* Take the device out of Ultra-Low-Power mode via the Management Engine — the
 * firmware path of e1000e's e1000_disable_ulp_lpt_lp. It's pure MAC-CSR I/O
 * (H2ME/FWSM), no PHY MDIO, so it's safe to run while the PHY is still parked
 * (which is exactly when a PHY-id read comes back 0). Only the ME can do this,
 * so it's a no-op without FWSM.FW_VALID. Bounded; never hangs. */
static void pch_disable_ulp(void)
{
  if (!(reg_read(E1000_FWSM) & ICH_FWSM_FW_VALID)) {
    dlog("  no ME firmware; cannot un-ULP via mailbox\n");
    return;
  }

  /* Ask the ME to unconfigure ULP, enforcing our request over its own state. */
  uint32_t h2me = reg_read(E1000_H2ME);
  h2me &= ~H2ME_ULP;
  h2me |= H2ME_ENFORCE_SETTINGS;
  reg_write(E1000_H2ME, h2me);

  /* Wait (bounded, ~400 ms) for the ME to finish: ULP_CFG_DONE clears. */
  int i = 0;
  while (reg_read(E1000_FWSM) & FWSM_ULP_CFG_DONE) {
    if (++i > 40) {
      dlog("  ULP cfg-done timeout\n");
      break;
    }
    pch_mdelay(10);
  }

  /* Stop enforcing so the ME can manage power again afterwards. */
  h2me = reg_read(E1000_H2ME);
  h2me &= ~H2ME_ENFORCE_SETTINGS;
  reg_write(E1000_H2ME, h2me);

  dlog("  ULP unconfigure requested\n");
}

/* Bring the MAC through a proper global reset (e1000e e1000_reset_hw_ich8lan).
 * This is what initializes the transmit DMA engine: without it the I219 leaves
 * TX halted (TDH stuck at 0, nothing sent) even with TCTL.EN set and RX
 * working. Skipped on the first attempt because it froze — but that was an
 * *ungated* reset followed by an immediate MMIO read, which e1000e warns hangs
 * the hardware. Done correctly here: quiesce DMA, disable the units, gate PHY
 * config, pulse CTRL.RST, then wait 20ms WITHOUT touching MMIO before reading
 * again. The caller must have captured the MAC first (the reset clears
 * RAL/RAH).*/
static void pch_reset(void)
{
  dlog("  MAC reset (PCH)\n");

  /* Quiesce bus-master DMA so the reset doesn't catch a transfer mid-flight. */
  reg_write(E1000_CTRL, reg_read(E1000_CTRL) | CTRL_GIO_MASTER_DISABLE);
  for (int g = 0; (reg_read(E1000_STATUS) & STATUS_GIO_MASTER_EN) && g < 100000; g++)
    ;

  /* Disable the receive and transmit units, then let them settle. */
  reg_write(E1000_RCTL, 0);
  reg_write(E1000_TCTL, TCTL_PSP);
  pch_mdelay(10);
  reg_write(E1000_IMC, 0xFFFFFFFF);

  /* Gate automatic PHY config so the reset can't hang reconfiguring the PHY. */
  uint32_t ext = reg_read(E1000_EXTCNF_CTRL);
  reg_write(E1000_EXTCNF_CTRL, ext | EXTCNF_GATE_PHY_CFG);

  /* Global reset. CRITICAL: touch no register for ~20ms afterwards — e1000e
   * notes a flush here hangs the hardware (this is what froze the first try).
   */
  reg_write(E1000_CTRL, reg_read(E1000_CTRL) | CTRL_RST);
  pch_mdelay(20);

  /* MMIO is safe again. Ungate PHY config and clear any pending interrupts. */
  ext = reg_read(E1000_EXTCNF_CTRL);
  reg_write(E1000_EXTCNF_CTRL, ext & ~EXTCNF_GATE_PHY_CFG);
  reg_write(E1000_IMC, 0xFFFFFFFF);
  (void)reg_read(E1000_ICR);

  dlog("  reset done\n");
}

/* True for the I219 (Sunrise-Point/Kaby-Lake-class) parts that need the TX
 * arbiter errata — i.e. every recognized PCH part except I217/I218. */
static bool is_pch_spt(uint16_t dev)
{
  switch (dev) {
  case 0x153A:
  case 0x153B: /* I217 (LPT)               */
  case 0x15A0:
  case 0x15A1:
  case 0x15A2:
  case 0x15A3: /* I218 (LPT)           */
    return false;
  case 0x100E:
  case 0x100F:
  case 0x10D3: /* QEMU/VMware discrete     */
    return false;
  default:
    return true; /* I219 (SPT and later)     */
  }
}

/* Apply the SPT/KBL transmit arbiter errata (e1000e e1000_configure_tx, pch_spt
 * branch): cap IOSF outstanding requests at 2, or the I219 transmits nothing.
 */
static void pch_tx_errata(void)
{
  if (!is_pch_spt(device_id))
    return;
  reg_write(E1000_IOSFPC, reg_read(E1000_IOSFPC) | RCTL_RDMTS_HEX);
  uint32_t tarc = reg_read(E1000_TARC0);
  tarc &= ~TARC0_CB_MULTIQ_3_REQ;
  tarc |= TARC0_CB_MULTIQ_2_REQ;
  reg_write(E1000_TARC0, tarc);
}

/* The undocumented "magic" init bits e1000e pokes in e1000_initialize_hw_bits_
 * ich8lan — required for the transmit arbiter/engine to actually run. We'd only
 * ever set the SPT CB_MULTIQ errata; these TARC0/TARC1/TXDCTL/CTRL_EXT bits are
 * separate and the strongest remaining suspect for "TX engine never fetches a
 * descriptor, even in loopback". Values for the pch_lpt+ (I219) path, verified
 * verbatim against torvalds/linux. */
static void pch_initialize_hw_bits(void)
{
  reg_write(E1000_CTRL_EXT, reg_read(E1000_CTRL_EXT) | (1u << 22) | CTRL_EXT_PHYPDEN);
  reg_write(E1000_TXDCTL, reg_read(E1000_TXDCTL) | (1u << 22));
  reg_write(E1000_TXDCTL1, reg_read(E1000_TXDCTL1) | (1u << 22));

  uint32_t t0 = reg_read(E1000_TARC0);
  t0 |= (1u << 23) | (1u << 24) | (1u << 26) | (1u << 27);
  reg_write(E1000_TARC0, t0);

  uint32_t t1 = reg_read(E1000_TARC1);
  if (reg_read(E1000_TCTL) & TCTL_MULR)
    t1 &= ~(1u << 28);
  else
    t1 |= (1u << 28);
  t1 |= (1u << 24) | (1u << 26) | (1u << 30);
  reg_write(E1000_TARC1, t1);

  /* RX NFS-filter disable WA + packet-buffer ECC enable (pch_lpt+). */
  reg_write(E1000_RFCTL, reg_read(E1000_RFCTL) | RFCTL_NFSW_DIS | RFCTL_NFSR_DIS);
  reg_write(E1000_PBECCSTS, reg_read(E1000_PBECCSTS) | PBECCSTS_ECC_ENABLE);
  reg_write(E1000_CTRL, reg_read(E1000_CTRL) | CTRL_MEHE);

  /* Enable PCIe DMA cache snooping (clear GCR no-snoop bits) + disable relaxed
   * ordering (e1000e e1000e_set_pcie_no_snoop + CTRL_EXT.RO_DIS). Without this
   * the chip's DMA reads bypass the CPU cache, so it reads a STALE TX
   * descriptor (the just-written one is still cached) and parks TDH at 0
   * forever — RX works only because its descriptors were written at boot and
   * long since flushed. */
  reg_write(E1000_GCR, reg_read(E1000_GCR) & ~PCIE_NO_SNOOP_ALL);
  reg_write(E1000_CTRL_EXT, reg_read(E1000_CTRL_EXT) | CTRL_EXT_RO_DIS);

  dlog("  set hw init bits + DMA snoop\n");
}

/* ----------------------------------------------------------------- MAC read */

/* EEPROM word read, used only if RAL/RAH was not pre-loaded. The done-bit and
 * address shift differ across chips, so try the 8254x layout then the 82574
 * one. Returns 0 and the word on success. */
static int eeprom_read(uint16_t addr, uint16_t *out)
{
  reg_write(E1000_EERD, ((uint32_t)addr << 8) | 1u); /* 8254x: addr<<8     */
  for (int i = 0; i < 100000; i++) {
    uint32_t v = reg_read(E1000_EERD);
    if (v & (1u << 4)) {
      *out = (uint16_t)(v >> 16);
      return 0;
    }
  }
  reg_write(E1000_EERD, ((uint32_t)addr << 2) | 1u); /* 82574: addr<<2     */
  for (int i = 0; i < 100000; i++) {
    uint32_t v = reg_read(E1000_EERD);
    if (v & (1u << 1)) {
      *out = (uint16_t)(v >> 16);
      return 0;
    }
  }
  return -1;
}

static void read_mac(void)
{
  uint32_t ral = reg_read(E1000_RAL);
  uint32_t rah = reg_read(E1000_RAH);

  if (rah & RAH_AV) { /* firmware/QEMU already loaded the address  */
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
    mac[i * 2] = w & 0xFF;
    mac[i * 2 + 1] = (w >> 8) & 0xFF;
  }
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
    rx_ring[i].addr = (uint32_t)(uintptr_t)rx_bufs[i];
    rx_ring[i].status = 0;
  }

  reg_write(E1000_RDBAL, (uint32_t)(uintptr_t)rx_ring);
  reg_write(E1000_RDBAH, 0);
  reg_write(E1000_RDLEN, RX_DESCS * sizeof(struct rx_desc));
  reg_write(E1000_RDH, 0);
  reg_write(E1000_RDT, RX_DESCS - 1); /* all but one descriptor are the card's */
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
    tx_ring[i].addr = (uint32_t)(uintptr_t)tx_bufs[i];
    tx_ring[i].status = 0;
  }

  reg_write(E1000_TDBAL, (uint32_t)(uintptr_t)tx_ring);
  reg_write(E1000_TDBAH, 0);
  reg_write(E1000_TDLEN, TX_DESCS * sizeof(struct tx_desc));
  reg_write(E1000_TDH, 0);
  reg_write(E1000_TDT, 0);
  tx_cur = 0;
  if (pch_part)
    reg_write(E1000_TXDCTL, TXDCTL_DMA_BURST); /* arm the prefetch engine    */
  reg_write(E1000_TIPG, 0x0060200A);           /* IPGT=10, IPGR1=8, IPGR2=6 (copper) */
  reg_write(E1000_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD | TCTL_RTLC);
  pch_tx_errata(); /* I219 transmits nothing without this */
  return 0;
}

/* --------------------------------------------------------------- transmit */

static void e1000_send(const uint8_t *frame, uint32_t len)
{
  if (!present || len > 1514)
    return;

  uint32_t fl = irq_save();

  uint32_t i = tx_cur;
  memcpy(tx_bufs[i], frame, len);
  tx_ring[i].addr = (uint32_t)(uintptr_t)tx_bufs[i];
  tx_ring[i].length = (uint16_t)len; /* PSP pads runts to 60 in hardware   */
  tx_ring[i].cmd = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;
  tx_ring[i].status = 0;

  tx_cur = (i + 1) % TX_DESCS;

  /* PCH PCIm2PCI arbiter workaround (e1000e __ew32_prepare): the ME shares the
   * PCIe path; if it's mid-transaction (FWSM.PCIM2PCI set) when we write TDT,
   * the write is silently dropped and the transmit engine never fetches the
   * descriptor — TDH stays put, nothing is sent (observed on the I219: TDH=0,
   * TDT advancing, TPT=0). Wait for the bit to clear first. PCH-only; on other
   * parts it's never set, so this falls straight through. */
  if (pch_part)
    for (int g = 0; (reg_read(E1000_FWSM) & FWSM_PCIM2PCI) && g < 200000; g++)
      ;
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

    if ((d->status & RXD_EOP) && d->errors == 0 && len >= 14 && len <= BUF_SIZE)
      net_rx_frame(rx_bufs[rx_cur], len); /* SECRC already dropped FCS  */

    d->status = 0;
    uint32_t done = rx_cur;
    rx_cur = (rx_cur + 1) % RX_DESCS;
    reg_write(E1000_RDT, done); /* hand the emptied descriptor back       */
  }
}

static void e1000_irq(struct registers *r)
{
  (void)r;
  (void)reg_read(E1000_ICR); /* acknowledge; netrx drains the ring */
}

/* IRQ-independent RX drain, called off the timer tick (see nic_poll). Some
 * hosts (VMware) route this card's INTx to a line our 8259 path never sees, so
 * the RX interrupt never fires and frames just pile up in the ring; polling
 * drains them anyway. IF-guarded so it can't race the real e1000_irq over
 * rx_cur/RDT. The descriptor ring is in cached RAM, but x86 DMA is
 * cache-coherent, so the DD bit the card writes is visible to a plain read. */
static void e1000_poll(void)
{
  if (!present)
    return;
  uint32_t fl = irq_save();
  rx_poll();
  irq_restore(fl);
}

/* --------------------------------------------------------------------- init */

static const uint8_t *e1000_mac(void) { return mac; }

static const char *model_name(uint16_t dev)
{
  switch (dev) {
  case 0x100E:
    return "82540EM";
  case 0x100F:
    return "82545EM";
  case 0x10D3:
    return "82574L";
  case 0x153A:
  case 0x153B:
    return "I217";
  case 0x15A0:
  case 0x15A1:
  case 0x15A2:
  case 0x15A3:
    return "I218";
  default:
    return "I219";
  }
}

/* I217/I218/I219 are PCH-integrated MACs: the MAC and PHY sit behind the
 * chipset's Management Engine, and the CSR window is dead until a LANPHYPC
 * pulse
 * + ME-handshake bring-up powers it on. We don't implement that sequence, and
 * on bare metal the *first* MMIO read of a not-yet-powered CSR window doesn't
 * fault — it never completes, hanging the CPU on the bus cycle (a frozen boot
 * with no panic). So we recognize these parts but refuse to touch their MMIO.
 * The QEMU parts (82540EM/82574L) and the VMware 82545EM are real PCI(e)
 * devices whose CSRs are live immediately, so they're driven normally. */
static bool is_pch_lan(uint16_t dev) { return dev != 0x100E && dev != 0x100F && dev != 0x10D3; }

static int e1000_probe(void)
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
        for (unsigned k = 0; k < sizeof(intel_nics) / sizeof(intel_nics[0]); k++) {
          if (dev == intel_nics[k]) {
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
    return 0; /* silent: another driver may own the NIC      */

  bool pch = is_pch_lan(device_id);
  pch_part = pch;
  diag_len = 0;
  diag[0] = 0;
  line_start = 0;
  dlog("net: Intel ");
  dlog(model_name(device_id));
  dlog(pch ? " (PCH-integrated)\n" : "\n");

#if !E1000_PCH_WAKE
  if (pch) {
    dlog("  PCH wake disabled at build; skipping\n");
    return 0;
  }
#endif

  uint32_t bar0 = pci_read32(bus, slot, func, 0x10);
  uint8_t irq = (uint8_t)(pci_read32(bus, slot, func, 0x3C) & 0xFF);
  uint32_t mmio_phys = bar0 & ~0xFu; /* BAR0 is a memory BAR               */

  /* PCH parts can be parked in a low-power state by firmware. Validate the BAR
   * and bring the device to D0 *before* any MMIO, so the first CSR read lands
   * on a powered window instead of hanging the bus. */
  if (pch) {
    if (mmio_phys == 0 || mmio_phys == 0xFFFFFFF0) {
      dlog("  BAR0 unassigned -- firmware left the NIC off; skipping\n");
      return 0;
    }
    pci_set_d0(bus, slot, func);
  }

  /* Enable memory-space decoding (bit 1) and bus-master DMA (bit 2). */
  uint32_t cmd = pci_read32(bus, slot, func, 0x04);
  pci_write32(bus, slot, func, 0x04, cmd | 0x06);

  /* Map the register file uncached. The e1000 register window is 128 KiB. */
  if (paging_identity_map(mmio_phys, 0x20000) != 0) {
    dlog("  MMIO map failed\n");
    return 0;
  }
  paging_mark_uc(mmio_phys, 0x20000);
  mmio = (volatile uint8_t *)(uintptr_t)mmio_phys;

  /* The moment of truth on real hardware: the first CSR read. A dead/parked
   * window reads all-ones (a PCIe unsupported-request completion) — detectable
   * and recoverable, not a hang. Then wake the PHY: leave ULP, force the
   * MAC-PHY interconnect out of SMBus into PCIe via a LANPHYPC pulse, and poll
   * the PHY id until it answers (mirrors e1000e's init_phy_workarounds_pchlan).
   * Observed first iteration: STATUS read fine, but the PHY id came back 0
   * (parked in ULP/SMBus) — this is what wakes it. */
  if (pch) {
    dlog("  reading STATUS (first MMIO)\n"); /* shown before the read     */
    uint32_t st = reg_read(E1000_STATUS);
    dlog("  STATUS=");
    dlogx(st);
    dlog("\n");
    if (st == 0xFFFFFFFF) {
      dlog("  CSR window dead after D0; skipping\n");
      return 0;
    }

    dlog("  disabling ULP\n");
    pch_disable_ulp();

    /* Capture the firmware-loaded MAC before the reset clears RAL/RAH, then
     * reset to initialize the TX DMA engine (the reason TX was dead). */
    read_mac();
    pch_reset();

    /* Undocumented TX-arbiter "magic" init bits (e1000e init_hw_bits), set
     * after reset before the link/TX setup — the prime remaining suspect for
     * the transmit engine never fetching a descriptor. */
    pch_initialize_hw_bits();

    /* Bring the MAC-PHY interconnect out of SMBus into PCIe mode (the real
     * fix for the dead-TX/10Mb/MDIO=0 symptom), then renegotiate the link. */
    pch_phy_workarounds();
    pch_phy_link_setup();

    /* Tell the ME firmware the host driver has taken control. On a managed
     * part the ME passes RX but gates host TX until this handshake — the
     * reason TX stays dead with a healthy link. Set after the reset (which
     * clears CTRL_EXT). */
    reg_write(E1000_CTRL_EXT, reg_read(E1000_CTRL_EXT) | CTRL_EXT_DRV_LOAD);
    dlog("  took hw control (DRV_LOAD)\n");

    /* Take the MAC<->PHY internal (Kumeran) link out of K1 low-power idle. */
    pch_configure_k1(false);

    /* Un-gate PHY config and let the MAC track the PHY's real link/speed (so
     * STATUS reflects a true link and the transmit engine gets a clock). */
    pch_force_mac_speed();
  }

  /* Reset the discrete parts here (QEMU/VMware): mask interrupts, pulse
   * CTRL.RST, wait for it to self-clear. PCH parts were already reset above by
   * pch_reset() (which must run gated and pre-PHY-wake to be safe), so don't
   * reset them a second time. */
  if (!pch) {
    dlog("  resetting MAC\n");
    reg_write(E1000_IMC, 0xFFFFFFFF);
    reg_write(E1000_CTRL, reg_read(E1000_CTRL) | CTRL_RST);
    for (int spins = 0; spins < 1000000; spins++)
      if (!(reg_read(E1000_CTRL) & CTRL_RST))
        break;
    reg_write(E1000_IMC, 0xFFFFFFFF); /* reset re-arms some causes           */
  }

  /* Bring the link up. ASDE lets the MAC adopt the speed/duplex the PHY
   * autonegotiates (essential on real copper — the QEMU parts fake link, so
   * SLU alone sufficed there); clear the reset/loopback/force bits so the MAC
   * tracks the PHY instead of a forced mode. PCH parts already had their link
   * forced from the PHY in pch_force_mac_speed(), so leave that alone. */
  if (!pch) {
    uint32_t ctrl = reg_read(E1000_CTRL);
    ctrl |= CTRL_SLU | CTRL_ASDE;
    ctrl &= ~(CTRL_LRST | CTRL_PHY_RST | CTRL_ILOS);
    reg_write(E1000_CTRL, ctrl);
  }

  /* Clear the multicast table so stray hashes don't accept junk. */
  for (int i = 0; i < 128; i++)
    reg_write(E1000_MTA + i * 4, 0);

  if (!pch)
    read_mac(); /* PCH already captured it before pch_reset()      */
  /* (Re)program the unicast filter from the MAC in case reset cleared it. */
  reg_write(E1000_RAL, mac[0] | mac[1] << 8 | mac[2] << 16 | (uint32_t)mac[3] << 24);
  reg_write(E1000_RAH, mac[4] | mac[5] << 8 | RAH_AV);

  dlog("  allocating rings\n");
  if (setup_rx() != 0 || setup_tx() != 0) {
    dlog("  ring allocation failed\n");
    return 0;
  }

  /* Enable receive interrupts and clear any pending cause. */
  dlog("  enabling interrupts\n");
  reg_write(E1000_IMS, INT_RXT0 | INT_RXDMT0 | INT_RXO | INT_LSC);
  reg_read(E1000_ICR);

  irq_register(irq, e1000_irq);
  irq_unmask(irq);
  if (irq >= 8)
    irq_unmask(2); /* cascade line for the slave PIC              */

  present = true;

  con_printf("net: Intel %s at mmio 0x%08X irq %u mac ", model_name(device_id), mmio_phys, irq);
  for (int i = 0; i < 6; i++) {
    print_hex2(mac[i]);
    if (i < 5)
      con_putc(':');
  }
  con_putc('\n');

  /* Record the outcome into the diagnostic log too — on a laptop the summary
   * above scrolls past invisibly, so `ethdiag` is how it's read back. */
  dlog("  up: mac ");
  for (int i = 0; i < 6; i++) {
    dlogb(mac[i]);
    dlog(i < 5 ? ":" : " ");
  }
  uint32_t fst = reg_read(E1000_STATUS);
  dlog("link ");
  dlog((fst & STATUS_LU) ? "up\n" : "down (cable?)\n");
  return 1;
}

/* `ethdiag`: replay the bring-up log and show live link state. The bring-up
 * happens before any console is visible on a laptop, so this is the channel for
 * inspecting how the PCH wake went after reaching the shell. */
/* `txtest`: force MAC-internal loopback (RCTL.LBM=01, forced 1000-FD link) and
 * push one frame through. Loopback bypasses the PHY and the Kumeran link, so it
 * isolates the transmit engine + DMA from the external-link problem. If TDH
 * advances / GPTC counts / the descriptor gets its DD bit here, the engine
 * works and the real-link MAC<->PHY interface is the only thing wrong; if
 * nothing moves even in loopback, the engine itself is gated by something
 * deeper. */
void e1000_txtest(void)
{
  if (!present) {
    con_write("txtest: no NIC\n");
    return;
  }

  uint32_t save_ctrl = reg_read(E1000_CTRL);
  uint32_t save_rctl = reg_read(E1000_RCTL);

  /* Put the PHY itself into loopback, forced to 100-full (MII BMCR: speed=100
   * bit13, full-duplex bit8, loopback bit14, autoneg off). The PHY loops TX
   * back to RX with its own internal clock — exercising the MAC transmit engine
   * AND the MAC<->PHY interface. If TX works here but not in MAC loopback, the
   * engine is fine and the external link is the only problem (winnable). */
  if (phy_id && phy_acquire() == 0) {
    phy_write_locked(phy_addr, 0, 0x6100);
    phy_release();
    pch_mdelay(20);
  }

  /* Re-initialize the TX ring from scratch. */
  reg_write(E1000_TDBAL, (uint32_t)(uintptr_t)tx_ring);
  reg_write(E1000_TDBAH, 0);
  reg_write(E1000_TDLEN, TX_DESCS * sizeof(struct tx_desc));
  reg_write(E1000_TDH, 0);
  reg_write(E1000_TDT, 0);
  tx_cur = 0;
  if (pch_part)
    reg_write(E1000_TXDCTL, TXDCTL_DMA_BURST);
  reg_write(E1000_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD | TCTL_RTLC);
  pch_tx_errata();

  /* MAC forced to 100-full + link up; NO MAC loopback (the PHY does the loop).
   */
  uint32_t ctrl = save_ctrl | CTRL_SLU | CTRL_FD | CTRL_FRCSPD | CTRL_FRCDPLX;
  ctrl = (ctrl & ~(3u << CTRL_SPEED_SHIFT)) | (1u << CTRL_SPEED_SHIFT); /* 100 */
  reg_write(E1000_CTRL, ctrl);
  reg_write(E1000_RCTL, save_rctl & ~0xC0u); /* clear LBM (no MAC loopback) */
  pch_mdelay(20);

  con_printf("txtest(PHY lb): TCTL=0x%08X CTRL=0x%08X STATUS=0x%08X\n", reg_read(E1000_TCTL), reg_read(E1000_CTRL),
             reg_read(E1000_STATUS));

  (void)reg_read(E1000_GPTC); /* clear-on-read: zero counters    */
  (void)reg_read(E1000_TPT);

  uint8_t frame[60];
  for (int i = 0; i < 60; i++)
    frame[i] = (i < 6) ? 0xFF : (uint8_t)(i + 1);
  uint32_t slot = tx_cur;
  e1000_send(frame, 60);
  pch_mdelay(50);

  con_printf("txtest(PHY lb): TDH=%u TDT=%u GPTC=%u TPT=%u status=0x%08X\n", reg_read(E1000_TDH), reg_read(E1000_TDT),
             reg_read(E1000_GPTC), reg_read(E1000_TPT), tx_ring[slot].status);

  /* Take the PHY back out of loopback. */
  if (phy_id && phy_acquire() == 0) {
    phy_write_locked(phy_addr, 0, 0x1200); /* autoneg enable + restart       */
    phy_release();
  }
  reg_write(E1000_CTRL, save_ctrl);
  reg_write(E1000_RCTL, save_rctl);
}

void e1000_diag(void)
{
  if (diag_len == 0) {
    con_write("ethdiag: no Intel NIC probed\n");
    return;
  }
  con_write(diag);
  if (!present)
    return;

  uint32_t st = reg_read(E1000_STATUS);
  static const char *const spd[] = {"10", "100", "1000", "1000"};
  con_printf("live: STATUS=0x%08X%s", st, (st & STATUS_LU) ? " link UP " : " link DOWN ");
  if (st & STATUS_LU) {
    con_printf("%s%s", spd[(st >> 6) & 3], (st & 1) ? "Mb FD" : "Mb HD");
  }
  con_putc('\n');

  /* Hardware packet counters (clear-on-read, so this shows traffic since the
   * last ethdiag). TX>0 means frames really left the MAC; RX>0 means frames
   * really arrived — together they localize a "no traffic" fault to TX, RX, or
   * the link/peer. */
  con_printf("  tx: good=%u total=%u  rx: good=%u total=%u\n"
             "  rx errors: missed=%u crc=%u\n"
             "  rings: RDH=%u RDT=%u TDH=%u TDT=%u\n"
             "  regs: CTRL=0x%08X TCTL=0x%08X RCTL=0x%08X CTRL_EXT=0x%08X "
             "FWSM=0x%08X\n"
             "  tx ring: TDBAL=0x%08X TDLEN=%u TXDCTL=0x%08X\n"
             "  rx ring: RDBAL=0x%08X RDLEN=%u RXDCTL=0x%08X",
             reg_read(E1000_GPTC), reg_read(E1000_TPT), reg_read(E1000_GPRC), reg_read(E1000_TPR), reg_read(E1000_MPC),
             reg_read(E1000_CRCERRS), reg_read(E1000_RDH), reg_read(E1000_RDT), reg_read(E1000_TDH),
             reg_read(E1000_TDT), reg_read(E1000_CTRL), reg_read(E1000_TCTL), reg_read(E1000_RCTL),
             reg_read(E1000_CTRL_EXT), reg_read(E1000_FWSM), reg_read(E1000_TDBAL), reg_read(E1000_TDLEN),
             reg_read(E1000_TXDCTL), reg_read(E1000_RDBAL), reg_read(E1000_RDLEN), reg_read(E1000_RXDCTL));

  /* Read the first TX descriptor back from memory: did the NIC write DD (bit0
   * of status) into it (= it processed/transmitted it) or never touch it (=
   * engine frozen)? And TARC0, the TX arbiter/queue-enable register. */
  if (tx_ring) {
    con_printf("\n  txd0: addr=0x%08X len=%u cmd=0x%08X status=0x%08X TARC0=0x%08X", (uint32_t)tx_ring[0].addr,
               tx_ring[0].length, tx_ring[0].cmd, tx_ring[0].status, reg_read(E1000_TARC0));
  }

  /* Scan MDIO addresses for the PHY (reg 2 = PHY ID1). A real Intel PHY answers
   * ~0x0154 at its address; all-zero/FFFF everywhere means MDIO isn't reaching
   * the PHY (interconnect likely stuck in SMBus, not PCIe) — the suspected
   * reason the link is degraded to 10Mb and TX won't run. */
  con_write("\n  phy scan (reg2):");
  for (int a = 0; a < 4; a++) {
    int v = phy_read((uint8_t)a, 2);
    con_printf(" %u:0x%08X", a, v < 0 ? 0xFFFFFFFFu : (uint32_t)v);
  }
  con_putc('\n');
}

/* Link state for the IP stack to gate DHCP on: real copper takes a couple of
 * seconds to autonegotiate, far longer than boot DHCP's patience. */
static int e1000_link_up(void) { return present && (reg_read(E1000_STATUS) & STATUS_LU); }

const struct nic_ops e1000_ops = {
    .name = "e1000",
    .probe = e1000_probe,
    .send = e1000_send,
    .mac = e1000_mac,
    .poll = e1000_poll,
    .link_up = e1000_link_up,
};
