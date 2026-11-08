/* virtioserial.c: virtio-serial transport (see virtioserial.h). */

#include "virtioserial.h"
#include "console.h"
#include "kstring.h"
#include "paging.h"
#include "pci.h"
#include "pmm.h"
#include "sched.h"
#include "timer.h"
#include <stddef.h>

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

#define ST_ACK 1u
#define ST_DRIVER 2u
#define ST_DRIVER_OK 4u
#define ST_FEATURES_OK 8u
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
#define POLL_MS 20

#define Q_CTL_RX 2
#define Q_CTL_TX 3

enum { CTL_DEVICE_READY = 0, CTL_DEVICE_ADD = 1, CTL_PORT_READY = 3, CTL_PORT_OPEN = 6, CTL_PORT_NAME = 7 };

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

struct port {
  const struct vserial_client *client;
  struct vq rx, tx;
  bool usable, present, host_open, guest_open;
};

static const struct vserial_client *const clients[] = {&vdagent_client, &qemuga_client};
#define NPORTS (sizeof clients / sizeof clients[0])

static volatile uint8_t *common;
static volatile uint8_t *notify_base;
static uint32_t notify_mult;
static struct vq ctl_rx, ctl_tx;
static struct port ports[NPORTS];

static inline void wr8(uint32_t o, uint8_t v) { *(volatile uint8_t *)(common + o) = v; }
static inline void wr16(uint32_t o, uint16_t v) { *(volatile uint16_t *)(common + o) = v; }
static inline void wr32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(common + o) = v; }
static inline uint8_t rd8(uint32_t o) { return *(volatile uint8_t *)(common + o); }
static inline uint16_t rd16(uint32_t o) { return *(volatile uint16_t *)(common + o); }
static inline uint32_t rd32(uint32_t o) { return *(volatile uint32_t *)(common + o); }

static inline uint32_t le32(const uint8_t *p)
{
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline void put32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static uint32_t cfg_read(int bus, int slot, uint32_t off)
{
  return pci_read32((uint8_t)bus, (uint8_t)slot, 0, (uint8_t)off);
}

/* Map a BAR-relative window uncached; 64-bit BARs above 4 GiB are unreachable from this kernel. */
static volatile uint8_t *map_window(int bus, int slot, uint32_t bar, uint32_t off, uint32_t len)
{
  uint32_t lo = cfg_read(bus, slot, 0x10 + bar * 4);
  if (lo & 1u)
    return NULL;
  if (((lo >> 1) & 3u) == 2 && cfg_read(bus, slot, 0x10 + (bar + 1) * 4) != 0)
    return NULL;
  uint32_t phys = (lo & 0xFFFFFFF0u) + off;
  if (paging_identity_map(phys & ~0xFFFu, len + (phys & 0xFFFu)) < 0)
    return NULL;
  paging_mark_uc(phys & ~0xFFFu, len + (phys & 0xFFFu));
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

static struct port *port_by_id(uint32_t id)
{
  for (size_t i = 0; i < NPORTS; i++)
    if (clients[i]->port == id && ports[i].usable)
      return &ports[i];
  return NULL;
}

static void ctl_rx_msg(const uint8_t *d, uint32_t len)
{
  if (len < 8)
    return;
  struct port *p = port_by_id(le32(d));
  if (!p)
    return;
  uint32_t port = p->client->port;
  uint32_t event = (uint32_t)d[4] | (uint32_t)d[5] << 8, value = (uint32_t)d[6] | (uint32_t)d[7] << 8;
  switch (event) {
  case CTL_DEVICE_ADD:
    p->present = true;
    ctl_send(port, CTL_PORT_READY, 1);
    break;
  case CTL_PORT_NAME: {
    uint32_t n = strlen(p->client->name);
    if (len - 8 >= n && memcmp(d + 8, p->client->name, n) == 0) {
      p->guest_open = true;
      ctl_send(port, CTL_PORT_OPEN, 1);
    }
    break;
  }
  case CTL_PORT_OPEN:
    p->host_open = value != 0;
    break;
  default:
    break;
  }
}

bool vserial_port_present(uint32_t port)
{
  struct port *p = port_by_id(port);
  return p && p->present;
}

bool vserial_port_open(uint32_t port)
{
  struct port *p = port_by_id(port);
  return p && p->host_open && p->guest_open;
}

bool vserial_write(uint32_t port, const void *data, uint32_t len)
{
  struct port *p = port_by_id(port);
  if (!p || !p->host_open || !p->guest_open || len > TX_BUF)
    return false;
  memcpy(p->tx.buf, data, len);
  return tx_submit(&p->tx, len);
}

static void vserial_thread(void)
{
  for (;;) {
    timer_sleep(POLL_MS);
    rx_drain(&ctl_rx, ctl_rx_msg);
    for (size_t i = 0; i < NPORTS; i++)
      if (ports[i].usable) {
        rx_drain(&ports[i].rx, ports[i].client->rx);
        if (ports[i].client->tick)
          ports[i].client->tick();
      }
  }
}

void vserial_init(void)
{
  int bus, slot = 0;
  bool found = false;
  for (bus = 0; bus < 256 && !found; bus++)
    for (slot = 0; slot < 32; slot++) {
      uint32_t id = cfg_read(bus, slot, 0);
      uint32_t dev = id >> 16;
      if ((id & 0xFFFFu) == VIRTIO_VENDOR && (dev == VIRTIO_DEV_SERIAL || dev == VIRTIO_DEV_SERIAL_TRANS)) {
        found = true;
        break;
      }
    }
  if (!found)
    return;
  bus--;

  pci_write32((uint8_t)bus, (uint8_t)slot, 0, 0x04, cfg_read(bus, slot, 0x04) | 0x6u | 0x400u); /* polled: mask INTx */

  for (uint32_t p = cfg_read(bus, slot, 0x34) & 0xFCu; p; p = (cfg_read(bus, slot, p) >> 8) & 0xFCu) {
    uint32_t w0 = cfg_read(bus, slot, p);
    if ((w0 & 0xFFu) != PCI_CAP_VENDOR)
      continue;
    uint32_t type = (w0 >> 24) & 0xFFu;
    uint32_t bar = cfg_read(bus, slot, p + 4) & 0xFFu;
    uint32_t off = cfg_read(bus, slot, p + 8);
    uint32_t len = cfg_read(bus, slot, p + 12);
    if (bar > 5)
      continue;
    if (type == CAP_COMMON)
      common = map_window(bus, slot, bar, off, len);
    else if (type == CAP_NOTIFY) {
      notify_mult = cfg_read(bus, slot, p + 16);
      notify_base = map_window(bus, slot, bar, off, len);
    }
  }
  if (!common || !notify_base)
    return;

  wr8(CC_STATUS, 0);
  wr8(CC_STATUS, ST_ACK);
  wr8(CC_STATUS, ST_ACK | ST_DRIVER);
  wr32(CC_DEV_FEAT_SEL, 0);
  bool multiport = (rd32(CC_DEV_FEAT) & F_MULTIPORT) != 0;
  wr32(CC_DEV_FEAT_SEL, 1);
  bool v1 = (rd32(CC_DEV_FEAT) & F_VERSION_1_HI) != 0;
  if (!multiport || !v1)
    return;
  wr32(CC_DRV_FEAT_SEL, 0);
  wr32(CC_DRV_FEAT, F_MULTIPORT);
  wr32(CC_DRV_FEAT_SEL, 1);
  wr32(CC_DRV_FEAT, F_VERSION_1_HI);
  wr8(CC_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK);
  if (!(rd8(CC_STATUS) & ST_FEATURES_OK))
    return;

  if (!setup_queue(&ctl_rx, Q_CTL_RX) || !setup_queue(&ctl_tx, Q_CTL_TX)) {
    con_write("virtio-serial: control queue setup failed.\n");
    return;
  }
  rx_fill(&ctl_rx);
  for (size_t i = 0; i < NPORTS; i++) {
    struct port *p = &ports[i];
    p->client = clients[i];
    uint16_t q = (uint16_t)(2 * clients[i]->port + 2);
    p->usable = setup_queue(&p->rx, q) && setup_queue(&p->tx, (uint16_t)(q + 1));
    if (p->usable)
      rx_fill(&p->rx);
  }
  wr8(CC_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK | ST_DRIVER_OK);
  ctl_send(0, CTL_DEVICE_READY, 1);
  con_write("virtio-serial: host agent channels up.\n");
  sched_create_thread("vserial", vserial_thread);
}
