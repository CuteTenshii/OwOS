/* bt_smp.c: LE security manager, initiator side: legacy and Secure Connections Just Works pairing, LTK bonds and
 * re-encryption of bonded links. Secure Connections uses the controller for P-256 (read local public key, generate
 * DHKey) and falls back to bt_p256.c when it refuses; the AES based functions run here. */

#include "bt_int.h"

#define SMP_PAIRING_REQ 0x01
#define SMP_PAIRING_RSP 0x02
#define SMP_PAIRING_CONFIRM 0x03
#define SMP_PAIRING_RANDOM 0x04
#define SMP_PAIRING_FAILED 0x05
#define SMP_ENC_INFO 0x06
#define SMP_CENTRAL_ID 0x07
#define SMP_ID_INFO 0x08
#define SMP_ID_ADDR 0x09
#define SMP_SIGN_INFO 0x0A
#define SMP_SECURITY_REQ 0x0B
#define SMP_PUBLIC_KEY 0x0C
#define SMP_DHKEY_CHECK 0x0D
#define SMP_KEYPRESS 0x0E

#define ERR_CONFIRM 0x04
#define ERR_NOT_SUPPORTED 0x05
#define ERR_KEY_SIZE 0x06
#define ERR_CMD_UNSUPPORTED 0x07
#define ERR_UNSPECIFIED 0x08
#define ERR_INVALID_PARAMS 0x0A
#define ERR_DHKEY 0x0B

#define IO_NONE 3
#define AUTH_BOND 0x01
#define AUTH_SC 0x08
#define DIST_ENC 0x01
#define DIST_ID 0x02
#define ENC_KEY_MISSING 0x06
#define TIMEOUT_MS 30000
#define MAX_BONDS 8

enum { P_IDLE, P_LOCAL_KEY, P_RSP, P_PEER_KEY, P_CONFIRM, P_RANDOM, P_DHKEY, P_CHECK, P_ENCRYPT, P_KEYS };

struct bond {
  bool used;
  uint8_t addr[6], atype, ltk[16], rand[8];
  uint16_t ediv;
};

static struct {
  struct bt_conn *c;
  int stage;
  bool sc, host_ecc, have_dhkey, has_id;
  uint32_t started;
  uint8_t preq[7], pres[7], key_size;
  uint8_t na[16], nb[16], cb[16];
  uint8_t pk[64], peer_pk[64], dhkey[32], mackey[16], ltk[16], priv[32];
  uint8_t a1[7], a2[7];
  uint16_t need, got; /* one bit per SMP opcode of the key distribution */
  uint16_t ediv;
  uint8_t rand[8], id_atype, id_addr[6];
} s;

static struct bond bonds[MAX_BONDS];
static bool no_ctrl_ecc;
static char last_result[48];

/* ---- crypto, over-the-air byte order in and out */

static void rev(uint8_t *dst, const uint8_t *src, int n)
{
  for (int i = 0; i < n; i++)
    dst[i] = src[n - 1 - i];
}

static void e128(const uint8_t k[16], const uint8_t in[16], uint8_t out[16])
{
  uint8_t kb[16], pb[16], ob[16];
  rev(kb, k, 16);
  rev(pb, in, 16);
  bt_aes128(kb, pb, ob);
  rev(out, ob, 16);
}

void smp_c1(const uint8_t k[16], const uint8_t r[16], const uint8_t preq[7], const uint8_t pres[7], uint8_t iat,
            const uint8_t ia[6], uint8_t rat, const uint8_t ra[6], uint8_t out[16])
{
  uint8_t p1[16], p2[16], t[16];
  p1[0] = iat;
  p1[1] = rat;
  memcpy(p1 + 2, preq, 7);
  memcpy(p1 + 9, pres, 7);
  for (int i = 0; i < 16; i++)
    t[i] = (uint8_t)(r[i] ^ p1[i]);
  e128(k, t, t);
  memcpy(p2, ra, 6);
  memcpy(p2 + 6, ia, 6);
  memset(p2 + 12, 0, 4);
  for (int i = 0; i < 16; i++)
    t[i] ^= p2[i];
  e128(k, t, out);
}

void smp_s1(const uint8_t k[16], const uint8_t r1[16], const uint8_t r2[16], uint8_t out[16])
{
  uint8_t r[16];
  memcpy(r, r2, 8);
  memcpy(r + 8, r1, 8);
  e128(k, r, out);
}

void smp_f4(const uint8_t u[32], const uint8_t v[32], const uint8_t x[16], uint8_t z, uint8_t out[16])
{
  uint8_t m[65], kb[16], ob[16];
  rev(m, u, 32);
  rev(m + 32, v, 32);
  m[64] = z;
  rev(kb, x, 16);
  bt_cmac(kb, m, 65, ob);
  rev(out, ob, 16);
}

void smp_f5(const uint8_t w[32], const uint8_t n1[16], const uint8_t n2[16], const uint8_t a1[7], const uint8_t a2[7],
            uint8_t mackey[16], uint8_t ltk[16])
{
  static const uint8_t salt[16] = {0x6C, 0x88, 0x83, 0x91, 0xAA, 0xF5, 0xA5, 0x38,
                                   0x60, 0x37, 0x0B, 0xDB, 0x5A, 0x60, 0x83, 0xBE};
  uint8_t wb[32], t[16], m[53] = {0, 'b', 't', 'l', 'e'}, ob[16];
  rev(wb, w, 32);
  bt_cmac(salt, wb, 32, t);
  rev(m + 5, n1, 16);
  rev(m + 21, n2, 16);
  rev(m + 37, a1, 7);
  rev(m + 44, a2, 7);
  m[51] = 0x01;
  m[52] = 0x00;
  bt_cmac(t, m, 53, ob);
  rev(mackey, ob, 16);
  m[0] = 1;
  bt_cmac(t, m, 53, ob);
  rev(ltk, ob, 16);
}

void smp_f6(const uint8_t w[16], const uint8_t n1[16], const uint8_t n2[16], const uint8_t r[16], const uint8_t io[3],
            const uint8_t a1[7], const uint8_t a2[7], uint8_t out[16])
{
  uint8_t m[65], kb[16], ob[16];
  rev(m, n1, 16);
  rev(m + 16, n2, 16);
  rev(m + 32, r, 16);
  rev(m + 48, io, 3);
  rev(m + 51, a1, 7);
  rev(m + 58, a2, 7);
  rev(kb, w, 16);
  bt_cmac(kb, m, 65, ob);
  rev(out, ob, 16);
}

/* ---- bonds */

static struct bond *bond_find(const uint8_t *addr)
{
  for (int i = 0; i < MAX_BONDS; i++)
    if (bonds[i].used && !memcmp(bonds[i].addr, addr, 6))
      return &bonds[i];
  return NULL;
}

static void bond_store(const uint8_t *addr, uint8_t atype, const uint8_t *ltk, const uint8_t *rand, uint16_t ediv)
{
  struct bond *b = bond_find(addr);
  for (int i = 0; i < MAX_BONDS && !b; i++)
    if (!bonds[i].used)
      b = &bonds[i];
  if (!b)
    b = &bonds[0];
  b->used = true;
  memcpy(b->addr, addr, 6);
  b->atype = atype;
  memcpy(b->ltk, ltk, 16);
  memcpy(b->rand, rand, 8);
  b->ediv = ediv;
}

/* ---- pairing */

static void start_encryption(struct bt_conn *c, const uint8_t rand[8], uint16_t ediv, const uint8_t ltk[16])
{
  uint8_t p[28];
  bt_put16(p, c->handle);
  memcpy(p + 2, rand, 8);
  bt_put16(p + 10, ediv);
  memcpy(p + 12, ltk, 16);
  hci_cmd(HCI_OP_LE_START_ENC, p, 28);
}

static void send_pdu(const uint8_t *pdu, int len) { l2cap_send_cid(s.c, L2CAP_CID_SMP, pdu, len); }

static void abort_pairing(uint8_t reason)
{
  struct bt_conn *c = s.c;
  bt_log("pairing failed: reason %02x", reason);
  bt_snprintf(last_result, sizeof last_result, "failed, reason %02x", reason);
  memset(&s, 0, sizeof s);
  gatt_secured(c, false);
}

static void fail(uint8_t reason)
{
  uint8_t p[2] = {SMP_PAIRING_FAILED, reason};
  send_pdu(p, 2);
  abort_pairing(reason);
}

/* Over-the-air public keys are X then Y, each little endian. */
static void swap_point(uint8_t *dst, const uint8_t *src)
{
  rev(dst, src, 32);
  rev(dst + 32, src + 32, 32);
}

static bool host_keygen(void)
{
  uint8_t pub[64];
  for (int i = 0; i < 8; i++) {
    bt_random(s.priv, 32);
    if (p256_pub(s.priv, pub)) {
      swap_point(s.pk, pub);
      s.host_ecc = true;
      return true;
    }
  }
  return false;
}

static void send_request(bool sc)
{
  const uint8_t req[7] = {SMP_PAIRING_REQ,   IO_NONE, 0, (uint8_t)(AUTH_BOND | (sc ? AUTH_SC : 0)), 16, 0,
                          DIST_ENC | DIST_ID};
  memcpy(s.preq, req, 7);
  s.sc = sc;
  s.stage = P_RSP;
  send_pdu(req, 7);
}

static void finish(void)
{
  if ((s.got & s.need) != s.need)
    return;
  bool id = s.has_id && s.id_atype <= 1;
  if (s.sc || s.need & 1 << SMP_ENC_INFO)
    bond_store(id ? s.id_addr : s.c->addr, id ? s.id_atype : s.c->addr_type, s.ltk, s.rand, s.ediv);
  bt_log("pairing complete");
  bt_snprintf(last_result, sizeof last_result, "complete");
  memset(&s, 0, sizeof s);
}

static void send_random(void)
{
  uint8_t p[17] = {SMP_PAIRING_RANDOM};
  memcpy(p + 1, s.na, 16);
  s.stage = P_RANDOM;
  send_pdu(p, 17);
}

static void legacy_confirm(void)
{
  static const uint8_t tk[16];
  uint8_t p[17] = {SMP_PAIRING_CONFIRM};
  bt_random(s.na, 16);
  smp_c1(tk, s.na, s.preq, s.pres, 0, hci_local_addr(), s.c->addr_type, s.c->addr, p + 1);
  s.stage = P_CONFIRM;
  send_pdu(p, 17);
}

static void on_response(const uint8_t *d, int len)
{
  if (s.stage != P_RSP || len != 7) {
    fail(len == 7 ? ERR_UNSPECIFIED : ERR_INVALID_PARAMS);
    return;
  }
  memcpy(s.pres, d, 7);
  s.sc = s.sc && (d[3] & AUTH_SC);
  s.key_size = d[4] < 16 ? d[4] : 16;
  if (s.key_size < 7 || (s.sc && s.key_size < 16)) {
    fail(ERR_KEY_SIZE);
    return;
  }
  uint8_t dist = d[6] & (s.sc ? DIST_ID : DIST_ENC | DIST_ID);
  if (dist & DIST_ENC)
    s.need |= 1 << SMP_ENC_INFO | 1 << SMP_CENTRAL_ID;
  if (dist & DIST_ID)
    s.need |= 1 << SMP_ID_INFO | 1 << SMP_ID_ADDR;
  if (s.sc) {
    uint8_t p[65] = {SMP_PUBLIC_KEY};
    memcpy(p + 1, s.pk, 64);
    s.stage = P_PEER_KEY;
    send_pdu(p, 65);
  } else {
    legacy_confirm();
  }
}

static void on_public_key(const uint8_t *d, int len)
{
  if (s.stage != P_PEER_KEY || len != 65) {
    fail(len == 65 ? ERR_UNSPECIFIED : ERR_INVALID_PARAMS);
    return;
  }
  memcpy(s.peer_pk, d + 1, 64);
  if (!memcmp(s.peer_pk, s.pk, 64)) {
    fail(ERR_INVALID_PARAMS);
    return;
  }
  if (s.host_ecc) {
    uint8_t peer[64], dh[32];
    swap_point(peer, s.peer_pk);
    if (!p256_ecdh(s.priv, peer, dh)) {
      fail(ERR_INVALID_PARAMS);
      return;
    }
    rev(s.dhkey, dh, 32);
    s.have_dhkey = true;
  } else if (!hci_cmd(HCI_OP_LE_DHKEY, s.peer_pk, 64)) {
    fail(ERR_UNSPECIFIED);
    return;
  }
  s.stage = P_CONFIRM;
}

static void on_confirm(const uint8_t *d, int len)
{
  if (s.stage != P_CONFIRM || len != 17) {
    fail(len == 17 ? ERR_UNSPECIFIED : ERR_INVALID_PARAMS);
    return;
  }
  memcpy(s.cb, d + 1, 16);
  if (s.sc)
    bt_random(s.na, 16);
  send_random();
}

static void send_check(void)
{
  static const uint8_t zero[16];
  uint8_t p[17] = {SMP_DHKEY_CHECK};
  memcpy(s.a1, hci_local_addr(), 6);
  s.a1[6] = 0;
  memcpy(s.a2, s.c->addr, 6);
  s.a2[6] = s.c->addr_type;
  smp_f5(s.dhkey, s.na, s.nb, s.a1, s.a2, s.mackey, s.ltk);
  smp_f6(s.mackey, s.na, s.nb, zero, s.preq + 1, s.a1, s.a2, p + 1);
  s.stage = P_CHECK;
  send_pdu(p, 17);
}

static void on_random(const uint8_t *d, int len)
{
  static const uint8_t zero[16];
  uint8_t cfm[16];
  if (s.stage != P_RANDOM || len != 17) {
    fail(len == 17 ? ERR_UNSPECIFIED : ERR_INVALID_PARAMS);
    return;
  }
  memcpy(s.nb, d + 1, 16);
  if (s.sc) {
    smp_f4(s.peer_pk, s.pk, s.nb, 0, cfm);
    if (memcmp(cfm, s.cb, 16)) {
      fail(ERR_CONFIRM);
      return;
    }
    s.stage = P_DHKEY;
    if (s.have_dhkey)
      send_check();
    return;
  }
  smp_c1(zero, s.nb, s.preq, s.pres, 0, hci_local_addr(), s.c->addr_type, s.c->addr, cfm);
  if (memcmp(cfm, s.cb, 16)) {
    fail(ERR_CONFIRM);
    return;
  }
  smp_s1(zero, s.nb, s.na, s.ltk);
  memset(s.ltk + s.key_size, 0, (uint32_t)(16 - s.key_size));
  s.stage = P_ENCRYPT;
  start_encryption(s.c, zero, 0, s.ltk);
}

static void on_dhkey_check(const uint8_t *d, int len)
{
  static const uint8_t zero[16];
  uint8_t e[16];
  if (s.stage != P_CHECK || len != 17) {
    fail(len == 17 ? ERR_UNSPECIFIED : ERR_INVALID_PARAMS);
    return;
  }
  smp_f6(s.mackey, s.nb, s.na, zero, s.pres + 1, s.a2, s.a1, e);
  if (memcmp(e, d + 1, 16)) {
    fail(ERR_DHKEY);
    return;
  }
  s.stage = P_ENCRYPT;
  start_encryption(s.c, zero, 0, s.ltk);
}

static void on_key(uint8_t op, const uint8_t *d, int len)
{
  if (s.stage != P_ENCRYPT && s.stage != P_KEYS)
    return;
  if (op == SMP_ENC_INFO && len == 17 && !s.sc)
    memcpy(s.ltk, d + 1, 16);
  else if (op == SMP_CENTRAL_ID && len == 11 && !s.sc) {
    s.ediv = bt_get16(d + 1);
    memcpy(s.rand, d + 3, 8);
  } else if (op == SMP_ID_ADDR && len == 8) {
    s.id_atype = d[1];
    memcpy(s.id_addr, d + 2, 6);
    s.has_id = true;
  }
  s.got |= (uint16_t)(1 << op);
  if (s.stage == P_KEYS)
    finish();
}

static void on_security_request(struct bt_conn *c)
{
  struct bond *b = bond_find(c->addr);
  if (s.stage != P_IDLE || c->secure)
    return;
  if (b)
    start_encryption(c, b->rand, b->ediv, b->ltk);
  else
    smp_start(c);
}

void smp_rx(struct bt_conn *c, const uint8_t *d, int len)
{
  if (len < 1)
    return;
  if (d[0] == SMP_SECURITY_REQ) {
    on_security_request(c);
    return;
  }
  if (s.stage == P_IDLE || s.c != c) {
    if (d[0] == SMP_PAIRING_REQ) {
      uint8_t p[2] = {SMP_PAIRING_FAILED, ERR_NOT_SUPPORTED};
      l2cap_send_cid(c, L2CAP_CID_SMP, p, 2);
    }
    return;
  }
  switch (d[0]) {
  case SMP_PAIRING_FAILED:
    abort_pairing(len > 1 ? d[1] : ERR_UNSPECIFIED);
    break;
  case SMP_PAIRING_RSP:
    on_response(d, len);
    break;
  case SMP_PUBLIC_KEY:
    on_public_key(d, len);
    break;
  case SMP_PAIRING_CONFIRM:
    on_confirm(d, len);
    break;
  case SMP_PAIRING_RANDOM:
    on_random(d, len);
    break;
  case SMP_DHKEY_CHECK:
    on_dhkey_check(d, len);
    break;
  case SMP_ENC_INFO:
  case SMP_CENTRAL_ID:
  case SMP_ID_INFO:
  case SMP_ID_ADDR:
  case SMP_SIGN_INFO:
    on_key(d[0], d, len);
    break;
  case SMP_KEYPRESS:
    break;
  default:
    fail(ERR_CMD_UNSUPPORTED);
  }
}

int smp_start(struct bt_conn *c)
{
  if (!c->le)
    return -1;
  if (s.stage != P_IDLE)
    return s.c == c ? 0 : -1;
  memset(&s, 0, sizeof s);
  s.c = c;
  s.started = bt_now();
  if (no_ctrl_ecc) {
    if (!host_keygen()) {
      s.c = NULL;
      return -1;
    }
    send_request(true);
  } else if (hci_cmd(HCI_OP_LE_P256_KEY, NULL, 0)) {
    s.stage = P_LOCAL_KEY;
  } else {
    s.c = NULL;
    return -1;
  }
  return 0;
}

void smp_p256_key(uint8_t status, const uint8_t *key)
{
  if (s.stage != P_LOCAL_KEY)
    return;
  if (status || !key) {
    no_ctrl_ecc = true;
    if (!host_keygen()) {
      fail(ERR_UNSPECIFIED);
      return;
    }
  } else {
    memcpy(s.pk, key, 64);
  }
  send_request(true);
}

void smp_dhkey(uint8_t status, const uint8_t *key)
{
  if (s.stage < P_CONFIRM || s.stage > P_DHKEY || s.have_dhkey)
    return;
  if (status || !key) {
    fail(ERR_INVALID_PARAMS);
    return;
  }
  memcpy(s.dhkey, key, 32);
  s.have_dhkey = true;
  if (s.stage == P_DHKEY)
    send_check();
}

void smp_cmd_failed(uint16_t op, uint8_t status)
{
  if (op == HCI_OP_LE_P256_KEY)
    smp_p256_key(status, NULL);
  else if (op == HCI_OP_LE_DHKEY)
    smp_dhkey(status, NULL);
  else if (op == HCI_OP_LE_START_ENC && s.stage == P_ENCRYPT)
    fail(ERR_UNSPECIFIED);
}

void smp_conn_up(struct bt_conn *c)
{
  struct bond *b = bond_find(c->addr);
  if (c->le && b)
    start_encryption(c, b->rand, b->ediv, b->ltk);
}

void smp_conn_down(struct bt_conn *c)
{
  if (s.c == c)
    memset(&s, 0, sizeof s);
}

void smp_enc_change(struct bt_conn *c, uint8_t status, bool enabled)
{
  c->secure = !status && enabled;
  if (s.c == c && s.stage == P_ENCRYPT) {
    if (!c->secure) {
      fail(ERR_UNSPECIFIED);
      return;
    }
    s.stage = P_KEYS;
    gatt_secured(c, true);
    finish();
    return;
  }
  if (status == ENC_KEY_MISSING) {
    struct bond *b = bond_find(c->addr);
    if (b)
      b->used = false;
  }
  if (c->secure)
    gatt_secured(c, true);
}

bool smp_unbond(const uint8_t *addr)
{
  struct bond *b = bond_find(addr);
  if (b)
    memset(b, 0, sizeof *b);
  return b != NULL;
}

void smp_poll(uint32_t now_ms)
{
  if (s.stage != P_IDLE && now_ms - s.started > TIMEOUT_MS)
    abort_pairing(ERR_UNSPECIFIED);
}

void smp_reset(void)
{
  memset(&s, 0, sizeof s);
  memset(bonds, 0, sizeof bonds);
  no_ctrl_ecc = false;
  last_result[0] = 0;
}

int smp_status_text(char *buf, int max)
{
  int n = 0;
  for (int i = 0; i < MAX_BONDS && n < max; i++) {
    const uint8_t *a = bonds[i].addr;
    if (bonds[i].used)
      n += bt_snprintf(buf + n, (uint32_t)(max - n), "bond %02x:%02x:%02x:%02x:%02x:%02x\n", a[5], a[4], a[3], a[2],
                       a[1], a[0]);
  }
  if (s.stage != P_IDLE && n < max)
    n += bt_snprintf(buf + n, (uint32_t)(max - n), "pairing in progress\n");
  else if (last_result[0] && n < max)
    n += bt_snprintf(buf + n, (uint32_t)(max - n), "pairing %s\n", last_result);
  return n < max ? n : max;
}
