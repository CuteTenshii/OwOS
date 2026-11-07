/* rar.h - read-only RAR 3.x/4.x/5.x reader: iterate entries, extract through a write callback.
 * Archives are untrusted; allocation and output are capped, checksum failures are errors. */
#ifndef OWOS_RAR_H
#define OWOS_RAR_H

#include <stdbool.h>
#include <stdint.h>

#define RAR_OK 0
#define RAR_EIO (-1)          /* source read failed */
#define RAR_EFORMAT (-2)      /* not a RAR archive or a malformed header */
#define RAR_ECRC (-3)         /* header or data checksum mismatch */
#define RAR_EPASSWORD (-4)    /* password needed or wrong */
#define RAR_EUNSUPPORTED (-5) /* method/feature not implemented */
#define RAR_ENOMEM (-6)       /* allocation failed or over the memory budget */
#define RAR_ELIMIT (-7)       /* extraction size limit reached */
#define RAR_EWRITE (-8)       /* the write callback failed */
#define RAR_EVOLUME (-9)      /* a volume of a multi-volume set is missing */
#define RAR_ECORRUPT (-10)    /* compressed data is corrupt */
#define RAR_EOLD (-11)        /* old RAR (1.x/2.x) method */
#define RAR_ESTATE (-12)      /* operation not valid in this state (e.g. list-only) */

const char *rar_strerror(int err);

struct rar_src {
  /* Read up to len bytes at off; return the count (0 at EOF) or < 0 on error. */
  int (*read)(void *ctx, uint64_t off, void *buf, uint32_t len);
  void (*close)(void *ctx); /* may be NULL */
  void *ctx;
};

struct rar_cfg {
  /* Open the volume called `name` (derived from cfg.name); 0 on success. May be NULL. */
  int (*open_volume)(void *ctx, const char *name, struct rar_src *out);
  void *ctx;
  const char *name;     /* name of the first volume, used to derive the others */
  const char *password; /* UTF-8, may be NULL */
  uint64_t max_total;   /* cap on bytes produced by rar_extract over the archive's life, 0 = none */
  uint64_t max_mem;     /* cap on decoder memory (window, PPMd model), 0 = 512 MiB */
  bool list_only;       /* never decode: solid state is not kept, rar_extract is refused for packed data */
};

struct rar_entry {
  const char *name; /* UTF-8, '/' separators; valid until the next rar_next/rar_close */
  const char *link; /* redirection target or NULL */
  uint64_t size;    /* unpacked size, UINT64_MAX if unknown */
  uint64_t packed;  /* packed size summed over every volume part */
  uint32_t crc;     /* CRC32 of the unpacked data (valid when has_crc) */
  uint32_t mtime;   /* Unix seconds, 0 if unknown */
  uint32_t attr;    /* raw host attributes */
  uint32_t mode;    /* Unix permission bits when the host is Unix, else 0 */
  uint8_t host;     /* 0 = Windows/DOS, 1 = Unix */
  uint8_t method;   /* 0 store, 1..5 */
  uint8_t version;  /* unpack version: 15, 20, 26, 29, 36 (RAR 1.5-4.x) or 50 (RAR 5) */
  bool has_crc;
  bool is_dir;
  bool is_symlink;
  bool is_hardlink;
  bool encrypted;
  bool solid;
  bool split; /* spans more than one volume */
};

struct rar;

/* Open an archive. The source is owned by the returned handle (closed by rar_close). On error the source is closed too.
 */
int rar_open(struct rar **out, const struct rar_src *src, const struct rar_cfg *cfg);
void rar_close(struct rar *r);

/* 1 = entry filled in, 0 = end of archive, < 0 = error. */
int rar_next(struct rar *r, struct rar_entry *e);

/* Decode the current entry, pushing bytes to write (return 0 to continue, nonzero to abort).
 * RAR_ECRC is returned after the whole entry was written when the checksum does not match. */
int rar_extract(struct rar *r, int (*write)(void *ctx, const void *buf, uint32_t len), void *ctx);

/* Archive-level facts, valid after rar_open. */
bool rar_is_solid(const struct rar *r);
bool rar_is_multivolume(const struct rar *r);
bool rar_headers_encrypted(const struct rar *r);

/* Replace the password (e.g. after RAR_EPASSWORD); keys are re-derived on demand. */
int rar_set_password(struct rar *r, const char *password);

/* True when `name` is a relative path without ".." components, drive letters or a leading slash. */
bool rar_path_is_safe(const char *name);
int rar_format(const struct rar *r); /* 4 or 5 */

#endif /* OWOS_RAR_H */
