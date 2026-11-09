/* archive.c: archive format dispatch + TAR parser.
 *
 * Reads the raw archive file (or decompresses a compressed tar first), indexes
 * its entries, and provides extract-by-index so the GUI never re-parses. TAR is
 * a simple 512-byte-block format: each file header is one block, the file data
 * follows (padded to the next block), and two consecutive zero blocks mark the
 * end. We support ustar / POSIX.1-2001 / legacy GNU long-name hacks.
 */
#include "archive.h"
#include "brotli.h"
#include "crc32.h"
#include "inflate.h"
#include "owos.h"
#include "zstd.h"

#define TAR_BLOCK 512
#define TAR_NAME 100
#define TAR_FNAME 155 /* ustar prefix */
#define TAR_MAGIC "ustar"
#define TAR_MAGIC2 "ustar  " /* POSIX.1-2001 */

/* --- helpers ------------------------------------------------------------- */

static int min(int a, int b) { return a < b ? a : b; }

static void str_ncpy(char *d, const char *s, int n)
{
  int i;
  for (i = 0; i < n && s[i]; i++)
    d[i] = s[i];
  d[i] = 0;
}

/* Read a big-endian or octal field from a TAR header. `len` is the field
 * width in bytes. Returns 0 on a malformed field. */
static unsigned tar_num(const unsigned char *f, int len)
{
  /* Octal (all standard ustar numeric fields). */
  if (f[0] == 0 || f[0] == ' ')
    return 0;
  unsigned v = 0;
  for (int i = 0; i < len; i++) {
    unsigned char c = f[i];
    if (c == 0 || c == ' ')
      break;
    if (c < '0' || c > '7')
      return 0;
    v = (v << 3) | (unsigned)(c - '0');
  }
  return v;
}

/* Check that the 512-byte block at `b` is all zeros. */
static int is_zero_block(const unsigned char *b)
{
  for (int i = 0; i < TAR_BLOCK; i++)
    if (b[i])
      return 0;
  return 1;
}

/* Compute the simple sum of the header bytes (unsigned), with the 8-byte
 * checksum field treated as spaces. Used for the TAR checksum check. */
static unsigned tar_chksum(const unsigned char *hdr)
{
  unsigned sum = 0;
  for (int i = 0; i < TAR_BLOCK; i++) {
    if (i >= 148 && i < 156)
      sum += 32; /* treat chksum field as spaces */
    else
      sum += hdr[i];
  }
  return sum;
}

/* --- TAR format ---------------------------------------------------------- */

/* Parse entries from raw TAR data (either a file we read block-by-block, or
 * the decompressed buffer of a compressed tar). `reader` returns the next
 * block of TAR_BLOCK bytes into `buf`; it returns the number of bytes read
 * (TAR_BLOCK normally, 0 at EOF, < TAR_BLOCK on error). `ctx` is the reader's
 * state. Returns the number of entries parsed (may be 0 on an empty archive),
 * or -1 on error. */
struct tar_ctx {
  const unsigned char *buf; /* for in-memory: full decompressed data */
  int len;
  int pos; /* current offset within buf */
};

static int tar_mem_read(void *vctx, unsigned char *block)
{
  struct tar_ctx *tc = (struct tar_ctx *)vctx;
  if (tc->pos + TAR_BLOCK > tc->len)
    return 0; /* EOF */
  memcpy(block, tc->buf + tc->pos, TAR_BLOCK);
  tc->pos += TAR_BLOCK;
  return TAR_BLOCK;
}

/* File-based TAR reader: uses readfile_at to pull blocks from the archive
 * file on disk. */
struct tar_file_ctx {
  int fd; /* unused; reads go through readfile_at */
  char path[256];
  int off; /* current byte offset */
};

static int tar_file_read(void *vctx, unsigned char *block)
{
  struct tar_file_ctx *fc = (struct tar_file_ctx *)vctx;
  int n = readfile_at(fc->path, fc->off, block, TAR_BLOCK);
  if (n <= 0)
    return 0;
  fc->off += TAR_BLOCK;
  return n;
}

typedef int (*tar_reader)(void *ctx, unsigned char *block);

/* Parse a TAR header block. Returns 1 on success (entry filled), 0 at end
 * of archive (two zero blocks), -1 on a malformed header. */
static int tar_parse_hdr(const unsigned char *hdr, struct arcent *out, char *long_name, int *long_len, int *has_long)
{
  if (is_zero_block(hdr))
    return 0; /* may be first of two end blocks */

  /* Checksum validation. */
  unsigned sum = tar_chksum(hdr);
  unsigned stored = tar_num(hdr + 148, 7);
  if (stored == 0)
    return 0; /* zero chksum = end of archive? check next */
  if (sum != stored) {
    /* Some old GNU tar variants use signed checksum; try alternate. */
    unsigned sum2 = 0;
    for (int i = 0; i < TAR_BLOCK; i++) {
      if (i >= 148 && i < 156)
        sum2 += 32;
      else
        sum2 += (signed char)hdr[i];
    }
    if (sum2 != stored)
      return -1;
  }

  /* Type flag: '0' or '\0' = regular file, '5' = directory,
   * 'L' = GNU long name (next block is the name), 'K' = GNU long link. */
  int type = hdr[156];
  int is_long = (type == 'L' || type == 'K');

  out->size = tar_num(hdr + 124, 12);
  out->mtime = tar_num(hdr + 136, 12);
  out->is_dir = (type == '5');

  /* Name: ustar prefix (345-499) + name (0-99). */
  char namebuf[512];
  int nl = 0;
  if (!memcmp(hdr + 257, TAR_MAGIC, 5) || !memcmp(hdr + 257, TAR_MAGIC2, 6)) {
    /* ustar/POSIX: prefix (155) then '/' then name (100). */
    int pi = 0;
    while (pi < TAR_FNAME && hdr[345 + pi])
      namebuf[nl++] = hdr[345 + pi++];
    if (nl > 0 && namebuf[nl - 1] != '/')
      namebuf[nl++] = '/';
  }
  /* Name field. */
  for (int i = 0; i < TAR_NAME && hdr[i]; i++)
    namebuf[nl++] = hdr[i];
  namebuf[nl] = 0;

  /* Handle GNU long name: the *previous* 'L' entry's data block holds the
   * real name of the *next* file entry. */
  if (is_long) {
    *has_long = 1;
    *long_len = 0;
    return 1; /* consumed; the name block is the data */
  }

  if (*has_long && *long_len > 0) {
    out->name[0] = 0;
    str_ncpy(out->name, long_name, min(*long_len, ARC_NAME_MAX - 1));
    *has_long = 0;
    *long_len = 0;
  } else {
    str_ncpy(out->name, namebuf, ARC_NAME_MAX - 1);
  }

  /* Strip trailing '/' from directory names (some tar writers include it). */
  if (out->is_dir) {
    int i = 0;
    while (out->name[i])
      i++;
    if (i > 0 && out->name[i - 1] == '/')
      out->name[i - 1] = 0;
  }

  /* Regular file with zero size is not treated as a dir here (some archivers
   * produce empty files). But if type is '5', it's always a dir. */
  return 1;
}

/* Parse a TAR archive from a reader. Populates a->ents and a->nents.
 * For in-memory readers, records data_offset as the position in the buffer.
 * For file-based readers, records data_offset as the file byte offset. */
static int tar_parse(struct archive *a, tar_reader rd, void *rctx, int is_mem)
{
  char long_buf[ARC_NAME_MAX];
  int long_len = 0;
  int has_long = 0;
  unsigned char hdr[TAR_BLOCK];

  while (a->nents < ARC_MAX_ENTS) {
    int n = rd(rctx, hdr);
    if (n == 0)
      break;
    if (n < TAR_BLOCK)
      return -1;

    /* Check for end-of-archive (two zero blocks). */
    if (is_zero_block(hdr)) {
      /* Peek at the next block. */
      unsigned char peek[TAR_BLOCK];
      int pn = rd(rctx, peek);
      if (pn == 0)
        break;
      if (pn < TAR_BLOCK)
        return -1;
      if (is_zero_block(peek))
        break;
      /* False alarm: process `peek` as a real header. */
      memcpy(hdr, peek, TAR_BLOCK);
    }

    struct arcent *e = &a->ents[a->nents];
    int r = tar_parse_hdr(hdr, e, long_buf, &long_len, &has_long);
    if (r < 0)
      return -1;
    if (r == 0)
      break; /* shouldn't happen after zero-block check */

    int type = hdr[156];
    int is_long = (type == 'L' || type == 'K');
    int data_blocks = (e->size + TAR_BLOCK - 1) / TAR_BLOCK;

    if (is_long) {
      /* Read the long name from the data block(s). */
      unsigned char name_block[TAR_BLOCK];
      for (int b = 0; b < data_blocks; b++) {
        int bn = rd(rctx, name_block);
        if (bn < TAR_BLOCK)
          return -1;
        /* The entry size comes from the file; keep only what fits, still consuming every block. */
        int copy = min(min((int)e->size - b * TAR_BLOCK, TAR_BLOCK), ARC_NAME_MAX - 1 - long_len);
        if (copy > 0) {
          memcpy(long_buf + long_len, name_block, copy);
          long_len += copy;
        }
      }
      /* Don't store the 'L'/'K' entry itself. */
      continue;
    }

    /* Record the data position. */
    if (is_mem) {
      struct tar_ctx *tc = (struct tar_ctx *)rctx;
      e->data_offset = tc->pos; /* current position after header */
    } else {
      struct tar_file_ctx *fc = (struct tar_file_ctx *)rctx;
      e->data_offset = fc->off;
    }
    e->data_size = e->size; /* stored = uncompressed for raw TAR */
    e->packed = e->size;
    e->format = a->fmt;

    /* Skip the data blocks. */
    for (int b = 0; b < data_blocks; b++) {
      unsigned char skip[TAR_BLOCK];
      int sn = rd(rctx, skip);
      if (sn < TAR_BLOCK)
        return -1;
    }

    a->nents++;
  }
  return 0;
}

/* --- ZIP format ---------------------------------------------------------- */
/* Declared in zip.c, called from our open. */
int zip_parse(struct archive *a);

/* --- open / detect ------------------------------------------------------- */

const char *archive_detect(const char *path)
{
  unsigned char head[8];
  int n = readfile_at(path, 0, head, 8);
  if (n < 4)
    return "unknown";

  /* ZIP: "PK\x03\x04" */
  if (head[0] == 'P' && head[1] == 'K' && head[2] == 0x03 && head[3] == 0x04)
    return "ZIP";

  /* Gzip: 1f 8b */
  if (head[0] == 0x1f && head[1] == 0x8b)
    return "TAR.GZ";

  /* Zstd: 28 b5 2f fd */
  if (head[0] == 0x28 && head[1] == 0xb5 && head[2] == 0x2f && head[3] == 0xfd)
    return "TAR.ZST";

  /* Try TAR: check the checksum for a valid header. */
  {
    unsigned char tar_check[TAR_BLOCK];
    int tn = readfile_at(path, 0, tar_check, TAR_BLOCK);
    if (tn == TAR_BLOCK) {
      unsigned sum = tar_chksum(tar_check);
      unsigned stored = tar_num(tar_check + 148, 7);
      if (stored > 0 && sum == stored)
        return "TAR";
      /* Try signed sum for old GNU tar. */
      unsigned sum2 = 0;
      for (int i = 0; i < TAR_BLOCK; i++) {
        if (i >= 148 && i < 156)
          sum2 += 32;
        else
          sum2 += (signed char)tar_check[i];
      }
      if (sum2 == stored)
        return "TAR";
    }
  }

  return "unknown";
}

/* --- inflate callbacks (file scope, shared by compressed-tar paths) ---- */

struct inf_rd {
  unsigned char *buf;
  int len;
  int pos;
};
struct inf_wr {
  unsigned char *buf;
  int cap;
  int len;
};

static int infl_reader(void *v)
{
  struct inf_rd *r = (struct inf_rd *)v;
  if (r->pos >= r->len)
    return -1;
  return r->buf[r->pos++];
}

static int infl_writer(void *v, const unsigned char *b, unsigned n)
{
  struct inf_wr *w = (struct inf_wr *)v;
  int need = w->len + (int)n;
  if (need > w->cap) {
    int nc = w->cap ? w->cap * 2 : 65536;
    if (nc < need)
      nc = need;
    unsigned char *nb = realloc(w->buf, (unsigned)nc);
    if (!nb)
      return -1;
    w->buf = nb;
    w->cap = nc;
  }
  memcpy(w->buf + w->len, b, n);
  w->len += (int)n;
  return 0;
}

/* --- compressed-tar helpers --------------------------------------------- */

/* Read entire file into a heap buffer. Returns NULL on error. */
static unsigned char *read_all(const char *path, unsigned *out_len)
{
  struct statinfo st;
  if (stat(path, &st) < 0 || st.size > 64 * 1024 * 1024)
    return NULL;
  unsigned char *buf = malloc(st.size);
  if (!buf)
    return NULL;
  int n = readfile_at(path, 0, buf, st.size);
  if (n < 0 || (unsigned)n != st.size) {
    free(buf);
    return NULL;
  }
  *out_len = st.size;
  return buf;
}

/* Decompress a gzip-compressed buffer into a heap buffer via inflate_http.
 * Returns the decompressed data and length; NULL on error. */
static unsigned char *gunzip(const unsigned char *in, int in_len, int *out_len)
{
  struct inf_rd rd = {(unsigned char *)in, in_len, 0};
  struct inf_wr wr = {NULL, 0, 0};
  if (inflate_http(infl_reader, &rd, infl_writer, &wr) < 0 || wr.len == 0) {
    free(wr.buf);
    return NULL;
  }
  *out_len = wr.len;
  return wr.buf;
}

/* Open and index a raw (file-based) TAR at path. */
static int open_tar_raw(struct archive *a, const char *path, int plen)
{
  a->fmt = ARC_FMT_TAR;
  struct tar_file_ctx fc;
  fc.off = 0;
  memcpy(fc.path, path, plen + 1);
  if (tar_parse(a, tar_file_read, &fc, 0) < 0)
    return -1;
  return 0;
}

/* Read a compressed file at path, decompress with fn, parse the result as TAR. */
static int open_tar_comp(struct archive *a, const char *path, int fmt,
                         unsigned char *(*decomp)(const unsigned char *, int, int *))
{
  if (!decomp)
    return -1;
  unsigned comp_len = 0;
  unsigned char *comp = read_all(path, &comp_len);
  if (!comp)
    return -1;

  int dlen = 0;
  unsigned char *raw = decomp(comp, (int)comp_len, &dlen);
  free(comp);
  if (!raw || dlen == 0)
    return -1;

  a->fmt = fmt;
  a->decomp = raw;
  a->decomp_len = dlen;
  struct tar_ctx tc;
  tc.buf = raw;
  tc.len = dlen;
  tc.pos = 0;
  if (tar_parse(a, tar_mem_read, &tc, 1) < 0)
    return -1;
  return 0;
}

struct archive *archive_open(const char *path)
{
  struct archive *a = calloc(1, sizeof(struct archive));
  if (!a)
    return NULL;

  int plen = 0;
  while (path[plen])
    plen++;
  a->path = malloc(plen + 1);
  if (!a->path) {
    free(a);
    return NULL;
  }
  memcpy(a->path, path, plen + 1);

  const char *fmt_str = archive_detect(path);

  if (streq(fmt_str, "TAR")) {
    if (open_tar_raw(a, path, plen) < 0)
      goto fail;
    return a;
  }
  if (streq(fmt_str, "ZIP")) {
    a->fmt = ARC_FMT_ZIP;
    if (zip_parse(a) < 0)
      goto fail;
    return a;
  }
  if (streq(fmt_str, "TAR.GZ")) {
    /* gzip: decompress via inflate, then parse as TAR. */
    unsigned comp_len = 0;
    unsigned char *comp = read_all(path, &comp_len);
    if (!comp)
      goto fail;

    int dlen = 0;
    unsigned char *raw = gunzip(comp, (int)comp_len, &dlen);
    free(comp);
    if (!raw || dlen == 0)
      goto fail;

    a->fmt = ARC_FMT_TAR_GZ;
    a->decomp = raw;
    a->decomp_len = dlen;
    struct tar_ctx tc;
    tc.buf = raw;
    tc.len = dlen;
    tc.pos = 0;
    if (tar_parse(a, tar_mem_read, &tc, 1) < 0)
      goto fail;
    return a;
  }
  if (streq(fmt_str, "TAR.ZST")) {
    if (open_tar_comp(a, path, ARC_FMT_TAR_ZST, zstd_decompress) < 0)
      goto fail;
    return a;
  }
  if (streq(fmt_str, "TAR.BR")) {
    if (open_tar_comp(a, path, ARC_FMT_TAR_BR, brotli_decompress) < 0)
      goto fail;
    return a;
  }

  /* Unknown format: brotli has no magic number, so try decoding it as a last
   * resort; if that yields a valid TAR, it's TAR.BR. */
  {
    unsigned comp_len = 0;
    unsigned char *comp = read_all(path, &comp_len);
    if (comp) {
      int dlen = 0;
      unsigned char *raw = brotli_decompress(comp, (int)comp_len, &dlen);
      free(comp);
      if (raw && dlen > 0 && dlen >= TAR_BLOCK) {
        /* Check if it looks like a TAR. */
        unsigned sum = tar_chksum(raw);
        unsigned stored = tar_num(raw + 148, 7);
        int looks_tar = (stored > 0 && sum == stored);
        if (!looks_tar) {
          unsigned sum2 = 0;
          for (int i = 0; i < TAR_BLOCK; i++) {
            if (i >= 148 && i < 156)
              sum2 += 32;
            else
              sum2 += (signed char)raw[i];
          }
          looks_tar = (sum2 == stored);
        }
        if (looks_tar) {
          a->fmt = ARC_FMT_TAR_BR;
          a->decomp = raw;
          a->decomp_len = dlen;
          struct tar_ctx tc;
          tc.buf = raw;
          tc.len = dlen;
          tc.pos = 0;
          if (tar_parse(a, tar_mem_read, &tc, 1) < 0)
            goto fail;
          return a;
        }
        free(raw);
      }
    }
  }

fail:
  archive_close(a);
  return NULL;
}

/* --- public API ---------------------------------------------------------- */

const char *archive_format(struct archive *a)
{
  if (!a)
    return "unknown";
  switch (a->fmt) {
  case ARC_FMT_TAR:
    return "TAR";
  case ARC_FMT_ZIP:
    return "ZIP";
  case ARC_FMT_TAR_GZ:
    return "TAR.GZ";
  case ARC_FMT_TAR_ZST:
    return "TAR.ZST";
  case ARC_FMT_TAR_BR:
    return "TAR.BR";
  default:
    return "unknown";
  }
}

int archive_count(struct archive *a)
{
  if (!a)
    return 0;
  return a->nents;
}

int archive_entry(struct archive *a, int idx, struct arcent *out)
{
  if (!a || idx < 0 || idx >= a->nents)
    return -1;
  memcpy(out, &a->ents[idx], sizeof(struct arcent));
  return 0;
}

int archive_extract(struct archive *a, int idx, const char *dest_path)
{
  if (!a || idx < 0 || idx >= a->nents)
    return -1;
  struct arcent *e = &a->ents[idx];
  if (e->is_dir) {
    struct statinfo st;
    /* mkdir fails on an existing directory, which an earlier entry's parents may have created */
    return (mkdir(dest_path) == 0 || (stat(dest_path, &st) == 0 && st.is_dir)) ? 0 : -1;
  }
  /* Only store and deflate are decodable; copying other methods would write compressed bytes. */
  if (a->fmt == ARC_FMT_ZIP && e->method != 0 && e->method != 8)
    return -1;

  if (stream_begin(dest_path) < 0)
    return -1;

  unsigned crc_accum = 0;
  int crc_wanted = (a->fmt == ARC_FMT_ZIP) ? (int)e->crc32 : 0;

  if (a->fmt == ARC_FMT_ZIP && e->method == 8) {
    /* Deflate-compressed ZIP: read compressed data, inflate to a
     * heap buffer, write to output, accumulate CRC32. */
    unsigned clen = e->data_size;
    if (clen > 64 * 1024 * 1024) {
      stream_end();
      return -1;
    }
    unsigned char *cbuf = malloc(clen);
    if (!cbuf) {
      stream_end();
      return -1;
    }
    if (readfile_at(a->path, e->data_offset, cbuf, clen) != (int)clen) {
      free(cbuf);
      stream_end();
      return -1;
    }
    struct inf_rd rd = {cbuf, (int)clen, 0};
    struct inf_wr wr = {NULL, 0, 0};
    int ir = inflate_stream(infl_reader, &rd, infl_writer, &wr);
    free(cbuf);
    if (ir < 0 || wr.len == 0) {
      free(wr.buf);
      stream_end();
      return -1;
    }

    crc_accum = crc32(0, wr.buf, (unsigned)wr.len);
    int written = stream_write(wr.buf, wr.len);
    free(wr.buf);
    if (written < 0) {
      stream_end();
      return -1;
    }
  } else if (a->fmt == ARC_FMT_TAR_GZ || a->fmt == ARC_FMT_TAR_ZST || a->fmt == ARC_FMT_TAR_BR) {
    /* In-memory (compressed-tar): copy directly from decomp buffer. */
    if (stream_write(a->decomp + e->data_offset, (int)e->data_size) < 0) {
      stream_end();
      return -1;
    }
  } else {
/* File-based (raw TAR or ZIP store): read in chunks. */
#define CHUNK 4096
    unsigned char buf[CHUNK];
    unsigned remain = e->data_size;
    unsigned off = e->data_offset;
    while (remain > 0) {
      unsigned chunk = remain > CHUNK ? CHUNK : remain;
      int n = readfile_at(a->path, off, buf, chunk);
      if (n <= 0) {
        stream_end();
        return -1;
      }
      if (stream_write(buf, n) < 0) {
        stream_end();
        return -1;
      }
      if (crc_wanted)
        crc_accum = crc32(crc_accum, buf, (unsigned)n);
      off += (unsigned)n;
      remain -= (unsigned)n;
    }
  }

  if (stream_end() < 0)
    return -1;

  if (crc_wanted && crc_accum != (unsigned)crc_wanted)
    return -1; /* CRC mismatch */

  return 0;
}

/* Return 1 if the entry name is safe (no path traversal), 0 otherwise. */
static int name_is_safe(const char *name)
{
  if (name[0] == '/' || name[0] == '\\')
    return 0;
  for (int i = 0; name[i]; i++) {
    if (name[i] == '.' && name[i + 1] == '.' && (name[i + 2] == '/' || name[i + 2] == '\\' || name[i + 2] == 0))
      return 0;
  }
  return 1;
}

int archive_extract_into(struct archive *a, int idx, const char *dest_dir)
{
  if (!a || idx < 0 || idx >= a->nents || !name_is_safe(a->ents[idx].name))
    return -1;
  const char *name = a->ents[idx].name;
  char dest[512];
  int dl = (int)strlen(dest_dir), nl = (int)strlen(name);
  if (dl + 1 + nl >= (int)sizeof dest)
    return -1;
  memcpy(dest, dest_dir, (unsigned)dl);
  dest[dl] = '/';
  memcpy(dest + dl + 1, name, (unsigned)nl + 1);

  /* Parents outermost first; mkdir's failure on an existing one is expected */
  for (int ci = dl + 1; dest[ci]; ci++) {
    if (dest[ci] == '/') {
      dest[ci] = 0;
      mkdir(dest);
      dest[ci] = '/';
    }
  }
  return archive_extract(a, idx, dest);
}

int archive_extract_all(struct archive *a, const char *dest_dir)
{
  if (!a)
    return -1;
  mkdir(dest_dir);
  for (int i = 0; i < a->nents; i++)
    if (name_is_safe(a->ents[i].name) && archive_extract_into(a, i, dest_dir) < 0)
      return -1;
  return 0;
}

void archive_close(struct archive *a)
{
  if (!a)
    return;
  if (a->path)
    free(a->path);
  if (a->decomp)
    free(a->decomp);
  free(a);
}
