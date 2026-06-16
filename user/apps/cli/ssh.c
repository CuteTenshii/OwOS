/* ssh.c — a from-scratch SSH-2 client, entirely in user space.
 *
 *   ssh user@host [port]
 *
 * Opens a raw TCP socket (net_connect SK_TCP) and speaks SSH-2 over it: the
 * curve25519-sha256 key exchange, the aes128-gcm@openssh.com AEAD transport
 * cipher, password authentication, then a "session" channel with a pty and an
 * interactive remote shell. It is the SSH analogue of curl: the protocol lives
 * here, over the same net_* / stdio primitives every other CLI tool uses.
 *
 * Pragmatic scope, in the spirit of the kernel's TLS stack:
 *   - One cipher suite only: curve25519-sha256 KEX + aes128-gcm@openssh.com.
 *     Both reuse the user crypto lib (X25519 + SHA-256 + AES-128-GCM) that
 *     already backs the QUIC/TLS client, so no new primitives are needed.
 *   - The server host key signature is NOT verified (same deliberate trade-off
 *     as tls.c: resists passive eavesdropping, not an active MITM). We still
 *     hash the host key into the exchange hash, we just don't check its
 *     signature -- verifying ed25519/RSA would need SHA-512/bignum we don't have.
 *   - Password auth only (prompted, no echo). One connection, in-order delivery.
 *
 * The terminal runs ssh like a pty: it captures our stdout (rendered inline) and
 * wires our stdin to a pipe it feeds with the window's keystrokes (spawn_cap_io
 * + render_child interactive mode). So fd 0 is the keyboard and fd 1 is the
 * screen, exactly as an SSH client expects.
 */

#include "owos.h"
#include "crypto.h"

/* ---- SSH-2 message numbers (RFC 4253 / 4252 / 4254) ---------------------- */
#define MSG_DISCONNECT       1
#define MSG_IGNORE           2
#define MSG_UNIMPLEMENTED    3
#define MSG_DEBUG            4
#define MSG_SERVICE_REQUEST  5
#define MSG_SERVICE_ACCEPT   6
#define MSG_EXT_INFO         7
#define MSG_KEXINIT          20
#define MSG_NEWKEYS          21
#define MSG_KEX_ECDH_INIT    30
#define MSG_KEX_ECDH_REPLY   31
#define MSG_USERAUTH_REQUEST 50
#define MSG_USERAUTH_FAILURE 51
#define MSG_USERAUTH_SUCCESS 52
#define MSG_USERAUTH_BANNER  53
#define MSG_GLOBAL_REQUEST   80
#define MSG_REQUEST_SUCCESS  81
#define MSG_REQUEST_FAILURE  82
#define MSG_CHANNEL_OPEN              90
#define MSG_CHANNEL_OPEN_CONFIRMATION 91
#define MSG_CHANNEL_OPEN_FAILURE      92
#define MSG_CHANNEL_WINDOW_ADJUST     93
#define MSG_CHANNEL_DATA              94
#define MSG_CHANNEL_EXTENDED_DATA     95
#define MSG_CHANNEL_EOF               96
#define MSG_CHANNEL_CLOSE             97
#define MSG_CHANNEL_REQUEST           98
#define MSG_CHANNEL_SUCCESS           99
#define MSG_CHANNEL_FAILURE           100

#define CLIENT_IDENT "SSH-2.0-OwOS_1.0"

#define MAXPKT       40000            /* largest SSH packet we'll accept/build */
#define LOCAL_WINDOW 0x100000u        /* flow-control window we advertise      */
#define LOCAL_MAXPKT 32768u           /* max channel-data payload we accept    */

/* ---- session state ------------------------------------------------------- */

static int  sock = -1;
static int  sock_dead;

static unsigned char rxbuf[49152];    /* raw bytes pulled off the socket       */
static int           rxlen;

static unsigned char payload[MAXPKT]; /* the decoded payload of one packet     */
static unsigned char ptext[MAXPKT];   /* recv GCM plaintext / caller build scratch */
static unsigned char sealbuf[MAXPKT]; /* send GCM plaintext (MUST be distinct from
                                       * ptext: callers build the payload in ptext
                                       * and pass it as send_packet's `pl`, so reusing
                                       * ptext as the seal scratch would memcpy-overlap
                                       * and corrupt the message). */
static unsigned char txbuf[MAXPKT + 64];

static int send_encrypted, recv_encrypted;
static unsigned char send_key[16], recv_key[16];
static unsigned char send_iv[12],  recv_iv[12];

/* Key-exchange artefacts kept for the exchange hash + key derivation. */
static unsigned char H[32], session_id[32];
static unsigned char kmp[40]; static int kmp_len;   /* mpint(shared secret) */

/* ---- little helpers ------------------------------------------------------ */

static void wb(unsigned char *d, int *p, unsigned v)        { d[(*p)++] = (unsigned char)v; }
static void w32(unsigned char *d, int *p, unsigned v)
{
	d[(*p)++] = (unsigned char)(v >> 24); d[(*p)++] = (unsigned char)(v >> 16);
	d[(*p)++] = (unsigned char)(v >> 8);  d[(*p)++] = (unsigned char)v;
}
static void wbytes(unsigned char *d, int *p, const void *s, int n)
{
	memcpy(d + *p, s, (unsigned)n); *p += n;
}
static void wstr(unsigned char *d, int *p, const void *s, int n) { w32(d, p, (unsigned)n); wbytes(d, p, s, n); }
static void wcstr(unsigned char *d, int *p, const char *s)       { wstr(d, p, s, strlen(s)); }

static unsigned r32(const unsigned char *d, int *p)
{
	unsigned v = ((unsigned)d[*p] << 24) | ((unsigned)d[*p + 1] << 16) |
	             ((unsigned)d[*p + 2] << 8) | (unsigned)d[*p + 3];
	*p += 4;
	return v;
}

/* Weak best-effort randomness (cookie, ephemeral key, padding). Not a real
 * entropy source -- the same compromise the QUIC/TLS user client makes. */
static void gen_random(unsigned char *out, int n)
{
	static unsigned ctr;
	int got = 0;
	while (got < n) {
		unsigned a = msec(), b = uptime(), c = now_hms(), d = ctr++;
		unsigned char seed[16];
		int sp = 0;
		w32(seed, &sp, a); w32(seed, &sp, b); w32(seed, &sp, c); w32(seed, &sp, d);
		unsigned char h[32];
		sha256(seed, 16, h);
		int chunk = (n - got) < 32 ? (n - got) : 32;
		memcpy(out + got, h, (unsigned)chunk);
		got += chunk;
	}
}

static void incr_iv(unsigned char *iv)        /* the GCM invocation counter */
{
	for (int i = 11; i >= 4; i--)
		if (++iv[i] != 0)
			break;
}

static void say(const char *s) { write(1, s, strlen(s)); }

/* ---- socket byte stream -------------------------------------------------- */

static int sock_send_all(const unsigned char *b, int n)
{
	int off = 0;
	while (off < n) {
		int w = net_send(sock, b + off, n - off);
		if (w <= 0) { sock_dead = 1; return -1; }
		off += w;
	}
	return 0;
}

/* Pull whatever is available off the socket into rxbuf (non-blocking).
 * Returns bytes added, 0 would-block, -1 on EOF/error. */
static int rx_pump(void)
{
	if (rxlen >= (int)sizeof rxbuf)
		return 0;
	int n = net_recv_nb(sock, rxbuf + rxlen, (int)sizeof rxbuf - rxlen);
	if (n > 0) { rxlen += n; return n; }
	if (n == 0) return 0;
	sock_dead = 1;
	return -1;
}

static void rx_consume(int n)
{
	if (n >= rxlen) { rxlen = 0; return; }
	memmove(rxbuf, rxbuf + n, (unsigned)(rxlen - n));
	rxlen -= n;
}

/* ---- packet framing ------------------------------------------------------ */

/* Send one SSH packet carrying `pl[0..plen)` as its payload. Adds padding to a
 * cipher-block multiple and, once keys are installed, GCM-seals it. */
static int send_packet(const unsigned char *pl, int plen)
{
	/* Padding makes a length multiple. RFC 4253 (cleartext "none" cipher, block
	 * 8): the multiple covers packet_length(4) + padding_length(1) + payload +
	 * padding -- the 4-byte length field is included. RFC 5647 (aes128-gcm, block
	 * 16): the length field is sent in clear and NOT counted, so only
	 * padding_length + payload + padding must be a multiple of 16. Getting the
	 * cleartext +4 wrong makes (packlen+4)%8 != 0, which OpenSSH rejects (it RSTs
	 * the connection without a DISCONNECT). Padding is at least 4 bytes. */
	int blk = send_encrypted ? 16 : 8;
	int lenfield = send_encrypted ? 0 : 4;
	int padlen = blk - ((lenfield + 1 + plen) % blk);
	if (padlen < 4) padlen += blk;
	unsigned pktlen = (unsigned)(1 + plen + padlen);
	int pos = 0;

	if (!send_encrypted) {
		w32(txbuf, &pos, pktlen);
		wb(txbuf, &pos, (unsigned)padlen);
		wbytes(txbuf, &pos, pl, plen);
		gen_random(txbuf + pos, padlen); pos += padlen;
		return sock_send_all(txbuf, pos);
	}

	/* aes128-gcm@openssh.com: the length is sent in clear and authenticated as
	 * AAD; padding_length||payload||padding is encrypted; a 16-byte tag follows. */
	unsigned char aad[4]; int ap = 0; w32(aad, &ap, pktlen);
	int pp = 0;
	wb(sealbuf, &pp, (unsigned)padlen);
	wbytes(sealbuf, &pp, pl, plen);                        /* pl may be `ptext` */
	gen_random(sealbuf + pp, padlen); pp += padlen;        /* pp == pktlen */

	w32(txbuf, &pos, pktlen);
	unsigned char tag[16];
	aes128_gcm_seal(send_key, send_iv, aad, 4, sealbuf, (unsigned)pp, txbuf + pos, tag);
	pos += pp;
	memcpy(txbuf + pos, tag, 16); pos += 16;
	incr_iv(send_iv);
	return sock_send_all(txbuf, pos);
}

/* Try to decode one packet from rxbuf without blocking.
 * 1 = decoded into payload (+*plen) and consumed; 0 = need more bytes; -1 err. */
static int parse_one(int *plen)
{
	if (rxlen < 4)
		return 0;
	unsigned pktlen = ((unsigned)rxbuf[0] << 24) | ((unsigned)rxbuf[1] << 16) |
	                  ((unsigned)rxbuf[2] << 8) | (unsigned)rxbuf[3];
	if (pktlen < 8 || pktlen > MAXPKT)
		return -1;

	if (!recv_encrypted) {
		if (4 + (int)pktlen > rxlen)
			return 0;
		int padlen = rxbuf[4];
		int payl = (int)pktlen - padlen - 1;
		if (payl < 0 || payl > MAXPKT)
			return -1;
		memcpy(payload, rxbuf + 5, (unsigned)payl);
		*plen = payl;
		rx_consume(4 + (int)pktlen);
		return 1;
	}

	if (4 + (int)pktlen + 16 > rxlen)
		return 0;
	unsigned char aad[4] = { rxbuf[0], rxbuf[1], rxbuf[2], rxbuf[3] };
	if (aes128_gcm_open(recv_key, recv_iv, aad, 4, rxbuf + 4, pktlen,
	                    ptext, rxbuf + 4 + pktlen) != 0)
		return -1;
	incr_iv(recv_iv);
	int padlen = ptext[0];
	int payl = (int)pktlen - padlen - 1;
	if (payl < 0 || payl > MAXPKT)
		return -1;
	memcpy(payload, ptext + 1, (unsigned)payl);
	*plen = payl;
	rx_consume(4 + (int)pktlen + 16);
	return 1;
}

/* Block (up to timeout_ms; -1 = forever) for one packet. 0 = ok, -1 = fail. */
static int read_packet(int *plen, int timeout_ms)
{
	unsigned start = msec();
	for (;;) {
		int r = parse_one(plen);
		if (r == 1) return 0;
		if (r < 0)  return -1;
		int p = rx_pump();
		if (p < 0)  return -1;
		if (p == 0) {
			if (timeout_ms >= 0 && (int)(msec() - start) > (unsigned)timeout_ms)
				return -1;
			msleep(3);
		} else {
			start = msec();
		}
	}
}

static void print_disconnect(int plen)
{
	int p = 1;
	if (plen >= 9) {
		r32(payload, &p);                       /* reason code */
		unsigned n = r32(payload, &p);
		say("ssh: server disconnected: ");
		if (n < (unsigned)(plen - p)) write(1, payload + p, (int)n);
		say("\n");
	} else {
		say("ssh: server disconnected\n");
	}
}

/* Read the next "interesting" packet, transparently skipping transport noise
 * (IGNORE/DEBUG/UNIMPLEMENTED) and reporting DISCONNECT. Returns the message
 * type (>0), or a negative on error. */
static int recv_msg(int *plen, int timeout_ms)
{
	for (;;) {
		if (read_packet(plen, timeout_ms) != 0)
			return -1;
		int t = payload[0];
		if (t == MSG_IGNORE || t == MSG_DEBUG || t == MSG_UNIMPLEMENTED || t == MSG_EXT_INFO)
			continue;
		if (t == MSG_DISCONNECT) { print_disconnect(*plen); return -1; }
		if (t == MSG_GLOBAL_REQUEST) {           /* e.g. OpenSSH hostkeys-00@... */
			int p = 1; unsigned n = r32(payload, &p);
			int wr = (p + (int)n < *plen) ? payload[p + (int)n] : 0;
			if (wr) { unsigned char f = MSG_REQUEST_FAILURE; send_packet(&f, 1); }
			continue;
		}
		return t;
	}
}

/* ---- version + key exchange ---------------------------------------------- */

static char v_s[256];                     /* server identification line        */
static unsigned char ic[2048]; static int ic_len;   /* our KEXINIT payload (I_C) */
static unsigned char is_buf[8192]; static int is_len; /* server KEXINIT (I_S)    */

static int read_ident(void)
{
	unsigned start = msec();
	for (;;) {
		int nl = -1;
		for (int i = 0; i < rxlen; i++)
			if (rxbuf[i] == '\n') { nl = i; break; }
		if (nl >= 0) {
			int len = nl;
			if (len > 0 && rxbuf[len - 1] == '\r') len--;
			if (len >= 4 && rxbuf[0] == 'S' && rxbuf[1] == 'S' &&
			    rxbuf[2] == 'H' && rxbuf[3] == '-') {
				int c = len < (int)sizeof v_s - 1 ? len : (int)sizeof v_s - 1;
				memcpy(v_s, rxbuf, (unsigned)c); v_s[c] = '\0';
				rx_consume(nl + 1);
				return 0;
			}
			rx_consume(nl + 1);                 /* a pre-banner line: ignore */
			continue;
		}
		int p = rx_pump();
		if (p < 0) return -1;
		if (p == 0) {
			if ((int)(msec() - start) > 15000) return -1;
			msleep(3);
		} else {
			start = msec();
		}
	}
}

/* Substring search over a byte span (for sanity-checking the server's offers). */
static int span_has(const unsigned char *hay, int n, const char *needle)
{
	int m = strlen(needle);
	for (int i = 0; i + m <= n; i++) {
		int k = 0;
		while (k < m && hay[i + k] == (unsigned char)needle[k]) k++;
		if (k == m) return 1;
	}
	return 0;
}

static void build_kexinit(void)
{
	int p = 0;
	wb(ic, &p, MSG_KEXINIT);
	unsigned char cookie[16]; gen_random(cookie, 16);
	wbytes(ic, &p, cookie, 16);
	wcstr(ic, &p, "curve25519-sha256,curve25519-sha256@libssh.org"); /* kex */
	wcstr(ic, &p, "ssh-ed25519,rsa-sha2-256,rsa-sha2-512,ssh-rsa");  /* host key */
	wcstr(ic, &p, "aes128-gcm@openssh.com");      /* enc c->s */
	wcstr(ic, &p, "aes128-gcm@openssh.com");      /* enc s->c */
	wcstr(ic, &p, "hmac-sha2-256");               /* mac c->s (unused: AEAD) */
	wcstr(ic, &p, "hmac-sha2-256");               /* mac s->c (unused: AEAD) */
	wcstr(ic, &p, "none");                         /* comp c->s */
	wcstr(ic, &p, "none");                         /* comp s->c */
	wcstr(ic, &p, "");                             /* lang c->s */
	wcstr(ic, &p, "");                             /* lang s->c */
	wb(ic, &p, 0);                                 /* first_kex_packet_follows */
	w32(ic, &p, 0);                                /* reserved */
	ic_len = p;
}

/* HASH(K || H || letter || session_id), extended to `outlen` bytes (RFC 4253). */
static void derive(unsigned char letter, unsigned char *out, int outlen)
{
	struct sha256 s;
	unsigned char block[32];
	sha256_init(&s);
	sha256_update(&s, kmp, (unsigned)kmp_len);
	sha256_update(&s, H, 32);
	sha256_update(&s, &letter, 1);
	sha256_update(&s, session_id, 32);
	sha256_final(&s, block);
	int got = 0;
	for (;;) {
		int c = (outlen - got) < 32 ? (outlen - got) : 32;
		memcpy(out + got, block, (unsigned)c);
		got += c;
		if (got >= outlen) break;
		sha256_init(&s);
		sha256_update(&s, kmp, (unsigned)kmp_len);
		sha256_update(&s, H, 32);
		sha256_update(&s, out, (unsigned)got);
		sha256_final(&s, block);
	}
}

/* Run the curve25519-sha256 exchange. 0 on success, -1 on failure. */
static int do_kex(const char *cident)
{
	/* Both sides send KEXINIT; we can fire ours immediately. */
	build_kexinit();
	if (send_packet(ic, ic_len) != 0) return -1;

	int plen;
	if (recv_msg(&plen, 15000) != MSG_KEXINIT) { say("ssh: expected KEXINIT\n"); return -1; }
	if (plen > (int)sizeof is_buf) { say("ssh: KEXINIT too large\n"); return -1; }
	memcpy(is_buf, payload, (unsigned)plen); is_len = plen;

	if (!span_has(is_buf, is_len, "curve25519-sha256")) {
		say("ssh: server does not offer curve25519-sha256 key exchange\n");
		return -1;
	}
	if (!span_has(is_buf, is_len, "aes128-gcm@openssh.com")) {
		say("ssh: server does not offer aes128-gcm@openssh.com cipher\n");
		return -1;
	}

	/* Our ephemeral X25519 key pair. */
	unsigned char priv[32], qc[32];
	gen_random(priv, 32);
	priv[0] &= 248; priv[31] &= 127; priv[31] |= 64;       /* clamp (idempotent) */
	static const unsigned char base[32] = { 9 };
	x25519(qc, priv, base);

	int p = 0;
	wb(ptext, &p, MSG_KEX_ECDH_INIT);
	wstr(ptext, &p, qc, 32);
	if (send_packet(ptext, p) != 0) return -1;

	if (recv_msg(&plen, 15000) != MSG_KEX_ECDH_REPLY) {
		say("ssh: key exchange failed\n");
		return -1;
	}
	/* KEX_ECDH_REPLY: string K_S, string Q_S, string signature. */
	p = 1;
	unsigned ks_len = r32(payload, &p);
	int ks_off = p;
	if (ks_off + (int)ks_len + 4 > plen) { say("ssh: malformed reply\n"); return -1; }
	p += (int)ks_len;
	unsigned qs_len = r32(payload, &p);
	if (qs_len != 32 || p + 32 > plen) { say("ssh: bad server key\n"); return -1; }
	const unsigned char *qs = payload + p;

	/* Shared secret + its mpint encoding (reused in hash + key derivation). */
	unsigned char shared[32];
	x25519(shared, priv, qs);
	{
		int i = 0; while (i < 32 && shared[i] == 0) i++;
		int n = 32 - i;
		int z = (n > 0 && (shared[i] & 0x80)) ? 1 : 0;
		int kp = 0;
		w32(kmp, &kp, (unsigned)(n + z));
		if (z) kmp[kp++] = 0;
		memcpy(kmp + kp, shared + i, (unsigned)n); kp += n;
		kmp_len = kp;
	}

	/* Exchange hash H = SHA256(V_C || V_S || I_C || I_S || K_S || Q_C || Q_S || K).
	 * The host-key signature is intentionally not verified (see file header). */
	{
		struct sha256 s;
		sha256_init(&s);
		unsigned char lp[4]; int z;
		z = 0; w32(lp, &z, strlen(cident));     sha256_update(&s, lp, 4); sha256_update(&s, cident, strlen(cident));
		z = 0; w32(lp, &z, strlen(v_s));         sha256_update(&s, lp, 4); sha256_update(&s, v_s, strlen(v_s));
		z = 0; w32(lp, &z, (unsigned)ic_len);    sha256_update(&s, lp, 4); sha256_update(&s, ic, (unsigned)ic_len);
		z = 0; w32(lp, &z, (unsigned)is_len);    sha256_update(&s, lp, 4); sha256_update(&s, is_buf, (unsigned)is_len);
		z = 0; w32(lp, &z, ks_len);              sha256_update(&s, lp, 4); sha256_update(&s, payload + ks_off, ks_len);
		z = 0; w32(lp, &z, 32);                  sha256_update(&s, lp, 4); sha256_update(&s, qc, 32);
		z = 0; w32(lp, &z, 32);                  sha256_update(&s, lp, 4); sha256_update(&s, qs, 32);
		sha256_update(&s, kmp, (unsigned)kmp_len);   /* K is already mpint-framed */
		sha256_final(&s, H);
	}
	memcpy(session_id, H, 32);                  /* first KEX fixes the session id */

	/* NEWKEYS: install the directional keys. After we send ours, our packets are
	 * encrypted; after we receive the server's, theirs are. */
	derive('A', send_iv, 12);   derive('B', recv_iv, 12);
	derive('C', send_key, 16);  derive('D', recv_key, 16);

	unsigned char nk = MSG_NEWKEYS;
	if (send_packet(&nk, 1) != 0) return -1;
	send_encrypted = 1;

	if (recv_msg(&plen, 15000) != MSG_NEWKEYS) { say("ssh: expected NEWKEYS\n"); return -1; }
	recv_encrypted = 1;
	return 0;
}

/* ---- authentication ------------------------------------------------------ */

/* Read a password from stdin with no echo. Returns length, or -1 if aborted
 * (Ctrl+C) / EOF (window closed). */
static int read_password(char *out, int max)
{
	int len = 0;
	for (;;) {
		char c;
		int n = read_nb(0, &c, 1);
		if (n < 0) return -1;                   /* stdin closed */
		if (n == 0) { msleep(4); continue; }
		if (c == '\r' || c == '\n') { say("\n"); break; }
		if ((unsigned char)c == KEY_INTR) { say("\n"); return -1; }
		if (c == '\b' || c == 0x7f) { if (len > 0) len--; continue; }
		if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7f && len < max - 1)
			out[len++] = c;
	}
	out[len] = '\0';
	return len;
}

/* Request the ssh-userauth service, then try password auth (up to 3 times).
 * 0 on success, -1 on failure. */
static int do_auth(const char *user, const char *host)
{
	int plen, p;

	p = 0; wb(ptext, &p, MSG_SERVICE_REQUEST); wcstr(ptext, &p, "ssh-userauth");
	if (send_packet(ptext, p) != 0) return -1;
	if (recv_msg(&plen, 15000) != MSG_SERVICE_ACCEPT) {
		say("ssh: userauth service refused\n");
		return -1;
	}

	for (int attempt = 0; attempt < 3; attempt++) {
		char pw[256];
		say(user); say("@"); say(host); say("'s password: ");
		int pl = read_password(pw, sizeof pw);
		if (pl < 0) { say("ssh: aborted\n"); return -1; }

		p = 0;
		wb(ptext, &p, MSG_USERAUTH_REQUEST);
		wcstr(ptext, &p, user);
		wcstr(ptext, &p, "ssh-connection");
		wcstr(ptext, &p, "password");
		wb(ptext, &p, 0);                       /* not a password change */
		wcstr(ptext, &p, pw);
		if (send_packet(ptext, p) != 0) return -1;

		for (;;) {
			int t = recv_msg(&plen, 30000);
			if (t < 0) return -1;
			if (t == MSG_USERAUTH_SUCCESS) return 0;
			if (t == MSG_USERAUTH_BANNER) {
				int bp = 1; unsigned n = r32(payload, &bp);
				if (n < (unsigned)(plen - bp)) write(1, payload + bp, (int)n);
				continue;                       /* read the real result next */
			}
			if (t == MSG_USERAUTH_FAILURE) {
				say("Permission denied, please try again.\n");
				break;                          /* re-prompt */
			}
			/* anything else: ignore and keep waiting */
		}
	}
	say("ssh: too many authentication failures\n");
	return -1;
}

/* ---- channel + interactive session --------------------------------------- */

static unsigned remote_chan;          /* the server's channel id            */
static unsigned remote_window;        /* bytes the server will accept from us */
static unsigned remote_maxpkt = 32768;
static unsigned local_window = LOCAL_WINDOW;

#define LOCAL_CHAN 0

/* Open a session channel and request a pty + an interactive shell. 0/-1. */
static int open_session(void)
{
	int plen, p;

	p = 0;
	wb(ptext, &p, MSG_CHANNEL_OPEN);
	wcstr(ptext, &p, "session");
	w32(ptext, &p, LOCAL_CHAN);
	w32(ptext, &p, LOCAL_WINDOW);
	w32(ptext, &p, LOCAL_MAXPKT);
	if (send_packet(ptext, p) != 0) return -1;

	int t = recv_msg(&plen, 15000);
	if (t == MSG_CHANNEL_OPEN_FAILURE) { say("ssh: server refused session channel\n"); return -1; }
	if (t != MSG_CHANNEL_OPEN_CONFIRMATION) { say("ssh: bad channel response\n"); return -1; }
	p = 1;
	r32(payload, &p);                            /* our channel (echoed) */
	remote_chan   = r32(payload, &p);
	remote_window = r32(payload, &p);
	remote_maxpkt = r32(payload, &p);
	if (remote_maxpkt == 0 || remote_maxpkt > MAXPKT) remote_maxpkt = 32768;

	/* pty-req: a vt100-ish terminal, 80x24, empty modes. want_reply=0 keeps the
	 * round-trips down; a standard sshd always grants it. */
	p = 0;
	wb(ptext, &p, MSG_CHANNEL_REQUEST);
	w32(ptext, &p, remote_chan);
	wcstr(ptext, &p, "pty-req");
	wb(ptext, &p, 0);                            /* want_reply */
	wcstr(ptext, &p, "xterm");
	w32(ptext, &p, 80);                          /* cols  */
	w32(ptext, &p, 24);                          /* rows  */
	w32(ptext, &p, 0);                           /* width px  */
	w32(ptext, &p, 0);                           /* height px */
	{ unsigned char modes[1] = { 0 }; wstr(ptext, &p, modes, 1); }  /* TTY_OP_END */
	if (send_packet(ptext, p) != 0) return -1;

	/* shell. */
	p = 0;
	wb(ptext, &p, MSG_CHANNEL_REQUEST);
	w32(ptext, &p, remote_chan);
	wcstr(ptext, &p, "shell");
	wb(ptext, &p, 0);                            /* want_reply */
	if (send_packet(ptext, p) != 0) return -1;
	return 0;
}

/* Send `n` bytes of channel data, chunked to the server's window + max packet. */
static int send_channel_data(const unsigned char *d, int n)
{
	int off = 0;
	while (off < n) {
		int chunk = n - off;
		if ((unsigned)chunk > remote_maxpkt) chunk = (int)remote_maxpkt;
		if ((unsigned)chunk > remote_window) chunk = (int)remote_window;
		if (chunk <= 0) {                        /* window exhausted: let the loop
		                                            pick up a WINDOW_ADJUST first */
			return off;
		}
		int p = 0;
		wb(ptext, &p, MSG_CHANNEL_DATA);
		w32(ptext, &p, remote_chan);
		wstr(ptext, &p, d + off, chunk);
		if (send_packet(ptext, p) != 0) return -1;
		remote_window -= (unsigned)chunk;
		off += chunk;
	}
	return off;
}

/* Replenish our advertised window if it's run low. */
static int top_up_window(void)
{
	if (local_window >= LOCAL_WINDOW / 2)
		return 0;
	unsigned add = LOCAL_WINDOW - local_window;
	int p = 0;
	wb(ptext, &p, MSG_CHANNEL_WINDOW_ADJUST);
	w32(ptext, &p, remote_chan);
	w32(ptext, &p, add);
	if (send_packet(ptext, p) != 0) return -1;
	local_window += add;
	return 0;
}

/* Translate one terminal keystroke (as the window delivers it, see owos.h
 * KEY_* and the printable range) into the bytes a remote pty expects, appending
 * to `out`. Returns the number of bytes appended. */
static int map_key(unsigned char b, unsigned char *out)
{
	switch (b) {
	case '\n': case '\r': out[0] = '\r'; return 1;
	case '\b': case 0x7f: out[0] = 0x7f; return 1;
	case '\t':            out[0] = '\t'; return 1;
	case KEY_INTR:        out[0] = 0x03; return 1;   /* ^C to the remote */
	case KEY_UP:    out[0] = 0x1b; out[1] = '['; out[2] = 'A'; return 3;
	case KEY_DOWN:  out[0] = 0x1b; out[1] = '['; out[2] = 'B'; return 3;
	case KEY_RIGHT: out[0] = 0x1b; out[1] = '['; out[2] = 'C'; return 3;
	case KEY_LEFT:  out[0] = 0x1b; out[1] = '['; out[2] = 'D'; return 3;
	case KEY_HOME:  out[0] = 0x1b; out[1] = '['; out[2] = 'H'; return 3;
	case KEY_END:   out[0] = 0x1b; out[1] = '['; out[2] = 'F'; return 3;
	case KEY_DELETE: out[0] = 0x1b; out[1] = '['; out[2] = '3'; out[3] = '~'; return 4;
	default:
		if (b >= 0x20 && b < 0x7f) { out[0] = b; return 1; }
		return 0;
	}
}

/* The interactive loop: stdin keystrokes -> CHANNEL_DATA, server CHANNEL_DATA ->
 * stdout, plus flow-control + channel teardown. Returns when the session ends. */
static void interactive(const char *host)
{
	unsigned char obuf[2048]; int olen = 0;     /* keystrokes pending to the server */
	int closed = 0;

	for (;;) {
		int did = 0;

		/* 1. stdin -> outgoing buffer (translated to pty byte sequences). */
		char kb[256];
		int kn = read_nb(0, kb, sizeof kb);
		if (kn < 0) { closed = 1; }              /* terminal closed our stdin */
		else if (kn > 0) {
			for (int i = 0; i < kn && olen + 4 <= (int)sizeof obuf; i++)
				olen += map_key((unsigned char)kb[i], obuf + olen);
			did = 1;
		}
		if (olen > 0 && !closed) {
			int sent = send_channel_data(obuf, olen);
			if (sent < 0) break;
			if (sent > 0) {
				memmove(obuf, obuf + sent, (unsigned)(olen - sent));
				olen -= sent;
				did = 1;
			}
		}

		/* 2. socket -> process inbound packets. */
		if (rx_pump() < 0) break;
		int plen;
		int r;
		while ((r = parse_one(&plen)) == 1) {
			did = 1;
			int t = payload[0];
			if (t == MSG_CHANNEL_DATA) {
				int p = 1; r32(payload, &p);     /* recipient channel */
				unsigned n = r32(payload, &p);
				if (n <= (unsigned)(plen - p)) {
					write(1, payload + p, (int)n);
					local_window = local_window > n ? local_window - n : 0;
				}
			} else if (t == MSG_CHANNEL_EXTENDED_DATA) {
				int p = 1; r32(payload, &p); r32(payload, &p);  /* chan, data_type */
				unsigned n = r32(payload, &p);
				if (n <= (unsigned)(plen - p)) {
					write(1, payload + p, (int)n);
					local_window = local_window > n ? local_window - n : 0;
				}
			} else if (t == MSG_CHANNEL_WINDOW_ADJUST) {
				int p = 1; r32(payload, &p);
				remote_window += r32(payload, &p);
			} else if (t == MSG_CHANNEL_EOF) {
				/* server won't send more data; keep going until CLOSE */
			} else if (t == MSG_CHANNEL_CLOSE) {
				unsigned char cl[5]; int cp = 0;
				wb(cl, &cp, MSG_CHANNEL_CLOSE); w32(cl, &cp, remote_chan);
				send_packet(cl, cp);
				closed = 1;
			} else if (t == MSG_CHANNEL_REQUEST) {
				/* e.g. "exit-status"/"exit-signal": informational; if want_reply
				 * is set we'd owe a reply, but standard sshd doesn't ask. */
			} else if (t == MSG_GLOBAL_REQUEST) {
				/* decline anything that wants a reply */
				int p = 1; unsigned n = r32(payload, &p);
				int wr = (p + (int)n < plen) ? payload[p + (int)n] : 0;
				if (wr) { unsigned char f = MSG_REQUEST_FAILURE; send_packet(&f, 1); }
			} else if (t == MSG_DISCONNECT) {
				print_disconnect(plen);
				closed = 1;
			}
		}
		if (r < 0) break;                        /* decode error */
		if (top_up_window() < 0) break;

		if (closed && olen == 0) break;
		if (sock_dead) break;
		if (!did) msleep(4);
	}

	say("\nConnection to ");
	say(host);
	say(" closed.\n");
}

/* ---- entry --------------------------------------------------------------- */

int main(int argc, char **argv)
{
	if (argc < 2) {
		say("usage: ssh user@host [port]\n");
		return 1;
	}

	/* Parse user@host (user defaults to root when omitted). */
	static char user[64], host[160];
	const char *spec = argv[1];
	const char *at = 0;
	for (const char *q = spec; *q; q++) if (*q == '@') at = q;
	if (at) {
		int ul = (int)(at - spec);
		if (ul <= 0 || ul >= (int)sizeof user) { say("ssh: bad user@host\n"); return 1; }
		memcpy(user, spec, (unsigned)ul); user[ul] = '\0';
		int hl = strlen(at + 1);
		if (hl <= 0 || hl >= (int)sizeof host) { say("ssh: bad user@host\n"); return 1; }
		strcpy(host, at + 1);
	} else {
		strcpy(user, "root");
		if (strlen(spec) >= sizeof host) { say("ssh: host too long\n"); return 1; }
		strcpy(host, spec);
	}

	int port = 22;
	if (argc > 2) {
		int v = 0; const char *q = argv[2];
		while (*q >= '0' && *q <= '9') v = v * 10 + (*q++ - '0');
		if (v > 0 && v < 65536) port = v;
	}

	say("ssh: connecting to "); say(host); say("...\n");
	sock = net_connect(host, port, SK_TCP);
	if (sock < 0) {
		switch (sock) {
		case -2: say("ssh: could not resolve "); say(host); say("\n"); break;
		case -1: say("ssh: no network device\n"); break;
		case NET_EINTR: return EXIT_SIGINT;
		default: say("ssh: failed to connect to "); say(host); say("\n"); break;
		}
		return 1;
	}

	/* Version exchange. */
	{
		char line[64]; int p = 0;
		const char *id = CLIENT_IDENT;
		while (*id) line[p++] = *id++;
		line[p++] = '\r'; line[p++] = '\n';
		if (sock_send_all((unsigned char *)line, p) != 0) {
			say("ssh: connection lost\n"); net_close(sock); return 1;
		}
	}
	if (read_ident() != 0) {
		say("ssh: no SSH banner from server\n");
		net_close(sock);
		return 1;
	}

	if (do_kex(CLIENT_IDENT) != 0)  { net_close(sock); return 1; }
	if (do_auth(user, host) != 0)   { net_close(sock); return 1; }
	if (open_session() != 0)        { net_close(sock); return 1; }

	interactive(host);

	net_close(sock);
	return 0;
}
