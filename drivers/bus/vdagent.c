/* vdagent.c: QEMU clipboard bridge (see vdagent.h).
 * Modern virtio-pci transport over a virtio-serial device, polled. Port 1 carries the spice vdagent
 * protocol in "clipboard by demand" mode: a grab announces ownership, the other side requests the data. */

#include "vdagent.h"
#include "console.h"
#include "kstring.h"
#include "paging.h"
#include "pci.h"
#include "pmm.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VIRTIO_VENDOR 0x1AF4
#define VIRTIO_DEV_SERIAL 0x1043
#define VIRTIO_DEV_SERIAL_TRANS 0x1003

#define PCI_CAP_VENDOR 0x09
#define CAP_COMMON 1
#define CAP_NOTIFY 2

#define CC_DEV_FEAT_SEL 0x00
#define CC_DEV_FEAT 0x04
#define CC_DRV_FEAT_SEL 0x08
#define CC_DRV_FEAT 0x0C
#define CC_STATUS 0x14
#define CC_Q_SELECT 0x16
#define CC_Q_SIZE 0x18
#define CC_Q_ENABLE 0x1C
#define CC_Q_NOTIFY_OFF 0x1E
#define CC_Q_DESC 0x20
#define CC_Q_DRIVER 0x28
#define CC_Q_DEVICE 0x30

#define ST_ACK 1
#define ST_DRIVER 2
#define ST_DRIVER_OK 4
#define ST_FEATURES_OK 8
#define F_MULTIPORT 2u    /* feature bit 1 */
#define F_VERSION_1_HI 1u /* feature bit 32 = bit 0 of the high word */

#define QSIZE 8
#define DESC_F_WRITE 2
#define QMEM 0x4000
#define Q_AVAIL 0x100
#define Q_USED 0x1000
#define Q_BUF 0x2000
#define RX_BUF 1024
#define TX_BUF 4096
#define POLL_LIMIT 5000000

/* Queue indexes: port 0 takes 0/1, the control port 2/3, port n >= 1 takes 2n+2 / 2n+3. */
#define Q_CTL_RX 2
#define Q_CTL_TX 3
#define PORT_ID 1
#define Q_PORT_RX 4
#define Q_PORT_TX 5

enum { CTL_DEVICE_READY = 0, CTL_DEVICE_ADD = 1, CTL_PORT_READY = 3, CTL_PORT_OPEN = 6, CTL_PORT_NAME = 7 };

#define PORT_NAME "com.redhat.spice.0"

#define VD_PROTOCOL 1
#define VD_CLIENT_PORT 1
#define VD_CHUNK_MAX 2048
#define VD_HDR 20
#define VD_CAP_CLIPBOARD_BY_DEMAND 5
#define VD_CLIPBOARD_UTF8_TEXT 1

enum {
  VD_MSG_CLIPBOARD = 4,
  VD_MSG_ANNOUNCE_CAPS = 6,
  VD_MSG_GRAB = 7,
  VD_MSG_REQUEST = 8,
  VD_MSG_RELEASE = 9,
};

#define CLIP_MAX 16384
#define MSG_MAX (VD_HDR + 4 + CLIP_MAX)

struct desc {
  uint64_t addr;
  uint32_t len;
  uint16_t flags, next;
};

struct vq {
  volatile struct desc *descs;
  volatile uint16_t *avail; /* flags, idx, ring[QSIZE] */
  volatile uint16_t *used;  /* flags, idx, then {id,len} pairs */
  volatile uint16_t *notify;
  uint8_t *buf;
  uint32_t buf_phys;
  uint16_t index, avail_idx, last_used;
};

static volatile uint8_t *common;
static volatile uint8_t *notify_base;
static uint32_t notify_mult;
static struct vq ctl_rx, ctl_tx, port_rx, port_tx;
static bool ready;
static bool host_open, guest_open, caps_sent;

static uint8_t stream_hdr[8];
static uint32_t stream_hdr_n, chunk_left, msg_n;
static uint8_t msg[MSG_MAX];
static uint8_t out[MSG_MAX];

static uint8_t host_clip[CLIP_MAX];
static int host_len;
static bool host_new, request_pending;
static uint8_t guest_clip[CLIP_MAX];
static int guest_len;
static bool grab_pending;

static inline void wr8(uint32_t o, uint8_t v) { *(volatile uint8_t *)(common + o) = v; }
static inline void wr16(uint32_t o, uint16_t v) { *(volatile uint16_t *)(common + o) = v; }
static inline void wr32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(common + o) = v; }
static inline uint8_t rd8(uint32_t o) { return *(volatile uint8_t *)(common + o); }
static inline uint16_t rd16(uint32_t o) { return *(volatile uint16_t *)(common + o); }
static inline uint32_t rd32(uint32_t o) { return *(volatile uint32_t *)(common + o); }

static inline uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static inline void put32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

/* Map a BAR-relative window uncached; 64-bit BARs above 4 GiB are unreachable from this kernel. */
static volatile uint8_t *map_window(int bus, int slot, int bar, uint32_t off, uint32_t len)
{
  uint32_t lo = pci_read32((uint8_t)bus, (uint8_t)slot, 0, (uint8_t)(0x10 + bar * 4));
  if (lo & 1)
    return NULL;
  if (((lo >> 1) & 3) == 2 && pci_read32((uint8_t)bus, (uint8_t)slot, 0, (uint8_t)(0x10 + (bar + 1) * 4)) != 0)
    return NULL;
  uint32_t phys = (lo & 0xFFFFFFF0u) + off;
  if (paging_identity_map(phys & ~0xFFFu, len + (phys & 0xFFF)) < 0)
    return NULL;
  paging_mark_uc(phys & ~0xFFFu, len + (phys & 0xFFF));
  return (volatile uint8_t *)(uintptr_t)phys;
}

static bool setup_queue(struct vq *q, uint16_t index)
{
  uint32_t mem = pmm_alloc_contig(QMEM);
  if (!mem || paging_identity_map(mem, QMEM) < 0)
    return false;
  memset((void *)(uintptr_t)mem, 0, QMEM);
  q->descs = (volatile struct desc *)(uintptr_t)mem;
  q->avail = (volatile uint16_t *)(uintptr_t)(mem + Q_AVAIL);
  q->used = (volatile uint16_t *)(uintptr_t)(mem + Q_USED);
  q->buf_phys = mem + Q_BUF;
  q->buf = (uint8_t *)(uintptr_t)q->buf_phys;
  q->index = index;
  wr16(CC_Q_SELECT, index);
  if (rd16(CC_Q_SIZE) < QSIZE)
    return false;
  wr16(CC_Q_SIZE, QSIZE);
  wr32(CC_Q_DESC, mem);
  wr32(CC_Q_DESC + 4, 0);
  wr32(CC_Q_DRIVER, mem + Q_AVAIL);
  wr32(CC_Q_DRIVER + 4, 0);
  wr32(CC_Q_DEVICE, mem + Q_USED);
  wr32(CC_Q_DEVICE + 4, 0);
  q->notify = (volatile uint16_t *)(notify_base + (uint32_t)rd16(CC_Q_NOTIFY_OFF) * notify_mult);
  wr16(CC_Q_ENABLE, 1);
  return true;
}

static void publish(struct vq *q)
{
  __asm__ volatile("mfence" ::: "memory");
  q->avail[1] = q->avail_idx;
  __asm__ volatile("mfence" ::: "memory");
  *q->notify = q->index;
}

static void rx_post(struct vq *q, uint16_t id)
{
  q->descs[id] = (struct desc){q->buf_phys + (uint32_t)id * RX_BUF, RX_BUF, DESC_F_WRITE, 0};
  q->avail[2 + (q->avail_idx % QSIZE)] = id;
  q->avail_idx++;
}

static void rx_fill(struct vq *q)
{
  for (uint16_t i = 0; i < QSIZE; i++)
    rx_post(q, i);
  publish(q);
}

/* Hand each completed buffer to fn, then give it back to the device. */
static void rx_drain(struct vq *q, void (*fn)(const uint8_t *, uint32_t))
{
  bool any = false;
  while (q->last_used != q->used[1]) {
    volatile uint32_t *e = (volatile uint32_t *)((volatile uint8_t *)q->used + 4) + 2 * (q->last_used % QSIZE);
    uint16_t id = (uint16_t)(e[0] % QSIZE);
    uint32_t len = e[1] < RX_BUF ? e[1] : RX_BUF;
    fn(q->buf + (uint32_t)id * RX_BUF, len);
    rx_post(q, id);
    q->last_used++;
    any = true;
  }
  if (any)
    publish(q);
}

/* Send the first len bytes of q->buf and wait for the device to take them. */
static bool tx_submit(struct vq *q, uint32_t len)
{
  q->descs[0] = (struct desc){q->buf_phys, len, 0, 0};
  q->avail[2 + (q->avail_idx % QSIZE)] = 0;
  q->avail_idx++;
  publish(q);
  for (int i = 0; i < POLL_LIMIT; i++)
    if (q->used[1] != q->last_used) {
      q->last_used = q->used[1];
      return true;
    }
  return false;
}

static void ctl_send(uint32_t id, uint16_t event, uint16_t value)
{
  put32(ctl_tx.buf, id);
  ctl_tx.buf[4] = (uint8_t)event;
  ctl_tx.buf[5] = (uint8_t)(event >> 8);
  ctl_tx.buf[6] = (uint8_t)value;
  ctl_tx.buf[7] = (uint8_t)(value >> 8);
  tx_submit(&ctl_tx, 8);
}

static void agent_send(uint32_t type, const void *a, uint32_t alen, const void *b, uint32_t blen)
{
  put32(out, VD_PROTOCOL);
  put32(out + 4, type);
  put32(out + 8, 0);
  put32(out + 12, 0);
  put32(out + 16, alen + blen);
  if (alen)
    memcpy(out + VD_HDR, a, alen);
  if (blen)
    memcpy(out + VD_HDR + alen, b, blen);
  uint32_t total = VD_HDR + alen + blen;
  for (uint32_t off = 0; off < total; off += VD_CHUNK_MAX) {
    uint32_t n = total - off < VD_CHUNK_MAX ? total - off : VD_CHUNK_MAX;
    put32(port_tx.buf, VD_CLIENT_PORT);
    put32(port_tx.buf + 4, n);
    memcpy(port_tx.buf + 8, out + off, n);
    if (!tx_submit(&port_tx, 8 + n))
      return;
  }
}

static void send_caps(bool request)
{
  uint32_t caps[2] = {request, 1u << VD_CAP_CLIPBOARD_BY_DEMAND};
  agent_send(VD_MSG_ANNOUNCE_CAPS, caps, sizeof caps, NULL, 0);
}

static void dispatch(uint32_t type, const uint8_t *body, uint32_t size)
{
  switch (type) {
  case VD_MSG_ANNOUNCE_CAPS:
    if (size >= 4 && le32(body))
      send_caps(false);
    break;
  case VD_MSG_GRAB:
    request_pending = false;
    for (uint32_t i = 0; i + 4 <= size; i += 4)
      if (le32(body + i) == VD_CLIPBOARD_UTF8_TEXT) {
        uint32_t t = VD_CLIPBOARD_UTF8_TEXT;
        agent_send(VD_MSG_REQUEST, &t, 4, NULL, 0);
        request_pending = true;
        break;
      }
    break;
  case VD_MSG_CLIPBOARD:
    if (size >= 4 && le32(body) == VD_CLIPBOARD_UTF8_TEXT) {
      host_len = (int)(size - 4);
      memcpy(host_clip, body + 4, (uint32_t)host_len);
      host_new = true;
    }
    request_pending = false;
    break;
  case VD_MSG_REQUEST:
    if (size >= 4 && le32(body) == VD_CLIPBOARD_UTF8_TEXT && guest_len > 0) {
      uint32_t t = VD_CLIPBOARD_UTF8_TEXT;
      agent_send(VD_MSG_CLIPBOARD, &t, 4, guest_clip, (uint32_t)guest_len);
    }
    break;
  case VD_MSG_RELEASE:
    request_pending = false;
    break;
  }
}

/* Reassemble messages from the chunked byte stream. A message too big for msg[] is dropped. */
static void port_rx_bytes(const uint8_t *d, uint32_t len)
{
  for (uint32_t i = 0; i < len; i++) {
    uint8_t b = d[i];
    if (chunk_left == 0) {
      stream_hdr[stream_hdr_n++] = b;
      if (stream_hdr_n == sizeof stream_hdr) {
        chunk_left = le32(stream_hdr + 4);
        stream_hdr_n = 0;
      }
      continue;
    }
    chunk_left--;
    if (msg_n < MSG_MAX)
      msg[msg_n] = b;
    msg_n++;
    if (msg_n >= VD_HDR && msg_n == VD_HDR + le32(msg + 16)) {
      if (msg_n <= MSG_MAX)
        dispatch(le32(msg + 4), msg + VD_HDR, msg_n - VD_HDR);
      msg_n = 0;
    }
  }
}

static void ctl_rx_msg(const uint8_t *d, uint32_t len)
{
  if (len < 8 || le32(d) != PORT_ID)
    return;
  uint16_t event = (uint16_t)(d[4] | d[5] << 8), value = (uint16_t)(d[6] | d[7] << 8);
  switch (event) {
  case CTL_DEVICE_ADD:
    ctl_send(PORT_ID, CTL_PORT_READY, 1);
    break;
  case CTL_PORT_NAME:
    if (len - 8 >= sizeof PORT_NAME - 1 && memcmp(d + 8, PORT_NAME, sizeof PORT_NAME - 1) == 0) {
      guest_open = true;
      ctl_send(PORT_ID, CTL_PORT_OPEN, 1);
    }
    break;
  case CTL_PORT_OPEN:
    host_open = value != 0;
    break;
  }
}

static void pump(void)
{
  rx_drain(&ctl_rx, ctl_rx_msg);
  rx_drain(&port_rx, port_rx_bytes);
  if (host_open && guest_open && !caps_sent) {
    caps_sent = true;
    send_caps(true);
  }
  if (grab_pending && caps_sent) {
    grab_pending = false;
    uint32_t t = VD_CLIPBOARD_UTF8_TEXT;
    agent_send(VD_MSG_GRAB, &t, 4, NULL, 0);
  }
}

int vdagent_present(void) { return ready; }

int vdagent_clip_recv(char *buf, int max)
{
  if (!ready)
    return 0;
  pump();
  for (int i = 0; request_pending && !host_new && i < POLL_LIMIT; i++)
    pump();
  if (!host_new)
    return 0;
  int n = host_len < max ? host_len : max;
  memcpy(buf, host_clip, (uint32_t)n);
  host_new = false;
  return n;
}

int vdagent_clip_send(const char *buf, int len)
{
  if (!ready)
    return -1;
  if (len <= 0)
    return 0;
  guest_len = len < CLIP_MAX ? len : CLIP_MAX;
  memcpy(guest_clip, buf, (uint32_t)guest_len);
  grab_pending = true;
  pump();
  return len;
}

void vdagent_init(void)
{
  int bus, slot = 0;
  bool found = false;
  for (bus = 0; bus < 256 && !found; bus++)
    for (slot = 0; slot < 32; slot++) {
      uint32_t id = pci_read32((uint8_t)bus, (uint8_t)slot, 0, 0);
      uint32_t dev = id >> 16;
      if ((id & 0xFFFF) == VIRTIO_VENDOR && (dev == VIRTIO_DEV_SERIAL || dev == VIRTIO_DEV_SERIAL_TRANS)) {
        found = true;
        break;
      }
    }
  if (!found)
    return;
  bus--;

  uint32_t cmd = pci_read32((uint8_t)bus, (uint8_t)slot, 0, 0x04);
  pci_write32((uint8_t)bus, (uint8_t)slot, 0, 0x04, cmd | 0x6 | 0x400); /* polled driver: mask INTx */

  for (uint32_t p = pci_read32((uint8_t)bus, (uint8_t)slot, 0, 0x34) & 0xFC; p;
       p = (pci_read32((uint8_t)bus, (uint8_t)slot, 0, (uint8_t)p) >> 8) & 0xFC) {
    uint32_t w0 = pci_read32((uint8_t)bus, (uint8_t)slot, 0, (uint8_t)p);
    if ((w0 & 0xFF) != PCI_CAP_VENDOR)
      continue;
    uint32_t type = (w0 >> 24) & 0xFF;
    uint32_t bar = pci_read32((uint8_t)bus, (uint8_t)slot, 0, (uint8_t)(p + 4)) & 0xFF;
    uint32_t off = pci_read32((uint8_t)bus, (uint8_t)slot, 0, (uint8_t)(p + 8));
    uint32_t len = pci_read32((uint8_t)bus, (uint8_t)slot, 0, (uint8_t)(p + 12));
    if (bar > 5)
      continue;
    if (type == CAP_COMMON)
      common = map_window(bus, slot, (int)bar, off, len);
    else if (type == CAP_NOTIFY) {
      notify_mult = pci_read32((uint8_t)bus, (uint8_t)slot, 0, (uint8_t)(p + 16));
      notify_base = map_window(bus, slot, (int)bar, off, len);
    }
  }
  if (!common || !notify_base)
    return;

  wr8(CC_STATUS, 0);
  wr8(CC_STATUS, ST_ACK);
  wr8(CC_STATUS, ST_ACK | ST_DRIVER);
  wr32(CC_DEV_FEAT_SEL, 0);
  bool multiport = rd32(CC_DEV_FEAT) & F_MULTIPORT;
  wr32(CC_DEV_FEAT_SEL, 1);
  bool v1 = rd32(CC_DEV_FEAT) & F_VERSION_1_HI;
  if (!multiport || !v1)
    return;
  wr32(CC_DRV_FEAT_SEL, 0);
  wr32(CC_DRV_FEAT, F_MULTIPORT);
  wr32(CC_DRV_FEAT_SEL, 1);
  wr32(CC_DRV_FEAT, F_VERSION_1_HI);
  wr8(CC_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK);
  if (!(rd8(CC_STATUS) & ST_FEATURES_OK))
    return;

  if (!setup_queue(&ctl_rx, Q_CTL_RX) || !setup_queue(&ctl_tx, Q_CTL_TX) || !setup_queue(&port_rx, Q_PORT_RX) ||
      !setup_queue(&port_tx, Q_PORT_TX)) {
    con_write("vdagent: queue setup failed.\n");
    return;
  }
  rx_fill(&ctl_rx);
  rx_fill(&port_rx);
  wr8(CC_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK | ST_DRIVER_OK);
  ready = true;
  ctl_send(0, CTL_DEVICE_READY, 1);
  con_write("vdagent: virtio-serial clipboard channel up.\n");
}
