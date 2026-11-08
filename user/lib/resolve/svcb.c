/* svcb.c — HTTPS RR (type 65) parsing; see svcb.h. */

#include "svcb.h"

#define SVCPARAM_ECH 5

static unsigned be16(const uint8_t *p) { return ((unsigned)p[0] << 8) | p[1]; }

int dns_name_len(const uint8_t *msg, int pos, int end)
{
  int p = pos;
  while (p < end) {
    unsigned b = msg[p];
    if (b == 0)
      return p + 1 - pos;
    if ((b & 0xC0) == 0xC0)
      return p + 2 - pos;
    p += 1 + (int)b;
  }
  return 0;
}

/* Locate the ech value in one HTTPS RDATA; 0 if it is AliasMode, malformed, or has none. */
static int rdata_ech(const uint8_t *rd, int rdlen, unsigned *prio, const uint8_t **val)
{
  if (rdlen < 3)
    return 0;
  *prio = be16(rd);
  if (*prio == 0)
    return 0;
  int tl = dns_name_len(rd, 2, rdlen); /* TargetName: uncompressed */
  if (!tl)
    return 0;
  int p = 2 + tl;
  while (p + 4 <= rdlen) {
    unsigned key = be16(rd + p), vl = be16(rd + p + 2);
    p += 4;
    if (p + (int)vl > rdlen)
      return 0;
    if (key == SVCPARAM_ECH) {
      *val = rd + p;
      return (int)vl;
    }
    p += (int)vl;
  }
  return 0;
}

int svcb_find_ech(const uint8_t *msg, int len, uint8_t *out, int max)
{
  if (len < 12)
    return 0;
  unsigned qd = be16(msg + 4), an = be16(msg + 6);
  int pos = 12;
  for (unsigned i = 0; i < qd; i++) {
    int l = dns_name_len(msg, pos, len);
    if (!l)
      return 0;
    pos += l + 4;
  }

  int best = 0;
  unsigned best_prio = 0;
  for (unsigned i = 0; i < an; i++) {
    int l = dns_name_len(msg, pos, len);
    if (!l || pos + l + 10 > len)
      break;
    pos += l;
    unsigned type = be16(msg + pos);
    int rdlen = (int)be16(msg + pos + 8);
    pos += 10;
    if (pos + rdlen > len)
      break;
    const uint8_t *val;
    unsigned prio;
    int vl;
    if (type == DNS_TYPE_HTTPS && (vl = rdata_ech(msg + pos, rdlen, &prio, &val)) > 0 && vl <= max &&
        (!best || prio < best_prio)) {
      for (int k = 0; k < vl; k++)
        out[k] = val[k];
      best = vl;
      best_prio = prio;
    }
    pos += rdlen;
  }
  return best;
}
