/* nvme.c — minimal polled NVMe block driver (admin + one I/O queue pair).
 *
 * Bring-up follows the NVMe spec's controller-init sequence:
 *   1. Find the controller on PCI (class 0x01 / subclass 0x08) and map its
 *      MMIO register block (BAR0) — it sits above the 16 MiB identity map.
 *   2. Disable the controller (CC.EN=0, wait CSTS.RDY=0), program the admin
 *      submission/completion queues (AQA/ASQ/ACQ), then enable it and wait for
 *      CSTS.RDY=1.
 *   3. Over the admin queue: create an I/O completion queue, an I/O submission
 *      queue, and IDENTIFY namespace 1 to learn its capacity.
 *   4. Per I/O: post a Read/Write command on the I/O queue with PRP1 pointing
 *      at a bounce buffer, ring the submission doorbell, and poll the
 *      completion queue's phase bit.
 *
 * All queues and buffers are page-aligned frames from the identity-mapped pool
 * (palloc), so physical == virtual — exactly the addresses the controller DMAs
 * to. Polled and synchronous: one command in flight at a time, which is all the
 * single-threaded shell/fs need. Every wait is bounded so a wedged controller
 * returns -1 instead of hanging.
 */

#include "nvme.h"
#include "console.h"
#include "paging.h"
#include "pci.h"
#include "spinlock.h"

#define NVME_CLASS 0x01
#define NVME_SUBCLASS 0x08

/* Controller registers (byte offsets into BAR0). */
#define REG_CAP 0x00        /* capabilities (64-bit)                          */
#define REG_CC 0x14         /* controller configuration                       */
#define REG_CSTS 0x1C       /* controller status                              */
#define REG_AQA 0x24        /* admin queue attributes                         */
#define REG_ASQ 0x28        /* admin submission queue base (64-bit)           */
#define REG_ACQ 0x30        /* admin completion queue base (64-bit)           */
#define REG_DOORBELL 0x1000 /* SQ0 tail doorbell; the rest follow by stride */

#define CC_EN 0x1
#define CSTS_RDY 0x1
#define CSTS_CFS 0x2 /* controller fatal status                        */

#define QDEPTH 8 /* entries per queue (fits in one 4 KiB frame)    */

/* Admin opcodes. */
#define ADM_CREATE_SQ 0x01
#define ADM_CREATE_CQ 0x05
#define ADM_IDENTIFY 0x06

/* NVM (I/O) opcodes. */
#define IO_WRITE 0x01
#define IO_READ 0x02

#define POLL_LIMIT 5000000u

/* --------------------------------------------------------------------- state
 */

static volatile uint32_t *bar; /* mapped MMIO register block                */
static int found;
static uint32_t dstrd; /* doorbell stride exponent (CAP bits 35:32) */
static uint32_t cap_sectors;
static uint16_t next_cid;
static char model[41]; /* controller model from Identify            */

struct queue {
  volatile uint32_t *sq; /* submission entries, 16 dwords each            */
  volatile uint32_t *cq; /* completion entries, 4 dwords each             */
  uint32_t sq_tail;
  uint32_t cq_head;
  int phase;      /* expected phase-tag for the next completion    */
  uint32_t sq_db; /* submission tail doorbell (byte offset)        */
  uint32_t cq_db; /* completion head doorbell (byte offset)        */
};

static struct queue admin_q, io_q;

static volatile uint8_t *dbuf; /* 4 KiB page: DMA bounce + identify buffer  */
static spinlock_t io_lock;

/* ----------------------------------------------------------------- MMIO + I/O
 */

static inline uint32_t reg_rd(uint32_t off) { return bar[off / 4]; }
static inline void reg_wr(uint32_t off, uint32_t v) { bar[off / 4] = v; }

/* Doorbell byte offset for queue y (sq=tail at even index, cq=head at odd). */
static uint32_t sq_doorbell(uint32_t y) { return REG_DOORBELL + (2 * y) * (4u << dstrd); }
static uint32_t cq_doorbell(uint32_t y) { return REG_DOORBELL + (2 * y + 1) * (4u << dstrd); }

/* Submit one 16-dword command and poll its completion. Returns the status-field
 * value (0 = success) or 0xFFFF on timeout. */
static uint32_t submit(struct queue *q, const uint32_t cmd[16])
{
  volatile uint32_t *slot = q->sq + q->sq_tail * 16;
  for (int i = 0; i < 16; i++)
    slot[i] = cmd[i];

  q->sq_tail = (q->sq_tail + 1) % QDEPTH;
  reg_wr(q->sq_db, q->sq_tail); /* ring submission doorbell        */

  volatile uint32_t *cqe = q->cq + q->cq_head * 4;
  uint32_t i;
  for (i = 0; i < POLL_LIMIT; i++)
    if (((cqe[3] >> 16) & 1) == (uint32_t)q->phase)
      break;
  if (i == POLL_LIMIT)
    return 0xFFFF;

  uint32_t status = (cqe[3] >> 17) & 0x7FF; /* status field of the completion */

  q->cq_head = (q->cq_head + 1) % QDEPTH;
  if (q->cq_head == 0)
    q->phase ^= 1;              /* phase flips each ring wrap      */
  reg_wr(q->cq_db, q->cq_head); /* ring completion doorbell        */
  return status;
}

static void build(uint32_t cmd[16], uint8_t opcode, uint32_t nsid, uint32_t prp1)
{
  for (int i = 0; i < 16; i++)
    cmd[i] = 0;
  cmd[0] = opcode | ((uint32_t)(next_cid++) << 16); /* opcode + command id */
  cmd[1] = nsid;                                    /* namespace id            */
  cmd[6] = prp1;                                    /* PRP1 (data/list ptr)    */
  cmd[7] = 0;                                       /* PRP1 high               */
}

/* ---------------------------------------------------------------- I/O path */

static int rw(uint32_t lba, uint32_t count, int write)
{
  if (count == 0 || count > 8)
    return -1;
  uint32_t cmd[16];
  build(cmd, write ? IO_WRITE : IO_READ, 1, (uint32_t)(uintptr_t)dbuf);
  cmd[10] = lba;       /* CDW10: starting LBA (low)       */
  cmd[11] = 0;         /* CDW11: starting LBA (high)      */
  cmd[12] = count - 1; /* CDW12: NLB = count - 1            */
  return submit(&io_q, cmd) == 0 ? 0 : -1;
}

int nvme_read_sector(uint32_t lba, void *buf) { return nvme_read(lba, buf, 1); }

int nvme_write_sector(uint32_t lba, const void *buf) { return nvme_write(lba, buf, 1); }

int nvme_read(uint32_t lba, void *buf, uint32_t count)
{
  if (!found || count == 0 || count > 8)
    return -1;
  uintptr_t flags = spin_lock_irqsave(&io_lock);
  if (rw(lba, count, 0)) {
    spin_unlock_irqrestore(&io_lock, flags);
    return -1;
  }
  uint8_t *d = buf;
  for (uint32_t i = 0; i < count * 512; i++)
    d[i] = dbuf[i];
  spin_unlock_irqrestore(&io_lock, flags);
  return 0;
}

int nvme_write(uint32_t lba, const void *buf, uint32_t count)
{
  if (!found || count == 0 || count > 8)
    return -1;
  uintptr_t flags = spin_lock_irqsave(&io_lock);
  const uint8_t *s = buf;
  for (uint32_t i = 0; i < count * 512; i++)
    dbuf[i] = s[i];
  int rc = rw(lba, count, 1);
  spin_unlock_irqrestore(&io_lock, flags);
  return rc;
}

int nvme_present(void) { return found; }
uint32_t nvme_sectors(void) { return cap_sectors; }
const char *nvme_model(void) { return model; }

/* --------------------------------------------------------------------- probe
 */

static int find_controller(uint8_t *bus, uint8_t *slot, uint8_t *func)
{
  for (int b = 0; b < 256; b++)
    for (int s = 0; s < 32; s++) {
      if ((pci_read32((uint8_t)b, (uint8_t)s, 0, 0x00) & 0xFFFF) == 0xFFFF)
        continue;
      for (int f = 0; f < 8; f++) {
        if ((pci_read32((uint8_t)b, (uint8_t)s, (uint8_t)f, 0x00) & 0xFFFF) == 0xFFFF)
          continue;
        uint32_t cls = pci_read32((uint8_t)b, (uint8_t)s, (uint8_t)f, 0x08);
        if (((cls >> 24) & 0xFF) == NVME_CLASS && ((cls >> 16) & 0xFF) == NVME_SUBCLASS) {
          *bus = (uint8_t)b;
          *slot = (uint8_t)s;
          *func = (uint8_t)f;
          return 1;
        }
      }
    }
  return 0;
}

static int wait_ready(uint32_t want) /* want = CSTS.RDY value to await  */
{
  for (uint32_t i = 0; i < POLL_LIMIT; i++) {
    uint32_t csts = reg_rd(REG_CSTS);
    if (csts & CSTS_CFS)
      return -1; /* controller fatal                */
    if ((csts & CSTS_RDY) == want)
      return 0;
  }
  return -1;
}

void nvme_init(void)
{
  uint8_t bus, slot, func;
  if (!find_controller(&bus, &slot, &func)) {
    con_write("nvme: no controller found\n");
    return;
  }

  uint32_t bar0 = pci_read32(bus, slot, func, 0x10) & 0xFFFFFFF0u;
  if (!bar0) {
    con_write("nvme: controller has no MMIO base\n");
    return;
  }

  uint32_t cmd = pci_read32(bus, slot, func, 0x04);
  pci_write32(bus, slot, func, 0x04, cmd | 0x06); /* memory space + bus master */

  if (paging_identity_map(bar0, 0x2000) != 0) {
    con_write("nvme: could not map MMIO\n");
    return;
  }
  /* Registers are MMIO: mark the BAR uncached or, on real silicon, a doorbell
   * write can sit in the write-back cache and the controller never sees the
   * submission (the polled completion then times out). QEMU hides this. */
  paging_mark_uc(bar0, 0x2000);
  bar = (volatile uint32_t *)(uintptr_t)bar0;

  dstrd = reg_rd(REG_CAP + 4) & 0xF; /* CAP bits 35:32 = doorbell stride */

  /* Disable the controller before reprogramming the admin queues. */
  reg_wr(REG_CC, reg_rd(REG_CC) & ~CC_EN);
  if (wait_ready(0)) {
    con_write("nvme: controller did not quiesce\n");
    return;
  }

  /* Admin queue: one frame each (zeroed by palloc -> CQ phase starts 0). */
  admin_q.sq = palloc();
  admin_q.cq = palloc();
  io_q.sq = palloc();
  io_q.cq = palloc();
  dbuf = palloc();
  if (!admin_q.sq || !admin_q.cq || !io_q.sq || !io_q.cq || !dbuf) {
    con_write("nvme: out of DMA frames\n");
    return;
  }
  admin_q.sq_tail = admin_q.cq_head = 0;
  admin_q.phase = 1; /* first valid completion has P=1  */
  admin_q.sq_db = sq_doorbell(0);
  admin_q.cq_db = cq_doorbell(0);

  reg_wr(REG_AQA, ((QDEPTH - 1) << 16) | (QDEPTH - 1));
  reg_wr(REG_ASQ, (uint32_t)(uintptr_t)admin_q.sq);
  reg_wr(REG_ASQ + 4, 0);
  reg_wr(REG_ACQ, (uint32_t)(uintptr_t)admin_q.cq);
  reg_wr(REG_ACQ + 4, 0);

  /* CC: IOSQES=6 (64-B SQ entries), IOCQES=4 (16-B CQ entries), MPS=0, EN=1. */
  reg_wr(REG_CC, (6 << 16) | (4 << 20) | CC_EN);
  if (wait_ready(CSTS_RDY)) {
    con_write("nvme: controller did not become ready\n");
    return;
  }

  /* Create the I/O completion queue (qid 1), then its submission queue. */
  io_q.sq_tail = io_q.cq_head = 0;
  io_q.phase = 1;
  io_q.sq_db = sq_doorbell(1);
  io_q.cq_db = cq_doorbell(1);

  uint32_t c[16];
  build(c, ADM_CREATE_CQ, 0, (uint32_t)(uintptr_t)io_q.cq);
  c[10] = ((QDEPTH - 1) << 16) | 1; /* CDW10: queue size-1 | qid=1      */
  c[11] = 1;                        /* CDW11: PC=1, interrupts off      */
  if (submit(&admin_q, c) != 0) {
    con_write("nvme: create CQ failed\n");
    return;
  }

  build(c, ADM_CREATE_SQ, 0, (uint32_t)(uintptr_t)io_q.sq);
  c[10] = ((QDEPTH - 1) << 16) | 1; /* CDW10: queue size-1 | qid=1      */
  c[11] = (1u << 16) | 1;           /* CDW11: CQID=1, PC=1              */
  if (submit(&admin_q, c) != 0) {
    con_write("nvme: create SQ failed\n");
    return;
  }

  /* IDENTIFY controller (CNS=1): model number is ASCII at bytes 24..63. */
  build(c, ADM_IDENTIFY, 0, (uint32_t)(uintptr_t)dbuf);
  c[10] = 1; /* CNS = 1: identify controller     */
  model[0] = '\0';
  if (submit(&admin_q, c) == 0) {
    int n = 0;
    for (int i = 0; i < 40; i++) /* not byte-swapped, space-padded   */
      model[n++] = (char)dbuf[24 + i];
    while (n > 0 && model[n - 1] == ' ')
      n--;
    model[n] = '\0';
  }

  /* IDENTIFY namespace 1 (CNS=0): NSZE (bytes 0-7) is the block count. */
  build(c, ADM_IDENTIFY, 1, (uint32_t)(uintptr_t)dbuf);
  c[10] = 0; /* CNS = 0: identify namespace      */
  if (submit(&admin_q, c) == 0) {
    const volatile uint32_t *idns = (const volatile uint32_t *)dbuf;
    cap_sectors = idns[0]; /* low 32 bits of NSZE is plenty    */
    /* The block layer addresses this device in 512-byte sectors. Confirm the
     * namespace is actually 512-byte formatted: FLBAS (byte 26) selects an LBA
     * format whose LBADS (2^n byte size) is bits 16-23 of the entry at
     * byte 128 + 4*index. A 4K-formatted drive would otherwise report 8x the
     * capacity and every read would return a fraction of a block. */
    uint32_t flbas = ((const volatile uint8_t *)dbuf)[26] & 0xF;
    uint32_t lbads = (idns[(128 / 4) + flbas] >> 16) & 0xFF;
    if (lbads != 9) {
      con_printf("nvme: unsupported LBA size %u bytes; ignoring device\n", 1u << lbads);
      return; /* leave found=0 so blk won't use it */
    }
  }

  found = 1;
  con_write("nvme: namespace 1 ready");
  if (model[0]) {
    con_printf(" [%s]", model);
  }
  if (cap_sectors) {
    con_printf(", %u MiB", cap_sectors / 2048); /* 512-B blocks -> MiB */
  }
  con_putc('\n');
}
