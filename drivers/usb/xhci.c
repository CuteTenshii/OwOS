/* xhci.c - minimal xHCI (USB 3.x) host controller driver, milestone 1.
 *
 * Goal of this file's first cut: bring an xHCI controller out of reset, stand
 * up the four DMA structures it needs (device-context base array, command ring,
 * event ring, scratchpad), then enumerate the device on a connected port -
 * enable a slot, address the device, and run a control transfer to read its
 * 18-byte device descriptor. That proves the whole control path (rings,
 * doorbells, contexts, transfer events) end to end; bulk transfers and the
 * RNDIS net driver build on exactly this machinery.
 *
 * All DMA memory comes from palloc() - zeroed, 4 KiB-aligned, and identity-
 * mapped (physical == virtual), so a pointer we dereference is the same address
 * we hand the controller. The MMIO register file is mapped uncached.
 *
 * Heavily instrumented on purpose: xHCI bring-up is fiddly and this is debugged
 * by booting in QEMU (`-device qemu-xhci -device usb-net`) and reading the COM1
 * log, so each stage announces itself and its result.
 */

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
#include "usbhost.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PCI_CLASS_SERIAL_BUS 0x0C
#define PCI_SUBCLASS_USB 0x03
#define USB_PROGIF_XHCI 0x30

/* ---- capability registers (at MMIO base) -------------------------------- */
#define XHCI_CAPLENGTH 0x00  /* byte: length of the capability region      */
#define XHCI_HCSPARAMS1 0x04 /* MaxSlots[7:0], MaxPorts[31:24]              */
#define XHCI_HCSPARAMS2 0x08 /* scratchpad buffer count (split fields)      */
#define XHCI_HCCPARAMS1 0x10 /* CSZ[2]=64-byte contexts, xECP[31:16]        */
#define XHCI_DBOFF 0x14      /* doorbell array offset from base             */
#define XHCI_RTSOFF 0x18     /* runtime register offset from base          */

/* ---- operational registers (at base + CAPLENGTH) ------------------------ */
#define XHCI_USBCMD 0x00
#define XHCI_USBSTS 0x04
#define XHCI_PAGESIZE 0x08
#define XHCI_CRCR 0x18                            /* command ring control (64-bit)              */
#define XHCI_DCBAAP 0x30                          /* device context base address array (64-bit) */
#define XHCI_CONFIG 0x38                          /* MaxSlotsEn[7:0]                            */
#define XHCI_PORTSC(p) (0x400 + ((p) - 1) * 0x10) /* per-port, 1-based    */

/* USBCMD / USBSTS bits. */
#define CMD_RS (1u << 0)    /* run/stop                             */
#define CMD_HCRST (1u << 1) /* host controller reset                */
#define STS_HCH (1u << 0)   /* HC halted                            */
#define STS_CNR (1u << 11)  /* controller not ready                 */

/* PORTSC bits. */
#define PORT_CCS (1u << 0)                /* current connect status               */
#define PORT_PED (1u << 1)                /* port enabled/disabled                */
#define PORT_PR (1u << 4)                 /* port reset                           */
#define PORT_PP (1u << 9)                 /* port power                           */
#define PORT_PLS(v) (((v) >> 5) & 0xF)    /* port link state      */
#define PORT_SPEED(v) (((v) >> 10) & 0xF) /* port speed id        */
/* Write-1-to-clear status-change bits live in the high half; preserve PED/PR
 * and the change bits carefully on every write (PED is RW1CS, so writing 1
 * disables the port - we must write 0 there unless we mean to). */
#define PORT_CSC (1u << 17)
#define PORT_PRC (1u << 21)
#define PORT_RWC_MASK (PORT_CSC | (1u << 18) | (1u << 19) | (1u << 20) | PORT_PRC | (1u << 22))

/* ---- runtime registers (at base + RTSOFF) ------------------------------- */
#define XHCI_IMAN 0x20   /* interrupter 0 management                   */
#define XHCI_IMOD 0x24   /* interrupter 0 moderation                   */
#define XHCI_ERSTSZ 0x28 /* event ring segment table size              */
#define XHCI_ERSTBA 0x30 /* event ring segment table base (64-bit)     */
#define XHCI_ERDP 0x38   /* event ring dequeue pointer (64-bit)        */

/* ---- TRB types ---------------------------------------------------------- */
#define TRB_NORMAL 1
#define TRB_SETUP 2
#define TRB_DATA 3
#define TRB_STATUS 4
#define TRB_LINK 6
#define TRB_ENABLE_SLOT 9
#define TRB_DISABLE_SLOT 10
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
#define TRB_ISP (1u << 2)
#define TRB_IOC (1u << 5)    /* interrupt on completion              */
#define TRB_IDT (1u << 6)    /* immediate data (setup stage)         */
#define TRB_TOGGLE (1u << 1) /* link TRB: toggle cycle               */

/* TRB completion codes (event status >> 24). */
#define CC_SUCCESS 1
#define CC_BABBLE 3      /* device sent more than expected; halts the ep    */
#define CC_USB_TXN_ERR 4 /* USB transaction error; can halt the ep          */
#define CC_STALL 6       /* endpoint STALL; halted until reset              */
#define CC_SHORT_PKT 13  /* transfer ended early (normal for variable data) */
#define CC_STOPPED 26
#define CC_STOPPED_LEN 27

/* A Transfer Request Block: four little-endian dwords. */
struct trb {
  volatile uint32_t p0;
  volatile uint32_t p1;
  volatile uint32_t status;
  volatile uint32_t control;
};

#define RING_TRBS 256 /* one 4 KiB page holds 256 16-byte TRBs             */

/* A software view of a single-segment ring with a wrapping Link TRB. */
struct ring {
  struct trb *base;
  uint32_t enq;   /* next enqueue index                            */
  uint32_t cycle; /* producer cycle state (1 then toggles on wrap) */
};

/* ------------------------------------------------------------------ state */
static volatile uint8_t *cap; /* capability registers (MMIO base)        */
static volatile uint8_t *op;  /* operational registers                   */
static volatile uint8_t *rt;  /* runtime registers                       */
static volatile uint32_t *db; /* doorbell array                          */
static uint32_t ctx_sz;       /* 32 or 64 bytes per context              */
static uint32_t max_ports;

static uint64_t *dcbaa; /* device context base address array       */
static struct ring cmd_ring;
static struct trb *event_ring; /* event ring segment (consumer)           */
static uint32_t event_cycle;   /* consumer cycle state                    */
static uint32_t event_idx;
/* Cleared whenever event_wait() times out (the controller posted no completion
 * in 500 ms) and set on any successful event. A wedged/yanked device that stops
 * completing transfers flips this to 0, so callers can drop it instead of
 * retrying forever. */
static volatile bool hc_responsive = true;

/* Devices by slot ID (8 bits, so at most 255); a device owns its EP0 ring and interrupt pipes. */
#define XHCI_MAX_SLOTS 256
#define PIPE_BUFS 2

struct usbh_pipe {
  struct usbh_pipe *next;
  int slot, dci;
  uint8_t ep_addr;
  uint16_t mps;
  struct ring ring;
  uint8_t *buf;
  uint8_t head, tail, queued, errors;
  bool halted, dead;
  usbh_pipe_cb cb;
  void *ctx;
};

struct xdev {
  struct usbdev pub;
  struct ring ep0;
  struct usbh_pipe *pipes;
};
static struct xdev *devs[XHCI_MAX_SLOTS];
static int *root_slot;
static int max_slot_id;
static volatile bool port_event;
static volatile bool pipe_halted;

/* The active RNDIS USB-Ethernet data path (there is one tethered NIC). Set up
 * once the device is enumerated + RNDIS-initialized; the USB thread then runs a
 * bulk TX/RX loop over these. */
static bool net_up; /* 1 once the bulk data path is live        */
static int net_slot;
static int net_in_dci, net_out_dci;
static uint32_t net_in_mps;
static struct ring net_in_ring, net_out_ring;
static uint8_t *net_rx_buf; /* one outstanding bulk-IN lands here       */
static uint8_t *net_tx_buf; /* one outstanding bulk-OUT is built here   */
static uint8_t net_mac[6];
static bool net_in_pending, net_out_pending;
static uint8_t net_in_ep;     /* bulk-IN endpoint address (for CLEAR_HALT) */
static int net_stall_retries; /* bound stall recovery so it can't spin    */
static int net_intr_dci;      /* RNDIS status (interrupt-IN) endpoint      */
static struct ring net_intr_ring;
static uint8_t *net_intr_buf;
static bool net_intr_pending;
static int net_ctrl_iface;           /* interface that takes encapsulated cmds    */
static int dbg_rx, dbg_tx, dbg_intr; /* one-shot diagnostics counters     */

/* Software TX queue: nic_send (command context) drops frames here; the USB
 * thread drains them onto the bulk-OUT ring. Fixed slots, no allocation. */
#define TXQ_SLOTS 16
#define TXQ_FRAME 1600
static uint8_t txq[TXQ_SLOTS][TXQ_FRAME];
static uint16_t txq_len[TXQ_SLOTS];
static volatile int txq_head, txq_tail; /* head=produce, tail=consume */

/* ---- small helpers ------------------------------------------------------ */

static inline uint32_t r32(volatile uint8_t *p, uint32_t off) { return *(volatile uint32_t *)(p + off); }
static inline void w32(volatile uint8_t *p, uint32_t off, uint32_t v) { *(volatile uint32_t *)(p + off) = v; }
static inline void w64(volatile uint8_t *p, uint32_t off, uint64_t v)
{
  *(volatile uint32_t *)(p + off) = (uint32_t)v;
  *(volatile uint32_t *)(p + off + 4) = (uint32_t)(v >> 32);
}

/* Initialize a producer ring in a fresh page, with a Link TRB in the last slot
 * that points back to the top and toggles the cycle bit on wrap. */
static void ring_init(struct ring *r)
{
  r->base = palloc();
  r->enq = 0;
  r->cycle = 1;
  if (!r->base) /* palloc pool exhausted: caller checks base */
    return;
  struct trb *link = &r->base[RING_TRBS - 1];
  link->p0 = (uint32_t)(uintptr_t)r->base;
  link->p1 = 0;
  link->status = 0;
  link->control = TRB_TYPE(TRB_LINK) | TRB_TOGGLE; /* cycle set on enqueue */
}

/* Enqueue one TRB (caller fills p0/p1/status and the type/flag bits of control,
 * minus the cycle bit which we apply). Advances the ring, handling the Link. */
static void ring_push(struct ring *r, uint32_t p0, uint32_t p1, uint32_t status, uint32_t control)
{
  struct trb *t = &r->base[r->enq];
  t->p0 = p0;
  t->p1 = p1;
  t->status = status;
  t->control = control | (r->cycle ? TRB_CYCLE : 0);

  r->enq++;
  if (r->enq == RING_TRBS - 1) { /* reached the Link TRB             */
    struct trb *link = &r->base[RING_TRBS - 1];
    if (r->cycle)
      link->control |= TRB_CYCLE;
    else
      link->control &= ~TRB_CYCLE;
    r->enq = 0;
    r->cycle ^= 1;
  }
}

/* Next event off the ring (advances ERDP), or NULL when it is empty. */
static struct trb *event_next(void)
{
  struct trb *e = &event_ring[event_idx];
  if ((e->control & TRB_CYCLE) != event_cycle)
    return NULL;
  event_idx++;
  if (event_idx == RING_TRBS) {
    event_idx = 0;
    event_cycle ^= 1;
  }
  w64(rt, XHCI_ERDP, (uint64_t)(uintptr_t)&event_ring[event_idx]);
  return e;
}

static bool pipe_event(struct trb *e);

/* Events no synchronous waiter owns: port changes (flagged for the poll thread) and interrupt-pipe completions. */
static bool event_consumed(struct trb *e)
{
  uint32_t type = TRB_GET_TYPE(e->control);
  if (type == TRB_PORT_STATUS) {
    port_event = true;
    return true;
  }
  return type == TRB_TRANSFER_EVENT && pipe_event(e);
}

/* Transfer events for another endpoint met while waiting (an RNDIS bulk completion during a control request) wait here
 * for data_loop; without a tether nothing reads them. */
static struct trb *defer_q;
static uint32_t defer_n, defer_cap;

static void defer_event(const struct trb *e)
{
  if (!net_up || TRB_GET_TYPE(e->control) != TRB_TRANSFER_EVENT)
    return;
  if (defer_n == defer_cap) {
    uint32_t ncap = defer_cap ? defer_cap * 2 : 16;
    struct trb *n = kmalloc(ncap * sizeof *n);
    if (!n)
      return;
    for (uint32_t i = 0; i < defer_n; i++)
      n[i] = defer_q[i];
    kfree(defer_q);
    defer_q = n;
    defer_cap = ncap;
  }
  struct trb *d = &defer_q[defer_n++];
  d->p0 = e->p0;
  d->p1 = e->p1;
  d->status = e->status;
  d->control = e->control;
}

/* Wait (bounded) for the command completion, or the transfer event of (slot, dci); NULL on timeout. */
static struct trb *event_wait(uint32_t type, int slot, int dci)
{
  for (int ms = 0; ms < 500; ms++) {
    struct trb *e;
    while ((e = event_next())) {
      if (event_consumed(e))
        continue;
      uint32_t t = TRB_GET_TYPE(e->control);
      if (t == type && (t == TRB_CMD_COMPLETION ||
                        ((int)((e->control >> 24) & 0xFF) == slot && (int)((e->control >> 16) & 0x1F) == dci))) {
        hc_responsive = 1;
        return e;
      }
      defer_event(e);
    }
    timer_sleep(1);
  }
  hc_responsive = 0; /* no completion in 500 ms: controller not responding */
  return NULL;
}

/* Non-blocking: return the next event if one is ready (advancing ERDP), else
 * NULL. Port-status events are consumed and skipped. Used by the data loop,
 * where bulk IN and OUT completions interleave and must be routed by endpoint.
 */
static struct trb *event_poll(void)
{
  static struct trb cur;
  if (defer_n) {
    cur.p0 = defer_q[0].p0;
    cur.p1 = defer_q[0].p1;
    cur.status = defer_q[0].status;
    cur.control = defer_q[0].control;
    defer_n--;
    for (uint32_t i = 0; i < defer_n; i++)
      defer_q[i] = defer_q[i + 1];
    return &cur;
  }
  struct trb *e;
  while ((e = event_next()))
    if (!event_consumed(e))
      return e;
  return NULL;
}

/* Slot ID from the last command completion (Enable Slot returns the new one). */
static int last_cmd_slot;

/* Ring the command doorbell (DB[0], target 0) and wait for its Command
 * Completion Event. Returns the completion code, or 0 on timeout. */
static int cmd_submit(void)
{
  db[0] = 0;
  struct trb *e = event_wait(TRB_CMD_COMPLETION, 0, 0);
  if (!e)
    return 0;
  last_cmd_slot = (e->control >> 24) & 0xFF;
  return (e->status >> 24) & 0xFF; /* completion code (slot id in p1)   */
}

/* --------------------------------------------------------------- bring-up */

/* Find an xHCI controller and map its MMIO BAR (a 64-bit memory BAR). Returns
 * the virtual base, or NULL. */
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

        /* Size BAR0 (a memory BAR) with memory decode off so the
         * all-ones probe write doesn't briefly alias live MMIO: write
         * 0xFFFFFFFF, read the size mask, restore the original. Real
         * Intel xHCI register files are 64 KiB with the extended caps
         * (Legacy Support) far out at +0x8000 - mapping only a few pages
         * page-faults the instant we walk them. */
        uint32_t cmd = pci_read32(bus, slot, f, 0x04);
        pci_write32(bus, slot, f, 0x04, cmd & ~0x2u); /* mem off       */
        uint32_t bar0 = pci_read32(bus, slot, f, 0x10);
        pci_write32(bus, slot, f, 0x10, 0xFFFFFFFFu);
        uint32_t mask = pci_read32(bus, slot, f, 0x10) & ~0xFu;
        pci_write32(bus, slot, f, 0x10, bar0);       /* restore       */
        pci_write32(bus, slot, f, 0x04, cmd | 0x06); /* mem + master  */

        uint32_t phys = bar0 & ~0xFu; /* low 32 bits are enough <4 GiB */
        uint32_t size = ~mask + 1;
        if (size < 0x10000 || size > 0x100000) /* 64 KiB..1 MiB */
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

/* Take the controller from the firmware. On real hardware the BIOS owns the
 * xHCI via the "USB Legacy Support" extended capability: we set the OS-owned
 * semaphore and wait for the BIOS-owned bit to clear, then disable SMIs so the
 * firmware stops servicing it. QEMU exposes no such capability (the walk just
 * finds nothing), so this is a no-op there. */
static void xhci_takeover(void)
{
  uint32_t xecp = (r32(cap, XHCI_HCCPARAMS1) >> 16) & 0xFFFF;
  if (!xecp)
    return;
  volatile uint8_t *p = cap + xecp * 4; /* xECP is in dword units        */
  for (int guard = 0; guard < 64; guard++) {
    uint32_t capreg = *(volatile uint32_t *)p;
    uint8_t id = capreg & 0xFF;
    uint8_t next = (capreg >> 8) & 0xFF;
    if (id == 1) {                                   /* USB Legacy Support             */
      *(volatile uint32_t *)p = capreg | (1u << 24); /* OS owned        */
      for (int ms = 0; ms < 200; ms++) {
        if (!(*(volatile uint32_t *)p & (1u << 16))) /* BIOS released   */
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

/* Power every root-hub port and give the ports time to report a connection.
 * Real controllers (with Port Power Control) bring ports up unpowered after a
 * reset, so CCS reads 0 until we set PP; QEMU powers them for us. */
static void power_all_ports(void)
{
  for (uint32_t p = 1; p <= max_ports; p++) {
    uint32_t sc = r32(op, XHCI_PORTSC(p));
    if (!(sc & PORT_PP))
      w32(op, XHCI_PORTSC(p), (sc & ~PORT_RWC_MASK & ~PORT_PED) | PORT_PP);
  }
  timer_sleep(20); /* PP -> stable connect status (spec: ~20 ms)    */
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
  uint32_t n = ((p2 >> 27) & 0x1F) | (((p2 >> 21) & 0x1F) << 5); /* hi|lo */
  if (!n)
    return;
  uint64_t *arr = palloc(); /* array of scratchpad page pointers */
  for (uint32_t i = 0; i < n && i < 512; i++)
    arr[i] = (uint64_t)(uintptr_t)palloc();
  dcbaa[0] = (uint64_t)(uintptr_t)arr; /* DCBAA slot 0 = scratchpad array   */
}

/* Reset one connected port and return its speed id, or 0 if it didn't enable.
 */
static int reset_port(uint32_t p)
{
  uint32_t sc = r32(op, XHCI_PORTSC(p));
  if (!(sc & PORT_CCS))
    return 0;

  /* Drive a port reset (clear pending change bits, do not write 1 to PED). */
  w32(op, XHCI_PORTSC(p), (sc & ~PORT_RWC_MASK & ~PORT_PED) | PORT_PR);
  for (int ms = 0; ms < 500; ms++) {
    sc = r32(op, XHCI_PORTSC(p));
    if (sc & PORT_PRC) /* reset complete                  */
      break;
    timer_sleep(1);
  }
  w32(op, XHCI_PORTSC(p), (sc & ~PORT_RWC_MASK & ~PORT_PED) | PORT_PRC);
  sc = r32(op, XHCI_PORTSC(p));
  return (sc & PORT_PED) ? PORT_SPEED(sc) : 0;
}

/* Build the input context (control + slot + EP0) and the output context in DCBAA[slot]; 0 on success. */
static int address_device(struct xdev *x)
{
  int slot_id = x->pub.slot, speed = x->pub.speed;
  uint8_t *input = palloc();  /* input control(0) + slot(1) + EP0(2)    */
  uint8_t *output = palloc(); /* device context for the controller      */
  ring_init(&x->ep0);
  if (!input || !output || !x->ep0.base) {
    if (input)
      pfree(input);
    if (output)
      pfree(output);
    if (x->ep0.base)
      pfree(x->ep0.base);
    x->ep0.base = 0;
    return -1;
  }

  uint32_t *icc = (uint32_t *)(input);               /* input control ctx */
  uint32_t *sctx = (uint32_t *)(input + ctx_sz * 1); /* slot context      */
  uint32_t *ep0 = (uint32_t *)(input + ctx_sz * 2);  /* EP0 context       */

  icc[1] = 0x3; /* add slot context (A0) + EP0 (A1)        */

  /* Slot: route string, speed, one context entry, root-hub port; low/full-speed devices behind a high-speed hub name
   * its transaction translator. */
  sctx[0] = x->pub.route | ((uint32_t)speed << 20) | (1u << 27);
  sctx[1] = (uint32_t)x->pub.root_port << 16;
  if (x->pub.tt_slot)
    sctx[2] = (uint32_t)x->pub.tt_slot | ((uint32_t)x->pub.tt_port << 8);

  /* EP0: control endpoint, default max packet by speed, its transfer ring. */
  uint32_t mps = (speed >= 4) ? 512 : (speed == 3 ? 64 : 8); /* SS/HS/FS-LS  */
  ep0[1] = (4u << 3) | (mps << 16);
  ep0[2] = (uint32_t)(uintptr_t)x->ep0.base | 1;
  ep0[3] = 0;

  /* Re-enumerating a slot: release the previous device context first, or it
   * leaks a page every hot-plug cycle until the palloc pool is exhausted. */
  if (dcbaa[slot_id])
    pfree((void *)(uintptr_t)dcbaa[slot_id]);
  dcbaa[slot_id] = (uint64_t)(uintptr_t)output;

  ring_push(&cmd_ring, (uint32_t)(uintptr_t)input, 0, 0, TRB_TYPE(TRB_ADDRESS_DEVICE) | ((uint32_t)slot_id << 24));
  int cc = cmd_submit();
  pfree(input); /* transient: the controller has consumed it */
  if (cc != CC_SUCCESS) {
    con_printf("xhci: address-device failed cc=%u\n", cc);
    dcbaa[slot_id] = 0;
    pfree(output);
    pfree(x->ep0.base);
    x->ep0.base = 0;
    return -1;
  }
  return 0;
}

/* Update EP0's max packet size with an Evaluate Context command, once the real
 * bMaxPacketSize0 is known. The spec evaluates only the max-packet-size field
 * for the default control endpoint, so the TR dequeue pointer the controller
 * already tracks is left untouched. Returns 0 on success. */
static int eval_context_mps(int slot_id, uint32_t mps)
{
  uint8_t *input = palloc();
  if (!input)
    return -1;
  uint32_t *icc = (uint32_t *)input;
  uint32_t *ep0 = (uint32_t *)(input + ctx_sz * 2);

  icc[1] = 0x2;                     /* add EP0 (A1) only                  */
  ep0[1] = (4u << 3) | (mps << 16); /* control EP, new max packet size    */

  ring_push(&cmd_ring, (uint32_t)(uintptr_t)input, 0, 0, TRB_TYPE(TRB_EVAL_CONTEXT) | ((uint32_t)slot_id << 24));
  int cc = cmd_submit();
  pfree(input); /* transient: consumed by the command */
  return cc == CC_SUCCESS ? 0 : -1;
}

static struct ring *ep0_of(int slot) { return &devs[slot]->ep0; }

/* Recover a halted endpoint: Reset Endpoint, then point the dequeue at the ring's enqueue position so the stale TDs
 * are skipped. */
static void ep_reset(int slot, int dci, struct ring *r)
{
  ring_push(&cmd_ring, 0, 0, 0, TRB_TYPE(TRB_RESET_EP) | ((uint32_t)dci << 16) | ((uint32_t)slot << 24));
  cmd_submit();
  uint32_t deq = (uint32_t)(uintptr_t)&r->base[r->enq] | r->cycle;
  ring_push(&cmd_ring, deq, 0, 0, TRB_TYPE(TRB_SET_TRDQ) | ((uint32_t)dci << 16) | ((uint32_t)slot << 24));
  cmd_submit();
}

/* Wait for the status-stage event of an EP0 transfer; a halted pipe is reset. Returns the completion code (0 on
 * timeout). */
static int control_finish(int slot, uint32_t *status)
{
  struct trb *e = event_wait(TRB_TRANSFER_EVENT, slot, 1);
  if (!e)
    return 0;
  int cc = (e->status >> 24) & 0xFF;
  if (status)
    *status = e->status;
  if (cc != CC_SUCCESS && cc != CC_SHORT_PKT && cc != CC_STOPPED)
    ep_reset(slot, 1, ep0_of(slot));
  return cc;
}

/* Control-IN over EP0: SETUP -> DATA IN -> STATUS OUT; len must fit in one page; returns len or -1. */
int usbh_control_in(int slot_id, uint8_t bmReqType, uint8_t bReq, uint16_t wValue, uint16_t wIndex, uint8_t *out,
                    int len)
{
  uint8_t *buf = palloc();
  if (!buf) /* palloc pool exhausted: fail gracefully */
    return -1;
  struct ring *ep0 = ep0_of(slot_id);
  uint32_t setup_lo = bmReqType | ((uint32_t)bReq << 8) | ((uint32_t)wValue << 16);
  uint32_t setup_hi = (uint32_t)wIndex | ((uint32_t)len << 16);
  ring_push(ep0, setup_lo, setup_hi, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | (3u << 16));
  ring_push(ep0, (uint32_t)(uintptr_t)buf, 0, len, TRB_TYPE(TRB_DATA) | (1u << 16));
  ring_push(ep0, 0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC);
  db[slot_id] = 1;

  int cc = control_finish(slot_id, 0);
  int rc = -1;
  if (cc == CC_SUCCESS || cc == CC_SHORT_PKT) {
    for (int i = 0; i < len; i++)
      out[i] = buf[i];
    rc = len;
  }
  pfree(buf);
  return rc;
}

/* Control transfer with no data stage (SET_CONFIGURATION etc.): SETUP -> STATUS IN. */
int usbh_control_nodata(int slot_id, uint8_t bmReqType, uint8_t bReq, uint16_t wValue, uint16_t wIndex)
{
  struct ring *ep0 = ep0_of(slot_id);
  uint32_t setup_lo = bmReqType | ((uint32_t)bReq << 8) | ((uint32_t)wValue << 16);
  ring_push(ep0, setup_lo, (uint32_t)wIndex, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT);
  ring_push(ep0, 0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC | (1u << 16));
  db[slot_id] = 1;
  return control_finish(slot_id, 0) == CC_SUCCESS ? 0 : -1;
}

/* Control transfer with an OUT data stage (e.g. RNDIS SEND_ENCAPSULATED_COMMAND): SETUP -> DATA OUT -> STATUS IN. */
int usbh_control_out(int slot_id, uint8_t bmReqType, uint8_t bReq, uint16_t wValue, uint16_t wIndex,
                     const uint8_t *data, int len)
{
  uint8_t *buf = palloc();
  if (!buf) /* palloc pool exhausted: fail gracefully */
    return -1;
  for (int i = 0; i < len; i++)
    buf[i] = data[i];

  struct ring *ep0 = ep0_of(slot_id);
  uint32_t setup_lo = bmReqType | ((uint32_t)bReq << 8) | ((uint32_t)wValue << 16);
  uint32_t setup_hi = (uint32_t)wIndex | ((uint32_t)len << 16);
  ring_push(ep0, setup_lo, setup_hi, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | (2u << 16));
  ring_push(ep0, (uint32_t)(uintptr_t)buf, 0, len, TRB_TYPE(TRB_DATA));
  ring_push(ep0, 0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC | (1u << 16));
  db[slot_id] = 1;

  int rc = control_finish(slot_id, 0) == CC_SUCCESS ? 0 : -1;
  pfree(buf);
  return rc;
}

/* ----------------------------------------------------------------- RNDIS */
/* RNDIS is the Microsoft "Ethernet over USB" protocol Android uses for USB
 * tethering. Control messages ride EP0 as CDC encapsulated commands/responses;
 * Ethernet frames (later) ride the bulk pipes wrapped in packet headers. This
 * does the bring-up handshake: INITIALIZE, then QUERY the permanent MAC. */

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
  if (usbh_control_out(slot, 0x21, 0x00, 0, iface, msg, mlen) != 0)
    return -1;
  return usbh_control_in(slot, 0xA1, 0x01, 0, iface, resp, rlen);
}

/* INITIALIZE the RNDIS device and query its MAC into d->mac. Returns 0 on
 * success. */
static int rndis_setup(struct usbdev *d)
{
  uint8_t msg[32], resp[64];

  for (int i = 0; i < 32; i++)
    msg[i] = 0;
  put_le32(msg + 0, RNDIS_INITIALIZE_MSG);
  put_le32(msg + 4, 24);      /* message length                       */
  put_le32(msg + 8, 1);       /* request id                           */
  put_le32(msg + 12, 1);      /* RNDIS major version                  */
  put_le32(msg + 16, 0);      /* minor version                        */
  put_le32(msg + 20, 0x4000); /* max transfer size we'll accept       */
  /* INITIALIZE_CMPLT layout: type(0) len(4) reqid(8) status(12) ... */
  if (rndis_cmd(d->slot, d->ctrl_iface, msg, 24, resp, sizeof resp) < 16)
    return -1;
  if (load_le32(resp + 12) != 0) /* INITIALIZE_CMPLT status              */
    return -1;

  for (int i = 0; i < 32; i++)
    msg[i] = 0;
  put_le32(msg + 0, RNDIS_QUERY_MSG);
  put_le32(msg + 4, 28);
  put_le32(msg + 8, 2);
  put_le32(msg + 12, OID_802_3_PERM_ADDR);
  put_le32(msg + 16, 0); /* info buffer length                   */
  put_le32(msg + 20, 0); /* info buffer offset                   */
  put_le32(msg + 24, 0); /* reserved                             */
  /* QUERY_CMPLT layout: type(0) len(4) reqid(8) status(12) infolen(16)
   * infooffset(20) [data]; the offset is measured from byte 8 (reqid). */
  int n = rndis_cmd(d->slot, d->ctrl_iface, msg, 28, resp, sizeof resp);
  if (n < 24 || load_le32(resp + 12) != 0) /* QUERY_CMPLT status */
    return -1;
  /* InfoBufferOffset is device-controlled and measured from byte 8. Compute in
   * 64-bit and compare unsigned so a huge offset can't go negative under the
   * signed check and read past the response buffer. */
  uint64_t off = (uint64_t)8 + load_le32(resp + 20);
  if (off + 6 > (uint32_t)n)
    return -1;
  for (int i = 0; i < 6; i++)
    d->mac[i] = resp[off + i];
  d->mac_ok = 1;
  return 0;
}

/* Scan the cached config descriptor for a USB-Ethernet shape: a bulk IN + bulk
 * OUT pair (the data pipes) and an interrupt IN on the comm interface (the
 * RNDIS notification). Works for both the textbook CDC form (02/02/ff + 0a) and
 * the vendor-class single-interface form some phones use. */
static void parse_net(struct usbdev *d)
{
  d->is_net = 0;
  int cur_iface = 0, intr_iface = -1;
  for (int i = 0; i + 2 <= d->cfg_len;) {
    uint8_t blen = d->cfg[i];
    if (blen < 2)
      break;
    uint8_t btype = d->cfg[i + 1];
    if (btype == 0x04 && i + 9 <= d->cfg_len) { /* interface         */
      cur_iface = d->cfg[i + 2];
    } else if (btype == 0x05 && i + 7 <= d->cfg_len) { /* endpoint          */
      uint8_t addr = d->cfg[i + 2];
      uint8_t attr = d->cfg[i + 3] & 0x3;
      uint16_t mps = (d->cfg[i + 4] | (d->cfg[i + 5] << 8)) & 0x7FF;
      if (attr == 2) { /* bulk              */
        if (addr & 0x80) {
          d->bulk_in = addr;
          d->bulk_in_mps = mps ? mps : 64;
        } else {
          d->bulk_out = addr;
          d->bulk_out_mps = mps ? mps : 64;
        }
      } else if (attr == 3 && (addr & 0x80)) { /* interrupt IN      */
        d->intr_in = addr;
        d->intr_in_mps = mps ? mps : 16;
        d->intr_interval = d->cfg[i + 6]; /* bInterval         */
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

/* Tell the RNDIS device which packets to deliver to us (directed to our MAC +
 * broadcast). Without this the device drops everything and RX is silent. */
static int rndis_set_filter(struct usbdev *d)
{
  uint8_t msg[64], resp[64];
  for (int i = 0; i < 64; i++)
    msg[i] = 0;
  put_le32(msg + 0, RNDIS_SET_MSG);
  put_le32(msg + 4, 28 + 4); /* header + 4-byte info buffer          */
  put_le32(msg + 8, 3);      /* request id                           */
  put_le32(msg + 12, OID_GEN_PACKET_FILTER);
  put_le32(msg + 16, 4);          /* info buffer length                   */
  put_le32(msg + 20, 20);         /* info buffer offset (from byte 8)      */
  put_le32(msg + 24, 0);          /* reserved                             */
  put_le32(msg + 28, 0x0000000D); /* DIRECTED | MULTICAST | BROADCAST     */
  if (rndis_cmd(d->slot, d->ctrl_iface, msg, 32, resp, sizeof resp) < 16)
    return -1;
  return load_le32(resp + 12) == 0 ? 0 : -1; /* SET_CMPLT status */
}

/* xHCI Device Context Index for an endpoint address: EP0 is 1; others are
 * ep_num*2 (+1 for IN). */
static int ep_dci(uint8_t addr) { return ((addr & 0x0F) * 2) + ((addr & 0x80) ? 1 : 0); }

/* Context Entries only grow: an input slot context copied from the live one must keep earlier endpoints. */
static void slot_set_entries(uint32_t *sctx, int dci)
{
  if (dci > (int)(sctx[0] >> 27))
    sctx[0] = (sctx[0] & ~(0x1Fu << 27)) | ((uint32_t)dci << 27);
}

/* Add the bulk IN/OUT endpoints to the device with a Configure Endpoint
 * command, each backed by its own transfer ring. Returns 0 on success. */
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

  /* Configure Endpoint's input slot context must be a COPY of the controller's
   * current (output) slot context with only Context Entries changed
   * (xHCI 4.6.6). Rebuilding it fresh would zero sctx[3] - the USB device
   * address Address Device assigned - and real Intel xHCI then addresses the
   * device as 0, so every bulk transfer comes back cc=4 (USB Transaction
   * Error). QEMU ignores the fields it doesn't need and hides this. Copy the
   * live context, then bump Context Entries (DWORD0 bits 27-31) to cover the
   * new endpoints; speed/port/address survive. */
  (void)port;
  uint32_t *out_slot = (uint32_t *)(uintptr_t)dcbaa[d->slot];
  for (uint32_t i = 0; i < ctx_sz / 4; i++)
    sctx[i] = out_slot[i];
  slot_set_entries(sctx, max_dci);

  /* EP type: bulk OUT = 2, bulk IN = 6, interrupt IN = 7. CErr=3. */
  epin[1] = (3u << 1) | (6u << 3) | ((uint32_t)d->bulk_in_mps << 16);
  epin[2] = (uint32_t)(uintptr_t)net_in_ring.base | 1; /* TR dequeue | DCS      */
  epin[4] = d->bulk_in_mps;                            /* avg TRB length        */
  epout[1] = (3u << 1) | (2u << 3) | ((uint32_t)d->bulk_out_mps << 16);
  epout[2] = (uint32_t)(uintptr_t)net_out_ring.base | 1;
  epout[4] = d->bulk_out_mps;
  if (intr_dci) {
    uint32_t *epi = (uint32_t *)(input + ctx_sz * (1 + intr_dci));
    uint32_t iv = d->intr_interval ? d->intr_interval - 1 : 6;
    epi[0] = iv << 16; /* Interval              */
    epi[1] = (3u << 1) | (7u << 3) | ((uint32_t)d->intr_in_mps << 16);
    epi[2] = (uint32_t)(uintptr_t)net_intr_ring.base | 1;
    epi[4] = d->intr_in_mps;
  }

  ring_push(&cmd_ring, (uint32_t)(uintptr_t)input, 0, 0, TRB_TYPE(TRB_CONFIG_EP) | ((uint32_t)d->slot << 24));
  int cc = cmd_submit();
  pfree(input);           /* transient: consumed by the command */
  if (cc != CC_SUCCESS) { /* config failed: free the transfer rings */
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

/* ------------------------------------------------------ RNDIS data framing */
/* Each Ethernet frame rides the bulk pipe wrapped in a 44-byte RNDIS packet
 * message: type=1, total length, then the data offset/length and zeroed
 * out-of-band/per-packet-info fields. */
#define RNDIS_PACKET_MSG 0x00000001
#define RNDIS_HDR 44

/* nic_ops.send: queue a frame for the USB thread to transmit (non-blocking). */
static void rndis_send(const uint8_t *frame, uint32_t len)
{
  if (!net_up || len > TXQ_FRAME)
    return;
  __asm__ volatile("cli");
  int next = (txq_head + 1) % TXQ_SLOTS;
  if (next != txq_tail) { /* drop if the queue is full            */
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

/* Post an interrupt-IN request to receive RNDIS status notifications (e.g.
 * "response available", "media connect"). Servicing this is what tells an
 * Android gadget the host is a real RNDIS peer so it starts the data path. */
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
  put_le32(net_tx_buf + 4, RNDIS_HDR + len); /* total message length        */
  put_le32(net_tx_buf + 8, RNDIS_HDR - 8);   /* data offset (from byte 8)   */
  put_le32(net_tx_buf + 12, len);            /* data length                 */
  for (int i = 0; i < len; i++)
    net_tx_buf[RNDIS_HDR + i] = frame[i];
  ring_push(&net_out_ring, (uint32_t)(uintptr_t)net_tx_buf, 0, RNDIS_HDR + len, TRB_TYPE(TRB_NORMAL) | TRB_IOC);
  db[net_slot] = net_out_dci;
  net_out_pending = 1;
}

/* Bring up the RNDIS data path for a freshly enumerated, initialized device:
 * set the packet filter, configure the bulk endpoints, allocate buffers, and
 * register as the active NIC. Returns 0 on success. */
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

  nic_set(&rndis_ops);
  net_up = 1;
  /* NB: the bulk/interrupt INs are armed in data_loop, NOT here - arming them
   * now would post transfer events onto the shared event ring while the boot
   * scan is still enumerating other devices, corrupting their command waits. */
  net_dhcp_spawn(); /* lease an IP from the phone/router    */
  return 0;
}

/* Recover a halted bulk-IN endpoint after a STALL: reset it on the controller,
 * clear the device-side halt over EP0, point the controller back at our ring,
 * then re-arm a bulk-IN. Bounded so a device that stalls forever doesn't spin.
 * Safe to do control/command transfers here: in data mode the USB thread owns
 * the event ring and no enumeration touches the global ep0_ring (still this
 * device's). */
static void recover_in_stall(void)
{
  if (net_stall_retries++ > 16)
    return; /* give up; DHCP will time out  */
  if (net_stall_retries <= 2)
    con_write("usbnet: recovering bulk-in\n");

  ring_push(&cmd_ring, 0, 0, 0, TRB_TYPE(TRB_RESET_EP) | ((uint32_t)net_in_dci << 16) | ((uint32_t)net_slot << 24));
  cmd_submit();
  usbh_control_nodata(net_slot, 0x02, 0x01, 0, net_in_ep);
  uint32_t deq = (uint32_t)(uintptr_t)&net_in_ring.base[net_in_ring.enq] | net_in_ring.cycle;
  ring_push(&cmd_ring, deq, 0, 0, TRB_TYPE(TRB_SET_TRDQ) | ((uint32_t)net_in_dci << 16) | ((uint32_t)net_slot << 24));
  cmd_submit();

  net_in_pending = 0;
  post_bulk_in();
}

static void usb_service(void);

/* The bulk TX/RX loop, run by the USB thread once the data path is up. Keeps
 * one bulk-IN outstanding; drains the TX queue onto bulk-OUT one frame at a
 * time; routes completions by endpoint id. */
static void data_loop(void)
{
  post_bulk_in(); /* arm RX + status here, after enumeration  */
  post_bulk_intr();
  for (;;) {
    /* TX: post one queued frame if the OUT ring is free. */
    if (!net_out_pending && txq_tail != txq_head) {
      post_bulk_out(txq[txq_tail], txq_len[txq_tail]);
      txq_tail = (txq_tail + 1) % TXQ_SLOTS;
    }
    if (!net_intr_pending) /* keep the status-IN armed            */
      post_bulk_intr();
    usb_service();
    bool resp_avail = 0;
    /* Drain completion events, routing by endpoint. */
    struct trb *e;
    while ((e = event_poll())) {
      if (TRB_GET_TYPE(e->control) != TRB_TRANSFER_EVENT)
        continue;
      if ((int)((e->control >> 24) & 0xFF) != net_slot)
        continue;
      int epid = (e->control >> 16) & 0x1F;
      int cc = (e->status >> 24) & 0xFF;
      if (epid == net_in_dci) {
        int residual = e->status & 0xFFFFFF;
        int got = 2048 - residual;
        /* DIAG: report the first few inbound transfers (cc, length,
         * RNDIS message type) so a stuck phone path is debuggable. */
        if (dbg_rx < 4) {
          dbg_rx++;
          con_printf("usbnet: rx cc=%u len=%u type=%08x\n", cc, got, load_le32(net_rx_buf));
        }
        if (cc == CC_STALL || cc == CC_BABBLE || cc == CC_USB_TXN_ERR) {
          /* These halt the endpoint - reset it and retry rather than
           * going permanently deaf. */
          recover_in_stall();
        } else {
          /* Unwrap the RNDIS packet message -> Ethernet frame. */
          if (got >= RNDIS_HDR && load_le32(net_rx_buf) == RNDIS_PACKET_MSG) {
            uint32_t off = load_le32(net_rx_buf + 8);
            uint32_t dl = load_le32(net_rx_buf + 12);
            /* off/dl come from the device: use 64-bit math so the frame
             * start (8 + off) and end can't wrap past net_rx_buf. */
            uint64_t start = (uint64_t)8 + off;
            uint64_t g = (uint32_t)got;
            if (start <= g && dl <= g - start)
              net_rx_frame(net_rx_buf + start, dl);
          }
          net_stall_retries = 0; /* a good transfer resets it    */
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
          resp_avail = 1; /* a notification is waiting          */
      }
    }
    /* A status notification means an RNDIS message is queued - read it (the
     * event ring is now drained, so this control transfer's completion comes
     * back cleanly). Draining it is what unblocks an Android gadget. */
    if (resp_avail) {
      uint8_t r[64];
      usbh_control_in(net_slot, 0xA1, 0x01, 0, net_ctrl_iface, r, sizeof r);
    }
    timer_sleep(1);
  }
}

/* ============================================================ USB Mass Storage
 *
 * A USB flash drive / SD reader speaks the Bulk-Only Transport (BOT) protocol
 * over a bulk IN + bulk OUT endpoint pair: a 31-byte Command Block Wrapper
 * (CBW) carrying a SCSI CDB, an optional data stage, then a 13-byte Command
 * Status Wrapper (CSW). We implement just enough SCSI (TEST UNIT READY,
 * INQUIRY, READ CAPACITY(10), READ(10), WRITE(10)) to expose the device as a
 * 512-byte block device for the FAT driver to mount/eject.
 *
 * Concurrency: BOT is strictly synchronous (one CBW->data->CSW at a time) and
 * the xHCI rings are driven from a single context -- the usb-poll thread. So
 * sector I/O isn't done in the caller's thread; the caller drops a request in a
 * one-slot mailbox and yields, and the poll thread runs the BOT exchange. The
 * capacity/inquiry probe runs synchronously at boot, while the poll thread does
 * not yet exist and enumeration owns the rings.
 *
 * Because the poll thread runs data_loop() forever once a tether is up, a live
 * RNDIS tether and a mass-storage device are mutually exclusive after boot --
 * an acceptable limit (you don't usually tether and mount a stick at once). */

static bool msc_ready; /* device configured + capacity known       */
static int msc_slot;
static int msc_in_dci, msc_out_dci;
static uint8_t msc_in_ep, msc_out_ep;
static uint16_t msc_in_mps, msc_out_mps;
static struct ring msc_in_ring, msc_out_ring;
static uint32_t msc_capacity; /* size in 512-byte sectors                  */
static uint32_t msc_tag;      /* CBW tag counter                           */
static char msc_model[40];
static uint8_t *msc_cbw, *msc_csw, *msc_data; /* DMA scratch (palloc'd once) */

/* One-slot request mailbox serviced by the poll thread. */
static volatile bool msc_req, msc_done, msc_rw, msc_busy;
static volatile int msc_result;
static volatile int msc_mb_count; /* sectors in this request            */
static volatile uint32_t msc_mb_lba;
static volatile uint8_t *msc_mb_buf;

/* Read cache: USB BOT transactions are slow (one CBW/data/CSW round-trip each,
 * plus a scheduler hop through the mailbox), and metadata sectors -- the FAT
 * table, an HFS+ catalog node -- get re-read constantly while listing a
 * directory. So reads go through a small set of MSC_CHUNK-sector lines: a miss
 * fetches a whole line in ONE multi-sector READ(10); hits are served from RAM
 * caller-side, skipping the mailbox entirely. Writes are write-through and
 * invalidate the covering line. Only the lock holder touches the cache. */
#define MSC_CHUNK 8  /* sectors per line (4 KiB == msc_data)*/
#define MSC_LINES 64 /* 64 * 4 KiB = 256 KiB cache          */
#define MSC_LINE_BYTES (MSC_CHUNK * 512)
static uint8_t *msc_cache;           /* MSC_LINES * MSC_LINE_BYTES, or NULL */
static uint32_t msc_ctag[MSC_LINES]; /* line's aligned base LBA (~0 = empty)*/
static int msc_victim;               /* round-robin replacement cursor      */

/* Find a Mass Storage interface (class 0x08, SCSI transparent 0x06, Bulk-Only
 * 0x50) in the cached config descriptor and grab its bulk IN/OUT endpoints. */
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
    if (t == 0x04 && i + 7 < len) { /* INTERFACE */
      in_iface = (p[i + 5] == 0x08 && p[i + 6] == 0x06 && p[i + 7] == 0x50);
    } else if (t == 0x05 && in_iface && i + 5 < len) { /* ENDPOINT */
      uint8_t addr = p[i + 2], attr = p[i + 3];
      uint16_t mps = p[i + 4] | (p[i + 5] << 8);
      if ((attr & 0x03) == 0x02) { /* bulk */
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

/* Configure Endpoint for the bulk pair (same input-context discipline as the
 * net path: copy the live slot context, only bump Context Entries). */
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
  slot_set_entries(sctx, max_dci);

  epin[1] = (3u << 1) | (6u << 3) | ((uint32_t)d->bulk_in_mps << 16); /* bulk IN  */
  epin[2] = (uint32_t)(uintptr_t)msc_in_ring.base | 1;
  epin[4] = d->bulk_in_mps;
  epout[1] = (3u << 1) | (2u << 3) | ((uint32_t)d->bulk_out_mps << 16); /* bulk OUT */
  epout[2] = (uint32_t)(uintptr_t)msc_out_ring.base | 1;
  epout[4] = d->bulk_out_mps;

  ring_push(&cmd_ring, (uint32_t)(uintptr_t)input, 0, 0, TRB_TYPE(TRB_CONFIG_EP) | ((uint32_t)d->slot << 24));
  int cc = cmd_submit();
  pfree(input);           /* transient: consumed by the command */
  if (cc != CC_SUCCESS) { /* config failed: free the transfer rings */
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

/* One bulk transfer: post a Normal TRB, ring the doorbell, wait for completion.
 * Returns bytes transferred (len - residue), or -1; *cc gets the completion
 * code. */
static int bulk_xfer(int slot, struct ring *ring, int dci, uint8_t *buf, int len, int *cc)
{
  ring_push(ring, (uint32_t)(uintptr_t)buf, 0, len, TRB_TYPE(TRB_NORMAL) | TRB_IOC);
  db[slot] = dci;
  struct trb *e = event_wait(TRB_TRANSFER_EVENT, slot, dci);
  if (!e) {
    *cc = 0;
    return -1;
  }
  *cc = (e->status >> 24) & 0xFF;
  if (*cc == CC_SUCCESS || *cc == CC_SHORT_PKT)
    return len - (int)(e->status & 0xFFFFFF); /* residue */
  return -1;
}

/* Clear a STALL on a bulk endpoint: reset it, CLEAR_FEATURE(HALT) over EP0,
 * then point the controller back at our ring. Runs in the poll-thread context,
 * so it borrows the global EP0 ring for the control transfer and restores it.
 */
static void msc_clear_stall(int dci, uint8_t ep_addr, struct ring *ring)
{
  ring_push(&cmd_ring, 0, 0, 0, TRB_TYPE(TRB_RESET_EP) | ((uint32_t)dci << 16) | ((uint32_t)msc_slot << 24));
  cmd_submit();

  usbh_control_nodata(msc_slot, 0x02, 0x01, 0, ep_addr);

  uint32_t deq = (uint32_t)(uintptr_t)&ring->base[ring->enq] | ring->cycle;
  ring_push(&cmd_ring, deq, 0, 0, TRB_TYPE(TRB_SET_TRDQ) | ((uint32_t)dci << 16) | ((uint32_t)msc_slot << 24));
  cmd_submit();
}

/* Run one BOT command: CBW (with the SCSI CDB) -> optional data -> CSW. For a
 * read, the data lands in `data`; for a write, `data` is sent. Returns 0 iff
 * the CSW reports success. */
static int bot_xfer(const uint8_t *cdb, int cdb_len, int dlen, bool write, uint8_t *data)
{
  /* --- Command Block Wrapper --- */
  for (int i = 0; i < 31; i++)
    msc_cbw[i] = 0;
  msc_cbw[0] = 'U';
  msc_cbw[1] = 'S';
  msc_cbw[2] = 'B';
  msc_cbw[3] = 'C';
  uint32_t tag = ++msc_tag;
  put_le32(msc_cbw + 4, tag);
  put_le32(msc_cbw + 8, (uint32_t)dlen);
  msc_cbw[12] = write ? 0x00 : 0x80; /* bmCBWFlags: 0x80 = data IN */
  msc_cbw[13] = 0;                   /* LUN 0 */
  msc_cbw[14] = (uint8_t)cdb_len;
  for (int i = 0; i < 16 && i < cdb_len; i++)
    msc_cbw[15 + i] = cdb[i];

  int cc;
  if (bulk_xfer(msc_slot, &msc_out_ring, msc_out_dci, msc_cbw, 31, &cc) < 0) {
    if (cc == CC_STALL)
      msc_clear_stall(msc_out_dci, msc_out_ep, &msc_out_ring);
    return -1;
  }

  /* --- Data stage --- */
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

  /* --- Command Status Wrapper --- */
  if (bulk_xfer(msc_slot, &msc_in_ring, msc_in_dci, msc_csw, 13, &cc) < 0) {
    if (cc != CC_STALL)
      return -1;
    msc_clear_stall(msc_in_dci, msc_in_ep, &msc_in_ring);
    if (bulk_xfer(msc_slot, &msc_in_ring, msc_in_dci, msc_csw, 13, &cc) < 0)
      return -1;
  }
  if (msc_csw[0] != 'U' || msc_csw[1] != 'S' || msc_csw[2] != 'B' || msc_csw[3] != 'S')
    return -1;
  if (load_le32(msc_csw + 4) != tag || msc_csw[12] != 0) /* tag mismatch / failed */
    return -1;

  if (dlen > 0 && !write)
    for (int i = 0; i < dlen; i++)
      data[i] = msc_data[i];
  return 0;
}

static int scsi_tur(void)
{
  uint8_t cdb[6] = {0}; /* TEST UNIT READY */
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

/* Configure the bulk pipes and probe the medium. Runs at boot enumeration time
 * (single ring context), so it uses the synchronous transfers directly. */
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
  for (int i = 0; i < MSC_LINES; i++) /* fresh medium: drop stale lines */
    msc_ctag[i] = 0xFFFFFFFFu;
  msc_victim = 0;
  msc_tag = 0;

  /* A freshly attached stick often reports "not ready" once; clear it with a
   * couple of TEST UNIT READY / REQUEST SENSE rounds before reading capacity.
   */
  for (int i = 0; i < 4; i++) {
    if (scsi_tur() == 0)
      break;
    uint8_t sense[18];
    scsi_request_sense(sense);
    timer_sleep(50);
  }

  uint8_t inq[36];
  msc_model[0] = '\0';
  if (scsi_inquiry(inq) == 0) { /* vendor[8..15] + product[16..31] */
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

/* Serialize MSC ops -- and the cache -- across caller threads. Every
 * sched_yield is preceded by `sti`: context_switch does NOT save/restore
 * EFLAGS, so yielding with interrupts off poisons the next-resumed thread (it
 * inherits IF=0 and its next `hlt` halts the CPU forever). The trailing sti in
 * msc_unlock guarantees we leave with interrupts on regardless of how we were
 * last resumed. */
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
    if (line < 0) { /* miss: fetch a whole line */
      line = msc_victim;
      msc_victim = (msc_victim + 1) % MSC_LINES;
      msc_ctag[line] = 0xFFFFFFFFu; /* invalid while filling */
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
    rc = msc_io(lba, buf, 1, false); /* no cache (alloc failed) */
  }

  msc_unlock();
  /* If the controller went silent on this op, drop the device: it was unplugged
   * mid-transfer or has wedged. Further reads fail instantly (msc_ready == 0)
   * and it disappears from the Devices list, rather than re-hanging each time.
   */
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
  if (msc_cache) { /* keep the cache coherent */
    uint32_t base = lba & ~(uint32_t)(MSC_CHUNK - 1);
    for (int i = 0; i < MSC_LINES; i++)
      if (msc_ctag[i] == base) {
        msc_ctag[i] = 0xFFFFFFFFu;
        break;
      }
  }
  msc_unlock();
  if (rc != 0 && !hc_responsive) /* controller went silent: drop it */
    msc_ready = 0;
  return rc;
}

/* ================================================================ MTP / PTP
 *
 * A phone or camera in "file transfer" mode speaks the Media Transfer Protocol
 * (a Microsoft extension of the Picture Transfer Protocol). Unlike a USB stick
 * it exposes *objects* (files + folders), not 512-byte sectors, so it can't be
 * a block device the FAT driver mounts -- it's browsed object-by-object
 * instead, via the `mtp` shell command (info / ls / get).
 *
 * The transport is the still-image USB class (interface 06/01/01) over a bulk
 * IN + bulk OUT pair (an interrupt-IN carries asynchronous events we ignore).
 * Everything rides "containers": a 12-byte little-endian header
 *   u32 length (incl. header)  u16 type  u16 code  u32 transaction-id
 * followed by a payload. A transaction is Command(type 1) -> optional Data(type
 * 2) -> Response(type 3). We implement just the read path: OpenSession,
 * GetDeviceInfo, GetObjectHandles, GetObjectInfo, GetObject.
 *
 * Concurrency mirrors the mass-storage path: the xHCI rings are driven only
 * from the USB poll thread, so the shell-side calls (xhci_mtp_*) drop a request
 * in a one-slot mailbox and yield; the poll thread runs the bulk container
 * exchange. One outstanding op at a time, serialized by a busy flag. */

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
#define MTP_FMT_ASSOCIATION 0x3001 /* a folder (ObjectInfo.ObjectFormat)  */
#define MTP_PARENT_ROOT 0xFFFFFFFFu
#define MTP_STORAGE_ALL 0xFFFFFFFFu

#define MTP_CHUNK 4096 /* one palloc page == one DMA chunk     */

static bool mtp_ready; /* device configured + session open     */
static int mtp_slot;
static int mtp_in_dci, mtp_out_dci;
static uint8_t mtp_in_ep, mtp_out_ep;
static uint16_t mtp_in_mps, mtp_out_mps;
static struct ring mtp_in_ring, mtp_out_ring;
static uint32_t mtp_xid;   /* transaction-id counter               */
static char mtp_model[64]; /* "<manufacturer> <model>"             */
static uint8_t *mtp_cmd;   /* DMA: command/response container       */
static uint8_t *mtp_buf;   /* DMA: one data-phase chunk             */

/* One-slot request mailbox serviced by the poll thread. kind 0 = a generic
 * transaction whose data-IN payload is copied into mb_out; kind 1 = stream
 * object mb_handle into owofs file mb_dst. */
static volatile bool mtp_req, mtp_done, mtp_busy;
static volatile int mtp_kind, mtp_result;
static volatile uint32_t mtp_resp_code;
static volatile uint16_t mtp_mb_op;
static volatile uint32_t mtp_mb_param[5];
static volatile int mtp_mb_nparam;
static volatile uint8_t *mtp_mb_out; /* kind 0: where to copy the payload    */
static volatile int mtp_mb_outcap;
static volatile uint32_t mtp_mb_handle; /* kind 1: object to download */
static volatile const char *mtp_mb_dst; /* kind 1: owofs destination path */

/* Find a still-image (MTP/PTP) interface 06/01/01 in the cached config
 * descriptor and grab its bulk IN/OUT endpoints. */
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
    if (t == 0x04 && i + 7 < len) { /* INTERFACE: still image PTP */
      in_iface = (p[i + 5] == 0x06 && p[i + 6] == 0x01 && p[i + 7] == 0x01);
    } else if (t == 0x05 && in_iface && i + 5 < len) { /* ENDPOINT */
      uint8_t addr = p[i + 2], attr = p[i + 3];
      uint16_t mps = p[i + 4] | (p[i + 5] << 8);
      if ((attr & 0x03) == 0x02) { /* bulk */
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

/* Configure the bulk pair (same input-context discipline as the MSC path: copy
 * the live slot context, only bump Context Entries). Returns 0 on success. */
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
  slot_set_entries(sctx, max_dci);

  epin[1] = (3u << 1) | (6u << 3) | ((uint32_t)d->bulk_in_mps << 16); /* bulk IN  */
  epin[2] = (uint32_t)(uintptr_t)mtp_in_ring.base | 1;
  epin[4] = d->bulk_in_mps;
  epout[1] = (3u << 1) | (2u << 3) | ((uint32_t)d->bulk_out_mps << 16); /* bulk OUT */
  epout[2] = (uint32_t)(uintptr_t)mtp_out_ring.base | 1;
  epout[4] = d->bulk_out_mps;

  ring_push(&cmd_ring, (uint32_t)(uintptr_t)input, 0, 0, TRB_TYPE(TRB_CONFIG_EP) | ((uint32_t)d->slot << 24));
  int cc = cmd_submit();
  pfree(input);           /* transient: consumed by the command */
  if (cc != CC_SUCCESS) { /* config failed: free the transfer rings */
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

/* Send a Command container (op + up to 5 params) on the bulk-OUT pipe. Returns
 * the transaction-id used, or 0 on error. */
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

/* Read a Response container (type 3) into mtp_cmd and return its response code,
 * or 0 on transport error. */
static uint32_t mtp_read_response(void)
{
  int cc;
  if (bulk_xfer(mtp_slot, &mtp_in_ring, mtp_in_dci, mtp_cmd, MTP_CHUNK, &cc) < 0)
    return 0;
  if (mtp_cmd[4] != MTP_TYPE_RESPONSE)
    return 0;
  return (uint32_t)mtp_cmd[6] | ((uint32_t)mtp_cmd[7] << 8);
}

/* Run a transaction whose data phase is IN (or absent): send the command, read
 * the Data container into out[outcap] with the 12-byte header stripped, then
 * the Response. Returns the payload length copied (>=0), or -1 on error; *rcode
 * gets the MTP response code. A no-data op passes out=NULL/outcap=0. */
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
      uint32_t total = load_le32(mtp_buf); /* whole container length  */
      int plen = (int)total - 12;
      if (plen < 0)
        plen = 0;
      if (plen > got - 12)
        plen = got - 12; /* only what this chunk held*/
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

/* Stream object `handle` to owofs path `dst`. Learns the size from the Data
 * container header, prealloc's the file, then writes each bulk-IN chunk
 * straight to disk. Runs on the poll thread (fs I/O is safe here: the caller is
 * blocked, so no concurrent owofs access). Returns 0 on success, -1 on error.
 */
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
  uint32_t total = load_le32(mtp_buf); /* header + file bytes          */
  uint32_t fsize = total >= 12 ? total - 12 : 0;

  if (fs_prealloc(dst, fsize) != 0)
    return -1;

  uint32_t written = 0;
  int first = (int)(got - 12); /* file bytes in the first chunk*/
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

/* ---- MTP dataset parsing (little-endian; runs in the caller's thread) ---- */

/* An MTP string: 1 byte count of UTF-16 units (incl. terminator, 0 == empty),
 * then that many UTF-16LE units. Copies the low byte of each as ASCII into
 * out[cap] (NUL-terminated, terminator dropped). Advances *off. */
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
    if (i < n - 1 && o < cap - 1) /* drop the NUL terminator    */
      out[o++] = (lo >= 0x20 && lo < 0x7F) ? (char)lo : '?';
  }
  out[o] = '\0';
}

/* Skip an MTP array: a u32 element count followed by `stride`-byte elements.
 * DeviceInfo's Operations/Events/Properties/Capture/Image arrays are AUINT16
 * (2-byte codes); storage-id and object-handle arrays are AUINT32. Advances
 * *off. */
static void mtp_skip_array(const uint8_t *buf, int len, int *off, int stride)
{
  if (*off + 4 > len) {
    *off = len;
    return;
  }
  uint32_t cnt = load_le32(buf + *off);
  /* cnt is device-controlled: compute the next offset in 64-bit and clamp to
   * len, so an overflowing count can't drive *off negative and let a later
   * signed `*off >= len` check pass on an out-of-bounds index. */
  uint64_t next = (uint64_t)*off + 4 + (uint64_t)cnt * (uint32_t)stride;
  *off = (next > (uint64_t)len) ? len : (int)next;
}
/* Skip an MTP string. */
static void mtp_skip_string(const uint8_t *buf, int len, int *off)
{
  char tmp[2];
  mtp_get_string(buf, len, off, tmp, sizeof tmp);
}

/* Bring up an MTP device: configure the bulk pipes, read DeviceInfo for the
 * model string, and open a session. Runs at enumeration time (single ring
 * context), so it uses the synchronous transfers directly. Returns 0 on
 * success. */
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

  /* DeviceInfo: walk past the version/extension/array fields to Manufacturer +
   * Model (see the dataset layout in the MTP spec). */
  uint32_t rc;
  int n = mtp_do_transaction(MTP_OP_GET_DEVICE_INFO, 0, 0, mtp_buf, MTP_CHUNK, &rc);
  mtp_model[0] = '\0';
  if (n > 0) {
    int o = 0;
    o += 2;                            /* StandardVersion (u16)         */
    o += 4;                            /* VendorExtensionID (u32)       */
    o += 2;                            /* VendorExtensionVersion (u16)  */
    mtp_skip_string(mtp_buf, n, &o);   /* VendorExtensionDesc           */
    o += 2;                            /* FunctionalMode (u16)          */
    mtp_skip_array(mtp_buf, n, &o, 2); /* OperationsSupported  (AUINT16)*/
    mtp_skip_array(mtp_buf, n, &o, 2); /* EventsSupported      (AUINT16)*/
    mtp_skip_array(mtp_buf, n, &o, 2); /* DevicePropertiesSupported     */
    mtp_skip_array(mtp_buf, n, &o, 2); /* CaptureFormats       (AUINT16)*/
    mtp_skip_array(mtp_buf, n, &o, 2); /* ImageFormats         (AUINT16)*/
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

  uint32_t sp[1] = {1}; /* session id (non-zero)         */
  if (mtp_do_transaction(MTP_OP_OPEN_SESSION, sp, 1, 0, 0, &rc) != 0 && rc != 0x201E /* SessionAlreadyOpen */) {
    con_printf("mtp: OpenSession failed rc=%04x\n", rc);
    return -1;
  }

  mtp_ready = 1;
  con_printf("mtp: %s ready (file transfer)\n", mtp_model[0] ? mtp_model : "device");
  return 0;
}

/* ---- poll-thread mailbox service + shell-thread wrappers ---- */

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

/* Serialize MTP ops across caller threads; every yield is preceded by `sti`
 * (see the matching note in msc_lock). */
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

/* List the children of `parent` (MTP_PARENT_ROOT for the storage root): for
 * each object, fetch its ObjectInfo and hand (handle, name, size, is_dir) to
 * `cb`. Returns the number listed, or -1 on error. */
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
    count = (uint32_t)(n - 4) / 4; /* clamp to what we received   */

  int listed = 0;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t h = load_le32(handles + 4 + i * 4);
    uint32_t ip[1] = {h};
    int in = mtp_xfer(MTP_OP_GET_OBJECT_INFO, ip, 1, info, sizeof info);
    if (in < 52)
      continue;
    uint16_t fmt = (uint16_t)(info[4] | (info[5] << 8));
    uint32_t size = load_le32(info + 8);
    int o = 52; /* Filename begins at byte 52  */
    char name[64];
    mtp_get_string(info, in, &o, name, sizeof name);
    if (cb)
      cb(h, name, size, fmt == MTP_FMT_ASSOCIATION, ctx);
    listed++;
  }
  mtp_unlock();
  return listed;
}

/* Download object `handle` to owofs path `dst`. Returns 0 on success, -1 err.
 */
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

/* Configure Endpoint adding one endpoint (context dwords 0..4); 0 on success. The input slot context is a copy of the
 * live one with only Context Entries raised. */
static int config_endpoint(int slot, int dci, const uint32_t ep[5])
{
  uint8_t *input = palloc();
  if (!input || !dcbaa[slot]) {
    if (input)
      pfree(input);
    return -1;
  }
  uint32_t *icc = (uint32_t *)input;
  uint32_t *sctx = (uint32_t *)(input + ctx_sz);
  uint32_t *out_slot = (uint32_t *)(uintptr_t)dcbaa[slot];
  icc[1] = 1u | (1u << dci);
  for (uint32_t i = 0; i < ctx_sz / 4; i++)
    sctx[i] = out_slot[i];
  slot_set_entries(sctx, dci);
  uint32_t *epc = (uint32_t *)(input + ctx_sz * (1 + dci));
  for (int i = 0; i < 5; i++)
    epc[i] = ep[i];

  ring_push(&cmd_ring, (uint32_t)(uintptr_t)input, 0, 0, TRB_TYPE(TRB_CONFIG_EP) | ((uint32_t)slot << 24));
  int cc = cmd_submit();
  pfree(input);
  if (cc != CC_SUCCESS)
    con_printf("xhci: configure-endpoint slot %d dci %d failed cc=%d\n", slot, dci, cc);
  return cc == CC_SUCCESS ? 0 : -1;
}

/* Queue Normal TRBs until the pipe has PIPE_BUFS outstanding, then ring its doorbell. */
static void pipe_post(struct usbh_pipe *p)
{
  while (p->queued < PIPE_BUFS) {
    ring_push(&p->ring, (uint32_t)(uintptr_t)(p->buf + p->tail * p->mps), 0, p->mps,
              TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
    p->tail = (p->tail + 1) % PIPE_BUFS;
    p->queued++;
  }
  db[p->slot] = p->dci;
}

static struct usbh_pipe *pipe_find(int slot, int dci)
{
  if (slot <= 0 || slot >= XHCI_MAX_SLOTS || !devs[slot])
    return NULL;
  for (struct usbh_pipe *p = devs[slot]->pipes; p; p = p->next)
    if (p->dci == dci)
      return p;
  return NULL;
}

/* A transfer event for an interrupt pipe: hand the report to its driver and re-arm. Errors never issue commands here
 * (we may be inside another command's wait); a halt is flagged for pipes_recover on the poll thread. */
static bool pipe_event(struct trb *e)
{
  struct usbh_pipe *p = pipe_find((e->control >> 24) & 0xFF, (e->control >> 16) & 0x1F);
  if (!p)
    return false;
  int cc = (e->status >> 24) & 0xFF;
  uint32_t resid = e->status & 0xFFFFFF;
  if (p->halted || p->dead)
    return true;
  uint8_t idx = p->head;
  p->head = (p->head + 1) % PIPE_BUFS;
  if (p->queued)
    p->queued--;
  if (cc == CC_SUCCESS || cc == CC_SHORT_PKT) {
    p->errors = 0;
    int len = resid < p->mps ? (int)(p->mps - resid) : 0;
    p->cb(p->ctx, p->buf + idx * p->mps, len);
    pipe_post(p);
  } else if (cc == CC_STALL || cc == CC_USB_TXN_ERR || cc == CC_BABBLE) {
    p->halted = true;
    pipe_halted = true;
  } else {
    p->dead = true;
  }
  return true;
}

/* Clear flagged halts: Reset Endpoint, CLEAR_FEATURE(HALT), skip the dead TDs, re-arm. */
static void pipes_recover(void)
{
  if (!pipe_halted)
    return;
  pipe_halted = false;
  for (int s = 1; s <= max_slot_id; s++) {
    if (!devs[s])
      continue;
    for (struct usbh_pipe *p = devs[s]->pipes; p; p = p->next) {
      if (!p->halted)
        continue;
      ep_reset(s, p->dci, &p->ring);
      usbh_control_nodata(s, 0x02, 0x01, 0, p->ep_addr);
      p->queued = p->head = p->tail = 0;
      p->halted = false;
      if (++p->errors > 8) {
        p->dead = true;
        con_printf("usb: slot %d ep %02x gave up after repeated errors\n", s, p->ep_addr);
      } else {
        pipe_post(p);
      }
    }
  }
}

struct usbh_pipe *usbh_open_intr(int slot, uint8_t ep_addr, uint16_t mps, uint8_t interval, usbh_pipe_cb cb, void *ctx)
{
  struct xdev *x = devs[slot];
  if (!x || mps == 0 || mps > 1024 || !(ep_addr & 0x80))
    return NULL;
  struct usbh_pipe *p = kmalloc(sizeof *p);
  if (!p)
    return NULL;
  memset(p, 0, sizeof *p);
  ring_init(&p->ring);
  p->buf = palloc();
  if (!p->ring.base || !p->buf) {
    if (p->ring.base)
      pfree(p->ring.base);
    if (p->buf)
      pfree(p->buf);
    kfree(p);
    return NULL;
  }
  p->slot = slot;
  p->dci = ep_dci(ep_addr);
  p->ep_addr = ep_addr;
  p->mps = mps;
  p->cb = cb;
  p->ctx = ctx;

  uint32_t ep[5] = {0};
  ep[0] = usbh_intr_exponent(x->pub.speed, interval) << 16;
  ep[1] = (3u << 1) | (7u << 3) | ((uint32_t)mps << 16);
  ep[2] = (uint32_t)(uintptr_t)p->ring.base | 1;
  ep[4] = mps | ((uint32_t)mps << 16);
  if (config_endpoint(slot, p->dci, ep) != 0) {
    pfree(p->ring.base);
    pfree(p->buf);
    kfree(p);
    return NULL;
  }
  p->next = x->pipes;
  x->pipes = p;
  pipe_post(p);
  return p;
}

int usbh_config_hub(int slot, int nports, int think_time)
{
  struct xdev *x = devs[slot];
  if (!x || !x->pub.cfg)
    return -1;
  /* A slot-only Configure Endpoint (A0) updates the Hub fields. */
  uint8_t *input = palloc();
  if (!input)
    return -1;
  uint32_t *icc = (uint32_t *)input;
  uint32_t *sctx = (uint32_t *)(input + ctx_sz);
  uint32_t *out_slot = (uint32_t *)(uintptr_t)dcbaa[slot];
  icc[1] = 1u;
  for (uint32_t i = 0; i < ctx_sz / 4; i++)
    sctx[i] = out_slot[i];
  sctx[0] |= 1u << 26;
  sctx[1] = (sctx[1] & 0x00FFFFFFu) | ((uint32_t)nports << 24);
  sctx[2] = (sctx[2] & ~(3u << 16)) | ((uint32_t)(think_time & 3) << 16);
  ring_push(&cmd_ring, (uint32_t)(uintptr_t)input, 0, 0, TRB_TYPE(TRB_CONFIG_EP) | ((uint32_t)slot << 24));
  int cc = cmd_submit();
  pfree(input);
  return cc == CC_SUCCESS ? 0 : -1;
}

struct usbdev *usbh_dev(int slot) { return slot > 0 && slot < XHCI_MAX_SLOTS && devs[slot] ? &devs[slot]->pub : NULL; }
int usbh_max_slot(void) { return max_slot_id; }
bool usbh_is_net_slot(int slot) { return net_up && slot == net_slot; }

/* Drop a device: children first, class drivers, then the slot and every page it held. */
static void remove_device(int slot)
{
  struct xdev *x = devs[slot];
  if (!x)
    return;
  for (struct usbh_pipe *p = x->pipes; p; p = p->next)
    p->dead = true;
  for (int s = 1; s <= max_slot_id; s++)
    if (devs[s] && devs[s]->pub.parent_slot == slot)
      remove_device(s);
  con_printf("usb: slot %d detached\n", slot);
  hub_unbind(slot);
  hid_unbind(slot);
  if (x->pub.is_msc && slot == msc_slot)
    msc_ready = 0;
  if (x->pub.is_mtp && slot == mtp_slot)
    mtp_ready = 0;

  ring_push(&cmd_ring, 0, 0, 0, TRB_TYPE(TRB_DISABLE_SLOT) | ((uint32_t)slot << 24));
  cmd_submit();

  devs[slot] = NULL;
  while (x->pipes) {
    struct usbh_pipe *p = x->pipes;
    x->pipes = p->next;
    pfree(p->ring.base);
    pfree(p->buf);
    kfree(p);
  }
  if (x->ep0.base)
    pfree(x->ep0.base);
  if (dcbaa[slot]) {
    pfree((void *)(uintptr_t)dcbaa[slot]);
    dcbaa[slot] = 0;
  }
  kfree(x->pub.cfg);
  kfree(x);
}

/* Enable a slot, address the device, read its descriptors, select configuration 1 and bind class drivers; returns
 * the slot or 0. parent_slot 0 means a root-hub port. */
static int enumerate_device(int parent_slot, int parent_port, uint32_t root_port, int speed)
{
  struct xdev *parent = parent_slot ? devs[parent_slot] : NULL;
  if (parent && parent->pub.depth >= 5)
    return 0;

  ring_push(&cmd_ring, 0, 0, 0, TRB_TYPE(TRB_ENABLE_SLOT));
  if (cmd_submit() != CC_SUCCESS || last_cmd_slot <= 0)
    return 0;
  int slot_id = last_cmd_slot;
  struct xdev *x = kmalloc(sizeof *x);
  if (!x) {
    ring_push(&cmd_ring, 0, 0, 0, TRB_TYPE(TRB_DISABLE_SLOT) | ((uint32_t)slot_id << 24));
    cmd_submit();
    return 0;
  }
  memset(x, 0, sizeof *x);
  struct usbdev *d = &x->pub;
  d->slot = slot_id;
  d->speed = speed;
  d->root_port = (uint8_t)root_port;
  d->parent_slot = parent_slot;
  d->parent_port = (uint8_t)parent_port;
  if (parent) {
    d->depth = parent->pub.depth + 1;
    d->route = parent->pub.route | ((uint32_t)(parent_port > 15 ? 15 : parent_port) << (4 * parent->pub.depth));
    if (speed < USB_SPEED_HIGH) {
      if (parent->pub.speed == USB_SPEED_HIGH) {
        d->tt_slot = parent_slot;
        d->tt_port = parent_port;
      } else {
        d->tt_slot = parent->pub.tt_slot;
        d->tt_port = parent->pub.tt_port;
      }
    }
  }
  devs[slot_id] = x;
  if (slot_id > max_slot_id)
    max_slot_id = slot_id;

  if (address_device(x) != 0) {
    devs[slot_id] = NULL;
    ring_push(&cmd_ring, 0, 0, 0, TRB_TYPE(TRB_DISABLE_SLOT) | ((uint32_t)slot_id << 24));
    cmd_submit();
    kfree(x);
    return 0;
  }

  uint8_t desc[18];
  /* Two-step read: 8 bytes first to learn bMaxPacketSize0, fix EP0 via Evaluate Context if it differs. */
  if (usbh_control_in(slot_id, 0x80, 0x06, 0x0100, 0, desc, 8) != 8)
    goto fail;
  if (speed < 4) {
    uint32_t guessed = (speed == 3) ? 64 : 8;
    uint32_t real = desc[7] ? desc[7] : 8;
    if (real != guessed)
      eval_context_mps(slot_id, real);
  }
  if (usbh_control_in(slot_id, 0x80, 0x06, 0x0100, 0, desc, 18) != 18)
    goto fail;

  d->vid = desc[8] | (desc[9] << 8);
  d->pid = desc[10] | (desc[11] << 8);
  d->cls = desc[4];

  /* Read the config descriptor (9-byte header for wTotalLength, then all), cache it, select it. */
  uint8_t hdr[9];
  if (usbh_control_in(slot_id, 0x80, 0x06, 0x0200, 0, hdr, 9) == 9) {
    int total = hdr[2] | (hdr[3] << 8);
    if (total > 4096)
      total = 4096;
    if (total >= 9) {
      d->cfg = kmalloc(total);
      if (d->cfg && usbh_control_in(slot_id, 0x80, 0x06, 0x0200, 0, d->cfg, total) == total) {
        d->cfg_len = total;
        usbh_control_nodata(slot_id, 0x00, 0x09, d->cfg[5], 0);
      }
    }
  }
  d->active = 1;

  /* Classify the device and bring up its driver; MTP first (its bulk pair looks like USB-Ethernet). */
  parse_mtp(d);
  if (d->is_mtp) {
    if (!mtp_ready) {
      if (bring_up_mtp(d, (int)root_port) == 0)
        d->bound |= USBH_BOUND_MTP;
    }
  } else {
    /* USB-Ethernet/RNDIS: run the control handshake now (over EP0 - the global
     * ep0_ring is still this device's, mid-enumeration) to INITIALIZE it and
     * learn its MAC. Bulk data path comes next. */
    parse_net(d);
    if (d->is_net && rndis_setup(d) == 0 && !net_up && bring_up_net(d, (int)root_port) == 0)
      d->bound |= USBH_BOUND_NET;

    /* Or USB mass-storage: configure its bulk pipes and probe capacity now
     * (still single-context), so it's a ready block device for FAT. */
    parse_msc(d);
    if (d->is_msc && !msc_ready && bring_up_msc(d, (int)root_port) == 0)
      d->bound |= USBH_BOUND_MSC;
  }
  con_printf("usb: slot %d %04x:%04x attached\n", slot_id, d->vid, d->pid);
  if (d->cfg_len) {
    hub_bind(d);
    hid_bind(d);
  }
  return slot_id;

fail:
  remove_device(slot_id);
  return 0;
}

int usbh_attach_child(int parent_slot, int port, int speed)
{
  struct xdev *parent = devs[parent_slot];
  return parent ? enumerate_device(parent_slot, port, parent->pub.root_port, speed) : 0;
}

void usbh_detach_child(int parent_slot, int port)
{
  for (int s = 1; s <= max_slot_id; s++)
    if (devs[s] && devs[s]->pub.parent_slot == parent_slot && devs[s]->pub.parent_port == port)
      remove_device(s);
}

/* Bring the controller out of reset and stand up its rings - ONCE. Kept
 * separate from port enumeration so OwOS can come up as a persistent USB host
 * at boot and stay running, rather than resetting the controller on every
 * `lsusb` (which looked like a disconnect/reconnect storm to a tethered phone).
 * Returns 0 if the controller is up, -1 if none was found. */
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
  root_slot = kmalloc((max_ports + 1) * sizeof *root_slot);
  if (!root_slot)
    return -1;
  memset(root_slot, 0, (max_ports + 1) * sizeof *root_slot);
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
  erst[0] = (uint32_t)(uintptr_t)event_ring; /* segment base low                    */
  erst[1] = 0;                               /* segment base high                   */
  erst[2] = RING_TRBS;                       /* segment size                        */
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

bool xhci_present(void) { return setup_done; }

/* Bring the port table up to date: enumerate a port only when it's newly
 * connected or its connect-status changed (CSC) - e.g. a phone re-attaching as
 * RNDIS when tethering is toggled. Stable devices keep their cached entry, so
 * we never re-address a configured device (which fails) or disrupt a live
 * tether. */
static void scan_ports(void)
{
  for (uint32_t p = 1; p <= max_ports; p++) {
    uint32_t sc = r32(op, XHCI_PORTSC(p));
    bool changed = (sc & PORT_CSC) != 0;
    if (changed) /* ack the change (write-1-to-clear) */
      w32(op, XHCI_PORTSC(p), (sc & ~PORT_RWC_MASK & ~PORT_PED) | PORT_CSC);
    int slot = root_slot[p];
    if (slot && usbh_is_net_slot(slot))
      continue;

    if ((sc & PORT_CCS) && slot && !changed)
      continue;
    if (slot) {
      remove_device(slot);
      root_slot[p] = 0;
    }
    if (sc & PORT_CCS) {
      int speed = reset_port(p);
      if (speed)
        root_slot[p] = enumerate_device(0, 0, p, speed);
    }
  }
}

/* Take every pending event: the pipe completions and port changes are consumed, the rest wait for data_loop. */
static void event_drain(void)
{
  struct trb *e;
  while ((e = event_next()))
    if (!event_consumed(e))
      defer_event(e);
}

/* Non-blocking upkeep shared by the poll loop and data_loop: halted pipes, HID repeat and LEDs, hub status changes
 * and root-port rescans (on a Port Status Change event, and about once a second regardless). */
static void usb_service(void)
{
  static uint32_t last_scan;
  pipes_recover();
  hid_tick();
  hub_service();
  uint32_t now = timer_ticks();
  if (port_event || now - last_scan >= TIMER_HZ) {
    port_event = false;
    last_scan = now;
    scan_ports();
  }
}

/* The one place ports are scanned after boot, so the xHCI rings are only ever
 * driven from a single context (no locking needed): a phone toggling tethering
 * re-attaches as RNDIS, and this notices the connect-change and reconfigures it
 * within a poll interval - fast enough that the phone's tethering switch
 * engages instead of timing out and snapping back off. `lsusb` only reads the
 * cache. */
static void usb_poll_thread(void)
{
  for (;;) {
    if (net_up)
      data_loop();
    event_drain();
    /* Service a queued mass-storage sector op promptly (checked every ~1 ms),
     * and rescan ports for hotplug only occasionally (~250 ms). Both run from
     * this one thread, so the rings stay single-context. */
    if (msc_req) {
      msc_service();
      continue;
    }
    if (mtp_req) {
      mtp_service();
      continue;
    }
    usb_service();
    timer_sleep(1);
  }
}

/* Boot-time: bring the host up, configure attached devices once, then hand all
 * further scanning to a background thread so OwOS stays a live, responsive USB
 * host - a tethered phone sees a stable computer that reconfigures it promptly.
 */
void xhci_boot(void)
{
  if (setup_controller() != 0)
    return;
  scan_ports(); /* configure whatever's already attached       */
  sched_create_thread("usbpoll", usb_poll_thread);
}
