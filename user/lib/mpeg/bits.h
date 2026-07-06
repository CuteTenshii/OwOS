/* bits.h — a big-endian (MSB-first) bit reader over an in-memory buffer, plus a
 * matching writer, shared by the MPEG-1 decoder and encoder. MPEG bitstreams are
 * MSB-first and have no emulation-prevention escaping (unlike H.264): 23 zero
 * bits only occur at a start code, which the reader detects by peeking. */
#ifndef OWOS_MPEG_BITS_H
#define OWOS_MPEG_BITS_H

/* ---- reader --------------------------------------------------------------- */
typedef struct {
	const unsigned char *buf;
	unsigned             size;   /* total bytes                                */
	unsigned             pos;    /* current byte                               */
	unsigned             bit;    /* bits consumed in buf[pos] (0..7, MSB first) */
} bitr;

static inline void br_init(bitr *b, const unsigned char *buf, unsigned size)
{ b->buf = buf; b->size = size; b->pos = 0; b->bit = 0; }

static inline unsigned br_get1(bitr *b)
{
	unsigned v;
	if (b->pos >= b->size) { b->pos++; return 0; }   /* past EOF reads as 0 */
	v = (b->buf[b->pos] >> (7 - b->bit)) & 1u;
	if (++b->bit == 8) { b->bit = 0; b->pos++; }
	return v;
}

static inline unsigned br_get(bitr *b, int n)
{ unsigned v = 0; while (n-- > 0) v = (v << 1) | br_get1(b); return v; }

/* Read n bits as a two's-complement signed value. */
static inline int br_gets(bitr *b, int n)
{
	unsigned v = br_get(b, n);
	if (n > 0 && (v & (1u << (n - 1)))) return (int)v - (1 << n);
	return (int)v;
}

static inline unsigned br_show(bitr *b, int n)
{ bitr t = *b; return br_get(&t, n); }

static inline void br_skip(bitr *b, int n) { while (n-- > 0) br_get1(b); }

static inline void br_align(bitr *b)
{ if (b->bit) { b->bit = 0; b->pos++; } }

static inline int br_bits_left(const bitr *b)
{ return (int)(b->size - b->pos) * 8 - (int)b->bit; }

/* At a byte-aligned or unaligned point, is the next thing a start code (>=23
 * leading zero bits then a 1)? Used to detect the end of a slice's macroblocks. */
static inline int br_at_start_code(bitr *b)
{ return br_bits_left(b) < 23 || br_show(b, 23) == 0; }

/* Byte-align, then scan forward to the next 00 00 01 xx start code. On success
 * leaves pos at the start-code id byte (read it with br_get(b,8)) and returns the
 * id (0x00..0xFF); returns -1 at end of stream. */
static inline int br_next_start(bitr *b)
{
	br_align(b);
	while (b->pos + 3 < b->size) {
		if (b->buf[b->pos] == 0 && b->buf[b->pos + 1] == 0 && b->buf[b->pos + 2] == 1) {
			b->pos += 3; b->bit = 0;
			return b->buf[b->pos];
		}
		b->pos++;
	}
	return -1;
}

/* ---- writer --------------------------------------------------------------- */
/* A growable MSB-first bit writer over a malloc'd buffer. */
typedef struct {
	unsigned char *buf;
	unsigned       cap;      /* bytes allocated                                */
	unsigned       len;      /* whole bytes emitted                            */
	unsigned       acc;      /* bit accumulator, bits packed toward the LSBs   */
	int            nbits;    /* pending bits in acc (0..7)                      */
	int            oom;      /* a grow failed                                  */
} bitw;

static inline int bw_init(bitw *w)
{
	w->cap = 4096; w->len = 0; w->acc = 0; w->nbits = 0; w->oom = 0;
	w->buf = (unsigned char *)malloc(w->cap);
	return w->buf ? 0 : -1;
}

static inline void bw_byte(bitw *w, unsigned char b)
{
	if (w->len >= w->cap) {
		unsigned nc = w->cap * 2;
		unsigned char *nb = (unsigned char *)realloc(w->buf, nc);
		if (!nb) { w->oom = 1; return; }
		w->buf = nb; w->cap = nc;
	}
	w->buf[w->len++] = b;
}

/* Write the low `n` bits of `val`, MSB first. */
static inline void bw_put(bitw *w, unsigned val, int n)
{
	int i;
	for (i = n - 1; i >= 0; i--) {
		w->acc = (w->acc << 1) | ((val >> i) & 1u);
		if (++w->nbits == 8) { bw_byte(w, (unsigned char)w->acc); w->acc = 0; w->nbits = 0; }
	}
}

/* Pad with zero bits up to the next byte boundary. */
static inline void bw_align(bitw *w)
{ if (w->nbits) { w->acc <<= (8 - w->nbits); bw_byte(w, (unsigned char)w->acc); w->acc = 0; w->nbits = 0; } }

#endif /* OWOS_MPEG_BITS_H */
