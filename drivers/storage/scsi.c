/* scsi.c: SCSI disk on top of a host bus adapter (see scsi.h). */

#include "scsi.h"
#include "console.h"
#include "spinlock.h"
#include <stdbool.h>
#include <stddef.h>

#define MAX_TARGET 16
#define MAX_SECTORS 8

#define SCSI_TUR 0x00
#define SCSI_INQUIRY 0x12
#define SCSI_READ_CAP10 0x25
#define SCSI_READ10 0x28
#define SCSI_WRITE10 0x2A

static const struct scsi_hba *hba;
static uint8_t target;
static uint32_t cap_sectors;
static char model[33];
static spinlock_t io_lock;

int scsi_present(void) { return hba != NULL; }
uint32_t scsi_sectors(void) { return cap_sectors; }
const char *scsi_model(void) { return model; }
const char *scsi_hba_name(void) { return hba ? hba->name : ""; }

static int rw(bool write, uint32_t lba, void *buf, uint32_t count)
{
  if (!hba || count == 0 || count > MAX_SECTORS)
    return -1;
  uint8_t cdb[10] = {write ? SCSI_WRITE10 : SCSI_READ10,
                     0,
                     (uint8_t)(lba >> 24),
                     (uint8_t)(lba >> 16),
                     (uint8_t)(lba >> 8),
                     (uint8_t)lba,
                     0,
                     (uint8_t)(count >> 8),
                     (uint8_t)count,
                     0};
  uintptr_t flags = spin_lock_irqsave(&io_lock);
  int rc = hba->cmd(target, cdb, sizeof cdb, write ? SCSI_DIR_TO_DEV : SCSI_DIR_FROM_DEV, buf, count * 512);
  spin_unlock_irqrestore(&io_lock, flags);
  return rc;
}

int scsi_read(uint32_t lba, void *buf, uint32_t count) { return rw(false, lba, buf, count); }
int scsi_write(uint32_t lba, const void *buf, uint32_t count) { return rw(true, lba, (void *)buf, count); }
int scsi_read_sector(uint32_t lba, void *buf) { return rw(false, lba, buf, 1); }
int scsi_write_sector(uint32_t lba, const void *buf) { return rw(true, lba, (void *)buf, 1); }

static uint32_t be32(const uint8_t *p)
{
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* Vendor and product from INQUIRY data, runs of blanks collapsed and trailing blanks trimmed. */
static void read_model(const uint8_t *inq)
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

/* True if `target` is a disk with 512-byte blocks; sets cap_sectors and model. */
static bool probe_target(const struct scsi_hba *h, uint8_t t)
{
  uint8_t buf[36];
  uint8_t inq[6] = {SCSI_INQUIRY, 0, 0, 0, sizeof buf, 0};
  uint8_t tur[6] = {SCSI_TUR};
  uint8_t cap[10] = {SCSI_READ_CAP10};
  if (h->cmd(t, inq, sizeof inq, SCSI_DIR_FROM_DEV, buf, sizeof buf) != 0 || buf[0] != 0)
    return false;
  read_model(buf);
  for (int i = 0; i < 4 && h->cmd(t, tur, sizeof tur, SCSI_DIR_NONE, NULL, 0) != 0; i++)
    ; /* the first command after reset reports a unit attention */
  if (h->cmd(t, cap, sizeof cap, SCSI_DIR_FROM_DEV, buf, 8) != 0)
    return false;
  if (be32(buf + 4) != 512) {
    con_printf("scsi: target %u has %u-byte blocks; ignoring\n", t, be32(buf + 4));
    return false;
  }
  uint32_t last = be32(buf);
  cap_sectors = last == 0xFFFFFFFFu ? last : last + 1;
  return true;
}

void scsi_init(void)
{
  const struct scsi_hba *candidates[] = {vscsi_probe(), pvscsi_probe()};
  bool any = false;
  for (unsigned i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
    const struct scsi_hba *h = candidates[i];
    if (!h)
      continue;
    any = true;
    for (uint8_t t = 0; t < MAX_TARGET; t++)
      if (probe_target(h, t)) {
        target = t;
        hba = h;
        con_printf("scsi: %s target %u ready [%s], %u MiB\n", h->name, t, model, cap_sectors / 2048);
        return;
      }
  }
  if (any)
    con_write("scsi: adapter found but no disk\n");
}
