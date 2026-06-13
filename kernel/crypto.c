/* crypto.c — primitives for the TLS 1.2 client (see crypto.h).
 *
 * Everything here is sized for correctness and small footprint, not speed:
 * GHASH multiplies bit-by-bit, AES is byte-oriented, and X25519 is the
 * TweetNaCl field arithmetic (public domain). The TLS handshake runs a handful
 * of these per connection, so the slowness is invisible next to the network.
 */

#include "crypto.h"
#include "console.h"
#include "kstring.h"

/* ------------------------------------------------------------------ helpers */

static inline uint32_t rotr32(uint32_t x, int n)
{
	return (x >> n) | (x << (32 - n));
}

static inline uint32_t rotl32(uint32_t x, int n)
{
	return (x << n) | (x >> (32 - n));
}

/* ================================================================= SHA-256 */

static const uint32_t sha256_k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
	0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
	0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
	0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
	0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
	0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

void sha256_init(struct sha256 *s)
{
	s->h[0] = 0x6a09e667; s->h[1] = 0xbb67ae85;
	s->h[2] = 0x3c6ef372; s->h[3] = 0xa54ff53a;
	s->h[4] = 0x510e527f; s->h[5] = 0x9b05688c;
	s->h[6] = 0x1f83d9ab; s->h[7] = 0x5be0cd19;
	s->total = 0;
	s->buflen = 0;
}

static void sha256_block(struct sha256 *s, const uint8_t *p)
{
	uint32_t w[64];
	for (int i = 0; i < 16; i++)
		w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
		       ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
	uint32_t e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
	for (int i = 0; i < 64; i++) {
		uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
		uint32_t ch = (e & f) ^ (~e & g);
		uint32_t t1 = h + S1 + ch + sha256_k[i] + w[i];
		uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
		uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
		uint32_t t2 = S0 + maj;
		h = g; g = f; f = e; e = d + t1;
		d = c; c = b; b = a; a = t1 + t2;
	}
	s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
	s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

void sha256_update(struct sha256 *s, const void *data, uint32_t len)
{
	const uint8_t *p = data;
	s->total += len;
	while (len) {
		uint32_t take = SHA256_BLOCK - s->buflen;
		if (take > len)
			take = len;
		memcpy(s->buf + s->buflen, p, take);
		s->buflen += take;
		p += take;
		len -= take;
		if (s->buflen == SHA256_BLOCK) {
			sha256_block(s, s->buf);
			s->buflen = 0;
		}
	}
}

void sha256_final(struct sha256 *s, uint8_t out[SHA256_DIGEST])
{
	uint64_t bits = s->total * 8;
	uint8_t pad = 0x80;
	sha256_update(s, &pad, 1);
	uint8_t zero = 0;
	while (s->buflen != 56)
		sha256_update(s, &zero, 1);
	uint8_t len[8];
	for (int i = 0; i < 8; i++)
		len[i] = (uint8_t)(bits >> (56 - i * 8));
	sha256_update(s, len, 8);
	for (int i = 0; i < 8; i++) {
		out[i * 4] = (uint8_t)(s->h[i] >> 24);
		out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16);
		out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8);
		out[i * 4 + 3] = (uint8_t)s->h[i];
	}
}

void sha256(const void *data, uint32_t len, uint8_t out[SHA256_DIGEST])
{
	struct sha256 s;
	sha256_init(&s);
	sha256_update(&s, data, len);
	sha256_final(&s, out);
}

/* =================================================================== SHA-1 */

void sha1_init(struct sha1 *s)
{
	s->h[0] = 0x67452301; s->h[1] = 0xefcdab89;
	s->h[2] = 0x98badcfe; s->h[3] = 0x10325476;
	s->h[4] = 0xc3d2e1f0;
	s->total = 0;
	s->buflen = 0;
}

static void sha1_block(struct sha1 *s, const uint8_t *p)
{
	uint32_t w[80];
	for (int i = 0; i < 16; i++)
		w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
		       ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
	for (int i = 16; i < 80; i++)
		w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
	for (int i = 0; i < 80; i++) {
		uint32_t f, k;
		if (i < 20)      { f = (b & c) | (~b & d);       k = 0x5a827999; }
		else if (i < 40) { f = b ^ c ^ d;                k = 0x6ed9eba1; }
		else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
		else             { f = b ^ c ^ d;                k = 0xca62c1d6; }
		uint32_t t = rotl32(a, 5) + f + e + k + w[i];
		e = d; d = c; c = rotl32(b, 30); b = a; a = t;
	}
	s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

void sha1_update(struct sha1 *s, const void *data, uint32_t len)
{
	const uint8_t *p = data;
	s->total += len;
	while (len) {
		uint32_t take = SHA1_BLOCK - s->buflen;
		if (take > len)
			take = len;
		memcpy(s->buf + s->buflen, p, take);
		s->buflen += take;
		p += take;
		len -= take;
		if (s->buflen == SHA1_BLOCK) {
			sha1_block(s, s->buf);
			s->buflen = 0;
		}
	}
}

void sha1_final(struct sha1 *s, uint8_t out[SHA1_DIGEST])
{
	uint64_t bits = s->total * 8;
	uint8_t pad = 0x80;
	sha1_update(s, &pad, 1);
	uint8_t zero = 0;
	while (s->buflen != 56)
		sha1_update(s, &zero, 1);
	uint8_t len[8];
	for (int i = 0; i < 8; i++)
		len[i] = (uint8_t)(bits >> (56 - i * 8));
	sha1_update(s, len, 8);
	for (int i = 0; i < 5; i++) {
		out[i * 4] = (uint8_t)(s->h[i] >> 24);
		out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16);
		out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8);
		out[i * 4 + 3] = (uint8_t)s->h[i];
	}
}

void sha1(const void *data, uint32_t len, uint8_t out[SHA1_DIGEST])
{
	struct sha1 s;
	sha1_init(&s);
	sha1_update(&s, data, len);
	sha1_final(&s, out);
}

/* ============================================================ HMAC + PRF */

void hmac_sha256(const uint8_t *key, uint32_t keylen,
                 const uint8_t *msg, uint32_t msglen,
                 uint8_t out[SHA256_DIGEST])
{
	uint8_t k[SHA256_BLOCK];
	memset(k, 0, SHA256_BLOCK);
	if (keylen > SHA256_BLOCK)
		sha256(key, keylen, k);
	else
		memcpy(k, key, keylen);

	uint8_t ipad[SHA256_BLOCK], opad[SHA256_BLOCK];
	for (int i = 0; i < SHA256_BLOCK; i++) {
		ipad[i] = k[i] ^ 0x36;
		opad[i] = k[i] ^ 0x5c;
	}

	uint8_t inner[SHA256_DIGEST];
	struct sha256 s;
	sha256_init(&s);
	sha256_update(&s, ipad, SHA256_BLOCK);
	sha256_update(&s, msg, msglen);
	sha256_final(&s, inner);

	sha256_init(&s);
	sha256_update(&s, opad, SHA256_BLOCK);
	sha256_update(&s, inner, SHA256_DIGEST);
	sha256_final(&s, out);
}

/* P_SHA256: A(0)=seed; A(i)=HMAC(secret,A(i-1)); out += HMAC(secret, A(i)||seed).
 * Here `seed` is the already-concatenated label||seed. */
static void p_sha256(const uint8_t *secret, uint32_t secretlen,
                     const uint8_t *seed, uint32_t seedlen,
                     uint8_t *out, uint32_t outlen)
{
	uint8_t a[SHA256_DIGEST];
	/* A(1) = HMAC(secret, seed) */
	hmac_sha256(secret, secretlen, seed, seedlen, a);

	uint8_t buf[SHA256_DIGEST + 512];   /* A(i) || seed; seed is small in TLS */
	while (outlen) {
		memcpy(buf, a, SHA256_DIGEST);
		memcpy(buf + SHA256_DIGEST, seed, seedlen);
		uint8_t block[SHA256_DIGEST];
		hmac_sha256(secret, secretlen, buf, SHA256_DIGEST + seedlen, block);
		uint32_t take = outlen < SHA256_DIGEST ? outlen : SHA256_DIGEST;
		memcpy(out, block, take);
		out += take;
		outlen -= take;
		/* A(i+1) = HMAC(secret, A(i)) */
		hmac_sha256(secret, secretlen, a, SHA256_DIGEST, a);
	}
}

void tls12_prf(const uint8_t *secret, uint32_t secretlen,
               const char *label,
               const uint8_t *seed, uint32_t seedlen,
               uint8_t *out, uint32_t outlen)
{
	uint8_t fullseed[512];
	uint32_t labellen = 0;
	while (label[labellen])
		labellen++;
	memcpy(fullseed, label, labellen);
	memcpy(fullseed + labellen, seed, seedlen);
	p_sha256(secret, secretlen, fullseed, labellen + seedlen, out, outlen);
}

/* =================================================================== HKDF */

void hkdf_extract(const uint8_t *salt, uint32_t saltlen,
                  const uint8_t *ikm, uint32_t ikmlen,
                  uint8_t prk[SHA256_DIGEST])
{
	hmac_sha256(salt, saltlen, ikm, ikmlen, prk);
}

void hkdf_expand(const uint8_t prk[SHA256_DIGEST],
                 const uint8_t *info, uint32_t infolen,
                 uint8_t *out, uint32_t outlen)
{
	uint8_t t[SHA256_DIGEST];
	uint32_t tlen = 0;
	uint8_t counter = 1;
	uint32_t done = 0;
	while (done < outlen) {
		uint8_t buf[SHA256_DIGEST + 512 + 1];
		uint32_t p = 0;
		memcpy(buf, t, tlen);
		p = tlen;
		memcpy(buf + p, info, infolen);
		p += infolen;
		buf[p++] = counter;
		hmac_sha256(prk, SHA256_DIGEST, buf, p, t);
		tlen = SHA256_DIGEST;
		uint32_t take = outlen - done < SHA256_DIGEST ? outlen - done : SHA256_DIGEST;
		memcpy(out + done, t, take);
		done += take;
		counter++;
	}
}

/* ================================================================= AES-128 */

static const uint8_t aes_sbox[256] = {
	0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
	0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
	0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
	0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
	0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
	0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
	0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
	0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
	0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
	0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
	0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
	0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
	0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
	0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
	0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
	0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

static const uint8_t aes_rcon[10] = {
	0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36,
};

void aes128_init(struct aes128 *a, const uint8_t key[16])
{
	uint32_t *rk = a->rk;
	for (int i = 0; i < 4; i++)
		rk[i] = ((uint32_t)key[4 * i] << 24) | ((uint32_t)key[4 * i + 1] << 16) |
		        ((uint32_t)key[4 * i + 2] << 8) | (uint32_t)key[4 * i + 3];
	for (int i = 4; i < 44; i++) {
		uint32_t t = rk[i - 1];
		if (i % 4 == 0) {
			t = (t << 8) | (t >> 24);   /* RotWord */
			t = ((uint32_t)aes_sbox[t >> 24] << 24) |
			    ((uint32_t)aes_sbox[(t >> 16) & 0xff] << 16) |
			    ((uint32_t)aes_sbox[(t >> 8) & 0xff] << 8) |
			    (uint32_t)aes_sbox[t & 0xff];
			t ^= (uint32_t)aes_rcon[i / 4 - 1] << 24;
		}
		rk[i] = rk[i - 4] ^ t;
	}
}

static inline uint8_t xtime(uint8_t x)
{
	return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b));
}

void aes128_encrypt_block(const struct aes128 *a, const uint8_t in[16],
                          uint8_t out[16])
{
	uint8_t s[16];
	for (int i = 0; i < 16; i++)
		s[i] = in[i] ^ (uint8_t)(a->rk[i / 4] >> (24 - 8 * (i % 4)));

	for (int round = 1; round <= 10; round++) {
		/* SubBytes */
		for (int i = 0; i < 16; i++)
			s[i] = aes_sbox[s[i]];
		/* ShiftRows (state is column-major: s[col*4 + row]) */
		uint8_t t[16];
		for (int r = 0; r < 4; r++)
			for (int c = 0; c < 4; c++)
				t[c * 4 + r] = s[((c + r) % 4) * 4 + r];
		memcpy(s, t, 16);
		/* MixColumns (skipped on the final round) */
		if (round != 10) {
			for (int c = 0; c < 4; c++) {
				uint8_t *col = s + c * 4;
				uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
				col[0] = xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3;
				col[1] = a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3;
				col[2] = a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3);
				col[3] = (xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3);
			}
		}
		/* AddRoundKey */
		for (int i = 0; i < 16; i++)
			s[i] ^= (uint8_t)(a->rk[round * 4 + i / 4] >> (24 - 8 * (i % 4)));
	}
	memcpy(out, s, 16);
}

/* ----------------------------------------------------------------- GCM */

/* Right-shift a 128-bit big-endian value by one bit (byte 0 is most sig.). */
static void gcm_rshift1(uint8_t v[16])
{
	for (int i = 15; i > 0; i--)
		v[i] = (uint8_t)((v[i] >> 1) | (v[i - 1] << 7));
	v[0] >>= 1;
}

/* Z = X * Y in GF(2^128) with the GCM reduction polynomial. */
static void gcm_mul(const uint8_t x[16], const uint8_t y[16], uint8_t z[16])
{
	uint8_t v[16];
	memcpy(v, y, 16);
	memset(z, 0, 16);
	for (int i = 0; i < 128; i++) {
		if (x[i / 8] & (0x80 >> (i % 8)))
			for (int j = 0; j < 16; j++)
				z[j] ^= v[j];
		int lsb = v[15] & 1;
		gcm_rshift1(v);
		if (lsb)
			v[0] ^= 0xe1;            /* reduction: R = 11100001 || 0^120 */
	}
}

static void ghash(const uint8_t h[16], const uint8_t *data, uint32_t len,
                  uint8_t y[16])
{
	uint32_t full = len / 16;
	for (uint32_t b = 0; b < full; b++) {
		for (int j = 0; j < 16; j++)
			y[j] ^= data[b * 16 + j];
		uint8_t t[16];
		gcm_mul(y, h, t);
		memcpy(y, t, 16);
	}
	uint32_t rem = len % 16;
	if (rem) {
		for (uint32_t j = 0; j < rem; j++)
			y[j] ^= data[full * 16 + j];
		uint8_t t[16];
		gcm_mul(y, h, t);
		memcpy(y, t, 16);
	}
}

static void gcm_inc32(uint8_t ctr[16])
{
	for (int i = 15; i >= 12; i--)
		if (++ctr[i])
			break;
}

/* Core GCM: produce keystream-XOR ciphertext and the auth tag. */
static void gcm_crypt(const struct aes128 *aes, const uint8_t iv[12],
                      const uint8_t *aad, uint32_t aadlen,
                      const uint8_t *in, uint32_t len, uint8_t *out,
                      uint8_t tag[16])
{
	uint8_t h[16] = {0};
	aes128_encrypt_block(aes, h, h);    /* H = E(K, 0) */

	uint8_t j0[16];
	memcpy(j0, iv, 12);
	j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;

	/* CTR encryption starting from J0+1. */
	uint8_t ctr[16];
	memcpy(ctr, j0, 16);
	for (uint32_t off = 0; off < len; off += 16) {
		gcm_inc32(ctr);
		uint8_t ks[16];
		aes128_encrypt_block(aes, ctr, ks);
		uint32_t n = len - off < 16 ? len - off : 16;
		for (uint32_t j = 0; j < n; j++)
			out[off + j] = in[off + j] ^ ks[j];
	}

	/* GHASH over AAD || C || len(AAD)||len(C) (bit lengths, 64-bit BE). */
	uint8_t y[16] = {0};
	ghash(h, aad, aadlen, y);
	ghash(h, out, len, y);
	uint8_t lenblock[16] = {0};
	uint64_t aadbits = (uint64_t)aadlen * 8;
	uint64_t cbits = (uint64_t)len * 8;
	for (int i = 0; i < 8; i++) {
		lenblock[i] = (uint8_t)(aadbits >> (56 - i * 8));
		lenblock[8 + i] = (uint8_t)(cbits >> (56 - i * 8));
	}
	ghash(h, lenblock, 16, y);

	uint8_t ej0[16];
	aes128_encrypt_block(aes, j0, ej0);
	for (int i = 0; i < 16; i++)
		tag[i] = y[i] ^ ej0[i];
}

void aes128_gcm_seal(const uint8_t key[16], const uint8_t iv[12],
                     const uint8_t *aad, uint32_t aadlen,
                     const uint8_t *in, uint32_t len, uint8_t *out,
                     uint8_t tag[16])
{
	struct aes128 aes;
	aes128_init(&aes, key);
	gcm_crypt(&aes, iv, aad, aadlen, in, len, out, tag);
}

int aes128_gcm_open(const uint8_t key[16], const uint8_t iv[12],
                    const uint8_t *aad, uint32_t aadlen,
                    const uint8_t *in, uint32_t len, uint8_t *out,
                    const uint8_t tag[16])
{
	struct aes128 aes;
	aes128_init(&aes, key);
	/* gcm_crypt GHASHes over the ciphertext, which is `in`; decrypt in place
	 * into out, but tag must be computed over the ciphertext, so compute the
	 * tag from a CTR pass that writes plaintext while GHASHing input. Simplest:
	 * GHASH ciphertext first, then CTR-decrypt. */
	uint8_t h[16] = {0};
	aes128_encrypt_block(&aes, h, h);

	uint8_t j0[16];
	memcpy(j0, iv, 12);
	j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;

	uint8_t y[16] = {0};
	ghash(h, aad, aadlen, y);
	ghash(h, in, len, y);
	uint8_t lenblock[16] = {0};
	uint64_t aadbits = (uint64_t)aadlen * 8, cbits = (uint64_t)len * 8;
	for (int i = 0; i < 8; i++) {
		lenblock[i] = (uint8_t)(aadbits >> (56 - i * 8));
		lenblock[8 + i] = (uint8_t)(cbits >> (56 - i * 8));
	}
	ghash(h, lenblock, 16, y);
	uint8_t ej0[16], want[16];
	aes128_encrypt_block(&aes, j0, ej0);
	for (int i = 0; i < 16; i++)
		want[i] = y[i] ^ ej0[i];

	int diff = 0;
	for (int i = 0; i < 16; i++)
		diff |= want[i] ^ tag[i];
	if (diff)
		return -1;

	uint8_t ctr[16];
	memcpy(ctr, j0, 16);
	for (uint32_t off = 0; off < len; off += 16) {
		gcm_inc32(ctr);
		uint8_t ks[16];
		aes128_encrypt_block(&aes, ctr, ks);
		uint32_t n = len - off < 16 ? len - off : 16;
		for (uint32_t j = 0; j < n; j++)
			out[off + j] = in[off + j] ^ ks[j];
	}
	return 0;
}

/* ================================================================= X25519 */
/* Field arithmetic adapted from TweetNaCl (Bernstein et al., public domain). */

typedef int64_t gf[16];

static const gf gf121665 = {0xDB41, 1};

static void car25519(gf o)
{
	for (int i = 0; i < 16; i++) {
		o[i] += (1LL << 16);
		int64_t c = o[i] >> 16;
		o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
		o[i] -= c << 16;
	}
}

static void sel25519(gf p, gf q, int b)
{
	int64_t c = ~(b - 1);
	for (int i = 0; i < 16; i++) {
		int64_t t = c & (p[i] ^ q[i]);
		p[i] ^= t;
		q[i] ^= t;
	}
}

static void pack25519(uint8_t *o, const gf n)
{
	gf t, m;
	for (int i = 0; i < 16; i++)
		t[i] = n[i];
	car25519(t);
	car25519(t);
	car25519(t);
	for (int j = 0; j < 2; j++) {
		m[0] = t[0] - 0xffed;
		for (int i = 1; i < 15; i++) {
			m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
			m[i - 1] &= 0xffff;
		}
		m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
		int b = (int)((m[15] >> 16) & 1);
		m[14] &= 0xffff;
		sel25519(t, m, 1 - b);
	}
	for (int i = 0; i < 16; i++) {
		o[2 * i] = (uint8_t)(t[i] & 0xff);
		o[2 * i + 1] = (uint8_t)(t[i] >> 8);
	}
}

static void unpack25519(gf o, const uint8_t *n)
{
	for (int i = 0; i < 16; i++)
		o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
	o[15] &= 0x7fff;
}

static void fadd(gf o, const gf a, const gf b)
{
	for (int i = 0; i < 16; i++)
		o[i] = a[i] + b[i];
}

static void fsub(gf o, const gf a, const gf b)
{
	for (int i = 0; i < 16; i++)
		o[i] = a[i] - b[i];
}

static void fmul(gf o, const gf a, const gf b)
{
	int64_t t[31];
	for (int i = 0; i < 31; i++)
		t[i] = 0;
	for (int i = 0; i < 16; i++)
		for (int j = 0; j < 16; j++)
			t[i + j] += a[i] * b[j];
	for (int i = 0; i < 15; i++)
		t[i] += 38 * t[i + 16];
	for (int i = 0; i < 16; i++)
		o[i] = t[i];
	car25519(o);
	car25519(o);
}

static void fsqr(gf o, const gf a)
{
	fmul(o, a, a);
}

static void finv(gf o, const gf i)
{
	gf c;
	for (int a = 0; a < 16; a++)
		c[a] = i[a];
	for (int a = 253; a >= 0; a--) {
		fsqr(c, c);
		if (a != 2 && a != 4)
			fmul(c, c, i);
	}
	for (int a = 0; a < 16; a++)
		o[a] = c[a];
}

void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32])
{
	uint8_t z[32];
	gf x, a, b, c, d, e, f;
	for (int i = 0; i < 31; i++)
		z[i] = scalar[i];
	z[31] = (scalar[31] & 127) | 64;
	z[0] &= 248;
	unpack25519(x, point);
	for (int i = 0; i < 16; i++) {
		b[i] = x[i];
		d[i] = a[i] = c[i] = 0;
	}
	a[0] = d[0] = 1;
	for (int i = 254; i >= 0; i--) {
		int r = (z[i >> 3] >> (i & 7)) & 1;
		sel25519(a, b, r);
		sel25519(c, d, r);
		fadd(e, a, c);
		fsub(a, a, c);
		fadd(c, b, d);
		fsub(b, b, d);
		fsqr(d, e);
		fsqr(f, a);
		fmul(a, c, a);
		fmul(c, b, e);
		fadd(e, a, c);
		fsub(a, a, c);
		fsqr(b, a);
		fsub(c, d, f);
		fmul(a, c, gf121665);
		fadd(a, a, d);
		fmul(c, c, a);
		fmul(a, d, f);
		fmul(d, b, x);
		fsqr(b, e);
		sel25519(a, b, r);
		sel25519(c, d, r);
	}
	finv(c, c);
	fmul(a, a, c);
	pack25519(out, a);
}

/* ============================================================ self-test */

static int eqbytes(const uint8_t *a, const uint8_t *b, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++)
		if (a[i] != b[i])
			return 0;
	return 1;
}

static void report(const char *name, int ok, int *all)
{
	con_write(name);
	con_write(ok ? ": PASS\n" : ": FAIL\n");
	if (!ok)
		*all = 0;
}

int crypto_selftest(void)
{
	int all = 1;

	/* SHA-256("abc") */
	{
		static const uint8_t want[32] = {
			0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,
			0x5d,0xae,0x22,0x23,0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,
			0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad };
		uint8_t got[32];
		sha256("abc", 3, got);
		report("sha256", eqbytes(got, want, 32), &all);
	}

	/* SHA-1("abc") */
	{
		static const uint8_t want[20] = {
			0xa9,0x99,0x3e,0x36,0x47,0x06,0x81,0x6a,0xba,0x3e,
			0x25,0x71,0x78,0x50,0xc2,0x6c,0x9c,0xd0,0xd8,0x9d };
		uint8_t got[20];
		sha1("abc", 3, got);
		report("sha1", eqbytes(got, want, 20), &all);
	}

	/* HMAC-SHA256, RFC 4231 test case 1 */
	{
		uint8_t key[20];
		for (int i = 0; i < 20; i++)
			key[i] = 0x0b;
		static const uint8_t want[32] = {
			0xb0,0x34,0x4c,0x61,0xd8,0xdb,0x38,0x53,0x5c,0xa8,0xaf,0xce,
			0xaf,0x0b,0xf1,0x2b,0x88,0x1d,0xc2,0x00,0xc9,0x83,0x3d,0xa7,
			0x26,0xe9,0x37,0x6c,0x2e,0x32,0xcf,0xf7 };
		uint8_t got[32];
		hmac_sha256(key, 20, (const uint8_t *)"Hi There", 8, got);
		report("hmac-sha256", eqbytes(got, want, 32), &all);
	}

	/* AES-128-GCM, NIST test case 3 (no AAD) */
	{
		static const uint8_t key[16] = {
			0xfe,0xff,0xe9,0x92,0x86,0x65,0x73,0x1c,
			0x6d,0x6a,0x8f,0x94,0x67,0x30,0x83,0x08 };
		static const uint8_t iv[12] = {
			0xca,0xfe,0xba,0xbe,0xfa,0xce,0xdb,0xad,0xde,0xca,0xf8,0x88 };
		static const uint8_t pt[64] = {
			0xd9,0x31,0x32,0x25,0xf8,0x84,0x06,0xe5,0xa5,0x59,0x09,0xc5,
			0xaf,0xf5,0x26,0x9a,0x86,0xa7,0xa9,0x53,0x15,0x34,0xf7,0xda,
			0x2e,0x4c,0x30,0x3d,0x8a,0x31,0x8a,0x72,0x1c,0x3c,0x0c,0x95,
			0x95,0x68,0x09,0x53,0x2f,0xcf,0x0e,0x24,0x49,0xa6,0xb5,0x25,
			0xb1,0x6a,0xed,0xf5,0xaa,0x0d,0xe6,0x57,0xba,0x63,0x7b,0x39,
			0x1a,0xaf,0xd2,0x55 };
		static const uint8_t ctw[64] = {
			0x42,0x83,0x1e,0xc2,0x21,0x77,0x74,0x24,0x4b,0x72,0x21,0xb7,
			0x84,0xd0,0xd4,0x9c,0xe3,0xaa,0x21,0x2f,0x2c,0x02,0xa4,0xe0,
			0x35,0xc1,0x7e,0x23,0x29,0xac,0xa1,0x2e,0x21,0xd5,0x14,0xb2,
			0x54,0x66,0x93,0x1c,0x7d,0x8f,0x6a,0x5a,0xac,0x84,0xaa,0x05,
			0x1b,0xa3,0x0b,0x39,0x6a,0x0a,0xac,0x97,0x3d,0x58,0xe0,0x91,
			0x47,0x3f,0x59,0x85 };
		static const uint8_t tagw[16] = {
			0x4d,0x5c,0x2a,0xf3,0x27,0xcd,0x64,0xa6,
			0x2c,0xf3,0x5a,0xbd,0x2b,0xa6,0xfa,0xb4 };
		/* FIPS-197 AES-128 ECB known answer, to isolate block vs GCM. */
		{
			static const uint8_t ek[16] = {
				0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
				0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f };
			static const uint8_t ep[16] = {
				0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
				0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff };
			static const uint8_t ec[16] = {
				0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,
				0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a };
			struct aes128 a;
			uint8_t eo[16];
			aes128_init(&a, ek);
			aes128_encrypt_block(&a, ep, eo);
			report("aes128-ecb", eqbytes(eo, ec, 16), &all);
		}
		uint8_t ct[64], tag[16];
		aes128_gcm_seal(key, iv, 0, 0, pt, 64, ct, tag);
		report("aes-gcm-ct", eqbytes(ct, ctw, 64), &all);
		report("aes-gcm-tag", eqbytes(tag, tagw, 16), &all);
		int ok = eqbytes(ct, ctw, 64) && eqbytes(tag, tagw, 16);
		uint8_t back[64];
		ok = ok && aes128_gcm_open(key, iv, 0, 0, ctw, 64, back, tagw) == 0
		        && eqbytes(back, pt, 64);
		report("aes128-gcm", ok, &all);
	}

	/* X25519, RFC 7748 §5.2 first vector */
	{
		static const uint8_t scalar[32] = {
			0xa5,0x46,0xe3,0x6b,0xf0,0x52,0x7c,0x9d,0x3b,0x16,0x15,0x4b,
			0x82,0x46,0x5e,0xdd,0x62,0x14,0x4c,0x0a,0xc1,0xfc,0x5a,0x18,
			0x50,0x6a,0x22,0x44,0xba,0x44,0x9a,0xc4 };
		static const uint8_t u[32] = {
			0xe6,0xdb,0x68,0x67,0x58,0x30,0x30,0xdb,0x35,0x94,0xc1,0xa4,
			0x24,0xb1,0x5f,0x7c,0x72,0x66,0x24,0xec,0x26,0xb3,0x35,0x3b,
			0x10,0xa9,0x03,0xa6,0xd0,0xab,0x1c,0x4c };
		static const uint8_t want[32] = {
			0xc3,0xda,0x55,0x37,0x9d,0xe9,0xc6,0x90,0x8e,0x94,0xea,0x4d,
			0xf2,0x8d,0x08,0x4f,0x32,0xec,0xcf,0x03,0x49,0x1c,0x71,0xf7,
			0x54,0xb4,0x07,0x55,0x77,0xa2,0x85,0x52 };
		uint8_t got[32];
		x25519(got, scalar, u);
		report("x25519", eqbytes(got, want, 32), &all);
	}

	/* HKDF-SHA256, RFC 5869 Test Case 1 */
	{
		uint8_t ikm[22], salt[13], info[10];
		for (int i = 0; i < 22; i++) ikm[i] = 0x0b;
		for (int i = 0; i < 13; i++) salt[i] = (uint8_t)i;
		for (int i = 0; i < 10; i++) info[i] = (uint8_t)(0xf0 + i);
		static const uint8_t prkw[32] = {
			0x07,0x77,0x09,0x36,0x2c,0x2e,0x32,0xdf,0x0d,0xdc,0x3f,0x0d,
			0xc4,0x7b,0xba,0x63,0x90,0xb6,0xc7,0x3b,0xb5,0x0f,0x9c,0x31,
			0x22,0xec,0x84,0x4a,0xd7,0xc2,0xb3,0xe5 };
		static const uint8_t okmw[42] = {
			0x3c,0xb2,0x5f,0x25,0xfa,0xac,0xd5,0x7a,0x90,0x43,0x4f,0x64,
			0xd0,0x36,0x2f,0x2a,0x2d,0x2d,0x0a,0x90,0xcf,0x1a,0x5a,0x4c,
			0x5d,0xb0,0x2d,0x56,0xec,0xc4,0xc5,0xbf,0x34,0x00,0x72,0x08,
			0xd5,0xb8,0x87,0x18,0x58,0x65 };
		uint8_t prk[32], okm[42];
		hkdf_extract(salt, 13, ikm, 22, prk);
		hkdf_expand(prk, info, 10, okm, 42);
		report("hkdf-sha256", eqbytes(prk, prkw, 32) && eqbytes(okm, okmw, 42), &all);
	}

	con_write(all ? "crypto: ALL PASS\n" : "crypto: FAILURES\n");
	return all ? 0 : -1;
}
