/* ehci.c: EHCI (USB 2.0) host controller driver. High-speed devices on the root ports and anything behind a
 * high-speed hub (full- and low-speed devices there through the hub's transaction translator, with split
 * transactions), with control, interrupt and bulk transfers. A full- or low-speed device on a root port is handed to
 * the companion controller (UHCI). No interrupts: the poll thread checks the armed qTDs every millisecond.
 *
 * Control and bulk QHs sit on the async ring behind a halted dummy head; interrupt QHs form one chain that every
 * frame-list entry points at, so interrupt endpoints are polled once a frame (bInterval is a maximum latency). DMA
 * memory comes from palloc (identity-mapped, below 4 GiB): CTRLDSSEGMENT stays 0 and the 64-bit structure extensions
 * stay zero. */

#include "ehci.h"
#include "console.h"
#include "heap.h"
#include "kstring.h"
#include "paging.h"
#include "pci.h"
#include "timer.h"
#include "usbhc.h"
#include <stdbool.h>
#include <stdint.h>

#define EHCI_MAX_HCS USBH_EHCI_MAX_HCS
#define EHCI_ADDRS USBH_ADDRS
#define EHCI_MAX_PORTS 15

#define CAP_LENGTH 0x00
#define CAP_HCSPARAMS 0x04
#define CAP_HCCPARAMS 0x08

#define OP_CMD 0x00
#define OP_STS 0x04
#define OP_INTR 0x08
#define OP_FRINDEX 0x0C
#define OP_SEGMENT 0x10
#define OP_PERIODIC 0x14
#define OP_ASYNC 0x18
#define OP_CONFIG 0x40
#define OP_PORTSC(p) (0x44 + 4 * ((p) - 1))

#define CMD_RUN (1u << 0)
#define CMD_RESET (1u << 1)
#define CMD_PSE (1u << 4)
#define CMD_ASE (1u << 5)
#define CMD_IAAD (1u << 6)
#define CMD_ITC_8 (8u << 16)

#define STS_EVENTS 0x0F /* USBINT, USBERRINT, port change, frame list rollover */
#define STS_HSE (1u << 4)
#define STS_IAA (1u << 5)
#define STS_HALTED (1u << 12)

#define HCS_PORTS(h) ((h) & 0xF)
#define HCS_PPC (1u << 4)
#define HCS_COMPANIONS(h) (((h) >> 12) & 0xF)
#define HCC_64BIT (1u << 0)
#define HCC_EECP(h) (((h) >> 8) & 0xFF)

#define PORT_CONNECT (1u << 0)
#define PORT_CSC (1u << 1)
#define PORT_PE (1u << 2)
#define PORT_PEC (1u << 3)
#define PORT_OCC (1u << 5)
#define PORT_RESET (1u << 8)
#define PORT_LINE (3u << 10)
#define PORT_LINE_K (1u << 10) /* a low-speed device idles in K */
#define PORT_POWER (1u << 12)
#define PORT_OWNER (1u << 13)
#define PORT_RWC (PORT_CSC | PORT_PEC | PORT_OCC)

/* PCI legacy support capability (USBLEGSUP) and its control/status dword. */
#define LEGSUP_ID 1
#define LEGSUP_BIOS (1u << 16)
#define LEGSUP_OS (1u << 24)
#define LEGCTL_HANDOFF 0xE0000000u /* SMI enables off, R/WC SMI events acked */

#define TOK_MISSED (1u << 2)
#define TOK_XACT (1u << 3)
#define TOK_BABBLE (1u << 4)
#define TOK_BUFFER (1u << 5)
#define TOK_HALTED (1u << 6)
#define TOK_ACTIVE (1u << 7)
#define TOK_OUT (0u << 8)
#define TOK_IN (1u << 8)
#define TOK_SETUP (2u << 8)
#define TOK_CERR3 (3u << 10)
#define TOK_IOC (1u << 15)
#define TOK_BYTES(n) ((uint32_t)(n) << 16)
#define TOK_LEFT(t) (((t) >> 16) & 0x7FFF)
#define TOK_DT (1u << 31)
#define TOK_FAULTS (TOK_MISSED | TOK_XACT | TOK_BABBLE | TOK_BUFFER)

#define LINK_TERM 1u
#define LINK_QH (1u << 1)

#define EPS_FULL 0u
#define EPS_LOW 1u
#define EPS_HIGH 2u
#define CHR_DTC (1u << 14)
#define CHR_HEAD (1u << 15)
#define CHR_CONTROL (1u << 27)
#define CHR_NAK_RELOAD(n) ((uint32_t)(n) << 28)
#define CAP_MULT1 (1u << 30)
/* Interrupt split transactions: start-split in microframe 0, complete-splits in 2, 3 and 4. */
#define CAP_SPLIT_SMASK 0x01u
#define CAP_SPLIT_CMASK (0x1Cu << 8)

#define CTRL_TIMEOUT_MS 1000
#define SCAN_MS 100
#define PIPE_MAX_ERRORS 8
#define XFER_MAX 4096
#define PIPE_BUF_OFF 1024 /* interrupt/bulk IN buffer within the pipe page, room for a 1024-byte packet */

/* qTD and QH sized for the 64-bit layout (the extended buffer dwords stay zero), so either controller kind reads
 * them the same. */
struct qtd {
  volatile uint32_t next;
  volatile uint32_t alt;
  volatile uint32_t token;
  volatile uint32_t buf[5];
  volatile uint32_t buf_hi[5];
  uint32_t pad[3];
};

struct qh {
  volatile uint32_t link;
  volatile uint32_t chars;
  volatile uint32_t caps;
  volatile uint32_t current;
  volatile uint32_t next; /* transfer overlay: a live copy of the current qTD */
  volatile uint32_t alt;
  volatile uint32_t token;
  volatile uint32_t buf[5];
  volatile uint32_t buf_hi[5];
  uint32_t pad[7];
};

_Static_assert(sizeof(struct qtd) == 64, "qTDs are laid out 64 bytes apart");
_Static_assert(sizeof(struct qh) == 96, "QHs are laid out 96 bytes apart");

struct ehc;
struct epipe;

struct edev {
  struct usbdev pub;
  struct ehc *hc;
  uint8_t addr; /* 0 until SET_ADDRESS */
  uint8_t mps0;
  uint8_t *ep0_page;
  struct qh *ep0;
  struct epipe *pipes;
};

/* An interrupt or bulk pipe: its QH, its one IN qTD and that qTD's buffer share a page. */
struct epipe {
  struct usbh_pipe base;
  struct epipe *next;
  struct edev *dev;
  uint8_t *page;
  struct qh *qh;
  struct qtd *td;
  uint8_t *buf;
  uint8_t ep_addr;
  uint16_t mps;
  bool out, periodic, dead, armed;
  uint8_t errors;
  uint32_t retry_at;
  usbh_pipe_cb cb;
  void *ctx;
};

struct ehc {
  volatile uint8_t *op;
  uint8_t bus, slot, func;
  int index, nports, ncompanions;
  uint32_t *frames;
  uint8_t *skel; /* page holding the async ring's dummy head */
  struct qh *async_head;
  struct edev *devs[EHCI_ADDRS];
  int last_addr;
  int root[EHCI_MAX_PORTS + 1];
  bool port_failed[EHCI_MAX_PORTS + 1];
  uint32_t companion_ports;
  bool halt_reported;
};

static struct ehc *hcs[EHCI_MAX_HCS];
static int nhcs;
static uint32_t last_scan;
static const struct usbh_hc_ops ehci_ops;

static uint32_t phys(const volatile void *p) { return (uint32_t)(uintptr_t)p; }
static struct qh *qh_at(uint32_t link) { return (struct qh *)(uintptr_t)(link & ~0x1Fu); }
static uint32_t rd(struct ehc *hc, uint32_t off) { return *(volatile uint32_t *)(hc->op + off); }
static void wr(struct ehc *hc, uint32_t off, uint32_t v) { *(volatile uint32_t *)(hc->op + off) = v; }

static uint32_t elapsed_ms(uint32_t start) { return (timer_ticks() - start) * 1000u / TIMER_HZ; }

static int dev_id(const struct ehc *hc, int addr) { return USBH_EHCI_FIRST_ID + hc->index * EHCI_ADDRS + addr; }

static struct edev *find_dev(int id)
{
  int rel = id - USBH_EHCI_FIRST_ID;
  if (rel < 0 || rel / EHCI_ADDRS >= nhcs)
    return NULL;
  return hcs[rel / EHCI_ADDRS]->devs[rel % EHCI_ADDRS];
}

/* Endpoint characteristics: address, endpoint, speed, max packet; control endpoints take their toggle from the qTDs,
 * and a full/low-speed one is flagged for the transaction translator. Periodic and split QHs must not reload NAKs. */
static uint32_t qh_chars(const struct edev *u, uint8_t ep, uint16_t mps, bool control, bool periodic)
{
  int speed = u->pub.speed;
  uint32_t eps = speed == USB_SPEED_HIGH ? EPS_HIGH : speed == USB_SPEED_LOW ? EPS_LOW : EPS_FULL;
  uint32_t c = u->addr | ((uint32_t)(ep & 0xF) << 8) | (eps << 12) | ((uint32_t)(mps & 0x7FF) << 16);
  if (control) {
    c |= CHR_DTC;
    if (speed != USB_SPEED_HIGH)
      c |= CHR_CONTROL;
  }
  if (speed == USB_SPEED_HIGH && !periodic)
    c |= CHR_NAK_RELOAD(4);
  return c;
}

/* Endpoint capabilities: one transaction per microframe, the polling microframes of a periodic QH, and for a
 * full/low-speed device the high-speed hub (address and port) whose transaction translator reaches it. */
static uint32_t qh_caps(const struct edev *u, bool periodic)
{
  uint32_t c = CAP_MULT1;
  if (u->pub.speed == USB_SPEED_HIGH)
    return periodic ? c | CAP_SPLIT_SMASK : c;
  struct edev *tt = find_dev(u->pub.tt_slot);
  c |= ((uint32_t)(tt ? tt->addr : 0) << 16) | ((uint32_t)(u->pub.tt_port & 0x7F) << 23);
  return periodic ? c | CAP_SPLIT_SMASK | CAP_SPLIT_CMASK : c;
}

static void qh_init(struct qh *qh, uint32_t chars, uint32_t caps)
{
  memset((void *)qh, 0, sizeof *qh);
  qh->link = LINK_TERM;
  qh->chars = chars;
  qh->caps = caps;
  qh->next = LINK_TERM;
  qh->alt = LINK_TERM;
}

/* Empty an idle or halted QH's overlay; the toggle survives unless the device's was reset too (CLEAR_FEATURE). */
static void qh_reset(struct qh *qh, bool keep_toggle)
{
  qh->next = LINK_TERM;
  qh->alt = LINK_TERM;
  for (int i = 0; i < 5; i++)
    qh->buf[i] = 0;
  __sync_synchronize();
  qh->token = keep_toggle ? qh->token & TOK_DT : 0;
}

static void async_link(struct ehc *hc, struct qh *qh)
{
  qh->link = hc->async_head->link;
  __sync_synchronize();
  hc->async_head->link = phys(qh) | LINK_QH;
}

static void async_remove(struct ehc *hc, struct qh *qh)
{
  uint32_t me = phys(qh) | LINK_QH;
  for (struct qh *q = hc->async_head;;) {
    if (q->link == me) {
      q->link = qh->link;
      return;
    }
    q = qh_at(q->link);
    if (q == hc->async_head)
      return;
  }
}

/* Ring the async-advance doorbell and wait for it: the controller no longer holds a QH removed before the call. */
static void async_advance(struct ehc *hc)
{
  wr(hc, OP_STS, STS_IAA);
  wr(hc, OP_CMD, rd(hc, OP_CMD) | CMD_IAAD);
  uint32_t start = timer_ticks();
  while (!(rd(hc, OP_STS) & STS_IAA) && elapsed_ms(start) < 20)
    timer_sleep(1);
  wr(hc, OP_STS, STS_IAA);
}

static void periodic_head(struct ehc *hc, uint32_t link)
{
  for (int i = 0; i < 1024; i++)
    hc->frames[i] = link;
}

static void periodic_link(struct ehc *hc, struct qh *qh)
{
  qh->link = hc->frames[0];
  __sync_synchronize();
  periodic_head(hc, phys(qh) | LINK_QH);
}

static void periodic_remove(struct ehc *hc, struct qh *qh)
{
  uint32_t me = phys(qh) | LINK_QH;
  if (hc->frames[0] == me) {
    periodic_head(hc, qh->link);
    return;
  }
  for (uint32_t l = hc->frames[0]; !(l & LINK_TERM); l = qh_at(l)->link)
    if (qh_at(l)->link == me) {
      qh_at(l)->link = qh->link;
      return;
    }
}

/* Wait two frames, so the controller no longer walks a QH taken off the periodic chain before the call. */
static void periodic_wait(struct ehc *hc)
{
  uint32_t f = rd(hc, OP_FRINDEX);
  uint32_t start = timer_ticks();
  while (((rd(hc, OP_FRINDEX) - f) & 0x3FFF) < 16 && elapsed_ms(start) < 10)
    timer_sleep(1);
}

static void qh_unlink(struct ehc *hc, struct qh *qh, bool periodic)
{
  if (periodic) {
    periodic_remove(hc, qh);
    periodic_wait(hc);
  } else {
    async_remove(hc, qh);
    async_advance(hc);
  }
}

/* Hand the qTD chain starting at `first` to an idle QH and wait for `last` to retire. 0 on success, -1 on an error or
 * timeout; the QH is left idle either way. */
static int run_qtds(struct ehc *hc, struct qh *qh, bool periodic, struct qtd *first, struct qtd *last)
{
  qh->alt = LINK_TERM;
  qh->token &= TOK_DT;
  __sync_synchronize();
  qh->next = phys(first);
  uint32_t start = timer_ticks();
  for (;;) {
    uint32_t tok = last->token;
    if (!(tok & TOK_ACTIVE) || (qh->token & TOK_HALTED)) {
      if (!(tok & TOK_HALTED) && !(qh->token & TOK_HALTED))
        return 0;
      qh_reset(qh, true);
      return -1;
    }
    if (elapsed_ms(start) > CTRL_TIMEOUT_MS)
      break;
    timer_sleep(1);
  }
  /* Still running: take the QH off the schedule so the controller lets go of the qTDs before they are freed. */
  qh_unlink(hc, qh, periodic);
  qh_reset(qh, true);
  if (periodic)
    periodic_link(hc, qh);
  else
    async_link(hc, qh);
  return -1;
}

static bool stalled(uint32_t token) { return (token & TOK_HALTED) && !(token & TOK_FAULTS); }

/* One control transfer on endpoint 0: SETUP, one DATA qTD (a page holds the whole stage), STATUS. A short IN reply
 * jumps to STATUS through the data qTD's alternate link and is zero-filled up to len. Returns len or -1. */
static int control(struct edev *u, uint8_t req_type, uint8_t req, uint16_t value, uint16_t index, uint8_t *data,
                   int len)
{
  bool in = req_type & 0x80;
  if (len < 0 || len > XFER_MAX)
    return -1;
  uint8_t *page = palloc();
  uint8_t *buf = len ? palloc() : NULL;
  if (!page || (len && !buf)) {
    if (page)
      pfree(page);
    if (buf)
      pfree(buf);
    return -1;
  }
  struct qtd *setup_td = (struct qtd *)page, *data_td = setup_td + 1, *status_td = setup_td + 2;
  uint8_t *setup = page + 256;
  setup[0] = req_type;
  setup[1] = req;
  setup[2] = (uint8_t)value;
  setup[3] = (uint8_t)(value >> 8);
  setup[4] = (uint8_t)index;
  setup[5] = (uint8_t)(index >> 8);
  setup[6] = (uint8_t)len;
  setup[7] = (uint8_t)(len >> 8);
  if (!in && len)
    memcpy(buf, data, (uint32_t)len);

  setup_td->next = phys(len ? data_td : status_td);
  setup_td->alt = LINK_TERM;
  setup_td->buf[0] = phys(setup);
  setup_td->token = TOK_ACTIVE | TOK_SETUP | TOK_CERR3 | TOK_BYTES(8);
  if (len) {
    data_td->next = phys(status_td);
    data_td->alt = phys(status_td);
    data_td->buf[0] = phys(buf);
    data_td->token = TOK_ACTIVE | (in ? TOK_IN : TOK_OUT) | TOK_CERR3 | TOK_BYTES(len) | TOK_DT;
  }
  status_td->next = LINK_TERM;
  status_td->alt = LINK_TERM;
  status_td->token = TOK_ACTIVE | (in && len ? TOK_OUT : TOK_IN) | TOK_CERR3 | TOK_IOC | TOK_DT;

  int rc = run_qtds(u->hc, u->ep0, false, setup_td, status_td);
  if (rc == 0 && in && len) {
    int got = len - (int)TOK_LEFT(data_td->token);
    if (got < 0)
      got = 0;
    memcpy(data, buf, (uint32_t)got);
    memset(data + got, 0, (uint32_t)(len - got));
  }
  pfree(page);
  if (buf)
    pfree(buf);
  return rc < 0 ? -1 : len;
}

static int eh_control_in(int id, uint8_t req_type, uint8_t req, uint16_t value, uint16_t index, uint8_t *out, int len)
{
  struct edev *u = find_dev(id);
  return u ? control(u, req_type, req, value, index, out, len) : -1;
}

static int eh_control_out(int id, uint8_t req_type, uint8_t req, uint16_t value, uint16_t index, const uint8_t *data,
                          int len)
{
  struct edev *u = find_dev(id);
  return u && control(u, req_type, req, value, index, (uint8_t *)data, len) >= 0 ? 0 : -1;
}

static int eh_control_nodata(int id, uint8_t req_type, uint8_t req, uint16_t value, uint16_t index)
{
  struct edev *u = find_dev(id);
  return u && control(u, req_type, req, value, index, NULL, 0) >= 0 ? 0 : -1;
}

static void pipe_arm(struct epipe *p)
{
  struct qtd *td = p->td;
  td->next = LINK_TERM;
  td->alt = LINK_TERM;
  td->buf[0] = phys(p->buf);
  __sync_synchronize();
  td->token = TOK_ACTIVE | TOK_IN | TOK_CERR3 | TOK_BYTES(p->mps);
  __sync_synchronize();
  p->qh->next = phys(td);
  p->armed = true;
}

/* A finished IN qTD: hand the data to the driver and re-arm, or back off after an error. A device that was unplugged
 * errors here until its hub or root port reports the disconnect, so retries are spaced out. */
static void pipe_poll(struct epipe *p)
{
  if (p->out || p->dead)
    return;
  uint32_t now = timer_ticks();
  if (!p->armed) {
    if ((int32_t)(now - p->retry_at) >= 0)
      pipe_arm(p);
    return;
  }
  uint32_t tok = p->td->token;
  if (tok & TOK_ACTIVE)
    return;
  p->armed = false;
  if (tok & TOK_HALTED) {
    bool stall = stalled(tok);
    qh_reset(p->qh, !stall);
    if (++p->errors > PIPE_MAX_ERRORS) {
      p->dead = true;
      con_printf("usb: slot %d ep %02x gave up after repeated errors\n", p->dev->pub.slot, p->ep_addr);
      return;
    }
    if (stall)
      control(p->dev, 0x02, 0x01, 0, p->ep_addr, NULL, 0);
    p->retry_at = now + (uint32_t)p->errors * 10u * TIMER_HZ / 1000u + 1;
    return;
  }
  p->errors = 0;
  int got = p->mps - (int)TOK_LEFT(tok);
  p->cb(p->ctx, p->buf, got < 0 ? 0 : got);
  if (!p->dead)
    pipe_arm(p);
}

static uint16_t max_packet(const struct edev *u, bool periodic)
{
  if (u->pub.speed == USB_SPEED_HIGH)
    return periodic ? 1024 : 512;
  return u->pub.speed == USB_SPEED_LOW ? 8 : 64;
}

static struct usbh_pipe *open_pipe(int id, uint8_t ep_addr, uint16_t mps, bool out, bool periodic, usbh_pipe_cb cb,
                                   void *ctx)
{
  struct edev *u = find_dev(id);
  if (!u || mps == 0 || mps > max_packet(u, periodic) || out != !(ep_addr & 0x80) || (!out && !cb))
    return NULL;
  if (!periodic && u->pub.speed == USB_SPEED_LOW)
    return NULL; /* low-speed devices have no bulk endpoints */
  struct epipe *p = kmalloc(sizeof *p);
  if (!p)
    return NULL;
  memset(p, 0, sizeof *p);
  p->page = palloc();
  if (!p->page) {
    kfree(p);
    return NULL;
  }
  p->base.ops = &ehci_ops;
  p->dev = u;
  p->qh = (struct qh *)p->page;
  p->td = (struct qtd *)(p->page + 128);
  p->buf = p->page + PIPE_BUF_OFF;
  p->ep_addr = ep_addr;
  p->mps = mps;
  p->out = out;
  p->periodic = periodic;
  p->cb = cb;
  p->ctx = ctx;
  qh_init(p->qh, qh_chars(u, ep_addr, mps, false, periodic), qh_caps(u, periodic));
  if (periodic)
    periodic_link(u->hc, p->qh);
  else
    async_link(u->hc, p->qh);
  p->next = u->pipes;
  u->pipes = p;
  if (!out)
    pipe_arm(p);
  return &p->base;
}

static struct usbh_pipe *eh_open_intr(int id, uint8_t ep_addr, uint16_t mps, uint8_t interval, usbh_pipe_cb cb,
                                      void *ctx)
{
  (void)interval;
  return open_pipe(id, ep_addr, mps, false, true, cb, ctx);
}

static struct usbh_pipe *eh_open_intr_out(int id, uint8_t ep_addr, uint16_t mps, uint8_t interval)
{
  (void)interval;
  return open_pipe(id, ep_addr, mps, true, true, NULL, NULL);
}

static struct usbh_pipe *eh_open_bulk_in(int id, uint8_t ep_addr, uint16_t mps, usbh_pipe_cb cb, void *ctx)
{
  return open_pipe(id, ep_addr, mps, false, false, cb, ctx);
}

static struct usbh_pipe *eh_open_bulk_out(int id, uint8_t ep_addr, uint16_t mps)
{
  return open_pipe(id, ep_addr, mps, true, false, NULL, NULL);
}

static int eh_pipe_send(struct usbh_pipe *bp, const uint8_t *data, int len)
{
  struct epipe *p = (struct epipe *)bp;
  if (!p->out || p->dead || len <= 0 || len > XFER_MAX)
    return -1;
  uint8_t *page = palloc();
  uint8_t *buf = palloc();
  if (!page || !buf) {
    if (page)
      pfree(page);
    if (buf)
      pfree(buf);
    return -1;
  }
  memcpy(buf, data, (uint32_t)len);
  struct qtd *td = (struct qtd *)page;
  td->next = LINK_TERM;
  td->alt = LINK_TERM;
  td->buf[0] = phys(buf);
  td->token = TOK_ACTIVE | TOK_OUT | TOK_CERR3 | TOK_IOC | TOK_BYTES(len);
  int rc = run_qtds(p->dev->hc, p->qh, p->periodic, td, td);
  if (rc < 0 && stalled(td->token)) {
    qh_reset(p->qh, false);
    control(p->dev, 0x02, 0x01, 0, p->ep_addr, NULL, 0);
  }
  pfree(page);
  pfree(buf);
  return rc;
}

static void free_pipe(struct epipe *p)
{
  pfree(p->page);
  kfree(p);
}

static void eh_close_pipe(struct usbh_pipe *bp)
{
  struct epipe *p = (struct epipe *)bp;
  struct edev *u = p->dev;
  p->dead = true;
  qh_unlink(u->hc, p->qh, p->periodic);
  for (struct epipe **pp = &u->pipes; *pp; pp = &(*pp)->next)
    if (*pp == p) {
      *pp = p->next;
      break;
    }
  free_pipe(p);
}

/* Take a device's QHs off both schedules with one wait each, then free them. */
static void free_endpoints(struct edev *u)
{
  struct ehc *hc = u->hc;
  bool any_periodic = false;
  for (struct epipe *p = u->pipes; p; p = p->next) {
    if (p->periodic) {
      periodic_remove(hc, p->qh);
      any_periodic = true;
    } else {
      async_remove(hc, p->qh);
    }
  }
  if (u->ep0)
    async_remove(hc, u->ep0);
  async_advance(hc);
  if (any_periodic)
    periodic_wait(hc);
  while (u->pipes) {
    struct epipe *p = u->pipes;
    u->pipes = p->next;
    free_pipe(p);
  }
  if (u->ep0_page)
    pfree(u->ep0_page);
}

/* Drop a device: children first, class drivers, then its endpoints and address. */
static void remove_device(int id)
{
  struct edev *u = find_dev(id);
  if (!u)
    return;
  struct ehc *hc = u->hc;
  for (struct epipe *p = u->pipes; p; p = p->next)
    p->dead = true;
  for (int a = 1; a < EHCI_ADDRS; a++)
    if (hc->devs[a] && hc->devs[a]->pub.parent_slot == id)
      remove_device(dev_id(hc, a));
  con_printf("usb: slot %d detached\n", id);
  usbh_unbind_classes(id);
  free_endpoints(u);
  hc->devs[u->addr] = NULL;
  kfree(u->pub.cfg);
  kfree(u);
}

/* Point endpoint 0's QH at the device's current address and max packet size (off the ring while it changes). */
static void ep0_update(struct edev *u)
{
  async_remove(u->hc, u->ep0);
  async_advance(u->hc);
  u->ep0->chars = qh_chars(u, 0, u->mps0, true, false);
  u->ep0->caps = qh_caps(u, false);
  async_link(u->hc, u->ep0);
}

/* Address the device a reset port just enabled, read its descriptors and bind class drivers; returns its id or 0.
 * parent NULL means a root port. */
static int enumerate(struct ehc *hc, struct edev *parent, int parent_port, int root_port, int speed)
{
  if (parent && parent->pub.depth >= 5)
    return 0;
  /* Round-robin, so a device that failed after SET_ADDRESS does not share its address with the next one. */
  int addr = 0;
  for (int i = 1; i < EHCI_ADDRS && !addr; i++) {
    int a = (hc->last_addr + i - 1) % (EHCI_ADDRS - 1) + 1;
    if (!hc->devs[a])
      addr = a;
  }
  if (!addr) {
    con_write("ehci: out of device addresses\n");
    return 0;
  }
  hc->last_addr = addr;
  struct edev *u = kmalloc(sizeof *u);
  if (!u)
    return 0;
  memset(u, 0, sizeof *u);
  u->hc = hc;
  u->mps0 = speed == USB_SPEED_HIGH ? 64 : 8;
  struct usbdev *d = &u->pub;
  d->slot = dev_id(hc, addr);
  d->speed = speed;
  d->kbps = usb3_default_kbps(speed);
  d->root_port = (uint8_t)root_port;
  d->parent_slot = parent ? parent->pub.slot : 0;
  d->parent_port = (uint8_t)parent_port;
  d->depth = parent ? (uint8_t)(parent->pub.depth + 1) : 0;
  if (parent && speed != USB_SPEED_HIGH) {
    if (parent->pub.speed == USB_SPEED_HIGH) {
      d->tt_slot = parent->pub.slot;
      d->tt_port = parent_port;
    } else {
      d->tt_slot = parent->pub.tt_slot;
      d->tt_port = parent->pub.tt_port;
    }
  }
  u->ep0_page = palloc();
  if (!u->ep0_page) {
    kfree(u);
    return 0;
  }
  u->ep0 = (struct qh *)u->ep0_page;
  qh_init(u->ep0, qh_chars(u, 0, u->mps0, true, false), qh_caps(u, false));
  async_link(hc, u->ep0);

  uint8_t desc[18];
  if (control(u, 0x80, 0x06, 0x0100, 0, desc, 8) < 0)
    goto fail;
  if (speed == USB_SPEED_FULL && (desc[7] == 16 || desc[7] == 32 || desc[7] == 64))
    u->mps0 = desc[7];
  if (control(u, 0x00, 0x05, (uint16_t)addr, 0, NULL, 0) < 0)
    goto fail;
  timer_sleep(2); /* SET_ADDRESS recovery */
  u->addr = (uint8_t)addr;
  ep0_update(u);
  if (control(u, 0x80, 0x06, 0x0100, 0, desc, 18) < 0)
    goto fail;
  hc->devs[addr] = u;

  d->bcd_usb = desc[2] | (desc[3] << 8);
  d->cls = desc[4];
  d->vid = desc[8] | (desc[9] << 8);
  d->pid = desc[10] | (desc[11] << 8);
  usbh_read_product(d, desc[15]);
  usbh_read_config(d);
  d->active = true;
  con_printf("usb: slot %d %04x:%04x attached\n", d->slot, d->vid, d->pid);
  usbh_bind_classes(d);
  return d->slot;

fail:
  con_printf("ehci: device on port %d%s did not answer\n", parent ? parent_port : root_port, parent ? " of a hub" : "");
  free_endpoints(u);
  kfree(u);
  return 0;
}

/* Give a root port's full- or low-speed device to the companion controller; false if there is none. */
static bool hand_off(struct ehc *hc, int port, const char *what)
{
  if (!hc->ncompanions) {
    con_printf("ehci: %s device on port %d needs a companion controller\n", what, port);
    return false;
  }
  uint32_t s = rd(hc, OP_PORTSC(port));
  wr(hc, OP_PORTSC(port), (s & ~PORT_RWC) | PORT_OWNER);
  return true;
}

/* Reset a root port; returns USB_SPEED_HIGH if a high-speed device enabled, otherwise 0 (the device went to the
 * companion, or nothing answered). *handed_off says which. */
static int reset_port(struct ehc *hc, int port, bool *handed_off)
{
  uint32_t reg = OP_PORTSC(port);
  uint32_t s = rd(hc, reg);
  *handed_off = false;
  if ((s & PORT_LINE) == PORT_LINE_K) {
    *handed_off = hand_off(hc, port, "low-speed");
    return 0;
  }
  wr(hc, reg, (s & ~(PORT_RWC | PORT_PE)) | PORT_RESET);
  timer_sleep(50);
  wr(hc, reg, rd(hc, reg) & ~(PORT_RWC | PORT_RESET));
  uint32_t start = timer_ticks();
  while ((rd(hc, reg) & PORT_RESET) && elapsed_ms(start) < 10)
    timer_sleep(1);
  timer_sleep(2);
  s = rd(hc, reg);
  if (!(s & PORT_CONNECT))
    return 0;
  if (s & PORT_PE) {
    timer_sleep(10); /* reset recovery */
    return USB_SPEED_HIGH;
  }
  *handed_off = hand_off(hc, port, "full-speed");
  return 0;
}

static void scan_ports(struct ehc *hc)
{
  for (int p = 1; p <= hc->nports; p++) {
    uint32_t reg = OP_PORTSC(p);
    uint32_t s = rd(hc, reg);
    /* The companion runs the port until its device disconnects, which hands it back. */
    if (s & PORT_OWNER) {
      hc->companion_ports |= 1u << p;
      continue;
    }
    hc->companion_ports &= ~(1u << p);
    bool connected = s & PORT_CONNECT;
    bool changed = s & PORT_CSC;
    if (s & PORT_RWC)
      wr(hc, reg, s);
    /* An error can disable the port without a connect change; the device is gone either way. */
    if (hc->root[p] && (s & PORT_PEC) && !(s & PORT_PE))
      changed = true;
    int id = hc->root[p];
    if (connected && id && !changed)
      continue;
    if (id) {
      remove_device(id);
      hc->root[p] = 0;
    }
    if (!connected) {
      hc->port_failed[p] = false;
      continue;
    }
    if (hc->port_failed[p] && !changed)
      continue;
    bool handed_off;
    int speed = reset_port(hc, p, &handed_off);
    if (handed_off) {
      hc->companion_ports |= 1u << p;
      continue;
    }
    hc->root[p] = speed ? enumerate(hc, NULL, 0, p, speed) : 0;
    hc->port_failed[p] = hc->root[p] == 0;
    if (hc->port_failed[p])
      wr(hc, reg, rd(hc, reg) & ~(PORT_RWC | PORT_PE)); /* silence a half-configured device */
  }
}

static void eh_poll(void)
{
  for (int i = 0; i < nhcs; i++) {
    struct ehc *hc = hcs[i];
    uint32_t sts = rd(hc, OP_STS);
    if (sts & STS_EVENTS)
      wr(hc, OP_STS, sts & STS_EVENTS);
    if ((sts & (STS_HALTED | STS_HSE)) && !hc->halt_reported) {
      hc->halt_reported = true;
      con_printf("ehci: controller %02x:%02x.%u stopped (status %08x)\n", hc->bus, hc->slot, hc->func, sts);
    }
    for (int a = 1; a < EHCI_ADDRS; a++)
      if (hc->devs[a])
        for (struct epipe *p = hc->devs[a]->pipes; p; p = p->next)
          pipe_poll(p);
  }
  uint32_t now = timer_ticks();
  if ((now - last_scan) * 1000u / TIMER_HZ >= SCAN_MS) {
    last_scan = now;
    for (int i = 0; i < nhcs; i++)
      scan_ports(hcs[i]);
  }
}

static struct usbdev *eh_dev(int id)
{
  struct edev *u = find_dev(id);
  return u ? &u->pub : NULL;
}

static int eh_max_id(void)
{
  for (int i = nhcs - 1; i >= 0; i--)
    for (int a = EHCI_ADDRS - 1; a > 0; a--)
      if (hcs[i]->devs[a])
        return dev_id(hcs[i], a);
  return 0;
}

/* EHCI keeps no per-hub state: split transactions name the hub and port in each QH. */
static int eh_config_hub(int id, int nports, int think_time)
{
  (void)nports;
  (void)think_time;
  return find_dev(id) ? 0 : -1;
}

static int eh_attach_child(int parent_id, int port, int speed, uint32_t kbps)
{
  (void)kbps;
  struct edev *parent = find_dev(parent_id);
  if (!parent || speed < USB_SPEED_FULL || speed > USB_SPEED_HIGH)
    return 0;
  return enumerate(parent->hc, parent, port, parent->pub.root_port, speed);
}

static void eh_detach_child(int parent_id, int port)
{
  struct edev *parent = find_dev(parent_id);
  if (!parent)
    return;
  struct ehc *hc = parent->hc;
  for (int a = 1; a < EHCI_ADDRS; a++)
    if (hc->devs[a] && hc->devs[a]->pub.parent_slot == parent_id && hc->devs[a]->pub.parent_port == port)
      remove_device(dev_id(hc, a));
}

static const struct usbh_hc_ops ehci_ops = {
    .dev = eh_dev,
    .max_id = eh_max_id,
    .control_in = eh_control_in,
    .control_out = eh_control_out,
    .control_nodata = eh_control_nodata,
    .open_intr = eh_open_intr,
    .open_intr_out = eh_open_intr_out,
    .open_bulk_in = eh_open_bulk_in,
    .open_bulk_out = eh_open_bulk_out,
    .open_isoc_in = NULL,
    .pipe_send = eh_pipe_send,
    .close_pipe = eh_close_pipe,
    .config_hub = eh_config_hub,
    .attach_child = eh_attach_child,
    .detach_child = eh_detach_child,
    .poll = eh_poll,
};

/* Take the controller from the BIOS through the USBLEGSUP capability in PCI config space. */
static void bios_handoff(uint8_t bus, uint8_t slot, uint8_t func, uint32_t hccparams)
{
  uint32_t off = HCC_EECP(hccparams);
  for (int guard = 0; off >= 0x40 && guard < 16; guard++) {
    uint32_t v = pci_read32(bus, slot, func, (uint8_t)off);
    if ((v & 0xFF) == LEGSUP_ID) {
      if (v & LEGSUP_BIOS) {
        pci_write32(bus, slot, func, (uint8_t)off, v | LEGSUP_OS);
        uint32_t start = timer_ticks();
        while ((pci_read32(bus, slot, func, (uint8_t)off) & LEGSUP_BIOS) && elapsed_ms(start) < 1000)
          timer_sleep(1);
        v = pci_read32(bus, slot, func, (uint8_t)off);
        if (v & LEGSUP_BIOS) {
          con_printf("ehci: %02x:%02x.%u BIOS kept ownership, taking it anyway\n", bus, slot, func);
          pci_write32(bus, slot, func, (uint8_t)off, (v & ~LEGSUP_BIOS) | LEGSUP_OS);
        }
      }
      pci_write32(bus, slot, func, (uint8_t)(off + 4), LEGCTL_HANDOFF);
      return;
    }
    off = (v >> 8) & 0xFF;
  }
}

static void free_hc(struct ehc *hc)
{
  if (hc->frames)
    pfree(hc->frames);
  if (hc->skel)
    pfree(hc->skel);
  kfree(hc);
}

static void setup_controller(uint8_t bus, uint8_t slot, uint8_t func)
{
  uint32_t bar0 = pci_read32(bus, slot, func, 0x10);
  if (bar0 & 1) {
    con_printf("ehci: %02x:%02x.%u has no memory BAR\n", bus, slot, func);
    return;
  }
  if ((bar0 & 0x6) == 0x4 && pci_read32(bus, slot, func, 0x14)) {
    con_printf("ehci: %02x:%02x.%u registers are above 4 GiB\n", bus, slot, func);
    return;
  }
  uint32_t base = bar0 & ~0xFu;
  /* The register file is at most a few hundred bytes inside one page. */
  uint32_t page = base & ~0xFFFu;
  if (paging_identity_map(page, 0x1000) != 0)
    return;
  paging_mark_uc(page, 0x1000);
  pci_write32(bus, slot, func, 0x04, pci_read32(bus, slot, func, 0x04) | 0x6);

  volatile uint8_t *cap = (volatile uint8_t *)(uintptr_t)base;
  uint32_t hcs_params = *(volatile uint32_t *)(cap + CAP_HCSPARAMS);
  uint32_t hcc_params = *(volatile uint32_t *)(cap + CAP_HCCPARAMS);
  bios_handoff(bus, slot, func, hcc_params);

  struct ehc *hc = kmalloc(sizeof *hc);
  if (!hc)
    return;
  memset(hc, 0, sizeof *hc);
  hc->op = cap + cap[CAP_LENGTH];
  hc->bus = bus;
  hc->slot = slot;
  hc->func = func;

  wr(hc, OP_CMD, rd(hc, OP_CMD) & ~CMD_RUN);
  uint32_t start = timer_ticks();
  while (!(rd(hc, OP_STS) & STS_HALTED) && elapsed_ms(start) < 20)
    timer_sleep(1);
  wr(hc, OP_CMD, CMD_RESET);
  start = timer_ticks();
  while ((rd(hc, OP_CMD) & CMD_RESET) && elapsed_ms(start) < 250)
    timer_sleep(1);
  if (rd(hc, OP_CMD) & CMD_RESET) {
    con_printf("ehci: %02x:%02x.%u did not come out of reset\n", bus, slot, func);
    kfree(hc);
    return;
  }

  hc->frames = palloc();
  hc->skel = palloc();
  if (!hc->frames || !hc->skel) {
    free_hc(hc);
    return;
  }
  hc->nports = HCS_PORTS(hcs_params);
  hc->ncompanions = HCS_COMPANIONS(hcs_params);
  periodic_head(hc, LINK_TERM);
  /* The async ring's head: marked as such, never executed (halted), linked to itself. */
  hc->async_head = (struct qh *)hc->skel;
  qh_init(hc->async_head, CHR_HEAD | (EPS_HIGH << 12), CAP_MULT1);
  hc->async_head->token = TOK_HALTED;
  hc->async_head->link = phys(hc->async_head) | LINK_QH;

  if (hcc_params & HCC_64BIT)
    wr(hc, OP_SEGMENT, 0);
  wr(hc, OP_INTR, 0);
  wr(hc, OP_PERIODIC, phys(hc->frames));
  wr(hc, OP_ASYNC, phys(hc->async_head));
  wr(hc, OP_STS, 0x3F);
  wr(hc, OP_CMD, CMD_RUN | CMD_PSE | CMD_ASE | CMD_ITC_8);
  wr(hc, OP_CONFIG, 1); /* route every root port here; full/low-speed devices are handed back port by port */
  start = timer_ticks();
  while ((rd(hc, OP_STS) & STS_HALTED) && elapsed_ms(start) < 20)
    timer_sleep(1);
  if (rd(hc, OP_STS) & STS_HALTED) {
    con_printf("ehci: %02x:%02x.%u did not start\n", bus, slot, func);
    wr(hc, OP_CONFIG, 0);
    free_hc(hc);
    return;
  }
  if (hcs_params & HCS_PPC)
    for (int p = 1; p <= hc->nports; p++)
      wr(hc, OP_PORTSC(p), (rd(hc, OP_PORTSC(p)) & ~PORT_RWC) | PORT_POWER);
  timer_sleep(20); /* power-on and routing settle */

  hc->index = nhcs;
  hcs[nhcs++] = hc;
  con_printf("ehci: controller %02x:%02x.%u, %d ports, %d companion controller(s)\n", bus, slot, func, hc->nports,
             hc->ncompanions);
}

void ehci_boot(void)
{
  struct pci_dev_info info;
  for (int i = 0; pci_dev_info(i, &info); i++) {
    if (info.class_code != 0x0C || info.subclass != 0x03 ||
        ((pci_read32(info.bus, info.slot, info.func, 0x08) >> 8) & 0xFF) != 0x20)
      continue;
    if (nhcs == EHCI_MAX_HCS) {
      con_write("ehci: too many controllers, ignoring the rest\n");
      break;
    }
    setup_controller(info.bus, info.slot, info.func);
  }
  if (!nhcs)
    return;
  usbh_register(&ehci_ops, USBH_EHCI_FIRST_ID, nhcs * EHCI_ADDRS);
  for (int i = 0; i < nhcs; i++)
    scan_ports(hcs[i]);
  last_scan = timer_ticks();
}

bool ehci_controller(uint8_t bus, uint8_t slot, uint8_t func, int *first_id, int *last_id, int *nports,
                     uint32_t *companion_ports)
{
  for (int i = 0; i < nhcs; i++) {
    struct ehc *hc = hcs[i];
    if (hc->bus == bus && hc->slot == slot && hc->func == func) {
      *first_id = dev_id(hc, 1);
      *last_id = dev_id(hc, EHCI_ADDRS - 1);
      *nports = hc->nports;
      *companion_ports = hc->companion_ports;
      return true;
    }
  }
  return false;
}
