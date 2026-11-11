/* vmxnet3.c: VMware vmxnet3 paravirtual NIC, revision 1, one TX and one RX queue, no offloads.
 * BAR0 holds the doorbells, BAR1 the command/config registers. Interrupts are masked; RX is polled from the timer tick.
 * RX uses ring 0 only (2 KiB head buffers); ring 1 stays unposted. Descriptors are owned by the side whose
 * generation matches the ring's, and the gen word is written last. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "console.h"
#include "heap.h"
#include "io.h"
#include "irqflags.h"
#include "kstring.h"
#include "net.h"
#include "paging.h"
#include "pci.h"
#include "vmxnet3.h"

#define VMXNET3_VENDOR 0x15AD
#define VMXNET3_DEVICE 0x07B0

/* BAR0 doorbells */
#define B0_IMR 0x000
#define B0_TXPROD 0x600
#define B0_RXPROD 0x800

/* BAR1 control registers */
#define B1_VRRS 0x000
#define B1_UVRS 0x008
#define B1_DSAL 0x010
#define B1_DSAH 0x018
#define B1_CMD 0x020
#define B1_MACL 0x028
#define B1_MACH 0x030

#define CMD_ACTIVATE_DEV 0xCAFE0000u
#define CMD_RESET_DEV 0xCAFE0002u
#define CMD_GET_LINK 0xF00D0002u

#define DS_MAGIC 0xBABEFEE1u
#define RXM_UCAST 0x01u
#define RXM_BCAST 0x04u
#define RXM_ALL_MULTI 0x08u
#define IC_DISABLE_ALL 0x1u
#define GOS_BITS_32 1u
#define GOS_BITS_64 2u
#define MAX_INTRS 25

#define TX_DESCS 32
#define RX0_DESCS 64
#define RX1_DESCS 32
#define RX_COMP_DESCS (RX0_DESCS + RX1_DESCS)
#define BUF_SIZE 2048

/* Descriptor dword layouts (little endian). dword 2 of TX/RX descriptors carries len and gen. */
#define TXD_GEN (1u << 14)
#define TXD_EOP (1u << 12)
#define TXD_CQ (1u << 13)
#define RXD_GEN (1u << 31)
#define TCD_GEN (1u << 31)
#define RCD_GEN (1u << 31)
#define RCD_EOP (1u << 14)
#define RCD_SOP (1u << 15)
#define RCD_LEN_MASK 0x3FFFu
#define RCD_ERROR (1u << 14)

struct desc {
  uint64_t addr;
  uint32_t d2;
  uint32_t d3;
} __attribute__((packed));

struct drv_info {
  uint32_t version, gos, rev_spt, upt_ver_spt;
} __attribute__((packed));

struct misc_conf {
  struct drv_info info;
  uint64_t upt_features, dd_pa, queue_desc_pa;
  uint32_t dd_len, queue_desc_len, mtu;
  uint16_t max_rx_sg;
  uint8_t num_tx_queues, num_rx_queues;
  uint32_t reserved[4];
} __attribute__((packed));

struct intr_conf {
  uint8_t auto_mask, num_intrs, event_intr_idx, mod_levels[MAX_INTRS];
  uint32_t intr_ctrl, reserved[2];
} __attribute__((packed));

struct rx_filter_conf {
  uint32_t rx_mode;
  uint16_t mf_table_len, pad;
  uint64_t mf_table_pa;
  uint32_t vf_table[128];
} __attribute__((packed));

struct var_conf {
  uint32_t ver, len;
  uint64_t pa;
} __attribute__((packed));

struct driver_shared {
  uint32_t magic, pad;
  struct misc_conf misc;
  struct intr_conf intr;
  struct rx_filter_conf rx_filter;
  struct var_conf rss, pm, plugin;
  uint32_t ecr, reserved[5];
} __attribute__((packed));

struct tx_queue_desc {
  uint32_t tx_num_deferred, tx_threshold;
  uint64_t ctrl_reserved;
  uint64_t tx_ring_pa, data_ring_pa, comp_ring_pa, dd_pa, conf_reserved;
  uint32_t tx_ring_size, data_ring_size, comp_ring_size, dd_len;
  uint8_t intr_idx, conf_pad[7];
  uint8_t status[8];
  uint64_t stats[10];
  uint8_t pad[88];
} __attribute__((packed));

struct rx_queue_desc {
  uint8_t update_rx_prod, ctrl_pad[7];
  uint64_t ctrl_reserved;
  uint64_t rx_ring_pa[2], comp_ring_pa, dd_pa, conf_reserved;
  uint32_t rx_ring_size[2], comp_ring_size, dd_len;
  uint8_t intr_idx, conf_pad[7];
  uint8_t status[8];
  uint64_t stats[10];
  uint8_t pad[88];
} __attribute__((packed));

_Static_assert(sizeof(struct driver_shared) == 720, "driver_shared layout");
_Static_assert(sizeof(struct tx_queue_desc) == 256, "tx_queue_desc layout");
_Static_assert(sizeof(struct rx_queue_desc) == 256, "rx_queue_desc layout");

static volatile uint8_t *bar0, *bar1;
static uint8_t mac[6];
static bool present;

static volatile struct desc *tx_ring, *tx_comp, *rx_ring, *rx_comp;
static uint8_t *tx_bufs[TX_DESCS];
static uint8_t *rx_bufs[RX0_DESCS];
static uint32_t tx_fill, tx_used, tx_comp_next, rx_fill, rx_comp_next;
static uint32_t tx_gen = 1, tx_comp_gen = 1, rx_gen = 1, rx_comp_gen = 1;

static inline void wr0(uint32_t off, uint32_t v) { *(volatile uint32_t *)(bar0 + off) = v; }
static inline void wr1(uint32_t off, uint32_t v) { *(volatile uint32_t *)(bar1 + off) = v; }
static inline uint32_t rd1(uint32_t off) { return *(volatile uint32_t *)(bar1 + off); }

static uint32_t command(uint32_t cmd)
{
  wr1(B1_CMD, cmd);
  return rd1(B1_CMD);
}

static void barrier(void) { __asm__ volatile("" ::: "memory"); }

static void print_hex2(uint8_t v)
{
  static const char h[] = "0123456789abcdef";
  con_printf("%c%c", h[v >> 4], h[v & 0xF]);
}

static void rx_post(void)
{
  uint32_t slot = rx_fill;
  rx_ring[slot].addr = (uint32_t)(uintptr_t)rx_bufs[slot];
  rx_ring[slot].d3 = 0;
  barrier();
  rx_ring[slot].d2 = BUF_SIZE | (rx_gen ? RXD_GEN : 0);
  if (++rx_fill == RX0_DESCS) {
    rx_fill = 0;
    rx_gen ^= 1;
  }
}

static void tx_reap(void)
{
  while (!!(tx_comp[tx_comp_next].d3 & TCD_GEN) == tx_comp_gen) {
    barrier();
    if (++tx_comp_next == TX_DESCS) {
      tx_comp_next = 0;
      tx_comp_gen ^= 1;
    }
    tx_used--;
  }
}

static void vmxnet3_send(const uint8_t *frame, uint32_t len)
{
  if (!present || len > 1514)
    return;

  uint32_t fl = irq_save();

  tx_reap();
  if (tx_used >= TX_DESCS - 1) {
    irq_restore(fl);
    return;
  }

  uint32_t i = tx_fill;
  memcpy(tx_bufs[i], frame, len);
  uint32_t wire = len < 60 ? 60 : len;
  if (wire > len)
    memset(tx_bufs[i] + len, 0, wire - len);

  tx_ring[i].addr = (uint32_t)(uintptr_t)tx_bufs[i];
  tx_ring[i].d3 = TXD_EOP | TXD_CQ;
  barrier();
  tx_ring[i].d2 = wire | (tx_gen ? TXD_GEN : 0);

  if (++tx_fill == TX_DESCS) {
    tx_fill = 0;
    tx_gen ^= 1;
  }
  tx_used++;
  wr0(B0_TXPROD, tx_fill);

  irq_restore(fl);
}

static void rx_drain(void)
{
  for (;;) {
    volatile struct desc *c = &rx_comp[rx_comp_next];
    if (!!(c->d3 & RCD_GEN) != rx_comp_gen)
      break;
    barrier();

    uint32_t d0 = *(volatile uint32_t *)c;
    uint32_t d2 = c->d2;
    uint32_t idx = d0 & 0xFFF;
    uint32_t len = d2 & RCD_LEN_MASK;

    if (idx < RX0_DESCS && (d0 & RCD_SOP) && (d0 & RCD_EOP) && !(d2 & RCD_ERROR) && len >= 14)
      net_rx_frame(rx_bufs[idx], (uint16_t)len);

    if (idx < RX0_DESCS) {
      rx_post();
      wr0(B0_RXPROD, rx_fill);
    }

    if (++rx_comp_next == RX_COMP_DESCS) {
      rx_comp_next = 0;
      rx_comp_gen ^= 1;
    }
  }
}

static void vmxnet3_poll(void)
{
  if (!present)
    return;
  uint32_t fl = irq_save();
  rx_drain();
  tx_reap();
  irq_restore(fl);
}

static const uint8_t *vmxnet3_mac(void) { return mac; }

static int vmxnet3_link_up(void) { return present && (command(CMD_GET_LINK) & 1); }

static int map_bar(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, volatile uint8_t **out)
{
  uint32_t phys = pci_read32(bus, slot, func, off) & ~0xFu;
  if (phys == 0 || phys == 0xFFFFFFF0)
    return -1;
  if (paging_identity_map(phys, 0x1000) != 0)
    return -1;
  paging_mark_uc(phys, 0x1000);
  *out = (volatile uint8_t *)(uintptr_t)phys;
  return 0;
}

static int setup_queues(struct driver_shared *ds, struct tx_queue_desc *tq, struct rx_queue_desc *rq)
{
  void *rx1_ring = palloc();
  tx_ring = palloc();
  tx_comp = palloc();
  rx_ring = palloc();
  rx_comp = palloc();
  if (!tx_ring || !tx_comp || !rx_ring || !rx1_ring || !rx_comp)
    return -1;

  for (int i = 0; i < TX_DESCS; i++)
    if (!(tx_bufs[i] = kmalloc(BUF_SIZE)))
      return -1;
  for (int i = 0; i < RX0_DESCS; i++)
    if (!(rx_bufs[i] = kmalloc(BUF_SIZE)))
      return -1;

  tq->tx_ring_pa = (uint32_t)(uintptr_t)tx_ring;
  tq->comp_ring_pa = (uint32_t)(uintptr_t)tx_comp;
  tq->tx_ring_size = TX_DESCS;
  tq->comp_ring_size = TX_DESCS;

  rq->rx_ring_pa[0] = (uint32_t)(uintptr_t)rx_ring;
  rq->rx_ring_pa[1] = (uint32_t)(uintptr_t)rx1_ring;
  rq->comp_ring_pa = (uint32_t)(uintptr_t)rx_comp;
  rq->rx_ring_size[0] = RX0_DESCS;
  rq->rx_ring_size[1] = RX1_DESCS;
  rq->comp_ring_size = RX_COMP_DESCS;

  for (int i = 0; i < RX0_DESCS - 1; i++)
    rx_post();

  ds->misc.queue_desc_pa = (uint32_t)(uintptr_t)tq;
  return 0;
}

static int vmxnet3_probe(void)
{
  bool found = false;
  uint8_t bus = 0, slot = 0, func = 0;

  for (int b = 0; b < 256 && !found; b++) {
    for (int s = 0; s < 32 && !found; s++) {
      if ((pci_read32((uint8_t)b, (uint8_t)s, 0, 0x00) & 0xFFFF) == 0xFFFF)
        continue;
      for (int f = 0; f < 8; f++) {
        uint32_t id = pci_read32((uint8_t)b, (uint8_t)s, (uint8_t)f, 0x00);
        if ((id & 0xFFFF) != VMXNET3_VENDOR || ((id >> 16) & 0xFFFF) != VMXNET3_DEVICE)
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

  if (map_bar(bus, slot, func, 0x10, &bar0) != 0 || map_bar(bus, slot, func, 0x14, &bar1) != 0) {
    con_write("net: vmxnet3 BAR map failed\n");
    return 0;
  }

  /* Memory-space decode (bit 1) and bus-master DMA (bit 2). */
  uint32_t cmd = pci_read32(bus, slot, func, 0x04);
  pci_write32(bus, slot, func, 0x04, cmd | 0x06);

  if (!(rd1(B1_VRRS) & 1) || !(rd1(B1_UVRS) & 1)) {
    con_write("net: vmxnet3 revision 1 unsupported\n");
    return 0;
  }
  wr1(B1_VRRS, 1);
  wr1(B1_UVRS, 1);
  command(CMD_RESET_DEV);

  /* One page: shared area at 0, queue descriptors at 1024 (512-byte aligned). */
  uint8_t *page = palloc();
  if (!page)
    return 0;
  struct driver_shared *ds = (struct driver_shared *)page;
  struct tx_queue_desc *tq = (struct tx_queue_desc *)(page + 1024);
  struct rx_queue_desc *rq = (struct rx_queue_desc *)(page + 1024 + sizeof(*tq));

  if (setup_queues(ds, tq, rq) != 0) {
    con_write("net: vmxnet3 ring allocation failed\n");
    return 0;
  }

  ds->magic = DS_MAGIC;
  ds->misc.info.version = 0x01000000;
  ds->misc.info.gos = sizeof(void *) == 8 ? GOS_BITS_64 : GOS_BITS_32;
  ds->misc.info.rev_spt = 1;
  ds->misc.info.upt_ver_spt = 1;
  ds->misc.queue_desc_len = sizeof(*tq) + sizeof(*rq);
  ds->misc.mtu = 1500;
  ds->misc.max_rx_sg = 1;
  ds->misc.num_tx_queues = 1;
  ds->misc.num_rx_queues = 1;
  ds->intr.num_intrs = 1;
  ds->intr.intr_ctrl = IC_DISABLE_ALL;
  ds->rx_filter.rx_mode = RXM_UCAST | RXM_BCAST | RXM_ALL_MULTI;

  uint32_t pa = (uint32_t)(uintptr_t)ds;
  wr1(B1_DSAL, pa);
  wr1(B1_DSAH, 0);
  wr0(B0_IMR, 1);

  if (command(CMD_ACTIVATE_DEV) != 0) {
    con_write("net: vmxnet3 activate failed\n");
    return 0;
  }
  wr0(B0_RXPROD, rx_fill);

  uint32_t ml = rd1(B1_MACL), mh = rd1(B1_MACH);
  mac[0] = ml & 0xFF;
  mac[1] = (ml >> 8) & 0xFF;
  mac[2] = (ml >> 16) & 0xFF;
  mac[3] = (ml >> 24) & 0xFF;
  mac[4] = mh & 0xFF;
  mac[5] = (mh >> 8) & 0xFF;

  present = true;

  con_printf("net: VMware vmxnet3 mac ");
  for (int i = 0; i < 6; i++) {
    print_hex2(mac[i]);
    if (i < 5)
      con_putc(':');
  }
  con_printf(" link %s", vmxnet3_link_up() ? "up\n" : "down\n");
  return 1;
}

const struct nic_ops vmxnet3_ops = {
    .name = "vmxnet3",
    .probe = vmxnet3_probe,
    .send = vmxnet3_send,
    .mac = vmxnet3_mac,
    .poll = vmxnet3_poll,
    .link_up = vmxnet3_link_up,
};
