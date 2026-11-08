/* bt_avdtp.c: AVDTP signalling and the A2DP source: stream setup against a sink, SBC encoding and RTP pacing. */

#include "bt_int.h"
#include "sbc.h"

#define PSM_AVDTP 0x19
#define SEID_LOCAL 1

#define SIG_DISCOVER 0x01
#define SIG_GET_CAPS 0x02
#define SIG_SET_CONFIG 0x03
#define SIG_OPEN 0x06
#define SIG_START 0x07
#define SIG_CLOSE 0x08
#define SIG_SUSPEND 0x09
#define SIG_ABORT 0x0A
#define SIG_DELAY_REPORT 0x0D

#define MSG_COMMAND 0
#define MSG_GENERAL_REJECT 1
#define MSG_ACCEPT 2
#define MSG_REJECT 3

#define CAP_MEDIA_TRANSPORT 0x01
#define CAP_MEDIA_CODEC 0x07
#define CODEC_SBC 0x00

#define PENDING_TIMEOUT_MS 5000
#define LEAD_MS 60
#define PCM_FRAMES 8192 /* ring capacity in stereo frames */
#define RTP_HEADER 12
#define SSRC 0x4F774F53u
#define MAX_PACKET 1000

enum {
  A_IDLE,
  A_CONNECTING,
  A_DISCOVER,
  A_CAPS,
  A_CONFIG,
  A_OPEN,
  A_MEDIA,
  A_READY,
  A_STARTING,
  A_STREAMING,
  A_FAILED
};

static const char *const state_names[] = {"idle",  "connecting", "discover", "capabilities", "configure", "open",
                                          "media", "ready",      "starting", "streaming",    "failed"};

static struct {
  int state;
  struct bt_conn *conn;
  struct l2cap_chan *sig, *media;
  uint8_t label, pending_sig, acp_seid;
  uint32_t pending_at;
  bool pending;

  struct sbc_enc enc;
  uint32_t rate;
  uint8_t cfg[4];

  volatile bool want_stream, want_suspend, writer;
  int16_t *pcm; /* stereo frames at the stream rate */
  volatile uint32_t pcm_head, pcm_tail;
  uint32_t rs_phase; /* resampler position, 16.16 */
  int16_t rs_prev[2];
  uint32_t t0, sent_samples;
  uint16_t seq;
  uint32_t timestamp;
} a;

/* ---- signalling */

static void sig_command(uint8_t sig, const uint8_t *p, int len)
{
  uint8_t b[16];
  if (!a.sig || len > 14)
    return;
  a.label = (uint8_t)((a.label + 1) & 0x0F);
  b[0] = (uint8_t)(a.label << 4 | MSG_COMMAND);
  b[1] = sig;
  memcpy(b + 2, p, (uint32_t)len);
  if (l2cap_send(a.sig, b, 2 + len) == 0) {
    a.pending = true;
    a.pending_sig = sig;
    a.pending_at = bt_now();
  }
}

static void sig_reply(uint8_t label, uint8_t msg, uint8_t sig)
{
  uint8_t b[2] = {(uint8_t)(label << 4 | msg), sig};
  l2cap_send(a.sig, b, 2);
}

static void fail(const char *why)
{
  bt_log("a2dp: %s", why);
  a.state = A_FAILED;
  a.pending = false;
  if (a.conn)
    hci_disconnect(a.conn);
}

static void stream_reset(void)
{
  a.pcm_head = a.pcm_tail = 0;
  a.rs_phase = 0;
  a.rs_prev[0] = a.rs_prev[1] = 0;
  a.sent_samples = 0;
}

static bool choose_config(const uint8_t *caps, int len)
{
  for (int i = 0; i + 2 <= len; i += 2 + caps[i + 1]) {
    int cl = caps[i + 1];
    if (i + 2 + cl > len)
      break;
    if (caps[i] != CAP_MEDIA_CODEC || cl < 6 || caps[i + 3] != CODEC_SBC)
      continue;
    const uint8_t *s = caps + i + 4;
    uint8_t freq, mode, blocks;
    if (s[0] & 0x10) {
      freq = SBC_FREQ_48000;
      a.cfg[0] = 0x10;
      a.rate = 48000;
    } else if (s[0] & 0x20) {
      freq = SBC_FREQ_44100;
      a.cfg[0] = 0x20;
      a.rate = 44100;
    } else {
      return false;
    }
    if (s[0] & 0x01) {
      mode = SBC_MODE_JOINT;
      a.cfg[0] |= 0x01;
    } else if (s[0] & 0x02) {
      mode = SBC_MODE_STEREO;
      a.cfg[0] |= 0x02;
    } else if (s[0] & 0x04) {
      mode = SBC_MODE_DUAL;
      a.cfg[0] |= 0x04;
    } else {
      return false;
    }
    if (!(s[1] & 0x04))
      return false;
    if (s[1] & 0x10) {
      blocks = 16;
      a.cfg[1] = 0x10;
    } else if (s[1] & 0x20) {
      blocks = 12;
      a.cfg[1] = 0x20;
    } else if (s[1] & 0x40) {
      blocks = 8;
      a.cfg[1] = 0x40;
    } else {
      blocks = 4;
      a.cfg[1] = 0x80;
    }
    a.cfg[1] |= 0x04;
    uint8_t alloc = (s[1] & 0x01) ? SBC_ALLOC_LOUDNESS : SBC_ALLOC_SNR;
    a.cfg[1] |= alloc == SBC_ALLOC_LOUDNESS ? 0x01 : 0x02;
    int bp = a.rate == 48000 ? 51 : 53;
    if (bp > s[3])
      bp = s[3];
    if (bp < s[2])
      bp = s[2];
    a.cfg[2] = a.cfg[3] = (uint8_t)bp;
    sbc_init(&a.enc, freq, blocks, mode, alloc, bp);
    return true;
  }
  return false;
}

static void media_state(struct l2cap_chan *ch);

static void on_accept(uint8_t sig, const uint8_t *p, int len)
{
  uint8_t b[16];
  switch (sig) {
  case SIG_DISCOVER:
    for (int i = 0; i + 2 <= len; i += 2) {
      bool in_use = p[i] & 2, sink = p[i + 1] & 8, audio = (p[i + 1] >> 4) == 0;
      if (!in_use && sink && audio) {
        a.acp_seid = p[i] >> 2;
        b[0] = (uint8_t)(a.acp_seid << 2);
        a.state = A_CAPS;
        sig_command(SIG_GET_CAPS, b, 1);
        return;
      }
    }
    fail("no free audio sink endpoint");
    break;
  case SIG_GET_CAPS:
    if (!choose_config(p, len)) {
      fail("sink offers no usable SBC configuration");
      return;
    }
    b[0] = (uint8_t)(a.acp_seid << 2);
    b[1] = (uint8_t)(SEID_LOCAL << 2);
    b[2] = CAP_MEDIA_TRANSPORT;
    b[3] = 0;
    b[4] = CAP_MEDIA_CODEC;
    b[5] = 6;
    b[6] = 0; /* audio */
    b[7] = CODEC_SBC;
    memcpy(b + 8, a.cfg, 4);
    a.state = A_CONFIG;
    sig_command(SIG_SET_CONFIG, b, 12);
    break;
  case SIG_SET_CONFIG:
    b[0] = (uint8_t)(a.acp_seid << 2);
    a.state = A_OPEN;
    sig_command(SIG_OPEN, b, 1);
    break;
  case SIG_OPEN:
    a.state = A_MEDIA;
    a.media = l2cap_connect(a.conn, PSM_AVDTP, NULL, media_state, NULL);
    if (!a.media)
      fail("no channel for the media transport");
    break;
  case SIG_START:
    a.state = A_STREAMING;
    a.t0 = bt_now();
    a.sent_samples = 0;
    bt_log("a2dp: streaming at %u Hz, bitpool %u", a.rate, a.enc.bitpool);
    break;
  case SIG_SUSPEND:
    if (a.state != A_FAILED)
      a.state = A_READY;
    break;
  }
}

static void sig_rx(struct l2cap_chan *ch, const uint8_t *d, int len)
{
  (void)ch;
  if (len < 2 || (d[0] >> 2 & 3) != 0)
    return;
  uint8_t label = d[0] >> 4, msg = d[0] & 3, sig = d[1] & 0x3F;
  if (msg == MSG_COMMAND) {
    if (sig == SIG_DELAY_REPORT || sig == SIG_CLOSE || sig == SIG_ABORT) {
      sig_reply(label, MSG_ACCEPT, sig);
      if (sig != SIG_DELAY_REPORT) {
        a.state = A_FAILED;
        if (a.conn)
          hci_disconnect(a.conn);
      }
    } else {
      sig_reply(label, MSG_GENERAL_REJECT, sig);
    }
    return;
  }
  if (!a.pending || sig != a.pending_sig || label != a.label)
    return;
  a.pending = false;
  if (msg == MSG_ACCEPT)
    on_accept(sig, d + 2, len - 2);
  else if (sig == SIG_START || sig == SIG_SUSPEND) {
    a.want_stream = false;
    a.state = A_READY;
  } else {
    fail("sink rejected a request");
  }
}

static void sig_state(struct l2cap_chan *ch)
{
  if (ch->open && a.state == A_CONNECTING) {
    a.state = A_DISCOVER;
    sig_command(SIG_DISCOVER, NULL, 0);
  } else if (!ch->open && a.state != A_IDLE && a.state != A_FAILED) {
    a.state = A_FAILED;
  }
}

static void media_state(struct l2cap_chan *ch)
{
  if (ch->open && a.state == A_MEDIA) {
    a.state = A_READY;
    bt_log("a2dp: ready");
  } else if (!ch->open && a.state >= A_READY) {
    a.state = A_FAILED;
  }
}

void avdtp_conn_ready(struct bt_conn *c)
{
  if (a.state != A_IDLE)
    return;
  if (!a.pcm)
    a.pcm = bt_alloc(PCM_FRAMES * 2 * sizeof *a.pcm);
  if (!a.pcm)
    return;
  a.conn = c;
  a.state = A_CONNECTING;
  stream_reset();
  a.sig = l2cap_connect(c, PSM_AVDTP, sig_rx, sig_state, NULL);
  if (!a.sig)
    a.state = A_FAILED;
}

void avdtp_conn_down(struct bt_conn *c)
{
  if (a.conn != c)
    return;
  bool had = a.state != A_IDLE;
  a.state = A_IDLE;
  a.conn = NULL;
  a.sig = a.media = NULL;
  a.pending = a.want_stream = a.want_suspend = false;
  if (had)
    bt_log("a2dp: connection closed");
}

/* ---- media */

static uint32_t pcm_fill(void) { return a.pcm_head - a.pcm_tail; }

static void put_be16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}

static void put_be32(uint8_t *p, uint32_t v)
{
  put_be16(p, (uint16_t)(v >> 16));
  put_be16(p + 2, (uint16_t)v);
}

static void pump_media(uint32_t now)
{
  int fs = sbc_frame_samples(&a.enc), flen = sbc_frame_len(&a.enc);
  int mtu = a.media->out_mtu > MAX_PACKET ? MAX_PACKET : a.media->out_mtu;
  int max_frames = (mtu - RTP_HEADER - 1) / flen;
  if (max_frames > 15)
    max_frames = 15;
  uint32_t target = (uint32_t)((uint64_t)(now - a.t0) * a.rate / 1000) + (uint32_t)((uint64_t)LEAD_MS * a.rate / 1000);

  if (pcm_fill() < (uint32_t)fs && a.sent_samples + (uint32_t)fs < target) {
    /* Underrun: restart the clock instead of bursting to catch up later. */
    a.t0 = now;
    a.sent_samples = 0;
    return;
  }
  while (l2cap_room(a.media) && pcm_fill() >= (uint32_t)fs && a.sent_samples + (uint32_t)fs <= target) {
    uint8_t pkt[MAX_PACKET + 16];
    int n = 0, off = RTP_HEADER + 1;
    while (n < max_frames && pcm_fill() >= (uint32_t)fs && a.sent_samples + (uint32_t)(n + 1) * fs <= target) {
      int16_t in[2 * 16 * SBC_SUBBANDS];
      for (int i = 0; i < fs; i++) {
        uint32_t idx = (a.pcm_tail + (uint32_t)i) % PCM_FRAMES;
        in[2 * i] = a.pcm[2 * idx];
        in[2 * i + 1] = a.pcm[2 * idx + 1];
      }
      a.pcm_tail += (uint32_t)fs;
      off += sbc_encode(&a.enc, in, pkt + off);
      n++;
    }
    if (!n)
      break;
    pkt[0] = 0x80;
    pkt[1] = 0x60;
    put_be16(pkt + 2, a.seq++);
    put_be32(pkt + 4, a.timestamp);
    put_be32(pkt + 8, SSRC);
    pkt[RTP_HEADER] = (uint8_t)n;
    a.timestamp += (uint32_t)(n * fs);
    a.sent_samples += (uint32_t)(n * fs);
    if (l2cap_send(a.media, pkt, off) != 0)
      return;
  }
}

void avdtp_poll(uint32_t now)
{
  if (a.pending && now - a.pending_at > PENDING_TIMEOUT_MS) {
    fail("signalling timed out");
    return;
  }
  uint8_t b = (uint8_t)(a.acp_seid << 2);
  if (a.state == A_READY && a.want_stream && !a.pending) {
    a.state = A_STARTING;
    sig_command(SIG_START, &b, 1);
  } else if (a.state == A_STREAMING && a.want_suspend && !a.pending && pcm_fill() == 0) {
    a.want_suspend = false;
    sig_command(SIG_SUSPEND, &b, 1);
  } else if (a.state == A_STREAMING && a.media && a.media->open) {
    pump_media(now);
  }
}

int avdtp_status_text(char *buf, int max)
{
  if (a.state == A_IDLE)
    return 0;
  int n = bt_snprintf(buf, (uint32_t)max, "a2dp %s", state_names[a.state]);
  if (a.state >= A_READY && a.state < A_FAILED && n < max)
    n += bt_snprintf(buf + n, (uint32_t)(max - n), " %u Hz bitpool %u", a.rate, a.enc.bitpool);
  if (n < max)
    n += bt_snprintf(buf + n, (uint32_t)(max - n), "\n");
  return n < max ? n : max;
}

/* ---- audio sink */

bool bt_a2dp_ready(void) { return a.state >= A_READY && a.state < A_FAILED; }

/* Wait for cond, giving the poll thread time; false on timeout. */
static bool wait_until(bool (*cond)(void), uint32_t ms)
{
  uint32_t start = bt_now();
  while (!cond()) {
    if (bt_now() - start > ms)
      return false;
    bt_yield();
  }
  return true;
}

static bool is_streaming(void) { return a.state == A_STREAMING; }
static bool is_drained(void) { return pcm_fill() == 0 || a.state != A_STREAMING; }

int bt_a2dp_open(void)
{
  if (!bt_a2dp_ready() || a.writer)
    return -1;
  a.writer = true;
  stream_reset();
  a.want_suspend = false;
  a.want_stream = true;
  if (a.state == A_STREAMING)
    return 0;
  if (!wait_until(is_streaming, 3000)) {
    a.want_stream = false;
    a.writer = false;
    return -1;
  }
  return 0;
}

int bt_a2dp_write(const void *pcm, uint32_t bytes)
{
  const int16_t *in = pcm;
  uint32_t frames = bytes / 4, used = 0;
  if (a.state != A_STREAMING)
    return -1;
  uint32_t step = a.rate == 48000 ? 0x10000u : (uint32_t)(((uint64_t)48000 << 16) / a.rate);
  while (used < frames) {
    if (a.state != A_STREAMING)
      break;
    if (pcm_fill() >= PCM_FRAMES - 1) {
      uint32_t start = bt_now();
      while (pcm_fill() >= PCM_FRAMES - 1 && a.state == A_STREAMING && bt_now() - start < 5000)
        bt_yield();
      if (pcm_fill() >= PCM_FRAMES - 1)
        break;
    }
    if (a.rate == 48000) {
      uint32_t idx = a.pcm_head % PCM_FRAMES;
      a.pcm[2 * idx] = in[2 * used];
      a.pcm[2 * idx + 1] = in[2 * used + 1];
      a.pcm_head++;
      used++;
      continue;
    }
    /* 48 kHz source to 44.1 kHz: linear interpolation between the previous and current input frame. */
    while (a.rs_phase < 0x10000u && pcm_fill() < PCM_FRAMES - 1) {
      uint32_t idx = a.pcm_head % PCM_FRAMES;
      for (int ch = 0; ch < 2; ch++) {
        int32_t p = a.rs_prev[ch], c = in[2 * used + ch];
        a.pcm[2 * idx + ch] = (int16_t)(p + (int32_t)(((int64_t)(c - p) * a.rs_phase) >> 16));
      }
      a.pcm_head++;
      a.rs_phase += step;
    }
    if (a.rs_phase >= 0x10000u) {
      a.rs_phase -= 0x10000u;
      a.rs_prev[0] = in[2 * used];
      a.rs_prev[1] = in[2 * used + 1];
      used++;
    }
  }
  return used ? (int)(used * 4) : -1;
}

void bt_a2dp_close(void)
{
  if (!a.writer)
    return;
  wait_until(is_drained, 3000);
  a.want_stream = false;
  a.want_suspend = true;
  a.writer = false;
}
