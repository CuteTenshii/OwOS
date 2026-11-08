/* tls.c — user-space TLS 1.2/1.3 client (see tls.h for the contract).
 *
 * Ported from kernel/tls.c. Same wire behavior; the only substitutions are the
 * transport (an owned SK_TCP socket via net_connect/net_send/net_recv/net_close
 * instead of tcp.c), the allocator (malloc/free instead of kmalloc/kfree), and
 * logging (dropped — errors surface as a NULL handle or a negative read/write).
 *
 * Speaks exactly one cipher suite per version:
 *   TLS 1.3: TLS_AES_128_GCM_SHA256 (x25519 key share, HKDF schedule)
 *   TLS 1.2: TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256 (x25519 ECDHE)
 *
 *   ClientHello (x25519 group, SNI)
 *        -> ServerHello, Certificate, ServerKeyExchange, ServerHelloDone (1.2)
 *   ClientKeyExchange (our x25519 pubkey), ChangeCipherSpec, Finished
 *        -> ChangeCipherSpec, Finished
 *
 * Both sides' Finished messages carry a PRF/HMAC over the full handshake
 * transcript, so a verified server Finished proves the whole key schedule.
 *
 * SECURITY: the server certificate is NOT verified (deliberate — see tls.h):
 * resists passive eavesdropping, not an active man-in-the-middle.
 *
 * NET_EINTR: net_recv/net_send return -4 when a cooperative Ctrl+C arrives
 * mid-call. It's latched in c->intr and propagated: the handshake aborts (NULL
 * handle) and tls_read returns NET_EINTR so the caller can tear down.
 */

#include <stddef.h>
#include <stdint.h>

#include "crypto.h"
#include "ech.h"
#include "owos.h"
#include "resolve.h"
#include "tls.h"

/* ------------------------------------------------------------------ helpers */

/* ClientHello.random + the ephemeral X25519 key come from the userspace CSPRNG
 * (rng_bytes: RDRAND-backed when the CPU has it). */
/* Overwrite secret material so freed heap / dead stack frames don't retain live
 * keys. volatile store the optimizer can't elide (no memset_s here). */
static void secure_wipe(void *p, uint32_t n)
{
  volatile uint8_t *v = p;
  while (n--)
    *v++ = 0;
}

/* Constant-time equality of two `n`-byte buffers: 1 if equal, 0 otherwise.
 * No data-dependent branch, so comparing a secret-derived MAC leaks no timing.
 */
static int ct_equal(const uint8_t *a, const uint8_t *b, uint32_t n)
{
  uint8_t diff = 0;
  for (uint32_t i = 0; i < n; i++)
    diff |= a[i] ^ b[i];
  return diff == 0;
}

/* An all-zero X25519 output means the peer forced a small-order point; the
 * shared secret would be attacker-known, so reject it (RFC 7748 §6.1). */
static int is_all_zero(const uint8_t *p, uint32_t n)
{
  uint8_t acc = 0;
  for (uint32_t i = 0; i < n; i++)
    acc |= p[i];
  return acc == 0;
}

static void put16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}
static void put64be(uint8_t *p, uint64_t v)
{
  for (int i = 0; i < 8; i++)
    p[i] = (uint8_t)(v >> (56 - i * 8));
}

/* ----------------------------------------------------------------- wire */

#define REC_CCS 20
#define REC_ALERT 21
#define REC_HS 22
#define REC_APP 23

#define HS_CLIENT_HELLO 1
#define HS_SERVER_HELLO 2
#define HS_CERT 11
#define HS_SKE 12
#define HS_CERT_REQ 13
#define HS_DONE 14
#define HS_CKE 16
#define HS_FINISHED 20
#define HS_ENC_EXT 8      /* EncryptedExtensions (TLS 1.3) */
#define HS_CERT_VERIFY 15 /* CertificateVerify   (TLS 1.3) */

#define CIPHER_ECDHE_RSA_AES128_GCM 0xC02F /* TLS 1.2 */
#define CIPHER_AES128_GCM_SHA256 0x1301    /* TLS 1.3 */

#define ECH_RETRY_MAX 1024u

#define TLS_RECMAX 18432u
#define TLS_HSMAX 20480u

/* ------------------------------------------------------------------- state */

/* One TLS session, heap-allocated per connection (~92 KiB: the
 * record/handshake/ app buffers, sized for the worst-case TLS record,
 * dominate). */
struct tls_conn {
  int established; /* handshake finished, app data flows     */
  int tcp;         /* owned SK_TCP socket handle              */
  int intr;        /* a NET_EINTR was seen; propagate it      */
  int is13;        /* negotiated TLS 1.3 (vs 1.2)             */
  uint64_t client_seq, server_seq;
  int send_encrypted, recv_encrypted;

  uint8_t client_key[16], server_key[16];
  uint8_t client_iv[12], server_iv[12]; /* 1.2 uses first 4; 1.3 all 12 */

  struct sha256 transcript; /* running hash of handshake messages      */

  /* ECH: 0 not offered, 1 offered but not (yet) accepted, 2 accepted. On a
   * rejection the server's retry_configs land in retry[]. */
  int ech;
  struct sha256 inner_transcript; /* ClientHelloInner, until acceptance is known */
  uint8_t inner_random[32];
  uint8_t retry[ECH_RETRY_MAX];
  uint32_t retrylen;

  /* ALPN: the application protocol the server selected ("h2"/"http/1.1"), or
   * empty if it offered none. Read by tls_alpn(). */
  char neg_alpn[16];
  uint32_t neg_alpn_len;

  uint8_t rxraw[TLS_RECMAX + 5]; /* raw inbound TCP bytes -> records     */
  uint32_t rxlen, rxpos;
  uint8_t recpt[TLS_RECMAX]; /* decrypted/plaintext current record   */
  uint8_t hsbuf[TLS_HSMAX];  /* reassembled handshake-message bytes  */
  uint32_t hslen, hspos;
  uint8_t appbuf[TLS_RECMAX]; /* leftover app plaintext for the caller */
  uint32_t applen, apppos;
  uint8_t sbuf[TLS_RECMAX + 32]; /* scratch for an outbound record        */
};

static void set_alpn(struct tls_conn *c, const uint8_t *p, uint32_t n)
{
  /* Keep one byte for a NUL so neg_alpn is always a valid C string, even if a
   * server sends a name that fills the buffer. Callers use neg_alpn_len today,
   * but the terminator guards any future string use. */
  if (n > sizeof c->neg_alpn - 1)
    n = sizeof c->neg_alpn - 1;
  memcpy(c->neg_alpn, p, n);
  c->neg_alpn[n] = '\0';
  c->neg_alpn_len = n;
}

int tls_alpn(tls_conn *c)
{
  if (!c)
    return ALPN_NONE;
  if (c->neg_alpn_len == 2 && c->neg_alpn[0] == 'h' && c->neg_alpn[1] == '2')
    return ALPN_H2;
  if (c->neg_alpn_len)
    return ALPN_HTTP11;
  return ALPN_NONE;
}

/* --------------------------------------------------------- socket writes */

/* Write all `len` bytes to the TCP socket; latches NET_EINTR. -1 on error. */
static int sock_write(struct tls_conn *c, const uint8_t *p, uint32_t len)
{
  uint32_t off = 0;
  while (off < len) {
    int n = net_send(c->tcp, p + off, (int)(len - off));
    if (n == NET_EINTR) {
      c->intr = 1;
      return -1;
    }
    if (n <= 0)
      return -1;
    off += (uint32_t)n;
  }
  return 0;
}

/* ----------------------------------------------------------- record layer */

/* Pull TCP bytes until rxraw holds at least `need` unconsumed. -1 on EOF/error;
 * latches c->intr on NET_EINTR. */
static int raw_fill(struct tls_conn *c, uint32_t need)
{
  while (c->rxlen - c->rxpos < need) {
    if (c->rxpos > 0) {
      memmove(c->rxraw, c->rxraw + c->rxpos, c->rxlen - c->rxpos);
      c->rxlen -= c->rxpos;
      c->rxpos = 0;
    }
    if (c->rxlen == sizeof c->rxraw)
      return -1;
    int n = net_recv(c->tcp, c->rxraw + c->rxlen, (int)(sizeof c->rxraw - c->rxlen));
    if (n == NET_EINTR) {
      c->intr = 1;
      return -1;
    }
    if (n > 0)
      c->rxlen += (uint32_t)n;
    else
      return -1; /* closed or idle timeout */
  }
  return 0;
}

/* TLS 1.3 per-record nonce: write_iv XOR seq (seq right-aligned in 12 bytes).
 */
static void nonce13(uint8_t out[12], const uint8_t iv[12], uint64_t seq)
{
  memcpy(out, iv, 12);
  for (int i = 0; i < 8; i++)
    out[11 - i] ^= (uint8_t)(seq >> (8 * i));
}

/* Receive one record; decrypt if the read side is active. Returns the (inner,
 * for 1.3) content type, fills out[*outlen] with plaintext. -1 on error/EOF. */
static int recv_record(struct tls_conn *c, uint8_t *type_out, uint8_t *out, uint32_t *outlen)
{
  for (;;) {
    if (raw_fill(c, 5) < 0)
      return -1;
    uint8_t *h = c->rxraw + c->rxpos;
    uint8_t type = h[0];
    if (h[1] != 0x03 || h[2] < 0x01 || h[2] > 0x03)
      return -1;
    if (type != REC_CCS && type != REC_ALERT && type != REC_HS && type != REC_APP)
      return -1;
    uint32_t len = ((uint32_t)h[3] << 8) | h[4];
    if (len > TLS_RECMAX || raw_fill(c, 5 + len) < 0)
      return -1;
    uint8_t *payload = c->rxraw + c->rxpos + 5;

    /* TLS 1.3 sends a dummy ChangeCipherSpec for middlebox compat; ignore. */
    if (c->is13 && type == REC_CCS) {
      c->rxpos += 5 + len;
      continue;
    }

    if (!c->recv_encrypted) {
      memcpy(out, payload, len);
      *outlen = len;
      *type_out = type;
      c->rxpos += 5 + len;
      return 0;
    }

    if (c->is13) {
      if (type != REC_APP)
        return -1;
      if (len < 16)
        return -1;
      uint8_t nonce[12];
      nonce13(nonce, c->server_iv, c->server_seq);
      uint8_t aad[5] = {type, 0x03, 0x03, (uint8_t)(len >> 8), (uint8_t)len};
      uint32_t inner = len - 16;
      const uint8_t *tag = payload + inner;
      if (aes128_gcm_open(c->server_key, nonce, aad, 5, payload, inner, out, tag) != 0)
        return -1;
      c->server_seq++;
      c->rxpos += 5 + len;
      while (inner > 0 && out[inner - 1] == 0) /* strip zero padding */
        inner--;
      if (inner == 0)
        return -1;
      uint8_t inner_type = out[inner - 1];
      if (inner_type != REC_ALERT && inner_type != REC_HS && inner_type != REC_APP)
        return -1;
      *type_out = inner_type; /* real content type */
      *outlen = inner - 1;
      return 0;
    }

    /* TLS 1.2 AEAD: explicit-nonce prefix, sequence-number AAD. */
    if (len < 8 + 16)
      return -1;
    uint32_t ctlen = len - 8 - 16;
    uint8_t nonce[12];
    memcpy(nonce, c->server_iv, 4);
    memcpy(nonce + 4, payload, 8);
    uint8_t aad[13];
    put64be(aad, c->server_seq);
    aad[8] = type;
    aad[9] = 0x03;
    aad[10] = 0x03;
    put16(aad + 11, (uint16_t)ctlen);
    if (aes128_gcm_open(c->server_key, nonce, aad, 13, payload + 8, ctlen, out, payload + 8 + ctlen) != 0)
      return -1;
    *outlen = ctlen;
    *type_out = type;
    c->server_seq++;
    c->rxpos += 5 + len;
    return 0;
  }
}

/* Send one record; encrypt if the write side is active. */
static int send_record(struct tls_conn *c, uint8_t type, const uint8_t *data, uint32_t len)
{
  uint8_t hdr[5];
  if (len > 16384 || (!data && len))
    return -1;
  if (!c->send_encrypted) {
    hdr[0] = type;
    hdr[1] = 0x03;
    hdr[2] = 0x03;
    put16(hdr + 3, (uint16_t)len);
    if (sock_write(c, hdr, 5) < 0)
      return -1;
    if (len && sock_write(c, data, len) < 0)
      return -1;
    return 0;
  }

  if (c->is13) {
    /* inner plaintext = content || real_type; outer type is always 23. */
    uint8_t nonce[12];
    nonce13(nonce, c->client_iv, c->client_seq);
    memcpy(c->sbuf, data, len);
    c->sbuf[len] = type;
    uint32_t inner = len + 1;
    uint32_t rlen = inner + 16;
    if (rlen > TLS_RECMAX)
      return -1;
    uint8_t aad[5] = {REC_APP, 0x03, 0x03, (uint8_t)(rlen >> 8), (uint8_t)rlen};
    uint8_t tag[16];
    aes128_gcm_seal(c->client_key, nonce, aad, 5, c->sbuf, inner, c->sbuf, tag);
    memcpy(c->sbuf + inner, tag, 16);
    hdr[0] = REC_APP;
    hdr[1] = 0x03;
    hdr[2] = 0x03;
    put16(hdr + 3, (uint16_t)rlen);
    if (sock_write(c, hdr, 5) < 0 || sock_write(c, c->sbuf, rlen) < 0)
      return -1;
    c->client_seq++;
    return 0;
  }

  uint8_t nonce[12];
  memcpy(nonce, c->client_iv, 4);
  put64be(c->sbuf, c->client_seq); /* explicit nonce = seq */
  memcpy(nonce + 4, c->sbuf, 8);
  uint8_t aad[13];
  put64be(aad, c->client_seq);
  aad[8] = type;
  aad[9] = 0x03;
  aad[10] = 0x03;
  put16(aad + 11, (uint16_t)len);
  uint8_t tag[16];
  aes128_gcm_seal(c->client_key, nonce, aad, 13, data, len, c->sbuf + 8, tag);
  memcpy(c->sbuf + 8 + len, tag, 16);
  uint32_t rlen = 8 + len + 16;
  hdr[0] = type;
  hdr[1] = 0x03;
  hdr[2] = 0x03;
  put16(hdr + 3, (uint16_t)rlen);
  if (sock_write(c, hdr, 5) < 0 || sock_write(c, c->sbuf, rlen) < 0)
    return -1;
  c->client_seq++;
  return 0;
}

/* ------------------------------------------------------- handshake reader */

/* Ensure the handshake buffer holds `need` bytes, pulling Handshake records.
 * Returns 0, -1 on error, -2 on an Alert. */
static int hs_fill(struct tls_conn *c, uint32_t need)
{
  while (c->hslen - c->hspos < need) {
    uint8_t type;
    uint32_t rl;
    if (recv_record(c, &type, c->recpt, &rl) < 0)
      return -1;
    if (type == REC_ALERT)
      return -2;
    if (type != REC_HS)
      return -1;
    if (c->hspos > 0) {
      memmove(c->hsbuf, c->hsbuf + c->hspos, c->hslen - c->hspos);
      c->hslen -= c->hspos;
      c->hspos = 0;
    }
    if (c->hslen + rl > sizeof c->hsbuf)
      return -1;
    memcpy(c->hsbuf + c->hslen, c->recpt, rl);
    c->hslen += rl;
  }
  return 0;
}

/* Next handshake message: sets type, points body at the payload, feeds the
 * full message (header included) into the transcript hash. */
static int hs_next(struct tls_conn *c, uint8_t *mtype, uint8_t **body, uint32_t *blen)
{
  int r = hs_fill(c, 4);
  if (r < 0)
    return r; /* propagate -2 (alert) vs -1 (EOF) */
  uint32_t l =
      ((uint32_t)c->hsbuf[c->hspos + 1] << 16) | ((uint32_t)c->hsbuf[c->hspos + 2] << 8) | c->hsbuf[c->hspos + 3];
  r = hs_fill(c, 4 + l);
  if (r < 0)
    return r;
  uint8_t *h = c->hsbuf + c->hspos;
  *mtype = h[0];
  sha256_update(&c->transcript, h, 4 + l);
  *body = h + 4;
  *blen = l;
  c->hspos += 4 + l;
  return 0;
}

/* ----------------------------------------------- TLS 1.3 key schedule */

/* HKDF-Expand-Label (RFC 8446 §7.1): wraps the secret expansion with the
 * "tls13 "-prefixed label and a context (here always a transcript hash). */
static void hkdf_expand_label(const uint8_t secret[32], const char *label, const uint8_t *ctx, uint8_t ctxlen,
                              uint8_t *out, uint16_t outlen)
{
  uint8_t info[2 + 1 + 6 + 32 + 1 + 32];
  uint32_t p = 0, ll = (uint32_t)strlen(label);
  info[p++] = (uint8_t)(outlen >> 8);
  info[p++] = (uint8_t)outlen;
  info[p++] = (uint8_t)(6 + ll);
  memcpy(info + p, "tls13 ", 6);
  p += 6;
  memcpy(info + p, label, ll);
  p += ll;
  info[p++] = ctxlen;
  if (ctxlen) {
    memcpy(info + p, ctx, ctxlen);
    p += ctxlen;
  }
  hkdf_expand(secret, info, p, out, outlen);
}

static void derive_secret(const uint8_t secret[32], const char *label, const uint8_t thash[32], uint8_t out[32])
{
  hkdf_expand_label(secret, label, thash, 32, out, 32);
}

/* Install a direction's traffic key+iv from its traffic secret. */
static void install_keys(struct tls_conn *c, const uint8_t c_secret[32], const uint8_t s_secret[32])
{
  hkdf_expand_label(c_secret, "key", 0, 0, c->client_key, 16);
  hkdf_expand_label(c_secret, "iv", 0, 0, c->client_iv, 12);
  hkdf_expand_label(s_secret, "key", 0, 0, c->server_key, 16);
  hkdf_expand_label(s_secret, "iv", 0, 0, c->server_iv, 12);
  c->client_seq = c->server_seq = 0;
}

/* --------------------------------------------------------- ClientHello */

/* One ClientHello offering both TLS 1.3 (key_share + supported_versions) and
 * TLS 1.2 (ECDHE-RSA). The server's chosen version decides which path runs.
 * `inner` builds a ClientHelloInner instead: TLS 1.3 only (RFC 9849 §6.1).
 * `tail` is a pre-encoded extension appended last (the ECH extension). */
static uint32_t build_client_hello(uint8_t *out, const uint8_t client_random[32], const uint8_t session_id[32],
                                   const uint8_t our_pub[32], const char *sni, int offer_h2, int inner,
                                   const uint8_t *tail, uint32_t taillen)
{
  uint32_t namelen = 0;
  if (sni)
    while (sni[namelen])
      namelen++;
  if (namelen > 255) /* a DNS name is <=253 bytes; anything
                      * longer can't be a real host, and would
                      * overrun the caller's fixed ClientHello
                      * buffer — clamp it out. */
    namelen = 255;

  uint8_t *p = out + 4; /* leave room for the HS header */

  *p++ = 0x03;
  *p++ = 0x03; /* legacy_version = TLS 1.2 */
  memcpy(p, client_random, 32);
  p += 32;
  *p++ = 32; /* legacy_session_id (compat) */
  memcpy(p, session_id, 32);
  p += 32;

  /* cipher_suites: TLS 1.3 AEAD, TLS 1.2 ECDHE-RSA, empty-reneg SCSV */
  put16(p, inner ? 2 : 6);
  p += 2;
  put16(p, CIPHER_AES128_GCM_SHA256);
  p += 2;
  if (!inner) {
    put16(p, CIPHER_ECDHE_RSA_AES128_GCM);
    p += 2;
    put16(p, 0x00FF);
    p += 2;
  }

  *p++ = 1;
  *p++ = 0; /* compression_methods: null */

  uint8_t *extlen = p;
  p += 2; /* extensions length, filled later */
  uint8_t *exts = p;

  /* server_name (SNI) */
  if (namelen) {
    put16(p, 0x0000);
    p += 2;
    put16(p, (uint16_t)(namelen + 5));
    p += 2;
    put16(p, (uint16_t)(namelen + 3));
    p += 2;
    *p++ = 0; /* host_name type */
    put16(p, (uint16_t)namelen);
    p += 2;
    memcpy(p, sni, namelen);
    p += namelen;
  }
  /* supported_groups: x25519 */
  put16(p, 0x000A);
  p += 2;
  put16(p, 4);
  p += 2;
  put16(p, 2);
  p += 2;
  put16(p, 0x001D);
  p += 2;
  /* ec_point_formats: uncompressed (for the TLS 1.2 path) */
  if (!inner) {
    put16(p, 0x000B);
    p += 2;
    put16(p, 2);
    p += 2;
    *p++ = 1;
    *p++ = 0;
  }
  /* signature_algorithms: rsa_pss (1.3) + rsa_pkcs1 + ecdsa */
  put16(p, 0x000D);
  p += 2;
  static const uint8_t sigalgs[] = {0x08, 0x04, 0x08, 0x05, 0x08, 0x06,  /* rsa_pss_rsae_sha256/384/512 */
                                    0x04, 0x01, 0x05, 0x01, 0x06, 0x01,  /* rsa_pkcs1_sha256/384/512    */
                                    0x04, 0x03, 0x05, 0x03, 0x06, 0x03}; /* ecdsa_secp256r1/384/521     */
  put16(p, (uint16_t)(sizeof sigalgs + 2));
  p += 2;
  put16(p, (uint16_t)sizeof sigalgs);
  p += 2;
  memcpy(p, sigalgs, sizeof sigalgs);
  p += sizeof sigalgs;
  /* supported_versions: TLS 1.3 then 1.2 */
  put16(p, 0x002B);
  p += 2;
  put16(p, inner ? 3 : 5);
  p += 2;
  *p++ = inner ? 2 : 4;
  put16(p, 0x0304);
  p += 2;
  if (!inner) {
    put16(p, 0x0303);
    p += 2;
  }
  /* key_share: one x25519 share */
  put16(p, 0x0033);
  p += 2;
  put16(p, 38);
  p += 2;
  put16(p, 36);
  p += 2; /* client_shares length */
  put16(p, 0x001D);
  p += 2; /* group x25519 */
  put16(p, 32);
  p += 2;
  memcpy(p, our_pub, 32);
  p += 32;
  /* ALPN: only offered when the caller wants HTTP/2 (offer_h2). Offering "h2"
   * unconditionally would make h2-preferring servers select HTTP/2 even for a
   * plain HTTP/1.1 request, breaking the connection — so a plain TLS handshake
   * carries no ALPN at all (identical to the pre-ALPN wire). */
  if (offer_h2) {
    put16(p, 0x0010);
    p += 2; /* application_layer_protocol_negotiation */
    put16(p, 14);
    p += 2; /* extension_data length                  */
    put16(p, 12);
    p += 2; /* ProtocolNameList length                */
    *p++ = 2;
    *p++ = 'h';
    *p++ = '2';
    *p++ = 8;
    memcpy(p, "http/1.1", 8);
    p += 8;
  }
  if (taillen) {
    memcpy(p, tail, taillen);
    p += taillen;
  }

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
static int parse_server_hello(struct tls_conn *c, const uint8_t *body, uint32_t bl, uint8_t server_random[32],
                              uint16_t *suite, int *sel13, uint8_t server_pub[32], int *have_ks)
{
  *sel13 = 0;
  *have_ks = 0;
  if (bl < 38)
    return -1;
  memcpy(server_random, body + 2, 32);
  uint32_t off = 34;
  uint8_t sidlen = body[off++];
  if (off + sidlen + 3u > bl)
    return -1;
  off += sidlen;
  *suite = ((uint16_t)body[off] << 8) | body[off + 1];
  off += 3; /* suite(2) + compression(1) */
  if (off + 2u > bl)
    return 0; /* no extensions: fine for TLS 1.2 */
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
        memcpy(server_pub, body + off + 4, 32);
        *have_ks = 1;
      }
    } else if (et == 0x0010 && el >= 3) {
      /* ALPN (TLS 1.2): ProtocolNameList(2) ProtocolName(len+bytes). */
      uint8_t nl = body[off + 2];
      if (off + 3u + nl <= end)
        set_alpn(c, body + off + 3, nl);
    }
    off += el;
  }
  return 0;
}

/* The encrypted second half of a TLS 1.3 handshake, after ServerHello: derive
 * handshake keys, read EncryptedExtensions..server Finished (verifying it),
 * send client Finished, switch to application keys. Returns 1 on success. */
static int tls13_handshake(struct tls_conn *c, const uint8_t our_priv[32], const uint8_t server_pub[32])
{
  uint8_t zero[32] = {0}, empty_hash[32];
  sha256("", 0, empty_hash);

  uint8_t early[32], derived[32], shared[32], hs_secret[32];
  hkdf_extract(zero, 32, zero, 32, early);
  hkdf_expand_label(early, "derived", empty_hash, 32, derived, 32);
  x25519(shared, our_priv, server_pub);
  if (is_all_zero(shared, 32)) /* small-order point: reject */
    return 0;
  hkdf_extract(derived, 32, shared, 32, hs_secret);

  uint8_t th[32];
  {
    struct sha256 ts = c->transcript;
    sha256_final(&ts, th);
  } /* ..ServerHello */
  uint8_t c_hs[32], s_hs[32];
  derive_secret(hs_secret, "c hs traffic", th, c_hs);
  derive_secret(hs_secret, "s hs traffic", th, s_hs);
  c->is13 = 1;
  c->send_encrypted = c->recv_encrypted = 1;
  install_keys(c, c_hs, s_hs);

  uint8_t derived2[32], master[32];
  hkdf_expand_label(hs_secret, "derived", empty_hash, 32, derived2, 32);
  hkdf_extract(derived2, 32, zero, 32, master);

  /* Read EncryptedExtensions, Certificate, CertificateVerify, Finished. */
  for (;;) {
    struct sha256 before = c->transcript; /* hash through prior messages */
    uint8_t mt, *body;
    uint32_t bl;
    int r = hs_next(c, &mt, &body, &bl);
    if (r < 0)
      return 0; /* alert or read failure */
    if (mt == HS_CERT_REQ)
      return 0; /* 1.3 client cert (unsupported) */
    if (mt == HS_FINISHED) {
      if (bl != 32)
        return 0;
      uint8_t thcv[32], fk[32], expect[32];
      sha256_final(&before, thcv); /* ..CertificateVerify */
      hkdf_expand_label(s_hs, "finished", 0, 0, fk, 32);
      hmac_sha256(fk, 32, thcv, 32, expect);
      if (!ct_equal(body, expect, 32))
        return 0; /* server Finished mismatch */
      break;
    }
    if (mt == HS_ENC_EXT && bl >= 2) {
      /* ALPN (TLS 1.3) rides here: Extensions(2) then ext list. */
      uint32_t el = ((uint32_t)body[0] << 8) | body[1];
      uint32_t o = 2, e = (2 + el < bl) ? 2 + el : bl;
      while (o + 4 <= e) {
        uint16_t et = ((uint16_t)body[o] << 8) | body[o + 1];
        uint16_t xl = ((uint16_t)body[o + 2] << 8) | body[o + 3];
        o += 4;
        if (o + xl > e)
          break;
        if (et == 0x0010 && xl >= 3) {
          uint8_t nl = body[o + 2];
          if (o + 3u + nl <= e)
            set_alpn(c, body + o + 3, nl);
        } else if (et == ECH_EXT_TYPE && c->ech == 1 && xl <= sizeof c->retry) {
          memcpy(c->retry, body + o, xl);
          c->retrylen = xl;
        }
        o += xl;
      }
    }
    if (mt == HS_ENC_EXT && c->ech == 1)
      return 0; /* ECH rejected: abort, the caller retries */
    /* EncryptedExtensions / Certificate / CertificateVerify: not verified. */
  }

  uint8_t th_sf[32];
  {
    struct sha256 ts = c->transcript;
    sha256_final(&ts, th_sf);
  } /* ..srv Finished */

  /* client Finished, still under the handshake key. */
  uint8_t fk_c[32], verify[32];
  hkdf_expand_label(c_hs, "finished", 0, 0, fk_c, 32);
  hmac_sha256(fk_c, 32, th_sf, 32, verify);
  uint8_t fin[36];
  fin[0] = HS_FINISHED;
  fin[1] = 0;
  fin[2] = 0;
  fin[3] = 32;
  memcpy(fin + 4, verify, 32);
  if (send_record(c, REC_HS, fin, 36) < 0)
    return 0;

  /* Switch both directions to application traffic keys. */
  uint8_t c_ap[32], s_ap[32];
  derive_secret(master, "c ap traffic", th_sf, c_ap);
  derive_secret(master, "s ap traffic", th_sf, s_ap);
  install_keys(c, c_ap, s_ap);

  /* Every derived secret is now in c->{client,server}_key/iv; wipe the rest. */
  secure_wipe(shared, sizeof shared);
  secure_wipe(early, sizeof early);
  secure_wipe(derived, sizeof derived);
  secure_wipe(derived2, sizeof derived2);
  secure_wipe(hs_secret, sizeof hs_secret);
  secure_wipe(master, sizeof master);
  secure_wipe(c_hs, sizeof c_hs);
  secure_wipe(s_hs, sizeof s_hs);
  secure_wipe(c_ap, sizeof c_ap);
  secure_wipe(s_ap, sizeof s_ap);

  c->established = 1;
  return 1;
}

/* ClientHelloOuter carrying the encrypted ClientHelloInner (RFC 9849 §6.1). Starts the inner
 * transcript for the acceptance check. Returns the outer length, or 0 on failure. */
static uint32_t build_ech_hello(struct tls_conn *c, const struct ech_cfg *cfg, const char *host,
                                const uint8_t client_random[32], const uint8_t session_id[32],
                                const uint8_t our_pub[32], int offer_h2, uint8_t *out)
{
  static const uint8_t inner_ext[5] = {0xfe, 0x0d, 0, 1, 1}; /* ECHClientHello type inner */
  uint8_t inner[768], encoded[1024], info[8 + 512], skE[32], enc[HPKE_ENC_LEN], ext[1100], sealed[1100];
  if (cfg->rawlen > 512)
    return 0;

  rng_bytes(c->inner_random, 32);
  uint32_t ilen = build_client_hello(inner, c->inner_random, session_id, our_pub, host, offer_h2, 1, inner_ext, 5);
  uint32_t elen = ech_encode_inner(inner, ilen, (uint32_t)strlen(host), cfg, encoded, sizeof encoded);
  if (!elen)
    return 0;

  memcpy(info, "tls ech", 8); /* includes the NUL */
  memcpy(info + 8, cfg->raw, cfg->rawlen);
  struct hpke_ctx hc;
  rng_bytes(skE, 32);
  int rc = hpke_setup_base_s(&hc, enc, cfg->pub, info, 8 + cfg->rawlen, skE);
  secure_wipe(skE, sizeof skE);
  if (rc)
    return 0;

  uint32_t payload = elen + HPKE_TAG_LEN;
  uint32_t xlen = ech_outer_ext(ext, cfg, enc, payload);
  uint32_t olen = build_client_hello(out, client_random, session_id, our_pub, cfg->public_name, offer_h2, 0, ext, xlen);
  /* The AAD is the outer hello body with the payload still zeroed. */
  hpke_seal(&hc, out + 4, olen - 4, encoded, elen, sealed);
  memcpy(out + olen - payload, sealed, payload);

  sha256_init(&c->inner_transcript);
  sha256_update(&c->inner_transcript, inner, ilen);
  c->ech = 1;
  return olen;
}

/* RFC 9849 §7.2: the server signals acceptance in the last 8 bytes of ServerHello.random. `sh` is
 * the ServerHello body, preceded in memory by its 4-byte handshake header. */
static int ech_accepted(const struct tls_conn *c, const uint8_t *sh, uint32_t bl)
{
  static const uint8_t zeros[8] = {0};
  struct sha256 ts = c->inner_transcript;
  sha256_update(&ts, sh - 4, 4 + 2 + 24);
  sha256_update(&ts, zeros, 8);
  sha256_update(&ts, sh + 34, bl - 34);
  uint8_t th[32], prk[32], conf[8];
  sha256_final(&ts, th);
  hkdf_extract(zeros, 8, c->inner_random, 32, prk);
  hkdf_expand_label(prk, "ech accept confirmation", th, 32, conf, 8);
  return ct_equal(conf, sh + 26, 8);
}

/* --------------------------------------------------------------- connect */

static const uint8_t x25519_base[32] = {9};
static int g_tls_error;

int tls_last_error(void) { return g_tls_error; }

/* ECH attempt state shared between tls_client and tls_open. */
struct ech_try {
  const struct ech_cfg *cfg; /* in: config to offer */
  int rejected;              /* out: the server did not accept ECH */
  uint8_t retry[ECH_RETRY_MAX];
  uint32_t retrylen;
};

static tls_conn *tls_open(const char *host, int port, int offer_h2, struct ech_try *ech)
{
  g_tls_error = TLS_OPEN_HANDSHAKE;
  /* Allocate this session's state (~92 KiB) with malloc; freed by tls_close.
   * The user heap serves multi-hundred-KiB allocations (e.g. the browser's
   * decode buffers), so a single 92 KiB struct is well within budget. */
  struct tls_conn *c = malloc(sizeof *c);
  if (!c)
    return NULL;
  memset(c, 0, sizeof *c);

  /* Resolve in user space (the kernel connect path takes an IP only), but keep
   * SNI = the original hostname (passed to build_client_hello below). */
  unsigned char ip[4];
  char ipstr[16];
  if (resolve(host, ip) != 1) {
    g_tls_error = TLS_OPEN_RESOLVE;
    free(c);
    return NULL;
  }
  {
    int di = 0;
    for (int i = 0; i < 4; i++) {
      unsigned v = ip[i], d = 100;
      int started = 0;
      while (d) {
        unsigned q = v / d;
        if (q || started || d == 1) {
          ipstr[di++] = (char)('0' + q);
          started = 1;
        }
        v %= d;
        d /= 10;
      }
      if (i < 3)
        ipstr[di++] = '.';
    }
    ipstr[di] = '\0';
  }

  c->tcp = net_connect(ipstr, port, SK_TCP);
  if (c->tcp < 0) {
    g_tls_error = TLS_OPEN_CONNECT;
    free(c);
    return NULL;
  }

  sha256_init(&c->transcript);

  uint8_t client_random[32], server_random[32];
  uint8_t our_priv[32], our_pub[32];
  rng_bytes(client_random, 32);
  rng_bytes(our_priv, 32);
  x25519(our_pub, our_priv, x25519_base);

  /* --- ClientHello --- */
  uint8_t session_id[32], ch[1536];
  rng_bytes(session_id, 32);
  uint32_t chlen = 0;
  if (ech && ech->cfg)
    chlen = build_ech_hello(c, ech->cfg, host, client_random, session_id, our_pub, offer_h2, ch);
  if (!chlen) {
    c->ech = 0;
    chlen = build_client_hello(ch, client_random, session_id, our_pub, host, offer_h2, 0, 0, 0);
  }
  sha256_update(&c->transcript, ch, chlen);
  if (send_record(c, REC_HS, ch, chlen) < 0) {
    net_close(c->tcp);
    free(c);
    return NULL;
  }

  /* --- ServerHello (plaintext in both versions); pick the version path --- */
  uint8_t server_pub[32];
  {
    uint8_t mt, *body;
    uint32_t bl;
    if (hs_next(c, &mt, &body, &bl) < 0 || mt != HS_SERVER_HELLO) {
      net_close(c->tcp);
      free(c);
      return NULL;
    }
    uint16_t suite;
    int sel13, have_ks;
    if (parse_server_hello(c, body, bl, server_random, &suite, &sel13, server_pub, &have_ks) < 0) {
      net_close(c->tcp);
      free(c);
      return NULL;
    }
    if (c->ech) {
      static const uint8_t hrr_random[32] = {0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C,
                                             0x02, 0x1E, 0x65, 0xB8, 0x91, 0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB,
                                             0x8C, 0x5E, 0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C};
      if (!ct_equal(server_random, hrr_random, 32) && sel13 && ech_accepted(c, body, bl)) {
        c->ech = 2;
        c->transcript = c->inner_transcript;
        sha256_update(&c->transcript, body - 4, 4 + bl);
      } else if (!sel13 || ct_equal(server_random, hrr_random, 32)) {
        ech->rejected = 1; /* no retry_configs without a TLS 1.3 handshake */
        net_close(c->tcp);
        free(c);
        return NULL;
      }
    }
    if (sel13) {
      if (suite != CIPHER_AES128_GCM_SHA256 || !have_ks || !tls13_handshake(c, our_priv, server_pub)) {
        if (c->ech == 1) {
          ech->rejected = 1;
          ech->retrylen = c->retrylen;
          memcpy(ech->retry, c->retry, c->retrylen);
        }
        net_close(c->tcp);
        free(c);
        return NULL;
      }
      secure_wipe(our_priv, sizeof our_priv); /* ephemeral key done */
      return c;
    }
    /* RFC 8446 §4.1.3 downgrade protection: this client always offers TLS 1.3,
     * so a server that answers with 1.2 while itself supporting 1.3 stamps a
     * sentinel into the last 8 bytes of its random ("DOWNGRD" + 01/00). Its
     * presence means the 1.3 offer was stripped in flight — abort. (Extra
     * defense today given certs aren't verified; load-bearing once they are.)
     */
    {
      static const uint8_t dg12[8] = {0x44, 0x4F, 0x57, 0x4E, 0x47, 0x52, 0x44, 0x01};
      static const uint8_t dg11[8] = {0x44, 0x4F, 0x57, 0x4E, 0x47, 0x52, 0x44, 0x00};
      if (ct_equal(server_random + 24, dg12, 8) || ct_equal(server_random + 24, dg11, 8)) {
        net_close(c->tcp);
        free(c);
        return NULL;
      }
    }
    if (suite != CIPHER_ECDHE_RSA_AES128_GCM) {
      net_close(c->tcp);
      free(c);
      return NULL;
    }
  }

  /* --- TLS 1.2 continuation: ServerKeyExchange .. ServerHelloDone --- */
  int have_ske = 0;
  for (;;) {
    uint8_t mt, *body;
    uint32_t bl;
    if (hs_next(c, &mt, &body, &bl) < 0) {
      net_close(c->tcp);
      free(c);
      return NULL;
    }
    if (mt == HS_SKE) {
      /* curve_type(1) named_curve(2) pubkeylen(1) pubkey ... */
      if (bl < 4 || body[0] != 3 || (((uint16_t)body[1] << 8) | body[2]) != 0x001D || body[3] != 32 || bl < 4u + 32) {
        net_close(c->tcp);
        free(c);
        return NULL;
      }
      memcpy(server_pub, body + 4, 32);
      have_ske = 1;
    } else if (mt == HS_CERT_REQ) {
      net_close(c->tcp); /* client cert (unsupported) */
      free(c);
      return NULL;
    } else if (mt == HS_DONE) {
      break;
    }
    /* HS_CERT and anything else: ignored (no verification). */
  }
  if (!have_ske) {
    net_close(c->tcp);
    free(c);
    return NULL;
  }

  /* --- key schedule --- */
  uint8_t pre_master[32];
  x25519(pre_master, our_priv, server_pub);
  if (is_all_zero(pre_master, 32)) { /* small-order point: reject */
    net_close(c->tcp);
    free(c);
    return NULL;
  }

  uint8_t seed[64], master[48], keyblock[40];
  memcpy(seed, client_random, 32);
  memcpy(seed + 32, server_random, 32);
  tls12_prf(pre_master, 32, "master secret", seed, 64, master, 48);

  memcpy(seed, server_random, 32);
  memcpy(seed + 32, client_random, 32);
  tls12_prf(master, 48, "key expansion", seed, 64, keyblock, 40);
  memcpy(c->client_key, keyblock, 16);
  memcpy(c->server_key, keyblock + 16, 16);
  memcpy(c->client_iv, keyblock + 32, 4);
  memcpy(c->server_iv, keyblock + 36, 4);

  /* --- ClientKeyExchange --- */
  uint8_t cke[4 + 1 + 32];
  cke[0] = HS_CKE;
  cke[1] = 0;
  cke[2] = 0;
  cke[3] = 33;
  cke[4] = 32;
  memcpy(cke + 5, our_pub, 32);
  sha256_update(&c->transcript, cke, sizeof cke);
  if (send_record(c, REC_HS, cke, sizeof cke) < 0) {
    net_close(c->tcp);
    free(c);
    return NULL;
  }

  /* --- ChangeCipherSpec, then switch our write side to encrypted --- */
  uint8_t one = 1;
  send_record(c, REC_CCS, &one, 1);
  c->send_encrypted = 1;
  c->client_seq = 0;

  /* --- client Finished --- */
  struct sha256 tc = c->transcript;
  uint8_t thash[32];
  sha256_final(&tc, thash);
  uint8_t verify[12];
  tls12_prf(master, 48, "client finished", thash, 32, verify, 12);
  uint8_t fin[16];
  fin[0] = HS_FINISHED;
  fin[1] = 0;
  fin[2] = 0;
  fin[3] = 12;
  memcpy(fin + 4, verify, 12);
  sha256_update(&c->transcript, fin, 16);
  if (send_record(c, REC_HS, fin, 16) < 0) {
    net_close(c->tcp);
    free(c);
    return NULL;
  }

  /* expected server Finished (transcript now includes client Finished) */
  struct sha256 tc2 = c->transcript;
  uint8_t thash2[32], server_verify[12];
  sha256_final(&tc2, thash2);
  tls12_prf(master, 48, "server finished", thash2, 32, server_verify, 12);

  /* --- server ChangeCipherSpec --- */
  uint8_t t;
  uint32_t l;
  if (recv_record(c, &t, c->recpt, &l) < 0 || t != REC_CCS) {
    net_close(c->tcp);
    free(c);
    return NULL;
  }
  c->recv_encrypted = 1;
  c->server_seq = 0;
  c->hslen = c->hspos = 0;

  /* --- server Finished (encrypted handshake record) --- */
  uint8_t mt, *body;
  uint32_t bl;
  if (hs_next(c, &mt, &body, &bl) < 0 || mt != HS_FINISHED || bl != 12) {
    net_close(c->tcp);
    free(c);
    return NULL;
  }
  if (!ct_equal(body, server_verify, 12)) { /* server Finished mismatch */
    net_close(c->tcp);
    free(c);
    return NULL;
  }

  /* Handshake secrets are done with; wipe them before returning. */
  secure_wipe(our_priv, sizeof our_priv);
  secure_wipe(pre_master, sizeof pre_master);
  secure_wipe(master, sizeof master);
  secure_wipe(keyblock, sizeof keyblock);
  secure_wipe(seed, sizeof seed);

  c->established = 1;
  return c;
}

/* --------------------------------------------------------- app data I/O */

int tls_write(tls_conn *c, const void *data, int len)
{
  if (!c || !c->established || len < 0)
    return -1;
  const uint8_t *p = data;
  uint32_t total = (uint32_t)len, sent = 0;
  while (sent < total) {
    uint32_t chunk = total - sent;
    if (chunk > 16384)
      chunk = 16384;
    if (send_record(c, REC_APP, p + sent, chunk) < 0)
      return c->intr ? NET_EINTR : -1;
    sent += chunk;
  }
  return len;
}

/* Offer ECH when the host publishes a usable config. A rejection retries once with the server's
 * retry_configs, then falls back to a plain handshake. */
tls_conn *tls_client(const char *host, int port, int offer_h2)
{
  uint8_t list[ECH_RETRY_MAX];
  struct ech_cfg cfg;
  struct ech_try ech;
  int n = resolve_ech(host, port, list, sizeof list);
  if (n && ech_pick_config(list, (uint32_t)n, &cfg)) {
    for (int attempt = 0; attempt < 2; attempt++) {
      memset(&ech, 0, sizeof ech);
      ech.cfg = &cfg;
      tls_conn *c = tls_open(host, port, offer_h2, &ech);
      if (c || !ech.rejected)
        return c;
      memcpy(list, ech.retry, ech.retrylen); /* cfg points into list */
      if (!ech_pick_config(list, ech.retrylen, &cfg))
        break;
    }
  }
  return tls_open(host, port, offer_h2, NULL);
}

int tls_read(tls_conn *c, void *buf, int max)
{
  if (!c || max <= 0)
    return -1;
  uint32_t umax = (uint32_t)max;
  if (c->apppos < c->applen) {
    uint32_t n = c->applen - c->apppos;
    if (n > umax)
      n = umax;
    memcpy(buf, c->appbuf + c->apppos, n);
    c->apppos += n;
    return (int)n;
  }
  if (!c->established)
    return 0;

  for (;;) {
    uint8_t t;
    uint32_t l;
    if (recv_record(c, &t, c->recpt, &l) < 0)
      return c->intr ? NET_EINTR : 0; /* NET_EINTR passthrough, else EOF */
    if (t == REC_APP) {
      if (l == 0)
        continue;
      memcpy(c->appbuf, c->recpt, l);
      c->applen = l;
      c->apppos = 0;
      uint32_t n = l < umax ? l : umax;
      memcpy(buf, c->appbuf, n);
      c->apppos = n;
      return (int)n;
    }
    if (t == REC_ALERT) /* close_notify or fatal */
      return 0;
    /* REC_HS (e.g. NewSessionTicket) and others: ignore, keep reading. */
  }
}

void tls_close(tls_conn *c)
{
  if (!c)
    return;
  if (c->established) {
    uint8_t alert[2] = {1, 0}; /* warning, close_notify */
    send_record(c, REC_ALERT, alert, 2);
  }
  c->established = 0;
  net_close(c->tcp);
  /* Wipe the session keys/IVs before the heap block is reused. */
  secure_wipe(c, sizeof *c);
  free(c);
}
