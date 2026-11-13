/* install.h: copy the system files embedded in the kernel image onto the boot disk. */
#ifndef OWOS_INSTALL_H
#define OWOS_INSTALL_H

#include <stdint.h>

/* One embedded file; the table is generated into kernel/payload.c by tools/genpayload.sh. */
struct payload_file {
  const char *path;
  const unsigned char *start, *end;
  uint32_t crc; /* POSIX cksum of the contents, taken at build time */
  uint32_t mode;
};

extern const struct payload_file payload[];
extern const unsigned payload_count;

/* Total size of the payload's files in bytes. */
uint32_t install_payload_bytes(void);

/* The payload file installed at `path`, or NULL. */
const struct payload_file *install_payload_find(const char *path);

/* Bring the disk in line with the payload, as recorded in the /var/lib/installed manifest (pkgdb.h): write files
 * that are missing, stale or older than this kernel, keep newer updates, and delete files a previous kernel
 * installed that this one no longer ships. Writes nothing when the disk already matches. */
void install_payload(void);

#endif /* OWOS_INSTALL_H */
