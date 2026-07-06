/* crypto_test.c — host KAT for the crypto.c paths touched by the audit fixes:
 * AES-128-GCM (constant-time gcm_mul rewrite) and HKDF-SHA256 / TLS1.2 PRF
 * (streamed-HMAC rewrite). Build:
 *   cc -std=c11 -Iscripts -Iuser/lib/crypto scripts/crypto_test.c \
 *      user/lib/crypto/crypto.c -o /tmp/crypto_test && /tmp/crypto_test
 */
#include <stdio.h>
#include <string.h>
#include "crypto.h"

static int hex(const char *s, uint8_t *out, int max) {
	int n = 0;
	for (; s[0] && s[1] && n < max; s += 2, n++) {
		int hi = s[0] <= '9' ? s[0]-'0' : (s[0]|32)-'a'+10;
		int lo = s[1] <= '9' ? s[1]-'0' : (s[1]|32)-'a'+10;
		out[n] = (uint8_t)((hi << 4) | lo);
	}
	return n;
}

static int fails = 0;
static void check(const char *name, const uint8_t *got, const uint8_t *want, int n) {
	if (memcmp(got, want, n) == 0) { printf("PASS %s\n", name); return; }
	printf("FAIL %s\n  got : ", name);
	for (int i = 0; i < n; i++) printf("%02x", got[i]);
	printf("\n  want: ");
	for (int i = 0; i < n; i++) printf("%02x", want[i]);
	printf("\n");
	fails++;
}

int main(void) {
	/* --- AES-128-GCM, NIST GCM spec Test Case 3 (64-byte PT, no AAD) --- */
	uint8_t key[16], iv[12], pt[64], ct[64], tag[16], out[64], gtag[16];
	hex("feffe9928665731c6d6a8f9467308308", key, 16);
	hex("cafebabefacedbaddecaf888", iv, 12);
	hex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255", pt, 64);
	hex("42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091473f5985", ct, 64);
	hex("4d5c2af327cd64a62cf35abd2ba6fab4", tag, 16);

	aes128_gcm_seal(key, iv, 0, 0, pt, 64, out, gtag);
	check("gcm seal ciphertext", out, ct, 64);
	check("gcm seal tag", gtag, tag, 16);

	int rc = aes128_gcm_open(key, iv, 0, 0, ct, 64, out, tag);
	printf("%s gcm open (auth ok)\n", rc == 0 ? "PASS" : "FAIL");
	if (rc != 0) fails++;
	check("gcm open plaintext", out, pt, 64);

	uint8_t badtag[16]; memcpy(badtag, tag, 16); badtag[0] ^= 1;
	rc = aes128_gcm_open(key, iv, 0, 0, ct, 64, out, badtag);
	printf("%s gcm open rejects bad tag\n", rc != 0 ? "PASS" : "FAIL");
	if (rc == 0) fails++;

	/* --- HKDF-SHA256, RFC 5869 Test Case 1 --- */
	uint8_t ikm[22], salt[13], info[10], prk[32], okm[42];
	uint8_t prk_want[32], okm_want[42];
	hex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", ikm, 22);
	hex("000102030405060708090a0b0c", salt, 13);
	hex("f0f1f2f3f4f5f6f7f8f9", info, 10);
	hex("077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5", prk_want, 32);
	hex("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865", okm_want, 42);

	hkdf_extract(salt, 13, ikm, 22, prk);
	check("hkdf-extract prk", prk, prk_want, 32);
	hkdf_expand(prk, info, 10, okm, 42);
	check("hkdf-expand okm", okm, okm_want, 42);

	/* --- TLS 1.2 PRF equivalence: the streamed P_SHA256 rewrite must match the
	 * original explicit-concatenation algorithm, computed here via the untouched
	 * hmac_sha256 primitive. Validates the rewrite over a length that spills past
	 * one HMAC block and needs an A(2). --- */
	{
		const char *label = "key expansion";
		uint8_t sec[20], seed2[40], prfout[70], ref[70];
		for (int i = 0; i < 20; i++) sec[i] = (uint8_t)(0x30 + i);
		for (int i = 0; i < 40; i++) seed2[i] = (uint8_t)(0xA0 ^ i);

		/* reference P_SHA256(sec, label||seed) -> ref[70] */
		int ll = (int)strlen(label);
		uint8_t full[13 + 40]; memcpy(full, label, ll); memcpy(full + ll, seed2, 40);
		int flen = ll + 40;
		uint8_t a[32], buf[32 + 53], blk[32];
		hmac_sha256(sec, 20, full, flen, a);
		int outlen = 70, off = 0;
		while (outlen) {
			memcpy(buf, a, 32); memcpy(buf + 32, full, flen);
			hmac_sha256(sec, 20, buf, 32 + flen, blk);
			int take = outlen < 32 ? outlen : 32;
			memcpy(ref + off, blk, take); off += take; outlen -= take;
			hmac_sha256(sec, 20, a, 32, a);
		}
		tls12_prf(sec, 20, label, seed2, 40, prfout, 70);
		check("tls1.2 prf == reference", prfout, ref, 70);
	}

	/* --- HMAC-SHA1, RFC 2202 Test Case 2 ("Jefe" / "what do ya want ...") --- */
	{
		uint8_t mac[20], want[20];
		hex("effcdf6ae5eb2fa2d27416d5f184df9c259a7c79", want, 20);
		hmac_sha1((const uint8_t *)"Jefe", 4,
		          (const uint8_t *)"what do ya want for nothing?", 28, mac);
		check("hmac-sha1 rfc2202 tc2", mac, want, 20);
	}

	/* --- AES-128-CTR, NIST SP 800-38A F.5.1/F.5.2 (CTR-AES128) --- */
	{
		uint8_t ckey[16], ictr[16], cpt[64], cct[64], cout[64];
		hex("2b7e151628aed2a6abf7158809cf4f3c", ckey, 16);
		hex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", ictr, 16);
		hex("6bc1bee22e409f96e93d7e117393172a"
		    "ae2d8a571e03ac9c9eb76fac45af8e51"
		    "30c81c46a35ce411e5fbc1191a0a52ef"
		    "f69f2445df4f9b17ad2b417be66c3710", cpt, 64);
		hex("874d6191b620e3261bef6864990db6ce"
		    "9806f66b7970fdff8617187bb9fffdff"
		    "5ae4df3edbd5d35e5b4f09020db03eab"
		    "1e031dda2fbe03d1792170a0f3009cee", cct, 64);
		aes128_ctr(ckey, ictr, cpt, 64, cout);
		check("aes-128-ctr encrypt", cout, cct, 64);
		aes128_ctr(ckey, ictr, cct, 64, cout);   /* decrypt = same op */
		check("aes-128-ctr decrypt", cout, cpt, 64);
		/* non-block-multiple length exercises the tail path */
		aes128_ctr(ckey, ictr, cpt, 20, cout);
		check("aes-128-ctr partial block", cout, cct, 20);
	}

	printf(fails ? "\n%d FAILURES\n" : "\nALL PASS\n", fails);
	return fails ? 1 : 0;
}
