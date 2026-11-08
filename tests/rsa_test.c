/* rsa_test.c — RSA PKCS#1 v1.5 and PSS verification against OpenSSL-made signatures, plus SHA-384. */
#define OWOS_SUITE_TEST
#include "test.h"
#include <string.h>

#include "../user/lib/pki/bn.c"
#include "../user/lib/pki/rsa.c"
#include "rsa_vectors.h"

static uint32_t unhex(const char *s, uint8_t *out)
{
  uint32_t n = 0;
  for (; s[0] && s[1]; s += 2, n++) {
    int hi = s[0] <= '9' ? s[0] - '0' : s[0] - 'a' + 10;
    int lo = s[1] <= '9' ? s[1] - '0' : s[1] - 'a' + 10;
    out[n] = (uint8_t)(hi << 4 | lo);
  }
  return n;
}

static int hash_of(const char *name) { return name[3] == '2' ? PK_SHA256 : name[3] == '3' ? PK_SHA384 : PK_SHA512; }

static const char *digest_hex(int alg)
{
  return alg == PK_SHA256 ? DIGEST_SHA256 : alg == PK_SHA384 ? DIGEST_SHA384 : DIGEST_SHA512;
}

static int verify(const struct rsa_vec *v, const uint8_t *digest, const uint8_t *sig, uint32_t siglen,
                  const char *scheme)
{
  static const uint8_t e[] = {1, 0, 1};
  uint8_t n[512];
  uint32_t nlen = unhex(v->n, n);
  int alg = hash_of(v->hash);
  return strcmp(scheme, "pss") ? rsa_verify_pkcs1(n, nlen, e, 3, alg, digest, sig, siglen)
                               : rsa_verify_pss(n, nlen, e, 3, alg, digest, sig, siglen);
}

static void verifies_openssl_signatures(void)
{
  for (unsigned i = 0; i < sizeof RSA_VECS / sizeof *RSA_VECS; i++) {
    const struct rsa_vec *v = &RSA_VECS[i];
    uint8_t digest[64], sig[512];
    unhex(digest_hex(hash_of(v->hash)), digest);
    uint32_t siglen = unhex(v->sig, sig);
    expect(verify(v, digest, sig, siglen, v->scheme) == 0);
  }
}

static void rejects_tampering(void)
{
  for (unsigned i = 0; i < sizeof RSA_VECS / sizeof *RSA_VECS; i++) {
    const struct rsa_vec *v = &RSA_VECS[i];
    uint8_t digest[64], sig[512];
    unhex(digest_hex(hash_of(v->hash)), digest);
    uint32_t siglen = unhex(v->sig, sig);

    sig[siglen / 2] ^= 1;
    expect(verify(v, digest, sig, siglen, v->scheme) != 0);
    sig[siglen / 2] ^= 1;

    digest[3] ^= 1;
    expect(verify(v, digest, sig, siglen, v->scheme) != 0);
    digest[3] ^= 1;

    expect(verify(v, digest, sig, siglen - 1, v->scheme) != 0);
    expect(verify(v, digest, sig, siglen, strcmp(v->scheme, "pss") ? "pss" : "pkcs1") != 0);
  }
}

static void sha384_known_answer(void)
{
  uint8_t out[48], want[48];
  sha384("abc", 3, out);
  unhex("cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7", want);
  expect(memcmp(out, want, 48) == 0);
}

int main(void)
{
  test("verifies OpenSSL RSA signatures", verifies_openssl_signatures);
  test("rejects tampered signatures and digests", rejects_tampering);
  test("SHA-384", sha384_known_answer);
  return 0;
}
