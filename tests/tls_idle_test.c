/* Host test for tls_peer_closed (user/lib/tls/tls.c): idle-connection checks against hand-built TLS 1.3 records.
 * tls.c is included so the test can set up a finished connection; its system and PKI dependencies are renamed to
 * stubs here. */
#define OWOS_SUITE_TEST
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define net_dial tlst_net_dial
#define net_recv tlst_net_recv
#define net_recv_nb tlst_net_recv_nb
#define net_send tlst_net_send
#define net_close tlst_net_close
#define gettime tlst_gettime
#define rng_bytes tlst_rng_bytes
#define resolve_ech tlst_resolve_ech
#define ech_pick_config tlst_ech_pick_config
#define ech_encode_inner tlst_ech_encode_inner
#define ech_outer_ext tlst_ech_outer_ext
#define x509_strerror tlst_x509_strerror
#define certs_init tlst_certs_init
#define certs_take tlst_certs_take
#define certs_check_sig tlst_certs_check_sig
#define certs_check_cv tlst_certs_check_cv
#define certs_fail tlst_certs_fail

struct timeval64 {
  long long tv_sec;
  long tv_usec;
};
int gettime(struct timeval64 *tv);
void rng_bytes(void *p, int n);
int net_dial(const char *host, int port, int proto);

#include "certs.h"
#include "crypto.h"
#include "ech.h"
#include "resolve.h"
#include "test.h"

int gettime(struct timeval64 *tv) { return tv->tv_sec = 0, -1; }
void rng_bytes(void *p, int n) { memset(p, 0, (size_t)n); }
int net_dial(const char *host, int port, int proto) { return (void)host, (void)port, (void)proto, -2; }
int net_recv(int h, void *buf, int max) { return (void)h, (void)buf, (void)max, 0; }
int net_send(int h, const void *buf, int len) { return (void)h, (void)buf, len; }
int net_close(int h) { return (void)h, 0; }
int resolve_ech(const char *host, int port, unsigned char *out, int max)
{
  return (void)host, (void)port, (void)out, (void)max, 0;
}
int ech_pick_config(const uint8_t *list, uint32_t len, struct ech_cfg *out)
{
  return (void)list, (void)len, (void)out, 0;
}
uint32_t ech_encode_inner(const uint8_t *ch, uint32_t chlen, uint32_t sni_len, const struct ech_cfg *cfg, uint8_t *out,
                          uint32_t outmax)
{
  return (void)ch, (void)chlen, (void)sni_len, (void)cfg, (void)out, (void)outmax, 0;
}
uint32_t ech_outer_ext(uint8_t *out, const struct ech_cfg *cfg, const uint8_t enc[32], uint32_t payload_len)
{
  return (void)out, (void)cfg, (void)enc, (void)payload_len, 0;
}
const char *x509_strerror(int err) { return (void)err, ""; }
void certs_init(struct certs *c) { c->verify = X509_E_UNTRUSTED; }
void certs_take(struct certs *c, const char *host, const uint8_t *body, uint32_t bl, int tls13)
{
  (void)c, (void)host, (void)body, (void)bl, (void)tls13;
}
void certs_check_sig(struct certs *c, uint16_t scheme, const uint8_t *msg, uint32_t msglen, const uint8_t *sig,
                     uint32_t siglen)
{
  (void)c, (void)scheme, (void)msg, (void)msglen, (void)sig, (void)siglen;
}
void certs_check_cv(struct certs *c, const uint8_t th[32], const uint8_t *body, uint32_t bl)
{
  (void)c, (void)th, (void)body, (void)bl;
}
void certs_fail(struct certs *c, int err) { (void)c, (void)err; }

#include "../user/lib/tls/tls.c"

/* ---- the wire: bytes the fake socket hands out, then quiet or FIN */
static uint8_t g_wire[4096];
static int g_wire_len, g_wire_pos, g_fin;

int net_recv_nb(int h, void *buf, int max)
{
  (void)h;
  int avail = g_wire_len - g_wire_pos;
  if (avail <= 0)
    return g_fin ? -1 : 0;
  int n = avail < max ? avail : max;
  memcpy(buf, g_wire + g_wire_pos, (size_t)n);
  g_wire_pos += n;
  return n;
}

static uint64_t g_seq; /* sequence number of the next record the "server" encrypts */

static struct tls_conn *new_conn(void)
{
  struct tls_conn *c = calloc(1, sizeof *c);
  c->established = c->is13 = c->recv_encrypted = 1;
  c->tcp = 3;
  for (int i = 0; i < 16; i++)
    c->server_key[i] = (uint8_t)(i + 1);
  for (int i = 0; i < 12; i++)
    c->server_iv[i] = (uint8_t)(0xa0 + i);
  g_wire_len = g_wire_pos = g_fin = 0;
  g_seq = 0;
  return c;
}

/* Append one encrypted TLS 1.3 record of inner type `type` to the wire; returns its start offset. */
static int push_record(struct tls_conn *c, uint8_t type, const void *pay, int len)
{
  uint8_t inner[64], ct[64], tag[16], nonce[12];
  memcpy(inner, pay, (size_t)len);
  inner[len] = type;
  nonce13(nonce, c->server_iv, g_seq++);
  uint16_t clen = (uint16_t)(len + 1 + 16);
  uint8_t hdr[5] = {REC_APP, 3, 3, (uint8_t)(clen >> 8), (uint8_t)clen};
  aes128_gcm_seal(c->server_key, nonce, hdr, 5, inner, (uint32_t)len + 1, ct, tag);
  int start = g_wire_len;
  memcpy(g_wire + g_wire_len, hdr, 5);
  memcpy(g_wire + g_wire_len + 5, ct, (size_t)len + 1);
  memcpy(g_wire + g_wire_len + 5 + len + 1, tag, 16);
  g_wire_len += 5 + len + 1 + 16;
  return start;
}

static void quiet_connection_is_open(void)
{
  struct tls_conn *c = new_conn();
  expect_eq(tls_peer_closed(c, 0), 0);
  expect_eq(tls_peer_closed(c, 1), 0);
  free(c);
}

static void fin_or_reset_is_closed(void)
{
  struct tls_conn *c = new_conn();
  g_fin = 1;
  expect_eq(tls_peer_closed(c, 1), 1);
  free(c);
}

static void close_notify_is_closed(void)
{
  struct tls_conn *c = new_conn();
  const uint8_t close_notify[2] = {1, 0};
  push_record(c, REC_ALERT, close_notify, 2);
  expect_eq(tls_peer_closed(c, 1), 1);
  free(c);
}

static void session_ticket_is_skipped(void)
{
  struct tls_conn *c = new_conn();
  const uint8_t ticket[8] = {4, 0, 0, 4, 1, 2, 3, 4};
  push_record(c, REC_HS, ticket, 8);
  expect_eq(tls_peer_closed(c, 0), 0);
  expect_eq(c->rxlen - c->rxpos, 0); /* consumed */
  const uint8_t close_notify[2] = {1, 0};
  push_record(c, REC_ALERT, close_notify, 2); /* a ticket must not hide a close behind it */
  push_record(c, REC_HS, ticket, 8);
  g_wire_pos = g_wire_len - (5 + 2 + 1 + 16) - (5 + 8 + 1 + 16);
  expect_eq(tls_peer_closed(c, 0), 1);
  free(c);
}

static void unexpected_data_closes_http1_but_not_h2(void)
{
  struct tls_conn *c = new_conn();
  push_record(c, REC_APP, "junk", 4);
  expect_eq(tls_peer_closed(c, 0), 1);
  free(c);

  c = new_conn();
  push_record(c, REC_APP, "PING", 4);
  expect_eq(tls_peer_closed(c, 1), 0);
  char buf[8];
  expect_eq(tls_read(c, buf, sizeof buf), 4); /* the record is still there for the protocol layer */
  expect(memcmp(buf, "PING", 4) == 0);
  free(c);
}

static void partial_record_is_not_a_close(void)
{
  struct tls_conn *c = new_conn();
  const uint8_t close_notify[2] = {1, 0};
  push_record(c, REC_ALERT, close_notify, 2);
  g_wire_len -= 3; /* the record's tail has not arrived yet */
  expect_eq(tls_peer_closed(c, 1), 0);
  free(c);
}

int main(void)
{
  test("quiet_connection_is_open", quiet_connection_is_open);
  test("fin_or_reset_is_closed", fin_or_reset_is_closed);
  test("close_notify_is_closed", close_notify_is_closed);
  test("session_ticket_is_skipped", session_ticket_is_skipped);
  test("unexpected_data_closes_http1_but_not_h2", unexpected_data_closes_http1_but_not_h2);
  test("partial_record_is_not_a_close", partial_record_is_not_a_close);
  return 0;
}
