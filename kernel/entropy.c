/* entropy.c: see entropy.h.
 * The pool is stirred from interrupts with the TSC (low bits carry interrupt-latency jitter);
 * extraction hashes the pool with a counter and fresh TSC, then folds the digest back in. */

#include "entropy.h"
#include "cpu.h"
#include "crypto.h"
#include "irqflags.h"
#include "kstring.h"

static inline uint64_t rdtsc(void)
{
  uint32_t a, d;
  __asm__ volatile("rdtsc" : "=a"(a), "=d"(d));
  return ((uint64_t)d << 32) | a;
}

/* 64 bytes of state stirred in place; a torn read only adds uncertainty, so no lock. */
static volatile uint32_t pool[16];
static volatile uint32_t widx;

/* True if feat is a whole space-separated token in a cpu_features() list. */
static bool feature_listed(const char *list, const char *feat)
{
  uint32_t fl = strlen(feat);
  for (const char *p = list; *p;) {
    const char *tok = p;
    while (*p && *p != ' ')
      p++;
    if ((uint32_t)(p - tok) == fl) {
      uint32_t i = 0;
      while (i < fl && tok[i] == feat[i])
        i++;
      if (i == fl)
        return true;
    }
    while (*p == ' ')
      p++;
  }
  return false;
}

/* RDRAND with bounded retry (CF=0 means not ready); 0 if it never succeeds. */
static bool rdrand32(uint32_t *out)
{
  for (int i = 0; i < 16; i++) {
    uint8_t ok;
    uint32_t v;
    __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
    if (ok) {
      *out = v;
      return true;
    }
  }
  return false;
}

void entropy_init(void)
{
  /* Only touch RDRAND if CPUID reports it (the opcode #UDs on older CPUs); RDSEED is not decoded. */
  char feats[256];
  cpu_features(feats, sizeof feats);
  int have_rdrand = feature_listed(feats, "rdrand");

  /* Mix samples so the pool is non-zero before the first tick: RDRAND if present, else TSC reads. */
  for (int i = 0; i < 64; i++) {
    uint32_t s = (uint32_t)rdtsc();
    if (have_rdrand) {
      uint32_t hw;
      if (rdrand32(&hw))
        s ^= hw;
    }
    entropy_stir(s);
  }
}

void entropy_stir(uint32_t sample)
{
  uint64_t t = rdtsc();
  uint32_t i = widx & 15;
  uint32_t v = pool[i] ^ sample ^ (uint32_t)t ^ (uint32_t)(t >> 32);
  v *= 2654435761u; /* Knuth multiplicative mix */
  pool[i] = (v << 13) | (v >> 19);
  widx++;
}

void entropy_get(void *out, uint32_t n)
{
  static uint32_t ctr;
  uint8_t *p = out;
  while (n) {
    struct sha256 s;
    uint8_t d[SHA256_DIGEST];

    uint32_t f = irq_save(); /* stable pool snapshot + ratchet */
    uint32_t c = ++ctr;
    uint64_t t = rdtsc();
    sha256_init(&s);
    sha256_update(&s, (const void *)pool, sizeof pool);
    sha256_update(&s, &c, sizeof c);
    sha256_update(&s, &t, sizeof t);
    sha256_final(&s, d);
    for (int i = 0; i < 8; i++)
      pool[i] ^= ((const uint32_t *)d)[i];
    irq_restore(f);

    uint32_t k = n < SHA256_DIGEST ? n : SHA256_DIGEST;
    memcpy(p, d, k);
    p += k;
    n -= k;
  }
}
