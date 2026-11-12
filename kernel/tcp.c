/* tcp.c: minimal TCP with concurrent connections, demuxed by 4-tuple (see tcp.h). */

#include <stddef.h>
#include <stdint.h>

#include "entropy.h"
#include "heap.h"
#include "kstring.h"
#include "net.h"
#include "sched.h"
#include "tcp.h"
#include "timer.h"

static inline uint16_t bswap16(uint16_t x) { return (uint16_t)((x << 8) | (x >> 8)); }
static inline uint32_t bswap32(uint32_t x) { return (x << 24) | ((x & 0xFF00) << 8) | ((x >> 8) & 0xFF00) | (x >> 24); }
#define htons(x) bswap16((uint16_t)(x))
#define ntohs(x) bswap16((uint16_t)(x))
#define htonl(x) bswap32((uint32_t)(x))
#define ntohl(x) bswap32((uint32_t)(x))

struct tcp_hdr {
  uint16_t sport, dport;
  uint32_t seq;
  uint32_t ack;
  uint8_t off;
  uint8_t flags;
  uint16_t win;
  uint16_t csum;
  uint16_t urg;
} __attribute__((packed));

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

#define IP_PROTO_TCP 6
#define TCP_MSS_MAX 1460 /* IPv4; IPv6 is 20 less */
#define TCP_RXBUF 65536u
#define TCP_TXBUF 65536u /* a power of two, so a byte's offset is its sequence number mod TCP_TXBUF */
#define TCP_MAX_CONNS 8
#define TCP_MAX_LISTEN 4
#define TCP_SYN_RCVD_TIMEOUT (2 * TIMER_HZ)

/* Retransmission (RFC 6298): the timeout doubles per unanswered retry, and the connection is reset after the last. */
#define TCP_RTO_INIT TIMER_HZ
#define TCP_RTO_MIN (TIMER_HZ / 5)
#define TCP_RTO_MAX (60 * TIMER_HZ)
#define TCP_MAX_RETRIES 8
#define TCP_LINGER (30 * TIMER_HZ)   /* how long a closed connection may keep flushing */
#define TCP_FIN_WAIT2 (2 * TIMER_HZ) /* grace for the peer's FIN once ours is acknowledged */

/* ST_LINGER: closed by its owner, still delivering queued data and the FIN from tcp_tick. */
enum { ST_CLOSED, ST_LISTEN, ST_SYN_SENT, ST_SYN_RCVD, ST_ESTAB, ST_CLOSE_WAIT, ST_LINGER, ST_DONE };

/* One connection control block; a slot is free iff state == ST_CLOSED. */
struct tcp_conn {
  volatile int state;
  uint8_t peer_ip[16]; /* v4-mapped for IPv4 */
  uint16_t lport, rport;

  volatile uint32_t snd_una; /* oldest unacknowledged sequence number */
  volatile uint32_t snd_nxt; /* next new sequence number to send; never rewound */
  volatile uint32_t snd_end; /* sequence number after the last queued byte, which is the FIN's */
  volatile uint32_t snd_wnd; /* peer's advertised window */
  uint32_t snd_wl1, snd_wl2; /* seq and ack of the segment that last set snd_wnd */
  volatile bool fin_queued;
  uint8_t *txbuf; /* allocated on first write, kept per slot */

  uint32_t rto;            /* base retransmission timeout in ticks, before backoff */
  uint32_t srtt8, rttvar4; /* smoothed RTT x8 and RTT variance x4; srtt8 is 0 until the first sample */
  bool rtt_on;             /* timing the segment starting at rtt_seq */
  uint32_t rtt_seq, rtt_start;
  bool timer_on; /* retransmission, or persist when nothing is in flight */
  uint32_t timer_at;
  uint8_t retries;
  uint32_t linger_until;

  volatile uint32_t rcv_nxt; /* written in interrupt context */
  volatile bool finned;
  volatile bool reset;

  uint8_t *rxbuf;            /* allocated lazily, kept per slot */
  volatile uint32_t wlen;    /* bytes written by the IRQ */
  volatile uint32_t rlen;    /* bytes consumed by the reader */
  volatile uint32_t adv_wnd; /* last window advertised */

  volatile bool accepted; /* handed out by tcp_accept */
  bool passive;           /* opened by a peer's SYN on a listener, not by tcp_connect */
  uint32_t opened;        /* timer_ticks() at SYN_RCVD */
};

static struct tcp_conn conns[TCP_MAX_CONNS];

/* Open listening ports. */
static struct {
  uint16_t port;
  volatile bool active;
} listeners[TCP_MAX_LISTEN];

/* Rotating ephemeral local port source. */
static uint16_t lport_seq;

/* Snapshot EFLAGS and cli; restore keeps IF clear if it was already clear (IRQ-safe). */
#include "irqflags.h"

/* Sequence-space comparisons, correct across 2^32 wraparound. */
static inline bool seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static inline bool seq_leq(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }
static inline bool seq_gt(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }
static inline bool seq_geq(uint32_t a, uint32_t b) { return (int32_t)(a - b) >= 0; }

static bool tick_passed(uint32_t t) { return (int32_t)(timer_ticks() - t) >= 0; }

/* Claim a free slot as `newstate`; need_rxbuf (IRQ path) only takes slots with a buffer. NULL if none. */
static struct tcp_conn *tcb_alloc(int newstate, int need_rxbuf)
{
  uint32_t f = irq_save();
  struct tcp_conn *c = NULL;
  for (int i = 0; i < TCP_MAX_CONNS; i++) {
    if (conns[i].state != ST_CLOSED)
      continue;
    if (need_rxbuf && !conns[i].rxbuf)
      continue;
    conns[i].state = (volatile int)newstate;
    c = &conns[i];
    break;
  }
  irq_restore(f);
  return c;
}

/* Reset the send side for a connection whose SYN uses `isn`. */
static void tcb_init_send(struct tcp_conn *c, uint32_t isn)
{
  c->snd_una = c->snd_nxt = c->snd_end = isn + 1;
  c->snd_wnd = 0;
  c->snd_wl1 = c->snd_wl2 = 0;
  c->fin_queued = false;
  c->rto = TCP_RTO_INIT;
  c->srtt8 = c->rttvar4 = 0;
  c->rtt_on = false;
  c->timer_on = false;
  c->retries = 0;
}

/* States in which queued data and the FIN still go out. */
static bool tcb_sending(const struct tcp_conn *c)
{
  int st = c->state;
  return !c->reset && (st == ST_ESTAB || st == ST_CLOSE_WAIT || st == ST_LINGER);
}

static bool tcp_fin_acked(const struct tcp_conn *c) { return c->fin_queued && c->snd_una == c->snd_end + 1; }

/* Mark the connection reset; a slot no handle refers to is freed outright. IRQs off. */
static void tcp_drop(struct tcp_conn *c)
{
  bool orphan = c->state == ST_LINGER || (c->passive && !c->accepted);
  c->reset = true;
  c->timer_on = false;
  c->state = orphan ? ST_CLOSED : ST_DONE;
}

/* Fill header (data at seg+20), checksum, send via IP. */
static void tcp_xmit(struct tcp_conn *c, uint8_t *seg, uint8_t flags, uint32_t seq, uint32_t dlen)
{
  struct tcp_hdr *t = (struct tcp_hdr *)seg;
  t->sport = htons(c->lport);
  t->dport = htons(c->rport);
  t->seq = htonl(seq);
  t->ack = htonl(c->rcv_nxt);
  t->off = 5 << 4;
  t->flags = flags;
  uint32_t space = TCP_RXBUF - (c->wlen - c->rlen);
  if (space > 65535)
    space = 65535;
  t->win = htons((uint16_t)space);
  c->adv_wnd = space;
  t->csum = 0;
  t->urg = 0;
  uint8_t src[16];
  net_src_addr(c->peer_ip, src);
  t->csum = htons(net_l4_csum(src, c->peer_ip, IP_PROTO_TCP, seg, 20 + dlen));
  net_ip_send(c->peer_ip, IP_PROTO_TCP, seg, 20 + dlen);
}

/* Control segment, no payload; small stack buffer, safe in interrupt context. */
static void tcp_send_ctl(struct tcp_conn *c, uint8_t flags, uint32_t seq)
{
  uint8_t seg[20];
  tcp_xmit(c, seg, flags, seq, 0);
}

/* Copy `len` queued bytes starting at sequence number `seq` out of the send ring. */
static void tx_load(const struct tcp_conn *c, uint32_t seq, uint8_t *out, uint32_t len)
{
  uint32_t off = seq % TCP_TXBUF;
  uint32_t first = TCP_TXBUF - off;
  if (first >= len) {
    memcpy(out, c->txbuf + off, len);
  } else {
    memcpy(out, c->txbuf + off, first);
    memcpy(out + first, c->txbuf, len - first);
  }
}

static void tx_store(struct tcp_conn *c, uint32_t seq, const uint8_t *in, uint32_t len)
{
  uint32_t off = seq % TCP_TXBUF;
  uint32_t first = TCP_TXBUF - off;
  if (first >= len) {
    memcpy(c->txbuf + off, in, len);
  } else {
    memcpy(c->txbuf + off, in, first);
    memcpy(c->txbuf, in + first, len - first);
  }
}

/* Current timeout with exponential backoff applied. */
static uint32_t tcp_backoff(const struct tcp_conn *c)
{
  uint32_t t = c->rto << c->retries;
  return t > TCP_RTO_MAX ? TCP_RTO_MAX : t;
}

static void tcp_timer_arm(struct tcp_conn *c)
{
  c->timer_at = timer_ticks() + tcp_backoff(c);
  c->timer_on = true;
}

/* Fold one round-trip measurement (ticks) into the RTO estimate, RFC 6298 section 2. */
static void tcp_rtt_sample(struct tcp_conn *c, uint32_t r)
{
  if (r == 0)
    r = 1;
  if (!c->srtt8) {
    c->srtt8 = r << 3;
    c->rttvar4 = r << 1;
  } else {
    int32_t delta = (int32_t)r - (int32_t)(c->srtt8 >> 3);
    c->srtt8 = (uint32_t)((int32_t)c->srtt8 + delta);
    if (delta < 0)
      delta = -delta;
    c->rttvar4 = c->rttvar4 + (uint32_t)delta - (c->rttvar4 >> 2);
  }
  uint32_t rto = (c->srtt8 >> 3) + (c->rttvar4 ? c->rttvar4 : 1);
  c->rto = rto < TCP_RTO_MIN ? TCP_RTO_MIN : rto > TCP_RTO_MAX ? TCP_RTO_MAX : rto;
}

/* Send the next new segment the peer's window allows; false if there is none. The segment is claimed with IRQs off
 * and sent after, since resolving the next hop may sleep in command context. */
static bool tcp_output_one(struct tcp_conn *c)
{
  uint8_t seg[20 + TCP_MSS_MAX];
  uintptr_t f = irq_save();
  uint32_t unsent = seq_lt(c->snd_nxt, c->snd_end) ? c->snd_end - c->snd_nxt : 0;
  uint32_t in_flight = c->snd_nxt - c->snd_una;
  uint32_t usable = c->snd_wnd > in_flight ? c->snd_wnd - in_flight : 0;
  uint32_t len = unsent < usable ? unsent : usable;
  uint32_t mss = net_tcp_mss(c->peer_ip);
  if (len > mss)
    len = mss;
  bool fin = c->fin_queued && seq_leq(c->snd_nxt, c->snd_end) && c->snd_nxt + len == c->snd_end;
  if (!tcb_sending(c) || (len == 0 && !fin)) {
    irq_restore(f);
    return false;
  }
  uint32_t seq = c->snd_nxt;
  tx_load(c, seq, seg + 20, len);
  if (c->snd_una == c->snd_nxt) {
    c->retries = 0;
    tcp_timer_arm(c);
  }
  if (len && !c->rtt_on) {
    c->rtt_on = true;
    c->rtt_seq = seq;
    c->rtt_start = timer_ticks();
  }
  c->snd_nxt = seq + len + (fin ? 1 : 0);
  irq_restore(f);
  tcp_xmit(c, seg, (uint8_t)(TCP_ACK | (len ? TCP_PSH : 0) | (fin ? TCP_FIN : 0)), seq, len);
  return true;
}

static void tcp_output(struct tcp_conn *c)
{
  while (tcp_output_one(c))
    ;
}

/* Retransmission timeout: resend the oldest unacknowledged segment, or probe a zero window; reset when out of
 * retries. Runs from tcp_tick, where sending never sleeps. */
static void tcp_timer_fire(struct tcp_conn *c)
{
  uint8_t seg[20 + TCP_MSS_MAX];
  uintptr_t f = irq_save();
  if (!tcb_sending(c) || !c->timer_on || !tick_passed(c->timer_at)) {
    irq_restore(f);
    return;
  }
  c->timer_on = false;
  bool in_flight = c->snd_una != c->snd_nxt;
  if (!in_flight && !seq_lt(c->snd_nxt, c->snd_end)) {
    irq_restore(f);
    return;
  }
  if (c->retries >= TCP_MAX_RETRIES) {
    tcp_send_ctl(c, TCP_RST | TCP_ACK, c->snd_nxt);
    tcp_drop(c);
    irq_restore(f);
    return;
  }
  c->retries++;
  tcp_timer_arm(c);
  if (!in_flight) {
    uint32_t probe = c->snd_una - 1;
    irq_restore(f);
    /* An already-acknowledged sequence number: the peer must answer with an ACK carrying its window. */
    tcp_send_ctl(c, TCP_ACK, probe);
    return;
  }
  c->rtt_on = false; /* Karn: no RTT sample from a retransmitted segment */
  uint32_t sent_end = seq_lt(c->snd_nxt, c->snd_end) ? c->snd_nxt : c->snd_end;
  uint32_t len = sent_end - c->snd_una;
  uint32_t mss = net_tcp_mss(c->peer_ip);
  if (len > mss)
    len = mss;
  bool fin = c->fin_queued && seq_gt(c->snd_nxt, c->snd_end) && c->snd_una + len == c->snd_end;
  uint32_t seq = c->snd_una;
  tx_load(c, seq, seg + 20, len);
  irq_restore(f);
  tcp_xmit(c, seg, (uint8_t)(TCP_ACK | (len ? TCP_PSH : 0) | (fin ? TCP_FIN : 0)), seq, len);
}

/* Process the acknowledgment and window of an acceptable segment (ack <= snd_nxt). */
static void tcp_ack_rcvd(struct tcp_conn *c, uint32_t seq, uint32_t ack, uint32_t win)
{
  uintptr_t f = irq_save();
  if (seq_gt(ack, c->snd_una)) {
    if (c->rtt_on && seq_gt(ack, c->rtt_seq)) {
      c->rtt_on = false;
      tcp_rtt_sample(c, timer_ticks() - c->rtt_start);
    }
    c->snd_una = ack;
    c->retries = 0;
    if (c->snd_una == c->snd_nxt)
      c->timer_on = false;
    else
      tcp_timer_arm(c);
    if (c->state == ST_LINGER && tcp_fin_acked(c) && !c->finned) {
      uint32_t until = timer_ticks() + TCP_FIN_WAIT2;
      if (seq_lt(until, c->linger_until))
        c->linger_until = until;
    }
  }
  if (seq_leq(c->snd_una, ack) && (seq_lt(c->snd_wl1, seq) || (c->snd_wl1 == seq && seq_leq(c->snd_wl2, ack)))) {
    c->snd_wnd = win;
    c->snd_wl1 = seq;
    c->snd_wl2 = ack;
  }
  if (c->snd_una == c->snd_nxt)
    c->retries = 0; /* the peer answered a zero-window probe */
  irq_restore(f);
}

/* Find the connection owning a segment from `src` with these ports, or NULL. */
static struct tcp_conn *tcp_demux(const uint8_t src[16], uint16_t dport, uint16_t sport)
{
  for (int i = 0; i < TCP_MAX_CONNS; i++) {
    struct tcp_conn *c = &conns[i];
    if (c->state == ST_CLOSED || c->state == ST_DONE)
      continue;
    if (c->lport != dport || c->rport != sport)
      continue;
    if (memcmp(src, c->peer_ip, 16) == 0)
      return c;
  }
  return NULL;
}

static bool tcp_is_listening(uint16_t port)
{
  for (int i = 0; i < TCP_MAX_LISTEN; i++)
    if (listeners[i].active && listeners[i].port == port)
      return true;
  return false;
}

/* Passive open on a SYN to a listening port: claim a buffered slot, reply SYN|ACK. IRQ context, no alloc. */
static void tcp_accept_syn(const uint8_t src[16], const struct tcp_hdr *t)
{
  struct tcp_conn *c = tcb_alloc(ST_SYN_RCVD, /*need_rxbuf=*/1);
  if (!c)
    return;

  memcpy(c->peer_ip, src, 16);
  c->lport = ntohs(t->dport);
  c->rport = ntohs(t->sport);
  uint32_t isn;
  entropy_get(&isn, 4);
  tcb_init_send(c, isn);
  c->rcv_nxt = ntohl(t->seq) + 1;
  c->wlen = 0;
  c->rlen = 0;
  c->adv_wnd = 0;
  c->finned = false;
  c->reset = false;
  c->accepted = false;
  c->passive = true;
  c->opened = timer_ticks();
  tcp_send_ctl(c, TCP_SYN | TCP_ACK, isn);
}

/* Is `seq` inside our receive window (wraparound-safe)? Rejects forged RSTs. */
static bool seq_in_rcv_window(const struct tcp_conn *c, uint32_t seq)
{
  uint32_t wnd = TCP_RXBUF - (c->wlen - c->rlen);
  if (wnd == 0)
    wnd = 1;
  return (int32_t)(seq - c->rcv_nxt) >= 0 && (int32_t)(seq - (c->rcv_nxt + wnd)) < 0;
}

/* RFC 793 acceptability, relaxed so a segment at rcv_nxt still carries its ACK through a zero window. */
static bool seg_acceptable(const struct tcp_conn *c, uint32_t seq, uint32_t seglen)
{
  uint32_t wnd = TCP_RXBUF - (c->wlen - c->rlen);
  return seq_geq(seq + seglen, c->rcv_nxt) && seq_leq(seq, c->rcv_nxt + wnd);
}

void tcp_input(const uint8_t src[16], const uint8_t dst[16], const uint8_t *seg, uint32_t len)
{
  if (len < 20)
    return;

  /* Drop segments with a bad checksum. */
  if (net_l4_csum(src, dst, IP_PROTO_TCP, seg, len) != 0)
    return;

  const struct tcp_hdr *t = (const struct tcp_hdr *)seg;
  if ((t->off & 0x0F) != 0)
    return;
  struct tcp_conn *c = tcp_demux(src, ntohs(t->dport), ntohs(t->sport));
  if (!c) {
    /* No TCB: a bare SYN (ACK clear) to a listening port opens a server connection. */
    if ((t->flags & (TCP_SYN | TCP_ACK)) == TCP_SYN && tcp_is_listening(ntohs(t->dport))) {
      tcp_accept_syn(src, t);
    }
    return;
  }

  uint8_t flags = t->flags;
  uint32_t seq = ntohl(t->seq);
  uint32_t ack = ntohl(t->ack);
  uint32_t win = ntohs(t->win);
  uint32_t hlen = (uint32_t)(t->off >> 4) * 4;
  if (hlen < 20 || hlen > len)
    return;
  const uint8_t *data = seg + hlen;
  uint32_t dlen = len - hlen;

  /* RFC 5961: accept RST only if it acks our SYN (SYN_SENT) or its seq is in the window. */
  if (flags & TCP_RST) {
    int ok = (c->state == ST_SYN_SENT) ? ((flags & TCP_ACK) && ack == c->snd_nxt) : seq_in_rcv_window(c, seq);
    if (ok) {
      uintptr_t f = irq_save();
      tcp_drop(c);
      irq_restore(f);
    }
    return;
  }

  if (c->state == ST_SYN_SENT) {
    /* Only accept a SYN|ACK that acknowledges the exact SYN we sent. */
    if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) && ack == c->snd_nxt) {
      c->rcv_nxt = seq + 1;
      c->snd_wnd = win;
      c->snd_wl1 = seq;
      c->snd_wl2 = ack;
      c->state = ST_ESTAB;
      tcp_send_ctl(c, TCP_ACK, c->snd_nxt);
    }
    return;
  }

  if (c->state == ST_SYN_RCVD) {
    if (flags & TCP_SYN) {
      tcp_send_ctl(c, TCP_SYN | TCP_ACK, c->snd_nxt - 1);
      return;
    }
    if (flags & TCP_ACK) {
      /* Must ack exactly isn+1, like the SYN_SENT path. */
      if (ack != c->snd_nxt)
        return;
      c->snd_wnd = win;
      c->snd_wl1 = seq;
      c->snd_wl2 = ack;
      c->state = ST_ESTAB;
    } else {
      return;
    }
  }

  /* A SYN on a synchronized connection gets a challenge ACK (RFC 5961); this also re-ACKs a resent SYN|ACK. */
  if (flags & TCP_SYN) {
    tcp_send_ctl(c, TCP_ACK, c->snd_nxt);
    return;
  }
  uint32_t seglen = dlen + ((flags & TCP_FIN) ? 1 : 0);
  /* Out-of-window and old duplicate segments (a resent FIN, a zero-window probe) are answered with an ACK. */
  if (!seg_acceptable(c, seq, seglen)) {
    tcp_send_ctl(c, TCP_ACK, c->snd_nxt);
    return;
  }
  if (!(flags & TCP_ACK))
    return;
  if (seq_gt(ack, c->snd_nxt)) {
    tcp_send_ctl(c, TCP_ACK, c->snd_nxt);
    return;
  }
  tcp_ack_rcvd(c, seq, ack, win);

  if (dlen > 0) {
    uint32_t skip = seq_lt(seq, c->rcv_nxt) ? c->rcv_nxt - seq : 0;
    if (skip < dlen && seq + skip == c->rcv_nxt) {
      const uint8_t *in = data + skip;
      uint32_t take = dlen - skip;
      if (c->state == ST_LINGER) {
        c->rcv_nxt += take; /* nobody will read it: acknowledge and discard */
      } else {
        /* wlen/rlen are monotonic counts indexed mod TCP_RXBUF. */
        uint32_t space = TCP_RXBUF - (c->wlen - c->rlen);
        if (take > space)
          take = space;
        if (take) {
          uint32_t woff = c->wlen % TCP_RXBUF;
          uint32_t first = TCP_RXBUF - woff;
          if (first >= take) {
            memcpy(c->rxbuf + woff, in, take);
          } else {
            memcpy(c->rxbuf + woff, in, first);
            memcpy(c->rxbuf, in + first, take - first);
          }
          c->wlen += take;
          c->rcv_nxt += take;
        }
      }
    }
  }
  if ((flags & TCP_FIN) && !c->finned && seq + dlen == c->rcv_nxt) {
    c->rcv_nxt += 1;
    c->finned = true;
    if (c->state == ST_ESTAB)
      c->state = ST_CLOSE_WAIT;
  }
  if (seglen > 0)
    tcp_send_ctl(c, TCP_ACK, c->snd_nxt);
  if (c->state == ST_LINGER && c->finned && tcp_fin_acked(c)) {
    uintptr_t f = irq_save();
    if (c->state == ST_LINGER) {
      c->timer_on = false;
      c->state = ST_CLOSED;
    }
    irq_restore(f);
  }
}

tcp_conn_t *tcp_connect(const uint8_t dst_ip[16], uint16_t dport)
{
  struct tcp_conn *c = tcb_alloc(ST_SYN_SENT, 0);
  if (!c)
    return NULL;
  int slot = (int)(c - conns);

  if (!c->rxbuf) {
    c->rxbuf = kmalloc(TCP_RXBUF);
    if (!c->rxbuf) {
      c->state = ST_CLOSED;
      return NULL;
    }
  }

  memcpy(c->peer_ip, dst_ip, 16);
  c->rport = dport;
  /* Each slot owns a distinct 0x400-wide local-port band. */
  c->lport = (uint16_t)(0xC000 + slot * 0x400 + (lport_seq++ & 0x3FF));
  uint32_t isn;
  entropy_get(&isn, 4);
  tcb_init_send(c, isn);
  c->rcv_nxt = 0;
  c->wlen = 0;
  c->rlen = 0;
  c->adv_wnd = 0;
  c->finned = false;
  c->reset = false;
  c->accepted = false;
  c->passive = false;
  c->state = ST_SYN_SENT;

  for (int tries = 0; tries < 6; tries++) {
    tcp_send_ctl(c, TCP_SYN, isn);
    uint32_t deadline = timer_ticks() + TIMER_HZ / 2;
    while (!tick_passed(deadline)) {
      __asm__ volatile("" ::: "memory");
      if (c->state == ST_ESTAB)
        return c;
      if (c->reset) {
        c->state = ST_CLOSED;
        return NULL;
      }
      if (sched_intr_pending()) {
        c->state = ST_CLOSED;
        return NULL;
      }
      __asm__ volatile("sti\n\thlt");
    }
  }
  if (c->state == ST_ESTAB)
    return c;
  c->state = ST_CLOSED;
  return NULL;
}

/* Queue data for sending, blocking while the send buffer is full. Returns the bytes queued, which falls short of
 * `len` only if interrupted or reset after some were queued; -1 or TCP_EINTR if none were. */
int tcp_write(tcp_conn_t *c, const void *data, uint32_t len)
{
  if (!c || (!data && len) || (c->state != ST_ESTAB && c->state != ST_CLOSE_WAIT))
    return -1;
  if (!c->txbuf) {
    c->txbuf = kmalloc(TCP_TXBUF);
    if (!c->txbuf)
      return -1;
  }
  const uint8_t *p = data;
  uint32_t done = 0;
  while (done < len) {
    if (c->reset || (c->state != ST_ESTAB && c->state != ST_CLOSE_WAIT))
      return done ? (int)done : -1;
    uint32_t room = TCP_TXBUF - (c->snd_end - c->snd_una);
    uint32_t n = len - done < room ? len - done : room;
    if (n) {
      tx_store(c, c->snd_end, p + done, n);
      __asm__ volatile("" ::: "memory");
      c->snd_end += n;
      done += n;
      tcp_output(c);
      continue;
    }
    if (sched_intr_pending())
      return done ? (int)done : TCP_EINTR;
    __asm__ volatile("sti\n\thlt");
  }
  return (int)done;
}

/* Non-blocking read: bytes copied, 0 if none, -1 on reset. *closed set once FIN is drained or on reset. */
int tcp_read_nb(tcp_conn_t *c, uint8_t *buf, uint32_t max, int *closed)
{
  *closed = 0;
  if (!c || c->reset) {
    *closed = 1;
    return -1;
  }
  uint32_t avail = c->wlen - c->rlen;
  if (avail > 0) {
    uint32_t n = avail < max ? avail : max;
    uint32_t roff = c->rlen % TCP_RXBUF;
    uint32_t first = TCP_RXBUF - roff;
    if (first >= n) {
      memcpy(buf, c->rxbuf + roff, n);
    } else {
      memcpy(buf, c->rxbuf + roff, first);
      memcpy((uint8_t *)buf + first, c->rxbuf, n - first);
    }
    c->rlen += n;

    /* RFC 1122 SWS: ACK once freed space exceeds the last advertised window by an MSS. */
    if (c->state == ST_ESTAB || c->state == ST_CLOSE_WAIT) {
      uint32_t freespace = TCP_RXBUF - (c->wlen - c->rlen);
      if (freespace >= c->adv_wnd + net_tcp_mss(c->peer_ip))
        tcp_send_ctl(c, TCP_ACK, c->snd_nxt);
    }

    *closed = c->finned && c->rlen == c->wlen;
    return (int)n;
  }
  if (c->finned)
    *closed = 1;
  return 0;
}

int tcp_read(tcp_conn_t *c, uint8_t *buf, uint32_t max, int *closed)
{
  if (!c) {
    *closed = 1;
    return -1;
  }
  uint32_t deadline = timer_ticks() + 5 * TIMER_HZ;
  for (;;) {
    int r = tcp_read_nb(c, buf, max, closed);
    if (r != 0)
      return r;
    if (*closed)
      return 0;
    if (sched_intr_pending())
      return TCP_EINTR;
    if (tick_passed(deadline)) {
      *closed = 1;
      return 0;
    }
    __asm__ volatile("sti\n\thlt");
  }
}

/* Never blocks: an open connection lingers, and tcp_tick delivers its queued data and FIN (bounded by
 * TCP_LINGER and the retry limit) before freeing the slot. */
void tcp_close(tcp_conn_t *c)
{
  if (!c)
    return;
  uintptr_t f = irq_save();
  if ((c->state == ST_ESTAB || c->state == ST_CLOSE_WAIT) && !c->reset) {
    c->rlen = c->wlen; /* unread data has no reader any more */
    c->fin_queued = true;
    c->linger_until = timer_ticks() + TCP_LINGER;
    c->state = ST_LINGER;
    irq_restore(f);
    tcp_output(c);
    return;
  }
  /* rxbuf and txbuf are kept for reuse; tcp_input ignores ST_CLOSED slots. */
  c->timer_on = false;
  c->state = ST_CLOSED;
  irq_restore(f);
}

int tcp_listen(uint16_t port)
{
  /* The IRQ accept path needs buffered slots, so allocate every rxbuf here. */
  for (int i = 0; i < TCP_MAX_CONNS; i++) {
    if (!conns[i].rxbuf) {
      conns[i].rxbuf = kmalloc(TCP_RXBUF);
      if (!conns[i].rxbuf)
        return -1;
    }
  }

  uint32_t f = irq_save();
  int slot = -1;
  for (int i = 0; i < TCP_MAX_LISTEN; i++) {
    if (listeners[i].active && listeners[i].port == port) { /* in use: sharing would split accepts */
      irq_restore(f);
      return -1;
    }
    if (slot < 0 && !listeners[i].active)
      slot = i;
  }
  if (slot < 0) {
    irq_restore(f);
    return -1;
  }
  listeners[slot].port = port;
  listeners[slot].active = true;
  irq_restore(f);
  return 0;
}

void tcp_unlisten(uint16_t port)
{
  for (int i = 0; i < TCP_MAX_LISTEN; i++)
    if (listeners[i].active && listeners[i].port == port)
      listeners[i].active = false;
  /* Connections still waiting to be accepted have no other owner. */
  for (int i = 0; i < TCP_MAX_CONNS; i++) {
    struct tcp_conn *c = &conns[i];
    if (c->state != ST_CLOSED && c->state != ST_LINGER && c->passive && !c->accepted && c->lport == port)
      tcp_close(c);
  }
}

/* Reap half-open (SYN_RCVD) connections whose peer never sent the final ACK. */
static void tcp_reap_halfopen(void)
{
  uint32_t now = timer_ticks();
  for (int i = 0; i < TCP_MAX_CONNS; i++) {
    struct tcp_conn *c = &conns[i];
    if (c->state == ST_SYN_RCVD && now - c->opened > TCP_SYN_RCVD_TIMEOUT) {
      uint32_t f = irq_save();
      if (c->state == ST_SYN_RCVD)
        c->state = ST_CLOSED;
      irq_restore(f);
    }
  }
}

/* A closed connection ran out of time: reset the peer if it still lacks our data or FIN, then free the slot. */
static void tcp_linger_expire(struct tcp_conn *c)
{
  uintptr_t f = irq_save();
  if (c->state == ST_LINGER) {
    if (!tcp_fin_acked(c))
      tcp_send_ctl(c, TCP_RST | TCP_ACK, c->snd_nxt);
    c->timer_on = false;
    c->state = ST_CLOSED;
  }
  irq_restore(f);
}

/* Every tick from the network thread: retransmission and persist timers, newly opened windows, lingering closes. */
void tcp_tick(void)
{
  tcp_reap_halfopen();
  for (int i = 0; i < TCP_MAX_CONNS; i++) {
    struct tcp_conn *c = &conns[i];
    if (!tcb_sending(c))
      continue;
    if (c->state == ST_LINGER && tick_passed(c->linger_until)) {
      tcp_linger_expire(c);
      continue;
    }
    if (c->timer_on && tick_passed(c->timer_at))
      tcp_timer_fire(c);
    tcp_output(c);
    uintptr_t f = irq_save();
    if (tcb_sending(c) && !c->timer_on && (c->snd_una != c->snd_nxt || seq_lt(c->snd_nxt, c->snd_end)))
      tcp_timer_arm(c);
    irq_restore(f);
  }
}

tcp_conn_t *tcp_accept(uint16_t port)
{
  for (;;) {
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
      struct tcp_conn *c = &conns[i];
      /* A connection the peer already half-closed is still handed out: its data and EOF remain readable. */
      bool open = c->state == ST_ESTAB || c->state == ST_CLOSE_WAIT;
      if (!open || c->lport != port || c->accepted || !c->passive)
        continue; /* an outgoing connection can share the port number but is never a listener's */
      uint32_t f = irq_save();
      if ((c->state == ST_ESTAB || c->state == ST_CLOSE_WAIT) && !c->accepted) {
        c->accepted = true;
        irq_restore(f);
        return c;
      }
      irq_restore(f);
    }
    tcp_reap_halfopen();
    if (sched_intr_pending())
      return NULL;
    __asm__ volatile("sti\n\thlt");
  }
}
