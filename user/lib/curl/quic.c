/* quic.c — a minimal QUIC v1 client (RFC 9000 transport, RFC 9001 TLS) in ring
 * 3, over a SK_UDP socket. See quic.h for the contract and scope notes.
 *
 * Layout: varint + byte helpers; the TLS 1.3 key schedule (HKDF-Expand-Label,
 * mirrored from kernel/tls.c) + QUIC key derivation; packet protection
 * (AES-128-GCM payload, AES-ECB header protection); the receive path (parse a
 * datagram's coalesced packets, decrypt, dispatch frames); the TLS handshake
 * carried in CRYPTO frames; and the public connect/send/read/poll/close API. */

#include "owos.h"
#include "crypto.h"
#include "quic.h"
#include "resolve.h"

/* ----------------------------------------------------------------- helpers */

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

/* QUIC variable-length integers (RFC 9000 §16): the top 2 bits of the first
 * byte give the length (1/2/4/8 bytes); the value is the remaining bits. */
static int getvar(const uint8_t *p, uint32_t *o, uint32_t len, uint64_t *v)
{
	if (*o >= len)
		return 0;
	uint8_t b = p[*o];
	int l = 1 << (b >> 6);
	if (*o + (uint32_t)l > len)
		return 0;
	uint64_t x = b & 0x3f;
	(*o)++;
	for (int i = 1; i < l; i++)
		x = (x << 8) | p[(*o)++];
	*v = x;
	return 1;
}

static int putvar(uint8_t *p, uint64_t v)
{
	if (v < 0x40)            { p[0] = (uint8_t)v; return 1; }
	if (v < 0x4000)          { p[0] = 0x40 | (uint8_t)(v >> 8); p[1] = (uint8_t)v; return 2; }
	if (v < 0x40000000)      { p[0] = 0x80 | (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
	                           p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; return 4; }
	for (int i = 0; i < 8; i++)
		p[i] = (uint8_t)(v >> (56 - 8 * i));
	p[0] |= 0xc0;
	return 8;
}

static int varsize(uint64_t v)
{
	return v < 0x40 ? 1 : v < 0x4000 ? 2 : v < 0x40000000 ? 4 : 8;
}

/* ClientHello.random, the ephemeral x25519 key and the connection IDs come from
 * the shared userspace CSPRNG (rng_bytes: RDRAND-backed). */
static void gen_random(uint8_t *out, uint32_t n) { rng_bytes(out, (int)n); }

/* ------------------------------------------------------- connection state */

enum { S_INIT = 0, S_HS = 1, S_APP = 2 };   /* packet-number spaces */

struct space {
	int      hk;                /* keys installed                       */
	uint8_t  ck[16], ci[12], ch[16];   /* our (client) key / iv / hp    */
	uint8_t  sk[16], si[12], sh[16];   /* their (server) key / iv / hp  */
	uint64_t spn;               /* next packet number to send           */
	uint64_t lrec;              /* largest packet number received       */
	int      hrec;              /* received any packet                   */
	int      ackp;              /* an ACK is owed in this space          */
	/* CRYPTO-stream reassembly (in-order; offset 0 based) */
	uint8_t  cbuf[16384];
	uint32_t chave;             /* contiguous bytes available           */
	uint32_t crd;               /* TLS-message read cursor              */
};

#define QS_MAX 8
#define QS_BUF 8192
struct qstream {
	int      used;
	uint64_t id;
	uint64_t recv_off;          /* in-order bytes received so far        */
	uint64_t send_off;          /* bytes we've sent                      */
	int      fin;
	uint64_t fin_off;
	uint8_t  buf[QS_BUF];       /* unread, in-order data [r_pos,r_len)   */
	uint32_t r_pos, r_len;
};

static int      sock;
static int      closed;
static struct space sp[3];
static struct qstream qs[QS_MAX];

static uint8_t  scid[8];   static int scid_len;     /* our source CID      */
static uint8_t  dcid[20];  static int dcid_len;     /* peer (server) CID   */
static int      server_cid_set;
static uint8_t  retry_token[256]; static int retry_token_len; static int did_retry;

/* TLS 1.3 handshake state (one connection, like kernel tls.c). */
static struct sha256 transcript;
static uint8_t  our_priv[32], our_pub[32];
static uint8_t  c_hs[32], s_hs[32], master[32];
static int      got_sh, app_done, hs_sent_fin, hs_confirmed;
static uint8_t  pending_fin[40]; static int pending_fin_len;

static uint8_t  chello[1024]; static uint32_t chello_len;   /* kept for retransmit */

static const uint8_t x25519_base[32] = { 9 };

/* ----------------------------------------------- TLS 1.3 / QUIC key schedule */

/* HKDF-Expand-Label (RFC 8446 §7.1): "tls13 "-prefixed label + a context. */
static void expand_label(const uint8_t secret[32], const char *label,
                         const uint8_t *ctx, uint8_t ctxlen,
                         uint8_t *out, uint16_t outlen)
{
	uint8_t info[2 + 1 + 6 + 32 + 1 + 32];
	uint32_t p = 0, ll = (uint32_t)strlen(label);
	info[p++] = (uint8_t)(outlen >> 8);
	info[p++] = (uint8_t)outlen;
	info[p++] = (uint8_t)(6 + ll);
	memcpy(info + p, "tls13 ", 6); p += 6;
	memcpy(info + p, label, ll);   p += ll;
	info[p++] = ctxlen;
	if (ctxlen) { memcpy(info + p, ctx, ctxlen); p += ctxlen; }
	hkdf_expand(secret, info, p, out, outlen);
}

static void derive_secret(const uint8_t secret[32], const char *label,
                          const uint8_t thash[32], uint8_t out[32])
{
	expand_label(secret, label, thash, 32, out, 32);
}

/* Install one space's packet-protection keys from its client/server secrets. */
static void install_space(int space, const uint8_t cli[32], const uint8_t srv[32])
{
	struct space *s = &sp[space];
	expand_label(cli, "quic key", 0, 0, s->ck, 16);
	expand_label(cli, "quic iv",  0, 0, s->ci, 12);
	expand_label(cli, "quic hp",  0, 0, s->ch, 16);
	expand_label(srv, "quic key", 0, 0, s->sk, 16);
	expand_label(srv, "quic iv",  0, 0, s->si, 12);
	expand_label(srv, "quic hp",  0, 0, s->sh, 16);
	s->hk = 1;
}

/* Initial secrets (RFC 9001 §5.2): HKDF-Extract(v1 salt, DCID) then "client in"
 * / "server in" expand. Reset the Initial space's counters too (used on Retry). */
static void derive_initial_keys(const uint8_t *cid, int cidlen)
{
	static const uint8_t salt[20] = {
		0x38,0x76,0x2c,0xf7,0xf5,0x59,0x34,0xb3,0x4d,0x17,
		0x9a,0xe6,0xa4,0xc8,0x0c,0xad,0xcc,0xbb,0x7f,0x0a };
	uint8_t isecret[32], cs[32], ss[32];
	hkdf_extract(salt, 20, cid, (uint32_t)cidlen, isecret);
	expand_label(isecret, "client in", 0, 0, cs, 32);
	expand_label(isecret, "server in", 0, 0, ss, 32);
	sp[S_INIT].spn = 0;
	sp[S_INIT].lrec = 0; sp[S_INIT].hrec = 0; sp[S_INIT].ackp = 0;
	sp[S_INIT].chave = sp[S_INIT].crd = 0;
	install_space(S_INIT, cs, ss);
}

static void th_snapshot(uint8_t out[32])
{
	struct sha256 c = transcript;
	sha256_final(&c, out);
}

/* ------------------------------------------------------------- ClientHello */

static const uint8_t sigalgs[] = {
	0x08,0x04, 0x08,0x05, 0x08,0x06,          /* rsa_pss_rsae_sha256/384/512 */
	0x04,0x01, 0x05,0x01, 0x06,0x01,          /* rsa_pkcs1_sha256/384/512    */
	0x04,0x03, 0x05,0x03, 0x06,0x03 };        /* ecdsa_secp256r1/384/521     */

/* QUIC transport parameters (TLS extension 0x39). */
static uint32_t build_tp(uint8_t *b)
{
	uint32_t p = 0;
	struct { uint64_t id, val; } ints[] = {
		{ 0x01, 30000 },        /* max_idle_timeout (ms)                 */
		{ 0x03, 1472 },         /* max_udp_payload_size                  */
		{ 0x04, 0x1000000 },    /* initial_max_data                      */
		{ 0x05, 0x1000000 },    /* initial_max_stream_data_bidi_local    */
		{ 0x06, 0x100000 },     /* initial_max_stream_data_bidi_remote   */
		{ 0x07, 0x100000 },     /* initial_max_stream_data_uni           */
		{ 0x08, 0 },            /* initial_max_streams_bidi              */
		{ 0x09, 3 },            /* initial_max_streams_uni               */
		{ 0x0e, 3 },            /* active_connection_id_limit            */
	};
	for (unsigned i = 0; i < sizeof ints / sizeof ints[0]; i++) {
		p += putvar(b + p, ints[i].id);
		p += putvar(b + p, (uint64_t)varsize(ints[i].val));
		p += putvar(b + p, ints[i].val);
	}
	/* initial_source_connection_id (0x0f): raw CID bytes */
	p += putvar(b + p, 0x0f);
	p += putvar(b + p, (uint64_t)scid_len);
	memcpy(b + p, scid, scid_len); p += scid_len;
	return p;
}

/* Build a TLS 1.3 ClientHello (4-byte HS header included) for QUIC: x25519 key
 * share, SNI, ALPN, and the QUIC transport-parameters extension. */
static uint32_t build_client_hello(uint8_t *out, const char *sni, const char *alpn)
{
	uint32_t namelen = sni ? (uint32_t)strlen(sni) : 0;
	uint32_t alpnlen = (uint32_t)strlen(alpn);
	uint8_t  client_random[32];
	gen_random(client_random, 32);

	uint8_t *p = out + 4;
	*p++ = 0x03; *p++ = 0x03;                  /* legacy_version = TLS 1.2 */
	memcpy(p, client_random, 32); p += 32;
	*p++ = 0;                                  /* legacy_session_id: empty */

	put16(p, 2); p += 2;                       /* cipher_suites */
	put16(p, 0x1301); p += 2;                  /* TLS_AES_128_GCM_SHA256 */
	*p++ = 1; *p++ = 0;                        /* compression: null */

	uint8_t *extlen = p; p += 2;
	uint8_t *exts = p;

	if (namelen) {                             /* server_name */
		put16(p, 0x0000); p += 2;
		put16(p, (uint16_t)(namelen + 5)); p += 2;
		put16(p, (uint16_t)(namelen + 3)); p += 2;
		*p++ = 0;
		put16(p, (uint16_t)namelen); p += 2;
		memcpy(p, sni, namelen); p += namelen;
	}
	put16(p, 0x000A); p += 2;                  /* supported_groups: x25519 */
	put16(p, 4); p += 2;
	put16(p, 2); p += 2;
	put16(p, 0x001D); p += 2;
	put16(p, 0x000D); p += 2;                  /* signature_algorithms */
	put16(p, (uint16_t)(sizeof sigalgs + 2)); p += 2;
	put16(p, (uint16_t)sizeof sigalgs); p += 2;
	memcpy(p, sigalgs, sizeof sigalgs); p += sizeof sigalgs;
	put16(p, 0x002B); p += 2;                  /* supported_versions: 1.3 only */
	put16(p, 3); p += 2;
	*p++ = 2;
	put16(p, 0x0304); p += 2;
	put16(p, 0x0033); p += 2;                  /* key_share: x25519 */
	put16(p, 38); p += 2;
	put16(p, 36); p += 2;
	put16(p, 0x001D); p += 2;
	put16(p, 32); p += 2;
	memcpy(p, our_pub, 32); p += 32;
	put16(p, 0x0010); p += 2;                  /* ALPN */
	put16(p, (uint16_t)(alpnlen + 3)); p += 2;
	put16(p, (uint16_t)(alpnlen + 1)); p += 2;
	*p++ = (uint8_t)alpnlen;
	memcpy(p, alpn, alpnlen); p += alpnlen;
	put16(p, 0x0039); p += 2;                  /* quic_transport_parameters */
	uint8_t *tplen = p; p += 2;
	uint32_t tpn = build_tp(p); p += tpn;
	put16(tplen, (uint16_t)tpn);

	put16(extlen, (uint16_t)(p - exts));

	uint32_t body = (uint32_t)(p - (out + 4));
	out[0] = 1;                                /* HS type = ClientHello */
	out[1] = (uint8_t)(body >> 16);
	out[2] = (uint8_t)(body >> 8);
	out[3] = (uint8_t)body;
	return body + 4;
}

/* Pull the server's x25519 share out of a ServerHello body. */
static int parse_server_hello(const uint8_t *body, uint32_t bl, uint8_t server_pub[32])
{
	if (bl < 38)
		return -1;
	uint32_t off = 34;
	uint8_t sidlen = body[off++];
	if (off + sidlen + 3u > bl)
		return -1;
	off += sidlen;
	uint16_t suite = ((uint16_t)body[off] << 8) | body[off + 1];
	off += 3;                                  /* suite(2) + compression(1) */
	if (suite != 0x1301)
		return -1;
	if (off + 2u > bl)
		return -1;
	uint16_t elen = ((uint16_t)body[off] << 8) | body[off + 1];
	off += 2;
	uint32_t end = off + elen < bl ? off + elen : bl;
	int have_ks = 0;
	while (off + 4u <= end) {
		uint16_t et = ((uint16_t)body[off] << 8) | body[off + 1];
		uint16_t el = ((uint16_t)body[off + 2] << 8) | body[off + 3];
		off += 4;
		if (off + el > end)
			break;
		if (et == 0x0033 && el >= 4) {
			uint16_t grp = ((uint16_t)body[off] << 8) | body[off + 1];
			uint16_t kl  = ((uint16_t)body[off + 2] << 8) | body[off + 3];
			if (grp == 0x001D && kl == 32 && el >= 36) {
				memcpy(server_pub, body + off + 4, 32);
				have_ks = 1;
			}
		}
		off += el;
	}
	return have_ks ? 0 : -1;
}

/* --------------------------------------------------- TLS message dispatch */

/* ServerHello processed: derive handshake + master secrets, install hs keys. */
static int on_server_hello(const uint8_t *body, uint32_t bl)
{
	uint8_t server_pub[32];
	if (parse_server_hello(body, bl, server_pub) < 0)
		return -1;

	uint8_t zero[32] = {0}, empty_hash[32];
	sha256("", 0, empty_hash);

	uint8_t early[32], derived[32], shared[32], hs_secret[32];
	hkdf_extract(zero, 32, zero, 32, early);
	expand_label(early, "derived", empty_hash, 32, derived, 32);
	x25519(shared, our_priv, server_pub);
	hkdf_extract(derived, 32, shared, 32, hs_secret);

	uint8_t th[32];
	th_snapshot(th);                           /* transcript ..ServerHello */
	derive_secret(hs_secret, "c hs traffic", th, c_hs);
	derive_secret(hs_secret, "s hs traffic", th, s_hs);
	install_space(S_HS, c_hs, s_hs);

	uint8_t derived2[32];
	expand_label(hs_secret, "derived", empty_hash, 32, derived2, 32);
	hkdf_extract(derived2, 32, zero, 32, master);
	got_sh = 1;
	return 0;
}

/* Server Finished received (th_pre = transcript through CertificateVerify):
 * verify it, derive + install 1-RTT keys, and stage the client Finished. */
static int on_server_finished(const uint8_t *body, uint32_t bl, const uint8_t th_pre[32])
{
	if (bl != 32)
		return -1;
	uint8_t fk[32], expect[32];
	expand_label(s_hs, "finished", 0, 0, fk, 32);
	hmac_sha256(fk, 32, th_pre, 32, expect);
	for (int i = 0; i < 32; i++)
		if (body[i] != expect[i])
			return -1;

	uint8_t th_sf[32];
	th_snapshot(th_sf);                         /* transcript ..server Finished */

	uint8_t fkc[32], verify[32];
	expand_label(c_hs, "finished", 0, 0, fkc, 32);
	hmac_sha256(fkc, 32, th_sf, 32, verify);
	pending_fin[0] = 20; pending_fin[1] = 0; pending_fin[2] = 0; pending_fin[3] = 32;
	memcpy(pending_fin + 4, verify, 32);
	pending_fin_len = 36;

	uint8_t c_ap[32], s_ap[32];
	derive_secret(master, "c ap traffic", th_sf, c_ap);
	derive_secret(master, "s ap traffic", th_sf, s_ap);
	install_space(S_APP, c_ap, s_ap);
	app_done = 1;
	return 0;
}

/* Extract complete TLS handshake messages from a space's CRYPTO buffer,
 * feeding each into the transcript hash and dispatching it. */
static void extract_tls(int space)
{
	struct space *s = &sp[space];
	for (;;) {
		if (s->crd + 4 > s->chave)
			return;
		uint8_t mt = s->cbuf[s->crd];
		uint32_t l = ((uint32_t)s->cbuf[s->crd + 1] << 16) |
		             ((uint32_t)s->cbuf[s->crd + 2] << 8) | s->cbuf[s->crd + 3];
		uint32_t total = 4 + l;
		if (s->crd + total > s->chave)
			return;
		const uint8_t *msg = s->cbuf + s->crd;

		uint8_t th_pre[32];
		if (mt == 20)                          /* Finished: hash BEFORE update */
			th_snapshot(th_pre);
		sha256_update(&transcript, msg, total);

		if (mt == 2)                           /* ServerHello */
			on_server_hello(msg + 4, l);
		else if (mt == 20)                     /* Finished */
			on_server_finished(msg + 4, l, th_pre);
		/* EncryptedExtensions/Certificate/CertificateVerify: not verified */

		s->crd += total;
	}
}

static void crypto_recv(int space, uint64_t off, const uint8_t *data, uint32_t n)
{
	struct space *s = &sp[space];
	if (off > s->chave)                        /* gap (assume in-order): hold */
		return;
	if (off + n > sizeof s->cbuf)
		n = (uint32_t)(sizeof s->cbuf - off);
	memcpy(s->cbuf + off, data, n);
	if (off + n > s->chave)
		s->chave = (uint32_t)(off + n);
	extract_tls(space);
}

/* ------------------------------------------------------------- streams */

static struct qstream *get_stream(uint64_t id)
{
	for (int i = 0; i < QS_MAX; i++)
		if (qs[i].used && qs[i].id == id)
			return &qs[i];
	for (int i = 0; i < QS_MAX; i++)
		if (!qs[i].used) {
			memset(&qs[i], 0, sizeof qs[i]);
			qs[i].used = 1;
			qs[i].id = id;
			return &qs[i];
		}
	return 0;
}

static void stream_recv(uint64_t id, uint64_t off, const uint8_t *data,
                        uint32_t n, int fin)
{
	if ((id & 0x03) != 0)                       /* not client-bidi: discard */
		return;
	struct qstream *q = get_stream(id);
	if (!q)
		return;
	if (fin) { q->fin = 1; q->fin_off = off + n; }
	if (off > q->recv_off)                      /* gap (in-order assumption) */
		return;
	uint64_t end = off + n;
	if (end <= q->recv_off)                     /* duplicate */
		return;
	uint32_t skip = (uint32_t)(q->recv_off - off);
	const uint8_t *src = data + skip;
	uint32_t cnt = (uint32_t)(end - q->recv_off);

	if (q->r_pos > 0) {                         /* compact consumed prefix */
		memmove(q->buf, q->buf + q->r_pos, q->r_len - q->r_pos);
		q->r_len -= q->r_pos;
		q->r_pos = 0;
	}
	if (q->r_len + cnt > QS_BUF)
		cnt = QS_BUF - q->r_len;                /* truncate on overflow */
	memcpy(q->buf + q->r_len, src, cnt);
	q->r_len += cnt;
	q->recv_off += cnt;
}

/* ------------------------------------------------------ frame processing */

static void process_frames(int space, const uint8_t *p, uint32_t len)
{
	uint32_t o = 0;
	int ackel = 0;
	while (o < len) {
		uint64_t t;
		if (!getvar(p, &o, len, &t))
			return;
		uint64_t a, b, c, rc;
		switch (t) {
		case 0x00:                                     /* PADDING */
			while (o < len && p[o] == 0x00) o++;
			break;
		case 0x01: ackel = 1; break;                   /* PING */
		case 0x02: case 0x03:                          /* ACK */
			if (!getvar(p,&o,len,&a) || !getvar(p,&o,len,&b) ||
			    !getvar(p,&o,len,&rc) || !getvar(p,&o,len,&c)) return;
			for (uint64_t i = 0; i < rc; i++)
				if (!getvar(p,&o,len,&a) || !getvar(p,&o,len,&b)) return;
			if (t == 0x03)
				if (!getvar(p,&o,len,&a) || !getvar(p,&o,len,&b) ||
				    !getvar(p,&o,len,&c)) return;
			break;
		case 0x04:                                     /* RESET_STREAM */
			if (!getvar(p,&o,len,&a) || !getvar(p,&o,len,&b) ||
			    !getvar(p,&o,len,&c)) return;
			ackel = 1; break;
		case 0x05:                                     /* STOP_SENDING */
			if (!getvar(p,&o,len,&a) || !getvar(p,&o,len,&b)) return;
			ackel = 1; break;
		case 0x06: {                                   /* CRYPTO */
			uint64_t off, clen;
			if (!getvar(p,&o,len,&off) || !getvar(p,&o,len,&clen)) return;
			if (o + clen > len) return;
			crypto_recv(space, off, p + o, (uint32_t)clen);
			o += (uint32_t)clen; ackel = 1; break; }
		case 0x07:                                     /* NEW_TOKEN */
			if (!getvar(p,&o,len,&a)) return;
			o += (uint32_t)a; ackel = 1; break;
		case 0x08: case 0x09: case 0x0a: case 0x0b:    /* STREAM */
		case 0x0c: case 0x0d: case 0x0e: case 0x0f: {
			uint64_t sid, soff = 0, slen;
			if (!getvar(p,&o,len,&sid)) return;
			if ((t & 0x04) && !getvar(p,&o,len,&soff)) return;
			if (t & 0x02) { if (!getvar(p,&o,len,&slen)) return; }
			else slen = len - o;
			if (o + slen > len) return;
			stream_recv(sid, soff, p + o, (uint32_t)slen, (int)(t & 0x01));
			o += (uint32_t)slen; ackel = 1; break; }
		case 0x10:                                     /* MAX_DATA */
			{ if (!getvar(p,&o,len,&a)) return; } ackel = 1; break;
		case 0x11:                                     /* MAX_STREAM_DATA */
			if (!getvar(p,&o,len,&a) || !getvar(p,&o,len,&b)) return;
			ackel = 1; break;
		case 0x12: case 0x13:                          /* MAX_STREAMS */
			{ if (!getvar(p,&o,len,&a)) return; } ackel = 1; break;
		case 0x14:                                     /* DATA_BLOCKED */
			{ if (!getvar(p,&o,len,&a)) return; } ackel = 1; break;
		case 0x15:                                     /* STREAM_DATA_BLOCKED */
			if (!getvar(p,&o,len,&a) || !getvar(p,&o,len,&b)) return;
			ackel = 1; break;
		case 0x16: case 0x17:                          /* STREAMS_BLOCKED */
			{ if (!getvar(p,&o,len,&a)) return; } ackel = 1; break;
		case 0x18:                                     /* NEW_CONNECTION_ID */
			if (!getvar(p,&o,len,&a) || !getvar(p,&o,len,&b)) return;
			if (o >= len) return;
			o += 1 + p[o];                             /* len byte + CID */
			o += 16;                                   /* stateless reset token */
			ackel = 1; break;
		case 0x19:                                     /* RETIRE_CONNECTION_ID */
			{ if (!getvar(p,&o,len,&a)) return; } ackel = 1; break;
		case 0x1a: o += 8; ackel = 1; break;           /* PATH_CHALLENGE */
		case 0x1b: o += 8; ackel = 1; break;           /* PATH_RESPONSE */
		case 0x1c:                                     /* CONNECTION_CLOSE (transport) */
			if (!getvar(p,&o,len,&a) || !getvar(p,&o,len,&b) ||
			    !getvar(p,&o,len,&c)) return;
			o += (uint32_t)c; closed = 1; return;
		case 0x1d:                                     /* CONNECTION_CLOSE (app) */
			if (!getvar(p,&o,len,&a) || !getvar(p,&o,len,&b)) return;
			o += (uint32_t)b; closed = 1; return;
		case 0x1e: ackel = 1; hs_confirmed = 1; break; /* HANDSHAKE_DONE */
		default:
			return;                                    /* unknown: stop */
		}
	}
	if (ackel)
		sp[space].ackp = 1;
}

/* -------------------------------------------------- packet protection */

static void hp_compute(const uint8_t hp[16], const uint8_t sample[16], uint8_t mask[16])
{
	struct aes128 a;
	aes128_init(&a, hp);
	aes128_encrypt_block(&a, sample, mask);
}

/* Reconstruct a full packet number from a truncated one (RFC 9000 §A.3). */
static uint64_t decode_pn(uint64_t trunc, int pnlen, uint64_t largest, int have)
{
	if (!have)
		return trunc;
	uint64_t expected = largest + 1;
	uint64_t win = (uint64_t)1 << (pnlen * 8);
	uint64_t hwin = win / 2;
	uint64_t mask = win - 1;
	uint64_t cand = (expected & ~mask) | trunc;
	if (cand + hwin <= expected && cand + win < ((uint64_t)1 << 62))
		return cand + win;
	if (cand > expected + hwin && cand >= win)
		return cand - win;
	return cand;
}

/* Decrypt one received packet in `space`. `pkt0` is the offset of the first
 * header byte in d; `pn_off` the packet-number field offset; `plen` the bytes
 * from pn_off to the packet end (pn + payload + tag). */
static int decrypt_pkt(int space, uint8_t *d, uint32_t pkt0, uint32_t pn_off,
                       uint32_t plen, int is_long)
{
	struct space *s = &sp[space];
	if (!s->hk || plen < 4 + 16)
		return -1;
	uint8_t mask[16];
	hp_compute(s->sh, d + pn_off + 4, mask);

	uint8_t b0 = d[pkt0] ^ (is_long ? (mask[0] & 0x0f) : (mask[0] & 0x1f));
	int pnlen = (b0 & 0x03) + 1;
	uint64_t trunc = 0;
	for (int i = 0; i < pnlen; i++)
		trunc = (trunc << 8) | (d[pn_off + i] ^ mask[1 + i]);
	uint64_t pn = decode_pn(trunc, pnlen, s->lrec, s->hrec);

	uint8_t aad[64];
	uint32_t al = 0;
	for (uint32_t i = pkt0; i < pn_off; i++)
		aad[al++] = d[i];
	aad[0] = b0;
	for (int i = 0; i < pnlen; i++)
		aad[al++] = (uint8_t)(trunc >> (8 * (pnlen - 1 - i)));

	uint32_t ct_off = pn_off + pnlen;
	uint32_t ct_len = plen - pnlen - 16;
	const uint8_t *tag = d + ct_off + ct_len;
	uint8_t nonce[12];
	memcpy(nonce, s->si, 12);
	for (int i = 0; i < 8; i++)
		nonce[11 - i] ^= (uint8_t)(pn >> (8 * i));

	static uint8_t pt[2048];
	if (ct_len > sizeof pt)
		return -1;
	if (aes128_gcm_open(s->sk, nonce, aad, al, d + ct_off, ct_len, pt, tag) != 0)
		return -1;

	if (!s->hrec || pn > s->lrec)
		s->lrec = pn;
	s->hrec = 1;
	process_frames(space, pt, ct_len);
	return 0;
}

static void handle_retry(const uint8_t *retry_scid, int sl, const uint8_t *p, uint32_t plen)
{
	if (did_retry)
		return;
	did_retry = 1;
	/* Retry layout after the SCID: Retry Token .. then 16-byte integrity tag. */
	if (plen < 16)
		return;
	uint32_t tl = plen - 16;
	if (tl > sizeof retry_token)
		tl = sizeof retry_token;
	memcpy(retry_token, p, tl);
	retry_token_len = (int)tl;
	if (sl > (int)sizeof dcid) sl = sizeof dcid;
	memcpy(dcid, retry_scid, sl);
	dcid_len = sl;
	server_cid_set = 0;                         /* adopt the real Initial's SCID next */
	derive_initial_keys(dcid, dcid_len);        /* new DCID => new Initial keys */
}

/* --------------------------------------------------------- packet send */

/* Encrypt+send `frames` as a single packet (its own datagram) in `space`. */
static int emit(int space, const uint8_t *frames, uint32_t flen)
{
	struct space *s = &sp[space];
	if (!s->hk)
		return -1;
	/* static (the connection is single-flow): keeps emit's ~5 KB of packet
	 * scratch off the 16 KiB user stack, which -O0 + deep call chains overflow. */
	static uint8_t out[2048];
	static uint8_t pt[1600];
	uint32_t p = 0;
	int is_long = (space != S_APP);
	uint32_t pn_off;

	if (is_long) {
		int type = (space == S_INIT) ? 0 : 2;
		out[p++] = (uint8_t)(0xC0 | (type << 4) | 0x03);
		out[p++] = 0; out[p++] = 0; out[p++] = 0; out[p++] = 1;   /* version 1 */
		out[p++] = (uint8_t)dcid_len; memcpy(out + p, dcid, dcid_len); p += dcid_len;
		out[p++] = (uint8_t)scid_len; memcpy(out + p, scid, scid_len); p += scid_len;
		if (space == S_INIT) {
			p += putvar(out + p, (uint64_t)retry_token_len);
			if (retry_token_len) { memcpy(out + p, retry_token, retry_token_len); p += retry_token_len; }
		}
		uint32_t len_idx = p; p += 2;                  /* 2-byte varint length */
		pn_off = p;
		uint64_t pn = s->spn;
		out[p++] = (uint8_t)(pn >> 24); out[p++] = (uint8_t)(pn >> 16);
		out[p++] = (uint8_t)(pn >> 8);  out[p++] = (uint8_t)pn;

		uint32_t ptl = flen;
		memcpy(pt, frames, flen);
		if (space == S_INIT) {                         /* pad datagram to 1200 */
			uint32_t projected = pn_off + 4 + ptl + 16;
			while (projected < 1200 && ptl < sizeof pt) { pt[ptl++] = 0x00; projected++; }
		}
		uint32_t lenval = 4 + ptl + 16;
		out[len_idx] = 0x40 | (uint8_t)(lenval >> 8);
		out[len_idx + 1] = (uint8_t)lenval;

		uint8_t nonce[12];
		memcpy(nonce, s->ci, 12);
		for (int i = 0; i < 8; i++)
			nonce[11 - i] ^= (uint8_t)(pn >> (8 * i));
		uint8_t tag[16];
		aes128_gcm_seal(s->ck, nonce, out, pn_off + 4, pt, ptl, out + pn_off + 4, tag);
		memcpy(out + pn_off + 4 + ptl, tag, 16);
		uint32_t dglen = pn_off + 4 + ptl + 16;

		uint8_t mask[16];
		hp_compute(s->ch, out + pn_off + 4, mask);
		out[0] ^= mask[0] & 0x0f;
		for (int i = 0; i < 4; i++) out[pn_off + i] ^= mask[1 + i];
		s->spn++;
		return net_send(sock, out, (int)dglen) < 0 ? -1 : 0;
	}

	out[p++] = 0x43;                                   /* short header, pnlen 4 */
	memcpy(out + p, dcid, dcid_len); p += dcid_len;
	pn_off = p;
	uint64_t pn = s->spn;
	out[p++] = (uint8_t)(pn >> 24); out[p++] = (uint8_t)(pn >> 16);
	out[p++] = (uint8_t)(pn >> 8);  out[p++] = (uint8_t)pn;

	uint32_t ptl = flen;
	memcpy(pt, frames, flen);
	uint8_t nonce[12];
	memcpy(nonce, s->ci, 12);
	for (int i = 0; i < 8; i++)
		nonce[11 - i] ^= (uint8_t)(pn >> (8 * i));
	uint8_t tag[16];
	aes128_gcm_seal(s->ck, nonce, out, pn_off + 4, pt, ptl, out + pn_off + 4, tag);
	memcpy(out + pn_off + 4 + ptl, tag, 16);
	uint32_t dglen = pn_off + 4 + ptl + 16;

	uint8_t mask[16];
	hp_compute(s->ch, out + pn_off + 4, mask);
	out[0] ^= mask[0] & 0x1f;
	for (int i = 0; i < 4; i++) out[pn_off + i] ^= mask[1 + i];
	s->spn++;
	return net_send(sock, out, (int)dglen) < 0 ? -1 : 0;
}

static void send_ack(int space)
{
	if (!sp[space].hk || !sp[space].hrec)
		return;
	uint8_t f[24];
	uint32_t p = 0;
	f[p++] = 0x02;
	p += putvar(f + p, sp[space].lrec);        /* Largest Acknowledged */
	p += putvar(f + p, 0);                      /* ACK Delay */
	p += putvar(f + p, 0);                      /* ACK Range Count */
	p += putvar(f + p, sp[space].lrec);         /* First ACK Range = [0..largest] */
	emit(space, f, p);
	sp[space].ackp = 0;
}

static void send_pending_acks(void)
{
	for (int s = 0; s < 3; s++)
		if (sp[s].ackp)
			send_ack(s);
}

/* Send a CRYPTO frame carrying `data` at offset 0 in `space`. */
static int send_crypto(int space, const uint8_t *data, uint32_t len)
{
	static uint8_t f[1200];
	uint32_t p = 0;
	f[p++] = 0x06;
	p += putvar(f + p, 0);
	p += putvar(f + p, len);
	memcpy(f + p, data, len); p += len;
	return emit(space, f, p);
}

/* --------------------------------------------------- datagram processing */

static int process_datagram(uint8_t *d, uint32_t dlen)
{
	uint32_t off = 0;
	while (off < dlen) {
		uint8_t b0 = d[off];
		if (!(b0 & 0x40))                       /* fixed bit clear: junk */
			break;
		if (b0 & 0x80) {                        /* long header */
			if (off + 7 > dlen) break;
			uint32_t ver = ((uint32_t)d[off+1] << 24) | ((uint32_t)d[off+2] << 16) |
			               ((uint32_t)d[off+3] << 8) | d[off+4];
			int type = (b0 >> 4) & 0x03;
			uint32_t q = off + 5;
			uint8_t dl = d[q++]; q += dl;       /* DCID (ours, ignore) */
			if (q >= dlen) break;
			uint8_t sl = d[q++];
			uint8_t *scid_p = d + q; q += sl;
			if (q > dlen) break;
			if (ver == 0)                       /* Version Negotiation */
				{ closed = 1; return -1; }
			if (type == 3) {                    /* Retry */
				handle_retry(scid_p, sl, d + q, dlen - q);
				return 1;
			}
			if (!server_cid_set && sl <= sizeof dcid) {
				memcpy(dcid, scid_p, sl); dcid_len = sl; server_cid_set = 1;
			}
			if (type == 0) {                    /* Initial: skip token */
				uint64_t tl;
				if (!getvar(d, &q, dlen, &tl)) break;
				q += (uint32_t)tl;
			} else if (type != 2) {             /* 0-RTT: not expected */
				break;
			}
			uint64_t length;
			if (!getvar(d, &q, dlen, &length)) break;
			uint32_t pn_off = q;
			if (pn_off + length > dlen) break;
			int space = (type == 0) ? S_INIT : S_HS;
			decrypt_pkt(space, d, off, pn_off, (uint32_t)length, 1);
			off = pn_off + (uint32_t)length;
		} else {                                /* short header (1-RTT) */
			uint32_t pn_off = off + 1 + scid_len;
			if (pn_off + 4 + 16 > dlen) break;
			decrypt_pkt(S_APP, d, off, pn_off, dlen - pn_off, 0);
			off = dlen;
		}
	}
	return closed ? -1 : 0;
}

/* Read one datagram, process it, and flush owed ACKs. 1 processed / 0 idle /
 * QUIC_EINTR / QUIC_ECLOSED. */
static int poll_once(void)
{
	static uint8_t dg[2048];
	int n = net_recv(sock, dg, (int)sizeof dg);
	if (n == NET_EINTR)
		return QUIC_EINTR;
	if (n <= 0)
		return 0;
	int r = process_datagram(dg, (uint32_t)n);
	send_pending_acks();
	if (r < 0 || closed)
		return QUIC_ECLOSED;
	return 1;
}

/* ---------------------------------------------------------- public API */

int quic_connect(const char *host, uint16_t port, const char *alpn)
{
	memset(sp, 0, sizeof sp);
	memset(qs, 0, sizeof qs);
	closed = server_cid_set = did_retry = 0;
	got_sh = app_done = hs_sent_fin = hs_confirmed = 0;
	retry_token_len = 0; pending_fin_len = 0;

	sock = net_dial(host, port, SK_UDP);   /* resolve to IP; SNI stays `host` below */
	if (sock < 0)
		return sock == NET_EINTR ? QUIC_EINTR :
		       sock == -2 ? QUIC_ERESOLVE : QUIC_ESOCK;

	scid_len = 8; gen_random(scid, 8);
	dcid_len = 8; gen_random(dcid, 8);
	derive_initial_keys(dcid, dcid_len);

	sha256_init(&transcript);
	gen_random(our_priv, 32);
	x25519(our_pub, our_priv, x25519_base);

	chello_len = build_client_hello(chello, host, alpn);
	sha256_update(&transcript, chello, chello_len);
	if (send_crypto(S_INIT, chello, chello_len) < 0)
		{ net_close(sock); return QUIC_ESOCK; }

	int tries = 0, last_retry = did_retry;
	while (!(app_done && hs_sent_fin)) {
		int r = poll_once();
		if (r == QUIC_EINTR) { net_close(sock); return QUIC_EINTR; }
		if (r == QUIC_ECLOSED) { net_close(sock); return QUIC_EHANDSHAKE; }
		if (did_retry != last_retry) {          /* Retry: resend Initial CH */
			last_retry = did_retry;
			tries = 0;
			send_crypto(S_INIT, chello, chello_len);
			continue;
		}
		if (r == 0) {                           /* idle: retransmit / nudge */
			if (++tries > 8) { net_close(sock); return QUIC_EHANDSHAKE; }
			if (!got_sh)
				send_crypto(S_INIT, chello, chello_len);
			else {
				sp[S_INIT].ackp = sp[S_INIT].hrec;
				sp[S_HS].ackp = sp[S_HS].hrec;
				send_pending_acks();
			}
			continue;
		}
		tries = 0;
		if (app_done && !hs_sent_fin) {         /* send client Finished */
			send_crypto(S_HS, pending_fin, (uint32_t)pending_fin_len);
			hs_sent_fin = 1;
		}
	}
	return QUIC_OK;
}

int quic_send(uint64_t sid, const void *data, uint32_t len, int fin)
{
	struct qstream *q = get_stream(sid);
	if (!q || !sp[S_APP].hk)
		return -1;
	const uint8_t *p = data;
	uint32_t done = 0;
	do {
		uint32_t chunk = len - done;
		if (chunk > 1100) chunk = 1100;
		static uint8_t f[1200];
		uint32_t fp = 0;
		uint8_t type = 0x08 | 0x04 | 0x02;      /* OFF + LEN */
		if (fin && done + chunk == len) type |= 0x01;
		f[fp++] = type;
		fp += putvar(f + fp, sid);
		fp += putvar(f + fp, q->send_off + done);
		fp += putvar(f + fp, chunk);
		memcpy(f + fp, p + done, chunk); fp += chunk;
		if (emit(S_APP, f, fp) < 0)
			return -1;
		done += chunk;
	} while (done < len);
	q->send_off += len;
	return 0;
}

int quic_read(uint64_t sid, void *buf, uint32_t max, int *fin)
{
	struct qstream *q = get_stream(sid);
	if (!q) { if (fin) *fin = 0; return 0; }
	uint32_t avail = q->r_len - q->r_pos;
	uint32_t n = avail < max ? avail : max;
	memcpy(buf, q->buf + q->r_pos, n);
	q->r_pos += n;
	if (q->r_pos == q->r_len)
		q->r_pos = q->r_len = 0;
	if (fin)
		*fin = (q->fin && q->recv_off >= q->fin_off && q->r_pos == q->r_len) ? 1 : 0;
	return (int)n;
}

int quic_poll(void)
{
	return poll_once();
}

void quic_close(void)
{
	if (sp[S_APP].hk && !closed) {
		uint8_t f[4];
		uint32_t p = 0;
		f[p++] = 0x1c;                          /* CONNECTION_CLOSE (transport) */
		p += putvar(f + p, 0);                   /* error = NO_ERROR */
		p += putvar(f + p, 0);                   /* frame type */
		p += putvar(f + p, 0);                   /* reason length */
		emit(S_APP, f, p);
	}
	net_close(sock);
}
