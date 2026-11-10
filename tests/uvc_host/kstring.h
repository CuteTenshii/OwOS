/* Host stand-in for kernel/kstring.h. */
#ifndef UVC_HOST_KSTRING_H
#define UVC_HOST_KSTRING_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ksnprintf snprintf
#define strlcpy uvc_host_strlcpy

static inline uint32_t uvc_host_strlcpy(char *dst, const char *src, uint32_t size)
{
  uint32_t n = (uint32_t)strlen(src);
  if (size) {
    uint32_t c = n < size - 1 ? n : size - 1;
    memcpy(dst, src, c);
    dst[c] = 0;
  }
  return n;
}

static inline int streq(const char *a, const char *b) { return strcmp(a, b) == 0; }

#endif
