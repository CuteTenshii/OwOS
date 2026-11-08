/* rsa.c — RSA signature verification (PKCS#1 v1.5 and PSS, RFC 8017). */

#include "bn.h"
#include "pk.h"
#include <string.h>

/* m = sig^e mod n as big-endian bytes in em[0..nbytes). */
static int rsa_public(const uint8_t *n, uint32_t nlen, const uint8_t *e, uint32_t elen, const uint8_t *sig,
                      uint32_t siglen, uint8_t *em, uint32_t *emlen)
{
  struct bn_mont m;
  uint32_t x[BN_MAX_LIMBS], ex[BN_MAX_LIMBS] = {0};
  if (bn_mont_init(&m, n, nlen) || siglen > m.len * 4 || elen == 0 || elen > 4 || m.len < 2)
    return -1;
  uint32_t nbytes = m.len * 4;
  while (nbytes && !((m.n[(nbytes - 1) / 4] >> (8 * ((nbytes - 1) % 4))) & 0xff))
    nbytes--;
  if (siglen != nbytes) /* signature must be exactly as long as the modulus */
    return -1;
  if (bn_from_be(&m, x, sig, siglen) || bn_cmp(&m, x, m.n) >= 0)
    return -1;
  for (uint32_t i = 0; i < elen; i++)
    ex[0] = ex[0] << 8 | e[i];
  if (ex[0] < 3 || !(ex[0] & 1))
    return -1;
  bn_to_mont(&m, x, x);
  bn_pow(&m, x, x, ex, 1);
  bn_from_mont(&m, x, x);
  *emlen = nbytes;
  return bn_to_be(&m, em, nbytes, x);
}

static const uint8_t DER_SHA256[] = {0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
                                     0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20};
static const uint8_t DER_SHA384[] = {0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
                                     0x65, 0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30};
static const uint8_t DER_SHA512[] = {0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
                                     0x65, 0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40};

int rsa_verify_pkcs1(const uint8_t *n, uint32_t nlen, const uint8_t *e, uint32_t elen, int alg, const uint8_t *digest,
                     const uint8_t *sig, uint32_t siglen)
{
  uint8_t em[BN_MAX_LIMBS * 4];
  uint32_t emlen;
  if (rsa_public(n, nlen, e, elen, sig, siglen, em, &emlen))
    return -1;
  const uint8_t *prefix = alg == PK_SHA256 ? DER_SHA256 : alg == PK_SHA384 ? DER_SHA384 : DER_SHA512;
  uint32_t plen = 19, dlen = pk_hash_len(alg), tlen = plen + dlen;
  if (emlen < tlen + 11)
    return -1;
  /* 00 01 FF..FF 00 DigestInfo, rebuilt and compared whole */
  uint32_t pad = emlen - tlen - 3;
  uint8_t diff = em[0] | (em[1] ^ 0x01) | em[2 + pad];
  for (uint32_t i = 0; i < pad; i++)
    diff |= em[2 + i] ^ 0xff;
  for (uint32_t i = 0; i < plen; i++)
    diff |= em[3 + pad + i] ^ prefix[i];
  for (uint32_t i = 0; i < dlen; i++)
    diff |= em[3 + pad + plen + i] ^ digest[i];
  return diff ? -1 : 0;
}

static void mgf1(int alg, const uint8_t *seed, uint32_t seedlen, uint8_t *out, uint32_t outlen)
{
  uint8_t buf[PK_HASH_MAX + 4], h[PK_HASH_MAX];
  uint32_t hl = pk_hash_len(alg);
  memcpy(buf, seed, seedlen);
  for (uint32_t counter = 0, done = 0; done < outlen; counter++) {
    buf[seedlen] = (uint8_t)(counter >> 24);
    buf[seedlen + 1] = (uint8_t)(counter >> 16);
    buf[seedlen + 2] = (uint8_t)(counter >> 8);
    buf[seedlen + 3] = (uint8_t)counter;
    pk_hash(alg, buf, seedlen + 4, h);
    for (uint32_t i = 0; i < hl && done < outlen; i++)
      out[done++] = h[i];
  }
}

int rsa_verify_pss(const uint8_t *n, uint32_t nlen, const uint8_t *e, uint32_t elen, int alg, const uint8_t *digest,
                   const uint8_t *sig, uint32_t siglen)
{
  uint8_t em[BN_MAX_LIMBS * 4];
  uint32_t emlen;
  if (rsa_public(n, nlen, e, elen, sig, siglen, em, &emlen))
    return -1;

  /* modBits - 1 bits of encoded message; with a modulus of 8k bits that drops the top byte. */
  while (nlen && !*n) {
    n++;
    nlen--;
  }
  uint32_t topbits = 0;
  for (uint8_t t = n[0]; t; t >>= 1)
    topbits++;
  const uint8_t *p = em;
  uint32_t len = emlen;
  if (topbits == 1) { /* modBits - 1 is a whole number of bytes */
    if (em[0])
      return -1;
    p++;
    len--;
  }
  uint8_t topmask = topbits == 1 ? 0xff : (uint8_t)(0xff >> (8 - (topbits - 1)));

  uint32_t hl = pk_hash_len(alg);
  if (len < hl + hl + 2 || p[len - 1] != 0xbc || (p[0] & ~topmask))
    return -1;
  uint32_t dblen = len - hl - 1;
  const uint8_t *h = p + dblen;
  uint8_t db[BN_MAX_LIMBS * 4];
  mgf1(alg, h, hl, db, dblen);
  for (uint32_t i = 0; i < dblen; i++)
    db[i] ^= p[i];
  db[0] &= topmask;

  uint32_t zeros = dblen - hl - 1;
  for (uint32_t i = 0; i < zeros; i++)
    if (db[i])
      return -1;
  if (db[zeros] != 0x01)
    return -1;
  const uint8_t *salt = db + zeros + 1;

  uint8_t mp[8 + PK_HASH_MAX * 2] = {0}, hh[PK_HASH_MAX];
  memcpy(mp + 8, digest, hl);
  memcpy(mp + 8 + hl, salt, hl);
  pk_hash(alg, mp, 8 + 2 * hl, hh);
  return memcmp(hh, h, hl) ? -1 : 0;
}
