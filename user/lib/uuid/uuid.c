/* uuid.c: RFC 9562 UUIDs. See uuid.h. */
#include "uuid.h"
#include "crypto.h"
#include "owos.h"

const uuid_t UUID_NS_DNS = {0x6b, 0xa7, 0xb8, 0x10, 0x9d, 0xad, 0x11, 0xd1, 0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8};
const uuid_t UUID_NS_URL = {0x6b, 0xa7, 0xb8, 0x11, 0x9d, 0xad, 0x11, 0xd1, 0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8};
const uuid_t UUID_NS_OID = {0x6b, 0xa7, 0xb8, 0x12, 0x9d, 0xad, 0x11, 0xd1, 0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8};
const uuid_t UUID_NS_X500 = {0x6b, 0xa7, 0xb8, 0x14, 0x9d, 0xad, 0x11, 0xd1, 0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8};

static void stamp(uuid_t u, int version)
{
  u[6] = (unsigned char)(u[6] & 0x0F | version << 4);
  u[8] = (unsigned char)(u[8] & 0x3F | 0x80);
}

void uuid_v4_from(uuid_t out, const unsigned char rnd[16])
{
  memcpy(out, rnd, 16);
  stamp(out, 4);
}

void uuid_v7_from(uuid_t out, unsigned long long ms, const unsigned char rnd[10])
{
  for (int i = 0; i < 6; i++)
    out[i] = (unsigned char)(ms >> (40 - 8 * i));
  memcpy(out + 6, rnd, 10);
  stamp(out, 7);
}

void uuid_v5(uuid_t out, const uuid_t ns, const void *name, int len)
{
  struct sha1 h;
  unsigned char d[SHA1_DIGEST];
  sha1_init(&h);
  sha1_update(&h, ns, 16);
  sha1_update(&h, name, (unsigned)len);
  sha1_final(&h, d);
  memcpy(out, d, 16);
  stamp(out, 5);
}

void uuid_generate_v4(uuid_t out)
{
  unsigned char r[16];
  rng_bytes(r, sizeof r);
  uuid_v4_from(out, r);
}

void uuid_generate_v7(uuid_t out)
{
  unsigned char r[10];
  rng_bytes(r, sizeof r);
  uuid_v7_from(out, (unsigned long long)now_ms(), r);
}

void uuid_to_string(const uuid_t u, char *out, int upper)
{
  const char *hex = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  int n = 0;
  for (int i = 0; i < 16; i++) {
    if (i == 4 || i == 6 || i == 8 || i == 10)
      out[n++] = '-';
    out[n++] = hex[u[i] >> 4];
    out[n++] = hex[u[i] & 15];
  }
  out[n] = '\0';
}

static int hexval(int c)
{
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

int uuid_parse(const char *s, uuid_t out)
{
  uuid_t u;
  int len, braced = 0, dashed;
  if (!s)
    return -1;
  if (starts_ci(s, "urn:uuid:"))
    s += 9;
  else if (*s == '{')
    braced = 1, s++;
  len = (int)strlen(s) - braced;
  if (braced && s[len] != '}')
    return -1;
  if (len != 36 && len != 32)
    return -1;
  dashed = len == 36;
  for (int i = 0, n = 0; n < 16; n++) {
    int hi, lo;
    if (dashed && (n == 4 || n == 6 || n == 8 || n == 10)) {
      if (s[i++] != '-')
        return -1;
    }
    hi = hexval(s[i]);
    lo = hi < 0 ? -1 : hexval(s[i + 1]);
    if (lo < 0)
      return -1;
    u[n] = (unsigned char)(hi << 4 | lo);
    i += 2;
  }
  memcpy(out, u, 16);
  return 0;
}

int uuid_compare(const uuid_t a, const uuid_t b) { return memcmp(a, b, 16); }

int uuid_is_nil(const uuid_t u)
{
  for (int i = 0; i < 16; i++)
    if (u[i])
      return 0;
  return 1;
}

int uuid_version(const uuid_t u) { return u[6] >> 4; }

int uuid_variant(const uuid_t u) { return u[8] >> 6 == 2 ? 2 : u[8] >> 5 == 6 ? 6 : u[8] >> 7 == 0 ? 0 : 7; }

unsigned long long uuid_v7_time(const uuid_t u)
{
  unsigned long long ms = 0;
  for (int i = 0; i < 6; i++)
    ms = ms << 8 | u[i];
  return ms;
}
