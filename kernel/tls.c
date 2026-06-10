/* tls.c — a single-connection TLS 1.2 client (see tls.h for the contract).
 *
 * Speaks exactly one cipher suite, TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256:
 *
 *   ClientHello (x25519 group, SNI)
 *        -> ServerHello, Certificate, ServerKeyExchange, ServerHelloDone
 *   ClientKeyExchange (our x25519 pubkey), ChangeCipherSpec, Finished
 *        -> ChangeCipherSpec, Finished
 *
 * The ECDHE shared secret feeds the TLS 1.2 PRF key schedule; both sides'
 * Finished messages carry a PRF over the full handshake transcript, so a
 * verified server Finished is end-to-end proof that every byte of the key
 * schedule (and therefore every primitive in crypto.c) is correct.
 *
 * Layered straight on tcp.c: records go out via tcp_write, come in via
 * tcp_read. Driven from command context only (curl), so no locking.
 */

#include <stdint.h>
#include <stddef.h>

#include "tls.h"
#include "tcp.h"
#include "crypto.h"
#include "timer.h"
#include "rtc.h"
#include "vga.h"
#include "kstring.h"

/* ------------------------------------------------------------------ helpers */

static void tmemmove(uint8_t *d, const uint8_t *s, uint32_t n)
{
	if (d < s) {
		while (n--)
			*d++ = *s++;
	} else {
		d += n; s += n;
		while (n--)
			*--d = *--s;
	}
}

static uint32_t rdtsc_mix(void)
{
	uint32_t lo, hi;
	__asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
	return lo ^ hi;
}

/* A weak best-effort CSPRNG: hash together the cycle counter, PIT time and the
 * RTC, chained through a counter. Adequate to keep ClientHello.random and the
 * ephemeral key unpredictable on this hobby kernel; not a real entropy source. */
static void gen_random(uint8_t *out, uint32_t n)
{
	static uint32_t ctr;
	while (n) {
		struct sha256 s;
		sha256_init(&s);
		uint32_t a = timer_us(), b = timer_ticks(), c = rdtsc_mix(), d = ctr++;
		struct rtc_time t;
		rtc_now(&t);
		sha256_update(&s, &a, 4);
		sha256_update(&s, &b, 4);
		sha256_update(&s, &c, 4);
		sha256_update(&s, &d, 4);
		sha256_update(&s, &t, sizeof t);
		uint8_t h[32];
		sha256_final(&s, h);
		uint32_t take = n < 32 ? n : 32;
		kmemcpy(out, h, take);
		out += take;
		n -= take;
	}
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put64be(uint8_t *p, uint64_t v)
{
	for (int i = 0; i < 8; i++)
		p[i] = (uint8_t)(v >> (56 - i * 8));
}

/* ----------------------------------------------------------------- wire */

#define REC_CCS    20
#define REC_ALERT  21
#define REC_HS     22
#define REC_APP    23

#define HS_CLIENT_HELLO 1
#define HS_SERVER_HELLO 2
#define HS_CERT         11
#define HS_SKE          12
#define HS_CERT_REQ     13
#define HS_DONE         14
#define HS_CKE          16
#define HS_FINISHED     20
#define HS_ENC_EXT       8       /* EncryptedExtensions (TLS 1.3) */
#define HS_CERT_VERIFY  15       /* CertificateVerify   (TLS 1.3) */

#define CIPHER_ECDHE_RSA_AES128_GCM 0xC02F   /* TLS 1.2 */
#define CIPHER_AES128_GCM_SHA256    0x1301   /* TLS 1.3 */

#define TLS_RECMAX 18432u
#define TLS_HSMAX  20480u

/* ------------------------------------------------------------------- state */

static int      established;
static tcp_conn_t *tcp;              /* the single TCP connection we ride on */
static int      is13;                /* negotiated TLS 1.3 (vs 1.2) */
static uint64_t client_seq, server_seq;
static int      send_encrypted, recv_encrypted;

static uint8_t  client_key[16], server_key[16];
static uint8_t  client_iv[12], server_iv[12];  /* 1.2 uses first 4; 1.3 all 12 */

static struct sha256 transcript;     /* running hash of handshake messages */

/* Raw inbound bytes from TCP, parsed into records. */
static uint8_t  rxraw[TLS_RECMAX + 5];
static uint32_t rxlen, rxpos;

/* Decrypted/plaintext payload of the current record. */
static uint8_t  recpt[TLS_RECMAX];

/* Reassembled handshake-message bytes (records can split/coalesce them). */
static uint8_t  hsbuf[TLS_HSMAX];
static uint32_t hslen, hspos;

/* Leftover application plaintext not yet handed to the caller. */
static uint8_t  appbuf[TLS_RECMAX];
static uint32_t applen, apppos;

/* Scratch for an outbound (possibly encrypted) record payload. */
static uint8_t  sbuf[TLS_RECMAX + 32];

/* ----------------------------------------------------------- record layer */

/* Pull TCP bytes until rxraw holds at least `need` unconsumed. -1 on EOF. */
static int raw_fill(uint32_t need)
{
	while (rxlen - rxpos < need) {
		if (rxpos > 0) {
			tmemmove(rxraw, rxraw + rxpos, rxlen - rxpos);
			rxlen -= rxpos;
			rxpos = 0;
		}
		if (rxlen == sizeof rxraw)
			return -1;
		int closed = 0;
		int n = tcp_read(tcp, rxraw + rxlen, sizeof rxraw - rxlen, &closed);
		if (n > 0)
			rxlen += (uint32_t)n;
		else
			return -1;            /* closed or idle timeout */
	}
	return 0;
}

/* TLS 1.3 per-record nonce: write_iv XOR seq (seq right-aligned in 12 bytes). */
static void nonce13(uint8_t out[12], const uint8_t iv[12], uint64_t seq)
{
	kmemcpy(out, iv, 12);
	for (int i = 0; i < 8; i++)
		out[11 - i] ^= (uint8_t)(seq >> (8 * i));
}

/* Receive one record; decrypt if the read side is active. Returns the (inner,
 * for 1.3) content type, fills out[*outlen] with plaintext. -1 on error/EOF. */
static int recv_record(uint8_t *type_out, uint8_t *out, uint32_t *outlen)
{
	for (;;) {
		if (raw_fill(5) < 0)
			return -1;
		uint8_t *h = rxraw + rxpos;
		uint8_t type = h[0];
		uint32_t len = ((uint32_t)h[3] << 8) | h[4];
		if (len > TLS_RECMAX || raw_fill(5 + len) < 0)
			return -1;
		uint8_t *payload = rxraw + rxpos + 5;

		/* TLS 1.3 sends a dummy ChangeCipherSpec for middlebox compat; ignore. */
		if (is13 && type == REC_CCS) {
			rxpos += 5 + len;
			continue;
		}

		if (!recv_encrypted) {
			kmemcpy(out, payload, len);
			*outlen = len;
			*type_out = type;
			rxpos += 5 + len;
			return 0;
		}

		if (is13) {
			if (len < 16)
				return -1;
			uint8_t nonce[12];
			nonce13(nonce, server_iv, server_seq);
			uint8_t aad[5] = { type, 0x03, 0x03,
			                   (uint8_t)(len >> 8), (uint8_t)len };
			uint32_t inner = len - 16;
			const uint8_t *tag = payload + inner;
			if (aes128_gcm_open(server_key, nonce, aad, 5, payload, inner,
			                    out, tag) != 0)
				return -1;
			server_seq++;
			rxpos += 5 + len;
			while (inner > 0 && out[inner - 1] == 0)   /* strip zero padding */
				inner--;
			if (inner == 0)
				return -1;
			*type_out = out[inner - 1];                /* real content type */
			*outlen = inner - 1;
			return 0;
		}

		/* TLS 1.2 AEAD: explicit-nonce prefix, sequence-number AAD. */
		if (len < 8 + 16)
			return -1;
		uint32_t ctlen = len - 8 - 16;
		uint8_t nonce[12];
		kmemcpy(nonce, server_iv, 4);
		kmemcpy(nonce + 4, payload, 8);
		uint8_t aad[13];
		put64be(aad, server_seq);
		aad[8] = type;
		aad[9] = 0x03; aad[10] = 0x03;
		put16(aad + 11, (uint16_t)ctlen);
		if (aes128_gcm_open(server_key, nonce, aad, 13, payload + 8, ctlen,
		                    out, payload + 8 + ctlen) != 0)
			return -1;
		*outlen = ctlen;
		*type_out = type;
		server_seq++;
		rxpos += 5 + len;
		return 0;
	}
}

/* Send one record; encrypt if the write side is active. */
static int send_record(uint8_t type, const uint8_t *data, uint32_t len)
{
	uint8_t hdr[5];
	if (!send_encrypted) {
		hdr[0] = type;
		hdr[1] = 0x03; hdr[2] = 0x03;
		put16(hdr + 3, (uint16_t)len);
		if (tcp_write(tcp, hdr, 5) < 0)
			return -1;
		if (len && tcp_write(tcp, data, len) < 0)
			return -1;
		return 0;
	}

	if (is13) {
		/* inner plaintext = content || real_type; outer type is always 23. */
		uint8_t nonce[12];
		nonce13(nonce, client_iv, client_seq);
		kmemcpy(sbuf, data, len);
		sbuf[len] = type;
		uint32_t inner = len + 1;
		uint32_t rlen = inner + 16;
		uint8_t aad[5] = { REC_APP, 0x03, 0x03,
		                   (uint8_t)(rlen >> 8), (uint8_t)rlen };
		uint8_t tag[16];
		aes128_gcm_seal(client_key, nonce, aad, 5, sbuf, inner, sbuf, tag);
		kmemcpy(sbuf + inner, tag, 16);
		hdr[0] = REC_APP;
		hdr[1] = 0x03; hdr[2] = 0x03;
		put16(hdr + 3, (uint16_t)rlen);
		if (tcp_write(tcp, hdr, 5) < 0 || tcp_write(tcp, sbuf, rlen) < 0)
			return -1;
		client_seq++;
		return 0;
	}

	uint8_t nonce[12];
	kmemcpy(nonce, client_iv, 4);
	put64be(sbuf, client_seq);                  /* explicit nonce = seq */
	kmemcpy(nonce + 4, sbuf, 8);
	uint8_t aad[13];
	put64be(aad, client_seq);
	aad[8] = type;
	aad[9] = 0x03; aad[10] = 0x03;
	put16(aad + 11, (uint16_t)len);
	uint8_t tag[16];
	aes128_gcm_seal(client_key, nonce, aad, 13, data, len, sbuf + 8, tag);
	kmemcpy(sbuf + 8 + len, tag, 16);
	uint32_t rlen = 8 + len + 16;
	hdr[0] = type;
	hdr[1] = 0x03; hdr[2] = 0x03;
	put16(hdr + 3, (uint16_t)rlen);
	if (tcp_write(tcp, hdr, 5) < 0 || tcp_write(tcp, sbuf, rlen) < 0)
		return -1;
	client_seq++;
	return 0;
}

/* ------------------------------------------------------- handshake reader */

/* Ensure the handshake buffer holds `need` bytes, pulling Handshake records.
 * Returns 0, -1 on error, -2 on an Alert. */
static int hs_fill(uint32_t need)
{
	while (hslen - hspos < need) {
		uint8_t type;
		uint32_t rl;
		if (recv_record(&type, recpt, &rl) < 0)
			return -1;
		if (type == REC_ALERT)
			return -2;
		if (type != REC_HS)
			return -1;
		if (hspos > 0) {
			tmemmove(hsbuf, hsbuf + hspos, hslen - hspos);
			hslen -= hspos;
			hspos = 0;
		}
		if (hslen + rl > sizeof hsbuf)
			return -1;
		kmemcpy(hsbuf + hslen, recpt, rl);
		hslen += rl;
	}
	return 0;
}

/* Next handshake message: sets type, points body at the payload, feeds the
 * full message (header included) into the transcript hash. */
static int hs_next(uint8_t *mtype, uint8_t **body, uint32_t *blen)
{
	int r = hs_fill(4);
	if (r < 0)
		return r;                     /* propagate -2 (alert) vs -1 (EOF) */
	uint32_t l = ((uint32_t)hsbuf[hspos + 1] << 16) |
	             ((uint32_t)hsbuf[hspos + 2] << 8) | hsbuf[hspos + 3];
	r = hs_fill(4 + l);
	if (r < 0)
		return r;
	uint8_t *h = hsbuf + hspos;
	*mtype = h[0];
	sha256_update(&transcript, h, 4 + l);
	*body = h + 4;
	*blen = l;
	hspos += 4 + l;
	return 0;
}

/* ----------------------------------------------- TLS 1.3 key schedule */

static uint32_t sstrlen(const char *s)
{
	uint32_t n = 0;
	while (s[n])
		n++;
	return n;
}

/* HKDF-Expand-Label (RFC 8446 §7.1): wraps the secret expansion with the
 * "tls13 "-prefixed label and a context (here always a transcript hash). */
static void hkdf_expand_label(const uint8_t secret[32], const char *label,
                              const uint8_t *ctx, uint8_t ctxlen,
                              uint8_t *out, uint16_t outlen)
{
	uint8_t info[2 + 1 + 6 + 32 + 1 + 32];
	uint32_t p = 0, ll = sstrlen(label);
	info[p++] = (uint8_t)(outlen >> 8);
	info[p++] = (uint8_t)outlen;
	info[p++] = (uint8_t)(6 + ll);
	kmemcpy(info + p, "tls13 ", 6); p += 6;
	kmemcpy(info + p, label, ll); p += ll;
	info[p++] = ctxlen;
	if (ctxlen) {
		kmemcpy(info + p, ctx, ctxlen);
		p += ctxlen;
	}
	hkdf_expand(secret, info, p, out, outlen);
}

static void derive_secret(const uint8_t secret[32], const char *label,
                          const uint8_t thash[32], uint8_t out[32])
{
	hkdf_expand_label(secret, label, thash, 32, out, 32);
}

/* Install a direction's traffic key+iv from its traffic secret. */
static void install_keys(const uint8_t c_secret[32], const uint8_t s_secret[32])
{
	hkdf_expand_label(c_secret, "key", 0, 0, client_key, 16);
	hkdf_expand_label(c_secret, "iv", 0, 0, client_iv, 12);
	hkdf_expand_label(s_secret, "key", 0, 0, server_key, 16);
	hkdf_expand_label(s_secret, "iv", 0, 0, server_iv, 12);
	client_seq = server_seq = 0;
}

/* --------------------------------------------------------- ClientHello */

/* One ClientHello offering both TLS 1.3 (key_share + supported_versions) and
 * TLS 1.2 (ECDHE-RSA). The server's chosen version decides which path runs. */
static uint32_t build_client_hello(uint8_t *out, const uint8_t client_random[32],
                                   const uint8_t our_pub[32], const char *sni)
{
	uint32_t namelen = 0;
	if (sni)
		while (sni[namelen])
			namelen++;

	uint8_t *p = out + 4;                 /* leave room for the HS header */

	*p++ = 0x03; *p++ = 0x03;             /* legacy_version = TLS 1.2 */
	kmemcpy(p, client_random, 32); p += 32;
	*p++ = 32;                            /* legacy_session_id (compat) */
	gen_random(p, 32); p += 32;

	/* cipher_suites: TLS 1.3 AEAD, TLS 1.2 ECDHE-RSA, empty-reneg SCSV */
	put16(p, 6); p += 2;
	put16(p, CIPHER_AES128_GCM_SHA256); p += 2;
	put16(p, CIPHER_ECDHE_RSA_AES128_GCM); p += 2;
	put16(p, 0x00FF); p += 2;

	*p++ = 1; *p++ = 0;                   /* compression_methods: null */

	uint8_t *extlen = p; p += 2;          /* extensions length, filled later */
	uint8_t *exts = p;

	/* server_name (SNI) */
	if (namelen) {
		put16(p, 0x0000); p += 2;
		put16(p, (uint16_t)(namelen + 5)); p += 2;
		put16(p, (uint16_t)(namelen + 3)); p += 2;
		*p++ = 0;                                 /* host_name type */
		put16(p, (uint16_t)namelen); p += 2;
		kmemcpy(p, sni, namelen); p += namelen;
	}
	/* supported_groups: x25519 */
	put16(p, 0x000A); p += 2;
	put16(p, 4); p += 2;
	put16(p, 2); p += 2;
	put16(p, 0x001D); p += 2;
	/* ec_point_formats: uncompressed (for the TLS 1.2 path) */
	put16(p, 0x000B); p += 2;
	put16(p, 2); p += 2;
	*p++ = 1; *p++ = 0;
	/* signature_algorithms: rsa_pss (1.3) + rsa_pkcs1 + ecdsa */
	put16(p, 0x000D); p += 2;
	static const uint8_t sigalgs[] = {
		0x08,0x04, 0x08,0x05, 0x08,0x06,          /* rsa_pss_rsae_sha256/384/512 */
		0x04,0x01, 0x05,0x01, 0x06,0x01,          /* rsa_pkcs1_sha256/384/512    */
		0x04,0x03, 0x05,0x03, 0x06,0x03 };        /* ecdsa_secp256r1/384/521     */
	put16(p, (uint16_t)(sizeof sigalgs + 2)); p += 2;
	put16(p, (uint16_t)sizeof sigalgs); p += 2;
	kmemcpy(p, sigalgs, sizeof sigalgs); p += sizeof sigalgs;
	/* supported_versions: TLS 1.3 then 1.2 */
	put16(p, 0x002B); p += 2;
	put16(p, 5); p += 2;
	*p++ = 4;
	put16(p, 0x0304); p += 2;
	put16(p, 0x0303); p += 2;
	/* key_share: one x25519 share */
	put16(p, 0x0033); p += 2;
	put16(p, 38); p += 2;
	put16(p, 36); p += 2;                 /* client_shares length */
	put16(p, 0x001D); p += 2;             /* group x25519 */
	put16(p, 32); p += 2;
	kmemcpy(p, our_pub, 32); p += 32;

	put16(extlen, (uint16_t)(p - exts));

	uint32_t body = (uint32_t)(p - (out + 4));
	out[0] = HS_CLIENT_HELLO;
	out[1] = (uint8_t)(body >> 16);
	out[2] = (uint8_t)(body >> 8);
	out[3] = (uint8_t)body;
	return body + 4;
}

/* Parse a ServerHello: extract server_random, chosen suite, whether TLS 1.3
 * was selected (supported_versions), and the server's x25519 share. */
static int parse_server_hello(const uint8_t *body, uint32_t bl,
                              uint8_t server_random[32], uint16_t *suite,
                              int *sel13, uint8_t server_pub[32], int *have_ks)
{
	*sel13 = 0;
	*have_ks = 0;
	if (bl < 38)
		return -1;
	kmemcpy(server_random, body + 2, 32);
	uint32_t off = 34;
	uint8_t sidlen = body[off++];
	if (off + sidlen + 3u > bl)
		return -1;
	off += sidlen;
	*suite = ((uint16_t)body[off] << 8) | body[off + 1];
	off += 3;                             /* suite(2) + compression(1) */
	if (off + 2u > bl)
		return 0;                     /* no extensions: fine for TLS 1.2 */
	uint16_t elen = ((uint16_t)body[off] << 8) | body[off + 1];
	off += 2;
	uint32_t end = off + elen < bl ? off + elen : bl;
	while (off + 4u <= end) {
		uint16_t et = ((uint16_t)body[off] << 8) | body[off + 1];
		uint16_t el = ((uint16_t)body[off + 2] << 8) | body[off + 3];
		off += 4;
		if (off + el > end)
			break;
		if (et == 0x002B && el >= 2) {
			if ((((uint16_t)body[off] << 8) | body[off + 1]) == 0x0304)
				*sel13 = 1;
		} else if (et == 0x0033 && el >= 4) {
			uint16_t grp = ((uint16_t)body[off] << 8) | body[off + 1];
			uint16_t kl = ((uint16_t)body[off + 2] << 8) | body[off + 3];
			if (grp == 0x001D && kl == 32 && el >= 4 + 32) {
				kmemcpy(server_pub, body + off + 4, 32);
				*have_ks = 1;
			}
		}
		off += el;
	}
	return 0;
}

/* The encrypted second half of a TLS 1.3 handshake, after ServerHello: derive
 * handshake keys, read EncryptedExtensions..server Finished (verifying it),
 * send client Finished, switch to application keys. Returns 1 on success. */
static int tls13_handshake(const uint8_t our_priv[32], const uint8_t server_pub[32])
{
	uint8_t zero[32] = {0}, empty_hash[32];
	sha256("", 0, empty_hash);

	uint8_t early[32], derived[32], shared[32], hs_secret[32];
	hkdf_extract(zero, 32, zero, 32, early);
	hkdf_expand_label(early, "derived", empty_hash, 32, derived, 32);
	x25519(shared, our_priv, server_pub);
	hkdf_extract(derived, 32, shared, 32, hs_secret);

	uint8_t th[32];
	{ struct sha256 c = transcript; sha256_final(&c, th); }   /* ..ServerHello */
	uint8_t c_hs[32], s_hs[32];
	derive_secret(hs_secret, "c hs traffic", th, c_hs);
	derive_secret(hs_secret, "s hs traffic", th, s_hs);
	is13 = 1;
	send_encrypted = recv_encrypted = 1;
	install_keys(c_hs, s_hs);

	uint8_t derived2[32], master[32];
	hkdf_expand_label(hs_secret, "derived", empty_hash, 32, derived2, 32);
	hkdf_extract(derived2, 32, zero, 32, master);

	/* Read EncryptedExtensions, Certificate, CertificateVerify, Finished. */
	for (;;) {
		struct sha256 before = transcript;   /* hash through prior messages */
		uint8_t mt, *body;
		uint32_t bl;
		int r = hs_next(&mt, &body, &bl);
		if (r == -2) { vga_write("tls: server alert (1.3)\n"); return 0; }
		if (r < 0) { vga_write("tls: 1.3 handshake read failed\n"); return 0; }
		if (mt == HS_CERT_REQ) {
			vga_write("tls: 1.3 client cert requested (unsupported)\n");
			return 0;
		}
		if (mt == HS_FINISHED) {
			if (bl != 32) { vga_write("tls: bad 1.3 Finished\n"); return 0; }
			uint8_t thcv[32], fk[32], expect[32];
			sha256_final(&before, thcv);  /* ..CertificateVerify */
			hkdf_expand_label(s_hs, "finished", 0, 0, fk, 32);
			hmac_sha256(fk, 32, thcv, 32, expect);
			for (int i = 0; i < 32; i++)
				if (body[i] != expect[i]) {
					vga_write("tls: 1.3 server Finished MISMATCH\n");
					return 0;
				}
			break;
		}
		/* EncryptedExtensions / Certificate / CertificateVerify: not verified. */
	}

	uint8_t th_sf[32];
	{ struct sha256 c = transcript; sha256_final(&c, th_sf); }  /* ..srv Finished */

	/* client Finished, still under the handshake key. */
	uint8_t fk_c[32], verify[32];
	hkdf_expand_label(c_hs, "finished", 0, 0, fk_c, 32);
	hmac_sha256(fk_c, 32, th_sf, 32, verify);
	uint8_t fin[36];
	fin[0] = HS_FINISHED; fin[1] = 0; fin[2] = 0; fin[3] = 32;
	kmemcpy(fin + 4, verify, 32);
	if (send_record(REC_HS, fin, 36) < 0) {
		vga_write("tls: send 1.3 Finished failed\n");
		return 0;
	}

	/* Switch both directions to application traffic keys. */
	uint8_t c_ap[32], s_ap[32];
	derive_secret(master, "c ap traffic", th_sf, c_ap);
	derive_secret(master, "s ap traffic", th_sf, s_ap);
	install_keys(c_ap, s_ap);

	established = 1;
	return 1;
}

/* --------------------------------------------------------------- connect */

static const uint8_t x25519_base[32] = { 9 };

int tls_connect(const uint8_t ip[4], uint16_t port, const char *sni)
{
	established = 0;
	is13 = 0;
	rxlen = rxpos = hslen = hspos = applen = apppos = 0;
	client_seq = server_seq = 0;
	send_encrypted = recv_encrypted = 0;

	tcp = tcp_connect(ip, port);
	if (!tcp) {
		vga_write("tls: tcp connect failed\n");
		return 0;
	}

	sha256_init(&transcript);

	uint8_t client_random[32], server_random[32];
	uint8_t our_priv[32], our_pub[32];
	gen_random(client_random, 32);
	gen_random(our_priv, 32);
	x25519(our_pub, our_priv, x25519_base);

	/* --- ClientHello --- */
	uint8_t ch[768];
	uint32_t chlen = build_client_hello(ch, client_random, our_pub, sni);
	sha256_update(&transcript, ch, chlen);
	if (send_record(REC_HS, ch, chlen) < 0) {
		vga_write("tls: send ClientHello failed\n");
		tcp_close(tcp);
		return 0;
	}

	/* --- ServerHello (plaintext in both versions); pick the version path --- */
	uint8_t server_pub[32];
	{
		uint8_t mt, *body;
		uint32_t bl;
		int r = hs_next(&mt, &body, &bl);
		if (r == -2) {
			vga_write("tls: server sent an alert\n");
			tcp_close(tcp);
			return 0;
		}
		if (r < 0 || mt != HS_SERVER_HELLO) {
			vga_write("tls: expected ServerHello\n");
			tcp_close(tcp);
			return 0;
		}
		uint16_t suite;
		int sel13, have_ks;
		if (parse_server_hello(body, bl, server_random, &suite, &sel13,
		                       server_pub, &have_ks) < 0) {
			vga_write("tls: bad ServerHello\n");
			tcp_close(tcp);
			return 0;
		}
		if (sel13) {
			if (suite != CIPHER_AES128_GCM_SHA256 || !have_ks) {
				vga_write("tls: unsupported TLS 1.3 parameters\n");
				tcp_close(tcp);
				return 0;
			}
			if (!tls13_handshake(our_priv, server_pub)) {
				tcp_close(tcp);
				return 0;
			}
			return 1;
		}
		if (suite != CIPHER_ECDHE_RSA_AES128_GCM) {
			vga_write("tls: server chose an unsupported cipher\n");
			tcp_close(tcp);
			return 0;
		}
	}

	/* --- TLS 1.2 continuation: ServerKeyExchange .. ServerHelloDone --- */
	int have_ske = 0;
	for (;;) {
		uint8_t mt, *body;
		uint32_t bl;
		int r = hs_next(&mt, &body, &bl);
		if (r == -2) {
			vga_write("tls: server sent an alert\n");
			tcp_close(tcp);
			return 0;
		}
		if (r < 0) {
			vga_write("tls: handshake read failed\n");
			tcp_close(tcp);
			return 0;
		}
		if (mt == HS_SKE) {
			/* curve_type(1) named_curve(2) pubkeylen(1) pubkey ... */
			if (bl < 4 || body[0] != 3) { tcp_close(tcp); return 0; }
			uint16_t curve = ((uint16_t)body[1] << 8) | body[2];
			uint8_t pklen = body[3];
			if (curve != 0x001D || pklen != 32 || bl < 4u + 32) {
				vga_write("tls: server key exchange not x25519/32\n");
				tcp_close(tcp);
				return 0;
			}
			kmemcpy(server_pub, body + 4, 32);
			have_ske = 1;
		} else if (mt == HS_CERT_REQ) {
			vga_write("tls: server requested a client certificate (unsupported)\n");
			tcp_close(tcp);
			return 0;
		} else if (mt == HS_DONE) {
			break;
		}
		/* HS_CERT and anything else: ignored (no verification). */
	}
	if (!have_ske) {
		vga_write("tls: no ServerKeyExchange\n");
		tcp_close(tcp);
		return 0;
	}

	/* --- key schedule --- */
	uint8_t pre_master[32];
	x25519(pre_master, our_priv, server_pub);

	uint8_t seed[64], master[48], keyblock[40];
	kmemcpy(seed, client_random, 32);
	kmemcpy(seed + 32, server_random, 32);
	tls12_prf(pre_master, 32, "master secret", seed, 64, master, 48);

	kmemcpy(seed, server_random, 32);
	kmemcpy(seed + 32, client_random, 32);
	tls12_prf(master, 48, "key expansion", seed, 64, keyblock, 40);
	kmemcpy(client_key, keyblock, 16);
	kmemcpy(server_key, keyblock + 16, 16);
	kmemcpy(client_iv, keyblock + 32, 4);
	kmemcpy(server_iv, keyblock + 36, 4);

	/* --- ClientKeyExchange --- */
	uint8_t cke[4 + 1 + 32];
	cke[0] = HS_CKE;
	cke[1] = 0; cke[2] = 0; cke[3] = 33;
	cke[4] = 32;
	kmemcpy(cke + 5, our_pub, 32);
	sha256_update(&transcript, cke, sizeof cke);
	if (send_record(REC_HS, cke, sizeof cke) < 0) {
		vga_write("tls: send CKE failed\n");
		tcp_close(tcp);
		return 0;
	}

	/* --- ChangeCipherSpec, then switch our write side to encrypted --- */
	uint8_t one = 1;
	send_record(REC_CCS, &one, 1);
	send_encrypted = 1;
	client_seq = 0;

	/* --- client Finished --- */
	struct sha256 tc = transcript;
	uint8_t thash[32];
	sha256_final(&tc, thash);
	uint8_t verify[12];
	tls12_prf(master, 48, "client finished", thash, 32, verify, 12);
	uint8_t fin[16];
	fin[0] = HS_FINISHED;
	fin[1] = 0; fin[2] = 0; fin[3] = 12;
	kmemcpy(fin + 4, verify, 12);
	sha256_update(&transcript, fin, 16);
	if (send_record(REC_HS, fin, 16) < 0) {
		vga_write("tls: send Finished failed\n");
		tcp_close(tcp);
		return 0;
	}

	/* expected server Finished (transcript now includes client Finished) */
	struct sha256 tc2 = transcript;
	uint8_t thash2[32], server_verify[12];
	sha256_final(&tc2, thash2);
	tls12_prf(master, 48, "server finished", thash2, 32, server_verify, 12);

	/* --- server ChangeCipherSpec --- */
	uint8_t t;
	uint32_t l;
	if (recv_record(&t, recpt, &l) < 0 || t != REC_CCS) {
		vga_write("tls: expected server ChangeCipherSpec\n");
		tcp_close(tcp);
		return 0;
	}
	recv_encrypted = 1;
	server_seq = 0;
	hslen = hspos = 0;

	/* --- server Finished (encrypted handshake record) --- */
	uint8_t mt, *body;
	uint32_t bl;
	if (hs_next(&mt, &body, &bl) < 0 || mt != HS_FINISHED || bl != 12) {
		vga_write("tls: bad server Finished\n");
		tcp_close(tcp);
		return 0;
	}
	for (int i = 0; i < 12; i++) {
		if (body[i] != server_verify[i]) {
			vga_write("tls: server Finished verify MISMATCH\n");
			tcp_close(tcp);
			return 0;
		}
	}

	established = 1;
	return 1;
}

/* --------------------------------------------------------- app data I/O */

int tls_write(const void *data, uint32_t len)
{
	if (!established)
		return -1;
	const uint8_t *p = data;
	uint32_t sent = 0;
	while (sent < len) {
		uint32_t chunk = len - sent;
		if (chunk > 16384)
			chunk = 16384;
		if (send_record(REC_APP, p + sent, chunk) < 0)
			return -1;
		sent += chunk;
	}
	return (int)len;
}

int tls_read(uint8_t *buf, uint32_t max, int *closed)
{
	*closed = 0;
	if (apppos < applen) {
		uint32_t n = applen - apppos;
		if (n > max)
			n = max;
		kmemcpy(buf, appbuf + apppos, n);
		apppos += n;
		return (int)n;
	}
	if (!established) {
		*closed = 1;
		return 0;
	}

	for (;;) {
		uint8_t t;
		uint32_t l;
		if (recv_record(&t, recpt, &l) < 0) {
			*closed = 1;
			return 0;
		}
		if (t == REC_APP) {
			if (l == 0)
				continue;
			kmemcpy(appbuf, recpt, l);
			applen = l;
			apppos = 0;
			uint32_t n = l < max ? l : max;
			kmemcpy(buf, appbuf, n);
			apppos = n;
			return (int)n;
		}
		if (t == REC_ALERT) {                 /* close_notify or fatal */
			*closed = 1;
			return 0;
		}
		/* REC_HS (e.g. NewSessionTicket) and others: ignore, keep reading. */
	}
}

void tls_close(void)
{
	if (established) {
		uint8_t alert[2] = { 1, 0 };          /* warning, close_notify */
		send_record(REC_ALERT, alert, 2);
	}
	established = 0;
	tcp_close(tcp);
}
