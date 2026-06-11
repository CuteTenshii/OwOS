/* xhci.c — minimal xHCI (USB 3.x) host controller driver, milestone 1.
 *
 * Goal of this file's first cut: bring an xHCI controller out of reset, stand up
 * the four DMA structures it needs (device-context base array, command ring,
 * event ring, scratchpad), then enumerate the device on a connected port —
 * enable a slot, address the device, and run a control transfer to read its
 * 18-byte device descriptor. That proves the whole control path (rings,
 * doorbells, contexts, transfer events) end to end; bulk transfers and the
 * RNDIS net driver build on exactly this machinery.
 *
 * All DMA memory comes from palloc() — zeroed, 4 KiB-aligned, and identity-
 * mapped (physical == virtual), so a pointer we dereference is the same address
 * we hand the controller. The MMIO register file is mapped uncached.
 *
 * Heavily instrumented on purpose: xHCI bring-up is fiddly and this is debugged
 * by booting in QEMU (`-device qemu-xhci -device usb-net`) and reading the COM1
 * log, so each stage announces itself and its result.
 */

#include <stdint.h>
#include <stddef.h>
#include "xhci.h"
#include "sched.h"
#include "nic.h"
#include "net.h"
#include "pci.h"
#include "io.h"
#include "vga.h"
#include "paging.h"
#include "timer.h"

#define PCI_CLASS_SERIAL_BUS  0x0C
#define PCI_SUBCLASS_USB      0x03
#define USB_PROGIF_XHCI       0x30

/* ---- capability registers (at MMIO base) -------------------------------- */
#define XHCI_CAPLENGTH   0x00   /* byte: length of the capability region      */
#define XHCI_HCSPARAMS1  0x04   /* MaxSlots[7:0], MaxPorts[31:24]              */
#define XHCI_HCSPARAMS2  0x08   /* scratchpad buffer count (split fields)      */
#define XHCI_HCCPARAMS1  0x10   /* CSZ[2]=64-byte contexts, xECP[31:16]        */
#define XHCI_DBOFF       0x14   /* doorbell array offset from base             */
#define XHCI_RTSOFF      0x18   /* runtime register offset from base          */

/* ---- operational registers (at base + CAPLENGTH) ------------------------ */
#define XHCI_USBCMD      0x00
#define XHCI_USBSTS      0x04
#define XHCI_PAGESIZE    0x08
#define XHCI_CRCR        0x18   /* command ring control (64-bit)              */
#define XHCI_DCBAAP      0x30   /* device context base address array (64-bit) */
#define XHCI_CONFIG      0x38   /* MaxSlotsEn[7:0]                            */
#define XHCI_PORTSC(p)   (0x400 + ((p) - 1) * 0x10)   /* per-port, 1-based    */

/* USBCMD / USBSTS bits. */
#define CMD_RS           (1u << 0)    /* run/stop                             */
#define CMD_HCRST        (1u << 1)    /* host controller reset                */
#define STS_HCH          (1u << 0)    /* HC halted                            */
#define STS_CNR          (1u << 11)   /* controller not ready                 */

/* PORTSC bits. */
#define PORT_CCS         (1u << 0)    /* current connect status               */
#define PORT_PED         (1u << 1)    /* port enabled/disabled                */
#define PORT_PR          (1u << 4)    /* port reset                           */
#define PORT_PP          (1u << 9)    /* port power                           */
#define PORT_PLS(v)      (((v) >> 5) & 0xF)            /* port link state      */
#define PORT_SPEED(v)    (((v) >> 10) & 0xF)           /* port speed id        */
/* Write-1-to-clear status-change bits live in the high half; preserve PED/PR
 * and the change bits carefully on every write (PED is RW1CS, so writing 1
 * disables the port — we must write 0 there unless we mean to). */
#define PORT_CSC         (1u << 17)
#define PORT_PRC         (1u << 21)
#define PORT_RWC_MASK    (PORT_CSC | (1u<<18) | (1u<<19) | (1u<<20) | PORT_PRC | (1u<<22))

/* ---- runtime registers (at base + RTSOFF) ------------------------------- */
#define XHCI_IMAN        0x20   /* interrupter 0 management                   */
#define XHCI_IMOD        0x24   /* interrupter 0 moderation                   */
#define XHCI_ERSTSZ      0x28   /* event ring segment table size              */
#define XHCI_ERSTBA      0x30   /* event ring segment table base (64-bit)     */
#define XHCI_ERDP        0x38   /* event ring dequeue pointer (64-bit)        */

/* ---- TRB types ---------------------------------------------------------- */
#define TRB_NORMAL          1
#define TRB_SETUP           2
#define TRB_DATA            3
#define TRB_STATUS          4
#define TRB_LINK            6
#define TRB_ENABLE_SLOT     9
#define TRB_ADDRESS_DEVICE  11
#define TRB_CONFIG_EP       12
#define TRB_EVAL_CONTEXT    13
#define TRB_RESET_EP        14
#define TRB_SET_TRDQ        16
#define TRB_TRANSFER_EVENT  32
#define TRB_CMD_COMPLETION  33
#define TRB_PORT_STATUS     34

#define TRB_CYCLE        (1u << 0)
#define TRB_TYPE(t)      ((uint32_t)(t) << 10)
#define TRB_GET_TYPE(c)  (((c) >> 10) & 0x3F)
#define TRB_IOC          (1u << 5)    /* interrupt on completion              */
#define TRB_IDT          (1u << 6)    /* immediate data (setup stage)         */
#define TRB_TOGGLE       (1u << 1)    /* link TRB: toggle cycle               */

/* TRB completion codes (event status >> 24). */
#define CC_SUCCESS       1
#define CC_BABBLE        3    /* device sent more than expected; halts the ep    */
#define CC_USB_TXN_ERR   4    /* USB transaction error; can halt the ep          */
#define CC_STALL         6    /* endpoint STALL; halted until reset              */
#define CC_SHORT_PKT     13   /* transfer ended early (normal for variable data) */

/* A Transfer Request Block: four little-endian dwords. */
struct trb {
	volatile uint32_t p0;
	volatile uint32_t p1;
	volatile uint32_t status;
	volatile uint32_t control;
};

#define RING_TRBS  256   /* one 4 KiB page holds 256 16-byte TRBs             */

/* A software view of a single-segment ring with a wrapping Link TRB. */
struct ring {
	struct trb *base;
	uint32_t    enq;        /* next enqueue index                            */
	uint32_t    cycle;      /* producer cycle state (1 then toggles on wrap) */
};

/* ------------------------------------------------------------------ state */
static volatile uint8_t *cap;     /* capability registers (MMIO base)        */
static volatile uint8_t *op;      /* operational registers                   */
static volatile uint8_t *rt;      /* runtime registers                       */
static volatile uint32_t *db;     /* doorbell array                          */
static uint32_t ctx_sz;           /* 32 or 64 bytes per context              */
static uint32_t max_ports;

static uint64_t *dcbaa;           /* device context base address array       */
static struct ring cmd_ring;
static struct ring ep0_ring;
static struct trb *event_ring;    /* event ring segment (consumer)           */
static uint32_t    event_cycle;   /* consumer cycle state                    */
static uint32_t    event_idx;

/* One enumerated device, indexed by root-hub port. Cached so each device is
 * enumerated exactly once (re-enumerating a configured device fails and would
 * disrupt a live tether); a port is only re-enumerated when its connection
 * actually changes — which is how we catch a phone switching into RNDIS. */
#define XHCI_MAX_PORTS 256
struct usbdev {
	int      active;
	int      slot;
	int      speed;
	uint16_t vid, pid;
	uint8_t  cls;
	uint8_t  cfg[256];     /* the configuration descriptor blob                 */
	int      cfg_len;
	/* If this looks like a USB-Ethernet (RNDIS) device: the comm interface that
	 * takes encapsulated commands, the bulk data pair, and the MAC we learned. */
	int      is_net;
	int      ctrl_iface;
	uint8_t  bulk_in, bulk_out, intr_in;
	uint16_t bulk_in_mps, bulk_out_mps, intr_in_mps;
	uint8_t  intr_interval;
	uint8_t  mac[6];
	int      mac_ok;
};
static struct usbdev port_dev[XHCI_MAX_PORTS];

/* The active RNDIS USB-Ethernet data path (there is one tethered NIC). Set up
 * once the device is enumerated + RNDIS-initialized; the USB thread then runs a
 * bulk TX/RX loop over these. */
static int        net_up;            /* 1 once the bulk data path is live        */
static int        net_slot;
static int        net_in_dci, net_out_dci;
static uint32_t   net_in_mps;
static struct ring net_in_ring, net_out_ring;
static uint8_t   *net_rx_buf;        /* one outstanding bulk-IN lands here       */
static uint8_t   *net_tx_buf;        /* one outstanding bulk-OUT is built here   */
static uint8_t    net_mac[6];
static int        net_in_pending, net_out_pending;
static uint8_t    net_in_ep;         /* bulk-IN endpoint address (for CLEAR_HALT) */
static int        net_stall_retries; /* bound stall recovery so it can't spin    */
static int        net_intr_dci;      /* RNDIS status (interrupt-IN) endpoint      */
static struct ring net_ep0;          /* the net device's OWN EP0 ring (control)   */
static struct ring net_intr_ring;
static uint8_t   *net_intr_buf;
static int        net_intr_pending;
static int        net_ctrl_iface;    /* interface that takes encapsulated cmds    */
static int        dbg_rx, dbg_tx, dbg_intr;  /* one-shot diagnostics counters     */

/* Software TX queue: nic_send (command context) drops frames here; the USB
 * thread drains them onto the bulk-OUT ring. Fixed slots, no allocation. */
#define TXQ_SLOTS  16
#define TXQ_FRAME  1600
static uint8_t    txq[TXQ_SLOTS][TXQ_FRAME];
static uint16_t   txq_len[TXQ_SLOTS];
static volatile int txq_head, txq_tail;   /* head=produce, tail=consume          */

/* ---- small helpers ------------------------------------------------------ */

static inline uint32_t r32(volatile uint8_t *p, uint32_t off)
{
	return *(volatile uint32_t *)(p + off);
}
static inline void w32(volatile uint8_t *p, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(p + off) = v;
}
static inline void w64(volatile uint8_t *p, uint32_t off, uint64_t v)
{
	*(volatile uint32_t *)(p + off)     = (uint32_t)v;
	*(volatile uint32_t *)(p + off + 4) = (uint32_t)(v >> 32);
}

static void put_hex(uint32_t value, int digits)
{
	static const char h[] = "0123456789abcdef";
	for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4)
		vga_putc(h[(value >> shift) & 0xF]);
}

/* Initialize a producer ring in a fresh page, with a Link TRB in the last slot
 * that points back to the top and toggles the cycle bit on wrap. */
static void ring_init(struct ring *r)
{
	r->base  = palloc();
	r->enq   = 0;
	r->cycle = 1;
	struct trb *link = &r->base[RING_TRBS - 1];
	link->p0      = (uint32_t)r->base;
	link->p1      = 0;
	link->status  = 0;
	link->control = TRB_TYPE(TRB_LINK) | TRB_TOGGLE;   /* cycle set on enqueue */
}

/* Enqueue one TRB (caller fills p0/p1/status and the type/flag bits of control,
 * minus the cycle bit which we apply). Advances the ring, handling the Link. */
static void ring_push(struct ring *r, uint32_t p0, uint32_t p1, uint32_t status,
                      uint32_t control)
{
	struct trb *t = &r->base[r->enq];
	t->p0     = p0;
	t->p1     = p1;
	t->status = status;
	t->control = control | (r->cycle ? TRB_CYCLE : 0);

	r->enq++;
	if (r->enq == RING_TRBS - 1) {        /* reached the Link TRB             */
		struct trb *link = &r->base[RING_TRBS - 1];
		if (r->cycle) link->control |= TRB_CYCLE;
		else          link->control &= ~TRB_CYCLE;
		r->enq   = 0;
		r->cycle ^= 1;
	}
}

/* Spin (bounded) for the next event ring entry whose cycle matches ours.
 * Returns a pointer to it and advances our consumer position + ERDP, or NULL on
 * timeout. */
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
			w64(rt, XHCI_ERDP, (uint64_t)(uint32_t)&event_ring[event_idx]);
			/* Port reset/connect post Port Status Change Events we don't act
			 * on here (we poll PORTSC directly); drain them so callers see the
			 * command/transfer event they're actually waiting for. */
			if (TRB_GET_TYPE(e->control) == TRB_PORT_STATUS)
				continue;
			return e;
		}
		timer_sleep(1);
	}
	return NULL;
}

/* Non-blocking: return the next event if one is ready (advancing ERDP), else
 * NULL. Port-status events are consumed and skipped. Used by the data loop,
 * where bulk IN and OUT completions interleave and must be routed by endpoint. */
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
		w64(rt, XHCI_ERDP, (uint64_t)(uint32_t)&event_ring[event_idx]);
		if (TRB_GET_TYPE(e->control) != TRB_PORT_STATUS)
			return e;
	}
}

/* Ring the command doorbell (DB[0], target 0) and wait for its Command
 * Completion Event. Returns the completion code, or 0 on timeout. */
static int cmd_submit(void)
{
	db[0] = 0;
	struct trb *e = event_wait();
	if (!e)
		return 0;
	if (TRB_GET_TYPE(e->control) != TRB_CMD_COMPLETION)
		return 0;
	return (e->status >> 24) & 0xFF;     /* completion code (slot id in p1)   */
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
				if (((cls >> 24) & 0xFF) != PCI_CLASS_SERIAL_BUS ||
				    ((cls >> 16) & 0xFF) != PCI_SUBCLASS_USB ||
				    ((cls >> 8)  & 0xFF) != USB_PROGIF_XHCI)
					continue;

				/* Size BAR0 (a memory BAR) with memory decode off so the
				 * all-ones probe write doesn't briefly alias live MMIO: write
				 * 0xFFFFFFFF, read the size mask, restore the original. Real
				 * Intel xHCI register files are 64 KiB with the extended caps
				 * (Legacy Support) far out at +0x8000 — mapping only a few pages
				 * page-faults the instant we walk them. */
				uint32_t cmd = pci_read32(bus, slot, f, 0x04);
				pci_write32(bus, slot, f, 0x04, cmd & ~0x2u);   /* mem off       */
				uint32_t bar0 = pci_read32(bus, slot, f, 0x10);
				pci_write32(bus, slot, f, 0x10, 0xFFFFFFFFu);
				uint32_t mask = pci_read32(bus, slot, f, 0x10) & ~0xFu;
				pci_write32(bus, slot, f, 0x10, bar0);          /* restore       */
				pci_write32(bus, slot, f, 0x04, cmd | 0x06);    /* mem + master  */

				uint32_t phys = bar0 & ~0xFu;   /* low 32 bits are enough <4 GiB */
				uint32_t size = ~mask + 1;
				if (size < 0x10000 || size > 0x100000)          /* 64 KiB..1 MiB */
					size = 0x10000;
				if (paging_identity_map(phys, size) != 0)
					return NULL;
				paging_mark_uc(phys, size);
				vga_write("xhci: controller at "); put_hex(phys, 8);
				vga_write(" size "); put_hex(size, 8); vga_putc('\n');
				return (volatile uint8_t *)phys;
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
	volatile uint8_t *p = cap + xecp * 4;     /* xECP is in dword units        */
	for (int guard = 0; guard < 64; guard++) {
		uint32_t capreg = *(volatile uint32_t *)p;
		uint8_t  id   = capreg & 0xFF;
		uint8_t  next = (capreg >> 8) & 0xFF;
		if (id == 1) {                        /* USB Legacy Support             */
			*(volatile uint32_t *)p = capreg | (1u << 24);   /* OS owned        */
			for (int ms = 0; ms < 200; ms++) {
				if (!(*(volatile uint32_t *)p & (1u << 16)))  /* BIOS released   */
					break;
				timer_sleep(1);
			}
			/* USBLEGCTLSTS (next dword): clear all SMI enables + ack changes. */
			*(volatile uint32_t *)(p + 4) = (1u<<29)|(1u<<30)|(1u<<31);
			vga_write("xhci: took ownership from BIOS\n");
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
	timer_sleep(20);          /* PP -> stable connect status (spec: ~20 ms)    */
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
		if (!(r32(op, XHCI_USBCMD) & CMD_HCRST) &&
		    !(r32(op, XHCI_USBSTS) & STS_CNR))
			return 0;
		timer_sleep(1);
	}
	return -1;
}

static void setup_scratchpad(void)
{
	uint32_t p2 = r32(cap, XHCI_HCSPARAMS2);
	uint32_t n  = ((p2 >> 27) & 0x1F) | (((p2 >> 21) & 0x1F) << 5);   /* hi|lo */
	if (!n)
		return;
	uint64_t *arr = palloc();              /* array of scratchpad page pointers */
	for (uint32_t i = 0; i < n && i < 512; i++)
		arr[i] = (uint32_t)palloc();
	dcbaa[0] = (uint32_t)arr;              /* DCBAA slot 0 = scratchpad array   */
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
		if (sc & PORT_PRC)                 /* reset complete                  */
			break;
		timer_sleep(1);
	}
	w32(op, XHCI_PORTSC(p), (sc & ~PORT_RWC_MASK & ~PORT_PED) | PORT_PRC);
	sc = r32(op, XHCI_PORTSC(p));
	return (sc & PORT_PED) ? PORT_SPEED(sc) : 0;
}

/* Build the input context (input control + slot + EP0) for Address Device, and
 * the output device context (registered in DCBAA[slot]). Returns 0 on success.
 * `speed` is the xHCI port speed id; `port` is the root-hub port number. */
static int address_device(int slot_id, int port, int speed)
{
	uint8_t *input  = palloc();      /* input control(0) + slot(1) + EP0(2)    */
	uint8_t *output = palloc();      /* device context for the controller      */

	uint32_t *icc  = (uint32_t *)(input);                 /* input control ctx */
	uint32_t *sctx = (uint32_t *)(input + ctx_sz * 1);    /* slot context      */
	uint32_t *ep0  = (uint32_t *)(input + ctx_sz * 2);    /* EP0 context       */

	icc[1] = 0x3;                    /* add slot context (A0) + EP0 (A1)        */

	/* Slot: context entries = 1, speed, root-hub port number. */
	sctx[0] = (1u << 27) | ((uint32_t)speed << 20);
	sctx[1] = (uint32_t)port << 16;

	/* EP0: control endpoint, default max packet by speed, its transfer ring. */
	uint32_t mps = (speed >= 4) ? 512 : (speed == 3 ? 64 : 8); /* SS/HS/FS-LS  */
	ring_init(&ep0_ring);
	ep0[1] = (4u << 3) | (mps << 16);                /* EP type 4 = control     */
	ep0[2] = (uint32_t)ep0_ring.base | 1;            /* TR dequeue ptr | DCS=1  */
	ep0[3] = 0;

	dcbaa[slot_id] = (uint32_t)output;

	ring_push(&cmd_ring, (uint32_t)input, 0, 0,
	          TRB_TYPE(TRB_ADDRESS_DEVICE) | ((uint32_t)slot_id << 24));
	int cc = cmd_submit();
	if (cc != CC_SUCCESS) {
		vga_write("xhci: address-device failed cc="); vga_write_dec(cc);
		vga_putc('\n');
		return -1;
	}
	return 0;
}

/* Update EP0's max packet size with an Evaluate Context command, once the real
 * bMaxPacketSize0 is known. The spec evaluates only the max-packet-size field for
 * the default control endpoint, so the TR dequeue pointer the controller already
 * tracks is left untouched. Returns 0 on success. */
static int eval_context_mps(int slot_id, uint32_t mps)
{
	uint8_t *input = palloc();
	uint32_t *icc  = (uint32_t *)input;
	uint32_t *ep0  = (uint32_t *)(input + ctx_sz * 2);

	icc[1] = 0x2;                         /* add EP0 (A1) only                  */
	ep0[1] = (4u << 3) | (mps << 16);     /* control EP, new max packet size    */

	ring_push(&cmd_ring, (uint32_t)input, 0, 0,
	          TRB_TYPE(TRB_EVAL_CONTEXT) | ((uint32_t)slot_id << 24));
	return cmd_submit() == CC_SUCCESS ? 0 : -1;
}

/* General control-IN transfer over EP0: SETUP (immediate 8-byte request) ->
 * DATA IN -> STATUS OUT. The data lands in out[]; returns `len` on success, -1
 * otherwise. `len` must fit in one page (the DMA buffer is a single frame). */
static int control_in(int slot_id, uint8_t bmReqType, uint8_t bReq,
                      uint16_t wValue, uint16_t wIndex, uint8_t *out, int len)
{
	uint8_t *buf = palloc();
	uint32_t setup_lo = bmReqType | ((uint32_t)bReq << 8) | ((uint32_t)wValue << 16);
	uint32_t setup_hi = (uint32_t)wIndex | ((uint32_t)len << 16);
	ring_push(&ep0_ring, setup_lo, setup_hi, 8,
	          TRB_TYPE(TRB_SETUP) | TRB_IDT | (3u << 16));   /* TRT=3 IN data   */
	ring_push(&ep0_ring, (uint32_t)buf, 0, len,
	          TRB_TYPE(TRB_DATA) | (1u << 16));              /* DIR=1 IN        */
	ring_push(&ep0_ring, 0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC);
	db[slot_id] = 1;                 /* device doorbell, target EP0 (DCI 1)     */

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

/* Control transfer with no data stage (SET_CONFIGURATION etc.): SETUP -> STATUS
 * IN. Returns 0 on success. */
static int control_no_data(int slot_id, uint8_t bmReqType, uint8_t bReq,
                           uint16_t wValue, uint16_t wIndex)
{
	uint32_t setup_lo = bmReqType | ((uint32_t)bReq << 8) | ((uint32_t)wValue << 16);
	ring_push(&ep0_ring, setup_lo, (uint32_t)wIndex, 8,
	          TRB_TYPE(TRB_SETUP) | TRB_IDT);                /* TRT=0 no data   */
	ring_push(&ep0_ring, 0, 0, 0,
	          TRB_TYPE(TRB_STATUS) | TRB_IOC | (1u << 16));  /* DIR=1 IN status */
	db[slot_id] = 1;

	struct trb *e = event_wait();
	if (!e || TRB_GET_TYPE(e->control) != TRB_TRANSFER_EVENT)
		return -1;
	return ((e->status >> 24) & 0xFF) == CC_SUCCESS ? 0 : -1;
}

/* Control transfer with an OUT data stage (e.g. RNDIS SEND_ENCAPSULATED_COMMAND):
 * SETUP -> DATA OUT -> STATUS IN. Returns 0 on success. */
static int control_out(int slot_id, uint8_t bmReqType, uint8_t bReq,
                       uint16_t wValue, uint16_t wIndex, const uint8_t *data, int len)
{
	uint8_t *buf = palloc();
	for (int i = 0; i < len; i++)
		buf[i] = data[i];

	uint32_t setup_lo = bmReqType | ((uint32_t)bReq << 8) | ((uint32_t)wValue << 16);
	uint32_t setup_hi = (uint32_t)wIndex | ((uint32_t)len << 16);
	ring_push(&ep0_ring, setup_lo, setup_hi, 8,
	          TRB_TYPE(TRB_SETUP) | TRB_IDT | (2u << 16));   /* TRT=2 OUT data  */
	ring_push(&ep0_ring, (uint32_t)buf, 0, len,
	          TRB_TYPE(TRB_DATA));                           /* DIR=0 OUT       */
	ring_push(&ep0_ring, 0, 0, 0,
	          TRB_TYPE(TRB_STATUS) | TRB_IOC | (1u << 16));  /* DIR=1 IN status */
	db[slot_id] = 1;

	struct trb *e = event_wait();
	int rc = -1;
	if (e && TRB_GET_TYPE(e->control) == TRB_TRANSFER_EVENT &&
	    ((e->status >> 24) & 0xFF) == CC_SUCCESS)
		rc = 0;
	pfree(buf);
	return rc;
}

/* ----------------------------------------------------------------- RNDIS */
/* RNDIS is the Microsoft "Ethernet over USB" protocol Android uses for USB
 * tethering. Control messages ride EP0 as CDC encapsulated commands/responses;
 * Ethernet frames (later) ride the bulk pipes wrapped in packet headers. This
 * does the bring-up handshake: INITIALIZE, then QUERY the permanent MAC. */

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

#define RNDIS_INITIALIZE_MSG   0x00000002
#define RNDIS_QUERY_MSG        0x00000004
#define RNDIS_SET_MSG          0x00000005
#define OID_802_3_PERM_ADDR    0x01010101
#define OID_GEN_PACKET_FILTER  0x0001010E

/* Send one RNDIS control message and read the reply, both over EP0. */
static int rndis_cmd(int slot, int iface, const uint8_t *msg, int mlen,
                     uint8_t *resp, int rlen)
{
	if (control_out(slot, 0x21, 0x00, 0, iface, msg, mlen) != 0)  /* SEND_ENCAP */
		return -1;
	return control_in(slot, 0xA1, 0x01, 0, iface, resp, rlen);    /* GET_ENCAP  */
}

/* INITIALIZE the RNDIS device and query its MAC into d->mac. Returns 0 on
 * success. */
static int rndis_setup(struct usbdev *d)
{
	uint8_t msg[32], resp[64];

	for (int i = 0; i < 32; i++) msg[i] = 0;
	put_le32(msg + 0, RNDIS_INITIALIZE_MSG);
	put_le32(msg + 4, 24);              /* message length                       */
	put_le32(msg + 8, 1);               /* request id                           */
	put_le32(msg + 12, 1);              /* RNDIS major version                  */
	put_le32(msg + 16, 0);              /* minor version                        */
	put_le32(msg + 20, 0x4000);         /* max transfer size we'll accept       */
	/* INITIALIZE_CMPLT layout: type(0) len(4) reqid(8) status(12) ... */
	if (rndis_cmd(d->slot, d->ctrl_iface, msg, 24, resp, sizeof resp) < 16)
		return -1;
	if (le32(resp + 12) != 0)           /* INITIALIZE_CMPLT status              */
		return -1;

	for (int i = 0; i < 32; i++) msg[i] = 0;
	put_le32(msg + 0, RNDIS_QUERY_MSG);
	put_le32(msg + 4, 28);
	put_le32(msg + 8, 2);
	put_le32(msg + 12, OID_802_3_PERM_ADDR);
	put_le32(msg + 16, 0);              /* info buffer length                   */
	put_le32(msg + 20, 0);              /* info buffer offset                   */
	put_le32(msg + 24, 0);              /* reserved                             */
	/* QUERY_CMPLT layout: type(0) len(4) reqid(8) status(12) infolen(16)
	 * infooffset(20) [data]; the offset is measured from byte 8 (reqid). */
	int n = rndis_cmd(d->slot, d->ctrl_iface, msg, 28, resp, sizeof resp);
	if (n < 24 || le32(resp + 12) != 0) /* QUERY_CMPLT status                   */
		return -1;
	uint32_t off = 8 + le32(resp + 20); /* InfoBufferOffset is from byte 8      */
	if ((int)off + 6 > n)
		return -1;
	for (int i = 0; i < 6; i++)
		d->mac[i] = resp[off + i];
	d->mac_ok = 1;
	return 0;
}

/* Scan the cached config descriptor for a USB-Ethernet shape: a bulk IN + bulk
 * OUT pair (the data pipes) and an interrupt IN on the comm interface (the RNDIS
 * notification). Works for both the textbook CDC form (02/02/ff + 0a) and the
 * vendor-class single-interface form some phones use. */
static void parse_net(struct usbdev *d)
{
	d->is_net = 0;
	int cur_iface = 0, intr_iface = -1;
	for (int i = 0; i + 2 <= d->cfg_len; ) {
		uint8_t blen = d->cfg[i];
		if (blen < 2)
			break;
		uint8_t btype = d->cfg[i + 1];
		if (btype == 0x04 && i + 9 <= d->cfg_len) {       /* interface         */
			cur_iface = d->cfg[i + 2];
		} else if (btype == 0x05 && i + 7 <= d->cfg_len) {/* endpoint          */
			uint8_t addr = d->cfg[i + 2];
			uint8_t attr = d->cfg[i + 3] & 0x3;
			uint16_t mps = (d->cfg[i + 4] | (d->cfg[i + 5] << 8)) & 0x7FF;
			if (attr == 2) {                              /* bulk              */
				if (addr & 0x80) { d->bulk_in  = addr; d->bulk_in_mps  = mps; }
				else             { d->bulk_out = addr; d->bulk_out_mps = mps; }
			} else if (attr == 3 && (addr & 0x80)) {      /* interrupt IN      */
				d->intr_in = addr;
				d->intr_in_mps = mps ? mps : 16;
				d->intr_interval = d->cfg[i + 6];        /* bInterval         */
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
	for (int i = 0; i < 64; i++) msg[i] = 0;
	put_le32(msg + 0, RNDIS_SET_MSG);
	put_le32(msg + 4, 28 + 4);          /* header + 4-byte info buffer          */
	put_le32(msg + 8, 3);               /* request id                           */
	put_le32(msg + 12, OID_GEN_PACKET_FILTER);
	put_le32(msg + 16, 4);              /* info buffer length                   */
	put_le32(msg + 20, 20);             /* info buffer offset (from byte 8)      */
	put_le32(msg + 24, 0);              /* reserved                             */
	put_le32(msg + 28, 0x0000000D);     /* DIRECTED | MULTICAST | BROADCAST     */
	if (rndis_cmd(d->slot, d->ctrl_iface, msg, 32, resp, sizeof resp) < 16)
		return -1;
	return le32(resp + 12) == 0 ? 0 : -1;   /* SET_CMPLT status                 */
}

/* xHCI Device Context Index for an endpoint address: EP0 is 1; others are
 * ep_num*2 (+1 for IN). */
static int ep_dci(uint8_t addr)
{
	return ((addr & 0x0F) * 2) + ((addr & 0x80) ? 1 : 0);
}

/* Add the bulk IN/OUT endpoints to the device with a Configure Endpoint command,
 * each backed by its own transfer ring. Returns 0 on success. */
static int configure_bulk(struct usbdev *d, int port)
{
	int in_dci   = ep_dci(d->bulk_in);
	int out_dci  = ep_dci(d->bulk_out);
	int intr_dci = d->intr_in ? ep_dci(d->intr_in) : 0;
	int max_dci  = in_dci > out_dci ? in_dci : out_dci;
	if (intr_dci > max_dci) max_dci = intr_dci;

	ring_init(&net_in_ring);
	ring_init(&net_out_ring);
	if (intr_dci) ring_init(&net_intr_ring);

	uint8_t *input = palloc();
	uint32_t *icc  = (uint32_t *)input;
	uint32_t *sctx = (uint32_t *)(input + ctx_sz * 1);
	uint32_t *epin  = (uint32_t *)(input + ctx_sz * (1 + in_dci));
	uint32_t *epout = (uint32_t *)(input + ctx_sz * (1 + out_dci));

	/* Add slot (A0, to bump Context Entries) + bulk in/out (+ interrupt). */
	icc[1] = (1u << 0) | (1u << in_dci) | (1u << out_dci) |
	         (intr_dci ? (1u << intr_dci) : 0);
	sctx[0] = ((uint32_t)max_dci << 27) | ((uint32_t)d->speed << 20);
	sctx[1] = (uint32_t)port << 16;

	/* EP type: bulk OUT = 2, bulk IN = 6, interrupt IN = 7. CErr=3. */
	epin[1]  = (3u << 1) | (6u << 3) | ((uint32_t)d->bulk_in_mps  << 16);
	epin[2]  = (uint32_t)net_in_ring.base | 1;          /* TR dequeue | DCS      */
	epin[4]  = d->bulk_in_mps;                          /* avg TRB length        */
	epout[1] = (3u << 1) | (2u << 3) | ((uint32_t)d->bulk_out_mps << 16);
	epout[2] = (uint32_t)net_out_ring.base | 1;
	epout[4] = d->bulk_out_mps;
	if (intr_dci) {
		uint32_t *epi = (uint32_t *)(input + ctx_sz * (1 + intr_dci));
		uint32_t iv = d->intr_interval ? d->intr_interval - 1 : 6;
		epi[0] = iv << 16;                              /* Interval              */
		epi[1] = (3u << 1) | (7u << 3) | ((uint32_t)d->intr_in_mps << 16);
		epi[2] = (uint32_t)net_intr_ring.base | 1;
		epi[4] = d->intr_in_mps;
	}

	ring_push(&cmd_ring, (uint32_t)input, 0, 0,
	          TRB_TYPE(TRB_CONFIG_EP) | ((uint32_t)d->slot << 24));
	if (cmd_submit() != CC_SUCCESS)
		return -1;

	net_slot     = d->slot;
	net_in_dci   = in_dci;
	net_out_dci  = out_dci;
	net_intr_dci = intr_dci;
	net_in_mps   = d->bulk_in_mps;
	net_in_ep    = d->bulk_in;
	net_ctrl_iface = d->ctrl_iface;
	return 0;
}

/* ------------------------------------------------------ RNDIS data framing */
/* Each Ethernet frame rides the bulk pipe wrapped in a 44-byte RNDIS packet
 * message: type=1, total length, then the data offset/length and zeroed
 * out-of-band/per-packet-info fields. */
#define RNDIS_PACKET_MSG  0x00000001
#define RNDIS_HDR         44

/* nic_ops.send: queue a frame for the USB thread to transmit (non-blocking). */
static void rndis_send(const uint8_t *frame, uint32_t len)
{
	if (!net_up || len > TXQ_FRAME)
		return;
	__asm__ volatile ("cli");
	int next = (txq_head + 1) % TXQ_SLOTS;
	if (next != txq_tail) {              /* drop if the queue is full            */
		for (uint32_t i = 0; i < len; i++)
			txq[txq_head][i] = frame[i];
		txq_len[txq_head] = (uint16_t)len;
		txq_head = next;
	}
	__asm__ volatile ("sti");
}

static const uint8_t *rndis_get_mac(void) { return net_mac; }

static const struct nic_ops rndis_ops = {
	.name = "rndis", .probe = 0, .send = rndis_send, .mac = rndis_get_mac,
};

/* Post a bulk-IN request so the device can hand us the next packet. */
static void post_bulk_in(void)
{
	ring_push(&net_in_ring, (uint32_t)net_rx_buf, 0, 2048,
	          TRB_TYPE(TRB_NORMAL) | TRB_IOC);
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
	ring_push(&net_intr_ring, (uint32_t)net_intr_buf, 0, 16,
	          TRB_TYPE(TRB_NORMAL) | TRB_IOC);
	db[net_slot] = net_intr_dci;
	net_intr_pending = 1;
}

/* Build the RNDIS-wrapped frame in net_tx_buf and post it on bulk-OUT. */
static void post_bulk_out(const uint8_t *frame, int len)
{
	for (int i = 0; i < RNDIS_HDR; i++) net_tx_buf[i] = 0;
	put_le32(net_tx_buf + 0, RNDIS_PACKET_MSG);
	put_le32(net_tx_buf + 4, RNDIS_HDR + len);   /* total message length        */
	put_le32(net_tx_buf + 8, RNDIS_HDR - 8);     /* data offset (from byte 8)   */
	put_le32(net_tx_buf + 12, len);              /* data length                 */
	for (int i = 0; i < len; i++)
		net_tx_buf[RNDIS_HDR + i] = frame[i];
	ring_push(&net_out_ring, (uint32_t)net_tx_buf, 0, RNDIS_HDR + len,
	          TRB_TYPE(TRB_NORMAL) | TRB_IOC);
	db[net_slot] = net_out_dci;
	net_out_pending = 1;
}

/* Bring up the RNDIS data path for a freshly enumerated, initialized device:
 * set the packet filter, configure the bulk endpoints, allocate buffers, and
 * register as the active NIC. Returns 0 on success. */
static int bring_up_net(struct usbdev *d, int port)
{
	if (rndis_set_filter(d) != 0) {
		vga_write("rndis: set-filter failed\n");
		return -1;
	}
	if (configure_bulk(d, port) != 0) {
		vga_write("rndis: configure-endpoint failed\n");
		return -1;
	}
	vga_write("rndis: data path up (in ep=");
	put_hex(d->bulk_in, 2);  vga_write(" dci="); vga_write_dec(net_in_dci);
	vga_write(" mps="); vga_write_dec(d->bulk_in_mps);
	vga_write(", out ep="); put_hex(d->bulk_out, 2);
	vga_write(" dci="); vga_write_dec(net_out_dci);
	vga_write(" mps="); vga_write_dec(d->bulk_out_mps);
	vga_write(")\n");

	net_rx_buf = palloc();
	net_tx_buf = palloc();
	net_intr_buf = palloc();
	for (int i = 0; i < 6; i++)
		net_mac[i] = d->mac[i];
	txq_head = txq_tail = 0;
	net_in_pending = net_out_pending = net_intr_pending = 0;

	/* Save this device's EP0 ring NOW, while the global ep0_ring still points at
	 * it. Later enumerations (other ports) overwrite the global ring, so the data
	 * loop's control transfers (CLEAR_FEATURE, encapsulated responses) must use
	 * this saved copy or they'd drive the wrong device's endpoint 0. */
	net_ep0 = ep0_ring;

	nic_set(&rndis_ops);
	net_up = 1;
	/* NB: the bulk/interrupt INs are armed in data_loop, NOT here — arming them
	 * now would post transfer events onto the shared event ring while the boot
	 * scan is still enumerating other devices, corrupting their command waits. */
	net_dhcp_start();                    /* lease an IP from the phone/router    */
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
		return;                                  /* give up; DHCP will time out  */
	if (net_stall_retries <= 2)
		vga_write("usbnet: recovering bulk-in\n");

	ring_push(&cmd_ring, 0, 0, 0, TRB_TYPE(TRB_RESET_EP) |
	          ((uint32_t)net_in_dci << 16) | ((uint32_t)net_slot << 24));
	cmd_submit();
	control_no_data(net_slot, 0x02, 0x01, 0, net_in_ep);   /* CLEAR_FEATURE HALT */
	uint32_t deq = (uint32_t)&net_in_ring.base[net_in_ring.enq] | net_in_ring.cycle;
	ring_push(&cmd_ring, deq, 0, 0, TRB_TYPE(TRB_SET_TRDQ) |
	          ((uint32_t)net_in_dci << 16) | ((uint32_t)net_slot << 24));
	cmd_submit();

	net_in_pending = 0;
	post_bulk_in();
}

/* The bulk TX/RX loop, run by the USB thread once the data path is up. Keeps one
 * bulk-IN outstanding; drains the TX queue onto bulk-OUT one frame at a time;
 * routes completions by endpoint id. */
static void data_loop(void)
{
	/* Reclaim the global EP0 ring for our device — other enumerations clobbered
	 * it after we were brought up. From here on no enumeration runs (data mode),
	 * so control transfers below safely target this device's endpoint 0. */
	ep0_ring = net_ep0;

	post_bulk_in();                  /* arm RX + status here, after enumeration  */
	post_bulk_intr();
	for (;;) {
		/* TX: post one queued frame if the OUT ring is free. */
		if (!net_out_pending && txq_tail != txq_head) {
			post_bulk_out(txq[txq_tail], txq_len[txq_tail]);
			txq_tail = (txq_tail + 1) % TXQ_SLOTS;
		}
		if (!net_intr_pending)           /* keep the status-IN armed            */
			post_bulk_intr();
		int resp_avail = 0;
		/* Drain completion events, routing by endpoint. */
		struct trb *e;
		while ((e = event_poll())) {
			if (TRB_GET_TYPE(e->control) != TRB_TRANSFER_EVENT)
				continue;
			int epid = (e->control >> 16) & 0x1F;
			int cc   = (e->status >> 24) & 0xFF;
			if (epid == net_in_dci) {
				int residual = e->status & 0xFFFFFF;
				int got = 2048 - residual;
				/* DIAG: report the first few inbound transfers (cc, length,
				 * RNDIS message type) so a stuck phone path is debuggable. */
				if (dbg_rx < 4) {
					dbg_rx++;
					vga_write("usbnet: rx cc="); vga_write_dec(cc);
					vga_write(" len="); vga_write_dec(got);
					vga_write(" type="); put_hex(le32(net_rx_buf), 8);
					vga_putc('\n');
				}
				if (cc == CC_STALL || cc == CC_BABBLE || cc == CC_USB_TXN_ERR) {
					/* These halt the endpoint — reset it and retry rather than
					 * going permanently deaf. */
					recover_in_stall();
				} else {
					/* Unwrap the RNDIS packet message -> Ethernet frame. */
					if (got >= RNDIS_HDR &&
					    le32(net_rx_buf) == RNDIS_PACKET_MSG) {
						uint32_t off = 8 + le32(net_rx_buf + 8);
						uint32_t dl  = le32(net_rx_buf + 12);
						if (off + dl <= (uint32_t)got)
							net_rx_frame(net_rx_buf + off, dl);
					}
					net_stall_retries = 0;       /* a good transfer resets it    */
					net_in_pending = 0;
					post_bulk_in();
				}
			} else if (epid == net_out_dci) {
				if (dbg_tx < 2) {
					dbg_tx++;
					vga_write("usbnet: tx cc="); vga_write_dec(cc); vga_putc('\n');
				}
				net_out_pending = 0;
			} else if (epid == net_intr_dci) {
				if (dbg_intr < 4) {
					dbg_intr++;
					vga_write("usbnet: intr cc="); vga_write_dec(cc);
					vga_write(" note="); put_hex(le32(net_intr_buf), 8);
					vga_putc('\n');
				}
				net_intr_pending = 0;
				if (cc == CC_SUCCESS || cc == CC_SHORT_PKT)
					resp_avail = 1;       /* a notification is waiting          */
			}
		}
		/* A status notification means an RNDIS message is queued — read it (the
		 * event ring is now drained, so this control transfer's completion comes
		 * back cleanly). Draining it is what unblocks an Android gadget. */
		if (resp_avail) {
			uint8_t r[64];
			control_in(net_slot, 0xA1, 0x01, 0, net_ctrl_iface, r, sizeof r);
		}
		timer_sleep(1);
	}
}

/* Walk a configuration descriptor blob and print its interfaces + endpoints, so
 * lsusb shows what each device actually exposes (class/subclass/protocol and the
 * bulk/interrupt endpoints a driver would use). */
static void print_config(const uint8_t *cfg, int total)
{
	for (int i = 0; i + 2 <= total; ) {
		uint8_t blen = cfg[i];
		if (blen < 2)
			break;
		uint8_t btype = cfg[i + 1];
		if (btype == 0x04 && i + 9 <= total) {            /* interface         */
			vga_write("         if "); vga_write_dec(cfg[i + 2]);
			vga_write(" class "); put_hex(cfg[i + 5], 2);
			vga_putc('/'); put_hex(cfg[i + 6], 2);
			vga_putc('/'); put_hex(cfg[i + 7], 2);
			vga_putc('\n');
		} else if (btype == 0x05 && i + 7 <= total) {     /* endpoint          */
			uint8_t addr = cfg[i + 2];
			uint8_t attr = cfg[i + 3] & 0x3;
			const char *t = attr == 2 ? "bulk" : attr == 3 ? "intr"
			              : attr == 1 ? "isoc" : "ctrl";
			vga_write("           ep "); put_hex(addr, 2);
			vga_write((addr & 0x80) ? " IN  " : " OUT ");
			vga_write(t); vga_putc('\n');
		}
		i += blen;
	}
}

/* Enable a slot for the device on `port`, address it, read its device
 * descriptor, and print vendor:product + class. One device at a time; failures
 * are reported but don't abort the caller's scan over the other ports.
 * (Milestone 1: the per-device contexts/rings are not yet freed — the restructure
 * to a persistent device model for bulk transfers will own that lifecycle.) */
/* Enable a slot for the device on `port`, address it, read its descriptors,
 * SET_CONFIGURATION it, and cache the result in port_dev[port]. No printing —
 * printing is separate (print_devs) so the boot pass stays silent. */
static void enumerate_device(uint32_t port, int speed)
{
	struct usbdev *d = &port_dev[port];
	d->active = 0;

	ring_push(&cmd_ring, 0, 0, 0, TRB_TYPE(TRB_ENABLE_SLOT));
	db[0] = 0;
	struct trb *e = event_wait();
	if (!e || TRB_GET_TYPE(e->control) != TRB_CMD_COMPLETION ||
	    ((e->status >> 24) & 0xFF) != CC_SUCCESS)
		return;
	int slot_id = (e->control >> 24) & 0xFF;

	if (address_device(slot_id, (int)port, speed) != 0)
		return;

	uint8_t desc[18];
	/* Two-step read: address_device set EP0's max packet to a speed-based guess
	 * (8 for full/low speed). Read the first 8 bytes — always one packet, so it
	 * works regardless — to learn the device's real bMaxPacketSize0 (byte 7); if
	 * it differs, fix EP0 with Evaluate Context before reading the full 18.
	 * SuperSpeed encodes it as an exponent (always 512), so skip those. */
	if (control_in(slot_id, 0x80, 0x06, 0x0100, 0, desc, 8) != 8)
		return;
	if (speed < 4) {
		uint32_t guessed = (speed == 3) ? 64 : 8;
		uint32_t real    = desc[7] ? desc[7] : 8;
		if (real != guessed)
			eval_context_mps(slot_id, real);
	}
	if (control_in(slot_id, 0x80, 0x06, 0x0100, 0, desc, 18) != 18)
		return;

	d->slot  = slot_id;
	d->speed = speed;
	d->vid   = desc[8]  | (desc[9]  << 8);
	d->pid   = desc[10] | (desc[11] << 8);
	d->cls   = desc[4];
	d->cfg_len = 0;

	/* Read the configuration descriptor (9-byte header for wTotalLength, then the
	 * whole thing), cache it, and select the configuration. SET_CONFIGURATION is
	 * what makes a phone offer USB tethering. */
	if (control_in(slot_id, 0x80, 0x06, 0x0200, 0, d->cfg, 9) == 9) {
		int total = d->cfg[2] | (d->cfg[3] << 8);
		if (total > (int)sizeof d->cfg)
			total = sizeof d->cfg;
		if (control_in(slot_id, 0x80, 0x06, 0x0200, 0, d->cfg, total) == total) {
			d->cfg_len = total;
			control_no_data(slot_id, 0x00, 0x09, d->cfg[5], 0); /* SET_CONFIG  */
		}
	}
	d->active = 1;

	/* If it's a USB-Ethernet/RNDIS device, run the RNDIS control handshake now
	 * (over EP0 — the global ep0_ring is still this device's, mid-enumeration) to
	 * INITIALIZE it and learn its MAC. Bulk data path comes next. */
	d->is_net = 0;
	d->mac_ok = 0;
	parse_net(d);
	if (d->is_net && rndis_setup(d) == 0 && !net_up)
		bring_up_net(d, (int)port);   /* filter + bulk endpoints + register NIC  */
}

/* Print the cached device table: one summary line per device, plus full
 * interface/endpoint detail for the one port the caller asked about. */
static void print_devs(int detail_port)
{
	int any = 0;
	for (uint32_t p = 1; p <= max_ports; p++) {
		struct usbdev *d = &port_dev[p];
		if (!d->active)
			continue;
		any = 1;
		vga_write("xhci: port "); vga_write_dec(p); vga_write(": ");
		put_hex(d->vid, 4); vga_putc(':'); put_hex(d->pid, 4);
		vga_write(" class "); put_hex(d->cls, 2);
		vga_write(" speed "); vga_write_dec(d->speed);
		if (d->mac_ok)                 /* only devices RNDIS actually answered  */
			vga_write(" [net]");
		vga_putc('\n');
		if (detail_port == (int)p) {
			if (d->cfg_len)
				print_config(d->cfg, d->cfg_len);
			if (d->is_net && d->mac_ok) {
				vga_write("         rndis mac ");
				for (int i = 0; i < 6; i++) {
					put_hex(d->mac[i], 2);
					if (i < 5) vga_putc(':');
				}
				vga_putc('\n');
			} else if (d->is_net) {
				vga_write("         rndis: init failed\n");
			}
		}
	}
	if (!any)
		vga_write("xhci: no usable device on any port\n");
}

/* Bring the controller out of reset and stand up its rings — ONCE. Kept separate
 * from port enumeration so OwOS can come up as a persistent USB host at boot and
 * stay running, rather than resetting the controller on every `lsusb` (which
 * looked like a disconnect/reconnect storm to a tethered phone). Returns 0 if the
 * controller is up, -1 if none was found. */
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
		vga_write("xhci: controller reset timed out\n");
		return -1;
	}

	/* Device context base array; tell the controller how many slots to enable. */
	dcbaa = palloc();
	setup_scratchpad();
	w64(op, XHCI_DCBAAP, (uint32_t)dcbaa);
	w32(op, XHCI_CONFIG, max_slots);

	/* Command ring. CRCR points at it with the ring cycle state (RCS=1). */
	ring_init(&cmd_ring);
	w64(op, XHCI_CRCR, (uint32_t)cmd_ring.base | 1);

	/* Event ring: one segment + a one-entry segment table. */
	event_ring  = palloc();
	event_cycle = TRB_CYCLE;
	event_idx   = 0;
	uint32_t *erst = palloc();
	erst[0] = (uint32_t)event_ring;     /* segment base low                    */
	erst[1] = 0;                        /* segment base high                   */
	erst[2] = RING_TRBS;                /* segment size                        */
	erst[3] = 0;
	w32(rt, XHCI_ERSTSZ, 1);
	w64(rt, XHCI_ERDP, (uint32_t)event_ring);
	w64(rt, XHCI_ERSTBA, (uint32_t)erst);

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

/* Bring the port table up to date: enumerate a port only when it's newly
 * connected or its connect-status changed (CSC) — e.g. a phone re-attaching as
 * RNDIS when tethering is toggled. Stable devices keep their cached entry, so we
 * never re-address a configured device (which fails) or disrupt a live tether. */
static void scan_ports(void)
{
	for (uint32_t p = 1; p <= max_ports && p < XHCI_MAX_PORTS; p++) {
		uint32_t sc = r32(op, XHCI_PORTSC(p));
		int changed = (sc & PORT_CSC) != 0;
		if (changed)                       /* ack the change (write-1-to-clear) */
			w32(op, XHCI_PORTSC(p),
			    (sc & ~PORT_RWC_MASK & ~PORT_PED) | PORT_CSC);

		if (sc & PORT_CCS) {
			if (!port_dev[p].active || changed) {
				int speed = reset_port(p);
				if (speed)
					enumerate_device(p, speed);
				else
					port_dev[p].active = 0;
			}
		} else {
			port_dev[p].active = 0;        /* device gone                       */
		}
	}
}

/* The one place ports are scanned after boot, so the xHCI rings are only ever
 * driven from a single context (no locking needed): a phone toggling tethering
 * re-attaches as RNDIS, and this notices the connect-change and reconfigures it
 * within a poll interval — fast enough that the phone's tethering switch engages
 * instead of timing out and snapping back off. `lsusb` only reads the cache. */
static void usb_poll_thread(void)
{
	for (;;) {
		if (net_up)
			data_loop();           /* once up, never rescan (would reset ports) */
		scan_ports();
		timer_sleep(250);
	}
}

/* `lsusb`: just print the cached device table — the poll thread keeps it fresh,
 * so we must NOT scan here (that would drive the rings from two contexts). */
int xhci_init(int detail_port)
{
	if (!setup_done)
		return 0;
	print_devs(detail_port);
	return 1;
}

/* Boot-time: bring the host up, configure attached devices once, then hand all
 * further scanning to a background thread so OwOS stays a live, responsive USB
 * host — a tethered phone sees a stable computer that reconfigures it promptly. */
void xhci_boot(void)
{
	if (setup_controller() != 0)
		return;
	scan_ports();                  /* configure whatever's already attached       */
	sched_create_thread("usbpoll", usb_poll_thread);
}
