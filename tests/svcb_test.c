/* svcb_test.c — HTTPS RR (type 65) ECH extraction from DNS responses. */
#define OWOS_SUITE_TEST
#include "test.h"
#include <string.h>

#include "../user/lib/resolve/svcb.c"

/* Header + question "a.b" HTTPS IN, ancount answers appended by the caller. */
static int header(uint8_t *m, int an)
{
  static const uint8_t q[] = {0, 0, 0x81, 0x80, 0, 1, 0, 0, 0, 0, 0, 0, 1, 'a', 1, 'b', 0, 0, 65, 0, 1};
  memcpy(m, q, sizeof q);
  m[7] = (uint8_t)an;
  return (int)sizeof q;
}

/* Owner via compression pointer, type, class IN, ttl, rdlen, then rdata. */
static int rr(uint8_t *m, int n, int type, const uint8_t *rd, int rdlen)
{
  static const uint8_t h[] = {0xC0, 12, 0, 0, 0, 1, 0, 0, 0, 60};
  memcpy(m + n, h, sizeof h);
  m[n + 2] = (uint8_t)(type >> 8);
  m[n + 3] = (uint8_t)type;
  n += (int)sizeof h;
  m[n++] = (uint8_t)(rdlen >> 8);
  m[n++] = (uint8_t)rdlen;
  memcpy(m + n, rd, rdlen);
  return n + rdlen;
}

/* priority, root target, optional alpn (key 1) and ech (key 5) params. */
static int svc(uint8_t *rd, int prio, const uint8_t *ech, int echlen)
{
  int n = 0;
  rd[n++] = 0;
  rd[n++] = (uint8_t)prio;
  rd[n++] = 0; /* TargetName "." */
  if (prio) {
    static const uint8_t alpn[] = {0, 1, 0, 3, 2, 'h', '2'};
    memcpy(rd + n, alpn, sizeof alpn);
    n += (int)sizeof alpn;
  }
  if (ech) {
    rd[n++] = 0;
    rd[n++] = 5;
    rd[n++] = 0;
    rd[n++] = (uint8_t)echlen;
    memcpy(rd + n, ech, echlen);
    n += echlen;
  }
  return n;
}

static void extracts_ech(void)
{
  static const uint8_t cfg[] = {0, 6, 0xfe, 0x0d, 0, 2, 9, 9};
  uint8_t m[512], rd[64], out[64];
  int n = header(m, 1);
  n = rr(m, n, 65, rd, svc(rd, 1, cfg, sizeof cfg));
  expect(svcb_find_ech(m, n, out, sizeof out) == (int)sizeof cfg);
  expect(memcmp(out, cfg, sizeof cfg) == 0);
}

static void picks_lowest_priority_with_ech(void)
{
  static const uint8_t a[] = {1, 1, 1}, b[] = {2, 2, 2, 2};
  uint8_t m[512], rd[64], out[64];
  int n = header(m, 3);
  n = rr(m, n, 65, rd, svc(rd, 3, a, sizeof a));
  n = rr(m, n, 65, rd, svc(rd, 2, b, sizeof b));
  n = rr(m, n, 65, rd, svc(rd, 1, 0, 0)); /* best priority but no ech */
  expect(svcb_find_ech(m, n, out, sizeof out) == (int)sizeof b);
  expect(memcmp(out, b, sizeof b) == 0);
}

static void skips_alias_and_other_types(void)
{
  static const uint8_t cfg[] = {7, 7};
  uint8_t m[512], rd[64], out[64];
  int n = header(m, 3);
  n = rr(m, n, 5, (const uint8_t *)"\1x\0", 3);      /* CNAME */
  n = rr(m, n, 65, rd, svc(rd, 0, cfg, sizeof cfg)); /* AliasMode */
  n = rr(m, n, 65, rd, svc(rd, 1, 0, 0));
  expect(svcb_find_ech(m, n, out, sizeof out) == 0);
}

static void rejects_oversize_and_truncated(void)
{
  static const uint8_t cfg[] = {1, 2, 3, 4, 5, 6, 7, 8};
  uint8_t m[512], rd[64], out[4];
  int n = header(m, 1);
  n = rr(m, n, 65, rd, svc(rd, 1, cfg, sizeof cfg));
  expect(svcb_find_ech(m, n, out, sizeof out) == 0);
  uint8_t big[64];
  for (int cut = 0; cut < n; cut++)
    expect(svcb_find_ech(m, cut, big, sizeof big) == 0);
  expect(svcb_find_ech(m, n, big, sizeof big) == (int)sizeof cfg);
}

int main(void)
{
  test("extracts the ech SvcParam", extracts_ech);
  test("lowest-priority record with ech wins", picks_lowest_priority_with_ech);
  test("AliasMode and other types are ignored", skips_alias_and_other_types);
  test("oversize and truncated responses", rejects_oversize_and_truncated);
  return 0;
}
