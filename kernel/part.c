/* part.c: GUID Partition Table lookup (see part.h). */
#include "part.h"
#include "crc32.h"

#define SECTOR 512u
#define MAX_ENTRIES 1024u

const uint8_t PART_TYPE_OWOS_ROOT[16] = {0xb5, 0x09, 0xae, 0x2e, 0xf1, 0x7b, 0x7e, 0x4c,
                                         0x9a, 0x7f, 0x47, 0x5a, 0x69, 0x18, 0x81, 0x93};
const uint8_t PART_TYPE_ESP[16] = {0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
                                   0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b};
const uint8_t PART_TYPE_BIOS_BOOT[16] = {0x48, 0x61, 0x68, 0x21, 0x49, 0x64, 0x6f, 0x6e,
                                         0x74, 0x4e, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49};

static uint32_t get32(const uint8_t *p)
{
  return (uint32_t)p[0] | (uint32_t)p[1] << 8u | (uint32_t)p[2] << 16u | (uint32_t)p[3] << 24u;
}

static uint64_t get64(const uint8_t *p) { return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32u; }

static void put32(uint8_t *p, uint32_t v)
{
  for (int i = 0; i < 4; i++)
    p[i] = (uint8_t)(v >> (8u * (uint32_t)i));
}

static void put64(uint8_t *p, uint64_t v)
{
  put32(p, (uint32_t)v);
  put32(p + 4, (uint32_t)(v >> 32u));
}

static void zero(uint8_t *p, uint32_t n)
{
  for (uint32_t i = 0; i < n; i++)
    p[i] = 0;
}

static void copy(uint8_t *d, const uint8_t *s, uint32_t n)
{
  for (uint32_t i = 0; i < n; i++)
    d[i] = s[i];
}

static int same(const uint8_t *a, const uint8_t *b, uint32_t n)
{
  for (uint32_t i = 0; i < n; i++)
    if (a[i] != b[i])
      return 0;
  return 1;
}

int part_find(part_read_fn read, const uint8_t type[16], uint32_t *start, uint32_t *count)
{
  uint8_t hdr[SECTOR], sec[SECTOR];
  if (read(1, hdr) != 0 || !same(hdr, (const uint8_t *)"EFI PART", 8))
    return -1;
  uint32_t hsize = get32(hdr + 12), hcrc = get32(hdr + 16);
  if (hsize < 92 || hsize > SECTOR)
    return -1;
  hdr[16] = hdr[17] = hdr[18] = hdr[19] = 0;
  if (crc32(0, hdr, hsize) != hcrc)
    return -1;

  uint64_t entries_lba = get64(hdr + 72);
  uint32_t n = get32(hdr + 80), esize = get32(hdr + 84), ecrc = get32(hdr + 88);
  if (n == 0 || n > MAX_ENTRIES || esize < 128 || esize > SECTOR || SECTOR % esize != 0 ||
      entries_lba + (uint64_t)n * esize / SECTOR > 0xFFFFFFFFu)
    return -1;

  /* The whole array is read once: its CRC must hold before any entry in it is trusted. */
  uint32_t per = SECTOR / esize, sectors = (n + per - 1) / per, crc = 0, left = n * esize;
  int found = -1;
  uint32_t fstart = 0, fcount = 0;
  for (uint32_t s = 0; s < sectors; s++) {
    if (read((uint32_t)entries_lba + s, sec) != 0)
      return -1;
    uint32_t take = left < SECTOR ? left : SECTOR;
    crc = crc32(crc, sec, take);
    left -= take;
    for (uint32_t e = 0; e < per && s * per + e < n && found < 0; e++) {
      const uint8_t *ent = &sec[e * esize];
      if (!same(ent, type, 16))
        continue;
      uint64_t first = get64(ent + 32), last = get64(ent + 40);
      if (first == 0 || last < first || last > 0xFFFFFFFFu)
        continue;
      fstart = (uint32_t)first;
      fcount = (uint32_t)(last - first + 1);
      found = 0;
    }
  }
  if (crc != ecrc || found < 0)
    return -1;
  *start = fstart;
  *count = fcount;
  return 0;
}

#define ENTRIES 128u
#define ENTRY_SIZE 128u
#define ENTRY_SECTORS (ENTRIES * ENTRY_SIZE / SECTOR)

/* Random bytes made into an RFC 4122 version 4 GUID, in on-disk order. */
static void make_guid(uint8_t *g, const uint8_t *random)
{
  copy(g, random, 16);
  g[7] = (uint8_t)((g[7] & 0x0Fu) | 0x40u);
  g[8] = (uint8_t)((g[8] & 0x3Fu) | 0x80u);
}

static void fill_header(uint8_t *h, uint64_t self, uint64_t other, uint64_t entries, uint32_t sectors,
                        const uint8_t *disk_guid, uint32_t entries_crc)
{
  zero(h, SECTOR);
  copy(h, (const uint8_t *)"EFI PART", 8);
  put32(h + 8, 0x00010000);
  put32(h + 12, 92);
  put64(h + 24, self);
  put64(h + 32, other);
  put64(h + 40, PART_GPT_HEAD);
  put64(h + 48, sectors - PART_GPT_TAIL - 1u);
  copy(h + 56, disk_guid, 16);
  put64(h + 72, entries);
  put32(h + 80, ENTRIES);
  put32(h + 84, ENTRY_SIZE);
  put32(h + 88, entries_crc);
  put32(h + 16, crc32(0, h, 92));
}

int part_write_gpt(part_write_fn write, uint32_t sectors, const struct part_spec *parts, int n, const uint8_t *random)
{
  if (n < 1 || (uint32_t)n > ENTRIES || sectors < PART_GPT_HEAD + PART_GPT_TAIL + 1u)
    return -1;
  uint32_t next = PART_GPT_HEAD;
  for (int i = 0; i < n; i++) {
    if (parts[i].count == 0 || parts[i].start < next || parts[i].count > sectors - PART_GPT_TAIL - parts[i].start)
      return -1;
    next = parts[i].start + parts[i].count;
  }

  static uint8_t entries[ENTRIES * ENTRY_SIZE];
  zero(entries, sizeof entries);
  for (int i = 0; i < n; i++) {
    uint8_t *e = &entries[(uint32_t)i * ENTRY_SIZE];
    copy(e, parts[i].type, 16);
    make_guid(e + 16, random + 16 * (i + 1));
    put64(e + 32, parts[i].start);
    put64(e + 40, (uint64_t)parts[i].start + parts[i].count - 1u);
    for (uint32_t c = 0; c < 36 && parts[i].name && parts[i].name[c]; c++)
      e[56 + 2 * c] = (uint8_t)parts[i].name[c];
  }
  uint32_t entries_crc = crc32(0, entries, sizeof entries);
  uint8_t disk_guid[16], sec[SECTOR];
  make_guid(disk_guid, random);

  /* The protective MBR: one 0xEE partition covering the disk, so MBR-only tools leave it alone. */
  zero(sec, SECTOR);
  uint8_t *pe = sec + 446;
  pe[1] = 0x00;
  pe[2] = 0x02;
  pe[4] = 0xEE;
  pe[5] = pe[6] = pe[7] = 0xFF;
  put32(pe + 8, 1);
  put32(pe + 12, sectors - 1u);
  sec[510] = 0x55;
  sec[511] = 0xAA;
  if (write(0, sec) != 0)
    return -1;

  uint32_t backup_entries = sectors - PART_GPT_TAIL;
  for (uint32_t s = 0; s < ENTRY_SECTORS; s++)
    if (write(2 + s, entries + s * SECTOR) != 0 || write(backup_entries + s, entries + s * SECTOR) != 0)
      return -1;
  fill_header(sec, 1, sectors - 1u, 2, sectors, disk_guid, entries_crc);
  if (write(1, sec) != 0)
    return -1;
  fill_header(sec, sectors - 1u, 1, backup_entries, sectors, disk_guid, entries_crc);
  return write(sectors - 1u, sec);
}
