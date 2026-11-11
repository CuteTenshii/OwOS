/* ufs.c: polled UFSHCI driver (see ufs.h). One transfer request slot, one request in flight, interrupts masked.
 * One identity-mapped region holds the request lists, the command descriptor and an 8 KiB data bounce buffer. */

#include "ufs.h"
#include "console.h"
#include "kstring.h"
#include "paging.h"
#include "pci.h"
#include "pmm.h"
#include "spinlock.h"
#include <stdbool.h>
#include <stddef.h>

#define UFS_CLASS 0x01
#define UFS_SUBCLASS 0x09

#define REG_VER 0x08
#define REG_IS 0x20
#define REG_IE 0x24
#define REG_HCS 0x30
#define REG_HCE 0x34
#define REG_UTRLBA 0x50
#define REG_UTRLBAU 0x54
#define REG_UTRLDBR 0x58
#define REG_UTRLCLR 0x5C
#define REG_UTRLRSR 0x60
#define REG_UTMRLBA 0x70
#define REG_UTMRLBAU 0x74
#define REG_UTMRLRSR 0x80
#define REG_UICCMD 0x90
#define REG_UCMDARG1 0x94
#define REG_UCMDARG2 0x98
#define REG_UCMDARG3 0x9C
#define MMIO_LEN 0x1000

#define HCS_DP (1u << 0)
#define HCS_UTRLRDY (1u << 1)
#define HCS_UTMRLRDY (1u << 2)
#define HCS_UCRDY (1u << 3)
#define IS_UCCS (1u << 10)
#define HCE_ENABLE 1u
#define UIC_DME_LINKSTARTUP 0x16

#define UTRD_CT_UFS_STORAGE (1u << 28)
#define UTRD_DD_SHIFT 25
#define DD_NONE 0u
#define DD_TO_DEV 1u
#define DD_FROM_DEV 2u
#define OCS_SUCCESS 0x0
#define OCS_INVALID 0xF

#define UPIU_NOP_OUT 0x00
#define UPIU_COMMAND 0x01
#define UPIU_QUERY_REQ 0x16
#define UPIU_NOP_IN 0x20
#define UPIU_RESPONSE 0x21
#define UPIU_QUERY_RSP 0x36
#define UPIU_FLAG_WRITE 0x20
#define UPIU_FLAG_READ 0x40
#define QUERY_FUNC_READ 0x01
#define QUERY_FUNC_WRITE 0x81
#define QUERY_OP_READ_FLAG 0x05
#define QUERY_OP_SET_FLAG 0x06
#define FLAG_FDEVICEINIT 0x01

#define SCSI_TUR 0x00
#define SCSI_INQUIRY 0x12
#define SCSI_READ_CAP10 0x25
#define SCSI_READ10 0x28
#define SCSI_WRITE10 0x2A
#define SCSI_GOOD 0x00

#define MAX_LUN 8
#define MAX_SECTORS 8
#define POLL_LIMIT 5000000u
#define INIT_POLLS 1000u

#define MEM_LEN 0x4000
#define OFF_UTRL 0x0000  /* 1 KiB aligned */
#define OFF_UTMRL 0x0400 /* 1 KiB aligned */
#define OFF_UCD 0x1000   /* 128-byte aligned */
#define UCD_RSP 0x200
#define UCD_PRDT 0x400
#define UPIU_SIZE 0x200
#define OFF_DATA 0x2000
#define DATA_MAX 0x2000

static volatile uint32_t *mmio;
static uint32_t mem;
static volatile uint32_t *utrd;
static volatile uint8_t *cmd_upiu, *rsp_upiu;
static volatile uint32_t *prdt;
static volatile uint8_t *data;
static uint8_t task_tag;

static bool found;
static uint8_t lun;
static uint32_t block_size;
static uint32_t cap_sectors;
static char model[33];
static spinlock_t io_lock;

int ufs_present(void) { return found; }
uint32_t ufs_sectors(void) { return cap_sectors; }
const char *ufs_model(void) { return model; }
uint8_t ufs_lun(void) { return lun; }

static inline void reg_wr(uint32_t off, uint32_t v) { mmio[off / 4] = v; }
static inline uint32_t reg_rd(uint32_t off) { return mmio[off / 4]; }
static inline void fence(void) { __asm__ volatile("mfence" ::: "memory"); }

static void put_be32(volatile uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static uint32_t be32(const volatile uint8_t *p)
{
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static bool wait_set(uint32_t off, uint32_t mask, uint32_t want)
{
  for (uint32_t i = 0; i < POLL_LIMIT; i++)
    if ((reg_rd(off) & mask) == want)
      return true;
  return false;
}

/* Start a new request UPIU of `type` in the command descriptor and return its task tag. */
static uint8_t begin_upiu(uint8_t type)
{
  memset((void *)cmd_upiu, 0, UPIU_SIZE);
  memset((void *)rsp_upiu, 0, UPIU_SIZE);
  cmd_upiu[0] = type;
  cmd_upiu[3] = ++task_tag;
  return task_tag;
}

/* Ring slot 0 with the UPIU already in the command descriptor and poll it to completion. */
static int transfer(uint32_t dd, uint32_t len)
{
  if (len > DATA_MAX)
    return -1;
  utrd[0] = UTRD_CT_UFS_STORAGE | dd << UTRD_DD_SHIFT;
  utrd[1] = 0;
  utrd[2] = OCS_INVALID;
  utrd[3] = 0;
  utrd[4] = mem + OFF_UCD;
  utrd[5] = 0;
  utrd[6] = (UCD_RSP / 4) << 16 | UPIU_SIZE / 4;
  utrd[7] = (UCD_PRDT / 4) << 16 | (len ? 1u : 0u);
  prdt[0] = mem + OFF_DATA;
  prdt[1] = 0;
  prdt[2] = 0;
  prdt[3] = ((len + 3) & ~3u) - 1; /* byte count minus one, dword granular */

  fence();
  reg_wr(REG_UTRLDBR, 1);
  if (!wait_set(REG_UTRLDBR, 1, 0)) {
    reg_wr(REG_UTRLCLR, ~1u); /* a 0 bit clears that slot */
    con_write("ufs: request timed out\n");
    return -1;
  }
  fence();
  reg_wr(REG_IS, reg_rd(REG_IS));
  if ((utrd[2] & 0xF) != OCS_SUCCESS)
    return -1;
  if (rsp_upiu[3] != cmd_upiu[3])
    return -1;
  return 0;
}

static int nop_ping(void)
{
  begin_upiu(UPIU_NOP_OUT);
  if (transfer(DD_NONE, 0) != 0)
    return -1;
  return rsp_upiu[0] == UPIU_NOP_IN && rsp_upiu[6] == 0 ? 0 : -1;
}

/* Run a flag query; on success the flag's value is in *value. */
static int query_flag(uint8_t func, uint8_t opcode, uint8_t idn, uint8_t *value)
{
  begin_upiu(UPIU_QUERY_REQ);
  cmd_upiu[5] = func;
  cmd_upiu[12] = opcode;
  cmd_upiu[13] = idn;
  if (transfer(DD_NONE, 0) != 0 || rsp_upiu[0] != UPIU_QUERY_RSP || rsp_upiu[6] != 0)
    return -1;
  *value = rsp_upiu[23];
  return 0;
}

/* Run one SCSI command on logical unit `l` with `len` bytes through the data buffer. 0 only on GOOD status. */
static int scsi_cmd(uint8_t l, const uint8_t *cdb, uint32_t cdb_len, uint32_t dd, uint32_t len)
{
  if (cdb_len > 16)
    return -1;
  begin_upiu(UPIU_COMMAND);
  cmd_upiu[1] = dd == DD_FROM_DEV ? UPIU_FLAG_READ : dd == DD_TO_DEV ? UPIU_FLAG_WRITE : 0;
  cmd_upiu[2] = l;
  put_be32(cmd_upiu + 12, len);
  for (uint32_t i = 0; i < cdb_len; i++)
    cmd_upiu[16 + i] = cdb[i];
  if (transfer(dd, len) != 0)
    return -1;
  return rsp_upiu[0] == UPIU_RESPONSE && rsp_upiu[6] == 0 && rsp_upiu[7] == SCSI_GOOD ? 0 : -1;
}

static int rw10(bool write, uint32_t block, uint32_t nblocks)
{
  uint8_t cdb[10] = {write ? SCSI_WRITE10 : SCSI_READ10,
                     0,
                     (uint8_t)(block >> 24),
                     (uint8_t)(block >> 16),
                     (uint8_t)(block >> 8),
                     (uint8_t)block,
                     0,
                     (uint8_t)(nblocks >> 8),
                     (uint8_t)nblocks,
                     0};
  return scsi_cmd(lun, cdb, sizeof cdb, write ? DD_TO_DEV : DD_FROM_DEV, nblocks * block_size);
}

static bool range_ok(uint32_t lba, uint32_t count)
{
  return found && count != 0 && count <= MAX_SECTORS && lba < cap_sectors && count <= cap_sectors - lba;
}

int ufs_read(uint32_t lba, void *buf, uint32_t count)
{
  if (!range_ok(lba, count))
    return -1;
  uint32_t spb = block_size / 512;
  uint32_t first = lba / spb, last = (lba + count - 1) / spb;
  uintptr_t flags = spin_lock_irqsave(&io_lock);
  int rc = rw10(false, first, last - first + 1);
  if (rc == 0)
    memcpy(buf, (const void *)(data + (lba % spb) * 512), count * 512);
  spin_unlock_irqrestore(&io_lock, flags);
  return rc;
}

/* Sectors that do not cover whole logical blocks are merged into the blocks read back from the device first. */
int ufs_write(uint32_t lba, const void *buf, uint32_t count)
{
  if (!range_ok(lba, count))
    return -1;
  uint32_t spb = block_size / 512;
  uint32_t first = lba / spb, last = (lba + count - 1) / spb, nblocks = last - first + 1;
  bool partial = lba % spb != 0 || count % spb != 0;
  uintptr_t flags = spin_lock_irqsave(&io_lock);
  int rc = partial ? rw10(false, first, nblocks) : 0;
  if (rc == 0) {
    memcpy((void *)(data + (lba % spb) * 512), buf, count * 512);
    rc = rw10(true, first, nblocks);
  }
  spin_unlock_irqrestore(&io_lock, flags);
  return rc;
}

int ufs_read_sector(uint32_t lba, void *buf) { return ufs_read(lba, buf, 1); }
int ufs_write_sector(uint32_t lba, const void *buf) { return ufs_write(lba, buf, 1); }

/* Vendor and product from INQUIRY data, runs of blanks collapsed and trailing blanks trimmed. */
static void read_model(const volatile uint8_t *inq)
{
  int n = 0;
  for (int i = 8; i < 32 && n < (int)sizeof model - 1; i++) {
    char c = (char)inq[i];
    if (c == ' ' && (n == 0 || model[n - 1] == ' '))
      continue;
    model[n++] = c;
  }
  while (n > 0 && model[n - 1] == ' ')
    n--;
  model[n] = 0;
}

/* True if LU `l` is a disk with 512- or 4096-byte blocks; sets block_size, cap_sectors and model. */
static bool probe_lun(uint8_t l)
{
  uint8_t inq[6] = {SCSI_INQUIRY, 0, 0, 0, 36, 0};
  uint8_t tur[6] = {SCSI_TUR};
  uint8_t cap[10] = {SCSI_READ_CAP10};
  if (scsi_cmd(l, inq, sizeof inq, DD_FROM_DEV, 36) != 0 || data[0] != 0)
    return false;
  read_model(data);
  for (int i = 0; i < 4 && scsi_cmd(l, tur, sizeof tur, DD_NONE, 0) != 0; i++)
    ; /* the first command after power-on reports a unit attention */
  if (scsi_cmd(l, cap, sizeof cap, DD_FROM_DEV, 8) != 0)
    return false;
  uint32_t bs = be32(data + 4);
  if (bs != 512 && bs != 4096) {
    con_printf("ufs: LUN %u has %u-byte blocks; ignoring\n", l, bs);
    return false;
  }
  uint64_t sectors = ((uint64_t)be32(data) + 1) * (bs / 512);
  block_size = bs;
  cap_sectors = sectors > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)sectors;
  return true;
}

static int link_startup(void)
{
  for (int attempt = 0; attempt < 3; attempt++) {
    if (!wait_set(REG_HCS, HCS_UCRDY, HCS_UCRDY))
      return -1;
    reg_wr(REG_UCMDARG1, 0);
    reg_wr(REG_UCMDARG2, 0);
    reg_wr(REG_UCMDARG3, 0);
    reg_wr(REG_UICCMD, UIC_DME_LINKSTARTUP);
    if (!wait_set(REG_IS, IS_UCCS, IS_UCCS))
      return -1;
    reg_wr(REG_IS, IS_UCCS);
    if ((reg_rd(REG_UCMDARG2) & 0xFF) == 0 && (reg_rd(REG_HCS) & HCS_DP))
      return 0;
  }
  return -1;
}

static int enable_host(void)
{
  if (reg_rd(REG_HCE) & HCE_ENABLE) {
    reg_wr(REG_HCE, 0);
    if (!wait_set(REG_HCE, HCE_ENABLE, 0))
      return -1;
  }
  reg_wr(REG_HCE, HCE_ENABLE);
  if (!wait_set(REG_HCE, HCE_ENABLE, HCE_ENABLE))
    return -1;
  reg_wr(REG_IE, 0);
  return 0;
}

static int start_lists(void)
{
  reg_wr(REG_IS, reg_rd(REG_IS));
  reg_wr(REG_UTRLBA, mem + OFF_UTRL);
  reg_wr(REG_UTRLBAU, 0);
  reg_wr(REG_UTMRLBA, mem + OFF_UTMRL);
  reg_wr(REG_UTMRLBAU, 0);
  if (!wait_set(REG_HCS, HCS_UTRLRDY | HCS_UTMRLRDY, HCS_UTRLRDY | HCS_UTMRLRDY))
    return -1;
  reg_wr(REG_UTRLRSR, 1);
  reg_wr(REG_UTMRLRSR, 1);
  return 0;
}

/* Set fDeviceInit and wait for the device to clear it. */
static int device_init(void)
{
  int i;
  for (i = 0; i < 50 && nop_ping() != 0; i++)
    ;
  if (i == 50)
    return -1;
  uint8_t v;
  if (query_flag(QUERY_FUNC_WRITE, QUERY_OP_SET_FLAG, FLAG_FDEVICEINIT, &v) != 0)
    return -1;
  for (uint32_t n = 0; n < INIT_POLLS; n++) {
    if (query_flag(QUERY_FUNC_READ, QUERY_OP_READ_FLAG, FLAG_FDEVICEINIT, &v) != 0)
      return -1;
    if (v == 0)
      return 0;
  }
  return -1;
}

/* BAR0 base, or 0 if it is missing, not memory, or above 4 GiB. */
static uint32_t bar0_base(const struct pci_dev_info *d)
{
  uint32_t lo = pci_read32(d->bus, d->slot, d->func, 0x10);
  if (lo & 1u)
    return 0;
  if ((lo & 0x6u) == 0x4u && pci_read32(d->bus, d->slot, d->func, 0x14) != 0)
    return 0;
  return lo & 0xFFFFFFF0u;
}

void ufs_init(void)
{
  struct pci_dev_info d;
  int idx = 0;
  while (pci_dev_info(idx, &d) && !(d.class_code == UFS_CLASS && d.subclass == UFS_SUBCLASS))
    idx++;
  if (!pci_dev_info(idx, &d))
    return;

  uint32_t bar0 = bar0_base(&d);
  if (!bar0 || paging_identity_map(bar0, MMIO_LEN) != 0) {
    con_write("ufs: could not map MMIO\n");
    return;
  }
  paging_mark_uc(bar0, MMIO_LEN);
  mmio = (volatile uint32_t *)(uintptr_t)bar0;
  pci_write32(d.bus, d.slot, d.func, 0x04, pci_read32(d.bus, d.slot, d.func, 0x04) | 0x6u | 0x400u);

  mem = pmm_alloc_contig(MEM_LEN);
  if (!mem || paging_identity_map(mem, MEM_LEN) != 0) {
    con_write("ufs: out of DMA memory\n");
    return;
  }
  memset((void *)(uintptr_t)mem, 0, MEM_LEN);
  utrd = (volatile uint32_t *)(uintptr_t)(mem + OFF_UTRL);
  cmd_upiu = (volatile uint8_t *)(uintptr_t)(mem + OFF_UCD);
  rsp_upiu = (volatile uint8_t *)(uintptr_t)(mem + OFF_UCD + UCD_RSP);
  prdt = (volatile uint32_t *)(uintptr_t)(mem + OFF_UCD + UCD_PRDT);
  data = (volatile uint8_t *)(uintptr_t)(mem + OFF_DATA);

  uint32_t ver = reg_rd(REG_VER);
  if (enable_host() != 0) {
    con_write("ufs: host controller did not enable\n");
    return;
  }
  if (link_startup() != 0) {
    con_write("ufs: link startup failed (no device?)\n");
    return;
  }
  if (start_lists() != 0) {
    con_write("ufs: request lists not ready\n");
    return;
  }
  if (device_init() != 0) {
    con_write("ufs: device did not initialise\n");
    return;
  }

  for (uint8_t l = 0; l < MAX_LUN; l++)
    if (probe_lun(l)) {
      lun = l;
      found = true;
      con_printf("ufs: UFSHCI %u.%u LUN %u ready [%s], %u MiB, %u-byte blocks\n", (ver >> 8) & 0xFF, (ver >> 4) & 0xF,
                 l, model, cap_sectors / 2048, block_size);
      return;
    }
  con_write("ufs: controller found but no disk LU\n");
}
