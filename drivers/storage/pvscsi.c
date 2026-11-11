/* pvscsi.c: VMware PVSCSI adapter (see scsi.h). One request in flight, polled, interrupts masked.
 * One identity-mapped region holds the rings-state page, the request and completion rings, the data page and sense. */

#include "console.h"
#include "kstring.h"
#include "paging.h"
#include "pci.h"
#include "pmm.h"
#include "scsi.h"
#include <stddef.h>

#define PVSCSI_VENDOR 0x15AD
#define PVSCSI_DEVICE 0x07C0

#define REG_COMMAND 0x0
#define REG_COMMAND_DATA 0x4
#define REG_INTR_STATUS 0x100C
#define REG_INTR_MASK 0x2010
#define REG_KICK_RW_IO 0x4018
#define MMIO_LEN 0x8000

#define CMD_ADAPTER_RESET 1
#define CMD_SETUP_RINGS 3

#define FLAG_DIR_NONE (1u << 2)
#define FLAG_DIR_TOHOST (1u << 3)
#define FLAG_DIR_TODEVICE (1u << 4)

#define REQ_ENTRIES 32  /* one page of 128-byte requests */
#define CMP_ENTRIES 128 /* one page of 32-byte completions */
#define SETUP_DWORDS 132
#define SENSE_LEN 96
#define POLL_LIMIT 20000000u

#define MEM_LEN 0x5000
#define OFF_STATE 0x0000
#define OFF_REQ 0x1000
#define OFF_CMP 0x2000
#define OFF_DATA 0x3000
#define OFF_SENSE 0x4000
#define DATA_MAX 4096

/* Rings-state page fields, as dword indexes. */
#define ST_REQ_PROD 0
#define ST_CMP_PROD 3
#define ST_CMP_CONS 4

struct req_desc {
  uint64_t context;
  uint64_t data_addr;
  uint64_t data_len;
  uint64_t sense_addr;
  uint32_t sense_len;
  uint32_t flags;
  uint8_t cdb[16];
  uint8_t cdb_len;
  uint8_t lun[8];
  uint8_t tag;
  uint8_t bus;
  uint8_t target;
  uint8_t vcpu_hint;
  uint8_t unused[59];
} __attribute__((packed));

struct cmp_desc {
  uint64_t context;
  uint64_t data_len;
  uint32_t sense_len;
  uint16_t host_status;
  uint16_t scsi_status;
  uint32_t pad[2];
} __attribute__((packed));

static volatile uint32_t *mmio;
static uint32_t mem;
static volatile uint32_t *state;
static volatile struct req_desc *req_ring;
static volatile struct cmp_desc *cmp_ring;
static volatile uint8_t *data;
static uint32_t req_prod, cmp_cons;

static inline void reg_wr(uint32_t off, uint32_t v) { mmio[off / 4] = v; }
static inline uint32_t reg_rd(uint32_t off) { return mmio[off / 4]; }

static void command(uint32_t cmd, const uint32_t *desc, uint32_t dwords)
{
  reg_wr(REG_COMMAND, cmd);
  for (uint32_t i = 0; i < dwords; i++)
    reg_wr(REG_COMMAND_DATA, desc[i]);
}

static int pvscsi_cmd(uint8_t target, const uint8_t *cdb, uint32_t cdb_len, enum scsi_dir dir, void *buf, uint32_t len)
{
  if (len > DATA_MAX || cdb_len > sizeof req_ring->cdb)
    return -1;
  if (dir == SCSI_DIR_TO_DEV)
    memcpy((void *)data, buf, len);

  volatile struct req_desc *r = &req_ring[req_prod % REQ_ENTRIES];
  memset((void *)r, 0, sizeof *r);
  memcpy((void *)r->cdb, cdb, cdb_len);
  r->context = 1;
  r->cdb_len = (uint8_t)cdb_len;
  r->target = target;
  r->sense_addr = mem + OFF_SENSE;
  r->sense_len = SENSE_LEN;
  r->data_addr = mem + OFF_DATA;
  r->data_len = len;
  r->flags = dir == SCSI_DIR_NONE ? FLAG_DIR_NONE : dir == SCSI_DIR_FROM_DEV ? FLAG_DIR_TOHOST : FLAG_DIR_TODEVICE;

  __asm__ volatile("mfence" ::: "memory");
  req_prod++;
  state[ST_REQ_PROD] = req_prod;
  __asm__ volatile("mfence" ::: "memory");
  reg_wr(REG_KICK_RW_IO, 0);

  for (uint32_t i = 0; i < POLL_LIMIT; i++) {
    if (state[ST_CMP_PROD] == cmp_cons)
      continue;
    __asm__ volatile("mfence" ::: "memory");
    volatile struct cmp_desc *c = &cmp_ring[cmp_cons % CMP_ENTRIES];
    uint16_t host = c->host_status, scsi = c->scsi_status;
    cmp_cons++;
    state[ST_CMP_CONS] = cmp_cons;
    reg_wr(REG_INTR_STATUS, reg_rd(REG_INTR_STATUS));
    if (host != 0 || scsi != 0)
      return -1;
    if (dir == SCSI_DIR_FROM_DEV)
      memcpy(buf, (const void *)data, len);
    return 0;
  }
  return -1;
}

static const struct scsi_hba pvscsi_hba = {"pvscsi", pvscsi_cmd};

const struct scsi_hba *pvscsi_probe(void)
{
  struct pci_dev_info d;
  int idx = 0;
  while (pci_dev_info(idx, &d) && !(d.vendor == PVSCSI_VENDOR && d.device == PVSCSI_DEVICE))
    idx++;
  if (!pci_dev_info(idx, &d))
    return NULL;

  uint32_t bar0 = pci_read32(d.bus, d.slot, d.func, 0x10) & 0xFFFFFFF0u;
  if (!bar0 || paging_identity_map(bar0, MMIO_LEN) != 0) {
    con_write("pvscsi: could not map MMIO\n");
    return NULL;
  }
  paging_mark_uc(bar0, MMIO_LEN);
  mmio = (volatile uint32_t *)(uintptr_t)bar0;
  pci_write32(d.bus, d.slot, d.func, 0x04, pci_read32(d.bus, d.slot, d.func, 0x04) | 0x6u | 0x400u);

  mem = pmm_alloc_contig(MEM_LEN);
  if (!mem || paging_identity_map(mem, MEM_LEN) != 0) {
    con_write("pvscsi: out of DMA memory\n");
    return NULL;
  }
  memset((void *)(uintptr_t)mem, 0, MEM_LEN);
  state = (volatile uint32_t *)(uintptr_t)(mem + OFF_STATE);
  req_ring = (volatile struct req_desc *)(uintptr_t)(mem + OFF_REQ);
  cmp_ring = (volatile struct cmp_desc *)(uintptr_t)(mem + OFF_CMP);
  data = (volatile uint8_t *)(uintptr_t)(mem + OFF_DATA);

  command(CMD_ADAPTER_RESET, NULL, 0);
  reg_wr(REG_INTR_MASK, 0);

  /* struct { u32 reqPages, cmpPages; u64 statePPN; u64 reqPPNs[32]; u64 cmpPPNs[32]; } */
  uint32_t setup[SETUP_DWORDS] = {0};
  setup[0] = 1;
  setup[1] = 1;
  setup[2] = (mem + OFF_STATE) >> 12;
  setup[4] = (mem + OFF_REQ) >> 12;
  setup[4 + 64] = (mem + OFF_CMP) >> 12;
  command(CMD_SETUP_RINGS, setup, SETUP_DWORDS);
  return &pvscsi_hba;
}
