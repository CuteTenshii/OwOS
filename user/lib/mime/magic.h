/* magic.h: identify content from its leading bytes, the way file(1) does.
 *
 * Covers executables, images, audio and video containers, fonts, archives, disk and filesystem
 * images, and text (encoding, line terminators, scripts and common markup). Pure byte inspection:
 * no I/O and no allocation, so it links into any program and runs on the host for tests. */
#ifndef OWOS_MAGIC_H
#define OWOS_MAGIC_H

/* Bytes worth handing to magic_identify: ISO 9660 descriptors sit at 32 KiB and the btrfs
 * superblock at 64 KiB. Fewer still works, those formats just go unrecognised. */
#define MAGIC_PEEK (68 * 1024)

struct magic_info {
  char desc[256];      /* "PNG image data, 640 x 480, 8-bit/color RGBA, non-interlaced" */
  const char *mime;    /* "image/png"; static, never freed */
  const char *charset; /* "us-ascii", "utf-8", "utf-16le", ... or "binary" for non-text */
};

/* Identify the `len` leading bytes of a file. Set `complete` when they are the whole file: it
 * enables checks that need the end (gzip's stored size, JSON's closing bracket). */
void magic_identify(const void *data, int len, int complete, struct magic_info *out);

#endif
