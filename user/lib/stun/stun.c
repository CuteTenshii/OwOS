/* stun.c — STUN (RFC 5389 / 8489) message codec + reflexive-address query.
 *
 * The generic NAT-traversal substrate ICE/WebRTC gathers candidates and runs
 * connectivity checks over. Wire format only; the transport is the caller's
 * (stun_query lends a UDP socket via net_dial). See stun.h for the API. */

#include "owos.h"
#include "stun.h"
#include "crypto.h"     /* hmac_sha1 for MESSAGE-INTEGRITY                       */
#include "resolve.h"    /* net_dial for stun_query's name->IP                   */
#include "crc32.h"      /* the shared zlib/gzip/PNG checksum, for FINGERPRINT    */

/* ---- byte order (STUN is big-endian on the wire) ----------------------- */

static void put16(unsigned char *p, unsigned v) { p[0] = v >> 8; p[1] = v; }
static void put32(unsigned char *p, unsigned v)
{ p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static unsigned be16(const unsigned char *p) { return ((unsigned)p[0] << 8) | p[1]; }
static unsigned be32(const unsigned char *p)
{ return ((unsigned)p[0]<<24)|((unsigned)p[1]<<16)|((unsigned)p[2]<<8)|p[3]; }

/* The magic cookie as four bytes, and the XOR keys derived from it. */
static const unsigned char COOKIE[4] = { 0x21, 0x12, 0xA4, 0x42 };

/* Keep the header length field (bytes 2..3) equal to len - 20 (attributes only). */
static void sync_len(stun_msg *m) { put16(m->buf + 2, (unsigned)(m->len - 20)); }

/* ---- building ---------------------------------------------------------- */

void stun_txid(unsigned char txid[12]) { getrandom(txid, 12); }

void stun_init(stun_msg *m, unsigned type, const unsigned char txid[12])
{
	put16(m->buf + 0, type & 0x3FFF);
	put16(m->buf + 2, 0);
	put32(m->buf + 4, STUN_MAGIC);
	memcpy(m->buf + 8, txid, 12);
	m->len = 20;
}

int stun_add_raw(stun_msg *m, unsigned attr, const void *val, int vlen)
{
	if (vlen < 0) return 0;
	int pad = (4 - (vlen & 3)) & 3;
	if (m->len + 4 + vlen + pad > STUN_MAX) return 0;
	put16(m->buf + m->len, attr);
	put16(m->buf + m->len + 2, (unsigned)vlen);
	if (vlen) memcpy(m->buf + m->len + 4, val, (unsigned)vlen);
	for (int i = 0; i < pad; i++) m->buf[m->len + 4 + vlen + i] = 0;
	m->len += 4 + vlen + pad;
	sync_len(m);
	return 1;
}

int stun_add_u32(stun_msg *m, unsigned attr, unsigned v)
{
	unsigned char b[4];
	put32(b, v);
	return stun_add_raw(m, attr, b, 4);
}

int stun_add_u64(stun_msg *m, unsigned attr, unsigned hi, unsigned lo)
{
	unsigned char b[8];
	put32(b, hi);
	put32(b + 4, lo);
	return stun_add_raw(m, attr, b, 8);
}

int stun_add_flag(stun_msg *m, unsigned attr) { return stun_add_raw(m, attr, 0, 0); }

int stun_add_string(stun_msg *m, unsigned attr, const char *s)
{ return stun_add_raw(m, attr, s, strlen(s)); }

int stun_add_xor_mapped(stun_msg *m, unsigned attr, const stun_addr *a)
{
	unsigned char v[8];
	v[0] = 0;
	v[1] = 0x01;                                   /* family: IPv4               */
	put16(v + 2, (a->port & 0xFFFF) ^ (STUN_MAGIC >> 16));
	for (int i = 0; i < 4; i++) v[4 + i] = a->ip[i] ^ COOKIE[i];
	return stun_add_raw(m, attr, v, 8);
}

int stun_add_message_integrity(stun_msg *m, const void *key, int keylen)
{
	if (m->len + 24 > STUN_MAX) return 0;
	/* HMAC covers the message so far, but the header length must already count
	 * the 24-byte MESSAGE-INTEGRITY attribute (RFC 5389 §15.4). */
	put16(m->buf + 2, (unsigned)(m->len - 20 + 24));
	unsigned char mac[SHA1_DIGEST];
	hmac_sha1((const unsigned char *)key, (unsigned)keylen, m->buf, (unsigned)m->len, mac);
	return stun_add_raw(m, STUN_ATTR_MESSAGE_INTEGRITY, mac, SHA1_DIGEST);
}

int stun_add_fingerprint(stun_msg *m)
{
	if (m->len + 8 > STUN_MAX) return 0;
	put16(m->buf + 2, (unsigned)(m->len - 20 + 8));   /* count FINGERPRINT       */
	unsigned crc = crc32(0, m->buf, (unsigned)m->len) ^ 0x5354554Eu;
	return stun_add_u32(m, STUN_ATTR_FINGERPRINT, crc);
}

/* ---- parsing ----------------------------------------------------------- */

int stun_valid(const unsigned char *buf, int len)
{
	if (len < 20) return 0;
	if (buf[0] & 0xC0) return 0;                       /* top two bits must be 0  */
	if (be32(buf + 4) != STUN_MAGIC) return 0;
	unsigned alen = be16(buf + 2);
	if (alen & 3) return 0;                            /* attributes 4-aligned    */
	if (20 + (int)alen > len) return 0;
	return 1;
}

unsigned stun_type(const unsigned char *buf) { return be16(buf) & 0x3FFF; }

int stun_txid_eq(const unsigned char *buf, const unsigned char txid[12])
{ return memcmp(buf + 8, txid, 12) == 0; }

const unsigned char *stun_find(const unsigned char *buf, int len,
                               unsigned attr, int *vlen)
{
	if (!stun_valid(buf, len)) return 0;
	int end = 20 + (int)be16(buf + 2);
	int p = 20;
	while (p + 4 <= end) {
		unsigned t = be16(buf + p), l = be16(buf + p + 2);
		int val = p + 4;
		if (val + (int)l > end) break;
		if (t == attr) { if (vlen) *vlen = (int)l; return buf + val; }
		p = val + (int)l + (int)((4 - (l & 3)) & 3);   /* skip padding            */
	}
	return 0;
}

/* Decode a mapped-address attribute value (family/port/addr), undoing the XOR
 * against cookie+txid when `xor` is set. */
static int decode_addr(const unsigned char *buf, const unsigned char *v, int vlen,
                       int xored, stun_addr *out)
{
	if (vlen < 8 || v[1] != 0x01) return 0;            /* IPv4 only (8 bytes)     */
	unsigned port = be16(v + 2);
	unsigned char ip[4];
	for (int i = 0; i < 4; i++) ip[i] = v[4 + i];
	if (xored) {
		port ^= (STUN_MAGIC >> 16);
		for (int i = 0; i < 4; i++) ip[i] ^= COOKIE[i];  /* addr XORs the cookie  */
	}
	(void)buf;
	out->port = port & 0xFFFF;
	memcpy(out->ip, ip, 4);
	return 1;
}

int stun_get_mapped(const unsigned char *buf, int len, stun_addr *out)
{
	int vlen;
	const unsigned char *v = stun_find(buf, len, STUN_ATTR_XOR_MAPPED_ADDRESS, &vlen);
	if (v) return decode_addr(buf, v, vlen, 1, out);
	v = stun_find(buf, len, STUN_ATTR_MAPPED_ADDRESS, &vlen);
	if (v) return decode_addr(buf, v, vlen, 0, out);
	return 0;
}

int stun_check_integrity(unsigned char *buf, int len, const void *key, int keylen)
{
	int vlen;
	const unsigned char *v = stun_find(buf, len, STUN_ATTR_MESSAGE_INTEGRITY, &vlen);
	if (!v || vlen != SHA1_DIGEST) return 0;

	int mi_hdr = (int)(v - buf) - 4;                  /* start of the MI attribute */
	unsigned saved = be16(buf + 2);
	/* Recompute over everything up to the MI attribute, with the length field
	 * declaring the message ends just past MI (RFC 5389 §15.4). */
	put16(buf + 2, (unsigned)(mi_hdr - 20 + 24));
	unsigned char mac[SHA1_DIGEST];
	hmac_sha1((const unsigned char *)key, (unsigned)keylen, buf, (unsigned)mi_hdr, mac);
	put16(buf + 2, saved);
	return memcmp(mac, v, SHA1_DIGEST) == 0;
}

/* ---- convenience ------------------------------------------------------- */

int stun_query(const char *host, int port, stun_addr *out)
{
	int h = net_dial(host, port, SK_UDP);
	if (h < 0) return (h == NET_EINTR) ? STUN_EINTR : 0;

	unsigned char txid[12];
	stun_txid(txid);
	stun_msg req;
	stun_init(&req, STUN_BINDING_REQUEST, txid);
	stun_add_string(&req, STUN_ATTR_SOFTWARE, "OwOS");
	stun_add_fingerprint(&req);

	int rc = 0;
	for (int attempt = 0; attempt < 4; attempt++) {
		if (net_send(h, req.buf, req.len) < 0) break;
		unsigned char r[STUN_MAX];
		int rn = net_recv(h, r, (int)sizeof r);
		if (rn == NET_EINTR) { rc = STUN_EINTR; break; }
		if (rn == 0) continue;                         /* timeout: retransmit     */
		if (rn < 20) continue;
		if (!stun_valid(r, rn) || !stun_txid_eq(r, txid)) continue;
		if (stun_type(r) != STUN_BINDING_SUCCESS) break;
		rc = stun_get_mapped(r, rn, out) ? 1 : 0;
		break;
	}
	net_close(h);
	return rc;
}
