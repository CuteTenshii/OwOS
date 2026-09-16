/* base64.c — RFC 4648 base64 encode/decode (see base64.h). */
#include "base64.h"

static const char ENC[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int base64_encoded_len(int n) { return n <= 0 ? 0 : ((n + 2) / 3) * 4; }

int base64_encode(const unsigned char *src, int n, char *dst)
{
  int o = 0, i = 0;
  for (; i + 3 <= n; i += 3) {
    unsigned v = ((unsigned)src[i] << 16) | ((unsigned)src[i + 1] << 8) | src[i + 2];
    dst[o++] = ENC[(v >> 18) & 63];
    dst[o++] = ENC[(v >> 12) & 63];
    dst[o++] = ENC[(v >> 6) & 63];
    dst[o++] = ENC[v & 63];
  }
  if (n - i == 1) {
    unsigned v = (unsigned)src[i] << 16;
    dst[o++] = ENC[(v >> 18) & 63];
    dst[o++] = ENC[(v >> 12) & 63];
    dst[o++] = '=';
    dst[o++] = '=';
  } else if (n - i == 2) {
    unsigned v = ((unsigned)src[i] << 16) | ((unsigned)src[i + 1] << 8);
    dst[o++] = ENC[(v >> 18) & 63];
    dst[o++] = ENC[(v >> 12) & 63];
    dst[o++] = ENC[(v >> 6) & 63];
    dst[o++] = '=';
  }
  dst[o] = '\0';
  return o;
}

static int dval(int c)
{
  if (c >= 'A' && c <= 'Z')
    return c - 'A';
  if (c >= 'a' && c <= 'z')
    return c - 'a' + 26;
  if (c >= '0' && c <= '9')
    return c - '0' + 52;
  if (c == '+')
    return 62;
  if (c == '/')
    return 63;
  return -1;
}

int base64_decoded_cap(int len) { return len <= 0 ? 0 : (len / 4 + 1) * 3; }

int base64_decode(const char *src, int len, unsigned char *dst)
{
  int o = 0, acc = 0, nb = 0;
  for (int i = 0; i < len; i++) {
    int c = (unsigned char)src[i];
    if (c == '=')
      break;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v')
      continue;
    int v = dval(c);
    if (v < 0)
      return -1;
    acc = (acc << 6) | v;
    nb += 6;
    if (nb >= 8) {
      nb -= 8;
      dst[o++] = (unsigned char)((acc >> nb) & 0xff);
    }
  }
  return o;
}
