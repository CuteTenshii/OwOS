/* uhci.c: UHCI (USB 1.1) host controller driver. Full- and low-speed devices on the root ports and behind hubs, with
 * the control and interrupt transfers hubs and HID need. No interrupts: the poll thread checks the armed TDs every
 * millisecond.
 *
 * Every frame runs the same schedule: a skeleton QH whose horizontal chain holds one QH per interrupt pipe and ends at
 * the control QH. Interrupt endpoints are therefore polled every frame, which bInterval (a maximum latency for full
 * and low speed) allows. All DMA memory comes from palloc: identity-mapped and below 4 GiB. */

#include "uhci.h"
#include "console.h"
#include "heap.h"
#include "io.h"
#include "kstring.h"
#include "paging.h"
#include "pci.h"
#include "timer.h"
#include "usbhc.h"
#include <stdbool.h>
#include <stdint.h>

#define UHCI_MAX_HCS USBH_UHCI_MAX_HCS
#define UHCI_ADDRS USBH_ADDRS /* addresses 1..127; a device's id is USBH_UHCI_FIRST_ID + hc * UHCI_ADDRS + address */
#define UHCI_MAX_PORTS 7

#define REG_CMD 0x00
#define REG_STS 0x02
#define REG_INTR 0x04
#define REG_FRNUM 0x06
#define REG_FLBASE 0x08
#define REG_SOF 0x0C
#define REG_PORTSC(p) (0x10 + 2 * ((p) - 1))

#define CMD_RUN 0x0001
#define CMD_HCRESET 0x0002
#define CMD_CF 0x0040
#define CMD_MAXP 0x0080

#define STS_INT 0x0001
#define STS_ERRINT 0x0002
#define STS_SYSTEM_ERR 0x0008
#define STS_PROCESS_ERR 0x0010
#define STS_HALTED 0x0020
#define STS_ALL 0x003F

#define PORT_CONNECTED 0x0001
#define PORT_CSC 0x0002
#define PORT_ENABLED 0x0004
#define PORT_PEDC 0x0008
#define PORT_PRESENT 0x0080 /* reserved bit that always reads 1 on an implemented port */
#define PORT_LOWSPEED 0x0100
#define PORT_RESET 0x0200
#define PORT_RWC (PORT_CSC | PORT_PEDC)

/* PCI legacy support register: clearing its SMI enables (and the R/WC trap bits) takes the controller from the BIOS. */
#define PCI_LEGSUP 0xC0
#define LEGSUP_HANDOFF 0x8F00

#define TD_ACTLEN 0x7FF
#define TD_BITSTUFF (1u << 17)
#define TD_CRC_TIMEOUT (1u << 18)
#define TD_BABBLE (1u << 20)
#define TD_BUFFER_ERR (1u << 21)
#define TD_STALLED (1u << 22)
#define TD_ACTIVE (1u << 23)
#define TD_LOWSPEED (1u << 26)
#define TD_CERR3 (3u << 27)
#define TD_SPD (1u << 29)
#define TD_ERRORS (TD_BITSTUFF | TD_CRC_TIMEOUT | TD_BABBLE | TD_BUFFER_ERR | TD_STALLED)

#define LINK_TERM 0x1
#define LINK_QH 0x2
#define LINK_DEPTH 0x4

#define PID_SETUP 0x2D
#define PID_IN 0x69
#define PID_OUT 0xE1

#define CTRL_TIMEOUT_MS 1000
#define SCAN_MS 100
#define PIPE_MAX_ERRORS 8

struct uhci_td {
  volatile uint32_t link;
  volatile uint32_t status;
  volatile uint32_t token;
  volatile uint32_t buffer;
  uint32_t sw[4]; /* pads TDs to 32 bytes, so a page-aligned array keeps the 16-byte alignment */
};

struct uhci_qh {
  volatile uint32_t head;
  volatile uint32_t element;
  uint32_t sw[2];
};

/* A transfer is limited to one page of data; at 8 bytes per packet that is 512 data TDs plus setup and status. */
#define XFER_MAX 4096
#define TDS_PER_PAGE (4096 / (int)sizeof(struct uhci_td))
#define CHAIN_PAGES ((XFER_MAX / 8 + 2 + TDS_PER_PAGE - 1) / TDS_PER_PAGE)

struct chain {
  uint8_t *pages[CHAIN_PAGES];
};

struct uhc;
struct upipe;

struct udev {
  struct usbdev pub;
  struct uhc *hc;
  uint8_t addr; /* 0 until SET_ADDRESS */
  uint8_t mps0;
  struct upipe *pipes;
};

/* An interrupt pipe: its QH, its one TD and that TD's buffer share a page. */
struct upipe {
  struct usbh_pipe base;
  struct upipe *next;
  struct udev *dev;
  uint8_t *page;
  struct uhci_qh *qh;
  struct uhci_td *td;
  uint8_t *buf;
  uint8_t ep_addr;
  uint16_t mps;
  bool out, toggle, dead, armed;
  uint8_t errors;
  uint32_t retry_at; /* timer tick to re-arm after an error */
  usbh_pipe_cb cb;
  void *ctx;
};

struct uhc {
  uint16_t io;
  uint8_t bus, slot, func;
  int index, nports;
  uint32_t *frames;
  struct uhci_qh *intr_qh, *ctrl_qh;
  uint8_t *skel; /* page holding the skeleton QHs and the setup packet */
  uint8_t *setup;
  struct udev *devs[UHCI_ADDRS];
  int last_addr;
  int root[UHCI_MAX_PORTS + 1];
  bool port_failed[UHCI_MAX_PORTS + 1];
  bool halt_reported;
};

static struct uhc *hcs[UHCI_MAX_HCS];
static int nhcs;
static uint32_t last_scan;
static const struct usbh_hc_ops uhci_ops;

static uint32_t phys(const volatile void *p) { return (uint32_t)(uintptr_t)p; }

static int dev_id(const struct uhc *hc, int addr) { return USBH_UHCI_FIRST_ID + hc->index * UHCI_ADDRS + addr; }

static struct udev *find_dev(int id)
{
  int rel = id - USBH_UHCI_FIRST_ID;
  if (rel < 0 || rel / UHCI_ADDRS >= nhcs)
    return NULL;
  return hcs[rel / UHCI_ADDRS]->devs[rel % UHCI_ADDRS];
}

static uint32_t td_token(uint8_t pid, uint8_t addr, uint8_t ep, bool toggle, int len)
{
  uint32_t maxlen = len ? (uint32_t)(len - 1) & 0x7FF : 0x7FF;
  return pid | ((uint32_t)addr << 8) | ((uint32_t)(ep & 0xF) << 15) | ((uint32_t)toggle << 19) | (maxlen << 21);
}

static uint32_t td_status(const struct udev *u, uint32_t extra)
{
  return TD_ACTIVE | TD_CERR3 | (u->pub.speed == USB_SPEED_LOW ? TD_LOWSPEED : 0) | extra;
}

static int td_actual(uint32_t status) { return (int)((status + 1) & TD_ACTLEN); }
static int td_wanted(uint32_t token) { return (int)(((token >> 21) + 1) & TD_ACTLEN); }

/* Wait for the controller to start a new frame, so it no longer walks anything unlinked before the call. */
static void frame_wait(struct uhc *hc)
{
  uint16_t f = inw(hc->io + REG_FRNUM) & 0x3FF;
  for (int i = 0; i < 4 && (inw(hc->io + REG_FRNUM) & 0x3FF) == f; i++)
    timer_sleep(1);
}

static struct uhci_td *chain_td(struct chain *c, int i)
{
  return (struct uhci_td *)c->pages[i / TDS_PER_PAGE] + i % TDS_PER_PAGE;
}

static void chain_free(struct chain *c)
{
  for (int i = 0; i < CHAIN_PAGES; i++)
    if (c->pages[i])
      pfree(c->pages[i]);
}

static bool chain_alloc(struct chain *c, int n)
{
  memset(c, 0, sizeof *c);
  for (int i = 0; i < (n + TDS_PER_PAGE - 1) / TDS_PER_PAGE; i++)
    if (!(c->pages[i] = palloc())) {
      chain_free(c);
      return false;
    }
  return true;
}

/* Run TDs 0..n-1 on qh until the last one completes. A short IN packet (on a TD with SPD) skips ahead to TD `resume`,
 * the status stage. Returns the bytes IN TDs received, or -1; *done counts the TDs that completed without error. */
static int run_chain(struct uhc *hc, struct uhci_qh *qh, struct chain *c, int n, int resume, int *done)
{
  for (int i = 0; i < n; i++)
    chain_td(c, i)->link = i + 1 < n ? phys(chain_td(c, i + 1)) | LINK_DEPTH : LINK_TERM;
  __sync_synchronize();
  qh->element = phys(chain_td(c, 0));

  int i = 0, got = 0;
  *done = 0;
  uint32_t start = timer_ticks();
  for (;;) {
    while (i < n) {
      struct uhci_td *td = chain_td(c, i);
      uint32_t st = td->status, token = td->token;
      if (st & TD_ACTIVE)
        break;
      if (st & TD_ERRORS)
        goto fail;
      (*done)++;
      if ((token & 0xFF) != PID_IN) {
        i++;
        continue;
      }
      got += td_actual(st);
      if (td_actual(st) < td_wanted(token) && resume > i) {
        i = resume;
        qh->element = phys(chain_td(c, resume));
      } else {
        i++;
      }
    }
    if (i >= n)
      return got;
    if ((timer_ticks() - start) * 1000u / TIMER_HZ > CTRL_TIMEOUT_MS)
      goto fail;
    timer_sleep(1);
  }

fail:
  qh->element = LINK_TERM;
  frame_wait(hc);
  return -1;
}

/* One control transfer on endpoint 0: SETUP, DATA in mps0 packets, STATUS. Returns the bytes received or sent, or -1;
 * a short IN reply is zero-filled up to len. */
static int control(struct udev *u, uint8_t req_type, uint8_t req, uint16_t value, uint16_t index, uint8_t *data,
                   int len)
{
  struct uhc *hc = u->hc;
  bool in = req_type & 0x80;
  int mps = u->mps0 ? u->mps0 : 8;
  if (len < 0 || len > XFER_MAX)
    return -1;
  int ndata = (len + mps - 1) / mps, n = ndata + 2;
  struct chain c;
  uint8_t *buf = len ? palloc() : NULL;
  if ((len && !buf) || !chain_alloc(&c, n)) {
    if (buf)
      pfree(buf);
    return -1;
  }
  if (!in && len)
    memcpy(buf, data, (uint32_t)len);

  uint8_t *setup = hc->setup;
  setup[0] = req_type;
  setup[1] = req;
  setup[2] = (uint8_t)value;
  setup[3] = (uint8_t)(value >> 8);
  setup[4] = (uint8_t)index;
  setup[5] = (uint8_t)(index >> 8);
  setup[6] = (uint8_t)len;
  setup[7] = (uint8_t)(len >> 8);

  struct uhci_td *td = chain_td(&c, 0);
  td->status = td_status(u, 0);
  td->token = td_token(PID_SETUP, u->addr, 0, false, 8);
  td->buffer = phys(setup);
  bool toggle = true;
  for (int i = 0; i < ndata; i++) {
    int off = i * mps, chunk = len - off < mps ? len - off : mps;
    td = chain_td(&c, 1 + i);
    td->status = td_status(u, in ? TD_SPD : 0);
    td->token = td_token(in ? PID_IN : PID_OUT, u->addr, 0, toggle, chunk);
    td->buffer = phys(buf + off);
    toggle = !toggle;
  }
  td = chain_td(&c, n - 1);
  td->status = td_status(u, 0);
  td->token = td_token(in && len ? PID_OUT : PID_IN, u->addr, 0, true, 0);
  td->buffer = 0;

  int done, got = run_chain(hc, hc->ctrl_qh, &c, n, n - 1, &done);
  if (got >= 0 && in) {
    if (got > len)
      got = len;
    memcpy(data, buf, (uint32_t)got);
    memset(data + got, 0, (uint32_t)(len - got));
  }
  chain_free(&c);
  if (buf)
    pfree(buf);
  return got < 0 ? -1 : len;
}

static int uh_control_in(int id, uint8_t req_type, uint8_t req, uint16_t value, uint16_t index, uint8_t *out, int len)
{
  struct udev *u = find_dev(id);
  return u ? control(u, req_type, req, value, index, out, len) : -1;
}

static int uh_control_out(int id, uint8_t req_type, uint8_t req, uint16_t value, uint16_t index, const uint8_t *data,
                          int len)
{
  struct udev *u = find_dev(id);
  return u && control(u, req_type, req, value, index, (uint8_t *)data, len) >= 0 ? 0 : -1;
}

static int uh_control_nodata(int id, uint8_t req_type, uint8_t req, uint16_t value, uint16_t index)
{
  struct udev *u = find_dev(id);
  return u && control(u, req_type, req, value, index, NULL, 0) >= 0 ? 0 : -1;
}

static void link_qh(struct uhc *hc, struct uhci_qh *qh)
{
  qh->head = hc->intr_qh->head;
  __sync_synchronize();
  hc->intr_qh->head = phys(qh) | LINK_QH;
}

/* Take a pipe QH out of the chain; the caller waits a frame before freeing it. */
static void unlink_qh(struct uhc *hc, struct uhci_qh *qh)
{
  uint32_t me = phys(qh) | LINK_QH;
  for (struct uhci_qh *q = hc->intr_qh; q != hc->ctrl_qh; q = (struct uhci_qh *)(uintptr_t)(q->head & ~0xFu))
    if (q->head == me) {
      q->head = qh->head;
      return;
    }
}

static void pipe_arm(struct upipe *p)
{
  struct uhci_td *td = p->td;
  td->link = LINK_TERM;
  td->buffer = phys(p->buf);
  td->token = td_token(PID_IN, p->dev->addr, p->ep_addr, p->toggle, p->mps);
  __sync_synchronize();
  td->status = td_status(p->dev, 0);
  __sync_synchronize();
  p->qh->element = phys(td);
  p->armed = true;
}

static bool stalled_only(uint32_t status) { return (status & TD_ERRORS) == TD_STALLED; }

/* A finished interrupt IN TD: hand the data to the driver and re-arm, or back off after an error. A device that was
 * unplugged errors here until its hub or root port reports the disconnect, so retries are spaced out. */
static void pipe_poll(struct upipe *p)
{
  if (p->out || p->dead)
    return;
  uint32_t now = timer_ticks();
  if (!p->armed) {
    if ((int32_t)(now - p->retry_at) >= 0)
      pipe_arm(p);
    return;
  }
  uint32_t st = p->td->status;
  if (st & TD_ACTIVE)
    return;
  p->armed = false;
  if (st & TD_ERRORS) {
    if (++p->errors > PIPE_MAX_ERRORS) {
      p->dead = true;
      con_printf("usb: slot %d ep %02x gave up after repeated errors\n", p->dev->pub.slot, p->ep_addr);
      return;
    }
    if (stalled_only(st)) {
      control(p->dev, 0x02, 0x01, 0, p->ep_addr, NULL, 0);
      p->toggle = false;
    }
    p->retry_at = now + (uint32_t)p->errors * 10u * TIMER_HZ / 1000u + 1;
    return;
  }
  p->errors = 0;
  p->toggle = !p->toggle;
  p->cb(p->ctx, p->buf, td_actual(st));
  if (!p->dead)
    pipe_arm(p);
}

static struct usbh_pipe *open_pipe(int id, uint8_t ep_addr, uint16_t mps, bool out, usbh_pipe_cb cb, void *ctx)
{
  struct udev *u = find_dev(id);
  uint16_t limit = u && u->pub.speed == USB_SPEED_LOW ? 8 : 64;
  if (!u || mps == 0 || mps > limit || out != !(ep_addr & 0x80) || (!out && !cb))
    return NULL;
  struct upipe *p = kmalloc(sizeof *p);
  if (!p)
    return NULL;
  memset(p, 0, sizeof *p);
  p->page = palloc();
  if (!p->page) {
    kfree(p);
    return NULL;
  }
  p->base.ops = &uhci_ops;
  p->dev = u;
  p->qh = (struct uhci_qh *)p->page;
  p->td = (struct uhci_td *)(p->page + 32);
  p->buf = p->page + 64;
  p->ep_addr = ep_addr;
  p->mps = mps;
  p->out = out;
  p->cb = cb;
  p->ctx = ctx;
  p->qh->head = LINK_TERM;
  p->qh->element = LINK_TERM;
  link_qh(u->hc, p->qh);
  p->next = u->pipes;
  u->pipes = p;
  if (!out)
    pipe_arm(p);
  return &p->base;
}

static struct usbh_pipe *uh_open_intr(int id, uint8_t ep_addr, uint16_t mps, uint8_t interval, usbh_pipe_cb cb,
                                      void *ctx)
{
  (void)interval;
  return open_pipe(id, ep_addr, mps, false, cb, ctx);
}

static struct usbh_pipe *uh_open_intr_out(int id, uint8_t ep_addr, uint16_t mps, uint8_t interval)
{
  (void)interval;
  return open_pipe(id, ep_addr, mps, true, NULL, NULL);
}

static int uh_pipe_send(struct usbh_pipe *bp, const uint8_t *data, int len)
{
  struct upipe *p = (struct upipe *)bp;
  if (!p->out || p->dead || len <= 0 || len > XFER_MAX)
    return -1;
  int n = (len + p->mps - 1) / p->mps;
  struct chain c;
  uint8_t *buf = palloc();
  if (!buf || !chain_alloc(&c, n)) {
    if (buf)
      pfree(buf);
    return -1;
  }
  memcpy(buf, data, (uint32_t)len);
  bool toggle = p->toggle;
  for (int i = 0; i < n; i++) {
    int off = i * p->mps, chunk = len - off < p->mps ? len - off : p->mps;
    struct uhci_td *td = chain_td(&c, i);
    td->status = td_status(p->dev, 0);
    td->token = td_token(PID_OUT, p->dev->addr, p->ep_addr, toggle, chunk);
    td->buffer = phys(buf + off);
    toggle = !toggle;
  }
  int done, rc = run_chain(p->dev->hc, p->qh, &c, n, n, &done);
  if (rc < 0 && done < n && stalled_only(chain_td(&c, done)->status)) {
    control(p->dev, 0x02, 0x01, 0, p->ep_addr, NULL, 0);
    p->toggle = false;
  } else {
    p->toggle = p->toggle ^ (done & 1);
  }
  chain_free(&c);
  pfree(buf);
  return rc < 0 ? -1 : 0;
}

static void free_pipes(struct udev *u)
{
  if (!u->pipes)
    return;
  for (struct upipe *p = u->pipes; p; p = p->next)
    unlink_qh(u->hc, p->qh);
  frame_wait(u->hc);
  while (u->pipes) {
    struct upipe *p = u->pipes;
    u->pipes = p->next;
    pfree(p->page);
    kfree(p);
  }
}

static void uh_close_pipe(struct usbh_pipe *bp)
{
  struct upipe *p = (struct upipe *)bp;
  struct udev *u = p->dev;
  p->dead = true;
  unlink_qh(u->hc, p->qh);
  frame_wait(u->hc);
  for (struct upipe **pp = &u->pipes; *pp; pp = &(*pp)->next)
    if (*pp == p) {
      *pp = p->next;
      break;
    }
  pfree(p->page);
  kfree(p);
}

/* Drop a device: children first, class drivers, then its pipes and address. */
static void remove_device(int id)
{
  struct udev *u = find_dev(id);
  if (!u)
    return;
  struct uhc *hc = u->hc;
  for (struct upipe *p = u->pipes; p; p = p->next)
    p->dead = true;
  for (int a = 1; a < UHCI_ADDRS; a++)
    if (hc->devs[a] && hc->devs[a]->pub.parent_slot == id)
      remove_device(dev_id(hc, a));
  con_printf("usb: slot %d detached\n", id);
  usbh_unbind_classes(id);
  free_pipes(u);
  hc->devs[u->addr] = NULL;
  kfree(u->pub.cfg);
  kfree(u);
}

/* Address the device that a reset port just enabled, read its descriptors and bind class drivers; returns its id or
 * 0. parent NULL means a root port. */
static int enumerate(struct uhc *hc, struct udev *parent, int parent_port, int root_port, int speed)
{
  if (parent && parent->pub.depth >= 5)
    return 0;
  /* Round-robin, so a device that failed after SET_ADDRESS does not share its address with the next one. */
  int addr = 0;
  for (int i = 1; i < UHCI_ADDRS && !addr; i++) {
    int a = (hc->last_addr + i - 1) % (UHCI_ADDRS - 1) + 1;
    if (!hc->devs[a])
      addr = a;
  }
  if (!addr) {
    con_write("uhci: out of device addresses\n");
    return 0;
  }
  hc->last_addr = addr;
  struct udev *u = kmalloc(sizeof *u);
  if (!u)
    return 0;
  memset(u, 0, sizeof *u);
  u->hc = hc;
  u->mps0 = 8;
  struct usbdev *d = &u->pub;
  d->slot = dev_id(hc, addr);
  d->speed = speed;
  d->kbps = usb3_default_kbps(speed);
  d->root_port = (uint8_t)root_port;
  d->parent_slot = parent ? parent->pub.slot : 0;
  d->parent_port = (uint8_t)parent_port;
  d->depth = parent ? (uint8_t)(parent->pub.depth + 1) : 0;

  uint8_t desc[18];
  if (control(u, 0x80, 0x06, 0x0100, 0, desc, 8) < 0)
    goto fail;
  if (desc[7] == 16 || desc[7] == 32 || desc[7] == 64)
    u->mps0 = speed == USB_SPEED_LOW ? 8 : desc[7];
  if (control(u, 0x00, 0x05, (uint16_t)addr, 0, NULL, 0) < 0)
    goto fail;
  timer_sleep(2); /* SET_ADDRESS recovery */
  u->addr = (uint8_t)addr;
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
  con_printf("uhci: device on port %d%s did not answer\n", parent ? parent_port : root_port, parent ? " of a hub" : "");
  kfree(u);
  return 0;
}

/* Reset a root port and enable it; returns the speed of the device behind it, or 0. */
static int reset_port(struct uhc *hc, int port)
{
  uint16_t reg = (uint16_t)(hc->io + REG_PORTSC(port));
  outw(reg, PORT_RESET);
  timer_sleep(50);
  outw(reg, 0);
  timer_sleep(1);
  for (int tries = 0; tries < 10; tries++) {
    uint16_t s = inw(reg);
    if (!(s & PORT_CONNECTED))
      return 0;
    outw(reg, (uint16_t)(PORT_ENABLED | (s & PORT_RWC)));
    if (s & PORT_ENABLED) {
      timer_sleep(10); /* reset recovery */
      return (inw(reg) & PORT_LOWSPEED) ? USB_SPEED_LOW : USB_SPEED_FULL;
    }
    timer_sleep(10);
  }
  return 0;
}

static void scan_ports(struct uhc *hc)
{
  for (int p = 1; p <= hc->nports; p++) {
    uint16_t reg = (uint16_t)(hc->io + REG_PORTSC(p));
    uint16_t s = inw(reg);
    bool connected = s & PORT_CONNECTED;
    bool changed = s & PORT_CSC;
    if (s & PORT_RWC)
      outw(reg, (uint16_t)(s & (PORT_ENABLED | PORT_RWC)));
    /* An error can disable the port without a connect change; the device is gone either way. */
    if (hc->root[p] && (s & PORT_PEDC) && !(s & PORT_ENABLED))
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
    int speed = reset_port(hc, p);
    hc->root[p] = speed ? enumerate(hc, NULL, 0, p, speed) : 0;
    hc->port_failed[p] = hc->root[p] == 0;
    if (hc->port_failed[p])
      outw(reg, 0); /* disable the port, silencing a half-configured device */
  }
}

static void uh_poll(void)
{
  for (int i = 0; i < nhcs; i++) {
    struct uhc *hc = hcs[i];
    uint16_t sts = inw(hc->io + REG_STS);
    if (sts & (STS_INT | STS_ERRINT))
      outw(hc->io + REG_STS, sts & (STS_INT | STS_ERRINT));
    if ((sts & (STS_HALTED | STS_PROCESS_ERR | STS_SYSTEM_ERR)) && !hc->halt_reported) {
      hc->halt_reported = true;
      con_printf("uhci: controller %02x:%02x.%u stopped (status %04x)\n", hc->bus, hc->slot, hc->func, sts);
    }
    for (int a = 1; a < UHCI_ADDRS; a++)
      if (hc->devs[a])
        for (struct upipe *p = hc->devs[a]->pipes; p; p = p->next)
          pipe_poll(p);
  }
  uint32_t now = timer_ticks();
  if ((now - last_scan) * 1000u / TIMER_HZ >= SCAN_MS) {
    last_scan = now;
    for (int i = 0; i < nhcs; i++)
      scan_ports(hcs[i]);
  }
}

static struct usbdev *uh_dev(int id)
{
  struct udev *u = find_dev(id);
  return u ? &u->pub : NULL;
}

static int uh_max_id(void)
{
  for (int i = nhcs - 1; i >= 0; i--)
    for (int a = UHCI_ADDRS - 1; a > 0; a--)
      if (hcs[i]->devs[a])
        return dev_id(hcs[i], a);
  return 0;
}

/* UHCI keeps no per-hub state: the hub's ports are reached through the hub itself. */
static int uh_config_hub(int id, int nports, int think_time)
{
  (void)nports;
  (void)think_time;
  return find_dev(id) ? 0 : -1;
}

static int uh_attach_child(int parent_id, int port, int speed, uint32_t kbps)
{
  (void)kbps;
  struct udev *parent = find_dev(parent_id);
  if (!parent || (speed != USB_SPEED_FULL && speed != USB_SPEED_LOW))
    return 0;
  return enumerate(parent->hc, parent, port, parent->pub.root_port, speed);
}

static void uh_detach_child(int parent_id, int port)
{
  struct udev *parent = find_dev(parent_id);
  if (!parent)
    return;
  struct uhc *hc = parent->hc;
  for (int a = 1; a < UHCI_ADDRS; a++)
    if (hc->devs[a] && hc->devs[a]->pub.parent_slot == parent_id && hc->devs[a]->pub.parent_port == port)
      remove_device(dev_id(hc, a));
}

static const struct usbh_hc_ops uhci_ops = {
    .dev = uh_dev,
    .max_id = uh_max_id,
    .control_in = uh_control_in,
    .control_out = uh_control_out,
    .control_nodata = uh_control_nodata,
    .open_intr = uh_open_intr,
    .open_intr_out = uh_open_intr_out,
    .open_bulk_in = NULL,
    .open_bulk_out = NULL,
    .open_isoc_in = NULL,
    .pipe_send = uh_pipe_send,
    .close_pipe = uh_close_pipe,
    .config_hub = uh_config_hub,
    .attach_child = uh_attach_child,
    .detach_child = uh_detach_child,
    .poll = uh_poll,
};

/* Implemented ports read back the always-one bit 7; most controllers have two. */
static int count_ports(uint16_t io)
{
  int n = 0;
  while (n < UHCI_MAX_PORTS) {
    uint16_t s = inw((uint16_t)(io + REG_PORTSC(n + 1)));
    if (!(s & PORT_PRESENT) || s == 0xFFFF)
      break;
    n++;
  }
  return n < 2 ? 2 : n;
}

static void free_hc(struct uhc *hc)
{
  if (hc->frames)
    pfree(hc->frames);
  if (hc->skel)
    pfree(hc->skel);
  kfree(hc);
}

static void setup_controller(uint8_t bus, uint8_t slot, uint8_t func)
{
  uint32_t bar4 = pci_read32(bus, slot, func, 0x20);
  if (!(bar4 & 1)) {
    con_printf("uhci: %02x:%02x.%u has no I/O BAR\n", bus, slot, func);
    return;
  }
  uint16_t io = (uint16_t)(bar4 & 0xFFE0);
  pci_write32(bus, slot, func, 0x04, pci_read32(bus, slot, func, 0x04) | 0x5);
  uint32_t legsup = pci_read32(bus, slot, func, PCI_LEGSUP);
  pci_write32(bus, slot, func, PCI_LEGSUP, (legsup & 0xFFFF0000u) | LEGSUP_HANDOFF);

  outw(io + REG_CMD, CMD_HCRESET);
  for (int ms = 0; ms < 10 && (inw(io + REG_CMD) & CMD_HCRESET); ms++)
    timer_sleep(1);
  if (inw(io + REG_CMD) & CMD_HCRESET) {
    con_printf("uhci: %02x:%02x.%u did not come out of reset\n", bus, slot, func);
    return;
  }
  outw(io + REG_INTR, 0);
  outw(io + REG_CMD, 0);

  struct uhc *hc = kmalloc(sizeof *hc);
  if (!hc)
    return;
  memset(hc, 0, sizeof *hc);
  hc->frames = palloc();
  hc->skel = palloc();
  if (!hc->frames || !hc->skel) {
    free_hc(hc);
    return;
  }
  hc->io = io;
  hc->bus = bus;
  hc->slot = slot;
  hc->func = func;
  hc->intr_qh = (struct uhci_qh *)hc->skel;
  hc->ctrl_qh = (struct uhci_qh *)(hc->skel + 32);
  hc->setup = hc->skel + 64;
  hc->ctrl_qh->head = LINK_TERM;
  hc->ctrl_qh->element = LINK_TERM;
  hc->intr_qh->head = phys(hc->ctrl_qh) | LINK_QH;
  hc->intr_qh->element = LINK_TERM;
  for (int i = 0; i < 1024; i++)
    hc->frames[i] = phys(hc->intr_qh) | LINK_QH;
  hc->nports = count_ports(io);

  outl(io + REG_FLBASE, phys(hc->frames));
  outw(io + REG_FRNUM, 0);
  outb(io + REG_SOF, 64);
  outw(io + REG_STS, STS_ALL);
  outw(io + REG_CMD, CMD_RUN | CMD_CF | CMD_MAXP);
  for (int ms = 0; ms < 10 && (inw(io + REG_STS) & STS_HALTED); ms++)
    timer_sleep(1);
  if (inw(io + REG_STS) & STS_HALTED) {
    con_printf("uhci: %02x:%02x.%u did not start\n", bus, slot, func);
    outw(io + REG_CMD, 0);
    free_hc(hc);
    return;
  }
  hc->index = nhcs;
  hcs[nhcs++] = hc;
  con_printf("uhci: controller %02x:%02x.%u at io %04x, %d ports\n", bus, slot, func, io, hc->nports);
}

void uhci_boot(void)
{
  struct pci_dev_info info;
  for (int i = 0; pci_dev_info(i, &info); i++) {
    if (info.class_code != 0x0C || info.subclass != 0x03 ||
        ((pci_read32(info.bus, info.slot, info.func, 0x08) >> 8) & 0xFF) != 0x00)
      continue;
    if (nhcs == UHCI_MAX_HCS) {
      con_write("uhci: too many controllers, ignoring the rest\n");
      break;
    }
    setup_controller(info.bus, info.slot, info.func);
  }
  if (!nhcs)
    return;
  usbh_register(&uhci_ops, USBH_UHCI_FIRST_ID, nhcs * UHCI_ADDRS);
  for (int i = 0; i < nhcs; i++)
    scan_ports(hcs[i]);
  last_scan = timer_ticks();
}

bool uhci_controller(uint8_t bus, uint8_t slot, uint8_t func, int *first_id, int *last_id, int *nports)
{
  for (int i = 0; i < nhcs; i++) {
    struct uhc *hc = hcs[i];
    if (hc->bus == bus && hc->slot == slot && hc->func == func) {
      *first_id = dev_id(hc, 1);
      *last_id = dev_id(hc, UHCI_ADDRS - 1);
      *nports = hc->nports;
      return true;
    }
  }
  return false;
}
