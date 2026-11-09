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
#if __STDC_HOSTED__
extern void (*bt_host_yield)(void); /* tests run the poll loop from here */
#endif
uintptr_t bt_irq_save(void);
void bt_irq_restore(uintptr_t flags);

#define BT_MAX_CONN 4
#define BT_MAX_DEV 24
#define BT_NAME_LEN 32
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
};

struct bt_conn {
  bool used, le, secure;
  uint16_t handle;
  uint8_t addr[6], addr_type;
  uint8_t *rx; /* L2CAP frame being reassembled */
  uint16_t rx_len, rx_need;
};

/* ---- hci */
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
bool l2cap_room(const struct l2cap_chan *ch);
void l2cap_close(struct l2cap_chan *ch);

/* ---- gatt */
void gatt_conn_up(struct bt_conn *c);
void gatt_conn_down(struct bt_conn *c);
void gatt_poll(void);
int gatt_command(const char *args);

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
