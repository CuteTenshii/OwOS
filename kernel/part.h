/* part.h: GUID Partition Table lookup. Pure logic over a sector-read callback, so the host tests can run it. */
#ifndef OWOS_PART_H
#define OWOS_PART_H

#include <stdint.h>

/* Partition type GUIDs in their on-disk (mixed-endian) byte order. */
extern const uint8_t PART_TYPE_OWOS_ROOT[16]; /* 2eae09b5-7bf1-4c7e-9a7f-475a69188193, OwOS's own */
extern const uint8_t PART_TYPE_ESP[16];       /* C12A7328-F81F-11D2-BA4B-00A0C93EC93B */
extern const uint8_t PART_TYPE_BIOS_BOOT[16]; /* 21686148-6449-6E6F-744E-656564454649 */

typedef int (*part_read_fn)(uint32_t lba, void *buf);
typedef int (*part_write_fn)(uint32_t lba, const void *buf);

struct part_spec {
  const uint8_t *type;
  uint32_t start, count; /* in sectors */
  const char *name;      /* ASCII, up to 36 characters */
};

/* Sectors the GPT itself takes at each end of the disk: the first usable sector, and how many the backup table
 * keeps at the end. */
#define PART_GPT_HEAD 34u
#define PART_GPT_TAIL 33u

/* Find the first partition of type `type` in the GPT of a disk with 512-byte sectors read through `read`. The
 * header and entry array must pass their CRCs and the partition must lie below 2^32 sectors. Fills *start and
 * *count (in sectors) and returns 0, or -1 if there is no valid GPT or no such partition. */
int part_find(part_read_fn read, const uint8_t type[16], uint32_t *start, uint32_t *count);

/* Write a protective MBR (no boot code) and a primary and backup GPT holding `parts` to a disk of `sectors`
 * sectors. `random` supplies 16 bytes per GUID: the disk's, then one per partition. Partitions must be in order,
 * not overlap and lie between PART_GPT_HEAD and sectors - PART_GPT_TAIL. 0, or -1 on a bad layout or write error. */
int part_write_gpt(part_write_fn write, uint32_t sectors, const struct part_spec *parts, int n, const uint8_t *random);

#endif /* OWOS_PART_H */
