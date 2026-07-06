/* ogg.c — the Ogg physical bitstream: CRC, a push demuxer that reassembles
 * logical-stream packets from pages, and a mux that packs packets into pages.
 * Payload-agnostic (RFC 3533); the Vorbis codec lives in vorbis.c / vorbis_enc.c.
 */
#include "ogg.h"

#ifdef OGG_HOST_TEST
#include <stdlib.h>
#include <string.h>
#else
#include "owos.h"
#endif

#ifndef NULL
#define NULL ((void *)0)
#endif

/* ---- Ogg CRC: polynomial 0x04c11db7, MSB-first, init 0, no final xor ------ */
static uint32_t crc_tab[256];
static int      crc_ready;

static void crc_init(void)
{
	for (int i = 0; i < 256; i++) {
		uint32_t r = (uint32_t)i << 24;
		for (int j = 0; j < 8; j++)
			r = (r & 0x80000000u) ? (r << 1) ^ 0x04c11db7u : (r << 1);
		crc_tab[i] = r;
	}
	crc_ready = 1;
}

uint32_t ogg_crc(const unsigned char *p, unsigned len)
{
	uint32_t c = 0;
	if (!crc_ready) crc_init();
	for (unsigned i = 0; i < len; i++)
		c = (c << 8) ^ crc_tab[((c >> 24) ^ p[i]) & 0xff];
	return c;
}

/* ---- little-endian scalar helpers ---------------------------------------- */
static uint32_t rd_u32(const unsigned char *p)
{ return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static int64_t rd_i64(const unsigned char *p)
{ return (int64_t)((uint64_t)rd_u32(p) | ((uint64_t)rd_u32(p + 4) << 32)); }
static void wr_u32(unsigned char *p, uint32_t v)
{ p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void wr_i64(unsigned char *p, int64_t v)
{ wr_u32(p, (uint32_t)v); wr_u32(p + 4, (uint32_t)((uint64_t)v >> 32)); }

/* ============================== demuxer ================================= */

/* Per-logical-stream packet assembly. */
typedef struct {
	uint32_t       serial;
	int            used;
	unsigned char *acc;        /* bytes of the in-progress packet             */
	unsigned       acc_len, acc_cap;
	int            started;    /* have seen this stream's BOS                 */
	int            saw_first;  /* emitted the BOS packet already              */
} demux_stream;

/* A reassembled packet awaiting the caller. */
typedef struct ogg_pkt {
	struct ogg_pkt *next;
	unsigned char  *data;
	unsigned        len;
	uint32_t        serial;
	int64_t         gpos;
	int             bos, eos;
} ogg_pkt;

struct ogg_demux {
	unsigned char *buf;        /* input ring (compacted, not circular)        */
	unsigned       head, len, cap;
	demux_stream  *streams;
	unsigned       nstreams, cap_streams;
	ogg_pkt       *q_head, *q_tail;   /* ready-packet FIFO                    */
	ogg_pkt       *last;              /* last returned (freed on next call)   */
};

ogg_demux *ogg_demux_new(void)
{
	ogg_demux *d = (ogg_demux *)malloc(sizeof *d);
	if (!d) return NULL;
	memset(d, 0, sizeof *d);
	return d;
}

static void demux_reset_streams(ogg_demux *d)
{
	for (unsigned i = 0; i < d->nstreams; i++) free(d->streams[i].acc);
	free(d->streams);
	d->streams = NULL; d->nstreams = d->cap_streams = 0;
}

void ogg_demux_free(ogg_demux *d)
{
	ogg_pkt *p;
	if (!d) return;
	for (p = d->q_head; p; ) { ogg_pkt *n = p->next; free(p->data); free(p); p = n; }
	if (d->last) { free(d->last->data); free(d->last); }
	demux_reset_streams(d);
	free(d->buf);
	free(d);
}

int ogg_demux_feed(ogg_demux *d, const void *data, unsigned len)
{
	if (!d) return OGG_EPARAM;
	if (!len) return OGG_OK;
	/* Compact consumed prefix before growing. */
	if (d->head > 0 && d->head == d->len) { d->head = d->len = 0; }
	else if (d->head > 65536) {
		memmove(d->buf, d->buf + d->head, d->len - d->head);
		d->len -= d->head; d->head = 0;
	}
	if (d->len + len > d->cap) {
		unsigned nc = d->cap ? d->cap : 8192;
		while (d->len + len > nc) nc *= 2;
		unsigned char *nb = (unsigned char *)realloc(d->buf, nc);
		if (!nb) return OGG_ENOMEM;
		d->buf = nb; d->cap = nc;
	}
	memcpy(d->buf + d->len, data, len);
	d->len += len;
	return OGG_OK;
}

static demux_stream *demux_stream_for(ogg_demux *d, uint32_t serial)
{
	for (unsigned i = 0; i < d->nstreams; i++)
		if (d->streams[i].used && d->streams[i].serial == serial)
			return &d->streams[i];
	for (unsigned i = 0; i < d->nstreams; i++)
		if (!d->streams[i].used) {
			demux_stream *s = &d->streams[i];
			memset(s, 0, sizeof *s); s->serial = serial; s->used = 1;
			return s;
		}
	if (d->nstreams == d->cap_streams) {
		unsigned nc = d->cap_streams ? d->cap_streams * 2 : 4;
		demux_stream *ns = (demux_stream *)realloc(d->streams, nc * sizeof *ns);
		if (!ns) return NULL;
		memset(ns + d->cap_streams, 0, (nc - d->cap_streams) * sizeof *ns);
		d->streams = ns; d->cap_streams = nc;
	}
	demux_stream *s = &d->streams[d->nstreams++];
	memset(s, 0, sizeof *s); s->serial = serial; s->used = 1;
	return s;
}

static int acc_append(demux_stream *s, const unsigned char *p, unsigned n)
{
	if (s->acc_len + n > s->acc_cap) {
		unsigned nc = s->acc_cap ? s->acc_cap : 4096;
		while (s->acc_len + n > nc) nc *= 2;
		unsigned char *nb = (unsigned char *)realloc(s->acc, nc);
		if (!nb) return OGG_ENOMEM;
		s->acc = nb; s->acc_cap = nc;
	}
	memcpy(s->acc + s->acc_len, p, n);
	s->acc_len += n;
	return OGG_OK;
}

static int demux_enqueue(ogg_demux *d, demux_stream *s, int64_t gpos, int bos, int eos)
{
	ogg_pkt *pk = (ogg_pkt *)malloc(sizeof *pk);
	if (!pk) return OGG_ENOMEM;
	pk->data = (unsigned char *)malloc(s->acc_len ? s->acc_len : 1);
	if (!pk->data) { free(pk); return OGG_ENOMEM; }
	memcpy(pk->data, s->acc, s->acc_len);
	pk->len = s->acc_len; pk->serial = s->serial;
	pk->gpos = gpos; pk->bos = bos; pk->eos = eos; pk->next = NULL;
	if (d->q_tail) d->q_tail->next = pk; else d->q_head = pk;
	d->q_tail = pk;
	s->acc_len = 0;
	return OGG_OK;
}

/* Parse every complete page currently in the buffer, enqueuing completed
 * packets. Returns OGG_OK (possibly enqueuing nothing) or OGG_ENOMEM. */
static int demux_parse(ogg_demux *d)
{
	while (1) {
		unsigned avail = d->len - d->head;
		unsigned char *b = d->buf + d->head;
		/* Resync to the "OggS" capture pattern. */
		if (avail < 27) return OGG_OK;
		if (!(b[0]=='O' && b[1]=='g' && b[2]=='g' && b[3]=='S')) {
			unsigned i = 1;
			while (i + 4 <= avail &&
			       !(b[i]=='O' && b[i+1]=='g' && b[i+2]=='g' && b[i+3]=='S'))
				i++;
			if (i + 4 > avail) { d->head = d->len - 3 > d->head ? d->len - 3 : d->head; return OGG_OK; }
			d->head += i;
			continue;
		}
		if (b[4] != 0) { d->head += 1; continue; }      /* bad version: resync */
		unsigned nseg = b[26];
		if (avail < 27u + nseg) return OGG_OK;
		unsigned body = 0;
		for (unsigned i = 0; i < nseg; i++) body += b[27 + i];
		unsigned pagelen = 27 + nseg + body;
		if (avail < pagelen) return OGG_OK;

		/* Verify CRC (field zeroed during the pass). */
		uint32_t stored = rd_u32(b + 22);
		unsigned char sv[4] = { b[22], b[23], b[24], b[25] };
		b[22] = b[23] = b[24] = b[25] = 0;
		uint32_t got = ogg_crc(b, pagelen);
		b[22] = sv[0]; b[23] = sv[1]; b[24] = sv[2]; b[25] = sv[3];
		if (got != stored) { d->head += 1; continue; }  /* corrupt: resync     */

		unsigned char htype = b[5];
		int64_t gpos = rd_i64(b + 6);
		uint32_t serial = rd_u32(b + 14);
		demux_stream *s = demux_stream_for(d, serial);
		if (!s) return OGG_ENOMEM;

		const unsigned char *seg = b + 27;
		const unsigned char *bodyp = b + 27 + nseg;
		unsigned off = 0;

		if (htype & OGG_F_BOS) { s->started = 1; s->saw_first = 0; }
		if (htype & OGG_F_CONTINUED) {
			/* first packet continues the accumulator (keep acc as-is) */
		} else {
			s->acc_len = 0;   /* drop any dangling partial packet */
		}

		/* Find the index of the last segment that terminates a packet, so the
		 * page granulepos attaches to that packet only. */
		int last_complete = -1;
		for (int i = 0; i < (int)nseg; i++) if (seg[i] < 255) last_complete = i;

		for (unsigned i = 0; i < nseg; i++) {
			unsigned L = seg[i];
			if (acc_append(s, bodyp + off, L) != OGG_OK) return OGG_ENOMEM;
			off += L;
			if (L < 255) {
				/* packet complete */
				int64_t g = ((int)i == last_complete) ? gpos : (int64_t)-1;
				int bos = 0, eos = 0;
				if ((htype & OGG_F_BOS) && !s->saw_first) { bos = 1; s->saw_first = 1; }
				if ((htype & OGG_F_EOS) && (int)i == last_complete) eos = 1;
				if (demux_enqueue(d, s, g, bos, eos) != OGG_OK) return OGG_ENOMEM;
			}
		}
		d->head += pagelen;
	}
}

int ogg_demux_packet(ogg_demux *d, const unsigned char **pkt, unsigned *len,
                     int64_t *granulepos, uint32_t *serial, int *bos, int *eos)
{
	if (!d) return OGG_EPARAM;
	if (d->last) { free(d->last->data); free(d->last); d->last = NULL; }
	if (!d->q_head) {
		int rc = demux_parse(d);
		if (rc != OGG_OK) return rc;
	}
	if (!d->q_head) return OGG_NEED;
	ogg_pkt *pk = d->q_head;
	d->q_head = pk->next;
	if (!d->q_head) d->q_tail = NULL;
	if (pkt) *pkt = pk->data;
	if (len) *len = pk->len;
	if (granulepos) *granulepos = pk->gpos;
	if (serial) *serial = pk->serial;
	if (bos) *bos = pk->bos;
	if (eos) *eos = pk->eos;
	d->last = pk;
	return OGG_OK;
}

/* ================================ mux =================================== */

typedef struct mux_page {
	struct mux_page *next;
	unsigned char   *data;
	unsigned         len;
} mux_page;

struct ogg_mux {
	uint32_t      serial;
	uint32_t      seqno;
	unsigned char seg[255];
	unsigned      nseg;
	unsigned char *body;
	unsigned       body_len, body_cap;
	int64_t        page_gpos;   /* gp of the last packet completing on this page */
	int            continued;   /* current page began mid-packet                */
	int            have_bos;    /* the pending page is the stream's first        */
	int            pending_eos; /* the completing packet closes the stream       */
	mux_page      *out_head, *out_tail, *last_out;
};

ogg_mux *ogg_mux_new(uint32_t serial)
{
	ogg_mux *m = (ogg_mux *)malloc(sizeof *m);
	if (!m) return NULL;
	memset(m, 0, sizeof *m);
	m->serial = serial;
	m->page_gpos = -1;
	return m;
}

void ogg_mux_free(ogg_mux *m)
{
	mux_page *p;
	if (!m) return;
	for (p = m->out_head; p; ) { mux_page *n = p->next; free(p->data); free(p); p = n; }
	if (m->last_out) { free(m->last_out->data); free(m->last_out); }
	free(m->body);
	free(m);
}

static int body_reserve(ogg_mux *m, unsigned n)
{
	if (m->body_len + n > m->body_cap) {
		unsigned nc = m->body_cap ? m->body_cap : 8192;
		while (m->body_len + n > nc) nc *= 2;
		unsigned char *nb = (unsigned char *)realloc(m->body, nc);
		if (!nb) return OGG_ENOMEM;
		m->body = nb; m->body_cap = nc;
	}
	return OGG_OK;
}

/* Assemble the current page into a byte blob and queue it; reset page state. */
static int mux_emit(ogg_mux *m, int eos)
{
	if (m->nseg == 0 && m->body_len == 0 && !eos) return OGG_OK;
	unsigned hdr = 27 + m->nseg;
	unsigned char *pg = (unsigned char *)malloc(hdr + m->body_len);
	if (!pg) return OGG_ENOMEM;
	pg[0] = 'O'; pg[1] = 'g'; pg[2] = 'g'; pg[3] = 'S'; pg[4] = 0;
	pg[5] = (m->continued ? OGG_F_CONTINUED : 0) |
	        (m->have_bos ? OGG_F_BOS : 0) | (eos ? OGG_F_EOS : 0);
	wr_i64(pg + 6, m->page_gpos);
	wr_u32(pg + 14, m->serial);
	wr_u32(pg + 18, m->seqno++);
	pg[22] = pg[23] = pg[24] = pg[25] = 0;
	pg[26] = (unsigned char)m->nseg;
	memcpy(pg + 27, m->seg, m->nseg);
	memcpy(pg + hdr, m->body, m->body_len);
	wr_u32(pg + 22, ogg_crc(pg, hdr + m->body_len));

	mux_page *mp = (mux_page *)malloc(sizeof *mp);
	if (!mp) { free(pg); return OGG_ENOMEM; }
	mp->data = pg; mp->len = hdr + m->body_len; mp->next = NULL;
	if (m->out_tail) m->out_tail->next = mp; else m->out_head = mp;
	m->out_tail = mp;

	/* reset for the next page */
	m->nseg = 0; m->body_len = 0; m->page_gpos = -1;
	m->have_bos = 0; m->pending_eos = 0;
	m->continued = 0;   /* set true below if a packet is still open */
	return OGG_OK;
}

int ogg_mux_packet(ogg_mux *m, const void *pkt, unsigned len,
                   int64_t granulepos, int bos, int eos)
{
	const unsigned char *p = (const unsigned char *)pkt;
	unsigned remaining = len;
	if (!m) return OGG_EPARAM;
	if (bos && m->seqno == 0 && m->nseg == 0) m->have_bos = 1;

	/* Emit the lacing values for this packet, spilling to a new page whenever
	 * the segment table fills (a packet longer than 255*255 bytes, or a page
	 * that was already partly full). */
	while (1) {
		if (m->nseg == 255) {
			if (mux_emit(m, 0) != OGG_OK) return OGG_ENOMEM;
			m->continued = 1;   /* the new page starts inside this packet */
		}
		unsigned L = remaining >= 255 ? 255 : remaining;
		if (body_reserve(m, L) != OGG_OK) return OGG_ENOMEM;
		memcpy(m->body + m->body_len, p, L);
		m->body_len += L; p += L; remaining -= L;
		m->seg[m->nseg++] = (unsigned char)L;
		if (L < 255) break;     /* terminating lacing written */
		if (remaining == 0) {
			/* length is a multiple of 255: write the 0 terminator */
			if (m->nseg == 255) {
				if (mux_emit(m, 0) != OGG_OK) return OGG_ENOMEM;
				m->continued = 1;
			}
			m->seg[m->nseg++] = 0;
			break;
		}
	}

	/* Packet complete: this page's granulepos becomes the packet's. */
	if (granulepos >= 0) m->page_gpos = granulepos;
	if (eos) m->pending_eos = 1;

	/* Emit if the stream ends, or the page has grown enough. */
	if (eos) return mux_emit(m, 1);
	if (m->body_len >= 8192 || m->nseg >= 255)
		return mux_emit(m, 0);
	return OGG_OK;
}

int ogg_mux_flush(ogg_mux *m)
{
	if (!m) return OGG_EPARAM;
	return mux_emit(m, m->pending_eos);
}

int ogg_mux_pull(ogg_mux *m, const unsigned char **page, unsigned *len)
{
	if (!m) return OGG_EPARAM;
	if (m->last_out) { free(m->last_out->data); free(m->last_out); m->last_out = NULL; }
	if (!m->out_head) return OGG_NEED;
	mux_page *mp = m->out_head;
	m->out_head = mp->next;
	if (!m->out_head) m->out_tail = NULL;
	if (page) *page = mp->data;
	if (len) *len = mp->len;
	m->last_out = mp;
	return OGG_OK;
}
