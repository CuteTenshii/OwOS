/* bt_int.h: state shared by the Bluetooth host stack layers (HCI, L2CAP, GATT, AVDTP). */
#ifndef OWOS_BT_INT_H
#define OWOS_BT_INT_H

#include "bt.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

#if __STDC_HOSTED__
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define bt_snprintf snprintf
#else
#include "heap.h"
#include "kstring.h"
#define bt_snprintf ksnprintf
#endif

void *bt_alloc(uint32_t n); /* zeroed */
void bt_free(void *p);
void bt_log(const char *fmt, ...);
uint32_t bt_now(void);
void bt_yield(void);
void bt_random(uint8_t *buf, int n);
#if __STDC_HOSTED__
extern void (*bt_host_yield)(void); /* tests run the poll loop from here */
#endif
uintptr_t bt_irq_save(void);
void bt_irq_restore(uintptr_t flags);

#define BT_MAX_CONN 4
#define BT_MAX_DEV 24
#define BT_NAME_LEN 32
#define BT_DEV_UUIDS 4
#define BT_RX_MAX 1030 /* largest reassembled L2CAP frame */

#define BT_ADDR_BR 0xFF /* addr_type of a BR/EDR device; LE uses the HCI types 0 (public) and 1 (random) */

/* True if s starts with prefix. */
static inline bool bt_has(const char *s, const char *prefix)
{
  while (*prefix)
    if (*s++ != *prefix++)
      return false;
  return true;
}

static inline uint16_t bt_get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline void bt_put16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

struct bt_dev {
  bool used, le;
  uint8_t addr[6], addr_type;
  bool name_tried;
  int8_t rssi;
  uint32_t cod;
  char name[BT_NAME_LEN];
  uint8_t nuuid;
  struct {
    uint8_t len; /* 2 or 16 */
    uint8_t v[16];
  } uuids[BT_DEV_UUIDS]; /* advertised service UUIDs */
};

struct bt_conn {
  bool used, le, secure;
  uint16_t handle;
  uint8_t addr[6], addr_type;
  uint8_t *rx; /* L2CAP frame being reassembled */
  uint16_t rx_len, rx_need;
};

/* ---- hci */
#define HCI_OP_LE_START_ENC 0x2019
#define HCI_OP_LE_P256_KEY 0x2025
#define HCI_OP_LE_DHKEY 0x2026

bool hci_cmd(uint16_t op, const void *params, int len);
const uint8_t *hci_local_addr(void);
struct bt_conn *hci_conn_by_handle(uint16_t handle);
struct bt_conn *hci_connected_br(void);
struct bt_conn *hci_connected_le(void);
bool hci_acl_room(const struct bt_conn *c, int frags);
int hci_acl_send(struct bt_conn *c, const uint8_t *data, int len);
void hci_disconnect(struct bt_conn *c);
bool hci_find_key(const uint8_t *addr, uint8_t key[16]);
uint32_t hci_dev_cod(const uint8_t *addr);     /* class of device from inquiry, 0 if unknown */
const char *hci_dev_name(const uint8_t *addr); /* "" if unknown */

/* ---- l2cap */
#define L2CAP_CID_SIGNALING 1
#define L2CAP_CID_ATT 4
#define L2CAP_CID_LE_SIGNALING 5
#define L2CAP_CID_SMP 6

struct l2cap_chan {
  bool used, open;
  struct bt_conn *conn;
  uint16_t psm, scid, dcid, out_mtu;
  uint8_t cfg; /* bit 0: our config accepted, bit 1: peer config accepted */
  uint8_t ident;
  void (*on_data)(struct l2cap_chan *ch, const uint8_t *data, int len);
  void (*on_state)(struct l2cap_chan *ch);
  void *user;
};

void l2cap_conn_up(struct bt_conn *c);
void l2cap_conn_down(struct bt_conn *c);
void l2cap_acl_rx(struct bt_conn *c, uint16_t flags, const uint8_t *data, int len);
struct l2cap_chan *l2cap_connect(struct bt_conn *c, uint16_t psm,
                                 void (*on_data)(struct l2cap_chan *, const uint8_t *, int),
                                 void (*on_state)(struct l2cap_chan *), void *user);
struct l2cap_chan *l2cap_att(struct bt_conn *c);
int l2cap_send(struct l2cap_chan *ch, const uint8_t *data, int len);
int l2cap_send_cid(struct bt_conn *c, uint16_t cid, const uint8_t *data, int len);
bool l2cap_room(const struct l2cap_chan *ch);
void l2cap_close(struct l2cap_chan *ch);

/* ---- gatt */
void gatt_conn_up(struct bt_conn *c);
void gatt_conn_down(struct bt_conn *c);
void gatt_poll(void);
void bt_uuid_text(char *out, int max, const uint8_t *v, int len);
void gatt_secured(struct bt_conn *c,
                  bool ok); /* pairing or re-encryption finished; retries a request refused for security */
int gatt_command(const char *args, bool ev);

/* ---- aes */
void bt_aes128(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
void bt_cmac(const uint8_t key[16], const uint8_t *msg, int len, uint8_t out[16]);

/* ---- p256 (big endian, X||Y for points) */
bool p256_pub(const uint8_t priv[32], uint8_t pub[64]);
bool p256_ecdh(const uint8_t priv[32], const uint8_t pub[64], uint8_t out[32]);

/* ---- smp (LE pairing). Multi-octet values below are in over-the-air (little endian) order. */
void smp_c1(const uint8_t k[16], const uint8_t r[16], const uint8_t preq[7], const uint8_t pres[7], uint8_t iat,
            const uint8_t ia[6], uint8_t rat, const uint8_t ra[6], uint8_t out[16]);
void smp_s1(const uint8_t k[16], const uint8_t r1[16], const uint8_t r2[16], uint8_t out[16]);
void smp_f4(const uint8_t u[32], const uint8_t v[32], const uint8_t x[16], uint8_t z, uint8_t out[16]);
void smp_f5(const uint8_t w[32], const uint8_t n1[16], const uint8_t n2[16], const uint8_t a1[7], const uint8_t a2[7],
            uint8_t mackey[16], uint8_t ltk[16]);
void smp_f6(const uint8_t w[16], const uint8_t n1[16], const uint8_t n2[16], const uint8_t r[16], const uint8_t io[3],
            const uint8_t a1[7], const uint8_t a2[7], uint8_t out[16]);

int smp_start(struct bt_conn *c);
void smp_rx(struct bt_conn *c, const uint8_t *data, int len);
void smp_conn_up(struct bt_conn *c);
void smp_conn_down(struct bt_conn *c);
void smp_enc_change(struct bt_conn *c, uint8_t status, bool enabled);
void smp_p256_key(uint8_t status, const uint8_t *key); /* key is NULL when the command itself failed */
void smp_dhkey(uint8_t status, const uint8_t *key);
void smp_cmd_failed(uint16_t op, uint8_t status);
void smp_poll(uint32_t now_ms);
void smp_reset(void);
bool smp_unbond(const uint8_t *addr);
int smp_status_text(char *buf, int max);

/* ---- avdtp / a2dp */
void avdtp_conn_ready(struct bt_conn *c);
void avdtp_conn_down(struct bt_conn *c);
void avdtp_poll(uint32_t now_ms);
int avdtp_status_text(char *buf, int max);

/* ---- hid (BR/EDR input devices) */
void bthid_conn_ready(struct bt_conn *c);
void bthid_conn_down(struct bt_conn *c);
void bthid_poll(uint32_t now_ms);
int bthid_status_text(char *buf, int max);

#endif
