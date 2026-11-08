/* vdagent.c: QEMU clipboard bridge (see vdagent.h).
 * Speaks the spice vdagent protocol in "clipboard by demand" mode over virtio-serial port 1: a grab announces
 * ownership, the other side requests the data. */

#include "vdagent.h"
#include "kstring.h"
#include "spinlock.h"
#include "virtioserial.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VD_PORT 1
#define VD_PROTOCOL 1
#define VD_CLIENT_PORT 1
#define VD_CHUNK_MAX 2048
#define VD_HDR 20
#define VD_CAP_CLIPBOARD_BY_DEMAND 5
#define VD_CLIPBOARD_UTF8_TEXT 1

enum {
  VD_MSG_CLIPBOARD = 4,
  VD_MSG_ANNOUNCE_CAPS = 6,
  VD_MSG_GRAB = 7,
  VD_MSG_REQUEST = 8,
};

#define CLIP_MAX 16384
#define MSG_MAX (VD_HDR + 4 + CLIP_MAX)

static uint8_t stream_hdr[8];
static uint32_t stream_hdr_n, chunk_left, msg_n;
static uint8_t msg[MSG_MAX];
static uint8_t out[MSG_MAX];
static uint8_t chunk[8 + VD_CHUNK_MAX];
static bool caps_sent;

static spinlock_t clip_lock; /* guards the clip buffers below, shared with /proc/hostclip */
static uint8_t host_clip[CLIP_MAX];
static int host_len;
static bool host_new;
static uint8_t guest_clip[CLIP_MAX];
static int guest_len;
static bool grab_pending;

static inline uint32_t le32(const uint8_t *p)
{
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline void put32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

/* Send a message whose body (bodylen bytes) is already at out + VD_HDR, split into chunks. */
static void agent_send(uint32_t type, uint32_t bodylen)
{
  put32(out, VD_PROTOCOL);
  put32(out + 4, type);
  put32(out + 8, 0);
  put32(out + 12, 0);
  put32(out + 16, bodylen);
  uint32_t total = VD_HDR + bodylen;
  for (uint32_t off = 0; off < total; off += VD_CHUNK_MAX) {
    uint32_t n = total - off < VD_CHUNK_MAX ? total - off : VD_CHUNK_MAX;
    put32(chunk, VD_CLIENT_PORT);
    put32(chunk + 4, n);
    memcpy(chunk + 8, out + off, n);
    if (!vserial_write(VD_PORT, chunk, 8 + n))
      return;
  }
}

static void send_text_type(uint32_t msg_type)
{
  put32(out + VD_HDR, VD_CLIPBOARD_UTF8_TEXT);
  agent_send(msg_type, 4);
}

static void send_caps(bool request)
{
  put32(out + VD_HDR, request);
  put32(out + VD_HDR + 4, 1u << VD_CAP_CLIPBOARD_BY_DEMAND);
  agent_send(VD_MSG_ANNOUNCE_CAPS, 8);
}

static void dispatch(uint32_t type, const uint8_t *body, uint32_t size)
{
  switch (type) {
  case VD_MSG_ANNOUNCE_CAPS:
    if (size >= 4 && le32(body))
      send_caps(false);
    break;
  case VD_MSG_GRAB:
    for (uint32_t i = 0; i + 4 <= size; i += 4)
      if (le32(body + i) == VD_CLIPBOARD_UTF8_TEXT) {
        send_text_type(VD_MSG_REQUEST);
        break;
      }
    break;
  case VD_MSG_CLIPBOARD:
    if (size >= 4 && le32(body) == VD_CLIPBOARD_UTF8_TEXT) {
      uintptr_t f = spin_lock_irqsave(&clip_lock);
      host_len = (int)(size - 4);
      memcpy(host_clip, body + 4, (uint32_t)host_len);
      host_new = true;
      spin_unlock_irqrestore(&clip_lock, f);
    }
    break;
  case VD_MSG_REQUEST:
    if (size >= 4 && le32(body) == VD_CLIPBOARD_UTF8_TEXT) {
      uintptr_t f = spin_lock_irqsave(&clip_lock);
      int n = guest_len;
      memcpy(out + VD_HDR + 4, guest_clip, (uint32_t)n);
      spin_unlock_irqrestore(&clip_lock, f);
      if (n > 0) {
        put32(out + VD_HDR, VD_CLIPBOARD_UTF8_TEXT);
        agent_send(VD_MSG_CLIPBOARD, 4 + (uint32_t)n);
      }
    }
    break;
  default:
    break;
  }
}

/* Reassemble messages from the chunked byte stream. A message too big for msg[] is dropped. */
static void rx(const uint8_t *d, uint32_t len)
{
  for (uint32_t i = 0; i < len; i++) {
    uint8_t b = d[i];
    if (chunk_left == 0) {
      stream_hdr[stream_hdr_n++] = b;
      if (stream_hdr_n == sizeof stream_hdr) {
        chunk_left = le32(stream_hdr + 4);
        stream_hdr_n = 0;
      }
      continue;
    }
    chunk_left--;
    if (msg_n < MSG_MAX)
      msg[msg_n] = b;
    msg_n++;
    if (msg_n >= VD_HDR && msg_n == VD_HDR + le32(msg + 16)) {
      if (msg_n <= MSG_MAX)
        dispatch(le32(msg + 4), msg + VD_HDR, msg_n - VD_HDR);
      msg_n = 0;
    }
  }
}

static void tick(void)
{
  if (!vserial_port_open(VD_PORT)) {
    caps_sent = false;
    return;
  }
  if (!caps_sent) {
    caps_sent = true;
    send_caps(true);
  }
  uintptr_t f = spin_lock_irqsave(&clip_lock);
  bool grab = grab_pending;
  grab_pending = false;
  spin_unlock_irqrestore(&clip_lock, f);
  if (grab)
    send_text_type(VD_MSG_GRAB);
}

const struct vserial_client vdagent_client = {VD_PORT, "com.redhat.spice.0", rx, tick};

int vdagent_present(void) { return vserial_port_present(VD_PORT); }

int vdagent_clip_recv(char *buf, int max)
{
  uintptr_t f = spin_lock_irqsave(&clip_lock);
  int n = 0;
  if (host_new) {
    n = host_len < max ? host_len : max;
    memcpy(buf, host_clip, (uint32_t)n);
    host_new = false;
  }
  spin_unlock_irqrestore(&clip_lock, f);
  return n;
}

int vdagent_clip_send(const char *buf, int len)
{
  if (!vdagent_present())
    return -1;
  if (len <= 0)
    return 0;
  uintptr_t f = spin_lock_irqsave(&clip_lock);
  guest_len = len < CLIP_MAX ? len : CLIP_MAX;
  memcpy(guest_clip, buf, (uint32_t)guest_len);
  grab_pending = true;
  spin_unlock_irqrestore(&clip_lock, f);
  return len;
}
