/* tcp.c — a minimal TCP client with several concurrent connections (see tcp.h).
 *
 * Each connection is a control block (struct tcp_conn) drawn from a fixed
 * table. tcp_input() demultiplexes an inbound segment to the owning TCB by its
 * 4-tuple (peer IP + both ports), so distinct connections never see each
 * other's bytes. Everything that used to be a module global now lives in the
 * TCB, and the transmit/receive helpers take the connection they act on.
 *
 * Sequence-number ownership keeps the two execution contexts from racing
 * within a single connection: snd_nxt is advanced only in command context
 * (connect/write/close); rcv_nxt and the receive buffer are advanced only in
 * interrupt context (tcp_input). Both are read from either side, but each has
 * a single writer, so plain volatile reads are safe on this single-core
 * kernel. The table itself is only structurally mutated (slot claimed/freed)
 * from command context, and tcp_input never claims or frees a slot — it only
 * touches slots already established — so the demux walk needs no lock either.
 *
 * ACKs are emitted straight from tcp_input so the peer keeps streaming without
 * waiting for the reader to be scheduled. Those run on whatever (possibly
 * 4 KiB) kernel stack the interrupt landed on, so the control-segment path
 * uses a 20-byte stack buffer; only the command-context data send uses a
 * full-MSS buffer.
 *
 * Server side: tcp_listen() marks a port open and pre-allocates the per-slot
 * receive buffers, so the passive-open path in tcp_input() — which runs in the
 * IRQ and *does* claim a slot — never allocates. That claim is the one place
 * the interrupt context structurally mutates the table, so slot allocation
 * goes through tcb_alloc(), which brackets the scan-and-reserve with IF off;
 * tcp_connect() (command context) uses the same helper, keeping the two
 * contexts from ever claiming the same slot. Once a passively-opened
 * connection reaches ESTAB it is indistinguishable from a tcp_connect() one and
 * shares every read/write/close path. tcp_accept() hands an established,
 * unclaimed connection to a daemon thread.
 */

#include <stdint.h>
#include <stddef.h>

#include "tcp.h"
#include "net.h"      /* net_ip_send, net_my_ip */
#include "heap.h"
#include "timer.h"
#include "kstring.h"
#include "sched.h"    /* sched_intr_pending: cooperative Ctrl+C abort */

/* ------------------------------------------------------------------ helpers */

static inline uint16_t bswap16(uint16_t x) { return (uint16_t)((x << 8) | (x >> 8)); }
static inline uint32_t bswap32(uint32_t x)
{
	return (x << 24) | ((x & 0xFF00) << 8) | ((x >> 8) & 0xFF00) | (x >> 24);
}
#define htons(x) bswap16((uint16_t)(x))
#define ntohs(x) bswap16((uint16_t)(x))
#define htonl(x) bswap32((uint32_t)(x))
#define ntohl(x) bswap32((uint32_t)(x))

/* ------------------------------------------------------------------ on wire */

struct tcp_hdr {
	uint16_t sport, dport;
	uint32_t seq;
	uint32_t ack;
	uint8_t  off;          /* data offset in 32-bit words, high nibble        */
	uint8_t  flags;
	uint16_t win;
	uint16_t csum;
	uint16_t urg;
} __attribute__((packed));

#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10

#define IP_PROTO_TCP 6
#define TCP_MSS      1460
#define TCP_RXBUF    65536u
#define TCP_MAX_CONNS 8
#define TCP_MAX_LISTEN 4
#define TCP_SYN_RCVD_TIMEOUT (2 * TIMER_HZ)  /* ~2 s before a half-open is reaped */

enum { ST_CLOSED, ST_LISTEN, ST_SYN_SENT, ST_SYN_RCVD,
       ST_ESTAB, ST_CLOSE_WAIT, ST_DONE };

/* ------------------------------------------------------------------- state */

/* One connection control block. `state` doubles as the in-use marker: a slot
 * is free iff state == ST_CLOSED. */
struct tcp_conn {
	volatile int      state;
	uint8_t           peer_ip[4];
	uint16_t          lport, rport;

	volatile uint32_t snd_nxt;       /* command context writes this        */
	volatile uint32_t rcv_nxt;       /* interrupt context writes this      */
	volatile int      finned;        /* peer sent FIN                      */
	volatile int      reset;         /* peer sent RST                      */

	uint8_t          *rxbuf;         /* lazily allocated, retained per slot */
	volatile uint32_t wlen;          /* bytes written by the IRQ           */
	volatile uint32_t rlen;          /* bytes consumed by the reader       */
	volatile uint32_t adv_wnd;       /* last receive window we advertised  */

	volatile int      accepted;      /* server: handed out by tcp_accept   */
	uint32_t          opened;        /* timer_ticks() at SYN_RCVD (reaping)*/
};

static struct tcp_conn conns[TCP_MAX_CONNS];

/* Open listening ports. A SYN whose dport matches a live entry is accepted. */
static struct { uint16_t port; volatile int active; } listeners[TCP_MAX_LISTEN];

/* Rotating source of ephemeral local ports; combined with the slot index so
 * two live connections can never share a local port (see tcp_connect). */
static uint16_t lport_seq;

/* ----------------------------------------------------------- slot allocation */

/* irq_save / irq_restore: snapshot EFLAGS + cli, restore the prior IF — safe to
 * call from the IRQ (IF already clear stays interrupts-off). */
#include "irqflags.h"

/* Claim a free slot (state == ST_CLOSED), reserving it as `newstate` so a
 * concurrent claim from the other execution context can't take the same slot.
 * With need_rxbuf set (the IRQ accept path) only slots whose buffer is already
 * allocated qualify, since the IRQ must not call kmalloc. Returns NULL if the
 * table is full (or, for need_rxbuf, no buffered slot is free). */
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

/* --------------------------------------------------------------- checksum */

/* TCP checksum over the IPv4 pseudo-header + segment. Reads big-endian words
 * and returns the host value; store it with htons(). Mandatory for TCP. */
static uint16_t tcp_checksum(const uint8_t src[4], const uint8_t dst[4],
                             const uint8_t *seg, uint32_t len)
{
	uint32_t sum = 0;
	sum += ((uint32_t)src[0] << 8) | src[1];
	sum += ((uint32_t)src[2] << 8) | src[3];
	sum += ((uint32_t)dst[0] << 8) | dst[1];
	sum += ((uint32_t)dst[2] << 8) | dst[3];
	sum += IP_PROTO_TCP;            /* zero byte + protocol                    */
	sum += len;                     /* TCP length                              */

	for (uint32_t i = 0; i + 1 < len; i += 2)
		sum += ((uint32_t)seg[i] << 8) | seg[i + 1];
	if (len & 1)
		sum += (uint32_t)seg[len - 1] << 8;

	while (sum >> 16)
		sum = (sum & 0xFFFF) + (sum >> 16);
	return (uint16_t)~sum;
}

/* --------------------------------------------------------------- transmit */

/* Fill a header (data already at seg+20 if any), checksum, and hand to IP. */
static void tcp_xmit(struct tcp_conn *c, uint8_t *seg, uint8_t flags,
                     uint32_t seq, uint32_t dlen)
{
	struct tcp_hdr *t = (struct tcp_hdr *)seg;
	t->sport = htons(c->lport);
	t->dport = htons(c->rport);
	t->seq   = htonl(seq);
	t->ack   = htonl(c->rcv_nxt);
	t->off   = 5 << 4;              /* 20-byte header, no options              */
	t->flags = flags;
	uint32_t space = TCP_RXBUF - (c->wlen - c->rlen);
	if (space > 65535) space = 65535;
	t->win   = htons((uint16_t)space);
	c->adv_wnd = space;             /* remember what the peer now believes */
	t->csum  = 0;
	t->urg   = 0;
	t->csum  = htons(tcp_checksum(net_my_ip(), c->peer_ip, seg, 20 + dlen));
	net_ip_send(c->peer_ip, IP_PROTO_TCP, seg, 20 + dlen);
}

/* Control segment (no payload). Small stack buffer — safe in interrupt ctx. */
static void tcp_send_ctl(struct tcp_conn *c, uint8_t flags, uint32_t seq)
{
	uint8_t seg[20];
	tcp_xmit(c, seg, flags, seq, 0);
}

/* Data segment. Command context only (full-MSS stack buffer). */
static void tcp_send_data(struct tcp_conn *c, uint32_t seq,
                          const uint8_t *data, uint32_t dlen)
{
	uint8_t seg[20 + TCP_MSS];
	if (dlen > TCP_MSS)
		dlen = TCP_MSS;
	memcpy(seg + 20, data, dlen);
	tcp_xmit(c, seg, TCP_PSH | TCP_ACK, seq, dlen);
}

/* ----------------------------------------------------------------- receive */

/* Find the live connection owning a segment from `src` with these ports.
 * Returns NULL if no established/closing TCB matches (stray or stale segment). */
static struct tcp_conn *tcp_demux(const uint8_t src[4], uint16_t dport,
                                  uint16_t sport)
{
	for (int i = 0; i < TCP_MAX_CONNS; i++) {
		struct tcp_conn *c = &conns[i];
		if (c->state == ST_CLOSED || c->state == ST_DONE)
			continue;
		if (c->lport != dport || c->rport != sport)
			continue;
		int ip_ok = 1;
		for (int j = 0; j < 4; j++)
			if (src[j] != c->peer_ip[j])
				ip_ok = 0;
		if (ip_ok)
			return c;
	}
	return NULL;
}

/* Is `port` an open listening port? (interrupt + command context) */
static int tcp_is_listening(uint16_t port)
{
	for (int i = 0; i < TCP_MAX_LISTEN; i++)
		if (listeners[i].active && listeners[i].port == port)
			return 1;
	return 0;
}

/* Passive open: a SYN landed on a listening port. Claim a buffered slot,
 * record the peer, and complete the handshake by replying SYN|ACK. Runs in
 * interrupt context, so it allocates nothing (tcb_alloc requires a ready
 * rxbuf) and uses only the small control-segment stack buffer. The connection
 * sits in SYN_RCVD until the peer's ACK promotes it to ESTAB in tcp_input. */
static void tcp_accept_syn(const uint8_t src[4], const struct tcp_hdr *t)
{
	struct tcp_conn *c = tcb_alloc(ST_SYN_RCVD, /*need_rxbuf=*/1);
	if (!c)
		return;                          /* table full / no buffer → client resends */

	for (int i = 0; i < 4; i++)
		c->peer_ip[i] = src[i];
	c->lport  = ntohs(t->dport);
	c->rport  = ntohs(t->sport);
	uint32_t isn = timer_us();
	c->snd_nxt = isn + 1;                    /* our SYN consumes isn               */
	c->rcv_nxt = ntohl(t->seq) + 1;          /* their SYN consumes one             */
	c->wlen    = 0;
	c->rlen    = 0;
	c->adv_wnd = 0;
	c->finned  = 0;
	c->reset   = 0;
	c->accepted = 0;
	c->opened  = timer_ticks();
	tcp_send_ctl(c, TCP_SYN | TCP_ACK, isn);
}

void tcp_input(const uint8_t src[4], const uint8_t *seg, uint32_t len)
{
	if (len < 20)
		return;

	const struct tcp_hdr *t = (const struct tcp_hdr *)seg;
	struct tcp_conn *c = tcp_demux(src, ntohs(t->dport), ntohs(t->sport));
	if (!c) {
		/* No live TCB — a bare SYN to a listening port opens a server
		 * connection. (SYN|ACK would belong to a client TCB, so require
		 * SYN set and ACK clear.) The handshake completes right here. */
		if ((t->flags & (TCP_SYN | TCP_ACK)) == TCP_SYN &&
		    tcp_is_listening(ntohs(t->dport))) {
			tcp_accept_syn(src, t);
		}
		return;
	}

	uint8_t  flags = t->flags;
	uint32_t seq   = ntohl(t->seq);
	uint32_t hlen  = (uint32_t)(t->off >> 4) * 4;
	if (hlen < 20 || hlen > len)
		return;
	const uint8_t *data = seg + hlen;
	uint32_t dlen = len - hlen;

	if (flags & TCP_RST) {
		c->reset = 1;
		c->state = ST_DONE;
		return;
	}

	if (c->state == ST_SYN_SENT) {
		if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
			c->rcv_nxt = seq + 1;            /* their SYN consumes one seq */
			c->state   = ST_ESTAB;
			tcp_send_ctl(c, TCP_ACK, c->snd_nxt);
		}
		return;
	}

	if (c->state == ST_SYN_RCVD) {
		if (flags & TCP_SYN) {                   /* our SYN|ACK was lost; resend */
			tcp_send_ctl(c, TCP_SYN | TCP_ACK, c->snd_nxt - 1);
			return;
		}
		if (flags & TCP_ACK) {                   /* third leg of the handshake   */
			c->state = ST_ESTAB;
		}
		/* fall through: the ACK may piggyback the first data byte(s) */
	}

	/* ESTAB / CLOSE_WAIT: accept in-order data, then ACK; handle FIN. */
	if (dlen > 0) {
		if (seq == c->rcv_nxt) {
			/* rxbuf is a ring: wlen/rlen are monotonic byte counts, indexed
			 * mod TCP_RXBUF, so draining by the reader frees space for the
			 * writer (a single connection can receive far more than its
			 * buffer over its lifetime). */
			uint32_t space = TCP_RXBUF - (c->wlen - c->rlen);
			uint32_t take  = dlen <= space ? dlen : space;
			if (take) {
				uint32_t woff  = c->wlen % TCP_RXBUF;
				uint32_t first = TCP_RXBUF - woff;   /* until wrap */
				if (first >= take) {
					memcpy(c->rxbuf + woff, data, take);
				} else {
					memcpy(c->rxbuf + woff, data, first);
					memcpy(c->rxbuf, data + first, take - first);
				}
				c->wlen    += take;
				c->rcv_nxt += take;
			}
		}
		tcp_send_ctl(c, TCP_ACK, c->snd_nxt);  /* ACK in-order or duplicate */
	}
	if ((flags & TCP_FIN) && seq + dlen == c->rcv_nxt) {
		c->rcv_nxt += 1;                        /* FIN consumes one seq      */
		c->finned   = 1;
		c->state    = ST_CLOSE_WAIT;
		tcp_send_ctl(c, TCP_ACK, c->snd_nxt);
	}
}

/* ------------------------------------------------------------------ client */

tcp_conn_t *tcp_connect(const uint8_t dst_ip[4], uint16_t dport)
{
	/* Claim a free slot, reserved as SYN_SENT so the accept path skips it. */
	struct tcp_conn *c = tcb_alloc(ST_SYN_SENT, 0);
	if (!c)
		return NULL;                /* table full                              */
	int slot = (int)(c - conns);

	if (!c->rxbuf) {
		c->rxbuf = kmalloc(TCP_RXBUF);
		if (!c->rxbuf) {
			c->state = ST_CLOSED;   /* release the reservation                */
			return NULL;
		}
	}

	for (int i = 0; i < 4; i++)
		c->peer_ip[i] = dst_ip[i];
	c->rport = dport;
	/* Each slot owns a distinct 0x400-wide local-port band, so two live
	 * connections never collide even if the low bits wrap. */
	c->lport  = (uint16_t)(0xC000 + slot * 0x400 + (lport_seq++ & 0x3FF));
	uint32_t isn = timer_us();
	c->snd_nxt = isn + 1;           /* our SYN consumes isn                    */
	c->rcv_nxt = 0;
	c->wlen    = 0;
	c->rlen    = 0;
	c->adv_wnd = 0;
	c->finned  = 0;
	c->reset   = 0;
	c->accepted = 0;
	c->state   = ST_SYN_SENT;

	for (int tries = 0; tries < 6; tries++) {
		tcp_send_ctl(c, TCP_SYN, isn);
		uint32_t deadline = timer_ticks() + TIMER_HZ / 2;   /* ~500 ms        */
		while (timer_ticks() < deadline) {
			__asm__ volatile ("" ::: "memory");
			if (c->state == ST_ESTAB)
				return c;
			if (c->reset) {
				c->state = ST_CLOSED;    /* release the slot */
				return NULL;
			}
			if (sched_intr_pending()) {  /* Ctrl+C: give up connecting */
				c->state = ST_CLOSED;
				return NULL;
			}
			__asm__ volatile ("sti\n\thlt");  /* never halt with IF clear */
		}
	}
	if (c->state == ST_ESTAB)
		return c;
	c->state = ST_CLOSED;                    /* gave up; release the slot */
	return NULL;
}

int tcp_write(tcp_conn_t *c, const void *data, uint32_t len)
{
	if (!c || (c->state != ST_ESTAB && c->state != ST_CLOSE_WAIT))
		return -1;
	const uint8_t *p = data;
	uint32_t sent = 0;
	while (sent < len) {
		uint32_t chunk = len - sent;
		if (chunk > TCP_MSS)
			chunk = TCP_MSS;
		tcp_send_data(c, c->snd_nxt, p + sent, chunk);
		c->snd_nxt += chunk;
		sent       += chunk;
	}
	return (int)sent;
}

/* Non-blocking read: returns bytes copied (>0), or 0 if none are available
 * right now, or -1 on reset. *closed is set when the peer's FIN has been seen
 * and the buffer is drained (or on reset). Never sleeps -- the caller polls. */
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
		uint32_t roff  = c->rlen % TCP_RXBUF;     /* ring read index */
		uint32_t first = TCP_RXBUF - roff;        /* until wrap      */
		if (first >= n) {
			memcpy(buf, c->rxbuf + roff, n);
		} else {
			memcpy(buf, c->rxbuf + roff, first);
			memcpy((uint8_t *)buf + first, c->rxbuf, n - first);
		}
		c->rlen += n;

		/* Window update (RFC 1122 SWS avoidance): draining freed space the peer
		 * doesn't know about. Once that space has grown an MSS past what we last
		 * advertised, send an ACK now — otherwise a receive buffer that filled
		 * while the app was busy leaves the sender stalled until its persist
		 * timer fires (~an RTO each time), which is the stop-and-go that throttles
		 * large downloads to a crawl. */
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
	return 0;                                         /* no data yet           */
}

int tcp_read(tcp_conn_t *c, uint8_t *buf, uint32_t max, int *closed)
{
	if (!c) {
		*closed = 1;
		return -1;
	}
	uint32_t deadline = timer_ticks() + 5 * TIMER_HZ;   /* ~5 s idle timeout   */
	for (;;) {
		int r = tcp_read_nb(c, buf, max, closed);
		if (r != 0)                            /* data, or error            */
			return r;
		if (*closed)                           /* FIN drained               */
			return 0;
		if (sched_intr_pending())              /* Ctrl+C: abort the read    */
			return TCP_EINTR;
		if (timer_ticks() > deadline) {
			*closed = 1;
			return 0;
		}
		__asm__ volatile ("sti\n\thlt");  /* never halt with IF clear */
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
	/* Release the slot for reuse. The rxbuf is retained for the next
	 * connection that lands here, avoiding heap churn. tcp_input() ignores
	 * ST_CLOSED slots, so a late retransmit after this point is dropped. */
	c->state = ST_CLOSED;
}

/* ------------------------------------------------------------------ server */

int tcp_listen(uint16_t port)
{
	/* The IRQ accept path only takes slots that already have a buffer, so
	 * pre-allocate every slot's rxbuf here, in command context. */
	for (int i = 0; i < TCP_MAX_CONNS; i++) {
		if (!conns[i].rxbuf) {
			conns[i].rxbuf = kmalloc(TCP_RXBUF);
			if (!conns[i].rxbuf)
				return -1;       /* out of heap */
		}
	}

	uint32_t f = irq_save();
	int slot = -1;
	for (int i = 0; i < TCP_MAX_LISTEN; i++) {
		if (listeners[i].active && listeners[i].port == port) {
			irq_restore(f);
			return 0;                /* already listening */
		}
		if (slot < 0 && !listeners[i].active)
			slot = i;
	}
	if (slot < 0) {
		irq_restore(f);
		return -1;                       /* listener table full */
	}
	listeners[slot].port   = port;
	listeners[slot].active = 1;
	irq_restore(f);
	return 0;
}

void tcp_unlisten(uint16_t port)
{
	for (int i = 0; i < TCP_MAX_LISTEN; i++)
		if (listeners[i].active && listeners[i].port == port)
			listeners[i].active = 0;
}

/* Reap half-open connections (SYN_RCVD whose peer never sent the final ACK)
 * so a few stray or malicious SYNs can't pin the whole table. */
static void tcp_reap_halfopen(void)
{
	uint32_t now = timer_ticks();
	for (int i = 0; i < TCP_MAX_CONNS; i++) {
		struct tcp_conn *c = &conns[i];
		if (c->state == ST_SYN_RCVD &&
		    now - c->opened > TCP_SYN_RCVD_TIMEOUT) {
			uint32_t f = irq_save();
			if (c->state == ST_SYN_RCVD)     /* recheck under IF off */
				c->state = ST_CLOSED;
			irq_restore(f);
		}
	}
}

tcp_conn_t *tcp_accept(uint16_t port, volatile int *stop)
{
	for (;;) {
		for (int i = 0; i < TCP_MAX_CONNS; i++) {
			struct tcp_conn *c = &conns[i];
			if (c->state != ST_ESTAB || c->lport != port || c->accepted)
				continue;
			uint32_t f = irq_save();
			if (c->state == ST_ESTAB && !c->accepted) {
				c->accepted = 1;
				irq_restore(f);
				return c;
			}
			irq_restore(f);
		}
		tcp_reap_halfopen();
		if (stop && *stop)
			return NULL;
		__asm__ volatile ("sti\n\thlt");  /* never halt with IF clear */
	}
}
