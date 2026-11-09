/* zip.c: ZIP archive format reader (store + deflate only).
 *
 * Parses the end-of-central-directory record (EOCD) to find the central
 * directory, reads file entries from it, and records their local-header
 * offsets so extraction can seek directly to the data. Supports only the
 * "store" (uncompressed) and "deflate" methods; a file compressed with an
 * unsupported method is listed but can't be extracted (its extraction returns
 * -1, logged via puts).
 */
#include "archive.h"
#include "inflate.h"
#include "owos.h"

/* On-disk structures, packed per the ZIP spec. All multi-byte integers are
 * little-endian. */
#pragma pack(push, 1)
struct zip_eocd {
  unsigned sig;           /* 0x06054b50            */
  unsigned short disk;    /* disk number           */
  unsigned short cd_disk; /* disk with CD start    */
  unsigned short cd_ents; /* entries on this disk  */
  unsigned short total;   /* total entries          */
  unsigned cd_size;       /* central directory size */
  unsigned cd_off;        /* offset of CD start     */
  unsigned short clen;    /* comment length         */
};

struct zip_cdfh {
  unsigned sig;            /* 0x02014b50               */
  unsigned short ver_made; /* version made by          */
  unsigned short ver_need; /* version needed           */
  unsigned short flags;    /* general-purpose bit flag  */
  unsigned short method;   /* compression (0=store, 8=deflate) */
  unsigned short mtime;    /* DOS time                  */
  unsigned short mdate;    /* DOS date                  */
  unsigned crc32;
  unsigned csize; /* compressed size            */
  unsigned usize; /* uncompressed size          */
  unsigned short namelen;
  unsigned short extralen;
  unsigned short clen;  /* file comment length        */
  unsigned short disk;  /* disk number start          */
  unsigned short iattr; /* internal file attributes   */
  unsigned eattr;       /* external file attributes   */
  unsigned lhoff;       /* local header offset        */
};

struct zip_lfh {
  unsigned sig; /* 0x04034b50               */
  unsigned short ver_need;
  unsigned short flags;
  unsigned short method;
  unsigned short mtime;
  unsigned short mdate;
  unsigned crc32;
  unsigned csize;
  unsigned usize;
  unsigned short namelen;
  unsigned short extralen;
};
#pragma pack(pop)

/* Convert DOS date/time to OwOS packed mtime. Very approximate. */
static unsigned dos2mtime(unsigned short ddate, unsigned short dtime)
{
  unsigned y = ((ddate >> 9) & 0x7F) + 1980;
  unsigned m = (ddate >> 5) & 0x0F;
  unsigned d = ddate & 0x1F;
  unsigned hh = (dtime >> 11) & 0x1F;
  unsigned mm = (dtime >> 5) & 0x3F;
  unsigned ss = (dtime & 0x1F) * 2;
  /* OwOS packed format: [year-2000:6][month:4][day:5][hour:5][min:6][sec:6] */
  return ((y - 2000) << 26) | (m << 22) | (d << 17) | (hh << 12) | (mm << 6) | ss;
}

/* Search backwards from the end of the file for the EOCD signature (which
 * may be followed by a variable-length comment). Returns 1 if found, 0 if not. */
static int find_eocd(const char *path, struct zip_eocd *out)
{
  unsigned char buf[1024];
  struct statinfo st;
  if (stat(path, &st) < 0)
    return 0;

  int search = st.size > 1024 ? 1024 : (int)st.size;
  int base = (int)st.size - search;
  if (base < 0)
    base = 0;
  search = (int)st.size - base;

  int n = readfile_at(path, base, buf, search);
  if (n < 22)
    return 0;

  /* Scan backwards for 0x06054b50. */
  for (int i = n - 22; i >= 0; i--) {
    if (buf[i] == 0x50 && buf[i + 1] == 0x4b && buf[i + 2] == 0x05 && buf[i + 3] == 0x06) {
      memcpy(out, buf + i, sizeof(struct zip_eocd));
      return 1;
    }
  }
  return 0;
}

int zip_parse(struct archive *a)
{
  (void)a; /* archive_open passes a partially-filled struct */

  struct zip_eocd eocd;
  if (!find_eocd(a->path, &eocd))
    return -1;
  if (eocd.total == 0)
    return 0; /* empty archive */

  /* Read the central directory. */
  unsigned cd_size = eocd.cd_size;
  unsigned cd_off = eocd.cd_off;
  if (cd_size > 4 * 1024 * 1024)
    return -1; /* 4 MiB cap on CD */

  unsigned char *cd = malloc(cd_size);
  if (!cd)
    return -1;
  int rn = readfile_at(a->path, cd_off, cd, cd_size);
  if (rn < 0 || (unsigned)rn != cd_size) {
    free(cd);
    return -1;
  }

  /* Walk the central directory entries. */
  int nents = 0;
  unsigned pos = 0;
  while (pos + 46 <= cd_size && nents < ARC_MAX_ENTS) {
    unsigned sig = cd[pos] | (cd[pos + 1] << 8) | (cd[pos + 2] << 16) | (cd[pos + 3] << 24);
    if (sig != 0x02014b50)
      break;

    struct zip_cdfh *fh = (struct zip_cdfh *)(cd + pos);
    unsigned namelen = fh->namelen;
    unsigned extralen = fh->extralen;
    unsigned clen = fh->clen;
    unsigned entry_sz = 46 + namelen + extralen + clen;

    if (pos + entry_sz > cd_size)
      break;

    char *name = (char *)(cd + pos + 46);
    struct arcent *e = &a->ents[nents];

    /* Copy the name; check for trailing '/' (directory). */
    int ni = 0;
    for (unsigned k = 0; k < namelen && ni < ARC_NAME_MAX - 1; k++)
      e->name[ni++] = name[k];
    e->name[ni] = 0;
    e->is_dir = (ni > 0 && e->name[ni - 1] == '/');
    if (e->is_dir)
      e->name[ni - 1] = 0;

    e->size = fh->usize;
    e->packed = fh->csize;
    e->mtime = dos2mtime(fh->mdate, fh->mtime);
    e->format = ARC_FMT_ZIP;
    e->method = fh->method;
    e->crc32 = fh->crc32;

    /* Compute the data offset within the file: local header offset +
     * 30 (fixed-size part of LFH) + namelen + extralen. */
    unsigned lh_off = fh->lhoff;
    /* Read the local file header to get its actual namelen/extralen
     * (they should match the CD, but we read them to be safe). */
    unsigned char lh_buf[30];
    int lr = readfile_at(a->path, lh_off, lh_buf, 30);
    if (lr == 30) {
      struct zip_lfh *lfh = (struct zip_lfh *)lh_buf;
      e->data_offset = lh_off + 30 + lfh->namelen + lfh->extralen;
      e->data_size = fh->csize;
    } else {
      e->data_offset = lh_off + 30 + namelen + extralen;
      e->data_size = fh->csize;
    }
    e->format = ARC_FMT_ZIP;

    nents++;
    pos += entry_sz;
  }

  free(cd);
  a->nents = nents;
  return 0; /* empty ZIP (nents=0) is valid */
}
