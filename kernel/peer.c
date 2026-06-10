/* peer.c — BitTorrent peer wire protocol, non-blocking state machine. See peer.h. */

#include "peer.h"
#include "torrent.h"
#include "tcp.h"
#include "crypto.h"
#include "kstring.h"
#include "heap.h"
#include "timer.h"

/* Our 20-byte peer id (Azureus-style prefix + filler); opaque to the peer. */
static const char PEER_ID[] = "-OW0001-owos00000001";  /* 20 bytes used */

#define PSTR     "BitTorrent protocol"
#define PSTRLEN  19
#define HS_LEN   68             /* 1 + 19 + 8 + 20 + 20                        */
#define BLOCK    16384u         /* canonical request block size               */
#define MAX_PENDING 8           /* outstanding requests (request pipelining)   */

enum { MSG_CHOKE = 0, MSG_UNCHOKE = 1, MSG_INTERESTED = 2, MSG_NOTINTERESTED = 3,
       MSG_HAVE = 4, MSG_BITFIELD = 5, MSG_REQUEST = 6, MSG_PIECE = 7,
       MSG_CANCEL = 8 };

/* Receive-parser states (one framed message at a time). */
enum { S_LEN, S_ID, S_PHDR, S_PBODY, S_BITFIELD, S_HAVE, S_SKIP };

struct peer_conn {
	tcp_conn_t *c;
	int         choked;            /* peer has us choked (start: yes)          */
	int         have_known;        /* we've seen the peer's bitfield           */
	uint8_t    *have;              /* bitfield of pieces the peer advertises   */
	uint32_t    have_len;

	/* receive state machine */
	int      rxstate;
	uint8_t  hdr[8];               /* staging for len/piece-header/have        */
	uint32_t hdrgot;
	uint32_t msg_len;              /* current message length prefix            */
	uint32_t pay_left;             /* payload bytes still to consume           */
	uint32_t bf_off;               /* bytes of a bitfield payload seen         */
	uint32_t blk_begin, blk_left;  /* current piece block being streamed       */

	/* active piece job */
	int      active;
	uint32_t index, piece_size, received, next_begin, pending;
	uint8_t *out;
};

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint32_t get32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

/* Blocking read of exactly n bytes — only used during the handshake. */
static int read_full(tcp_conn_t *c, uint8_t *buf, uint32_t n)
{
	uint32_t got = 0;
	while (got < n) {
		int closed = 0;
		int r = tcp_read(c, buf + got, n - got, &closed);
		if (r < 0)
			return -1;
		if (r == 0)
			return closed ? -1 : 0;
		got += (uint32_t)r;
	}
	return 0;
}

int peer_has_piece(const peer_conn_t *pc, uint32_t index)
{
	if (!pc->have_known)
		return 1;                              /* optimistic until bitfield */
	uint32_t byte = index / 8;
	if (byte >= pc->have_len)
		return 0;
	return (pc->have[byte] >> (7 - (index & 7))) & 1;
}

static int send_request(tcp_conn_t *c, uint32_t index, uint32_t begin, uint32_t len)
{
	uint8_t m[17];
	put32(m, 13);
	m[4] = MSG_REQUEST;
	put32(m + 5, index);
	put32(m + 9, begin);
	put32(m + 13, len);
	return tcp_write(c, m, sizeof(m)) == (int)sizeof(m) ? 0 : -1;
}

/* Fill the request pipeline while unchoked. Returns -1 if a send fails. */
static int peer_kick(peer_conn_t *pc)
{
	if (!pc->active)
		return 0;
	while (!pc->choked && pc->pending < MAX_PENDING && pc->next_begin < pc->piece_size) {
		uint32_t blk = pc->piece_size - pc->next_begin;
		if (blk > BLOCK)
			blk = BLOCK;
		if (send_request(pc->c, pc->index, pc->next_begin, blk) != 0)
			return -1;
		pc->next_begin += blk;
		pc->pending++;
	}
	return 0;
}

peer_conn_t *peer_connect(const struct torrent *t, const uint8_t ip[4],
                          uint16_t port)
{
	tcp_conn_t *c = tcp_connect(ip, port);
	if (!c)
		return NULL;

	uint8_t hs[HS_LEN];
	hs[0] = PSTRLEN;
	for (int i = 0; i < PSTRLEN; i++)
		hs[1 + i] = (uint8_t)PSTR[i];
	for (int i = 0; i < 8; i++)
		hs[20 + i] = 0;
	kmemcpy(hs + 28, t->infohash, 20);
	kmemcpy(hs + 48, PEER_ID, 20);
	if (tcp_write(c, hs, HS_LEN) != HS_LEN) {
		tcp_close(c);
		return NULL;
	}

	uint8_t rhs[HS_LEN];
	if (read_full(c, rhs, HS_LEN) != 0 || rhs[0] != PSTRLEN) {
		tcp_close(c);
		return NULL;
	}
	for (int i = 0; i < 20; i++)
		if (rhs[28 + i] != t->infohash[i]) {
			tcp_close(c);
			return NULL;
		}

	peer_conn_t *pc = kmalloc(sizeof *pc);
	uint32_t hl = (t->n_pieces + 7) / 8;
	uint8_t *have = kmalloc(hl ? hl : 1);
	if (!pc || !have) {
		kfree(pc); kfree(have);
		tcp_close(c);
		return NULL;
	}
	for (uint32_t i = 0; i < hl; i++)
		have[i] = 0;
	pc->c = c;
	pc->choked = 1;
	pc->have_known = 0;
	pc->have = have;
	pc->have_len = hl;
	pc->rxstate = S_LEN;
	pc->hdrgot = 0;
	pc->active = 0;

	uint8_t interested[5];
	put32(interested, 1);
	interested[4] = MSG_INTERESTED;
	tcp_write(c, interested, sizeof(interested));
	return pc;
}

void peer_disconnect(peer_conn_t *pc)
{
	if (!pc)
		return;
	tcp_close(pc->c);
	kfree(pc->have);
	kfree(pc);
}

int peer_busy(const peer_conn_t *pc)        { return pc->active; }
uint32_t peer_piece(const peer_conn_t *pc)  { return pc->index; }

int peer_start_piece(peer_conn_t *pc, const struct torrent *t,
                     uint32_t index, uint8_t *out)
{
	if (index >= t->n_pieces)
		return -1;
	if (pc->have_known && !peer_has_piece(pc, index))
		return -1;

	uint64_t off = (uint64_t)index * (uint64_t)t->piece_length;
	pc->piece_size = (index == t->n_pieces - 1)
	               ? (uint32_t)((uint64_t)t->total_length - off)
	               : (uint32_t)t->piece_length;
	pc->index = index;
	pc->out = out;
	pc->received = 0;
	pc->next_begin = 0;
	pc->pending = 0;
	pc->active = 1;
	peer_kick(pc);                                 /* request now if unchoked */
	return 0;
}

/* Advance the parser over a span of freshly-received bytes. */
static void feed(peer_conn_t *pc, const uint8_t *p, uint32_t n)
{
	while (n > 0) {
		switch (pc->rxstate) {
		case S_LEN: {
			uint32_t need = 4 - pc->hdrgot;
			uint32_t take = n < need ? n : need;
			for (uint32_t k = 0; k < take; k++)
				pc->hdr[pc->hdrgot + k] = p[k];
			pc->hdrgot += take; p += take; n -= take;
			if (pc->hdrgot == 4) {
				pc->msg_len = get32(pc->hdr);
				pc->hdrgot = 0;
				if (pc->msg_len != 0)
					pc->rxstate = S_ID;   /* else keep-alive: stay */
			}
			break;
		}
		case S_ID: {
			uint8_t id = *p++; n--;
			pc->pay_left = pc->msg_len - 1;
			if (id == MSG_PIECE) {
				pc->rxstate = S_PHDR; pc->hdrgot = 0;
			} else if (id == MSG_BITFIELD) {
				pc->rxstate = S_BITFIELD; pc->bf_off = 0;
				if (pc->pay_left == 0) {
					pc->have_known = 1; pc->hdrgot = 0;
					pc->rxstate = S_LEN;
				}
			} else if (id == MSG_HAVE) {
				pc->rxstate = S_HAVE; pc->hdrgot = 0;
			} else {
				if (id == MSG_UNCHOKE) pc->choked = 0;
				else if (id == MSG_CHOKE) pc->choked = 1;
				if (pc->pay_left) {
					pc->rxstate = S_SKIP;
				} else {
					pc->hdrgot = 0; pc->rxstate = S_LEN;
				}
			}
			break;
		}
		case S_PHDR: {
			uint32_t need = 8 - pc->hdrgot;
			uint32_t take = n < need ? n : need;
			for (uint32_t k = 0; k < take; k++)
				pc->hdr[pc->hdrgot + k] = p[k];
			pc->hdrgot += take; p += take; n -= take; pc->pay_left -= take;
			if (pc->hdrgot == 8) {
				uint32_t pidx  = get32(pc->hdr);
				uint32_t begin = get32(pc->hdr + 4);
				pc->blk_left = pc->pay_left;
				pc->hdrgot = 0;
				if (pc->active && pidx == pc->index &&
				    begin + pc->blk_left <= pc->piece_size) {
					pc->blk_begin = begin;
					pc->rxstate = pc->blk_left ? S_PBODY : S_LEN;
				} else {
					pc->rxstate = pc->pay_left ? S_SKIP : S_LEN;
				}
			}
			break;
		}
		case S_PBODY: {
			uint32_t take = n < pc->blk_left ? n : pc->blk_left;
			for (uint32_t k = 0; k < take; k++)
				pc->out[pc->blk_begin + k] = p[k];
			pc->blk_begin += take; pc->blk_left -= take;
			pc->pay_left  -= take; pc->received += take;
			p += take; n -= take;
			if (pc->blk_left == 0) {
				if (pc->pending) pc->pending--;
				pc->hdrgot = 0;
				pc->rxstate = S_LEN;
			}
			break;
		}
		case S_BITFIELD: {
			uint32_t take = n < pc->pay_left ? n : pc->pay_left;
			for (uint32_t k = 0; k < take; k++) {
				uint32_t off = pc->bf_off + k;
				if (off < pc->have_len)
					pc->have[off] = p[k];
			}
			pc->bf_off += take; pc->pay_left -= take; p += take; n -= take;
			if (pc->pay_left == 0) {
				pc->have_known = 1;
				pc->hdrgot = 0;
				pc->rxstate = S_LEN;
			}
			break;
		}
		case S_HAVE: {
			uint32_t need = 4 - pc->hdrgot;
			uint32_t take = n < need ? n : need;
			for (uint32_t k = 0; k < take; k++)
				pc->hdr[pc->hdrgot + k] = p[k];
			pc->hdrgot += take; p += take; n -= take; pc->pay_left -= take;
			if (pc->hdrgot == 4) {
				uint32_t h = get32(pc->hdr);
				if (h / 8 < pc->have_len)
					pc->have[h / 8] |= (uint8_t)(0x80 >> (h & 7));
				pc->hdrgot = 0;
				pc->rxstate = S_LEN;
			}
			break;
		}
		case S_SKIP: {
			uint32_t take = n < pc->pay_left ? n : pc->pay_left;
			pc->pay_left -= take; p += take; n -= take;
			if (pc->pay_left == 0) {
				pc->hdrgot = 0;
				pc->rxstate = S_LEN;
			}
			break;
		}
		}
	}
}

int peer_pump(peer_conn_t *pc, const struct torrent *t)
{
	uint8_t scratch[2048];
	int closed = 0;
	for (;;) {
		int r = tcp_read_nb(pc->c, scratch, sizeof scratch, &closed);
		if (r < 0)
			return PEER_DEAD;
		if (r == 0)
			break;                                 /* nothing more right now */
		feed(pc, scratch, (uint32_t)r);
	}

	if (peer_kick(pc) != 0)
		return PEER_DEAD;

	if (pc->active) {
		/* Learned from the bitfield that this peer lacks our piece. */
		if (pc->received == 0 && pc->have_known && !peer_has_piece(pc, pc->index)) {
			pc->active = 0;
			return PEER_LACKS;
		}
		if (pc->received == pc->piece_size) {
			uint8_t got[20];
			sha1(pc->out, pc->piece_size, got);
			pc->active = 0;
			for (int i = 0; i < 20; i++)
				if (got[i] != t->pieces[pc->index * 20 + i])
					return PEER_DEAD;      /* bad data: drop the peer  */
			return PEER_PIECE;
		}
	}

	if (closed && pc->active)                      /* FIN mid-piece            */
		return PEER_DEAD;
	return PEER_WORKING;
}

int peer_fetch_piece(const struct torrent *t, const uint8_t ip[4],
                     uint16_t port, uint32_t index, uint8_t *out)
{
	peer_conn_t *pc = peer_connect(t, ip, port);
	if (!pc)
		return -1;
	int rc = -1;
	if (peer_start_piece(pc, t, index, out) == 0) {
		uint32_t deadline = timer_ticks() + 3000;  /* ~30 s                  */
		for (;;) {
			int r = peer_pump(pc, t);
			if (r == PEER_PIECE) { rc = (int)pc->piece_size; break; }
			if (r == PEER_DEAD)  { rc = -1; break; }
			if (r == PEER_LACKS) { rc = 0; break; }
			if (timer_ticks() > deadline) { rc = -1; break; }
			__asm__ volatile ("hlt");          /* wake on next IRQ        */
		}
	}
	peer_disconnect(pc);
	return rc;
}
