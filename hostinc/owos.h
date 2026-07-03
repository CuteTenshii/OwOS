/* Host shim standing in for user/lib/owos.h when building browser/font code on
 * the host (gshot/fontshot previews). Only what those translation units use. */
#ifndef OWOS_HOSTSHIM
#define OWOS_HOSTSHIM
#include <stdlib.h>
#include <string.h>

static inline int streq(const char *a, const char *b) { return strcmp(a, b) == 0; }

int utf8_seqlen(unsigned char lead);
int utf8_decode(const char *s, const char *end, unsigned *cp);
int utf8_encode(unsigned cp, char *out);

/* Filesystem bits html.c's @font-face local loader touches (the harness stubs
 * these to "absent", so the /etc/fonts monospace path is simply skipped). */
struct statinfo { int is_dir; unsigned size; unsigned mtime; };
int stat(const char *path, struct statinfo *out);
int readfile_at(const char *name, unsigned off, void *buf, unsigned len);
#endif
