/* archive.h: a reusable archive-format reader for user programs.
 *
 * Whole-archive view-and-extract API: open an archive by path, enumerate its
 * entries (with name, size, and type), extract one entry or all of them to the
 * filesystem. The read side only (no write); this is the building block for the
 * GUI archive viewer and for any CLI tool that inspects archives.
 *
 * Supported formats detected automatically by content, not extension:
 *   - TAR (ustar / POSIX / old-GNU), raw and uncompressed
 *   - TAR compressed: gzip (.tar.gz/.tgz), zstd (.tar.zst), brotli (.tar.br)
 *   - ZIP (deflate + store only, from the central directory)
 *
 * Compressed tars are decompressed entirely into a heap buffer at open time
 * (using the existing inflate/zstd/brotli libs); raw TAR and ZIP only keep an
 * in-memory index and read on demand during extraction.
 */
#ifndef OWOS_ARCHIVE_H
#define OWOS_ARCHIVE_H

#define ARC_NAME_MAX 256
#define ARC_MAX_ENTS 2048

/* Internal format tags. */
#define ARC_FMT_TAR 1
#define ARC_FMT_ZIP 2
#define ARC_FMT_TAR_GZ 3
#define ARC_FMT_TAR_ZST 4
#define ARC_FMT_TAR_BR 5

struct arcent {
  char name[ARC_NAME_MAX]; /* path inside the archive            */
  unsigned size;           /* uncompressed size (0 for dirs)     */
  unsigned packed;         /* compressed/stored size, 0 if same  */
  unsigned mtime;          /* packed wall-clock timestamp        */
  int is_dir;              /* 1 = directory entry                */
  int method;              /* compression method (0=store, 8=deflate) */
  unsigned crc32;          /* stored CRC32 (0 if not available)  */
  /* --- internal (not part of the public listing) --- */
  unsigned data_offset; /* byte offset of file data in source */
  unsigned data_size;   /* bytes of data (stored size)        */
  int format;           /* internal: ARC_FMT_*                */
};

struct archive {
  char *path;
  unsigned char *decomp;
  int decomp_len;
  int fmt;
  int nents;
  struct arcent ents[ARC_MAX_ENTS];
};

/* Open an archive at `path` (an absolute or cwd-relative owofs path). Auto-
 * detects the format by inspecting the first few bytes. Returns NULL on error
 * (file not found, unrecognised format, OOM during decompress). */
struct archive *archive_open(const char *path);

/* Number of entries in the opened archive. */
int archive_count(struct archive *a);

/* Fill `out` with the idx-th entry (0-based). Returns 0 on success, -1 if
 * out of range. The returned entry's .name is NUL-terminated. */
int archive_entry(struct archive *a, int idx, struct arcent *out);

/* Extract entry `idx` to the file at `dest_path` (an absolute owofs path;
 * parent directories are created on demand). Returns 0 / -1. */
int archive_extract(struct archive *a, int idx, const char *dest_path);

/* Extract every entry into the directory `dest_dir` (created if absent),
 * preserving the archive's internal directory structure. Returns 0 / -1. */
int archive_extract_all(struct archive *a, const char *dest_dir);

/* Free the archive object and all owned buffers. */
void archive_close(struct archive *a);

/* Return the format string for an already-opened archive (e.g. "TAR.BR").
 * More reliable than archive_detect() for formats without magic bytes. */
const char *archive_format(struct archive *a);

/* Sniff the first few bytes of a file and return a short string describing
 * the format (e.g. "ZIP", "TAR", "TAR.GZ", "TAR.ZST", "TAR.BR"). Returns
 * "unknown" on an unrecognised signature or an unreadable file. The returned
 * pointer is to a static buffer; copy it if you need to keep it. */
const char *archive_detect(const char *path);

#endif /* OWOS_ARCHIVE_H */
