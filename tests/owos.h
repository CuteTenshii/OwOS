/* owos.h: host test shim (tests/), NOT the real kernel/user owos.h.
 *
 * The real user/lib/owos.h is a freestanding libc for the -m32 guest and can't
 * compile on the host. httpcache.c and headers.c pull only a handful of decls
 * from it; this shim supplies exactly those over the host libc so the pure
 * caching-policy code can be unit-tested natively (see httpcache_test.c). */
#ifndef OWOS_HOST_TEST_SHIM_H
#define OWOS_HOST_TEST_SHIM_H

#include "../user/lib/ascii.h"
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Key codes the widgets under test use; mirror user/lib/owos.h. */
#define KEY_UP 0x01
#define KEY_DOWN 0x02
#define KEY_HOME 0x05
#define KEY_END 0x06
#define KEY_LEFT 0x03
#define KEY_RIGHT 0x04
#define KEY_DELETE 0x07
#define KMOD_SHIFT 0x01
#define KMOD_CTRL 0x02

/* Provided per suite where a library under test draws randomness or reads the clock. */
void rng_bytes(void *out, int n);
long long now_ms(void);

/* The guest strdup returns 0 for NULL; the host one would crash. */
static inline char *owos_strdup(const char *s) { return s ? strdup(s) : 0; }
#define strdup owos_strdup

/* Byte-exact string compare, matching user/lib/owos.h's inline version. */
static inline int streq(const char *a, const char *b) { return strcmp(a, b) == 0; }

/* Whole-file I/O over host stdio, mirroring the guest libc signatures so
 * storage libs (sqlite.c) that persist through readfile/writefile compile and
 * run natively. writefile truncates; readfile_at reads `len` bytes from `off`. */
static inline int writefile(const char *name, const void *buf, int len)
{
  FILE *f = fopen(name, "wb");
  if (!f)
    return -1;
  int wrote = (len > 0) ? (int)fwrite(buf, 1, (size_t)len, f) : 0;
  fclose(f);
  return (wrote == len) ? 0 : -1;
}
static inline int readfile(const char *name, char *buf, int max)
{
  FILE *f = fopen(name, "rb");
  if (!f)
    return -1;
  int got = (int)fread(buf, 1, (size_t)max, f);
  fclose(f);
  return got;
}
static inline int readfile_at(const char *name, unsigned off, void *buf, unsigned len)
{
  FILE *f = fopen(name, "rb");
  if (!f)
    return -1;
  if (fseek(f, (long)off, SEEK_SET) != 0) {
    fclose(f);
    return -1;
  }
  int got = (int)fread(buf, 1, (size_t)len, f);
  fclose(f);
  return got;
}

/* File metadata, mirroring user/lib/owos.h so storage/media libs that stat a file
 * before slurping it (media_video, mpeg/h264 front ends) compile natively. */
struct statinfo {
  int is_dir;
  unsigned size;
  unsigned mtime;
  unsigned mode;
  unsigned uid;
  unsigned gid;
};
static inline int stat(const char *path, struct statinfo *out)
{
  FILE *f = fopen(path, "rb");
  if (!f)
    return -1;
  /* Linux opens a directory for reading but refuses the read itself */
  out->is_dir = fgetc(f) == EOF && ferror(f) && errno == EISDIR;
  fseek(f, 0, SEEK_END);
  out->size = out->is_dir ? 0 : (unsigned)ftell(f);
  out->mtime = 0;
  out->mode = 0644;
  out->uid = out->gid = 0;
  fclose(f);
  return 0;
}

/* UTF-8 encode a code point into `out` (up to 4 bytes); returns the byte count.
 * Mirrors user/lib/owos.h so the freestanding lexer/builtins compile on the host. */
static inline int utf8_encode(unsigned cp, char *out)
{
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}
static inline int utf8_decode(const char *s, const char *end, unsigned *cp)
{
  unsigned char c = (unsigned char)s[0];
  int n = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
  unsigned v = c & (n == 1 ? 0x7F : n == 2 ? 0x1F : n == 3 ? 0x0F : 0x07);
  int i;
  if (s + n > end) {
    *cp = 0xFFFD;
    return 1;
  }
  for (i = 1; i < n; i++) {
    if (((unsigned char)s[i] & 0xC0) != 0x80) {
      *cp = 0xFFFD;
      return 1;
    }
    v = (v << 6) | ((unsigned char)s[i] & 0x3F);
  }
  *cp = v;
  return n;
}

unsigned msec(void);
long long now_ms(void);

/* Threads: host tests map them onto pthreads (tests/bindings_host.c). */
int thread_create_arg(void (*fn)(void *), void *arg, void *stack_top);
void *thread_stack_alloc(int bytes);
void thread_stack_free(void *top);
int waitpid_nb(int pid, int *status);
void msleep(unsigned ms);
static inline void thread_exit(void) { pthread_exit(0); }

/* Guest network + CSPRNG decls the freestanding net libs (stun.c, resolve.c)
 * pull from the real owos.h. Host tests exercise only the wire codecs, so the
 * test binary supplies stub definitions; these mirror the guest signatures.
 * Codes and SK_* match user/lib/owos.h. */
#define NET_EINTR (-4)
#define SK_TCP 1
#define SK_UDP 2
#define SK_ICMP 3
int getrandom(void *buf, int len);
int net_dial(const char *host, int port, int proto);
int net_send(int h, const void *buf, int len);
int net_recv(int h, void *buf, int max);
int net_close(int h);
int net_recv_nb(int h, void *buf, int max);
#define ALPN_NONE 0
#define ALPN_HTTP11 1
#define ALPN_H2 2

#endif /* OWOS_HOST_TEST_SHIM_H */
