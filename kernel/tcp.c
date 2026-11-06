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
#define TCP_MSS 1460
#define TCP_RXBUF 65536u
#define TCP_MAX_CONNS 8
#define TCP_MAX_LISTEN 4
#define TCP_SYN_RCVD_TIMEOUT (2 * TIMER_HZ)

enum { ST_CLOSED, ST_LISTEN, ST_SYN_SENT, ST_SYN_RCVD, ST_ESTAB, ST_CLOSE_WAIT, ST_DONE };

/* One connection control block; a slot is free iff state == ST_CLOSED. */
struct tcp_conn {
  volatile int state;
  uint8_t peer_ip[4];
  uint16_t lport, rport;

  volatile uint32_t snd_nxt; /* written in command context */
  volatile uint32_t rcv_nxt; /* written in interrupt context */
  volatile bool finned;
  volatile bool reset;

  uint8_t *rxbuf;            /* allocated lazily, kept per slot */
  volatile uint32_t wlen;    /* bytes written by the IRQ */
  volatile uint32_t rlen;    /* bytes consumed by the reader */
  volatile uint32_t adv_wnd; /* last window advertised */

  volatile bool accepted; /* handed out by tcp_accept */
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

/* TCP checksum over IPv4 pseudo-header + segment; returns host value, store with htons(). */
static uint16_t tcp_checksum(const uint8_t src[4], const uint8_t dst[4], const uint8_t *seg, uint32_t len)
{
  uint32_t sum = 0;
  sum += ((uint32_t)src[0] << 8) | src[1];
  sum += ((uint32_t)src[2] << 8) | src[3];
  sum += ((uint32_t)dst[0] << 8) | dst[1];
  sum += ((uint32_t)dst[2] << 8) | dst[3];
  sum += IP_PROTO_TCP;
  sum += len;

  for (uint32_t i = 0; i + 1 < len; i += 2)
    sum += ((uint32_t)seg[i] << 8) | seg[i + 1];
  if (len & 1)
    sum += (uint32_t)seg[len - 1] << 8;

  while (sum >> 16)
    sum = (sum & 0xFFFF) + (sum >> 16);
  return (uint16_t)~sum;
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
  t->csum = htons(tcp_checksum(net_my_ip(), c->peer_ip, seg, 20 + dlen));
  net_ip_send(c->peer_ip, IP_PROTO_TCP, seg, 20 + dlen);
}

/* Control segment, no payload; small stack buffer, safe in interrupt context. */
static void tcp_send_ctl(struct tcp_conn *c, uint8_t flags, uint32_t seq)
{
  uint8_t seg[20];
  tcp_xmit(c, seg, flags, seq, 0);
}

/* Data segment; command context only. */
static void tcp_send_data(struct tcp_conn *c, uint32_t seq, const uint8_t *data, uint32_t dlen)
{
  uint8_t seg[20 + TCP_MSS];
  if (dlen > TCP_MSS)
    dlen = TCP_MSS;
  memcpy(seg + 20, data, dlen);
  tcp_xmit(c, seg, TCP_PSH | TCP_ACK, seq, dlen);
}

/* Find the connection owning a segment from `src` with these ports, or NULL. */
static struct tcp_conn *tcp_demux(const uint8_t src[4], uint16_t dport, uint16_t sport)
{
  for (int i = 0; i < TCP_MAX_CONNS; i++) {
    struct tcp_conn *c = &conns[i];
    if (c->state == ST_CLOSED || c->state == ST_DONE)
      continue;
    if (c->lport != dport || c->rport != sport)
      continue;
    bool ip_ok = true;
    for (int j = 0; j < 4; j++)
      if (src[j] != c->peer_ip[j])
        ip_ok = false;
    if (ip_ok)
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
static void tcp_accept_syn(const uint8_t src[4], const struct tcp_hdr *t)
{
  struct tcp_conn *c = tcb_alloc(ST_SYN_RCVD, /*need_rxbuf=*/1);
  if (!c)
    return;

  for (int i = 0; i < 4; i++)
    c->peer_ip[i] = src[i];
  c->lport = ntohs(t->dport);
  c->rport = ntohs(t->sport);
  uint32_t isn;
  entropy_get(&isn, 4);
  c->snd_nxt = isn + 1;
  c->rcv_nxt = ntohl(t->seq) + 1;
  c->wlen = 0;
  c->rlen = 0;
  c->adv_wnd = 0;
  c->finned = false;
  c->reset = false;
  c->accepted = false;
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

void tcp_input(const uint8_t src[4], const uint8_t *seg, uint32_t len)
{
  if (len < 20)
    return;

  /* Drop segments with a bad checksum. */
  if (tcp_checksum(src, net_my_ip(), seg, len) != 0)
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
  uint32_t hlen = (uint32_t)(t->off >> 4) * 4;
  if (hlen < 20 || hlen > len)
    return;
  const uint8_t *data = seg + hlen;
  uint32_t dlen = len - hlen;

  /* RFC 5961: accept RST only if it acks our SYN (SYN_SENT) or its seq is in the window. */
  if (flags & TCP_RST) {
    int ok = (c->state == ST_SYN_SENT) ? ((flags & TCP_ACK) && ack == c->snd_nxt) : seq_in_rcv_window(c, seq);
    if (ok) {
      c->reset = true;
      c->state = ST_DONE;
    }
    return;
  }

  if (c->state == ST_SYN_SENT) {
    /* Only accept a SYN|ACK that acknowledges the exact SYN we sent. */
    if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) && ack == c->snd_nxt) {
      c->rcv_nxt = seq + 1;
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
      c->state = ST_ESTAB;
    } else {
      return;
    }
  }

  if (dlen > 0) {
    if (seq == c->rcv_nxt) {
      /* wlen/rlen are monotonic counts indexed mod TCP_RXBUF. */
      uint32_t space = TCP_RXBUF - (c->wlen - c->rlen);
      uint32_t take = dlen <= space ? dlen : space;
      if (take) {
        uint32_t woff = c->wlen % TCP_RXBUF;
        uint32_t first = TCP_RXBUF - woff;
        if (first >= take) {
          memcpy(c->rxbuf + woff, data, take);
        } else {
          memcpy(c->rxbuf + woff, data, first);
          memcpy(c->rxbuf, data + first, take - first);
        }
        c->wlen += take;
        c->rcv_nxt += take;
      }
    }
    tcp_send_ctl(c, TCP_ACK, c->snd_nxt);
  }
  if ((flags & TCP_FIN) && seq + dlen == c->rcv_nxt) {
    c->rcv_nxt += 1;
    c->finned = true;
    c->state = ST_CLOSE_WAIT;
    tcp_send_ctl(c, TCP_ACK, c->snd_nxt);
  }
}

tcp_conn_t *tcp_connect(const uint8_t dst_ip[4], uint16_t dport)
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

  for (int i = 0; i < 4; i++)
    c->peer_ip[i] = dst_ip[i];
  c->rport = dport;
  /* Each slot owns a distinct 0x400-wide local-port band. */
  c->lport = (uint16_t)(0xC000 + slot * 0x400 + (lport_seq++ & 0x3FF));
  uint32_t isn;
  entropy_get(&isn, 4);
  c->snd_nxt = isn + 1;
  c->rcv_nxt = 0;
  c->wlen = 0;
  c->rlen = 0;
  c->adv_wnd = 0;
  c->finned = false;
  c->reset = false;
  c->accepted = false;
  c->state = ST_SYN_SENT;

  for (int tries = 0; tries < 6; tries++) {
    tcp_send_ctl(c, TCP_SYN, isn);
    uint32_t deadline = timer_ticks() + TIMER_HZ / 2;
    while (timer_ticks() < deadline) {
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

int tcp_write(tcp_conn_t *c, const void *data, uint32_t len)
{
  if (!c || (!data && len) || (c->state != ST_ESTAB && c->state != ST_CLOSE_WAIT))
    return -1;
  const uint8_t *p = data;
  uint32_t sent = 0;
  while (sent < len) {
    uint32_t chunk = len - sent;
    if (chunk > TCP_MSS)
      chunk = TCP_MSS;
    tcp_send_data(c, c->snd_nxt, p + sent, chunk);
    c->snd_nxt += chunk;
    sent += chunk;
  }
  return (int)sent;
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
      if (freespace >= c->adv_wnd + TCP_MSS)
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
    if (timer_ticks() > deadline) {
      *closed = 1;
      return 0;
    }
    __asm__ volatile("sti\n\thlt");
  }
}

void tcp_close(tcp_conn_t *c)
{
  if (!c)
    return;
  if (c->state == ST_ESTAB || c->state == ST_CLOSE_WAIT) {
    tcp_send_ctl(c, TCP_FIN | TCP_ACK, c->snd_nxt);
    c->snd_nxt += 1;
  }
  /* rxbuf is kept for reuse; tcp_input ignores ST_CLOSED slots. */
  c->state = ST_CLOSED;
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
    if (listeners[i].active && listeners[i].port == port) {
      irq_restore(f);
      return 0;
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

tcp_conn_t *tcp_accept(uint16_t port)
{
  for (;;) {
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
      struct tcp_conn *c = &conns[i];
      if (c->state != ST_ESTAB || c->lport != port || c->accepted)
        continue;
      uint32_t f = irq_save();
      if (c->state == ST_ESTAB && !c->accepted) {
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
