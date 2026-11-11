/* scsi.h: SCSI disk layer over a pluggable host bus adapter (virtio-scsi, VMware PVSCSI).
 *
 * Probes targets 0..15 (LUN 0) of the first adapter found for a direct-access disk with 512-byte blocks and talks to
 * it with INQUIRY / READ CAPACITY(10) / READ(10) / WRITE(10). Same contract as ata.c/ahci.c/nvme.c. */
#ifndef OWOS_SCSI_H
#define OWOS_SCSI_H

#include <stdint.h>

enum scsi_dir { SCSI_DIR_NONE, SCSI_DIR_FROM_DEV, SCSI_DIR_TO_DEV };

/* One adapter. cmd runs a command on LUN 0 of `target` and polls it; len is at most 4096. It returns 0 only when the
 * adapter and the SCSI status are both good. */
struct scsi_hba {
  const char *name;
  int (*cmd)(uint8_t target, const uint8_t *cdb, uint32_t cdb_len, enum scsi_dir dir, void *buf, uint32_t len);
};

/* Adapter probes: the adapter, or NULL if absent or it failed to come up. */
const struct scsi_hba *vscsi_probe(void);
const struct scsi_hba *pvscsi_probe(void);

/* Bring up the first adapter that has a disk. Prints one line and does nothing else when there is none. */
void scsi_init(void);

int scsi_present(void);
uint32_t scsi_sectors(void);
const char *scsi_model(void);
const char *scsi_hba_name(void);

/* Return 0 on success, -1 on error/timeout. buf holds 512 * count bytes; count is at most 8. */
int scsi_read_sector(uint32_t lba, void *buf);
int scsi_write_sector(uint32_t lba, const void *buf);
int scsi_read(uint32_t lba, void *buf, uint32_t count);
int scsi_write(uint32_t lba, const void *buf, uint32_t count);

#endif /* OWOS_SCSI_H */
