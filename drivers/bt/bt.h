/* bt.h: Bluetooth host stack services for the rest of the kernel. The stack runs on the USB poll thread; the calls
 * below are safe from other threads. */
#ifndef OWOS_BT_H
#define OWOS_BT_H

#include <stdbool.h>
#include <stdint.h>

#define BT_PKT_CMD 1
#define BT_PKT_ACL 2
#define BT_PKT_EVT 4

/* A controller transport. send runs on the poll thread and returns 0 on success. */
struct bt_transport {
  void *ctx;
  int (*send)(void *ctx, int type, const uint8_t *data, int len);
};

/* Transport side: attach starts controller bring-up, rx queues one packet (any context), poll does the work. */
void bt_attach(const struct bt_transport *t);
void bt_detach(void);
void bt_rx(int type, const uint8_t *data, int len);
void bt_poll(uint32_t now_ms);

bool bt_present(void);

/* Text views for sysfs; each returns the byte count. */
int bt_status_text(char *buf, int max);
int bt_devices_text(char *buf, int max);
int bt_gatt_text(char *buf, int max);

/* Run one control line: "scan on|off", "connect <addr>", "disconnect", "gatt read <handle>", "gatt write <handle>
 * <hex>", "gatt notify <handle> on|off", "pair" (LE link), "unpair <addr>" (drop the bond or link key, disconnect).
 * Returns 0 once queued, -1 if malformed. */
int bt_command(const char *line);

/* A2DP source: PCM is 48 kHz signed 16-bit stereo, as in the audio syscalls. */
bool bt_a2dp_ready(void);
int bt_a2dp_open(void);
int bt_a2dp_write(const void *pcm, uint32_t bytes);
void bt_a2dp_close(void);

#endif
