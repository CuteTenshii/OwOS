/* entropy.c: see entropy.h.
 * The pool is stirred from interrupts with the cycle counter (low bits carry interrupt-latency jitter: the TSC on x86,
 * the virtual counter on AArch64); extraction hashes the pool with a counter and a fresh count, then folds the digest
 * back in. A hardware RNG (RDRAND, RNDR) seeds it at boot when the CPU has one. */

#include "entropy.h"
#include "cpu.h"
#include "crypto.h"
#include "irqflags.h"
#include "kstring.h"

#if defined(__aarch64__)
static inline uint64_t cycles(void)
{
  uint64_t v;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
  return v;
}
#define HWRNG_FEATURE "rng"
#else
static inline uint64_t cycles(void)
{
  uint32_t a, d;
  __asm__ volatile("rdtsc" : "=a"(a), "=d"(d));
  return ((uint64_t)d << 32) | a;
}
#define HWRNG_FEATURE "rdrand"
#endif

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

/* The hardware RNG with bounded retry; false if it never delivers. RDRAND clears CF and RNDR sets Z when no
 * number is ready. */
static bool hwrng32(uint32_t *out)
{
  for (int i = 0; i < 16; i++) {
#if defined(__aarch64__)
    uint64_t v, ok;
    __asm__ volatile("mrs %0, s3_3_c2_c4_0\n\tcset %1, ne" : "=r"(v), "=r"(ok)::"cc"); /* RNDR */
#else
    uint8_t ok;
    uint32_t v;
    __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
#endif
    if (ok) {
      *out = (uint32_t)v;
      return true;
    }
  }
  return false;
}

void entropy_init(void)
{
  /* Only touch the hardware RNG if the CPU reports one (the instruction is undefined without it). */
  char feats[256];
  cpu_features(feats, sizeof feats);
  int have_hwrng = feature_listed(feats, HWRNG_FEATURE);

  /* Mix samples so the pool is non-zero before the first tick: the hardware RNG if present, else counter reads. */
  for (int i = 0; i < 64; i++) {
    uint32_t s = (uint32_t)cycles();
    if (have_hwrng) {
      uint32_t hw;
      if (hwrng32(&hw))
        s ^= hw;
    }
    entropy_stir(s);
  }
}

void entropy_stir(uint32_t sample)
{
  uint64_t t = cycles();
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

    uintptr_t f = irq_save(); /* stable pool snapshot + ratchet */
    uint32_t c = ++ctr;
    uint64_t t = cycles();
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
