/* virtiogpu.c: virtio-gpu 2D display adapter (see virtiogpu.h).
 * Modern virtio-pci transport: the common/notify configs live in BAR windows found via vendor
 * capabilities. One polled control queue; no interrupts. */

#include "virtiogpu.h"
#include "console.h"
#include "fb.h"
#include "kstring.h"
#include "paging.h"
#include "pci.h"
#include "pmm.h"
#include "spinlock.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VIRTIO_VENDOR 0x1AF4
#define VIRTIO_DEV_GPU 0x1050

#define PCI_CAP_VENDOR 0x09
#define CAP_COMMON 1
#define CAP_NOTIFY 2

/* Common configuration offsets */
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
#define F_VERSION_1_HI 1u /* feature bit 32 = bit 0 of the high word */

#define QSIZE 8
#define DESC_F_NEXT 1
#define DESC_F_WRITE 2

enum {
  CMD_CREATE_2D = 0x101,
  CMD_SET_SCANOUT = 0x103,
  CMD_FLUSH = 0x104,
  CMD_TRANSFER_2D = 0x105,
  CMD_ATTACH_BACKING = 0x106,
  CMD_UPDATE_CURSOR = 0x300,
  CMD_MOVE_CURSOR = 0x301,
  RESP_OK_NODATA = 0x1100,
};
#define FORMAT_B8G8R8A8 1
#define FORMAT_B8G8R8X8 2
#define RESOURCE_ID 1
#define CURSOR_RESOURCE_ID 2
#define CURSOR_DIM 64 /* the host only accepts 64x64 cursor resources */
#define POLL_LIMIT 5000000

struct hdr {
  uint32_t type, flags;
  uint64_t fence_id;
  uint32_t ctx_id, pad;
};
struct rect {
  uint32_t x, y, w, h;
};
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
  uint16_t avail_idx, last_used;
};

static volatile uint8_t *common;
static struct vq ctrl, curq;
static uint8_t *cmd_buf;
static uint8_t *resp_buf;
static uint32_t cmd_phys, resp_phys;
static uint32_t gw, gh, gpitch;

static spinlock_t cursor_lock; /* the mouse IRQ moves the cursor */
static uint8_t *cur_cmd;
static uint32_t *cur_pix;
static bool cursor_ready, cursor_on;
static volatile int cursor_scale = 1;
static uint32_t flush_x0, flush_y0, flush_x1, flush_y1; /* union of the rects transferred this batch */

static uint32_t cfg_read32(int bus, int slot, int off) { return pci_read32(bus, slot, 0, (uint8_t)off); }

/* Map a BAR-relative window uncached; 64-bit BARs above 4 GiB are unreachable from this kernel. */
static volatile uint8_t *map_window(int bus, int slot, int bar, uint32_t off, uint32_t len)
{
  uint32_t lo = cfg_read32(bus, slot, 0x10 + bar * 4);
  if (lo & 1)
    return NULL;
  if (((lo >> 1) & 3) == 2 && cfg_read32(bus, slot, 0x10 + (bar + 1) * 4) != 0)
    return NULL;
  uint32_t phys = (lo & 0xFFFFFFF0u) + off;
  if (paging_identity_map(phys & ~0xFFFu, len + (phys & 0xFFF)) < 0)
    return NULL;
  paging_mark_uc(phys & ~0xFFFu, len + (phys & 0xFFF));
  return (volatile uint8_t *)(uintptr_t)phys;
}

static inline void wr8(uint32_t o, uint8_t v) { *(volatile uint8_t *)(common + o) = v; }
static inline void wr16(uint32_t o, uint16_t v) { *(volatile uint16_t *)(common + o) = v; }
static inline void wr32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(common + o) = v; }
static inline uint8_t rd8(uint32_t o) { return *(volatile uint8_t *)(common + o); }
static inline uint16_t rd16(uint32_t o) { return *(volatile uint16_t *)(common + o); }
static inline uint32_t rd32(uint32_t o) { return *(volatile uint32_t *)(common + o); }

/* Submit cmd (req_len bytes) with a resp_len-byte reply buffer and wait for the device. */
static bool submit(uint32_t req_len, uint32_t resp_len)
{
  memset(resp_buf, 0, resp_len);
  ctrl.descs[0] = (struct desc){cmd_phys, req_len, DESC_F_NEXT, 1};
  ctrl.descs[1] = (struct desc){resp_phys, resp_len, DESC_F_WRITE, 0};
  ctrl.avail[2 + (ctrl.avail_idx % QSIZE)] = 0;
  __asm__ volatile("" ::: "memory");
  ctrl.avail[1] = ++ctrl.avail_idx;
  __asm__ volatile("mfence" ::: "memory");
  *ctrl.notify = 0;
  for (int i = 0; i < POLL_LIMIT; i++) {
    if (ctrl.used[1] != ctrl.last_used) {
      ctrl.last_used = ctrl.used[1];
      return ((struct hdr *)resp_buf)->type == RESP_OK_NODATA;
    }
  }
  return false;
}

static struct hdr *begin(uint32_t type, uint32_t size)
{
  memset(cmd_buf, 0, size);
  struct hdr *h = (struct hdr *)cmd_buf;
  h->type = type;
  return h;
}

static bool cmd_transfer(uint32_t res, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t pitch)
{
  struct {
    struct hdr h;
    struct rect r;
    uint64_t offset;
    uint32_t res, pad;
  } *t = (void *)begin(CMD_TRANSFER_2D, sizeof *t);
  t->r = (struct rect){x, y, w, h};
  t->offset = (uint64_t)y * pitch + (uint64_t)x * 4;
  t->res = res;
  return submit(sizeof *t, sizeof(struct hdr));
}

static bool cmd_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
  struct {
    struct hdr h;
    struct rect r;
    uint32_t res, pad;
  } *f = (void *)begin(CMD_FLUSH, sizeof *f);
  f->r = (struct rect){x, y, w, h};
  f->res = RESOURCE_ID;
  return submit(sizeof *f, sizeof(struct hdr));
}

/* Transfer each rect now; the single flush over their union runs in vgpu_flush. */
static void vgpu_present(int x, int y, int w, int h)
{
  if (w <= 0 || h <= 0)
    return;
  __asm__ volatile("sfence" ::: "memory"); /* drain write-combined framebuffer stores before the device reads them */
  if (!cmd_transfer(RESOURCE_ID, (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, gpitch))
    return;
  if (flush_x1 <= flush_x0) {
    flush_x0 = (uint32_t)x;
    flush_y0 = (uint32_t)y;
    flush_x1 = (uint32_t)(x + w);
    flush_y1 = (uint32_t)(y + h);
    return;
  }
  flush_x0 = flush_x0 < (uint32_t)x ? flush_x0 : (uint32_t)x;
  flush_y0 = flush_y0 < (uint32_t)y ? flush_y0 : (uint32_t)y;
  flush_x1 = flush_x1 > (uint32_t)(x + w) ? flush_x1 : (uint32_t)(x + w);
  flush_y1 = flush_y1 > (uint32_t)(y + h) ? flush_y1 : (uint32_t)(y + h);
}

static void vgpu_flush(void)
{
  if (flush_x1 <= flush_x0)
    return;
  cmd_flush(flush_x0, flush_y0, flush_x1 - flush_x0, flush_y1 - flush_y0);
  flush_x1 = flush_x0;
}

/* Cursor queue commands carry no reply; the shared buffer is reused once the previous one completes. */
static void cursor_send(uint32_t type, int x, int y, uint32_t res, int hot_x, int hot_y)
{
  struct {
    struct hdr h;
    uint32_t scanout, x, y, pad;
    uint32_t res, hot_x, hot_y, pad2;
  } *c = (void *)cur_cmd;
  uintptr_t f = spin_lock_irqsave(&cursor_lock);
  for (int i = 0; i < POLL_LIMIT && curq.used[1] != curq.avail_idx; i++)
    ;
  curq.last_used = curq.used[1];
  memset(c, 0, sizeof *c);
  c->h.type = type;
  c->x = (uint32_t)x;
  c->y = (uint32_t)y;
  c->res = res;
  c->hot_x = (uint32_t)hot_x;
  c->hot_y = (uint32_t)hot_y;
  curq.descs[0] = (struct desc){(uint32_t)(uintptr_t)cur_cmd, sizeof *c, 0, 0};
  curq.avail[2 + (curq.avail_idx % QSIZE)] = 0;
  __asm__ volatile("" ::: "memory");
  curq.avail[1] = ++curq.avail_idx;
  __asm__ volatile("mfence" ::: "memory");
  *curq.notify = 1;
  spin_unlock_irqrestore(&cursor_lock, f);
}

static int vgpu_cursor_set(const uint32_t *argb, int w, int h, int hot_x, int hot_y, int scale)
{
  if (!cursor_ready)
    return -1;
  if (!argb) {
    if (cursor_on)
      cursor_send(CMD_UPDATE_CURSOR, 0, 0, 0, 0, 0);
    cursor_on = false;
    return 0;
  }
  if (w <= 0 || h <= 0 || w > CURSOR_DIM || h > CURSOR_DIM)
    return -1;
  memset(cur_pix, 0, CURSOR_DIM * CURSOR_DIM * 4);
  for (int y = 0; y < h; y++)
    memcpy(cur_pix + y * CURSOR_DIM, argb + y * w, (uint32_t)w * 4);
  __asm__ volatile("mfence" ::: "memory");
  if (!cmd_transfer(CURSOR_RESOURCE_ID, 0, 0, CURSOR_DIM, CURSOR_DIM, CURSOR_DIM * 4))
    return -1;
  cursor_scale = scale > 0 ? scale : 1;
  cursor_send(CMD_UPDATE_CURSOR, 0, 0, CURSOR_RESOURCE_ID, hot_x, hot_y);
  cursor_on = true;
  return 0;
}

static void vgpu_cursor_move(int x, int y)
{
  if (cursor_on)
    cursor_send(CMD_MOVE_CURSOR, x * cursor_scale, y * cursor_scale, CURSOR_RESOURCE_ID, 0, 0);
}

static bool setup_queue(struct vq *q, int index, uint32_t mem, volatile uint8_t *notify_base, uint32_t notify_mult)
{
  q->descs = (volatile struct desc *)(uintptr_t)mem;
  q->avail = (volatile uint16_t *)(uintptr_t)(mem + 0x100);
  q->used = (volatile uint16_t *)(uintptr_t)(mem + 0x1000);
  wr16(CC_Q_SELECT, (uint16_t)index);
  if (rd16(CC_Q_SIZE) < QSIZE)
    return false;
  wr16(CC_Q_SIZE, QSIZE);
  wr32(CC_Q_DESC, mem);
  wr32(CC_Q_DESC + 4, 0);
  wr32(CC_Q_DRIVER, mem + 0x100);
  wr32(CC_Q_DRIVER + 4, 0);
  wr32(CC_Q_DEVICE, mem + 0x1000);
  wr32(CC_Q_DEVICE + 4, 0);
  q->notify = (volatile uint16_t *)(notify_base + (uint32_t)rd16(CC_Q_NOTIFY_OFF) * notify_mult);
  wr16(CC_Q_ENABLE, 1);
  return true;
}

/* Back resource res with n bytes of guest RAM at phys. */
static bool create_backed(uint32_t res, uint32_t format, uint32_t w, uint32_t h, uint32_t phys, uint32_t bytes)
{
  struct {
    struct hdr h;
    uint32_t res, format, w, hh;
  } *c = (void *)begin(CMD_CREATE_2D, sizeof *c);
  c->res = res;
  c->format = format;
  c->w = w;
  c->hh = h;
  if (!submit(sizeof *c, sizeof(struct hdr)))
    return false;

  struct {
    struct hdr h;
    uint32_t res, nr;
    uint64_t addr;
    uint32_t len, pad;
  } *a = (void *)begin(CMD_ATTACH_BACKING, sizeof *a);
  a->res = res;
  a->nr = 1;
  a->addr = phys;
  a->len = bytes;
  return submit(sizeof *a, sizeof(struct hdr));
}

int virtiogpu_init(uint32_t want_w, uint32_t want_h)
{
  int bus = 0, slot = 0;
  bool found = false;
  for (bus = 0; bus < 256 && !found; bus++)
    for (slot = 0; slot < 32; slot++) {
      uint32_t id = cfg_read32(bus, slot, 0);
      if ((id & 0xFFFF) == VIRTIO_VENDOR && ((id >> 16) & 0xFFFF) == VIRTIO_DEV_GPU) {
        found = true;
        break;
      }
    }
  if (!found)
    return -1;
  bus--;

  uint32_t cmd = cfg_read32(bus, slot, 0x04);
  pci_write32(bus, slot, 0, 0x04, cmd | 0x6);

  uint32_t notify_mult = 0;
  common = NULL;
  volatile uint8_t *notify_base = NULL;
  for (uint32_t p = cfg_read32(bus, slot, 0x34) & 0xFC; p; p = (cfg_read32(bus, slot, (int)p) >> 8) & 0xFC) {
    uint32_t w0 = cfg_read32(bus, slot, (int)p);
    if ((w0 & 0xFF) != PCI_CAP_VENDOR)
      continue;
    uint32_t type = (w0 >> 24) & 0xFF;
    uint32_t bar = cfg_read32(bus, slot, (int)p + 4) & 0xFF;
    uint32_t off = cfg_read32(bus, slot, (int)p + 8);
    uint32_t len = cfg_read32(bus, slot, (int)p + 12);
    if (bar > 5)
      continue;
    if (type == CAP_COMMON)
      common = map_window(bus, slot, (int)bar, off, len);
    else if (type == CAP_NOTIFY) {
      notify_mult = cfg_read32(bus, slot, (int)p + 16);
      notify_base = map_window(bus, slot, (int)bar, off, len);
    }
  }
  if (!common || !notify_base) {
    con_write("virtio-gpu: transport capabilities missing.\n");
    return -1;
  }
  con_write("virtio-gpu: device found (1af4:1050).\n");

  wr8(CC_STATUS, 0);
  wr8(CC_STATUS, ST_ACK);
  wr8(CC_STATUS, ST_ACK | ST_DRIVER);
  wr32(CC_DEV_FEAT_SEL, 1);
  if (!(rd32(CC_DEV_FEAT) & F_VERSION_1_HI)) {
    con_write("virtio-gpu: legacy-only device, unsupported.\n");
    return -1;
  }
  wr32(CC_DRV_FEAT_SEL, 0);
  wr32(CC_DRV_FEAT, 0);
  wr32(CC_DRV_FEAT_SEL, 1);
  wr32(CC_DRV_FEAT, F_VERSION_1_HI);
  wr8(CC_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK);
  if (!(rd8(CC_STATUS) & ST_FEATURES_OK)) {
    con_write("virtio-gpu: feature negotiation rejected.\n");
    return -1;
  }

  /* Control queue: descriptors + avail page, used page, then the command and reply buffers. */
  uint32_t mem = pmm_alloc_contig(0x4000);
  if (!mem || paging_identity_map(mem, 0x4000) < 0)
    return -1;
  memset((void *)(uintptr_t)mem, 0, 0x4000);
  cmd_phys = mem + 0x2000;
  resp_phys = mem + 0x3000;
  cmd_buf = (uint8_t *)(uintptr_t)cmd_phys;
  resp_buf = (uint8_t *)(uintptr_t)resp_phys;
  if (!setup_queue(&ctrl, 0, mem, notify_base, notify_mult)) {
    con_write("virtio-gpu: control queue too small.\n");
    return -1;
  }

  /* Cursor queue (1) and its 64x64 ARGB image; optional, the software cursor remains without it. */
  uint32_t cmem = pmm_alloc_contig(0x3000 + CURSOR_DIM * CURSOR_DIM * 4);
  if (cmem && paging_identity_map(cmem, 0x3000 + CURSOR_DIM * CURSOR_DIM * 4) == 0) {
    memset((void *)(uintptr_t)cmem, 0, 0x3000 + CURSOR_DIM * CURSOR_DIM * 4);
    cur_cmd = (uint8_t *)(uintptr_t)(cmem + 0x2000);
    cur_pix = (uint32_t *)(uintptr_t)(cmem + 0x3000);
    cursor_ready = setup_queue(&curq, 1, cmem, notify_base, notify_mult);
  }
  wr8(CC_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK | ST_DRIVER_OK);

  gw = want_w;
  gh = want_h;
  gpitch = want_w * 4;
  uint32_t fb_bytes = gpitch * gh;
  uint32_t fb_phys = pmm_alloc_contig(fb_bytes);
  if (!fb_phys)
    return -1;

  if (!create_backed(RESOURCE_ID, FORMAT_B8G8R8X8, gw, gh, fb_phys, fb_bytes)) {
    con_write("virtio-gpu: framebuffer resource setup failed.\n");
    return -1;
  }
  if (cursor_ready && !create_backed(CURSOR_RESOURCE_ID, FORMAT_B8G8R8A8, CURSOR_DIM, CURSOR_DIM, cmem + 0x3000,
                                     CURSOR_DIM * CURSOR_DIM * 4))
    cursor_ready = false;

  struct {
    struct hdr h;
    struct rect r;
    uint32_t scanout, res;
  } *s = (void *)begin(CMD_SET_SCANOUT, sizeof *s);
  s->r = (struct rect){0, 0, gw, gh};
  s->res = RESOURCE_ID;
  if (!submit(sizeof *s, sizeof(struct hdr))) {
    con_write("virtio-gpu: set scanout failed.\n");
    return -1;
  }

  con_printf("virtio-gpu: mode %ux%ux32.\n", gw, gh);
  fb_set_present_hook(vgpu_present, vgpu_flush);
  if (cursor_ready)
    fb_set_cursor_hooks(vgpu_cursor_set, vgpu_cursor_move);
  if (fb_adopt(fb_phys, gw, gh, gpitch, 32) != 0) {
    con_write("virtio-gpu: fb_adopt failed.\n");
    fb_set_present_hook(NULL, NULL);
    fb_set_cursor_hooks(NULL, NULL);
    return -1;
  }
  return 0;
}
