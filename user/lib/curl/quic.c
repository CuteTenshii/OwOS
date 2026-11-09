/* quic.c — a minimal QUIC v1 client (RFC 9000 transport, RFC 9001 TLS) in ring
 * 3, over a SK_UDP socket. See quic.h for the contract and scope notes.
 *
 * Layout: varint + byte helpers; the TLS 1.3 key schedule (HKDF-Expand-Label,
 * mirrored from kernel/tls.c) + QUIC key derivation; packet protection
 * (AES-128-GCM payload, AES-ECB header protection); the receive path (parse a
 * datagram's coalesced packets, decrypt, dispatch frames); the TLS handshake
 * carried in CRYPTO frames; and the public connect/send/read/poll/close API. */

#include "quic.h"
#include "certs.h"
#include "crypto.h"
#include "owos.h"
#include "resolve.h"

/* ----------------------------------------------------------------- helpers */

static void put16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}

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
  if (v < 0x40) {
    p[0] = (uint8_t)v;
    return 1;
  }
  if (v < 0x4000) {
    p[0] = 0x40 | (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
    return 2;
  }
  if (v < 0x40000000) {
    p[0] = 0x80 | (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
    return 4;
  }
  for (int i = 0; i < 8; i++)
    p[i] = (uint8_t)(v >> (56 - 8 * i));
  p[0] |= 0xc0;
  return 8;
}

static int varsize(uint64_t v) { return v < 0x40 ? 1 : v < 0x4000 ? 2 : v < 0x40000000 ? 4 : 8; }

/* ClientHello.random, the ephemeral x25519 key and the connection IDs come from
 * the shared userspace CSPRNG (rng_bytes: RDRAND-backed). */
/* ------------------------------------------------------- connection state */

enum { S_INIT = 0, S_HS = 1, S_APP = 2 }; /* packet-number spaces */

#define SENT_MAX 32 /* ack-eliciting packets in flight per space */
#define RR_MAX 16   /* received packet-number ranges we report */
#define PTO_MAX 8   /* probe timeouts in a row before the connection is given up */

/* An ack-eliciting packet awaiting its ACK; `frames` (a copy) is NULL in a free slot. */
struct sent_pkt {
  uint64_t pn;
  unsigned t; /* msec() when sent */
  uint32_t len;
  uint8_t *frames;
};

struct range {
  uint64_t lo, hi;
};

struct space {
  int hk;                         /* keys installed                       */
  uint8_t ck[16], ci[12], ch[16]; /* our (client) key / iv / hp    */
  uint8_t sk[16], si[12], sh[16]; /* their (server) key / iv / hp  */
  uint64_t spn;                   /* next packet number to send           */
  uint64_t lrec;                  /* largest packet number received       */
  int hrec;                       /* received any packet                   */
  int ackp;                       /* an ACK is owed in this space          */
  struct range rr[RR_MAX];        /* packet numbers accepted, highest range first */
  int nrr;
  struct sent_pkt sent[SENT_MAX];
  int has_acked;
  uint64_t largest_acked;
  unsigned loss_time; /* when the youngest packet below the largest ACKed would count as lost; 0 = none */
  /* CRYPTO-stream reassembly (in-order; offset 0 based) */
  uint8_t cbuf[16384];
  uint32_t chave; /* contiguous bytes available           */
  uint32_t crd;   /* TLS-message read cursor              */
};

#define QS_MAX 24
#define QS_BUF 4096
struct qstream {
  int used;
  uint64_t id;
  uint64_t recv_off; /* in-order bytes received so far        */
  uint64_t send_off; /* bytes we've sent                      */
  int fin;
  uint64_t fin_off;
  uint8_t buf[QS_BUF]; /* unread, in-order data [r_pos,r_len)   */
  uint32_t r_pos, r_len;
};

/* One connection: everything the packet, TLS and stream layers share. Heap-allocated, so any number of
 * connections can run at once; the packet scratch buffers live here too (they are too big for a small stack). */
struct quic {
  int sock;
  int closed;
  struct space sp[3];
  struct qstream qs[QS_MAX];

  uint8_t scid[8];
  int scid_len; /* our source CID      */
  uint8_t dcid[20];
  int dcid_len; /* peer (server) CID   */
  int server_cid_set;
  uint8_t retry_token[256];
  int retry_token_len;
  int did_retry;

  struct sha256 transcript;
  uint8_t our_priv[32], our_pub[32];
  uint8_t c_hs[32], s_hs[32], master[32];
  int got_sh, app_done, hs_sent_fin, hs_confirmed;
  uint8_t pending_fin[40];
  int pending_fin_len;

  uint8_t chello[1024];
  uint32_t chello_len; /* kept for retransmit */

  /* Round-trip estimate (RFC 9002 §5) and probe backoff. */
  unsigned srtt, rttvar, latest_rtt;
  int rtt_init, pto_count;
  int init_dropped; /* the Initial space is retired once Handshake packets flow */

  /* The server's certificate, checked against `host` while the handshake runs. */
  char host[256];
  struct certs certs;
  int cert_fail;

  /* Flow control and limits. rx_* is what we grant the server; peer_* what it grants us (server transport
   * parameters, raised by MAX_DATA / MAX_STREAMS frames). */
  uint64_t rx_total, rx_limit;
  uint64_t tx_total, peer_max_data;
  uint64_t peer_stream_data;
  uint64_t peer_max_bidi;
  uint64_t peer_idle_ms;
  uint64_t next_bidi; /* index of the next client bidi stream to open */

  uint8_t tx_out[2048], tx_pt[1600], tx_cf[1200], tx_sf[1200];
  uint8_t rx_dg[2048], rx_pt[2048];
};

static const uint8_t x25519_base[32] = {9};

/* ----------------------------------------------- TLS 1.3 / QUIC key schedule
 */

/* HKDF-Expand-Label (RFC 8446 §7.1): "tls13 "-prefixed label + a context. */
static void expand_label(const uint8_t secret[32], const char *label, const uint8_t *ctx, uint8_t ctxlen, uint8_t *out,
                         uint16_t outlen)
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
  expand_label(secret, label, thash, 32, out, 32);
}

/* Install one space's packet-protection keys from its client/server secrets. */
static void install_space(struct quic *Q, int space, const uint8_t cli[32], const uint8_t srv[32])
{
  struct space *s = &Q->sp[space];
  expand_label(cli, "quic key", 0, 0, s->ck, 16);
  expand_label(cli, "quic iv", 0, 0, s->ci, 12);
  expand_label(cli, "quic hp", 0, 0, s->ch, 16);
  expand_label(srv, "quic key", 0, 0, s->sk, 16);
  expand_label(srv, "quic iv", 0, 0, s->si, 12);
  expand_label(srv, "quic hp", 0, 0, s->sh, 16);
  s->hk = 1;
}

/* Initial secrets (RFC 9001 §5.2): HKDF-Extract(v1 salt, DCID) then "client in"
 * / "server in" expand. Reset the Initial space's counters too (used on Retry).
 */
static void derive_initial_keys(struct quic *Q, const uint8_t *cid, int cidlen)
{
  static const uint8_t salt[20] = {0x38, 0x76, 0x2c, 0xf7, 0xf5, 0x59, 0x34, 0xb3, 0x4d, 0x17,
                                   0x9a, 0xe6, 0xa4, 0xc8, 0x0c, 0xad, 0xcc, 0xbb, 0x7f, 0x0a};
  uint8_t isecret[32], cs[32], ss[32];
  hkdf_extract(salt, 20, cid, (uint32_t)cidlen, isecret);
  expand_label(isecret, "client in", 0, 0, cs, 32);
  expand_label(isecret, "server in", 0, 0, ss, 32);
  Q->sp[S_INIT].spn = 0;
  Q->sp[S_INIT].lrec = 0;
  Q->sp[S_INIT].hrec = 0;
  Q->sp[S_INIT].ackp = 0;
  Q->sp[S_INIT].chave = Q->sp[S_INIT].crd = 0;
  install_space(Q, S_INIT, cs, ss);
}

static void th_snapshot(struct quic *Q, uint8_t out[32])
{
  struct sha256 c = Q->transcript;
  sha256_final(&c, out);
}

/* ------------------------------------------------------------- ClientHello */

static const uint8_t sigalgs[] = {0x08, 0x04, 0x08, 0x05, 0x08, 0x06,  /* rsa_pss_rsae_sha256/384/512 */
                                  0x04, 0x01, 0x05, 0x01, 0x06, 0x01,  /* rsa_pkcs1_sha256/384/512    */
                                  0x04, 0x03, 0x05, 0x03, 0x06, 0x03}; /* ecdsa_secp256r1/384/521     */

/* QUIC transport parameters (TLS extension 0x39). */
static uint32_t build_tp(struct quic *Q, uint8_t *b)
{
  uint32_t p = 0;
  struct {
    uint64_t id, val;
  } ints[] = {
      {0x01, 30000},     /* max_idle_timeout (ms)                 */
      {0x03, 1472},      /* max_udp_payload_size                  */
      {0x04, 0x1000000}, /* initial_max_data                      */
      {0x05, 0x1000000}, /* initial_max_stream_data_bidi_local    */
      {0x06, 0x100000},  /* initial_max_stream_data_bidi_remote   */
      {0x07, 0x100000},  /* initial_max_stream_data_uni           */
      {0x08, 0},         /* initial_max_streams_bidi              */
      {0x09, 3},         /* initial_max_streams_uni               */
      {0x0e, 3},         /* active_connection_id_limit            */
  };
  for (unsigned i = 0; i < sizeof ints / sizeof ints[0]; i++) {
    p += putvar(b + p, ints[i].id);
    p += putvar(b + p, (uint64_t)varsize(ints[i].val));
    p += putvar(b + p, ints[i].val);
  }
  /* initial_source_connection_id (0x0f): raw CID bytes */
  p += putvar(b + p, 0x0f);
  p += putvar(b + p, (uint64_t)Q->scid_len);
  memcpy(b + p, Q->scid, Q->scid_len);
  p += Q->scid_len;
  return p;
}

/* Build a TLS 1.3 ClientHello (4-byte HS header included) for QUIC: x25519 key
 * share, SNI, ALPN, and the QUIC transport-parameters extension. */
static uint32_t build_client_hello(struct quic *Q, uint8_t *out, const char *sni, const char *alpn)
{
  uint32_t namelen = sni ? (uint32_t)strlen(sni) : 0;
  uint32_t alpnlen = (uint32_t)strlen(alpn);
  uint8_t client_random[32];
  rng_bytes(client_random, 32);

  uint8_t *p = out + 4;
  *p++ = 0x03;
  *p++ = 0x03; /* legacy_version = TLS 1.2 */
  memcpy(p, client_random, 32);
  p += 32;
  *p++ = 0; /* legacy_session_id: empty */

  put16(p, 2);
  p += 2; /* cipher_suites */
  put16(p, 0x1301);
  p += 2; /* TLS_AES_128_GCM_SHA256 */
  *p++ = 1;
  *p++ = 0; /* compression: null */

  uint8_t *extlen = p;
  p += 2;
  uint8_t *exts = p;

  if (namelen) { /* server_name */
    put16(p, 0x0000);
    p += 2;
    put16(p, (uint16_t)(namelen + 5));
    p += 2;
    put16(p, (uint16_t)(namelen + 3));
    p += 2;
    *p++ = 0;
    put16(p, (uint16_t)namelen);
    p += 2;
    memcpy(p, sni, namelen);
    p += namelen;
  }
  put16(p, 0x000A);
  p += 2; /* supported_groups: x25519 */
  put16(p, 4);
  p += 2;
  put16(p, 2);
  p += 2;
  put16(p, 0x001D);
  p += 2;
  put16(p, 0x000D);
  p += 2; /* signature_algorithms */
  put16(p, (uint16_t)(sizeof sigalgs + 2));
  p += 2;
  put16(p, (uint16_t)sizeof sigalgs);
  p += 2;
  memcpy(p, sigalgs, sizeof sigalgs);
  p += sizeof sigalgs;
  put16(p, 0x002B);
  p += 2; /* supported_versions: 1.3 only */
  put16(p, 3);
  p += 2;
  *p++ = 2;
  put16(p, 0x0304);
  p += 2;
  put16(p, 0x0033);
  p += 2; /* key_share: x25519 */
  put16(p, 38);
  p += 2;
  put16(p, 36);
  p += 2;
  put16(p, 0x001D);
  p += 2;
  put16(p, 32);
  p += 2;
  memcpy(p, Q->our_pub, 32);
  p += 32;
  put16(p, 0x0010);
  p += 2; /* ALPN */
  put16(p, (uint16_t)(alpnlen + 3));
  p += 2;
  put16(p, (uint16_t)(alpnlen + 1));
  p += 2;
  *p++ = (uint8_t)alpnlen;
  memcpy(p, alpn, alpnlen);
  p += alpnlen;
  put16(p, 0x0039);
  p += 2; /* quic_transport_parameters */
  uint8_t *tplen = p;
  p += 2;
  uint32_t tpn = build_tp(Q, p);
  p += tpn;
  put16(tplen, (uint16_t)tpn);

  put16(extlen, (uint16_t)(p - exts));

  uint32_t body = (uint32_t)(p - (out + 4));
  out[0] = 1; /* HS type = ClientHello */
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
  off += 3; /* suite(2) + compression(1) */
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
      uint16_t kl = ((uint16_t)body[off + 2] << 8) | body[off + 3];
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
static int on_server_hello(struct quic *Q, const uint8_t *body, uint32_t bl)
{
  uint8_t server_pub[32];
  if (parse_server_hello(body, bl, server_pub) < 0)
    return -1;

  uint8_t zero[32] = {0}, empty_hash[32];
  sha256("", 0, empty_hash);

  uint8_t early[32], derived[32], shared[32], hs_secret[32];
  hkdf_extract(zero, 32, zero, 32, early);
  expand_label(early, "derived", empty_hash, 32, derived, 32);
  x25519(shared, Q->our_priv, server_pub);
  hkdf_extract(derived, 32, shared, 32, hs_secret);

  uint8_t th[32];
  th_snapshot(Q, th); /* transcript ..ServerHello */
  derive_secret(hs_secret, "c hs traffic", th, Q->c_hs);
  derive_secret(hs_secret, "s hs traffic", th, Q->s_hs);
  install_space(Q, S_HS, Q->c_hs, Q->s_hs);

  uint8_t derived2[32];
  expand_label(hs_secret, "derived", empty_hash, 32, derived2, 32);
  hkdf_extract(derived2, 32, zero, 32, Q->master);
  Q->got_sh = 1;
  return 0;
}

/* Server Finished received (th_pre = transcript through CertificateVerify):
 * check the certificate verdict and the Finished, derive + install 1-RTT keys, and stage the client Finished. */
static int on_server_finished(struct quic *Q, const uint8_t *body, uint32_t bl, const uint8_t th_pre[32])
{
  if (bl != 32)
    return -1;
  /* The connection goes ahead only with a certificate that checks out for this host (or when there is no root
   * bundle to check against, as for TLS over TCP). */
  if (Q->certs.verify != X509_OK && Q->certs.verify != X509_E_NO_STORE) {
    Q->cert_fail = 1;
    return -1;
  }
  uint8_t fk[32], expect[32];
  expand_label(Q->s_hs, "finished", 0, 0, fk, 32);
  hmac_sha256(fk, 32, th_pre, 32, expect);
  for (int i = 0; i < 32; i++)
    if (body[i] != expect[i])
      return -1;

  uint8_t th_sf[32];
  th_snapshot(Q, th_sf); /* transcript ..server Finished */

  uint8_t fkc[32], verify[32];
  expand_label(Q->c_hs, "finished", 0, 0, fkc, 32);
  hmac_sha256(fkc, 32, th_sf, 32, verify);
  Q->pending_fin[0] = 20;
  Q->pending_fin[1] = 0;
  Q->pending_fin[2] = 0;
  Q->pending_fin[3] = 32;
  memcpy(Q->pending_fin + 4, verify, 32);
  Q->pending_fin_len = 36;

  uint8_t c_ap[32], s_ap[32];
  derive_secret(Q->master, "c ap traffic", th_sf, c_ap);
  derive_secret(Q->master, "s ap traffic", th_sf, s_ap);
  install_space(Q, S_APP, c_ap, s_ap);
  Q->app_done = 1;
  return 0;
}

/* EncryptedExtensions: read the limits the server grants us out of its QUIC transport parameters (extension
 * 0x39); a parameter it leaves out is 0. */
static void parse_server_tp(struct quic *Q, const uint8_t *b, uint32_t len)
{
  if (len < 2)
    return;
  uint32_t o = 2, end = 2 + (((uint32_t)b[0] << 8) | b[1]);
  if (end > len)
    end = len;
  while (o + 4 <= end) {
    uint16_t et = (uint16_t)((b[o] << 8) | b[o + 1]), el = (uint16_t)((b[o + 2] << 8) | b[o + 3]);
    o += 4;
    if (o + el > end)
      return;
    if (et == 0x0039) {
      uint32_t p = o, pe = o + el;
      uint64_t id, vl, v;
      while (getvar(b, &p, pe, &id) && getvar(b, &p, pe, &vl) && p + vl <= pe) {
        uint32_t vo = p;
        if (vl && getvar(b, &vo, p + (uint32_t)vl, &v)) {
          if (id == 0x01)
            Q->peer_idle_ms = v;
          else if (id == 0x04)
            Q->peer_max_data = v;
          else if (id == 0x06)
            Q->peer_stream_data = v;
          else if (id == 0x08)
            Q->peer_max_bidi = v;
        }
        p += (uint32_t)vl;
      }
    }
    o += el;
  }
}

/* Extract complete TLS handshake messages from a space's CRYPTO buffer,
 * feeding each into the transcript hash and dispatching it. */
static void extract_tls(struct quic *Q, int space)
{
  struct space *s = &Q->sp[space];
  for (;;) {
    if (s->crd + 4 > s->chave)
      return;
    uint8_t mt = s->cbuf[s->crd];
    uint32_t l = ((uint32_t)s->cbuf[s->crd + 1] << 16) | ((uint32_t)s->cbuf[s->crd + 2] << 8) | s->cbuf[s->crd + 3];
    uint32_t total = 4 + l;
    if (s->crd + total > s->chave)
      return;
    const uint8_t *msg = s->cbuf + s->crd;

    uint8_t th_pre[32];
    if (mt == 20 || mt == 15) /* Finished, CertificateVerify: hash BEFORE update */
      th_snapshot(Q, th_pre);
    sha256_update(&Q->transcript, msg, total);

    if (mt == 11) /* Certificate */
      certs_take(&Q->certs, Q->host, msg + 4, l, 1);
    if (mt == 15) /* CertificateVerify */
      certs_check_cv(&Q->certs, th_pre, msg + 4, l);

    if (mt == 8) /* EncryptedExtensions */
      parse_server_tp(Q, msg + 4, l);
    if (mt == 2) /* ServerHello */
      on_server_hello(Q, msg + 4, l);
    else if (mt == 20) /* Finished */
      on_server_finished(Q, msg + 4, l, th_pre);

    s->crd += total;
  }
}

static void crypto_recv(struct quic *Q, int space, uint64_t off, const uint8_t *data, uint32_t n)
{
  struct space *s = &Q->sp[space];
  if (off > s->chave) /* gap (assume in-order): hold */
    return;
  if (off + n > sizeof s->cbuf)
    n = (uint32_t)(sizeof s->cbuf - off);
  memcpy(s->cbuf + off, data, n);
  if (off + n > s->chave)
    s->chave = (uint32_t)(off + n);
  extract_tls(Q, space);
}

/* ------------------------------------------------------------- streams */

static struct qstream *find_stream(struct quic *Q, uint64_t id)
{
  for (int i = 0; i < QS_MAX; i++)
    if (Q->qs[i].used && Q->qs[i].id == id)
      return &Q->qs[i];
  return 0;
}

static struct qstream *get_stream(struct quic *Q, uint64_t id)
{
  for (int i = 0; i < QS_MAX; i++)
    if (Q->qs[i].used && Q->qs[i].id == id)
      return &Q->qs[i];
  for (int i = 0; i < QS_MAX; i++)
    if (!Q->qs[i].used) {
      memset(&Q->qs[i], 0, sizeof Q->qs[i]);
      Q->qs[i].used = 1;
      Q->qs[i].id = id;
      return &Q->qs[i];
    }
  return 0;
}

static void stream_recv(struct quic *Q, uint64_t id, uint64_t off, const uint8_t *data, uint32_t n, int fin)
{
  if ((id & 0x03) != 0) /* not client-bidi: discard */
    return;
  struct qstream *q = find_stream(Q, id); /* a finished stream's late retransmission finds nothing: ignored */
  if (!q)
    return;
  if (fin) {
    q->fin = 1;
    q->fin_off = off + n;
  }
  if (off > q->recv_off) /* gap (in-order assumption) */
    return;
  uint64_t end = off + n;
  if (end <= q->recv_off) /* duplicate */
    return;
  uint32_t skip = (uint32_t)(q->recv_off - off);
  const uint8_t *src = data + skip;
  uint32_t cnt = (uint32_t)(end - q->recv_off);

  if (q->r_pos > 0) { /* compact consumed prefix */
    memmove(q->buf, q->buf + q->r_pos, q->r_len - q->r_pos);
    q->r_len -= q->r_pos;
    q->r_pos = 0;
  }
  if (q->r_len + cnt > QS_BUF)
    cnt = QS_BUF - q->r_len; /* truncate on overflow */
  memcpy(q->buf + q->r_len, src, cnt);
  q->r_len += cnt;
  q->recv_off += cnt;
  Q->rx_total += cnt;
}

/* ------------------------------------------------------ loss recovery */

/* RFC 9002 in small: every ack-eliciting packet is kept until acknowledged; one the peer's ACKs skip past (three
 * packets, or a time threshold) or that no ACK covers within the probe timeout is sent again under a new packet
 * number. There is no congestion controller: the number of packets in flight is capped at SENT_MAX per space instead.
 * The receive side acknowledges exactly the packet numbers it accepted, and drops (without acknowledging) a packet
 * whose stream or CRYPTO data lies beyond a gap, so the peer resends it. */

static int emit(struct quic *Q, int space, const uint8_t *frames, uint32_t flen, int track);
static int poll_once(struct quic *Q);

static unsigned qnow(void) { return msec(); }

static int rr_has(const struct space *s, uint64_t pn)
{
  for (int i = 0; i < s->nrr; i++)
    if (pn >= s->rr[i].lo && pn <= s->rr[i].hi)
      return 1;
  return 0;
}

static void rr_remove(struct space *s, int i)
{
  memmove(&s->rr[i], &s->rr[i + 1], (unsigned)(s->nrr - i - 1) * sizeof s->rr[0]);
  s->nrr--;
}

/* Note packet number `pn` as received: ranges stay disjoint, non-adjacent, highest first. */
static void rr_add(struct space *s, uint64_t pn)
{
  int i;
  for (i = 0; i < s->nrr; i++) {
    struct range *r = &s->rr[i];
    if (pn >= r->lo && pn <= r->hi)
      return;
    if (pn == r->hi + 1) { /* extends this range upwards, maybe reaching the one above */
      r->hi = pn;
      if (i > 0 && s->rr[i - 1].lo == pn + 1) {
        r->hi = s->rr[i - 1].hi;
        rr_remove(s, i - 1);
      }
      return;
    }
    if (r->lo > 0 && pn == r->lo - 1) { /* downwards, maybe reaching the one below */
      r->lo = pn;
      if (i + 1 < s->nrr && s->rr[i + 1].hi + 1 == pn) {
        r->lo = s->rr[i + 1].lo;
        rr_remove(s, i + 1);
      }
      return;
    }
    if (pn > r->hi)
      break;
  }
  if (s->nrr == RR_MAX) {
    if (i == RR_MAX)
      return; /* older than everything we still track */
    s->nrr--; /* forget the oldest range */
  }
  memmove(&s->rr[i + 1], &s->rr[i], (unsigned)(s->nrr - i) * sizeof s->rr[0]);
  s->rr[i].lo = s->rr[i].hi = pn;
  s->nrr++;
}

static struct sent_pkt *sent_slot(struct space *s)
{
  for (int i = 0; i < SENT_MAX; i++)
    if (!s->sent[i].frames)
      return &s->sent[i];
  return 0;
}

static void sent_clear(struct space *s)
{
  for (int i = 0; i < SENT_MAX; i++) {
    free(s->sent[i].frames);
    s->sent[i].frames = 0;
  }
  s->loss_time = 0;
}

/* A packet's frames sent again in a new packet; the old record is dropped either way. */
static void retransmit(struct quic *Q, int space, struct sent_pkt *p)
{
  uint8_t *frames = p->frames;
  uint32_t len = p->len;
  p->frames = 0;
  emit(Q, space, frames, len, 1);
  free(frames);
}

/* Declare lost every packet below the largest acknowledged one that is three packets or a round trip (+1/8) old,
 * resend them, and note when the youngest of the rest would become lost. */
static void detect_lost(struct quic *Q, int space)
{
  struct space *s = &Q->sp[space];
  s->loss_time = 0;
  if (!s->has_acked)
    return;
  unsigned now = qnow(), rtt = Q->latest_rtt > Q->srtt ? Q->latest_rtt : Q->srtt;
  unsigned delay = rtt + rtt / 8;
  if (delay < 2)
    delay = 2;
  for (int i = 0; i < SENT_MAX; i++) {
    struct sent_pkt *p = &s->sent[i];
    if (!p->frames || p->pn >= s->largest_acked)
      continue;
    if (p->pn + 3 <= s->largest_acked || now - p->t >= delay) {
      retransmit(Q, space, p);
    } else {
      unsigned due = p->t + delay;
      if (!s->loss_time || (int)(due - s->loss_time) < 0)
        s->loss_time = due;
    }
  }
}

struct ackinfo {
  uint64_t largest;
  struct range r[32];
  int n;
};

/* Read the body of an ACK frame (after its type); 0 if malformed. Ranges beyond the 32 we keep are skipped. */
static int read_ack(const uint8_t *p, uint32_t *o, uint32_t len, int ecn, struct ackinfo *ai)
{
  uint64_t delay, count, first, gap, rlen, x;
  if (!getvar(p, o, len, &ai->largest) || !getvar(p, o, len, &delay) || !getvar(p, o, len, &count) ||
      !getvar(p, o, len, &first) || first > ai->largest)
    return 0;
  uint64_t lo = ai->largest - first;
  ai->r[0].hi = ai->largest;
  ai->r[0].lo = lo;
  ai->n = 1;
  for (uint64_t i = 0; i < count; i++) {
    if (!getvar(p, o, len, &gap) || !getvar(p, o, len, &rlen) || lo < gap + 2)
      return 0;
    uint64_t hi = lo - gap - 2;
    if (rlen > hi)
      return 0;
    lo = hi - rlen;
    if (ai->n < 32) {
      ai->r[ai->n].hi = hi;
      ai->r[ai->n].lo = lo;
      ai->n++;
    }
  }
  if (ecn && (!getvar(p, o, len, &x) || !getvar(p, o, len, &x) || !getvar(p, o, len, &x)))
    return 0;
  return 1;
}

static void rtt_sample(struct quic *Q, unsigned rtt)
{
  Q->latest_rtt = rtt;
  if (!Q->rtt_init) {
    Q->rtt_init = 1;
    Q->srtt = rtt;
    Q->rttvar = rtt / 2;
    return;
  }
  unsigned diff = Q->srtt > rtt ? Q->srtt - rtt : rtt - Q->srtt;
  Q->rttvar = (3 * Q->rttvar + diff) / 4;
  Q->srtt = (7 * Q->srtt + rtt) / 8;
}

/* An ACK arrived in `space`: retire what it covers, take an RTT sample from the largest, then look for losses. */
static void apply_ack(struct quic *Q, int space, const struct ackinfo *ai)
{
  struct space *s = &Q->sp[space];
  int newly = 0;
  for (int i = 0; i < SENT_MAX; i++) {
    struct sent_pkt *p = &s->sent[i];
    if (!p->frames)
      continue;
    for (int k = 0; k < ai->n; k++)
      if (p->pn >= ai->r[k].lo && p->pn <= ai->r[k].hi) {
        if (p->pn == ai->largest)
          rtt_sample(Q, qnow() - p->t);
        free(p->frames);
        p->frames = 0;
        newly = 1;
        break;
      }
  }
  if (!s->has_acked || ai->largest > s->largest_acked) {
    s->largest_acked = ai->largest;
    s->has_acked = 1;
  }
  if (newly)
    Q->pto_count = 0;
  detect_lost(Q, space);
}

/* How long to wait for an ACK before sending a probe. */
static unsigned pto_interval(const struct quic *Q)
{
  unsigned srtt = Q->rtt_init ? Q->srtt : 333, var = Q->rtt_init ? Q->rttvar : 166;
  unsigned base = srtt + (4 * var > 1 ? 4 * var : 1) + 25;
  return base << (Q->pto_count < 6 ? Q->pto_count : 6);
}

/* The next time something needs doing in a space: a packet turning lost, else a probe timeout. 0 = nothing pending.
 */
static unsigned next_deadline(struct quic *Q, int *space, int *probe)
{
  unsigned best = 0;
  *probe = 0;
  for (int sp = 0; sp < 3; sp++) {
    struct space *s = &Q->sp[sp];
    if (s->loss_time && (!best || (int)(s->loss_time - best) < 0)) {
      best = s->loss_time;
      *space = sp;
      *probe = 0;
    }
  }
  if (best)
    return best;
  for (int sp = 0; sp < 3; sp++) {
    struct space *s = &Q->sp[sp];
    unsigned last = 0;
    int any = 0;
    for (int i = 0; i < SENT_MAX; i++)
      if (s->sent[i].frames && (!any || (int)(s->sent[i].t - last) > 0)) {
        last = s->sent[i].t;
        any = 1;
      }
    if (!any)
      continue;
    unsigned due = last + pto_interval(Q);
    if (!best || (int)(due - best) < 0) {
      best = due;
      *space = sp;
      *probe = 1;
    }
  }
  return best;
}

/* Fire whatever is due. After too many probes in a row the connection is given up. */
static void run_timers(struct quic *Q)
{
  int space = 0, probe;
  unsigned due = next_deadline(Q, &space, &probe);
  if (!due || (int)(qnow() - due) < 0)
    return;
  if (!probe) {
    detect_lost(Q, space);
    return;
  }
  if (++Q->pto_count > PTO_MAX) {
    Q->closed = 1;
    return;
  }
  struct space *s = &Q->sp[space];
  struct sent_pkt *oldest = 0;
  for (int i = 0; i < SENT_MAX; i++)
    if (s->sent[i].frames && (!oldest || s->sent[i].pn < oldest->pn))
      oldest = &s->sent[i];
  if (oldest)
    retransmit(Q, space, oldest);
}

/* Wait (pumping the connection) until a packet slot is free in `space`; 0 if the connection died or went quiet. */
static int wait_slot(struct quic *Q, int space)
{
  for (int idle = 0; idle < 6; idle++) {
    if (sent_slot(&Q->sp[space]))
      return 1;
    int r = poll_once(Q);
    if (r < 0)
      return 0;
    if (r > 0)
      idle = 0;
  }
  return sent_slot(&Q->sp[space]) != 0;
}

/* ------------------------------------------------------ frame processing */

/* Walk the frames of one decrypted packet. With `dry` nothing changes: it only reports (return 0) whether the
 * packet carries CRYPTO or stream data past a gap, or more than a stream can buffer. */
static int process_frames(struct quic *Q, int space, const uint8_t *p, uint32_t len, int dry)
{
  uint32_t o = 0;
  int ackel = 0;
  while (o < len) {
    uint64_t t;
    if (!getvar(p, &o, len, &t))
      break;
    uint64_t a, b, c;
    switch (t) {
    case 0x00: /* PADDING */
      while (o < len && p[o] == 0x00)
        o++;
      break;
    case 0x01:
      ackel = 1;
      break; /* PING */
    case 0x02:
    case 0x03: { /* ACK */
      struct ackinfo ai;
      if (!read_ack(p, &o, len, t == 0x03, &ai))
        goto done;
      if (!dry)
        apply_ack(Q, space, &ai);
      break;
    }
    case 0x04: /* RESET_STREAM */
      if (!getvar(p, &o, len, &a) || !getvar(p, &o, len, &b) || !getvar(p, &o, len, &c))
        goto done;
      ackel = 1;
      break;
    case 0x05: /* STOP_SENDING */
      if (!getvar(p, &o, len, &a) || !getvar(p, &o, len, &b))
        goto done;
      ackel = 1;
      break;
    case 0x06: { /* CRYPTO */
      uint64_t off, clen;
      if (!getvar(p, &o, len, &off) || !getvar(p, &o, len, &clen) || o + clen > len)
        goto done;
      if (dry) {
        if (off > Q->sp[space].chave || off + clen > sizeof Q->sp[space].cbuf)
          return 0;
      } else {
        crypto_recv(Q, space, off, p + o, (uint32_t)clen);
      }
      o += (uint32_t)clen;
      ackel = 1;
      break;
    }
    case 0x07: /* NEW_TOKEN */
      if (!getvar(p, &o, len, &a))
        goto done;
      o += (uint32_t)a;
      ackel = 1;
      break;
    case 0x08:
    case 0x09:
    case 0x0a:
    case 0x0b: /* STREAM */
    case 0x0c:
    case 0x0d:
    case 0x0e:
    case 0x0f: {
      uint64_t sid, soff = 0, slen;
      if (!getvar(p, &o, len, &sid))
        goto done;
      if ((t & 0x04) && !getvar(p, &o, len, &soff))
        goto done;
      if (t & 0x02) {
        if (!getvar(p, &o, len, &slen))
          goto done;
      } else
        slen = len - o;
      if (o + slen > len)
        goto done;
      if (dry) {
        struct qstream *q = (sid & 3) == 0 ? find_stream(Q, sid) : 0;
        if (q) {
          if (soff > q->recv_off)
            return 0;
          uint64_t fresh = soff + slen > q->recv_off ? soff + slen - q->recv_off : 0;
          if (fresh > QS_BUF - (q->r_len - q->r_pos))
            return 0;
        }
      } else {
        stream_recv(Q, sid, soff, p + o, (uint32_t)slen, (int)(t & 0x01));
      }
      o += (uint32_t)slen;
      ackel = 1;
      break;
    }
    case 0x10: /* MAX_DATA */
      if (!getvar(p, &o, len, &a))
        goto done;
      if (!dry && a > Q->peer_max_data)
        Q->peer_max_data = a;
      ackel = 1;
      break;
    case 0x11: /* MAX_STREAM_DATA */
      if (!getvar(p, &o, len, &a) || !getvar(p, &o, len, &b))
        goto done;
      ackel = 1;
      break;
    case 0x12:
    case 0x13: /* MAX_STREAMS */
      if (!getvar(p, &o, len, &a))
        goto done;
      if (!dry && t == 0x12 && a > Q->peer_max_bidi)
        Q->peer_max_bidi = a;
      ackel = 1;
      break;
    case 0x14: /* DATA_BLOCKED */
      if (!getvar(p, &o, len, &a))
        goto done;
      ackel = 1;
      break;
    case 0x15: /* STREAM_DATA_BLOCKED */
      if (!getvar(p, &o, len, &a) || !getvar(p, &o, len, &b))
        goto done;
      ackel = 1;
      break;
    case 0x16:
    case 0x17: /* STREAMS_BLOCKED */
      if (!getvar(p, &o, len, &a))
        goto done;
      ackel = 1;
      break;
    case 0x18: /* NEW_CONNECTION_ID */
      if (!getvar(p, &o, len, &a) || !getvar(p, &o, len, &b) || o >= len)
        goto done;
      o += 1 + p[o]; /* len byte + CID */
      o += 16;       /* stateless reset token */
      ackel = 1;
      break;
    case 0x19: /* RETIRE_CONNECTION_ID */
      if (!getvar(p, &o, len, &a))
        goto done;
      ackel = 1;
      break;
    case 0x1a:
      o += 8;
      ackel = 1;
      break; /* PATH_CHALLENGE */
    case 0x1b:
      o += 8;
      ackel = 1;
      break;   /* PATH_RESPONSE */
    case 0x1c: /* CONNECTION_CLOSE (transport) */
      if (!getvar(p, &o, len, &a) || !getvar(p, &o, len, &b) || !getvar(p, &o, len, &c))
        goto done;
      if (!dry)
        Q->closed = 1;
      goto done;
    case 0x1d: /* CONNECTION_CLOSE (app) */
      if (!getvar(p, &o, len, &a) || !getvar(p, &o, len, &b))
        goto done;
      if (!dry)
        Q->closed = 1;
      goto done;
    case 0x1e: /* HANDSHAKE_DONE */
      ackel = 1;
      if (!dry) {
        Q->hs_confirmed = 1;
        sent_clear(&Q->sp[S_HS]); /* the Handshake keys are retired */
        Q->sp[S_HS].ackp = 0;
      }
      break;
    default:
      goto done; /* unknown: stop */
    }
  }
done:
  if (!dry && ackel)
    Q->sp[space].ackp = 1;
  return 1;
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
static int decrypt_pkt(struct quic *Q, int space, uint8_t *d, uint32_t pkt0, uint32_t pn_off, uint32_t plen,
                       int is_long)
{
  struct space *s = &Q->sp[space];
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

  uint8_t *pt = Q->rx_pt;
  if (ct_len > sizeof Q->rx_pt)
    return -1;
  if (aes128_gcm_open(s->sk, nonce, aad, al, d + ct_off, ct_len, pt, tag) != 0)
    return -1;

  if (rr_has(s, pn)) { /* a duplicate: acknowledge again, nothing else */
    s->ackp = 1;
    return 0;
  }
  if (!process_frames(Q, space, pt, ct_len, 1))
    return 0; /* data past a gap: dropped unacknowledged, so the server sends it again */
  rr_add(s, pn);
  if (!s->hrec || pn > s->lrec)
    s->lrec = pn;
  s->hrec = 1;
  process_frames(Q, space, pt, ct_len, 0);
  return 0;
}

static void handle_retry(struct quic *Q, const uint8_t *retry_scid, int sl, const uint8_t *p, uint32_t plen)
{
  if (Q->did_retry)
    return;
  Q->did_retry = 1;
  /* Retry layout after the SCID: Retry Token .. then 16-byte integrity tag. */
  if (plen < 16)
    return;
  uint32_t tl = plen - 16;
  if (tl > sizeof Q->retry_token)
    tl = sizeof Q->retry_token;
  memcpy(Q->retry_token, p, tl);
  Q->retry_token_len = (int)tl;
  if (sl > (int)sizeof Q->dcid)
    sl = sizeof Q->dcid;
  memcpy(Q->dcid, retry_scid, sl);
  Q->dcid_len = sl;
  Q->server_cid_set = 0;                        /* adopt the real Initial's SCID next */
  derive_initial_keys(Q, Q->dcid, Q->dcid_len); /* new DCID => new Initial keys */
  sent_clear(&Q->sp[S_INIT]);
  Q->sp[S_INIT].nrr = 0;
}

/* --------------------------------------------------------- packet send */

/* Encrypt+send `frames` as a single packet (its own datagram) in `space`. With `track` the frames are kept until
 * the packet is acknowledged, so they can be sent again if it is lost. */
static int emit_packet(struct quic *Q, int space, const uint8_t *frames, uint32_t flen, uint64_t *pn_out)
{
  struct space *s = &Q->sp[space];
  if (!s->hk)
    return -1;
  uint8_t *out = Q->tx_out; /* packet scratch lives in the connection, off the small user stack */
  uint8_t *pt = Q->tx_pt;
  uint32_t p = 0;
  int is_long = (space != S_APP);
  uint32_t pn_off;

  if (is_long) {
    int type = (space == S_INIT) ? 0 : 2;
    out[p++] = (uint8_t)(0xC0 | (type << 4) | 0x03);
    out[p++] = 0;
    out[p++] = 0;
    out[p++] = 0;
    out[p++] = 1; /* version 1 */
    out[p++] = (uint8_t)Q->dcid_len;
    memcpy(out + p, Q->dcid, Q->dcid_len);
    p += Q->dcid_len;
    out[p++] = (uint8_t)Q->scid_len;
    memcpy(out + p, Q->scid, Q->scid_len);
    p += Q->scid_len;
    if (space == S_INIT) {
      p += putvar(out + p, (uint64_t)Q->retry_token_len);
      if (Q->retry_token_len) {
        memcpy(out + p, Q->retry_token, Q->retry_token_len);
        p += Q->retry_token_len;
      }
    }
    uint32_t len_idx = p;
    p += 2; /* 2-byte varint length */
    pn_off = p;
    uint64_t pn = s->spn;
    out[p++] = (uint8_t)(pn >> 24);
    out[p++] = (uint8_t)(pn >> 16);
    out[p++] = (uint8_t)(pn >> 8);
    out[p++] = (uint8_t)pn;

    uint32_t ptl = flen;
    memcpy(pt, frames, flen);
    if (space == S_INIT) { /* pad datagram to 1200 */
      uint32_t projected = pn_off + 4 + ptl + 16;
      while (projected < 1200 && ptl < sizeof Q->tx_pt) {
        pt[ptl++] = 0x00;
        projected++;
      }
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
    for (int i = 0; i < 4; i++)
      out[pn_off + i] ^= mask[1 + i];
    *pn_out = s->spn++;
    return net_send(Q->sock, out, (int)dglen) < 0 ? -1 : 0;
  }

  out[p++] = 0x43; /* short header, pnlen 4 */
  memcpy(out + p, Q->dcid, Q->dcid_len);
  p += Q->dcid_len;
  pn_off = p;
  uint64_t pn = s->spn;
  out[p++] = (uint8_t)(pn >> 24);
  out[p++] = (uint8_t)(pn >> 16);
  out[p++] = (uint8_t)(pn >> 8);
  out[p++] = (uint8_t)pn;

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
  for (int i = 0; i < 4; i++)
    out[pn_off + i] ^= mask[1 + i];
  *pn_out = s->spn++;
  return net_send(Q->sock, out, (int)dglen) < 0 ? -1 : 0;
}

static int emit(struct quic *Q, int space, const uint8_t *frames, uint32_t flen, int track)
{
  struct space *s = &Q->sp[space];
  if (track && !sent_slot(s) && !wait_slot(Q, space))
    return -1;
  uint64_t pn = 0;
  int rc = emit_packet(Q, space, frames, flen, &pn);
  if (rc == 0 && space == S_HS && !Q->init_dropped) { /* Handshake packets flow: the Initial keys are retired */
    Q->init_dropped = 1;
    sent_clear(&Q->sp[S_INIT]);
    Q->sp[S_INIT].ackp = 0;
  }
  struct sent_pkt *rec = track && rc == 0 ? sent_slot(s) : 0;
  if (rec) {
    rec->frames = malloc(flen);
    if (rec->frames) {
      memcpy(rec->frames, frames, flen);
      rec->pn = pn;
      rec->len = flen;
      rec->t = qnow();
    }
  }
  return rc;
}

static void send_ack(struct quic *Q, int space)
{
  struct space *s = &Q->sp[space];
  if (!s->hk || !s->nrr)
    return;
  uint8_t f[RR_MAX * 16 + 32];
  uint32_t p = 0;
  f[p++] = 0x02;
  p += putvar(f + p, s->rr[0].hi);               /* Largest Acknowledged */
  p += putvar(f + p, 0);                         /* ACK Delay */
  p += putvar(f + p, (uint64_t)(s->nrr - 1));    /* ACK Range Count */
  p += putvar(f + p, s->rr[0].hi - s->rr[0].lo); /* First ACK Range */
  for (int i = 1; i < s->nrr; i++) {
    p += putvar(f + p, s->rr[i - 1].lo - s->rr[i].hi - 2); /* Gap */
    p += putvar(f + p, s->rr[i].hi - s->rr[i].lo);         /* ACK Range Length */
  }
  emit(Q, space, f, p, 0);
  s->ackp = 0;
}

static void send_pending_acks(struct quic *Q)
{
  for (int s = 0; s < 3; s++)
    if (Q->sp[s].ackp)
      send_ack(Q, s);
}

/* Send a CRYPTO frame carrying `data` at offset 0 in `space`. */
static int send_crypto(struct quic *Q, int space, const uint8_t *data, uint32_t len)
{
  uint8_t *f = Q->tx_cf;
  uint32_t p = 0;
  f[p++] = 0x06;
  p += putvar(f + p, 0);
  p += putvar(f + p, len);
  memcpy(f + p, data, len);
  p += len;
  return emit(Q, space, f, p, 1);
}

/* --------------------------------------------------- datagram processing */

static int process_datagram(struct quic *Q, uint8_t *d, uint32_t dlen)
{
  uint32_t off = 0;
  while (off < dlen) {
    uint8_t b0 = d[off];
    if (!(b0 & 0x40)) /* fixed bit clear: junk */
      break;
    if (b0 & 0x80) { /* long header */
      if (off + 7 > dlen)
        break;
      uint32_t ver =
          ((uint32_t)d[off + 1] << 24) | ((uint32_t)d[off + 2] << 16) | ((uint32_t)d[off + 3] << 8) | d[off + 4];
      int type = (b0 >> 4) & 0x03;
      uint32_t q = off + 5;
      uint8_t dl = d[q++];
      q += dl; /* DCID (ours, ignore) */
      if (q >= dlen)
        break;
      uint8_t sl = d[q++];
      uint8_t *scid_p = d + q;
      q += sl;
      if (q > dlen)
        break;
      if (ver == 0) /* Version Negotiation */
      {
        Q->closed = 1;
        return -1;
      }
      if (type == 3) { /* Retry */
        handle_retry(Q, scid_p, sl, d + q, dlen - q);
        return 1;
      }
      if (!Q->server_cid_set && sl <= sizeof Q->dcid) {
        memcpy(Q->dcid, scid_p, sl);
        Q->dcid_len = sl;
        Q->server_cid_set = 1;
      }
      if (type == 0) { /* Initial: skip token */
        uint64_t tl;
        if (!getvar(d, &q, dlen, &tl))
          break;
        q += (uint32_t)tl;
      } else if (type != 2) { /* 0-RTT: not expected */
        break;
      }
      uint64_t length;
      if (!getvar(d, &q, dlen, &length))
        break;
      uint32_t pn_off = q;
      if (pn_off + length > dlen)
        break;
      int space = (type == 0) ? S_INIT : S_HS;
      decrypt_pkt(Q, space, d, off, pn_off, (uint32_t)length, 1);
      off = pn_off + (uint32_t)length;
    } else { /* short header (1-RTT) */
      uint32_t pn_off = off + 1 + Q->scid_len;
      if (pn_off + 4 + 16 > dlen)
        break;
      decrypt_pkt(Q, S_APP, d, off, pn_off, dlen - pn_off, 0);
      off = dlen;
    }
  }
  return Q->closed ? -1 : 0;
}

/* Raise the server's connection-level send limit once less than half of it is left, so a connection that stays
 * open across many responses never stalls. */
static void grant_flow(struct quic *Q)
{
  if (!Q->sp[S_APP].hk || Q->rx_total + 0x800000 <= Q->rx_limit)
    return;
  Q->rx_limit = Q->rx_total + 0x1000000;
  uint8_t f[10];
  uint32_t p = 0;
  f[p++] = 0x10; /* MAX_DATA */
  p += putvar(f + p, Q->rx_limit);
  emit(Q, S_APP, f, p, 1);
}

/* Read one datagram, process it, and flush owed ACKs, running loss and probe timers meanwhile. 1 processed / 0 idle
 * (about 2 s without a datagram) / QUIC_EINTR / QUIC_ECLOSED. */
static int poll_once(struct quic *Q)
{
  uint8_t *dg = Q->rx_dg;
  unsigned start = qnow();
  for (;;) {
    run_timers(Q);
    if (Q->closed)
      return QUIC_ECLOSED;
    int space, probe, n;
    if (next_deadline(Q, &space, &probe)) { /* a timer is running: look at the socket without blocking */
      n = net_recv_nb(Q->sock, dg, (int)sizeof Q->rx_dg);
      if (n == NET_EINTR)
        return QUIC_EINTR;
      if (n <= 0) {
        if (qnow() - start >= 2000)
          return 0;
        msleep(2);
        continue;
      }
    } else {
      n = net_recv(Q->sock, dg, (int)sizeof Q->rx_dg);
      if (n == NET_EINTR)
        return QUIC_EINTR;
      if (n <= 0)
        return 0;
    }
    int r = process_datagram(Q, dg, (uint32_t)n);
    send_pending_acks(Q);
    grant_flow(Q);
    if (r < 0 || Q->closed || Q->cert_fail)
      return QUIC_ECLOSED;
    return 1;
  }
}

/* ---------------------------------------------------------- public API */

int quic_connect(quic **out, const char *host, uint16_t port, const char *alpn)
{
  struct quic *Q = calloc(1, sizeof *Q);
  if (!Q)
    return QUIC_ESOCK;
  Q->rx_limit = 0x1000000; /* initial_max_data we advertise */
  certs_init(&Q->certs);
  for (int i = 0; host[i] && i < (int)sizeof Q->host - 1; i++)
    Q->host[i] = host[i];

  Q->sock = net_dial(host, port, SK_UDP); /* resolve to IP; SNI stays `host` below */
  if (Q->sock < 0) {
    int e = Q->sock;
    free(Q);
    return e == NET_EINTR ? QUIC_EINTR : e == -2 ? QUIC_ERESOLVE : QUIC_ESOCK;
  }

  Q->scid_len = 8;
  rng_bytes(Q->scid, 8);
  Q->dcid_len = 8;
  rng_bytes(Q->dcid, 8);
  derive_initial_keys(Q, Q->dcid, Q->dcid_len);

  sha256_init(&Q->transcript);
  rng_bytes(Q->our_priv, 32);
  x25519(Q->our_pub, Q->our_priv, x25519_base);

  Q->chello_len = build_client_hello(Q, Q->chello, host, alpn);
  sha256_update(&Q->transcript, Q->chello, Q->chello_len);
  int err = QUIC_ESOCK;
  if (send_crypto(Q, S_INIT, Q->chello, Q->chello_len) < 0)
    goto fail;

  int tries = 0, last_retry = Q->did_retry;
  while (!(Q->app_done && Q->hs_sent_fin)) {
    int r = poll_once(Q);
    if (Q->cert_fail) {
      err = QUIC_ECERT;
      goto fail;
    }
    if (r == QUIC_EINTR) {
      err = QUIC_EINTR;
      goto fail;
    }
    if (r == QUIC_ECLOSED) {
      err = QUIC_EHANDSHAKE;
      goto fail;
    }
    if (Q->did_retry != last_retry) { /* Retry: resend Initial CH */
      last_retry = Q->did_retry;
      tries = 0;
      send_crypto(Q, S_INIT, Q->chello, Q->chello_len);
      continue;
    }
    if (r == 0) { /* quiet; lost handshake packets are resent by the loss timers */
      if (++tries > 8) {
        err = QUIC_EHANDSHAKE;
        goto fail;
      }
      continue;
    }
    tries = 0;
    if (Q->app_done && !Q->hs_sent_fin) { /* send client Finished */
      send_crypto(Q, S_HS, Q->pending_fin, (uint32_t)Q->pending_fin_len);
      Q->hs_sent_fin = 1;
    }
  }
  *out = Q;
  return QUIC_OK;

fail:
  net_close(Q->sock);
  for (int i = 0; i < 3; i++)
    sent_clear(&Q->sp[i]);
  free(Q);
  return err;
}

int quic_open(quic *Q, uint64_t *sid)
{
  if (Q->next_bidi >= Q->peer_max_bidi)
    return -1;
  *sid = 4 * Q->next_bidi++;
  return 0;
}

int quic_send(quic *Q, uint64_t sid, const void *data, uint32_t len, int fin)
{
  struct qstream *q = get_stream(Q, sid);
  if (!q || !Q->sp[S_APP].hk)
    return -1;
  if (Q->tx_total + len > Q->peer_max_data || (!(sid & 2) && q->send_off + len > Q->peer_stream_data))
    return -2; /* over what the server lets us send */
  const uint8_t *p = data;
  uint32_t done = 0;
  do {
    uint32_t chunk = len - done;
    if (chunk > 1100)
      chunk = 1100;
    uint8_t *f = Q->tx_sf;
    uint32_t fp = 0;
    uint8_t type = 0x08 | 0x04 | 0x02; /* OFF + LEN */
    if (fin && done + chunk == len)
      type |= 0x01;
    f[fp++] = type;
    fp += putvar(f + fp, sid);
    fp += putvar(f + fp, q->send_off + done);
    fp += putvar(f + fp, chunk);
    memcpy(f + fp, p + done, chunk);
    fp += chunk;
    if (emit(Q, S_APP, f, fp, 1) < 0)
      return -1;
    done += chunk;
  } while (done < len);
  q->send_off += len;
  Q->tx_total += len;
  return 0;
}

int quic_read(quic *Q, uint64_t sid, void *buf, uint32_t max, int *fin)
{
  struct qstream *q = find_stream(Q, sid);
  if (!q) {
    if (fin)
      *fin = 0;
    return 0;
  }
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

void quic_stream_done(quic *Q, uint64_t sid)
{
  for (int i = 0; i < QS_MAX; i++)
    if (Q->qs[i].used && Q->qs[i].id == sid)
      Q->qs[i].used = 0;
}

int quic_poll(quic *Q) { return poll_once(Q); }

int quic_peer_closed(quic *Q)
{
  for (; !Q->closed;) {
    int n = net_recv_nb(Q->sock, Q->rx_dg, (int)sizeof Q->rx_dg);
    if (n < 0 && n != NET_EINTR)
      return 1;
    if (n <= 0)
      break;
    process_datagram(Q, Q->rx_dg, (uint32_t)n);
  }
  send_pending_acks(Q);
  return Q->closed;
}

unsigned quic_idle_ms(const quic *Q) { return (unsigned)Q->peer_idle_ms; }

int quic_streams_left(const quic *Q)
{
  return Q->peer_max_bidi > Q->next_bidi ? (int)(Q->peer_max_bidi - Q->next_bidi) : 0;
}

void quic_close(quic *Q)
{
  if (Q->sp[S_APP].hk && !Q->closed) {
    uint8_t f[4];
    uint32_t p = 0;
    f[p++] = 0x1c;         /* CONNECTION_CLOSE (transport) */
    p += putvar(f + p, 0); /* error = NO_ERROR */
    p += putvar(f + p, 0); /* frame type */
    p += putvar(f + p, 0); /* reason length */
    emit(Q, S_APP, f, p, 0);
  }
  net_close(Q->sock);
  for (int i = 0; i < 3; i++)
    sent_clear(&Q->sp[i]);
  free(Q);
}
