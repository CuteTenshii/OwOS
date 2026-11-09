/* e1000.c: Intel Gigabit Ethernet (8254x / 82574 / I217-I219), legacy descriptor model over MMIO.
 * I210/I211 are the igb family and live in igb.c.
 * MAC comes from RAL/RAH[0]; rings sit in cached RAM (x86 DMA is coherent), only registers are UC. */

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

/* Wake and drive PCH MACs (I217/I218/I219); set to 0 to recognize-but-skip if boot hangs. */
#define E1000_PCH_WAKE 1

/* irq_save / irq_restore: snapshot EFLAGS + cli, restore the prior IF. */
#include "irqflags.h"

static void print_hex2(uint8_t v)
{
  static const char h[] = "0123456789abcdef";
  con_printf("%c%c", h[v >> 4], h[v & 0xF]);
}

#define E1000_CTRL 0x0000
#define E1000_STATUS 0x0008
#define E1000_EERD 0x0014
#define E1000_ICR 0x00C0
#define E1000_IMS 0x00D0
#define E1000_IMC 0x00D8
#define E1000_RCTL 0x0100
#define E1000_TCTL 0x0400
#define E1000_TIPG 0x0410
#define E1000_RDBAL 0x2800
#define E1000_RDBAH 0x2804
#define E1000_RDLEN 0x2808
#define E1000_RDH 0x2810
#define E1000_RDT 0x2818
#define E1000_RXDCTL 0x2828
#define E1000_TDBAL 0x3800
#define E1000_TDBAH 0x3804
#define E1000_TDLEN 0x3808
#define E1000_TDH 0x3810
#define E1000_TDT 0x3818
#define E1000_TXDCTL 0x3828
#define E1000_TXDCTL1 0x3928
#define E1000_TARC0 0x3840
#define E1000_TARC1 0x3940
#define E1000_RFCTL 0x5008
#define E1000_PBECCSTS 0x100C
#define E1000_GCR 0x5B00
#define E1000_IOSFPC 0x0F28
#define E1000_CRCERRS 0x4000
#define E1000_MPC 0x4010
#define E1000_GPRC 0x4074
#define E1000_GPTC 0x4080
#define E1000_TPR 0x40D0
#define E1000_TPT 0x40D4
#define E1000_MTA 0x5200
#define E1000_RAL 0x5400
#define E1000_RAH 0x5404
#define E1000_MDIC 0x0020
#define E1000_CTRL_EXT 0x0018
#define E1000_FEXTNVM3 0x003C
#define E1000_EXTCNF_CTRL 0x0F00
#define E1000_H2ME 0x5B50
#define E1000_FWSM 0x5B54
#define E1000_KMRNCTRLSTA 0x0034

/* CTRL bits */
#define CTRL_FD (1u << 0)
#define CTRL_GIO_MASTER_DISABLE (1u << 2)
#define CTRL_ASDE (1u << 5)
#define CTRL_SLU (1u << 6)
#define CTRL_SPEED_SHIFT 8
#define CTRL_FRCSPD (1u << 11)
#define CTRL_FRCDPLX (1u << 12)
#define CTRL_ILOS (1u << 7)
#define CTRL_RST (1u << 26)
#define CTRL_PHY_RST (1u << 31)
#define CTRL_LRST (1u << 3)
#define CTRL_LANPHYPC_OVERRIDE (1u << 16)
#define CTRL_LANPHYPC_VALUE (1u << 17)

/* STATUS bits */
#define STATUS_LU (1u << 1)
#define STATUS_GIO_MASTER_EN (1u << 19)

/* EXTCNF_CTRL: gate automatic PHY config across a MAC reset, or the reset hangs the bus. */
#define EXTCNF_GATE_PHY_CFG (1u << 7)

/* MDIC (PHY register access over MDIO) and the EXTCNF_CTRL software flag */
#define MDIC_REG_SHIFT 16
#define MDIC_PHY_SHIFT 21
#define MDIC_OP_READ (2u << 26)
#define MDIC_OP_WRITE (1u << 26)
#define MDIC_READY (1u << 28)
#define MDIC_ERROR (1u << 30)
#define EXTCNF_SWFLAG (1u << 5)

/* PHY ULP / SMBus exit (PCH parts), values from Linux e1000e ich8lan. */
#define CTRL_EXT_FORCE_SMBUS 0x00000800u
#define CTRL_EXT_LPCD 0x00000004u
#define CTRL_EXT_DRV_LOAD 0x10000000u
#define CTRL_EXT_PHYPDEN 0x00100000u
#define CTRL_EXT_SPD_BYPS 0x00008000u
#define CTRL_MEHE 0x00080000u
#define TCTL_MULR 0x10000000u
#define RFCTL_NFSW_DIS 0x00000040u
#define RFCTL_NFSR_DIS 0x00000080u
#define PBECCSTS_ECC_ENABLE 0x00010000u
#define CTRL_EXT_RO_DIS 0x00020000u
#define PCIE_NO_SNOOP_ALL 0x0000003Fu
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
#define ICH_FWSM_FW_VALID 0x00008000u
#define FWSM_ULP_CFG_DONE 0x00000400u
#define H2ME_ULP 0x00000800u
#define H2ME_ENFORCE_SETTINGS 0x00001000u
#define FWSM_PCIM2PCI 0x01000000u

#define RAH_AV (1u << 31)

/* RCTL bits; BSIZE=00 with BSEX=0 is a 2048-byte receive buffer. */
#define RCTL_EN (1u << 1)
#define RCTL_MPE (1u << 4) /* accept all multicast: IPv6 ND and mDNS need it, the MTA is empty */
#define RCTL_BAM (1u << 15)
#define RCTL_SECRC (1u << 26)

/* TCTL bits */
#define TCTL_EN (1u << 1)
#define TCTL_PSP (1u << 3)
#define TCTL_CT (0x10u << 4)
#define TCTL_COLD (0x40u << 12)
#define TCTL_RTLC (1u << 24)

/* Sunrise/Kaby Lake (I219) TX errata: the IOSF arbiter hangs TX with 3 outstanding requests; use 2. */
#define RCTL_RDMTS_HEX 0x00010000u
#define TARC0_CB_MULTIQ_3_REQ 0x30000000u
#define TARC0_CB_MULTIQ_2_REQ 0x20000000u

/* e1000e TXDCTL for PCH: HTHRESH=1, WTHRESH=1, PTHRESH=0x1f; without prefetch TDH never advances. */
#define TXDCTL_DMA_BURST 0x0141011Fu

/* interrupt cause / mask bits */
#define INT_LSC (1u << 2)
#define INT_RXDMT0 (1u << 4)
#define INT_RXO (1u << 6)
#define INT_RXT0 (1u << 7)

/* descriptor status / command bits */
#define RXD_DD (1u << 0)
#define RXD_EOP (1u << 1)
#define TXD_DD (1u << 0)
#define TXD_CMD_EOP (1u << 0)
#define TXD_CMD_IFCS (1u << 1)
#define TXD_CMD_RS (1u << 3)

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

#define RX_DESCS 32
#define TX_DESCS 16
#define BUF_SIZE 2048

#define INTEL_VENDOR 0x8086

/* Recognized Intel parts, all using the legacy descriptor ring. */
static const uint16_t intel_nics[] = {
    0x100E, 0x100F, 0x10D3, 0x153A, 0x153B, 0x15A0, 0x15A1, 0x15A2, 0x15A3,
    0x156F, 0x1570, 0x15B7, 0x15B8, 0x15B9, 0x15D6, 0x15D7, 0x15D8, 0x15E3,
};

static volatile uint8_t *mmio;
static uint8_t mac[6];
static bool present;
static bool pch_part;
static uint16_t device_id;

static struct rx_desc *rx_ring;
static struct tx_desc *tx_ring;
static uint8_t *rx_bufs[RX_DESCS];
static uint8_t *tx_bufs[TX_DESCS];
static uint32_t rx_cur, tx_cur;

/* MMIO accessors; the UC mapping keeps plain volatile accesses ordered. */
static inline void reg_write(uint32_t off, uint32_t val) { *(volatile uint32_t *)(mmio + off) = val; }
static inline uint32_t reg_read(uint32_t off) { return *(volatile uint32_t *)(mmio + off); }

/* PCH bring-up: firmware may leave the CSR window in D3 and the PHY parked; steps are bounded and logged for `ethdiag`.
 */

static char diag[1024];
static unsigned diag_len;
static unsigned line_start;

/* Append to the diagnostic log (read by `ethdiag`) and mirror to the console. */
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
static void dlogb(uint8_t v)
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

/* Interrupt-free ms delay via PIT channel 2 polling port 0x61; runs before timer_init. */
static void pch_mdelay(uint32_t ms)
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

/* Bring the function to PCI D0 via its PM capability; a NIC parked in D3 ignores MMIO. */
static void pci_set_d0(uint8_t b, uint8_t s, uint8_t f)
{
  if (!(pci_read16(b, s, f, 0x06) & (1u << 4)))
    return;
  uint8_t cap = pci_read8(b, s, f, 0x34) & 0xFC;
  for (int i = 0; cap && i < 48; i++) {
    if (pci_read8(b, s, f, cap) == 0x01) {
      uint16_t pmcsr = pci_read16(b, s, f, cap + 4);
      if ((pmcsr & 0x3) != 0) {
        pci_write16(b, s, f, cap + 4, pmcsr & ~0x3u);
        pch_mdelay(10);
        dlog("  powered up from D3\n");
      }
      return;
    }
    cap = pci_read8(b, s, f, cap + 1) & 0xFC;
  }
}

/* Pulse LANPHYPC low then release to restart the PCH PHY clock (e1000_toggle_lanphypc_pch_lpt). */
static void pch_lanphypc(void)
{
  /* Program the PHY config counter to 50ms before pulsing (e1000e order). */
  uint32_t f3 = reg_read(E1000_FEXTNVM3);
  f3 &= ~FEXTNVM3_PHY_CFG_COUNTER_MASK;
  f3 |= FEXTNVM3_PHY_CFG_COUNTER_50MSEC;
  reg_write(E1000_FEXTNVM3, f3);

  uint32_t c = reg_read(E1000_CTRL);
  c |= CTRL_LANPHYPC_OVERRIDE;
  c &= ~CTRL_LANPHYPC_VALUE;
  reg_write(E1000_CTRL, c);
  (void)reg_read(E1000_STATUS);
  pch_mdelay(1);
  c &= ~CTRL_LANPHYPC_OVERRIDE;
  reg_write(E1000_CTRL, c);
  (void)reg_read(E1000_STATUS);

  /* Wait up to ~100ms for CTRL_EXT.LPCD (PHY power-up done), then settle. */
  for (int count = 0; count < 20 && !(reg_read(E1000_CTRL_EXT) & CTRL_EXT_LPCD); count++)
    pch_mdelay(5);
  pch_mdelay(30);
}

/* Take the EXTCNF_CTRL software flag, arbitrating with the ME; hold it across the whole SMBus->PCIe transition. */
static int phy_acquire(void)
{
  int i;
  for (i = 0; (reg_read(E1000_EXTCNF_CTRL) & EXTCNF_SWFLAG) && i < 100; i++)
    pch_mdelay(1);
  reg_write(E1000_EXTCNF_CTRL, reg_read(E1000_EXTCNF_CTRL) | EXTCNF_SWFLAG);
  for (i = 0; i < 1000; i++) {
    if (reg_read(E1000_EXTCNF_CTRL) & EXTCNF_SWFLAG)
      return 0;
    pch_mdelay(1);
  }
  return -1;
}
static void phy_release(void) { reg_write(E1000_EXTCNF_CTRL, reg_read(E1000_EXTCNF_CTRL) & ~EXTCNF_SWFLAG); }

/* One MDIC PHY read; caller holds the flag; -1 on error. */
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

/* Diagnostic PHY read that acquires and releases the flag. */
static int phy_read(uint8_t addr, uint8_t reg)
{
  if (phy_acquire() != 0)
    return -1;
  int v = phy_read_locked(addr, reg);
  phy_release();
  return v;
}

static uint8_t phy_addr;
static uint32_t phy_id;

/* Is the PHY answering MDIO? Tries addresses 1 then 2 (caller holds the flag). */
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

/* Bring the MAC-PHY link out of SMBus into PCIe mode so MDIO reaches the PHY (e1000_init_phy_workarounds_pchlan). */
static void pch_phy_workarounds(void)
{
  /* Gate hardware PHY auto-config for the whole transition. */
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

/* Renegotiate a clean copper link (advertise gigabit, restart autoneg) and wait ~4s for link. */
static void pch_phy_link_setup(void)
{
  if (phy_id == 0) {
    dlog("  no PHY; skipping link setup\n");
    return;
  }
  if (phy_acquire() != 0)
    return;
  int gbcr = phy_read_locked(phy_addr, 9);
  if (gbcr >= 0)
    phy_write_locked(phy_addr, 9, (uint16_t)((gbcr & 0xFCFF) | 0x0200));
  phy_write_locked(phy_addr, 0, 0x1200);
  phy_release();
  dlog("  PHY autoneg restarted; waiting for link\n");

  bool up = false;
  for (int i = 0; i < 400; i++) {
    if (phy_acquire() == 0) {
      int bmsr = phy_read_locked(phy_addr, 1);
      phy_release();
      if (bmsr > 0 && (bmsr & 0x0004)) {
        up = true;
        break;
      }
    }
    pch_mdelay(10);
  }
  dlog(up ? "  PHY link up\n" : "  PHY link timeout\n");
}

/* Un-gate PHY config so the MAC tracks the PHY; while gated, STATUS never sees a real link and TX gets no clock. */
static void pch_force_mac_speed(void)
{
  if (phy_id == 0)
    return;

  reg_write(E1000_EXTCNF_CTRL, reg_read(E1000_EXTCNF_CTRL) & ~EXTCNF_GATE_PHY_CFG);

  uint32_t ctrl = reg_read(E1000_CTRL);
  ctrl |= CTRL_SLU | CTRL_ASDE;
  ctrl &= ~(CTRL_FRCSPD | CTRL_FRCDPLX | CTRL_LRST | CTRL_PHY_RST | CTRL_ILOS);
  reg_write(E1000_CTRL, ctrl);

  dlog("  ungated PHY cfg; MAC tracking link\n");
}

/* Kumeran register access; caller holds the flag. Offset in bits 16-20, REN to read. */
static int kmrn_read_locked(uint8_t off)
{
  reg_write(E1000_KMRNCTRLSTA, (((uint32_t)off << KMRN_OFFSET_SHIFT) & KMRN_OFFSET_MASK) | KMRN_REN);
  (void)reg_read(E1000_STATUS);
  pch_mdelay(1);
  return (int)(reg_read(E1000_KMRNCTRLSTA) & 0xFFFF);
}
static void kmrn_write_locked(uint8_t off, uint16_t data)
{
  reg_write(E1000_KMRNCTRLSTA, (((uint32_t)off << KMRN_OFFSET_SHIFT) & KMRN_OFFSET_MASK) | data);
  (void)reg_read(E1000_STATUS);
  pch_mdelay(1);
}

/* Configure Kumeran K1 (e1000_configure_k1_ich8lan); K1 off keeps the link out of low-power idle. */
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

  /* Pulse FRCSPD + CTRL_EXT.SPD_BYPS to apply K1, then restore CTRL/CTRL_EXT. */
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

/* Leave ULP via the ME (e1000_disable_ulp_lpt_lp); MAC-CSR only, bounded, no-op without FWSM.FW_VALID. */
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

/* Global reset (e1000_reset_hw_ich8lan): quiesce DMA, gate PHY config, pulse RST, no MMIO for 20ms. Capture the MAC
 * first. */
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

  /* Global reset; touch no register for ~20ms afterwards (a flush here hangs the hardware). */
  reg_write(E1000_CTRL, reg_read(E1000_CTRL) | CTRL_RST);
  pch_mdelay(20);

  /* MMIO is safe again. Ungate PHY config and clear any pending interrupts. */
  ext = reg_read(E1000_EXTCNF_CTRL);
  reg_write(E1000_EXTCNF_CTRL, ext & ~EXTCNF_GATE_PHY_CFG);
  reg_write(E1000_IMC, 0xFFFFFFFF);
  (void)reg_read(E1000_ICR);

  dlog("  reset done\n");
}

/* True for I219-class parts needing the TX arbiter errata (every PCH part except I217/I218). */
static bool is_pch_spt(uint16_t dev)
{
  switch (dev) {
  case 0x153A:
  case 0x153B:
  case 0x15A0:
  case 0x15A1:
  case 0x15A2:
  case 0x15A3:
    return false;
  case 0x100E:
  case 0x100F:
  case 0x10D3:
    return false;
  default:
    return true;
  }
}

/* SPT/KBL TX arbiter errata (e1000_configure_tx): cap IOSF outstanding requests at 2. */
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

/* Magic init bits from e1000e e1000_initialize_hw_bits_ich8lan (pch_lpt+); the TX engine needs them. */
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

  /* Enable PCIe DMA cache snooping and disable relaxed ordering; otherwise the NIC reads stale TX descriptors. */
  reg_write(E1000_GCR, reg_read(E1000_GCR) & ~PCIE_NO_SNOOP_ALL);
  reg_write(E1000_CTRL_EXT, reg_read(E1000_CTRL_EXT) | CTRL_EXT_RO_DIS);

  dlog("  set hw init bits + DMA snoop\n");
}

/* EEPROM word read, used only if RAL/RAH is not preloaded; tries the 8254x then the 82574 layout. */
static int eeprom_read(uint16_t addr, uint16_t *out)
{
  reg_write(E1000_EERD, ((uint32_t)addr << 8) | 1u);
  for (int i = 0; i < 100000; i++) {
    uint32_t v = reg_read(E1000_EERD);
    if (v & (1u << 4)) {
      *out = (uint16_t)(v >> 16);
      return 0;
    }
  }
  reg_write(E1000_EERD, ((uint32_t)addr << 2) | 1u);
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

  if (rah & RAH_AV) {
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

static int setup_rx(void)
{
  rx_ring = palloc();
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
  reg_write(E1000_RDT, RX_DESCS - 1);
  rx_cur = 0;
  reg_write(E1000_RCTL, RCTL_EN | RCTL_MPE | RCTL_BAM | RCTL_SECRC);
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
    reg_write(E1000_TXDCTL, TXDCTL_DMA_BURST);
  reg_write(E1000_TIPG, 0x0060200A);
  reg_write(E1000_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD | TCTL_RTLC);
  pch_tx_errata();
  return 0;
}

static void e1000_send(const uint8_t *frame, uint32_t len)
{
  if (!present || len > 1514)
    return;

  uint32_t fl = irq_save();

  uint32_t i = tx_cur;
  memcpy(tx_bufs[i], frame, len);
  tx_ring[i].addr = (uint32_t)(uintptr_t)tx_bufs[i];
  tx_ring[i].length = (uint16_t)len;
  tx_ring[i].cmd = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;
  tx_ring[i].status = 0;

  tx_cur = (i + 1) % TX_DESCS;

  /* PCH PCIm2PCI workaround (__ew32_prepare): a TDT write is dropped while FWSM.PCIM2PCI is set, so wait for it to
   * clear. */
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

/* Drain every complete frame the card has DMA'd in. Called from the IRQ. */
static void rx_poll(void)
{
  while (rx_ring[rx_cur].status & RXD_DD) {
    struct rx_desc *d = &rx_ring[rx_cur];
    uint16_t len = d->length;

    if ((d->status & RXD_EOP) && d->errors == 0 && len >= 14 && len <= BUF_SIZE)
      net_rx_frame(rx_bufs[rx_cur], len);

    d->status = 0;
    uint32_t done = rx_cur;
    rx_cur = (rx_cur + 1) % RX_DESCS;
    reg_write(E1000_RDT, done);
  }
}

static void e1000_irq(struct registers *r)
{
  (void)r;
  (void)reg_read(E1000_ICR);
}

/* IRQ-independent RX drain from the timer tick (nic_poll); some hosts (VMware) never deliver the RX IRQ. */
static void e1000_poll(void)
{
  if (!present)
    return;
  uint32_t fl = irq_save();
  rx_poll();
  irq_restore(fl);
}

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

/* I217/I218/I219: the CSR window is dead until the LANPHYPC/ME bring-up, and a read before it hangs the CPU. */
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
    return 0;

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
  uint32_t mmio_phys = bar0 & ~0xFu;

  /* PCH parts: validate the BAR and enter D0 before any MMIO. */
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

  /* First CSR read: a dead window reads all-ones. Then wake the PHY (leave ULP, LANPHYPC pulse, poll PHY id). */
  if (pch) {
    dlog("  reading STATUS (first MMIO)\n");
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

    /* Capture the firmware-loaded MAC before the reset clears RAL/RAH, then reset. */
    read_mac();
    pch_reset();

    /* TX-arbiter magic init bits (e1000e init_hw_bits), after reset and before link setup. */
    pch_initialize_hw_bits();

    /* Bring the MAC-PHY link out of SMBus into PCIe mode, then renegotiate. */
    pch_phy_workarounds();
    pch_phy_link_setup();

    /* Tell the ME the host driver owns us, or it gates host TX. Set after the reset (which clears CTRL_EXT). */
    reg_write(E1000_CTRL_EXT, reg_read(E1000_CTRL_EXT) | CTRL_EXT_DRV_LOAD);
    dlog("  took hw control (DRV_LOAD)\n");

    /* Take the MAC<->PHY internal (Kumeran) link out of K1 low-power idle. */
    pch_configure_k1(false);

    /* Un-gate PHY config so the MAC tracks the real link/speed. */
    pch_force_mac_speed();
  }

  /* Reset the discrete parts here; PCH parts were already reset by pch_reset(). */
  if (!pch) {
    dlog("  resetting MAC\n");
    reg_write(E1000_IMC, 0xFFFFFFFF);
    reg_write(E1000_CTRL, reg_read(E1000_CTRL) | CTRL_RST);
    for (int spins = 0; spins < 1000000; spins++)
      if (!(reg_read(E1000_CTRL) & CTRL_RST))
        break;
    reg_write(E1000_IMC, 0xFFFFFFFF);
  }

  /* Link up: ASDE adopts the PHY's autonegotiated speed; clear reset/loopback/force bits. PCH forced it already. */
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
    read_mac();
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
    irq_unmask(2);

  present = true;

  con_printf("net: Intel %s at mmio 0x%08X irq %u mac ", model_name(device_id), mmio_phys, irq);
  for (int i = 0; i < 6; i++) {
    print_hex2(mac[i]);
    if (i < 5)
      con_putc(':');
  }
  con_putc('\n');

  /* Record the outcome in the diagnostic log; `ethdiag` reads it back. */
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

/* `ethdiag`: replay the bring-up log and show live link state. */
/* `txtest`: force MAC loopback and send one frame to isolate the TX engine from the link. */
void e1000_txtest(void)
{
  if (!present) {
    con_write("txtest: no NIC\n");
    return;
  }

  uint32_t save_ctrl = reg_read(E1000_CTRL);
  uint32_t save_rctl = reg_read(E1000_RCTL);

  /* PHY loopback forced to 100-full (BMCR bits 13, 8, 14, autoneg off); exercises the MAC TX engine and the MAC<->PHY
   * link. */
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

  /* MAC forced to 100-full, link up, no MAC loopback (the PHY loops). */
  uint32_t ctrl = save_ctrl | CTRL_SLU | CTRL_FD | CTRL_FRCSPD | CTRL_FRCDPLX;
  ctrl = (ctrl & ~(3u << CTRL_SPEED_SHIFT)) | (1u << CTRL_SPEED_SHIFT);
  reg_write(E1000_CTRL, ctrl);
  reg_write(E1000_RCTL, save_rctl & ~0xC0u);
  pch_mdelay(20);

  con_printf("txtest(PHY lb): TCTL=0x%08X CTRL=0x%08X STATUS=0x%08X\n", reg_read(E1000_TCTL), reg_read(E1000_CTRL),
             reg_read(E1000_STATUS));

  (void)reg_read(E1000_GPTC);
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
    phy_write_locked(phy_addr, 0, 0x1200);
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

  /* Hardware packet counters (clear-on-read): traffic since the last ethdiag. */
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

  /* Read back the first TX descriptor (DD set means the NIC processed it) and TARC0. */
  if (tx_ring) {
    con_printf("\n  txd0: addr=0x%08X len=%u cmd=0x%08X status=0x%08X TARC0=0x%08X", (uint32_t)tx_ring[0].addr,
               tx_ring[0].length, tx_ring[0].cmd, tx_ring[0].status, reg_read(E1000_TARC0));
  }

  /* Scan MDIO addresses for the PHY (reg 2 = ID1); all-zero or FFFF means MDIO isn't reaching it. */
  con_write("\n  phy scan (reg2):");
  for (int a = 0; a < 4; a++) {
    int v = phy_read((uint8_t)a, 2);
    con_printf(" %u:0x%08X", a, v < 0 ? 0xFFFFFFFFu : (uint32_t)v);
  }
  con_putc('\n');
}

/* Link state for the IP stack to gate DHCP on; copper takes seconds to autonegotiate. */
static int e1000_link_up(void) { return present && (reg_read(E1000_STATUS) & STATUS_LU); }

const struct nic_ops e1000_ops = {
    .name = "e1000",
    .probe = e1000_probe,
    .send = e1000_send,
    .mac = e1000_mac,
    .poll = e1000_poll,
    .link_up = e1000_link_up,
};
