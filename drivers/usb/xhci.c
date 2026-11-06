/* xhci.c: minimal xHCI (USB 3.x) host controller driver: ring/context setup, enumeration, control and bulk transfers.
 * DMA memory comes from palloc() (zeroed, identity-mapped); MMIO is mapped uncached. Each stage logs to COM1. */

#include "xhci.h"
#include "console.h"
#include "endian.h"
#include "fs.h"
#include "heap.h"
#include "io.h"
#include "kstring.h"
#include "net.h"
#include "nic.h"
#include "paging.h"
#include "pci.h"
#include "sched.h"
#include "timer.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PCI_CLASS_SERIAL_BUS 0x0C
#define PCI_SUBCLASS_USB 0x03
#define USB_PROGIF_XHCI 0x30

#define XHCI_CAPLENGTH 0x00
#define XHCI_HCSPARAMS1 0x04
#define XHCI_HCSPARAMS2 0x08
#define XHCI_HCCPARAMS1 0x10
#define XHCI_DBOFF 0x14
#define XHCI_RTSOFF 0x18

#define XHCI_USBCMD 0x00
#define XHCI_USBSTS 0x04
#define XHCI_PAGESIZE 0x08
#define XHCI_CRCR 0x18
#define XHCI_DCBAAP 0x30
#define XHCI_CONFIG 0x38
#define XHCI_PORTSC(p) (0x400 + ((p) - 1) * 0x10)

/* USBCMD / USBSTS bits. */
#define CMD_RS (1u << 0)
#define CMD_HCRST (1u << 1)
#define STS_HCH (1u << 0)
#define STS_CNR (1u << 11)

/* PORTSC bits. */
#define PORT_CCS (1u << 0)
#define PORT_PED (1u << 1)
#define PORT_PR (1u << 4)
#define PORT_PP (1u << 9)
#define PORT_PLS(v) (((v) >> 5) & 0xF)
#define PORT_SPEED(v) (((v) >> 10) & 0xF)
/* Status-change bits are write-1-to-clear; PED is RW1CS, so write 0 there unless disabling the port. */
#define PORT_CSC (1u << 17)
#define PORT_PRC (1u << 21)
#define PORT_RWC_MASK (PORT_CSC | (1u << 18) | (1u << 19) | (1u << 20) | PORT_PRC | (1u << 22))

#define XHCI_IMAN 0x20
#define XHCI_IMOD 0x24
#define XHCI_ERSTSZ 0x28
#define XHCI_ERSTBA 0x30
#define XHCI_ERDP 0x38

#define TRB_NORMAL 1
#define TRB_SETUP 2
#define TRB_DATA 3
#define TRB_STATUS 4
#define TRB_LINK 6
#define TRB_ENABLE_SLOT 9
#define TRB_ADDRESS_DEVICE 11
#define TRB_CONFIG_EP 12
#define TRB_EVAL_CONTEXT 13
#define TRB_RESET_EP 14
#define TRB_SET_TRDQ 16
#define TRB_TRANSFER_EVENT 32
#define TRB_CMD_COMPLETION 33
#define TRB_PORT_STATUS 34

#define TRB_CYCLE (1u << 0)
#define TRB_TYPE(t) ((uint32_t)(t) << 10)
#define TRB_GET_TYPE(c) (((c) >> 10) & 0x3F)
#define TRB_IOC (1u << 5)
#define TRB_IDT (1u << 6)
#define TRB_TOGGLE (1u << 1)

/* TRB completion codes (event status >> 24). */
#define CC_SUCCESS 1
#define CC_BABBLE 3
#define CC_USB_TXN_ERR 4
#define CC_STALL 6
#define CC_SHORT_PKT 13

/* A Transfer Request Block: four little-endian dwords. */
struct trb {
  volatile uint32_t p0;
  volatile uint32_t p1;
  volatile uint32_t status;
  volatile uint32_t control;
};

#define RING_TRBS 256

/* A software view of a single-segment ring with a wrapping Link TRB. */
struct ring {
  struct trb *base;
  uint32_t enq;
  uint32_t cycle;
};

static volatile uint8_t *cap;
static volatile uint8_t *op;
static volatile uint8_t *rt;
static volatile uint32_t *db;
static uint32_t ctx_sz;
static uint32_t max_ports;

static uint64_t *dcbaa;
static struct ring cmd_ring;
static struct ring ep0_ring;
static struct trb *event_ring;
static uint32_t event_cycle;
static uint32_t event_idx;
/* Cleared when event_wait() times out (500 ms), set on any successful event; lets callers drop a wedged device. */
static volatile bool hc_responsive = true;

/* One enumerated device per root-hub port; cached so it is not re-enumerated (that drops a tether). */
#define XHCI_MAX_PORTS 256
struct usbdev {
  bool active;
  int slot;
  int speed;
  uint16_t vid, pid;
  uint8_t cls;
  uint8_t cfg[256];
  int cfg_len;
  /* RNDIS device: comm interface for encapsulated commands, bulk data pair, learned MAC. */
  bool is_net;
  bool is_msc;
  bool is_mtp;
  int ctrl_iface;
  uint8_t bulk_in, bulk_out, intr_in;
  uint16_t bulk_in_mps, bulk_out_mps, intr_in_mps;
  uint8_t intr_interval;
  uint8_t mac[6];
  bool mac_ok;
};
static struct usbdev port_dev[XHCI_MAX_PORTS];

/* The active RNDIS USB-Ethernet data path (there is one tethered NIC). */
static bool net_up;
static int net_slot;
static int net_in_dci, net_out_dci;
static uint32_t net_in_mps;
static struct ring net_in_ring, net_out_ring;
static uint8_t *net_rx_buf;
static uint8_t *net_tx_buf;
static uint8_t net_mac[6];
static bool net_in_pending, net_out_pending;
static uint8_t net_in_ep;
static int net_stall_retries;
static int net_intr_dci;
static struct ring net_ep0;
static struct ring net_intr_ring;
static uint8_t *net_intr_buf;
static bool net_intr_pending;
static int net_ctrl_iface;
static int dbg_rx, dbg_tx, dbg_intr;

/* Software TX queue: nic_send drops frames here, the USB thread drains them onto bulk-OUT. */
#define TXQ_SLOTS 16
#define TXQ_FRAME 1600
static uint8_t txq[TXQ_SLOTS][TXQ_FRAME];
static uint16_t txq_len[TXQ_SLOTS];
static volatile int txq_head, txq_tail;

static inline uint32_t r32(volatile uint8_t *p, uint32_t off) { return *(volatile uint32_t *)(p + off); }
static inline void w32(volatile uint8_t *p, uint32_t off, uint32_t v) { *(volatile uint32_t *)(p + off) = v; }
static inline void w64(volatile uint8_t *p, uint32_t off, uint64_t v)
{
  *(volatile uint32_t *)(p + off) = (uint32_t)v;
  *(volatile uint32_t *)(p + off + 4) = (uint32_t)(v >> 32);
}

/* Init a producer ring in a fresh page; the last slot is a Link TRB back to the top that toggles the cycle bit. */
static void ring_init(struct ring *r)
{
  r->base = palloc();
  r->enq = 0;
  r->cycle = 1;
  if (!r->base)
    return;
  struct trb *link = &r->base[RING_TRBS - 1];
  link->p0 = (uint32_t)(uintptr_t)r->base;
  link->p1 = 0;
  link->status = 0;
  link->control = TRB_TYPE(TRB_LINK) | TRB_TOGGLE;
}

/* Enqueue one TRB (caller fills p0/p1/status and control minus the cycle bit); advances the ring, handling the Link. */
static void ring_push(struct ring *r, uint32_t p0, uint32_t p1, uint32_t status, uint32_t control)
{
  struct trb *t = &r->base[r->enq];
  t->p0 = p0;
  t->p1 = p1;
  t->status = status;
  t->control = control | (r->cycle ? TRB_CYCLE : 0);

  r->enq++;
  if (r->enq == RING_TRBS - 1) {
    struct trb *link = &r->base[RING_TRBS - 1];
    if (r->cycle)
      link->control |= TRB_CYCLE;
    else
      link->control &= ~TRB_CYCLE;
    r->enq = 0;
    r->cycle ^= 1;
  }
}

/* Spin (bounded) for the next event matching our cycle; advances ERDP; NULL on timeout. */
static struct trb *event_wait(void)
{
  for (int ms = 0; ms < 500; ms++) {
    struct trb *e = &event_ring[event_idx];
    if ((e->control & TRB_CYCLE) == event_cycle) {
      event_idx++;
      if (event_idx == RING_TRBS) {
        event_idx = 0;
        event_cycle ^= 1;
      }
      /* Tell the controller where we are (set ERDP). */
      w64(rt, XHCI_ERDP, (uint64_t)(uintptr_t)&event_ring[event_idx]);
      /* Drain port-status events (we poll PORTSC) so callers see the event they wait for. */
      if (TRB_GET_TYPE(e->control) == TRB_PORT_STATUS)
        continue;
      hc_responsive = 1;
      return e;
    }
    timer_sleep(1);
  }
  hc_responsive = 0;
  return NULL;
}

/* Non-blocking next event (advances ERDP, skips port-status events); used by the data loop. */
static struct trb *event_poll(void)
{
  for (;;) {
    struct trb *e = &event_ring[event_idx];
    if ((e->control & TRB_CYCLE) != event_cycle)
      return NULL;
    event_idx++;
    if (event_idx == RING_TRBS) {
      event_idx = 0;
      event_cycle ^= 1;
    }
    w64(rt, XHCI_ERDP, (uint64_t)(uintptr_t)&event_ring[event_idx]);
    if (TRB_GET_TYPE(e->control) != TRB_PORT_STATUS)
      return e;
  }
}

/* Ring the command doorbell (DB[0], target 0) and wait for its Command Completion Event. */
static int cmd_submit(void)
{
  db[0] = 0;
  struct trb *e = event_wait();
  if (!e)
    return 0;
  if (TRB_GET_TYPE(e->control) != TRB_CMD_COMPLETION)
    return 0;
  return (e->status >> 24) & 0xFF;
}

/* Find an xHCI controller and map its MMIO BAR (a 64-bit memory BAR). */
static volatile uint8_t *find_and_map(void)
{
  for (int bus = 0; bus < 256; bus++) {
    for (int slot = 0; slot < 32; slot++) {
      if ((pci_read32(bus, slot, 0, 0x00) & 0xFFFF) == 0xFFFF)
        continue;
      uint8_t header = (pci_read32(bus, slot, 0, 0x0C) >> 16) & 0xFF;
      int funcs = (header & 0x80) ? 8 : 1;
      for (int f = 0; f < funcs; f++) {
        uint32_t cls = pci_read32(bus, slot, f, 0x08);
        if (((cls >> 24) & 0xFF) != PCI_CLASS_SERIAL_BUS || ((cls >> 16) & 0xFF) != PCI_SUBCLASS_USB ||
            ((cls >> 8) & 0xFF) != USB_PROGIF_XHCI)
          continue;

        /* Size BAR0 with decode off (all-ones write, read mask, restore); map it all, caps sit at +0x8000. */
        uint32_t cmd = pci_read32(bus, slot, f, 0x04);
        pci_write32(bus, slot, f, 0x04, cmd & ~0x2u);
        uint32_t bar0 = pci_read32(bus, slot, f, 0x10);
        pci_write32(bus, slot, f, 0x10, 0xFFFFFFFFu);
        uint32_t mask = pci_read32(bus, slot, f, 0x10) & ~0xFu;
        pci_write32(bus, slot, f, 0x10, bar0);
        pci_write32(bus, slot, f, 0x04, cmd | 0x06);

        uint32_t phys = bar0 & ~0xFu;
        uint32_t size = ~mask + 1;
        if (size < 0x10000 || size > 0x100000)
          size = 0x10000;
        if (paging_identity_map(phys, size) != 0)
          return NULL;
        paging_mark_uc(phys, size);
        con_printf("xhci: controller at %08x size %08x\n", phys, size);
        return (volatile uint8_t *)(uintptr_t)phys;
      }
    }
  }
  return NULL;
}

/* Take the controller from the BIOS via Legacy Support: set OS-owned, wait for BIOS-owned to clear. */
static void xhci_takeover(void)
{
  uint32_t xecp = (r32(cap, XHCI_HCCPARAMS1) >> 16) & 0xFFFF;
  if (!xecp)
    return;
  volatile uint8_t *p = cap + xecp * 4;
  for (int guard = 0; guard < 64; guard++) {
    uint32_t capreg = *(volatile uint32_t *)p;
    uint8_t id = capreg & 0xFF;
    uint8_t next = (capreg >> 8) & 0xFF;
    if (id == 1) {
      *(volatile uint32_t *)p = capreg | (1u << 24);
      for (int ms = 0; ms < 200; ms++) {
        if (!(*(volatile uint32_t *)p & (1u << 16)))
          break;
        timer_sleep(1);
      }
      /* USBLEGCTLSTS (next dword): clear all SMI enables + ack changes. */
      *(volatile uint32_t *)(p + 4) = (1u << 29) | (1u << 30) | (1u << 31);
      con_write("xhci: took ownership from BIOS\n");
      return;
    }
    if (!next)
      break;
    p += next * 4;
  }
}

/* Power every root-hub port; real controllers come up unpowered (CCS reads 0 until PP is set). */
static void power_all_ports(void)
{
  for (uint32_t p = 1; p <= max_ports; p++) {
    uint32_t sc = r32(op, XHCI_PORTSC(p));
    if (!(sc & PORT_PP))
      w32(op, XHCI_PORTSC(p), (sc & ~PORT_RWC_MASK & ~PORT_PED) | PORT_PP);
  }
  timer_sleep(20);
}

static int controller_reset(void)
{
  /* Halt first. */
  w32(op, XHCI_USBCMD, r32(op, XHCI_USBCMD) & ~CMD_RS);
  for (int ms = 0; ms < 100; ms++) {
    if (r32(op, XHCI_USBSTS) & STS_HCH)
      break;
    timer_sleep(1);
  }
  /* Reset. */
  w32(op, XHCI_USBCMD, CMD_HCRST);
  for (int ms = 0; ms < 1000; ms++) {
    if (!(r32(op, XHCI_USBCMD) & CMD_HCRST) && !(r32(op, XHCI_USBSTS) & STS_CNR))
      return 0;
    timer_sleep(1);
  }
  return -1;
}

static void setup_scratchpad(void)
{
  uint32_t p2 = r32(cap, XHCI_HCSPARAMS2);
  uint32_t n = ((p2 >> 27) & 0x1F) | (((p2 >> 21) & 0x1F) << 5);
  if (!n)
    return;
  uint64_t *arr = palloc();
  for (uint32_t i = 0; i < n && i < 512; i++)
    arr[i] = (uint64_t)(uintptr_t)palloc();
  dcbaa[0] = (uint64_t)(uintptr_t)arr;
}

/* Reset one connected port and return its speed id, or 0 if it didn't enable. */
static int reset_port(uint32_t p)
{
  uint32_t sc = r32(op, XHCI_PORTSC(p));
  if (!(sc & PORT_CCS))
    return 0;

  /* Drive a port reset (clear pending change bits, do not write 1 to PED). */
  w32(op, XHCI_PORTSC(p), (sc & ~PORT_RWC_MASK & ~PORT_PED) | PORT_PR);
  for (int ms = 0; ms < 500; ms++) {
    sc = r32(op, XHCI_PORTSC(p));
    if (sc & PORT_PRC)
      break;
    timer_sleep(1);
  }
  w32(op, XHCI_PORTSC(p), (sc & ~PORT_RWC_MASK & ~PORT_PED) | PORT_PRC);
  sc = r32(op, XHCI_PORTSC(p));
  return (sc & PORT_PED) ? PORT_SPEED(sc) : 0;
}

/* Build the input context (control + slot + EP0) and the output context in DCBAA[slot]; 0 on success. */
static int address_device(int slot_id, int port, int speed)
{
  uint8_t *input = palloc();
  uint8_t *output = palloc();
  if (!input || !output) {
    if (input)
      pfree(input);
    if (output)
      pfree(output);
    return -1;
  }

  uint32_t *icc = (uint32_t *)(input);
  uint32_t *sctx = (uint32_t *)(input + ctx_sz * 1);
  uint32_t *ep0 = (uint32_t *)(input + ctx_sz * 2);

  icc[1] = 0x3;

  /* Slot: context entries = 1, speed, root-hub port number. */
  sctx[0] = (1u << 27) | ((uint32_t)speed << 20);
  sctx[1] = (uint32_t)port << 16;

  /* EP0: control endpoint, default max packet by speed, its transfer ring. */
  uint32_t mps = (speed >= 4) ? 512 : (speed == 3 ? 64 : 8);
  ring_init(&ep0_ring);
  if (!ep0_ring.base) {
    pfree(input);
    pfree(output);
    return -1;
  }
  ep0[1] = (4u << 3) | (mps << 16);
  ep0[2] = (uint32_t)(uintptr_t)ep0_ring.base | 1;
  ep0[3] = 0;

  /* Re-enumerating a slot: free the old device context first, or each hot-plug leaks a page. */
  if (dcbaa[slot_id])
    pfree((void *)(uintptr_t)dcbaa[slot_id]);
  dcbaa[slot_id] = (uint64_t)(uintptr_t)output;

  ring_push(&cmd_ring, (uint32_t)(uintptr_t)input, 0, 0, TRB_TYPE(TRB_ADDRESS_DEVICE) | ((uint32_t)slot_id << 24));
  int cc = cmd_submit();
  pfree(input);
  if (cc != CC_SUCCESS) {
    con_printf("xhci: address-device failed cc=%u\n", cc);
    dcbaa[slot_id] = 0;
    pfree(output);
    pfree(ep0_ring.base);
    ep0_ring.base = 0;
    return -1;
  }
  return 0;
}

/* Evaluate Context: set EP0's max packet size; the TR dequeue pointer is untouched. */
static int eval_context_mps(int slot_id, uint32_t mps)
{
  uint8_t *input = palloc();
  if (!input)
    return -1;
  uint32_t *icc = (uint32_t *)input;
  uint32_t *ep0 = (uint32_t *)(input + ctx_sz * 2);

  icc[1] = 0x2;
  ep0[1] = (4u << 3) | (mps << 16);

  ring_push(&cmd_ring, (uint32_t)(uintptr_t)input, 0, 0, TRB_TYPE(TRB_EVAL_CONTEXT) | ((uint32_t)slot_id << 24));
  int cc = cmd_submit();
  pfree(input);
  return cc == CC_SUCCESS ? 0 : -1;
}

/* Control-IN over EP0: SETUP -> DATA IN -> STATUS OUT; len must fit in one page; returns len or -1. */
static int control_in(int slot_id, uint8_t bmReqType, uint8_t bReq, uint16_t wValue, uint16_t wIndex, uint8_t *out,
                      int len)
{
  uint8_t *buf = palloc();
  if (!buf)
    return -1;
  uint32_t setup_lo = bmReqType | ((uint32_t)bReq << 8) | ((uint32_t)wValue << 16);
  uint32_t setup_hi = (uint32_t)wIndex | ((uint32_t)len << 16);
  ring_push(&ep0_ring, setup_lo, setup_hi, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | (3u << 16));
  ring_push(&ep0_ring, (uint32_t)(uintptr_t)buf, 0, len, TRB_TYPE(TRB_DATA) | (1u << 16));
  ring_push(&ep0_ring, 0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC);
  db[slot_id] = 1;

  struct trb *e = event_wait();
  int rc = -1;
  if (e && TRB_GET_TYPE(e->control) == TRB_TRANSFER_EVENT) {
    int cc = (e->status >> 24) & 0xFF;
    if (cc == CC_SUCCESS || cc == CC_SHORT_PKT) {
      for (int i = 0; i < len; i++)
        out[i] = buf[i];
      rc = len;
    }
  }
  pfree(buf);
  return rc;
}

/* Control transfer with no data stage (SET_CONFIGURATION etc.): SETUP -> STATUS IN. */
static int control_no_data(int slot_id, uint8_t bmReqType, uint8_t bReq, uint16_t wValue, uint16_t wIndex)
{
  uint32_t setup_lo = bmReqType | ((uint32_t)bReq << 8) | ((uint32_t)wValue << 16);
  ring_push(&ep0_ring, setup_lo, (uint32_t)wIndex, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT);
  ring_push(&ep0_ring, 0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC | (1u << 16));
  db[slot_id] = 1;

  struct trb *e = event_wait();
  if (!e || TRB_GET_TYPE(e->control) != TRB_TRANSFER_EVENT)
    return -1;
  return ((e->status >> 24) & 0xFF) == CC_SUCCESS ? 0 : -1;
}

/* Control transfer with an OUT data stage (e.g. RNDIS SEND_ENCAPSULATED_COMMAND): SETUP -> DATA OUT -> STATUS IN. */
static int control_out(int slot_id, uint8_t bmReqType, uint8_t bReq, uint16_t wValue, uint16_t wIndex,
                       const uint8_t *data, int len)
{
  uint8_t *buf = palloc();
  if (!buf)
    return -1;
  for (int i = 0; i < len; i++)
    buf[i] = data[i];

  uint32_t setup_lo = bmReqType | ((uint32_t)bReq << 8) | ((uint32_t)wValue << 16);
  uint32_t setup_hi = (uint32_t)wIndex | ((uint32_t)len << 16);
  ring_push(&ep0_ring, setup_lo, setup_hi, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | (2u << 16));
  ring_push(&ep0_ring, (uint32_t)(uintptr_t)buf, 0, len, TRB_TYPE(TRB_DATA));
  ring_push(&ep0_ring, 0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC | (1u << 16));
  db[slot_id] = 1;

  struct trb *e = event_wait();
  int rc = -1;
  if (e && TRB_GET_TYPE(e->control) == TRB_TRANSFER_EVENT && ((e->status >> 24) & 0xFF) == CC_SUCCESS)
    rc = 0;
  pfree(buf);
  return rc;
}

/* RNDIS (Android tethering): control over EP0 as CDC encapsulated commands, frames over bulk. */

static void put_le32(uint8_t *p, uint32_t v)
{
  p[0] = v;
  p[1] = v >> 8;
  p[2] = v >> 16;
  p[3] = v >> 24;
}

#define RNDIS_INITIALIZE_MSG 0x00000002
#define RNDIS_QUERY_MSG 0x00000004
#define RNDIS_SET_MSG 0x00000005
#define OID_802_3_PERM_ADDR 0x01010101
#define OID_GEN_PACKET_FILTER 0x0001010E

/* Send one RNDIS control message and read the reply, both over EP0. */
static int rndis_cmd(int slot, int iface, const uint8_t *msg, int mlen, uint8_t *resp, int rlen)
{
  if (control_out(slot, 0x21, 0x00, 0, iface, msg, mlen) != 0)
    return -1;
  return control_in(slot, 0xA1, 0x01, 0, iface, resp, rlen);
}

/* INITIALIZE the RNDIS device and query its MAC into d->mac. */
static int rndis_setup(struct usbdev *d)
{
  uint8_t msg[32], resp[64];

  for (int i = 0; i < 32; i++)
    msg[i] = 0;
  put_le32(msg + 0, RNDIS_INITIALIZE_MSG);
  put_le32(msg + 4, 24);
  put_le32(msg + 8, 1);
  put_le32(msg + 12, 1);
  put_le32(msg + 16, 0);
  put_le32(msg + 20, 0x4000);
  /* INITIALIZE_CMPLT layout: type(0) len(4) reqid(8) status(12) ... */
  if (rndis_cmd(d->slot, d->ctrl_iface, msg, 24, resp, sizeof resp) < 16)
    return -1;
  if (load_le32(resp + 12) != 0)
    return -1;

  for (int i = 0; i < 32; i++)
    msg[i] = 0;
  put_le32(msg + 0, RNDIS_QUERY_MSG);
  put_le32(msg + 4, 28);
  put_le32(msg + 8, 2);
  put_le32(msg + 12, OID_802_3_PERM_ADDR);
  put_le32(msg + 16, 0);
  put_le32(msg + 20, 0);
  put_le32(msg + 24, 0);
  /* QUERY_CMPLT: type(0) len(4) reqid(8) status(12) infolen(16) infooffset(20), offset from byte 8. */
  int n = rndis_cmd(d->slot, d->ctrl_iface, msg, 28, resp, sizeof resp);
  if (n < 24 || load_le32(resp + 12) != 0)
    return -1;
  /* InfoBufferOffset is device-controlled; use 64-bit math and an unsigned compare. */
  uint64_t off = (uint64_t)8 + load_le32(resp + 20);
  if (off + 6 > (uint32_t)n)
    return -1;
  for (int i = 0; i < 6; i++)
    d->mac[i] = resp[off + i];
  d->mac_ok = 1;
  return 0;
}

/* Find a USB-Ethernet shape: bulk IN + OUT pair plus interrupt IN on the comm interface. */
static void parse_net(struct usbdev *d)
{
  d->is_net = 0;
  int cur_iface = 0, intr_iface = -1;
  for (int i = 0; i + 2 <= d->cfg_len;) {
    uint8_t blen = d->cfg[i];
    if (blen < 2)
      break;
    uint8_t btype = d->cfg[i + 1];
    if (btype == 0x04 && i + 9 <= d->cfg_len) {
      cur_iface = d->cfg[i + 2];
    } else if (btype == 0x05 && i + 7 <= d->cfg_len) {
      uint8_t addr = d->cfg[i + 2];
      uint8_t attr = d->cfg[i + 3] & 0x3;
      uint16_t mps = (d->cfg[i + 4] | (d->cfg[i + 5] << 8)) & 0x7FF;
      if (attr == 2) {
        if (addr & 0x80) {
          d->bulk_in = addr;
          d->bulk_in_mps = mps ? mps : 64;
        } else {
          d->bulk_out = addr;
          d->bulk_out_mps = mps ? mps : 64;
        }
      } else if (attr == 3 && (addr & 0x80)) {
        d->intr_in = addr;
        d->intr_in_mps = mps ? mps : 16;
        d->intr_interval = d->cfg[i + 6];
        intr_iface = cur_iface;
      }
    }
    i += blen;
  }
  if (d->bulk_in && d->bulk_out) {
    d->is_net = 1;
    d->ctrl_iface = (intr_iface >= 0) ? intr_iface : 0;
  }
}

/* Set the RNDIS packet filter (directed + broadcast); without it RX is silent. */
static int rndis_set_filter(struct usbdev *d)
{
  uint8_t msg[64], resp[64];
  for (int i = 0; i < 64; i++)
    msg[i] = 0;
  put_le32(msg + 0, RNDIS_SET_MSG);
  put_le32(msg + 4, 28 + 4);
  put_le32(msg + 8, 3);
  put_le32(msg + 12, OID_GEN_PACKET_FILTER);
  put_le32(msg + 16, 4);
  put_le32(msg + 20, 20);
  put_le32(msg + 24, 0);
  put_le32(msg + 28, 0x0000000D);
  if (rndis_cmd(d->slot, d->ctrl_iface, msg, 32, resp, sizeof resp) < 16)
    return -1;
  return load_le32(resp + 12) == 0 ? 0 : -1;
}

/* xHCI Device Context Index for an endpoint address: EP0 is 1; others are ep_num*2 (+1 for IN). */
static int ep_dci(uint8_t addr) { return ((addr & 0x0F) * 2) + ((addr & 0x80) ? 1 : 0); }

/* Configure Endpoint for the bulk IN/OUT endpoints, each with its own transfer ring; 0 on success. */
static int configure_bulk(struct usbdev *d, int port)
{
  int in_dci = ep_dci(d->bulk_in);
  int out_dci = ep_dci(d->bulk_out);
  int intr_dci = d->intr_in ? ep_dci(d->intr_in) : 0;
  int max_dci = in_dci > out_dci ? in_dci : out_dci;
  if (intr_dci > max_dci)
    max_dci = intr_dci;

  ring_init(&net_in_ring);
  ring_init(&net_out_ring);
  if (intr_dci)
    ring_init(&net_intr_ring);

  uint8_t *input = palloc();
  if (!input || !net_in_ring.base || !net_out_ring.base || (intr_dci && !net_intr_ring.base)) {
    if (input)
      pfree(input);
    if (net_in_ring.base) {
      pfree(net_in_ring.base);
      net_in_ring.base = 0;
    }
    if (net_out_ring.base) {
      pfree(net_out_ring.base);
      net_out_ring.base = 0;
    }
    if (net_intr_ring.base) {
      pfree(net_intr_ring.base);
      net_intr_ring.base = 0;
    }
    return -1;
  }
  uint32_t *icc = (uint32_t *)input;
  uint32_t *sctx = (uint32_t *)(input + ctx_sz * 1);
  uint32_t *epin = (uint32_t *)(input + ctx_sz * (1 + in_dci));
  uint32_t *epout = (uint32_t *)(input + ctx_sz * (1 + out_dci));

  /* Add slot (A0, to bump Context Entries) + bulk in/out (+ interrupt). */
  icc[1] = (1u << 0) | (1u << in_dci) | (1u << out_dci) | (intr_dci ? (1u << intr_dci) : 0);

  /* Input slot context must COPY the live one, only bumping Context Entries (xHCI 4.6.6);
   * a fresh one zeroes the device address and real Intel xHCI fails bulk transfers (cc=4). */
  (void)port;
  uint32_t *out_slot = (uint32_t *)(uintptr_t)dcbaa[d->slot];
  for (uint32_t i = 0; i < ctx_sz / 4; i++)
    sctx[i] = out_slot[i];
  sctx[0] = (sctx[0] & ~(0x1Fu << 27)) | ((uint32_t)max_dci << 27);

  /* EP type: bulk OUT = 2, bulk IN = 6, interrupt IN = 7. CErr=3. */
  epin[1] = (3u << 1) | (6u << 3) | ((uint32_t)d->bulk_in_mps << 16);
  epin[2] = (uint32_t)(uintptr_t)net_in_ring.base | 1;
  epin[4] = d->bulk_in_mps;
  epout[1] = (3u << 1) | (2u << 3) | ((uint32_t)d->bulk_out_mps << 16);
  epout[2] = (uint32_t)(uintptr_t)net_out_ring.base | 1;
  epout[4] = d->bulk_out_mps;
  if (intr_dci) {
    uint32_t *epi = (uint32_t *)(input + ctx_sz * (1 + intr_dci));
    uint32_t iv = d->intr_interval ? d->intr_interval - 1 : 6;
    epi[0] = iv << 16;
    epi[1] = (3u << 1) | (7u << 3) | ((uint32_t)d->intr_in_mps << 16);
    epi[2] = (uint32_t)(uintptr_t)net_intr_ring.base | 1;
    epi[4] = d->intr_in_mps;
  }

  ring_push(&cmd_ring, (uint32_t)(uintptr_t)input, 0, 0, TRB_TYPE(TRB_CONFIG_EP) | ((uint32_t)d->slot << 24));
  int cc = cmd_submit();
  pfree(input);
  if (cc != CC_SUCCESS) {
    pfree(net_in_ring.base);
    net_in_ring.base = 0;
    pfree(net_out_ring.base);
    net_out_ring.base = 0;
    if (net_intr_ring.base) {
      pfree(net_intr_ring.base);
      net_intr_ring.base = 0;
    }
    return -1;
  }

  net_slot = d->slot;
  net_in_dci = in_dci;
  net_out_dci = out_dci;
  net_intr_dci = intr_dci;
  net_in_mps = d->bulk_in_mps;
  net_in_ep = d->bulk_in;
  net_ctrl_iface = d->ctrl_iface;
  return 0;
}

/* Each frame rides the bulk pipe in a 44-byte RNDIS packet message (type=1, length, data offset/length). */
#define RNDIS_PACKET_MSG 0x00000001
#define RNDIS_HDR 44

/* nic_ops.send: queue a frame for the USB thread to transmit (non-blocking). */
static void rndis_send(const uint8_t *frame, uint32_t len)
{
  if (!net_up || len > TXQ_FRAME)
    return;
  __asm__ volatile("cli");
  int next = (txq_head + 1) % TXQ_SLOTS;
  if (next != txq_tail) {
    for (uint32_t i = 0; i < len; i++)
      txq[txq_head][i] = frame[i];
    txq_len[txq_head] = (uint16_t)len;
    txq_head = next;
  }
  __asm__ volatile("sti");
}

static const uint8_t *rndis_get_mac(void) { return net_mac; }

static const struct nic_ops rndis_ops = {
    .name = "rndis",
    .probe = 0,
    .send = rndis_send,
    .mac = rndis_get_mac,
};

/* Post a bulk-IN request so the device can hand us the next packet. */
static void post_bulk_in(void)
{
  ring_push(&net_in_ring, (uint32_t)(uintptr_t)net_rx_buf, 0, 2048, TRB_TYPE(TRB_NORMAL) | TRB_IOC);
  db[net_slot] = net_in_dci;
  net_in_pending = 1;
}

/* Post an interrupt-IN for RNDIS status; servicing it makes an Android gadget start data. */
static void post_bulk_intr(void)
{
  if (!net_intr_dci)
    return;
  ring_push(&net_intr_ring, (uint32_t)(uintptr_t)net_intr_buf, 0, 16, TRB_TYPE(TRB_NORMAL) | TRB_IOC);
  db[net_slot] = net_intr_dci;
  net_intr_pending = 1;
}

/* Build the RNDIS-wrapped frame in net_tx_buf and post it on bulk-OUT. */
static void post_bulk_out(const uint8_t *frame, int len)
{
  for (int i = 0; i < RNDIS_HDR; i++)
    net_tx_buf[i] = 0;
  put_le32(net_tx_buf + 0, RNDIS_PACKET_MSG);
  put_le32(net_tx_buf + 4, RNDIS_HDR + len);
  put_le32(net_tx_buf + 8, RNDIS_HDR - 8);
  put_le32(net_tx_buf + 12, len);
  for (int i = 0; i < len; i++)
    net_tx_buf[RNDIS_HDR + i] = frame[i];
  ring_push(&net_out_ring, (uint32_t)(uintptr_t)net_tx_buf, 0, RNDIS_HDR + len, TRB_TYPE(TRB_NORMAL) | TRB_IOC);
  db[net_slot] = net_out_dci;
  net_out_pending = 1;
}

/* Bring up the RNDIS data path: packet filter, bulk endpoints, buffers, register as the active NIC; 0 on success. */
static int bring_up_net(struct usbdev *d, int port)
{
  if (rndis_set_filter(d) != 0) {
    con_write("rndis: set-filter failed\n");
    return -1;
  }
  if (configure_bulk(d, port) != 0) {
    con_write("rndis: configure-endpoint failed\n");
    return -1;
  }
  con_printf("rndis: data path up (in ep=%02x dci=%u mps=%u, out ep=%02x "
             "dci=%u mps=%u)\n",
             d->bulk_in, net_in_dci, d->bulk_in_mps, d->bulk_out, net_out_dci, d->bulk_out_mps);

  net_rx_buf = palloc();
  net_tx_buf = palloc();
  net_intr_buf = palloc();
  for (int i = 0; i < 6; i++)
    net_mac[i] = d->mac[i];
  txq_head = txq_tail = 0;
  net_in_pending = net_out_pending = net_intr_pending = false;

  /* Save this device's EP0 ring now; later enumerations overwrite the global ep0_ring. */
  net_ep0 = ep0_ring;

  nic_set(&rndis_ops);
  net_up = 1;
  /* Bulk/interrupt INs are armed in data_loop, not here: arming now would post events during the boot scan. */
  net_dhcp_spawn();
  return 0;
}

/* Recover a halted bulk-IN after a STALL: reset endpoint, clear halt over EP0, restore ring, re-arm. */
static void recover_in_stall(void)
{
  if (net_stall_retries++ > 16)
    return;
  if (net_stall_retries <= 2)
    con_write("usbnet: recovering bulk-in\n");

  ring_push(&cmd_ring, 0, 0, 0, TRB_TYPE(TRB_RESET_EP) | ((uint32_t)net_in_dci << 16) | ((uint32_t)net_slot << 24));
  cmd_submit();
  control_no_data(net_slot, 0x02, 0x01, 0, net_in_ep);
  uint32_t deq = (uint32_t)(uintptr_t)&net_in_ring.base[net_in_ring.enq] | net_in_ring.cycle;
  ring_push(&cmd_ring, deq, 0, 0, TRB_TYPE(TRB_SET_TRDQ) | ((uint32_t)net_in_dci << 16) | ((uint32_t)net_slot << 24));
  cmd_submit();

  net_in_pending = 0;
  post_bulk_in();
}

/* The bulk TX/RX loop, run by the USB thread once the data path is up. */
static void data_loop(void)
{
  /* Reclaim the global EP0 ring for our device; other enumerations clobbered it, and none run from here on. */
  ep0_ring = net_ep0;

  post_bulk_in();
  post_bulk_intr();
  for (;;) {
    /* TX: post one queued frame if the OUT ring is free. */
    if (!net_out_pending && txq_tail != txq_head) {
      post_bulk_out(txq[txq_tail], txq_len[txq_tail]);
      txq_tail = (txq_tail + 1) % TXQ_SLOTS;
    }
    if (!net_intr_pending)
      post_bulk_intr();
    bool resp_avail = 0;
    /* Drain completion events, routing by endpoint. */
    struct trb *e;
    while ((e = event_poll())) {
      if (TRB_GET_TYPE(e->control) != TRB_TRANSFER_EVENT)
        continue;
      int epid = (e->control >> 16) & 0x1F;
      int cc = (e->status >> 24) & 0xFF;
      if (epid == net_in_dci) {
        int residual = e->status & 0xFFFFFF;
        int got = 2048 - residual;
        /* DIAG: report the first few inbound transfers (cc, length, RNDIS type). */
        if (dbg_rx < 4) {
          dbg_rx++;
          con_printf("usbnet: rx cc=%u len=%u type=%08x\n", cc, got, load_le32(net_rx_buf));
        }
        if (cc == CC_STALL || cc == CC_BABBLE || cc == CC_USB_TXN_ERR) {
          /* These halt the endpoint - reset it and retry rather than going permanently deaf. */
          recover_in_stall();
        } else {
          /* Unwrap the RNDIS packet message -> Ethernet frame. */
          if (got >= RNDIS_HDR && load_le32(net_rx_buf) == RNDIS_PACKET_MSG) {
            uint32_t off = load_le32(net_rx_buf + 8);
            uint32_t dl = load_le32(net_rx_buf + 12);
            /* off/dl are device-controlled: use 64-bit math so the frame can't wrap past net_rx_buf. */
            uint64_t start = (uint64_t)8 + off;
            uint64_t g = (uint32_t)got;
            if (start <= g && dl <= g - start)
              net_rx_frame(net_rx_buf + start, dl);
          }
          net_stall_retries = 0;
          net_in_pending = 0;
          post_bulk_in();
        }
      } else if (epid == net_out_dci) {
        if (dbg_tx < 2) {
          dbg_tx++;
          con_printf("usbnet: tx cc=%u\n", cc);
        }
        net_out_pending = 0;
      } else if (epid == net_intr_dci) {
        if (dbg_intr < 4) {
          dbg_intr++;
          con_printf("usbnet: intr cc=%u note=%08x\n", cc, load_le32(net_intr_buf));
        }
        net_intr_pending = 0;
        if (cc == CC_SUCCESS || cc == CC_SHORT_PKT)
          resp_avail = 1;
      }
    }
    /* A status notification means an RNDIS message is queued; read it to drain the event ring. */
    if (resp_avail) {
      uint8_t r[64];
      control_in(net_slot, 0xA1, 0x01, 0, net_ctrl_iface, r, sizeof r);
    }
    timer_sleep(1);
  }
}

/* USB Mass Storage: Bulk-Only Transport (31-byte CBW, data stage, 13-byte CSW) exposing a 512-byte block device.
 * BOT is synchronous: sector I/O goes through a one-slot mailbox to the usb-poll thread, the only context driving the
 * rings. data_loop() runs forever once a tether is up, so a live RNDIS tether and mass storage are exclusive after
 * boot. */

static bool msc_ready;
static int msc_slot;
static int msc_in_dci, msc_out_dci;
static uint8_t msc_in_ep, msc_out_ep;
static uint16_t msc_in_mps, msc_out_mps;
static struct ring msc_in_ring, msc_out_ring;
static struct ring msc_ep0;
static uint32_t msc_capacity;
static uint32_t msc_tag;
static char msc_model[40];
static uint8_t *msc_cbw, *msc_csw, *msc_data;

/* One-slot request mailbox serviced by the poll thread. */
static volatile bool msc_req, msc_done, msc_rw, msc_busy;
static volatile int msc_result;
static volatile int msc_mb_count;
static volatile uint32_t msc_mb_lba;
static volatile uint8_t *msc_mb_buf;

/* Read cache: MSC_CHUNK-sector lines filled by one READ(10); write-through with invalidate; lock holder only. */
#define MSC_CHUNK 8
#define MSC_LINES 64
#define MSC_LINE_BYTES (MSC_CHUNK * 512)
static uint8_t *msc_cache;
static uint32_t msc_ctag[MSC_LINES];
static int msc_victim;

/* Find a Mass Storage interface (class 0x08, subclass 0x06, protocol 0x50) and its bulk IN/OUT endpoints. */
static void parse_msc(struct usbdev *d)
{
  d->is_msc = 0;
  const uint8_t *p = d->cfg;
  int len = d->cfg_len, in_iface = 0;
  uint8_t bin = 0, bout = 0;
  uint16_t binmps = 0, boutmps = 0;

  for (int i = 0; i + 1 < len;) {
    uint8_t l = p[i], t = p[i + 1];
    if (l < 2)
      break;
    if (t == 0x04 && i + 7 < len) {
      in_iface = (p[i + 5] == 0x08 && p[i + 6] == 0x06 && p[i + 7] == 0x50);
    } else if (t == 0x05 && in_iface && i + 5 < len) {
      uint8_t addr = p[i + 2], attr = p[i + 3];
      uint16_t mps = p[i + 4] | (p[i + 5] << 8);
      if ((attr & 0x03) == 0x02) {
        if (addr & 0x80) {
          bin = addr;
          binmps = mps;
        } else {
          bout = addr;
          boutmps = mps;
        }
      }
    }
    i += l;
  }
  if (bin && bout) {
    d->is_msc = 1;
    d->bulk_in = bin;
    d->bulk_in_mps = binmps ? binmps : 64;
    d->bulk_out = bout;
    d->bulk_out_mps = boutmps ? boutmps : 64;
  }
}

/* Configure the bulk pair (copy the live slot context, only bump Context Entries). */
static int configure_bulk_msc(struct usbdev *d)
{
  int in_dci = ep_dci(d->bulk_in);
  int out_dci = ep_dci(d->bulk_out);
  int max_dci = in_dci > out_dci ? in_dci : out_dci;

  ring_init(&msc_in_ring);
  ring_init(&msc_out_ring);

  uint8_t *input = palloc();
  if (!input || !msc_in_ring.base || !msc_out_ring.base) {
    if (input)
      pfree(input);
    if (msc_in_ring.base) {
      pfree(msc_in_ring.base);
      msc_in_ring.base = 0;
    }
    if (msc_out_ring.base) {
      pfree(msc_out_ring.base);
      msc_out_ring.base = 0;
    }
    return -1;
  }
  uint32_t *icc = (uint32_t *)input;
  uint32_t *sctx = (uint32_t *)(input + ctx_sz * 1);
  uint32_t *epin = (uint32_t *)(input + ctx_sz * (1 + in_dci));
  uint32_t *epout = (uint32_t *)(input + ctx_sz * (1 + out_dci));

  icc[1] = (1u << 0) | (1u << in_dci) | (1u << out_dci);

  uint32_t *out_slot = (uint32_t *)(uintptr_t)dcbaa[d->slot];
  for (uint32_t i = 0; i < ctx_sz / 4; i++)
    sctx[i] = out_slot[i];
  sctx[0] = (sctx[0] & ~(0x1Fu << 27)) | ((uint32_t)max_dci << 27);

  epin[1] = (3u << 1) | (6u << 3) | ((uint32_t)d->bulk_in_mps << 16);
  epin[2] = (uint32_t)(uintptr_t)msc_in_ring.base | 1;
  epin[4] = d->bulk_in_mps;
  epout[1] = (3u << 1) | (2u << 3) | ((uint32_t)d->bulk_out_mps << 16);
  epout[2] = (uint32_t)(uintptr_t)msc_out_ring.base | 1;
  epout[4] = d->bulk_out_mps;

  ring_push(&cmd_ring, (uint32_t)(uintptr_t)input, 0, 0, TRB_TYPE(TRB_CONFIG_EP) | ((uint32_t)d->slot << 24));
  int cc = cmd_submit();
  pfree(input);
  if (cc != CC_SUCCESS) {
    pfree(msc_in_ring.base);
    msc_in_ring.base = 0;
    pfree(msc_out_ring.base);
    msc_out_ring.base = 0;
    return -1;
  }

  msc_slot = d->slot;
  msc_in_dci = in_dci;
  msc_out_dci = out_dci;
  msc_in_ep = d->bulk_in;
  msc_out_ep = d->bulk_out;
  msc_in_mps = d->bulk_in_mps;
  msc_out_mps = d->bulk_out_mps;
  return 0;
}

/* One bulk transfer: post a Normal TRB, ring the doorbell, wait for completion. */
static int bulk_xfer(int slot, struct ring *ring, int dci, uint8_t *buf, int len, int *cc)
{
  ring_push(ring, (uint32_t)(uintptr_t)buf, 0, len, TRB_TYPE(TRB_NORMAL) | TRB_IOC);
  db[slot] = dci;
  struct trb *e = event_wait();
  if (!e || TRB_GET_TYPE(e->control) != TRB_TRANSFER_EVENT) {
    *cc = 0;
    return -1;
  }
  *cc = (e->status >> 24) & 0xFF;
  if (*cc == CC_SUCCESS || *cc == CC_SHORT_PKT)
    return len - (int)(e->status & 0xFFFFFF);
  return -1;
}

/* Clear a bulk STALL: reset endpoint, CLEAR_FEATURE(HALT) over EP0, restore ring. */
static void msc_clear_stall(int dci, uint8_t ep_addr, struct ring *ring)
{
  ring_push(&cmd_ring, 0, 0, 0, TRB_TYPE(TRB_RESET_EP) | ((uint32_t)dci << 16) | ((uint32_t)msc_slot << 24));
  cmd_submit();

  struct ring saved = ep0_ring;
  ep0_ring = msc_ep0;
  control_no_data(msc_slot, 0x02, 0x01, 0, ep_addr);
  ep0_ring = saved;

  uint32_t deq = (uint32_t)(uintptr_t)&ring->base[ring->enq] | ring->cycle;
  ring_push(&cmd_ring, deq, 0, 0, TRB_TYPE(TRB_SET_TRDQ) | ((uint32_t)dci << 16) | ((uint32_t)msc_slot << 24));
  cmd_submit();
}

/* Run one BOT command: CBW (with the SCSI CDB) -> optional data -> CSW. */
static int bot_xfer(const uint8_t *cdb, int cdb_len, int dlen, bool write, uint8_t *data)
{
  for (int i = 0; i < 31; i++)
    msc_cbw[i] = 0;
  msc_cbw[0] = 'U';
  msc_cbw[1] = 'S';
  msc_cbw[2] = 'B';
  msc_cbw[3] = 'C';
  uint32_t tag = ++msc_tag;
  put_le32(msc_cbw + 4, tag);
  put_le32(msc_cbw + 8, (uint32_t)dlen);
  msc_cbw[12] = write ? 0x00 : 0x80;
  msc_cbw[13] = 0;
  msc_cbw[14] = (uint8_t)cdb_len;
  for (int i = 0; i < 16 && i < cdb_len; i++)
    msc_cbw[15 + i] = cdb[i];

  int cc;
  if (bulk_xfer(msc_slot, &msc_out_ring, msc_out_dci, msc_cbw, 31, &cc) < 0) {
    if (cc == CC_STALL)
      msc_clear_stall(msc_out_dci, msc_out_ep, &msc_out_ring);
    return -1;
  }

  if (dlen > 0) {
    if (write) {
      for (int i = 0; i < dlen; i++)
        msc_data[i] = data[i];
      if (bulk_xfer(msc_slot, &msc_out_ring, msc_out_dci, msc_data, dlen, &cc) < 0 && cc == CC_STALL)
        msc_clear_stall(msc_out_dci, msc_out_ep, &msc_out_ring);
    } else {
      if (bulk_xfer(msc_slot, &msc_in_ring, msc_in_dci, msc_data, dlen, &cc) < 0 && cc == CC_STALL)
        msc_clear_stall(msc_in_dci, msc_in_ep, &msc_in_ring);
    }
  }

  if (bulk_xfer(msc_slot, &msc_in_ring, msc_in_dci, msc_csw, 13, &cc) < 0) {
    if (cc != CC_STALL)
      return -1;
    msc_clear_stall(msc_in_dci, msc_in_ep, &msc_in_ring);
    if (bulk_xfer(msc_slot, &msc_in_ring, msc_in_dci, msc_csw, 13, &cc) < 0)
      return -1;
  }
  if (msc_csw[0] != 'U' || msc_csw[1] != 'S' || msc_csw[2] != 'B' || msc_csw[3] != 'S')
    return -1;
  if (load_le32(msc_csw + 4) != tag || msc_csw[12] != 0)
    return -1;

  if (dlen > 0 && !write)
    for (int i = 0; i < dlen; i++)
      data[i] = msc_data[i];
  return 0;
}

static int scsi_tur(void)
{
  uint8_t cdb[6] = {0};
  return bot_xfer(cdb, 6, 0, false, 0);
}
static int scsi_request_sense(uint8_t *out18)
{
  uint8_t cdb[6] = {0x03, 0, 0, 0, 18, 0};
  return bot_xfer(cdb, 6, 18, false, out18);
}
static int scsi_inquiry(uint8_t *out36)
{
  uint8_t cdb[6] = {0x12, 0, 0, 0, 36, 0};
  return bot_xfer(cdb, 6, 36, false, out36);
}
static int scsi_read_capacity(uint32_t *last_lba, uint32_t *blk)
{
  uint8_t cdb[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  uint8_t r[8];
  if (bot_xfer(cdb, 10, 8, false, r) != 0)
    return -1;
  *last_lba = ((uint32_t)r[0] << 24) | (r[1] << 16) | (r[2] << 8) | r[3];
  *blk = ((uint32_t)r[4] << 24) | (r[5] << 16) | (r[6] << 8) | r[7];
  return 0;
}
static int scsi_rw10(uint32_t lba, uint8_t *buf, int count, bool write)
{
  uint8_t cdb[10] = {write ? (uint8_t)0x2A : (uint8_t)0x28,
                     0,
                     (uint8_t)(lba >> 24),
                     (uint8_t)(lba >> 16),
                     (uint8_t)(lba >> 8),
                     (uint8_t)lba,
                     0,
                     (uint8_t)(count >> 8),
                     (uint8_t)count,
                     0};
  return bot_xfer(cdb, 10, count * 512, write, buf);
}

/* Configure the bulk pipes and probe the medium. */
static int bring_up_msc(struct usbdev *d, int port)
{
  (void)port;
  if (configure_bulk_msc(d) != 0) {
    con_write("usbmsc: configure-endpoint failed\n");
    return -1;
  }
  if (!msc_cbw)
    msc_cbw = palloc();
  if (!msc_csw)
    msc_csw = palloc();
  if (!msc_data)
    msc_data = palloc();
  if (!msc_cache)
    msc_cache = kmalloc(MSC_LINES * MSC_LINE_BYTES);
  for (int i = 0; i < MSC_LINES; i++)
    msc_ctag[i] = 0xFFFFFFFFu;
  msc_victim = 0;
  msc_ep0 = ep0_ring;
  msc_tag = 0;

  /* A fresh stick often reports "not ready" once; clear it with TEST UNIT READY / REQUEST SENSE rounds. */
  for (int i = 0; i < 4; i++) {
    if (scsi_tur() == 0)
      break;
    uint8_t sense[18];
    scsi_request_sense(sense);
    timer_sleep(50);
  }

  uint8_t inq[36];
  msc_model[0] = '\0';
  if (scsi_inquiry(inq) == 0) {
    int o = 0;
    for (int i = 8; i < 32 && o < 38; i++)
      msc_model[o++] = (inq[i] >= 0x20 && inq[i] < 0x7F) ? (char)inq[i] : ' ';
    while (o > 0 && msc_model[o - 1] == ' ')
      o--;
    msc_model[o] = '\0';
  }

  uint32_t last, blk;
  if (scsi_read_capacity(&last, &blk) != 0) {
    con_write("usbmsc: READ CAPACITY failed\n");
    return -1;
  }
  if (blk != 512) {
    con_printf("usbmsc: unsupported block size %u\n", blk);
    return -1;
  }
  msc_capacity = last + 1;
  msc_ready = 1;
  con_printf("usbmsc: %s %u MiB ready\n", msc_model[0] ? msc_model : "USB disk", msc_capacity / 2048);
  return 0;
}

/* Poll-thread side of the mailbox: run the queued (multi-sector) read/write. */
static void msc_service(void)
{
  msc_result = scsi_rw10(msc_mb_lba, (uint8_t *)msc_mb_buf, msc_mb_count, msc_rw);
  msc_req = 0;
  msc_done = 1;
}

/* Serialize MSC ops and the cache; sti before each sched_yield (context_switch skips EFLAGS). */
static void msc_lock(void)
{
  for (;;) {
    __asm__ volatile("cli");
    if (!msc_busy) {
      msc_busy = 1;
      __asm__ volatile("sti");
      return;
    }
    __asm__ volatile("sti");
    sched_yield();
  }
}
static void msc_unlock(void)
{
  msc_busy = 0;
  __asm__ volatile("sti");
}

/* Hand one op to the poll thread and wait (caller already holds the lock). */
static int msc_io(uint32_t lba, uint8_t *buf, int count, bool write)
{
  msc_mb_lba = lba;
  msc_mb_buf = buf;
  msc_mb_count = count;
  msc_rw = write;
  msc_done = 0;
  msc_req = 1;
  while (!msc_done) {
    __asm__ volatile("sti");
    sched_yield();
  }
  return msc_result;
}

int xhci_msc_present(void) { return msc_ready; }
uint32_t xhci_msc_sectors(void) { return msc_ready ? msc_capacity : 0; }
const char *xhci_msc_model(void) { return msc_ready ? msc_model : ""; }

int xhci_msc_read(uint32_t lba, void *buf)
{
  if (!msc_ready || lba >= msc_capacity)
    return -1;
  msc_lock();

  int rc = 0;
  if (msc_cache) {
    uint32_t base = lba & ~(uint32_t)(MSC_CHUNK - 1);
    int line = -1;
    for (int i = 0; i < MSC_LINES; i++)
      if (msc_ctag[i] == base) {
        line = i;
        break;
      }
    if (line < 0) {
      line = msc_victim;
      msc_victim = (msc_victim + 1) % MSC_LINES;
      msc_ctag[line] = 0xFFFFFFFFu;
      uint32_t cnt = MSC_CHUNK;
      if (base + cnt > msc_capacity)
        cnt = msc_capacity - base;
      if (msc_io(base, msc_cache + line * MSC_LINE_BYTES, (int)cnt, false) != 0)
        rc = -1;
      else
        msc_ctag[line] = base;
    }
    if (rc == 0)
      memcpy(buf, msc_cache + line * MSC_LINE_BYTES + (lba - base) * 512, 512);
  } else {
    rc = msc_io(lba, buf, 1, false);
  }

  msc_unlock();
  /* On a silent controller drop the device so reads fail fast and it leaves the Devices list. */
  if (rc != 0 && !hc_responsive)
    msc_ready = 0;
  return rc;
}

int xhci_msc_write(uint32_t lba, const void *buf)
{
  if (!msc_ready || lba >= msc_capacity)
    return -1;
  msc_lock();
  int rc = msc_io(lba, (uint8_t *)buf, 1, true);
  if (msc_cache) {
    uint32_t base = lba & ~(uint32_t)(MSC_CHUNK - 1);
    for (int i = 0; i < MSC_LINES; i++)
      if (msc_ctag[i] == base) {
        msc_ctag[i] = 0xFFFFFFFFu;
        break;
      }
  }
  msc_unlock();
  if (rc != 0 && !hc_responsive)
    msc_ready = 0;
  return rc;
}

/* MTP / PTP: phone/camera file transfer via the still-image class (06/01/01) over bulk IN/OUT, browsed per object with
 * `mtp` (info / ls / get). Containers: 12-byte header (u32 length, u16 type, u16 code, u32 transaction id) + payload;
 * Command(1), Data(2), Response(3). Read path only. Like mass storage, requests go through a one-slot mailbox to the
 * USB poll thread. */

#define MTP_TYPE_COMMAND 1
#define MTP_TYPE_DATA 2
#define MTP_TYPE_RESPONSE 3

#define MTP_OP_GET_DEVICE_INFO 0x1001
#define MTP_OP_OPEN_SESSION 0x1002
#define MTP_OP_CLOSE_SESSION 0x1003
#define MTP_OP_GET_OBJECT_HANDLES 0x1007
#define MTP_OP_GET_OBJECT_INFO 0x1008
#define MTP_OP_GET_OBJECT 0x1009

#define MTP_RESP_OK 0x2001
#define MTP_FMT_ASSOCIATION 0x3001
#define MTP_PARENT_ROOT 0xFFFFFFFFu
#define MTP_STORAGE_ALL 0xFFFFFFFFu

#define MTP_CHUNK 4096

static bool mtp_ready;
static int mtp_slot;
static int mtp_in_dci, mtp_out_dci;
static uint8_t mtp_in_ep, mtp_out_ep;
static uint16_t mtp_in_mps, mtp_out_mps;
static struct ring mtp_in_ring, mtp_out_ring;
static uint32_t mtp_xid;
static char mtp_model[64];
static uint8_t *mtp_cmd;
static uint8_t *mtp_buf;

/* One-slot mailbox for the poll thread; kind 0 = transaction (data-IN to mb_out), 1 = stream mb_handle. */
static volatile bool mtp_req, mtp_done, mtp_busy;
static volatile int mtp_kind, mtp_result;
static volatile uint32_t mtp_resp_code;
static volatile uint16_t mtp_mb_op;
static volatile uint32_t mtp_mb_param[5];
static volatile int mtp_mb_nparam;
static volatile uint8_t *mtp_mb_out;
static volatile int mtp_mb_outcap;
static volatile uint32_t mtp_mb_handle;
static volatile const char *mtp_mb_dst;

/* Find a still-image (MTP/PTP) interface 06/01/01 and its bulk IN/OUT endpoints. */
static void parse_mtp(struct usbdev *d)
{
  d->is_mtp = 0;
  const uint8_t *p = d->cfg;
  int len = d->cfg_len, in_iface = 0;
  uint8_t bin = 0, bout = 0;
  uint16_t binmps = 0, boutmps = 0;

  for (int i = 0; i + 1 < len;) {
    uint8_t l = p[i], t = p[i + 1];
    if (l < 2)
      break;
    if (t == 0x04 && i + 7 < len) {
      in_iface = (p[i + 5] == 0x06 && p[i + 6] == 0x01 && p[i + 7] == 0x01);
    } else if (t == 0x05 && in_iface && i + 5 < len) {
      uint8_t addr = p[i + 2], attr = p[i + 3];
      uint16_t mps = p[i + 4] | (p[i + 5] << 8);
      if ((attr & 0x03) == 0x02) {
        if (addr & 0x80) {
          bin = addr;
          binmps = mps;
        } else {
          bout = addr;
          boutmps = mps;
        }
      }
    }
    i += l;
  }
  if (bin && bout) {
    d->is_mtp = 1;
    d->bulk_in = bin;
    d->bulk_in_mps = binmps ? binmps : 64;
    d->bulk_out = bout;
    d->bulk_out_mps = boutmps ? boutmps : 64;
  }
}

/* Configure the bulk pair (copy the live slot context, only bump Context Entries). */
static int configure_bulk_mtp(struct usbdev *d)
{
  int in_dci = ep_dci(d->bulk_in);
  int out_dci = ep_dci(d->bulk_out);
  int max_dci = in_dci > out_dci ? in_dci : out_dci;

  ring_init(&mtp_in_ring);
  ring_init(&mtp_out_ring);

  uint8_t *input = palloc();
  if (!input || !mtp_in_ring.base || !mtp_out_ring.base || !dcbaa[d->slot]) {
    if (input)
      pfree(input);
    if (mtp_in_ring.base) {
      pfree(mtp_in_ring.base);
      mtp_in_ring.base = 0;
    }
    if (mtp_out_ring.base) {
      pfree(mtp_out_ring.base);
      mtp_out_ring.base = 0;
    }
    return -1;
  }
  uint32_t *icc = (uint32_t *)input;
  uint32_t *sctx = (uint32_t *)(input + ctx_sz * 1);
  uint32_t *epin = (uint32_t *)(input + ctx_sz * (1 + in_dci));
  uint32_t *epout = (uint32_t *)(input + ctx_sz * (1 + out_dci));

  icc[1] = (1u << 0) | (1u << in_dci) | (1u << out_dci);

  uint32_t *out_slot = (uint32_t *)(uintptr_t)dcbaa[d->slot];
  for (uint32_t i = 0; i < ctx_sz / 4; i++)
    sctx[i] = out_slot[i];
  sctx[0] = (sctx[0] & ~(0x1Fu << 27)) | ((uint32_t)max_dci << 27);

  epin[1] = (3u << 1) | (6u << 3) | ((uint32_t)d->bulk_in_mps << 16);
  epin[2] = (uint32_t)(uintptr_t)mtp_in_ring.base | 1;
  epin[4] = d->bulk_in_mps;
  epout[1] = (3u << 1) | (2u << 3) | ((uint32_t)d->bulk_out_mps << 16);
  epout[2] = (uint32_t)(uintptr_t)mtp_out_ring.base | 1;
  epout[4] = d->bulk_out_mps;

  ring_push(&cmd_ring, (uint32_t)(uintptr_t)input, 0, 0, TRB_TYPE(TRB_CONFIG_EP) | ((uint32_t)d->slot << 24));
  int cc = cmd_submit();
  pfree(input);
  if (cc != CC_SUCCESS) {
    pfree(mtp_in_ring.base);
    mtp_in_ring.base = 0;
    pfree(mtp_out_ring.base);
    mtp_out_ring.base = 0;
    return -1;
  }

  mtp_slot = d->slot;
  mtp_in_dci = in_dci;
  mtp_out_dci = out_dci;
  mtp_in_ep = d->bulk_in;
  mtp_out_ep = d->bulk_out;
  mtp_in_mps = d->bulk_in_mps;
  mtp_out_mps = d->bulk_out_mps;
  return 0;
}

/* Send a Command container (op + up to 5 params) on the bulk-OUT pipe. */
static uint32_t mtp_send_command(uint16_t op, const uint32_t *params, int nparams)
{
  if (nparams < 0)
    nparams = 0;
  if (nparams > 5)
    nparams = 5;
  int clen = 12 + 4 * nparams;
  uint32_t xid = ++mtp_xid;
  put_le32(mtp_cmd + 0, (uint32_t)clen);
  mtp_cmd[4] = MTP_TYPE_COMMAND;
  mtp_cmd[5] = 0;
  mtp_cmd[6] = op & 0xFF;
  mtp_cmd[7] = op >> 8;
  put_le32(mtp_cmd + 8, xid);
  for (int i = 0; i < nparams; i++)
    put_le32(mtp_cmd + 12 + 4 * i, params[i]);

  int cc;
  if (bulk_xfer(mtp_slot, &mtp_out_ring, mtp_out_dci, mtp_cmd, clen, &cc) < 0)
    return 0;
  return xid;
}

/* Read a Response container (type 3) into mtp_cmd; returns its response code, or 0 on transport error. */
static uint32_t mtp_read_response(void)
{
  int cc;
  if (bulk_xfer(mtp_slot, &mtp_in_ring, mtp_in_dci, mtp_cmd, MTP_CHUNK, &cc) < 0)
    return 0;
  if (mtp_cmd[4] != MTP_TYPE_RESPONSE)
    return 0;
  return (uint32_t)mtp_cmd[6] | ((uint32_t)mtp_cmd[7] << 8);
}

/* IN (or no) data phase: send command, read Data (minus 12-byte header) into out, then Response. */
static int mtp_do_transaction(uint16_t op, const uint32_t *params, int nparams, uint8_t *out, int outcap,
                              uint32_t *rcode)
{
  *rcode = 0;
  if (mtp_send_command(op, params, nparams) == 0)
    return -1;

  int payload = 0;
  if (out && outcap > 0) {
    int cc;
    int got = bulk_xfer(mtp_slot, &mtp_in_ring, mtp_in_dci, mtp_buf, MTP_CHUNK, &cc);
    if (got < 0)
      return -1;
    if (mtp_buf[4] == MTP_TYPE_DATA && got >= 12) {
      uint32_t total = load_le32(mtp_buf);
      int plen = (int)total - 12;
      if (plen < 0)
        plen = 0;
      if (plen > got - 12)
        plen = got - 12;
      if (plen > outcap)
        plen = outcap;
      for (int i = 0; i < plen; i++)
        out[i] = mtp_buf[12 + i];
      payload = plen;
    } else if (mtp_buf[4] == MTP_TYPE_RESPONSE) {
      /* No data phase -- this was already the response. */
      *rcode = (uint32_t)mtp_buf[6] | ((uint32_t)mtp_buf[7] << 8);
      return 0;
    }
  }

  *rcode = mtp_read_response();
  if (*rcode != MTP_RESP_OK)
    return -1;
  return payload;
}

/* Stream object `handle` to owofs `dst` on the poll thread; 0 on success. */
static int mtp_do_get(uint32_t handle, const char *dst, uint32_t *rcode)
{
  *rcode = 0;
  uint32_t p[1] = {handle};
  if (mtp_send_command(MTP_OP_GET_OBJECT, p, 1) == 0)
    return -1;

  int cc;
  int got = bulk_xfer(mtp_slot, &mtp_in_ring, mtp_in_dci, mtp_buf, MTP_CHUNK, &cc);
  if (got < 12 || mtp_buf[4] != MTP_TYPE_DATA)
    return -1;
  uint32_t total = load_le32(mtp_buf);
  uint32_t fsize = total >= 12 ? total - 12 : 0;

  if (fs_prealloc(dst, fsize) != 0)
    return -1;

  uint32_t written = 0;
  int first = (int)(got - 12);
  if (first > (int)fsize)
    first = (int)fsize;
  if (first > 0) {
    if (fs_write_at(dst, 0, mtp_buf + 12, (uint32_t)first) < 0)
      return -1;
    written = (uint32_t)first;
  }
  while (written < fsize) {
    uint32_t want = fsize - written;
    if (want > MTP_CHUNK)
      want = MTP_CHUNK;
    got = bulk_xfer(mtp_slot, &mtp_in_ring, mtp_in_dci, mtp_buf, (int)want, &cc);
    if (got <= 0)
      return -1;
    if (fs_write_at(dst, written, mtp_buf, (uint32_t)got) < 0)
      return -1;
    written += (uint32_t)got;
  }

  *rcode = mtp_read_response();
  return *rcode == MTP_RESP_OK ? 0 : -1;
}

/* MTP string: 1-byte UTF-16 unit count (0 == empty) then UTF-16LE; low bytes copied as ASCII. */
static void mtp_get_string(const uint8_t *buf, int len, int *off, char *out, int cap)
{
  out[0] = '\0';
  if (*off >= len)
    return;
  int n = buf[(*off)++];
  int o = 0;
  for (int i = 0; i < n && *off + 1 < len; i++) {
    uint8_t lo = buf[*off];
    *off += 2;
    if (i < n - 1 && o < cap - 1)
      out[o++] = (lo >= 0x20 && lo < 0x7F) ? (char)lo : '?';
  }
  out[o] = '\0';
}

/* Skip an MTP array (u32 count + stride-byte elements); code arrays are AUINT16, storage-id/handle arrays AUINT32. */
static void mtp_skip_array(const uint8_t *buf, int len, int *off, int stride)
{
  if (*off + 4 > len) {
    *off = len;
    return;
  }
  uint32_t cnt = load_le32(buf + *off);
  /* cnt is device-controlled: clamp the 64-bit next offset to len so *off can't go negative. */
  uint64_t next = (uint64_t)*off + 4 + (uint64_t)cnt * (uint32_t)stride;
  *off = (next > (uint64_t)len) ? len : (int)next;
}
/* Skip an MTP string. */
static void mtp_skip_string(const uint8_t *buf, int len, int *off)
{
  char tmp[2];
  mtp_get_string(buf, len, off, tmp, sizeof tmp);
}

/* Bring up an MTP device: configure the bulk pipes, read DeviceInfo for the model string, and open a session. */
static int bring_up_mtp(struct usbdev *d, int port)
{
  (void)port;
  if (configure_bulk_mtp(d) != 0) {
    con_write("mtp: configure-endpoint failed\n");
    return -1;
  }
  if (!mtp_cmd)
    mtp_cmd = palloc();
  if (!mtp_buf)
    mtp_buf = palloc();
  mtp_xid = 0;

  /* DeviceInfo: skip version/extension/array fields to Manufacturer + Model. */
  uint32_t rc;
  int n = mtp_do_transaction(MTP_OP_GET_DEVICE_INFO, 0, 0, mtp_buf, MTP_CHUNK, &rc);
  mtp_model[0] = '\0';
  if (n > 0) {
    int o = 0;
    o += 2;
    o += 4;
    o += 2;
    mtp_skip_string(mtp_buf, n, &o);
    o += 2;
    mtp_skip_array(mtp_buf, n, &o, 2);
    mtp_skip_array(mtp_buf, n, &o, 2);
    mtp_skip_array(mtp_buf, n, &o, 2);
    mtp_skip_array(mtp_buf, n, &o, 2);
    mtp_skip_array(mtp_buf, n, &o, 2);
    char manu[32], modl[32];
    mtp_get_string(mtp_buf, n, &o, manu, sizeof manu);
    mtp_get_string(mtp_buf, n, &o, modl, sizeof modl);
    int k = 0;
    for (int i = 0; manu[i] && k < (int)sizeof(mtp_model) - 2; i++)
      mtp_model[k++] = manu[i];
    if (k && modl[0])
      mtp_model[k++] = ' ';
    for (int i = 0; modl[i] && k < (int)sizeof(mtp_model) - 1; i++)
      mtp_model[k++] = modl[i];
    mtp_model[k] = '\0';
  }

  uint32_t sp[1] = {1};
  if (mtp_do_transaction(MTP_OP_OPEN_SESSION, sp, 1, 0, 0, &rc) != 0 && rc != 0x201E /* SessionAlreadyOpen */) {
    con_printf("mtp: OpenSession failed rc=%04x\n", rc);
    return -1;
  }

  mtp_ready = 1;
  con_printf("mtp: %s ready (file transfer)\n", mtp_model[0] ? mtp_model : "device");
  return 0;
}

static void mtp_service(void)
{
  uint32_t rc = 0;
  uint32_t p[5];
  for (int i = 0; i < 5; i++)
    p[i] = mtp_mb_param[i];
  if (mtp_kind == 0)
    mtp_result = mtp_do_transaction(mtp_mb_op, p, mtp_mb_nparam, (uint8_t *)mtp_mb_out, mtp_mb_outcap, &rc);
  else
    mtp_result = mtp_do_get(mtp_mb_handle, (const char *)mtp_mb_dst, &rc);
  mtp_resp_code = rc;
  mtp_req = 0;
  mtp_done = 1;
}

/* Serialize MTP ops across caller threads; every yield is preceded by `sti` (see the matching note in msc_lock). */
static void mtp_lock(void)
{
  for (;;) {
    __asm__ volatile("cli");
    if (!mtp_busy) {
      mtp_busy = 1;
      __asm__ volatile("sti");
      return;
    }
    __asm__ volatile("sti");
    sched_yield();
  }
}
static void mtp_unlock(void)
{
  mtp_busy = 0;
  __asm__ volatile("sti");
}
static void mtp_wait(void)
{
  mtp_done = 0;
  mtp_req = 1;
  while (!mtp_done) {
    __asm__ volatile("sti");
    sched_yield();
  }
}

/* Run a data-IN transaction via the poll thread (caller holds the lock). */
static int mtp_xfer(uint16_t op, const uint32_t *params, int nparams, uint8_t *out, int outcap)
{
  mtp_mb_op = op;
  mtp_mb_nparam = nparams;
  for (int i = 0; i < 5; i++)
    mtp_mb_param[i] = i < nparams ? params[i] : 0;
  mtp_mb_out = out;
  mtp_mb_outcap = outcap;
  mtp_kind = 0;
  mtp_wait();
  return mtp_result;
}

int xhci_mtp_present(void) { return mtp_ready; }
const char *xhci_mtp_model(void) { return mtp_ready ? mtp_model : ""; }

/* List children of `parent` (MTP_PARENT_ROOT = storage root) via cb(handle, name, size, is_dir). */
int xhci_mtp_list(uint32_t parent, void (*cb)(uint32_t handle, const char *name, uint32_t size, int is_dir, void *ctx),
                  void *ctx)
{
  if (!mtp_ready)
    return -1;
  static uint8_t handles[MTP_CHUNK];
  static uint8_t info[MTP_CHUNK];
  mtp_lock();

  uint32_t hp[3] = {MTP_STORAGE_ALL, 0, parent};
  int n = mtp_xfer(MTP_OP_GET_OBJECT_HANDLES, hp, 3, handles, sizeof handles);
  if (n < 4) {
    mtp_unlock();
    return n < 0 ? -1 : 0;
  }

  uint32_t count = load_le32(handles);
  if (count > (uint32_t)(n - 4) / 4)
    count = (uint32_t)(n - 4) / 4;

  int listed = 0;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t h = load_le32(handles + 4 + i * 4);
    uint32_t ip[1] = {h};
    int in = mtp_xfer(MTP_OP_GET_OBJECT_INFO, ip, 1, info, sizeof info);
    if (in < 52)
      continue;
    uint16_t fmt = (uint16_t)(info[4] | (info[5] << 8));
    uint32_t size = load_le32(info + 8);
    int o = 52;
    char name[64];
    mtp_get_string(info, in, &o, name, sizeof name);
    if (cb)
      cb(h, name, size, fmt == MTP_FMT_ASSOCIATION, ctx);
    listed++;
  }
  mtp_unlock();
  return listed;
}

/* Download object `handle` to owofs path `dst`. */
int xhci_mtp_get(uint32_t handle, const char *dst)
{
  if (!mtp_ready)
    return -1;
  mtp_lock();
  mtp_mb_handle = handle;
  mtp_mb_dst = dst;
  mtp_kind = 1;
  mtp_wait();
  int rc = mtp_result;
  mtp_unlock();
  return rc;
}

/* Walk a configuration descriptor blob and print its interfaces and endpoints. */
static void print_config(const uint8_t *cfg, int total)
{
  for (int i = 0; i + 2 <= total;) {
    uint8_t blen = cfg[i];
    if (blen < 2)
      break;
    uint8_t btype = cfg[i + 1];
    if (btype == 0x04 && i + 9 <= total) {
      con_printf("         if %u class %02x/%02x/%02x\n", cfg[i + 2], cfg[i + 5], cfg[i + 6], cfg[i + 7]);
    } else if (btype == 0x05 && i + 7 <= total) {
      uint8_t addr = cfg[i + 2];
      uint8_t attr = cfg[i + 3] & 0x3;
      const char *t = attr == 2 ? "bulk" : attr == 3 ? "intr" : attr == 1 ? "isoc" : "ctrl";
      con_printf("           ep %02x%s%s\n", addr, (addr & 0x80) ? " IN  " : " OUT ", t);
    }
    i += blen;
  }
}

/* Enable a slot for the device on `port`, address it, read its device descriptor, print vendor:product + class. */
/* Enable a slot for the device on `port`, address it, read descriptors, SET_CONFIGURATION, cache. */
static void enumerate_device(uint32_t port, int speed)
{
  struct usbdev *d = &port_dev[port];
  d->active = 0;

  ring_push(&cmd_ring, 0, 0, 0, TRB_TYPE(TRB_ENABLE_SLOT));
  db[0] = 0;
  struct trb *e = event_wait();
  if (!e || TRB_GET_TYPE(e->control) != TRB_CMD_COMPLETION || ((e->status >> 24) & 0xFF) != CC_SUCCESS)
    return;
  int slot_id = (e->control >> 24) & 0xFF;

  if (address_device(slot_id, (int)port, speed) != 0)
    return;

  uint8_t desc[18];
  /* Two-step read: 8 bytes first to learn bMaxPacketSize0, fix EP0 via Evaluate Context if it differs. */
  if (control_in(slot_id, 0x80, 0x06, 0x0100, 0, desc, 8) != 8)
    return;
  if (speed < 4) {
    uint32_t guessed = (speed == 3) ? 64 : 8;
    uint32_t real = desc[7] ? desc[7] : 8;
    if (real != guessed)
      eval_context_mps(slot_id, real);
  }
  if (control_in(slot_id, 0x80, 0x06, 0x0100, 0, desc, 18) != 18)
    return;

  d->slot = slot_id;
  d->speed = speed;
  d->vid = desc[8] | (desc[9] << 8);
  d->pid = desc[10] | (desc[11] << 8);
  d->cls = desc[4];
  d->cfg_len = 0;

  /* Read the config descriptor (9-byte header for wTotalLength, then all), cache it, select it. */
  if (control_in(slot_id, 0x80, 0x06, 0x0200, 0, d->cfg, 9) == 9) {
    int total = d->cfg[2] | (d->cfg[3] << 8);
    if (total > (int)sizeof d->cfg)
      total = sizeof d->cfg;
    if (control_in(slot_id, 0x80, 0x06, 0x0200, 0, d->cfg, total) == total) {
      d->cfg_len = total;
      control_no_data(slot_id, 0x00, 0x09, d->cfg[5], 0);
    }
  }
  d->active = 1;

  /* Classify the device and bring up its driver; MTP first (its bulk pair looks like USB-Ethernet). */
  d->is_net = 0;
  d->mac_ok = 0;
  d->is_mtp = 0;
  d->is_msc = 0;
  parse_mtp(d);
  if (d->is_mtp) {
    if (!mtp_ready)
      bring_up_mtp(d, (int)port);
  } else {
    /* RNDIS: INITIALIZE and learn the MAC now over EP0 (the global ep0_ring is still this device's). */
    parse_net(d);
    if (d->is_net && rndis_setup(d) == 0 && !net_up)
      bring_up_net(d, (int)port);

    /* Mass storage: configure the bulk pipes and probe capacity now (single ring context). */
    parse_msc(d);
    if (d->is_msc && !msc_ready)
      bring_up_msc(d, (int)port);
  }
}

/* Print the cached device table, with interface/endpoint detail for the requested port. */
static void print_devs(int detail_port)
{
  int any = 0;
  for (uint32_t p = 1; p <= max_ports; p++) {
    struct usbdev *d = &port_dev[p];
    if (!d->active)
      continue;
    any = 1;
    con_printf("xhci: port %u: %04x:%04x class %02x speed %u", p, d->vid, d->pid, d->cls, d->speed);
    if (d->mac_ok)
      con_write(" [net]");
    if (d->is_mtp)
      con_write(" [mtp]");
    con_putc('\n');
    if (detail_port == (int)p) {
      if (d->cfg_len)
        print_config(d->cfg, d->cfg_len);
      if (d->is_net && d->mac_ok) {
        con_write("         rndis mac ");
        for (int i = 0; i < 6; i++) {
          con_put_hex(d->mac[i], 2);
          if (i < 5)
            con_putc(':');
        }
        con_putc('\n');
      } else if (d->is_net) {
        con_write("         rndis: init failed\n");
      }
    }
  }
  if (!any)
    con_write("xhci: no usable device on any port\n");
}

/* Bring the controller up once; kept apart from enumeration so `lsusb` never resets it. */
static int setup_done;

static int setup_controller(void)
{
  if (setup_done)
    return 0;

  cap = find_and_map();
  if (!cap)
    return -1;

  uint32_t caplen = r32(cap, XHCI_CAPLENGTH) & 0xFF;
  op = cap + caplen;
  rt = cap + (r32(cap, XHCI_RTSOFF) & ~0x1Fu);
  db = (volatile uint32_t *)(cap + (r32(cap, XHCI_DBOFF) & ~0x3u));

  uint32_t hcs1 = r32(cap, XHCI_HCSPARAMS1);
  uint32_t max_slots = hcs1 & 0xFF;
  max_ports = (hcs1 >> 24) & 0xFF;
  ctx_sz = (r32(cap, XHCI_HCCPARAMS1) & (1u << 2)) ? 64 : 32;

  xhci_takeover();

  if (controller_reset() != 0) {
    con_write("xhci: controller reset timed out\n");
    return -1;
  }

  /* Device context base array; tell the controller how many slots to enable. */
  dcbaa = palloc();
  setup_scratchpad();
  w64(op, XHCI_DCBAAP, (uint64_t)(uintptr_t)dcbaa);
  w32(op, XHCI_CONFIG, max_slots);

  /* Command ring. CRCR points at it with the ring cycle state (RCS=1). */
  ring_init(&cmd_ring);
  w64(op, XHCI_CRCR, (uint64_t)(uintptr_t)cmd_ring.base | 1);

  /* Event ring: one segment + a one-entry segment table. */
  event_ring = palloc();
  event_cycle = TRB_CYCLE;
  event_idx = 0;
  uint32_t *erst = palloc();
  erst[0] = (uint32_t)(uintptr_t)event_ring;
  erst[1] = 0;
  erst[2] = RING_TRBS;
  erst[3] = 0;
  w32(rt, XHCI_ERSTSZ, 1);
  w64(rt, XHCI_ERDP, (uint64_t)(uintptr_t)event_ring);
  w64(rt, XHCI_ERSTBA, (uint64_t)(uintptr_t)erst);

  /* Run, and leave it running. */
  w32(op, XHCI_USBCMD, r32(op, XHCI_USBCMD) | CMD_RS);
  for (int ms = 0; ms < 100; ms++) {
    if (!(r32(op, XHCI_USBSTS) & STS_HCH))
      break;
    timer_sleep(1);
  }

  /* Power the ports (real HW comes up unpowered) and let them settle. */
  power_all_ports();

  setup_done = 1;
  return 0;
}

/* Enumerate a port only when newly connected or CSC changed; stable devices keep their cache. */
static void scan_ports(void)
{
  for (uint32_t p = 1; p <= max_ports && p < XHCI_MAX_PORTS; p++) {
    uint32_t sc = r32(op, XHCI_PORTSC(p));
    int changed = (sc & PORT_CSC) != 0;
    if (changed)
      w32(op, XHCI_PORTSC(p), (sc & ~PORT_RWC_MASK & ~PORT_PED) | PORT_CSC);

    if (sc & PORT_CCS) {
      if (!port_dev[p].active || changed) {
        int speed = reset_port(p);
        if (speed)
          enumerate_device(p, speed);
        else
          port_dev[p].active = 0;
      }
    } else {
      /* Device gone; if it was the mass-storage device, drop the cached MSC state. */
      if (port_dev[p].is_msc)
        msc_ready = 0;
      port_dev[p].active = 0;
      port_dev[p].is_msc = 0;
    }
  }
}

/* The only post-boot port scan, so the rings have a single context; tethering toggles reconfigure within a poll. */
static void usb_poll_thread(void)
{
  int idle = 0;
  for (;;) {
    if (net_up)
      data_loop();
    /* Service queued mass-storage ops promptly (~1 ms) and rescan ports for hotplug occasionally (~250 ms). */
    if (msc_req) {
      msc_service();
      idle = 0;
      continue;
    }
    if (mtp_req) {
      mtp_service();
      idle = 0;
      continue;
    }
    if (++idle >= 250) {
      scan_ports();
      idle = 0;
    }
    timer_sleep(1);
  }
}

/* `lsusb`: print the cached table only; scanning here would drive the rings from two contexts. */
int xhci_init(int detail_port)
{
  if (!setup_done)
    return 0;
  print_devs(detail_port);
  return 1;
}

/* Boot-time: bring the host up, configure devices once, then hand scanning to a background thread. */
void xhci_boot(void)
{
  if (setup_controller() != 0)
    return;
  scan_ports();
  sched_create_thread("usbpoll", usb_poll_thread);
}
