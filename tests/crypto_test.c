/* crypto_test.c — host KAT for the crypto.c paths touched by the audit fixes:
 * AES-128-GCM (constant-time gcm_mul rewrite) and HKDF-SHA256 / TLS1.2 PRF
 * (streamed-HMAC rewrite). Build:
 *   cc -std=c11 -Itests -Iuser/lib/crypto tests/crypto_test.c \
 *      user/lib/crypto/crypto.c -o /tmp/crypto_test && /tmp/crypto_test
 */
#include <stdio.h>
#define OWOS_SUITE_TEST
#include "crypto.h"
#include <string.h>

static int hex(const char *s, uint8_t *out, int max)
{
  int n = 0;
  for (; s[0] && s[1] && n < max; s += 2, n++) {
    int hi = s[0] <= '9' ? s[0] - '0' : (s[0] | 32) - 'a' + 10;
    int lo = s[1] <= '9' ? s[1] - '0' : (s[1] | 32) - 'a' + 10;
    out[n] = (uint8_t)((hi << 4) | lo);
  }
  return n;
}

/* --- AES-128-GCM, NIST GCM spec Test Case 3 (64-byte PT, no AAD) --- */
static void check_gcm(void)
{
  uint8_t key[16], iv[12], pt[64], ct[64], tag[16], out[64], gtag[16];
  hex("feffe9928665731c6d6a8f9467308308", key, 16);
  hex("cafebabefacedbaddecaf888", iv, 12);
  hex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657"
      "ba637b391aafd255",
      pt, 64);
  hex("42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac97"
      "3d58e091473f5985",
      ct, 64);
  hex("4d5c2af327cd64a62cf35abd2ba6fab4", tag, 16);

  aes128_gcm_seal(key, iv, 0, 0, pt, 64, out, gtag);
  expect(memcmp(out, ct, sizeof ct) == 0);
  expect(memcmp(gtag, tag, sizeof tag) == 0);

  int rc = aes128_gcm_open(key, iv, 0, 0, ct, 64, out, tag);
  expect(rc == 0);
  expect(memcmp(out, pt, sizeof pt) == 0);

  uint8_t badtag[16];
  memcpy(badtag, tag, 16);
  badtag[0] ^= 1;
  rc = aes128_gcm_open(key, iv, 0, 0, ct, 64, out, badtag);
  expect(rc != 0);
}

/* --- HKDF-SHA256, RFC 5869 Test Case 1 --- */
static void check_hkdf(void)
{
  uint8_t ikm[22], salt[13], info[10], prk[32], okm[42];
  uint8_t prk_want[32], okm_want[42];
  hex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", ikm, 22);
  hex("000102030405060708090a0b0c", salt, 13);
  hex("f0f1f2f3f4f5f6f7f8f9", info, 10);
  hex("077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5", prk_want, 32);
  hex("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865", okm_want, 42);

  hkdf_extract(salt, 13, ikm, 22, prk);
  expect(memcmp(prk, prk_want, sizeof prk) == 0);
  hkdf_expand(prk, info, 10, okm, 42);
  expect(memcmp(okm, okm_want, sizeof okm) == 0);
}

/* --- TLS 1.2 PRF equivalence: the streamed P_SHA256 rewrite must match the
 * original explicit-concatenation algorithm, computed here via the untouched
 * hmac_sha256 primitive. Validates the rewrite over a length that spills past
 * one HMAC block and needs an A(2). --- */
static void check_tls12_prf(void)
{
  const char *label = "key expansion";
  uint8_t sec[20], seed2[40], prfout[70], ref[70];
  for (int i = 0; i < 20; i++)
    sec[i] = (uint8_t)(0x30 + i);
  for (int i = 0; i < 40; i++)
    seed2[i] = (uint8_t)(0xA0 ^ i);

  /* reference P_SHA256(sec, label||seed) -> ref[70] */
  int ll = (int)strlen(label);
  uint8_t full[13 + 40];
  memcpy(full, label, ll);
  memcpy(full + ll, seed2, 40);
  int flen = ll + 40;
  uint8_t a[32], buf[32 + 53], blk[32];
  hmac_sha256(sec, 20, full, flen, a);
  int outlen = 70, off = 0;
  while (outlen) {
    memcpy(buf, a, 32);
    memcpy(buf + 32, full, flen);
    hmac_sha256(sec, 20, buf, 32 + flen, blk);
    int take = outlen < 32 ? outlen : 32;
    memcpy(ref + off, blk, take);
    off += take;
    outlen -= take;
    hmac_sha256(sec, 20, a, 32, a);
  }
  tls12_prf(sec, 20, label, seed2, 40, prfout, 70);
  expect(memcmp(prfout, ref, sizeof prfout) == 0);
}

/* --- HMAC-SHA1, RFC 2202 Test Case 2 ("Jefe" / "what do ya want ...") --- */
static void check_hmac_sha1(void)
{
  uint8_t mac[20], want[20];
  hex("effcdf6ae5eb2fa2d27416d5f184df9c259a7c79", want, 20);
  hmac_sha1((const uint8_t *)"Jefe", 4, (const uint8_t *)"what do ya want for nothing?", 28, mac);
  expect(memcmp(mac, want, sizeof mac) == 0);
}

/* --- AES-128-CTR, NIST SP 800-38A F.5.1/F.5.2 (CTR-AES128) --- */
static void check_aes_ctr(void)
{
  uint8_t ckey[16], ictr[16], cpt[64], cct[64], cout[64];
  hex("2b7e151628aed2a6abf7158809cf4f3c", ckey, 16);
  hex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", ictr, 16);
  hex("6bc1bee22e409f96e93d7e117393172a"
      "ae2d8a571e03ac9c9eb76fac45af8e51"
      "30c81c46a35ce411e5fbc1191a0a52ef"
      "f69f2445df4f9b17ad2b417be66c3710",
      cpt, 64);
  hex("874d6191b620e3261bef6864990db6ce"
      "9806f66b7970fdff8617187bb9fffdff"
      "5ae4df3edbd5d35e5b4f09020db03eab"
      "1e031dda2fbe03d1792170a0f3009cee",
      cct, 64);
  aes128_ctr(ckey, ictr, cpt, 64, cout);
  expect(memcmp(cout, cct, sizeof cout) == 0);
  aes128_ctr(ckey, ictr, cct, 64, cout); /* decrypt = same op */
  expect(memcmp(cout, cpt, sizeof cout) == 0);
  /* non-block-multiple length exercises the tail path */
  aes128_ctr(ckey, ictr, cpt, 20, cout);
  expect(memcmp(cout, cct, 20) == 0);
}

/* --- HPKE, RFC 9180 Appendix A.1 (X25519/HKDF-SHA256/AES-128-GCM, base mode) --- */
static void check_hpke(void)
{
  uint8_t skE[32], pkR[32], info[32], enc[HPKE_ENC_LEN], pkE[32], key[16], nonce[12], pt[64], aad[16], ct[80], want[80];
  hex("52c4a758a802cd8b936eceea314432798d5baf2d7e9235dc084ab1b9cfa2f736", skE, 32);
  hex("3948cfe0ad1ddb695d780e59077195da6c56506b027329794ab02bca80815c4d", pkR, 32);
  hex("37fda3567bdbd628e88668c3c8d7e97d1d1253b6d4ea6d44c150f741f1bf4431", pkE, 32);
  hex("4f6465206f6e2061204772656369616e2055726e", info, 32);
  hex("4531685d41d65f03dc48f6b8302c05b0", key, 16);
  hex("56d890e5accaaf011cff4b7d", nonce, 12);
  int ptlen = hex("4265617574792069732074727574682c20747275746820626561757479", pt, 64);

  struct hpke_ctx ctx;
  expect(hpke_setup_base_s(&ctx, enc, pkR, info, 20, skE) == 0);
  expect(memcmp(enc, pkE, 32) == 0);
  expect(memcmp(ctx.key, key, 16) == 0);
  expect(memcmp(ctx.base_nonce, nonce, 12) == 0);

  int al = hex("436f756e742d30", aad, 16);
  hex("f938558b5d72f1a23810b4be2ab4f84331acc02fc97babc53a52ae8218a355a96d8770ac83d07bea87e13c512a", want, 80);
  hpke_seal(&ctx, aad, (uint32_t)al, pt, (uint32_t)ptlen, ct);
  expect(memcmp(ct, want, ptlen + HPKE_TAG_LEN) == 0);

  al = hex("436f756e742d31", aad, 16);
  hex("af2d7e9ac9ae7e270f46ba1f975be53c09f8d875bdc8535458c2494e8a6eab251c03d0c22a56b8ca42c2063b84", want, 80);
  hpke_seal(&ctx, aad, (uint32_t)al, pt, (uint32_t)ptlen, ct);
  expect(memcmp(ct, want, ptlen + HPKE_TAG_LEN) == 0);

  uint8_t low[32] = {0};
  expect(hpke_setup_base_s(&ctx, enc, low, info, 20, skE) != 0);
}

int main(void)
{
  test("AES-128-GCM seal and open", check_gcm);
  test("HKDF-SHA256", check_hkdf);
  test("TLS 1.2 PRF", check_tls12_prf);
  test("HMAC-SHA1", check_hmac_sha1);
  test("AES-128-CTR", check_aes_ctr);
  test("HPKE base mode", check_hpke);
  return 0;
}
