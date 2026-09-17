/* kstring.c — see kstring.h. */
#include "kstring.h"

/* gcc lowers struct/array assignments (e.g. in fat.c) to implicit memcpy calls
 * even under -ffreestanding, so the freestanding kernel must provide one — with
 * the standard signature, since that's what the compiler emits calls to.
 *
 * A byte-loop memcpy is a footgun at -O2+, where the loop-idiom pass would
 * rewrite it into a self-call; the kernel builds with
 * -fno-tree-loop-distribute-patterns (see toolchain.mk) precisely so this loop
 * can live here directly rather than behind a differently-named worker. */
/* Word-at-a-time copy: `rep movsl` for the dword bulk, `rep movsb` for the
 * 0..3 trailing bytes. Byte-at-a-time was ~4-8x slower and dominated every
 * framebuffer blit/present at -O1 (the compositor copies whole windows). */
void *memcpy(void *dst, const void *src, uint32_t n)
{
  int d0, d1, d2;
  __asm__ volatile("rep ; movsl\n\t"
                   "movl %4, %%ecx\n\t"
                   "andl $3, %%ecx\n\t"
                   "rep ; movsb"
                   : "=&c"(d0), "=&D"(d1), "=&S"(d2)
                   : "0"(n >> 2), "g"(n), "1"(dst), "2"(src)
                   : "memory");
  return dst;
}

/* Overlap-safe copy: gcc lowers some struct/array moves to memmove too, and the
 * forward loop above corrupts an upward-overlapping copy, so copy back-to-front
 * when the destination sits inside the source. */
void *memmove(void *dst, const void *src, uint32_t n)
{
  uint8_t *d = dst;
  const uint8_t *s = src;
  if (d < s) {
    return memcpy(dst, src, n);
  } else {
    d += n;
    s += n;
    while (n--)
      *--d = *--s;
  }
  return dst;
}

/* The other primitive gcc emits implicitly (e.g. for `struct x = {0}`). */
void *memset(void *dst, int c, uint32_t n)
{
  int d0, d1;
  unsigned cc = (unsigned char)c;
  unsigned pat = cc | (cc << 8) | (cc << 16) | (cc << 24);
  __asm__ volatile("rep ; stosl\n\t"
                   "movl %4, %%ecx\n\t"
                   "andl $3, %%ecx\n\t"
                   "rep ; stosb"
                   : "=&c"(d0), "=&D"(d1)
                   : "a"(pat), "0"(n >> 2), "g"(n), "1"(dst)
                   : "memory");
  return dst;
}

int memcmp(const void *a, const void *b, uint32_t n)
{
  const uint8_t *pa = a, *pb = b;
  while (n--) {
    if (*pa != *pb)
      return (int)*pa - (int)*pb;
    pa++;
    pb++;
  }
  return 0;
}

uint32_t strlen(const char *s)
{
  uint32_t n = 0;
  while (s[n])
    n++;
  return n;
}

int strcmp(const char *a, const char *b)
{
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

/* Bounded string copy (BSD strlcpy): copy `src` into `dst` of `size` bytes,
 * always NUL-terminating when size > 0. Returns strlen(src). Avoids the
 * per-file local copies kernel code kept re-rolling. */
uint32_t strlcpy(char *dst, const char *src, uint32_t size)
{
  uint32_t n = 0;
  if (size) {
    while (n < size - 1 && src[n]) {
      dst[n] = src[n];
      n++;
    }
    dst[n] = '\0';
  }
  while (src[n])
    n++;
  return n;
}

/* --------------------------------------------------------------- formatting */

/* Emit an unsigned value in `base` (10 or 16), `upper` for hex case, padded to
 * `width` with '0' (zero) or ' ' (space). `neg` prepends a '-': with zero
 * padding the sign leads ("-007"); with space padding the spaces lead ("  -7").
 */
static void emit_uint(void (*sink)(char, void *), void *ctx, uint32_t v, uint32_t base, int upper, int width, int zero,
                      int neg)
{
  const char *digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  char tmp[32];
  int n = 0;
  do {
    tmp[n++] = digs[v % base];
    v /= base;
  } while (v);
  int total = n + (neg ? 1 : 0);
  if (zero) {
    if (neg)
      sink('-', ctx);
    for (int pad = width - total; pad > 0; pad--)
      sink('0', ctx);
  } else {
    for (int pad = width - total; pad > 0; pad--)
      sink(' ', ctx);
    if (neg)
      sink('-', ctx);
  }
  while (n--)
    sink(tmp[n], ctx);
}

void kvformat(void (*sink)(char, void *), void *ctx, const char *fmt, va_list ap)
{
  for (; *fmt; fmt++) {
    if (*fmt != '%') {
      sink(*fmt, ctx);
      continue;
    }
    fmt++;
    int zero = 0, width = 0;
    if (*fmt == '0') {
      zero = 1;
      fmt++;
    }
    while (*fmt >= '0' && *fmt <= '9') {
      width = width * 10 + (*fmt - '0');
      fmt++;
    }
    switch (*fmt) {
    case 's': {
      const char *s = va_arg(ap, const char *);
      if (!s)
        s = "(null)";
      while (*s)
        sink(*s++, ctx);
      break;
    }
    case 'c':
      sink((char)va_arg(ap, int), ctx);
      break;
    case 'd':
    case 'i': {
      int v = va_arg(ap, int), neg = v < 0;
      emit_uint(sink, ctx, neg ? -(uint32_t)v : (uint32_t)v, 10, 0, width, zero, neg);
      break;
    }
    case 'u':
      emit_uint(sink, ctx, va_arg(ap, uint32_t), 10, 0, width, zero, 0);
      break;
    case 'x':
      emit_uint(sink, ctx, va_arg(ap, uint32_t), 16, 0, width, zero, 0);
      break;
    case 'X':
      emit_uint(sink, ctx, va_arg(ap, uint32_t), 16, 1, width, zero, 0);
      break;
    case 'p':
      sink('0', ctx);
      sink('x', ctx);
      emit_uint(sink, ctx, (uint32_t)(uintptr_t)va_arg(ap, void *), 16, 0, 8, 1, 0);
      break;
    case '%':
      sink('%', ctx);
      break;
    case '\0':
      return; /* trailing '%' — stop */
    default:
      sink('%', ctx);
      sink(*fmt, ctx);
      break;
    }
  }
}

/* ksnprintf sink: write into a buffer, but keep counting past the end so the
 * return value reports the would-be length (C99). */
struct buf_sink {
  char *p;
  uint32_t len, cap;
};
static void buf_put(char c, void *ctx)
{
  struct buf_sink *b = ctx;
  if (b->len + 1 < b->cap) /* leave room for the NUL */
    b->p[b->len] = c;
  b->len++;
}

int kvsnprintf(char *buf, uint32_t size, const char *fmt, va_list ap)
{
  struct buf_sink b = {buf, 0, size};
  kvformat(buf_put, &b, fmt, ap);
  if (size)
    buf[b.len < size ? b.len : size - 1] = '\0';
  return (int)b.len;
}

int ksnprintf(char *buf, uint32_t size, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  int n = kvsnprintf(buf, size, fmt, ap);
  va_end(ap);
  return n;
}
