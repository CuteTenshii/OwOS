/* vscsi.c: virtio-scsi adapter (see scsi.h). Queue memory and the data buffer are identity-mapped pmm frames. */

#include "console.h"
#include "kstring.h"
#include "paging.h"
#include "pci.h"
#include "pmm.h"
#include "scsi.h"
#include <stdbool.h>
#include <stddef.h>

#define VIRTIO_VENDOR 0x1AF4
#define VIRTIO_DEV_SCSI 0x1048
#define VIRTIO_DEV_SCSI_TRANS 0x1004

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
#define F_VERSION_1_HI 1u /* feature bit 32 = bit 0 of the high word */

#define Q_REQUEST 2 /* queue 0 = control, 1 = event */
#define QSIZE 8
#define DESC_F_NEXT 1
#define DESC_F_WRITE 2
#define POLL_LIMIT 20000000u
#define DATA_MAX 4096

/* One page-aligned region: descriptors, avail ring, used ring, request header, response, then the data page. */
#define QMEM 0x4000
#define Q_AVAIL 0x100
#define Q_USED 0x1000
#define Q_REQ 0x2000
#define Q_RESP 0x2040
#define Q_DATA 0x3000

#define REQ_LEN 51   /* lun[8] id[8] task_attr prio crn cdb[32] */
#define RESP_LEN 108 /* sense_len resid status_qualifier status response sense[96] */
#define RESP_STATUS 10
#define RESP_RESPONSE 11

struct desc {
  uint64_t addr;
  uint32_t len;
  uint16_t flags, next;
};

static volatile uint8_t *common;
static volatile uint8_t *notify_base;
static uint32_t notify_mult;
static volatile uint16_t *notify;

static volatile struct desc *descs;
static volatile uint16_t *avail;
static volatile uint16_t *used;
static volatile uint8_t *req, *resp, *data;
static uint32_t qphys;
static uint16_t avail_idx, last_used;

static inline void wr8(uint32_t o, uint8_t v) { *(volatile uint8_t *)(common + o) = v; }
static inline void wr16(uint32_t o, uint16_t v) { *(volatile uint16_t *)(common + o) = v; }
static inline void wr32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(common + o) = v; }
static inline uint8_t rd8(uint32_t o) { return *(volatile uint8_t *)(common + o); }
static inline uint16_t rd16(uint32_t o) { return *(volatile uint16_t *)(common + o); }
static inline uint32_t rd32(uint32_t o) { return *(volatile uint32_t *)(common + o); }

static uint32_t cfg_read(const struct pci_dev_info *d, uint32_t off)
{
  return pci_read32(d->bus, d->slot, d->func, (uint8_t)off);
}

/* Map a BAR-relative window uncached; 64-bit BARs above 4 GiB are unreachable from this kernel. */
static volatile uint8_t *map_window(const struct pci_dev_info *d, uint32_t bar, uint32_t off, uint32_t len)
{
  uint32_t lo = cfg_read(d, 0x10 + bar * 4);
  if (lo & 1u)
    return NULL;
  if (((lo >> 1) & 3u) == 2 && cfg_read(d, 0x10 + (bar + 1) * 4) != 0)
    return NULL;
  uint32_t phys = (lo & 0xFFFFFFF0u) + off;
  if (paging_identity_map(phys & ~0xFFFu, len + (phys & 0xFFFu)) < 0)
    return NULL;
  paging_mark_uc(phys & ~0xFFFu, len + (phys & 0xFFFu));
  return (volatile uint8_t *)(uintptr_t)phys;
}

static bool setup_queue(void)
{
  uint32_t mem = pmm_alloc_contig(QMEM);
  if (!mem || paging_identity_map(mem, QMEM) < 0)
    return false;
  memset((void *)(uintptr_t)mem, 0, QMEM);
  qphys = mem;
  descs = (volatile struct desc *)(uintptr_t)mem;
  avail = (volatile uint16_t *)(uintptr_t)(mem + Q_AVAIL);
  used = (volatile uint16_t *)(uintptr_t)(mem + Q_USED);
  req = (volatile uint8_t *)(uintptr_t)(mem + Q_REQ);
  resp = (volatile uint8_t *)(uintptr_t)(mem + Q_RESP);
  data = (volatile uint8_t *)(uintptr_t)(mem + Q_DATA);
  wr16(CC_Q_SELECT, Q_REQUEST);
  if (rd16(CC_Q_SIZE) < QSIZE)
    return false;
  wr16(CC_Q_SIZE, QSIZE);
  wr32(CC_Q_DESC, mem);
  wr32(CC_Q_DESC + 4, 0);
  wr32(CC_Q_DRIVER, mem + Q_AVAIL);
  wr32(CC_Q_DRIVER + 4, 0);
  wr32(CC_Q_DEVICE, mem + Q_USED);
  wr32(CC_Q_DEVICE + 4, 0);
  notify = (volatile uint16_t *)(notify_base + (uint32_t)rd16(CC_Q_NOTIFY_OFF) * notify_mult);
  wr16(CC_Q_ENABLE, 1);
  return true;
}

static int vscsi_cmd(uint8_t target, const uint8_t *cdb, uint32_t cdb_len, enum scsi_dir dir, void *buf, uint32_t len)
{
  if (len > DATA_MAX || cdb_len > 32)
    return -1;
  memset((void *)req, 0, REQ_LEN);
  req[0] = 1;
  req[1] = target;
  req[2] = 0x40;
  memcpy((void *)(req + 19), cdb, cdb_len);
  memset((void *)resp, 0xFF, RESP_LEN);

  if (dir == SCSI_DIR_TO_DEV)
    memcpy((void *)data, buf, len);

  uint16_t n = 0;
  descs[n] = (struct desc){qphys + Q_REQ, REQ_LEN, DESC_F_NEXT, 1};
  n++;
  if (dir == SCSI_DIR_TO_DEV) {
    descs[n] = (struct desc){qphys + Q_DATA, len, DESC_F_NEXT, 2};
    n++;
  }
  descs[n] = (struct desc){qphys + Q_RESP, RESP_LEN, DESC_F_WRITE | (dir == SCSI_DIR_FROM_DEV ? DESC_F_NEXT : 0),
                           (uint16_t)(n + 1)};
  n++;
  if (dir == SCSI_DIR_FROM_DEV)
    descs[n] = (struct desc){qphys + Q_DATA, len, DESC_F_WRITE, 0};

  avail[2 + (avail_idx % QSIZE)] = 0;
  avail_idx++;
  __asm__ volatile("mfence" ::: "memory");
  avail[1] = avail_idx;
  __asm__ volatile("mfence" ::: "memory");
  *notify = Q_REQUEST;

  for (uint32_t i = 0; i < POLL_LIMIT; i++)
    if (used[1] != last_used) {
      last_used = used[1];
      __asm__ volatile("mfence" ::: "memory");
      if (resp[RESP_RESPONSE] != 0 || resp[RESP_STATUS] != 0)
        return -1;
      if (dir == SCSI_DIR_FROM_DEV)
        memcpy(buf, (const void *)data, len);
      return 0;
    }
  return -1;
}

static const struct scsi_hba vscsi_hba = {"virtio-scsi", vscsi_cmd};

const struct scsi_hba *vscsi_probe(void)
{
  struct pci_dev_info d;
  int idx = 0;
  while (pci_dev_info(idx, &d) &&
         !(d.vendor == VIRTIO_VENDOR && (d.device == VIRTIO_DEV_SCSI || d.device == VIRTIO_DEV_SCSI_TRANS)))
    idx++;
  if (!pci_dev_info(idx, &d))
    return NULL;

  pci_write32(d.bus, d.slot, d.func, 0x04, cfg_read(&d, 0x04) | 0x6u | 0x400u); /* polled: mask INTx */

  for (uint32_t p = cfg_read(&d, 0x34) & 0xFCu; p; p = (cfg_read(&d, p) >> 8) & 0xFCu) {
    uint32_t w0 = cfg_read(&d, p);
    if ((w0 & 0xFFu) != PCI_CAP_VENDOR)
      continue;
    uint32_t type = (w0 >> 24) & 0xFFu;
    uint32_t bar = cfg_read(&d, p + 4) & 0xFFu;
    uint32_t off = cfg_read(&d, p + 8);
    uint32_t len = cfg_read(&d, p + 12);
    if (bar > 5)
      continue;
    if (type == CAP_COMMON)
      common = map_window(&d, bar, off, len);
    else if (type == CAP_NOTIFY) {
      notify_mult = cfg_read(&d, p + 16);
      notify_base = map_window(&d, bar, off, len);
    }
  }
  if (!common || !notify_base) {
    con_write("vscsi: controller has no modern virtio-pci windows\n");
    return NULL;
  }

  wr8(CC_STATUS, 0);
  wr8(CC_STATUS, ST_ACK);
  wr8(CC_STATUS, ST_ACK | ST_DRIVER);
  wr32(CC_DEV_FEAT_SEL, 1);
  if (!(rd32(CC_DEV_FEAT) & F_VERSION_1_HI)) {
    con_write("vscsi: device lacks VIRTIO_F_VERSION_1\n");
    return NULL;
  }
  wr32(CC_DRV_FEAT_SEL, 0);
  wr32(CC_DRV_FEAT, 0);
  wr32(CC_DRV_FEAT_SEL, 1);
  wr32(CC_DRV_FEAT, F_VERSION_1_HI);
  wr8(CC_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK);
  if (!(rd8(CC_STATUS) & ST_FEATURES_OK) || !setup_queue()) {
    con_write("vscsi: setup failed\n");
    return NULL;
  }
  wr8(CC_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK | ST_DRIVER_OK);

  return &vscsi_hba;
}
