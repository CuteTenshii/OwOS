/* gzip.c — a freestanding gzip codec: DEFLATE (RFC 1951) under gzip framing
 * (RFC 1952), interoperable with the standard gzip/gunzip tools.
 *
 * Decoder (inflate): a complete DEFLATE reader handling all three block types
 *   — stored, fixed-Huffman, and dynamic-Huffman — so it decompresses anything
 *   real gzip or a web server emits. The canonical Huffman decode follows the
 *   approach in Mark Adler's puff.c.
 *
 * Encoder (deflate): an LZ77 pass over the input (3..258-byte matches in a
 *   32 KiB window, found with hash chains) feeds a single block emitted as the
 *   cheapest of dynamic-Huffman (per-data optimal, length-limited code lengths
 *   built with zlib's algorithm), fixed-Huffman, or stored. Two LZ77 passes
 *   (count frequencies, then emit) avoid buffering the token list. Output is
 *   standard gzip that any tool can inflate; the stored fallback caps expansion
 *   on incompressible data.
 *
 * The bit order is DEFLATE's: data bits are packed LSB-first within each byte,
 * while Huffman codes travel MSB-first (so the encoder reverses each code and
 * the decoder reassembles MSB-first).
 */

#include <stddef.h>
#include <stdint.h>
#include "gzip.h"
#include "endian.h"
#include "heap.h"

#define MAXBITS    15           /* longest Huffman code                        */
#define MAXLCODES  286          /* max literal/length codes                    */
#define MAXDCODES  30           /* max distance codes                          */
#define FIXLCODES  288          /* fixed literal/length codes                  */
#define HEAP_SIZE  (2 * MAXLCODES + 1)  /* Huffman build heap/tree storage     */

#define WINDOW     32768        /* DEFLATE's 32 KiB back-reference window       */
#define MIN_MATCH  3
#define MAX_MATCH  258
#define HASH_BITS  15
#define HASH_SIZE  (1u << HASH_BITS)
#define MAX_CHAIN  256          /* bound the match search per position          */

/* Length symbol (257..285) base value and extra-bit count, indexed by sym-257. */
static const uint16_t lbase[29] = {
	3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
	67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t lext[29] = {
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
	4, 4, 4, 4, 5, 5, 5, 5, 0
};
/* Distance symbol (0..29) base value and extra-bit count. */
static const uint16_t dbase[30] = {
	1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
	513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const uint8_t dext[30] = {
	0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
	9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static void put_u32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}


/* ---- CRC32 (gzip's polynomial, lazily-built table) --------------------- */

static uint32_t crc_table[256];
static int crc_ready;

static void crc_build(void)
{
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;
		for (int k = 0; k < 8; k++)
			c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
		crc_table[i] = c;
	}
	crc_ready = 1;
}

static uint32_t crc32(const uint8_t *data, uint32_t n)
{
	if (!crc_ready)
		crc_build();
	uint32_t c = 0xFFFFFFFFu;
	for (uint32_t i = 0; i < n; i++)
		c = crc_table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
	return c ^ 0xFFFFFFFFu;
}

/* ---- inflate (DEFLATE decoder) ----------------------------------------- */

struct inbits {
	const uint8_t *src;
	uint32_t       len;        /* one past the last readable byte             */
	uint32_t       pos;
	uint32_t       bitbuf;
	int            bitcnt;
	int            err;
};

static int get_bit(struct inbits *s)
{
	if (s->bitcnt == 0) {
		if (s->pos >= s->len) {
			s->err = 1;
			return 0;
		}
		s->bitbuf = s->src[s->pos++];
		s->bitcnt = 8;
	}
	int b = s->bitbuf & 1;
	s->bitbuf >>= 1;
	s->bitcnt--;
	return b;
}

/* Read n data bits, LSB-first (used for lengths, distances, and headers). */
static uint32_t get_bits(struct inbits *s, int n)
{
	uint32_t v = 0;
	for (int i = 0; i < n; i++)
		v |= (uint32_t)get_bit(s) << i;
	return v;
}

struct huff {
	short count[MAXBITS + 1];  /* number of codes of each length              */
	short symbol[MAXLCODES];   /* symbols sorted by (length, value)           */
};

/* Build a canonical Huffman decoding table from per-symbol code lengths. */
static int huff_build(struct huff *h, const short *length, int n)
{
	short offs[MAXBITS + 1];
	for (int len = 0; len <= MAXBITS; len++)
		h->count[len] = 0;
	for (int sym = 0; sym < n; sym++)
		h->count[length[sym]]++;
	if (h->count[0] == n)              /* no codes at all                     */
		return 0;
	offs[1] = 0;
	for (int len = 1; len < MAXBITS; len++)
		offs[len + 1] = offs[len] + h->count[len];
	for (int sym = 0; sym < n; sym++)
		if (length[sym] != 0)
			h->symbol[offs[length[sym]]++] = (short)sym;
	return 0;
}

/* Decode one symbol: read bits MSB-first, reassembling the code. */
static int huff_decode(struct inbits *s, const struct huff *h)
{
	int code = 0, first = 0, index = 0;
	for (int len = 1; len <= MAXBITS; len++) {
		code |= get_bit(s);
		if (s->err)
			return -1;
		int cnt = h->count[len];
		if (code - first < cnt)
			return h->symbol[index + (code - first)];
		index += cnt;
		first += cnt;
		first <<= 1;
		code <<= 1;
	}
	return -1;
}

/* Decode the literal/length + distance stream of one block. */
static int inflate_block(struct inbits *s, const struct huff *lit,
			 const struct huff *dist, uint8_t *dst,
			 uint32_t dcap, uint32_t *outp)
{
	uint32_t out = *outp;
	for (;;) {
		int sym = huff_decode(s, lit);
		if (sym < 0)
			return -1;
		if (sym == 256)
			break;                 /* end of block                    */
		if (sym < 256) {
			if (out >= dcap)
				return -1;
			dst[out++] = (uint8_t)sym;
		} else {
			sym -= 257;
			if (sym >= 29)
				return -1;
			uint32_t len = lbase[sym] + get_bits(s, lext[sym]);
			int dsym = huff_decode(s, dist);
			if (dsym < 0 || dsym >= 30)
				return -1;
			uint32_t d = dbase[dsym] + get_bits(s, dext[dsym]);
			if (d > out || out + len > dcap)
				return -1;
			uint32_t from = out - d;
			for (uint32_t i = 0; i < len; i++)
				dst[out + i] = dst[from + i];
			out += len;
		}
		if (s->err)
			return -1;
	}
	*outp = out;
	return 0;
}

static int inflate_stored(struct inbits *s, uint8_t *dst, uint32_t dcap,
			  uint32_t *outp)
{
	s->bitcnt = 0;                         /* align to a byte boundary        */
	if (s->pos + 4 > s->len)
		return -1;
	uint32_t len = s->src[s->pos] | ((uint32_t)s->src[s->pos + 1] << 8);
	s->pos += 4;                           /* skip LEN and its complement     */
	if (s->pos + len > s->len || *outp + len > dcap)
		return -1;
	for (uint32_t i = 0; i < len; i++)
		dst[*outp + i] = s->src[s->pos + i];
	s->pos += len;
	*outp += len;
	return 0;
}

/* Build the two fixed Huffman tables (same for every fixed block). */
static void fixed_tables(struct huff *lit, struct huff *dist)
{
	short lengths[FIXLCODES];
	int i = 0;
	for (; i < 144; i++) lengths[i] = 8;
	for (; i < 256; i++) lengths[i] = 9;
	for (; i < 280; i++) lengths[i] = 7;
	for (; i < 288; i++) lengths[i] = 8;
	huff_build(lit, lengths, FIXLCODES);
	for (i = 0; i < 30; i++) lengths[i] = 5;
	huff_build(dist, lengths, 30);
}

/* Read a dynamic block's two Huffman tables from the stream. */
static int dynamic_tables(struct inbits *s, struct huff *lit, struct huff *dist)
{
	static const short order[19] = {
		16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
	};
	int hlit = (int)get_bits(s, 5) + 257;
	int hdist = (int)get_bits(s, 5) + 1;
	int hclen = (int)get_bits(s, 4) + 4;
	if (hlit > MAXLCODES || hdist > MAXDCODES)
		return -1;

	short cl[19];
	for (int i = 0; i < 19; i++)
		cl[i] = 0;
	for (int i = 0; i < hclen; i++)
		cl[order[i]] = (short)get_bits(s, 3);
	struct huff clh;
	huff_build(&clh, cl, 19);

	short lengths[MAXLCODES + MAXDCODES];
	int idx = 0;
	while (idx < hlit + hdist) {
		int sym = huff_decode(s, &clh);
		if (sym < 0)
			return -1;
		if (sym < 16) {
			lengths[idx++] = (short)sym;
		} else {
			int rep;
			short val = 0;
			if (sym == 16) {
				if (idx == 0)
					return -1;
				val = lengths[idx - 1];
				rep = 3 + (int)get_bits(s, 2);
			} else if (sym == 17) {
				rep = 3 + (int)get_bits(s, 3);
			} else {
				rep = 11 + (int)get_bits(s, 7);
			}
			if (idx + rep > hlit + hdist)
				return -1;
			while (rep--)
				lengths[idx++] = val;
		}
		if (s->err)
			return -1;
	}
	huff_build(lit, lengths, hlit);
	huff_build(dist, lengths + hlit, hdist);
	return 0;
}

/* Inflate the DEFLATE stream in src[start..end) into dst. Returns length. */
static int inflate(const uint8_t *src, uint32_t start, uint32_t end,
		   uint8_t *dst, uint32_t dcap)
{
	struct inbits s = { src, end, start, 0, 0, 0 };
	uint32_t out = 0;
	int last;
	do {
		last = get_bit(&s);
		int type = (int)get_bits(&s, 2);
		if (s.err)
			return -1;
		if (type == 0) {
			if (inflate_stored(&s, dst, dcap, &out) < 0)
				return -1;
		} else if (type == 1 || type == 2) {
			struct huff *lit = kmalloc(sizeof(struct huff));
			struct huff *dist = kmalloc(sizeof(struct huff));
			if (!lit || !dist) {
				kfree(lit);
				kfree(dist);
				return -1;
			}
			int ok;
			if (type == 1) {
				fixed_tables(lit, dist);
				ok = 0;
			} else {
				ok = dynamic_tables(&s, lit, dist);
			}
			if (ok == 0)
				ok = inflate_block(&s, lit, dist, dst, dcap, &out);
			kfree(lit);
			kfree(dist);
			if (ok < 0)
				return -1;
		} else {
			return -1;             /* reserved block type             */
		}
	} while (!last && !s.err);
	return s.err ? -1 : (int)out;
}

/* ---- deflate (fixed-Huffman encoder) ----------------------------------- */

struct outbits {
	uint8_t *buf;
	uint32_t cap;
	uint32_t pos;
	uint32_t acc;
	int      nbits;
	int      err;
};

/* Append n data bits, LSB-first. */
static void put_bits(struct outbits *o, uint32_t val, int n)
{
	if (n == 0)
		return;
	o->acc |= (val & ((1u << n) - 1)) << o->nbits;
	o->nbits += n;
	while (o->nbits >= 8) {
		if (o->pos < o->cap)
			o->buf[o->pos++] = (uint8_t)(o->acc & 0xFF);
		else
			o->err = 1;
		o->acc >>= 8;
		o->nbits -= 8;
	}
}

/* Append a Huffman code of `len` bits, sent MSB-first (so reverse it). */
static void put_code(struct outbits *o, uint32_t code, int len)
{
	uint32_t r = 0;
	for (int i = 0; i < len; i++) {
		r = (r << 1) | (code & 1);
		code >>= 1;
	}
	put_bits(o, r, len);
}

static void flush_bits(struct outbits *o)
{
	if (o->nbits > 0) {
		if (o->pos < o->cap)
			o->buf[o->pos++] = (uint8_t)(o->acc & 0xFF);
		else
			o->err = 1;
		o->acc = 0;
		o->nbits = 0;
	}
}

/* Write one already-byte-aligned raw byte (used by stored blocks). */
static void put_raw(struct outbits *o, uint8_t b)
{
	if (o->pos < o->cap)
		o->buf[o->pos++] = b;
	else
		o->err = 1;
}

/* Map a match length (3..258) to its length symbol (257..285) and extra bits. */
static int len_code(int len, uint32_t *extra, int *ebits)
{
	int i = 28;
	while (len < lbase[i])
		i--;
	*extra = (uint32_t)len - lbase[i];
	*ebits = lext[i];
	return 257 + i;
}

/* Map a distance (1..32768) to its distance symbol (0..29) and extra bits. */
static int dist_code(uint32_t dist, uint32_t *extra, int *ebits)
{
	int i = 29;
	while (dist < dbase[i])
		i--;
	*extra = dist - dbase[i];
	*ebits = dext[i];
	return i;
}

/* Canonical Huffman codes from per-symbol code lengths (RFC 1951 3.2.2). */
static void build_canonical(const uint8_t *len, int n, uint16_t *code)
{
	uint16_t bl_count[MAXBITS + 1];
	uint16_t next[MAXBITS + 1];
	for (int b = 0; b <= MAXBITS; b++)
		bl_count[b] = 0;
	for (int i = 0; i < n; i++)
		bl_count[len[i]]++;
	bl_count[0] = 0;
	uint16_t c = 0;
	for (int b = 1; b <= MAXBITS; b++) {
		c = (uint16_t)((c + bl_count[b - 1]) << 1);
		next[b] = c;
	}
	for (int i = 0; i < n; i++)
		code[i] = len[i] ? next[len[i]]++ : 0;
}

static uint32_t hash3(const uint8_t *p)
{
	uint32_t v = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
	return (v * 2654435761u) >> (32 - HASH_BITS);
}

static void ht_insert(int *head, int *prev, const uint8_t *src,
		      uint32_t slen, uint32_t pos)
{
	if (pos + MIN_MATCH <= slen) {
		uint32_t h = hash3(src + pos);
		prev[pos & (WINDOW - 1)] = head[h];
		head[h] = (int)pos;
	}
}

/* Per-symbol Huffman code table (literal/length uses all, distance uses 0..29). */
struct codes {
	uint16_t code[MAXLCODES];
	uint8_t  len[MAXLCODES];
};

/* Heap ordering for Huffman tree building: lower frequency first, shallower
 * subtree as the tiebreaker (keeps codes short and the build deterministic). */
static int hsmaller(const uint32_t *fr, const uint8_t *depth, int n, int m)
{
	return fr[n] < fr[m] || (fr[n] == fr[m] && depth[n] <= depth[m]);
}

static void pqdown(const uint32_t *fr, const uint8_t *depth, int *heap,
		   int heap_len, int k)
{
	int v = heap[k];
	int j = k << 1;
	while (j <= heap_len) {
		if (j < heap_len && hsmaller(fr, depth, heap[j + 1], heap[j]))
			j++;
		if (hsmaller(fr, depth, v, heap[j]))
			break;
		heap[k] = heap[j];
		k = j;
		j <<= 1;
	}
	heap[k] = v;
}

/* Length-limited Huffman code lengths (zlib's build_tree + gen_bitlen overflow
 * redistribution): optimal-ish lengths for freq[0..elems-1], none longer than
 * maxlen, written to out_len[]. At least two codes are forced so the tree — and
 * the decoder reading it — is always well-formed. Single-threaded; the work
 * arrays are static to keep the kernel stack small. */
static void build_lengths(const uint32_t *freq, int elems, int maxlen,
			  uint8_t *out_len)
{
	static uint32_t fr[HEAP_SIZE];
	static int      dad[HEAP_SIZE];
	static int      ln[HEAP_SIZE];
	static int      heap[HEAP_SIZE + 1];
	static uint8_t  depth[HEAP_SIZE];
	short bl_count[MAXBITS + 1];
	int heap_len = 0, heap_max = HEAP_SIZE, max_code = -1;

	for (int n = 0; n < elems; n++) {
		fr[n] = freq[n];
		dad[n] = 0;
		ln[n] = 0;
		depth[n] = 0;
		out_len[n] = 0;
	}
	for (int n = 0; n < elems; n++)
		if (fr[n] != 0) {
			heap[++heap_len] = n;
			max_code = n;
		}
	while (heap_len < 2) {                  /* force two codes                 */
		int node = (max_code < 2) ? ++max_code : 0;
		fr[node] = 1;
		heap[++heap_len] = node;
	}

	for (int k = heap_len / 2; k >= 1; k--)
		pqdown(fr, depth, heap, heap_len, k);

	int node = elems;                       /* next internal-node index        */
	do {
		int n = heap[1];
		heap[1] = heap[heap_len--];
		pqdown(fr, depth, heap, heap_len, 1);
		int m = heap[1];
		heap[--heap_max] = n;           /* stash sorted, for gen_bitlen    */
		heap[--heap_max] = m;
		fr[node] = fr[n] + fr[m];
		depth[node] = (uint8_t)((depth[n] >= depth[m] ?
					 depth[n] : depth[m]) + 1);
		dad[n] = dad[m] = node;
		heap[1] = node++;
		pqdown(fr, depth, heap, heap_len, 1);
	} while (heap_len >= 2);
	heap[--heap_max] = heap[1];             /* the root                        */

	for (int b = 0; b <= MAXBITS; b++)
		bl_count[b] = 0;
	ln[heap[heap_max]] = 0;
	int overflow = 0;
	for (int h = heap_max + 1; h < HEAP_SIZE; h++) {
		int n = heap[h];
		int bits = ln[dad[n]] + 1;
		if (bits > maxlen) {
			bits = maxlen;
			overflow++;
		}
		ln[n] = bits;
		if (n <= max_code)              /* a leaf, not an internal node    */
			bl_count[bits]++;
	}
	if (overflow > 0) {
		do {                            /* push leaves down to fit maxlen  */
			int bits = maxlen - 1;
			while (bl_count[bits] == 0)
				bits--;
			bl_count[bits]--;
			bl_count[bits + 1] += 2;
			bl_count[maxlen]--;
			overflow -= 2;
		} while (overflow > 0);
		int h = HEAP_SIZE;              /* reassign by descending frequency */
		for (int bits = maxlen; bits != 0; bits--) {
			int n = bl_count[bits];
			while (n != 0) {
				int m = heap[--h];
				if (m > max_code)
					continue;
				ln[m] = bits;
				n--;
			}
		}
	}
	for (int n = 0; n < elems; n++)
		out_len[n] = (uint8_t)ln[n];
}

/* The DEFLATE fixed Huffman tables, derived as canonical codes from the fixed
 * length assignment (so they match the decoder's fixed tables exactly). */
static void fixed_codes(struct codes *lit, struct codes *dist)
{
	static uint8_t  len288[FIXLCODES];
	static uint16_t code288[FIXLCODES];
	int i = 0;
	for (; i < 144; i++) len288[i] = 8;
	for (; i < 256; i++) len288[i] = 9;
	for (; i < 280; i++) len288[i] = 7;
	for (; i < 288; i++) len288[i] = 8;
	build_canonical(len288, FIXLCODES, code288);
	for (i = 0; i < MAXLCODES; i++) {     /* 286/287 are never emitted       */
		lit->len[i] = len288[i];
		lit->code[i] = code288[i];
	}
	for (i = 0; i < MAXDCODES; i++)
		dist->len[i] = 5;
	build_canonical(dist->len, MAXDCODES, dist->code);
}

/* One LZ77 pass. emit==0: accumulate symbol frequencies into lf/df. emit==1:
 * write the tokens with the literal/length table lt and distance table dt.
 * Tokenization is deterministic, so the counting and emitting passes agree. */
static void lz77(const uint8_t *src, uint32_t slen, int *head, int *prev,
		 int emit, struct outbits *o,
		 const struct codes *lt, const struct codes *dt,
		 uint32_t *lf, uint32_t *df)
{
	for (uint32_t i = 0; i < HASH_SIZE; i++)
		head[i] = -1;
	uint32_t pos = 0;
	while (pos < slen) {
		int best_len = 0;
		uint32_t best_dist = 0;
		if (pos + MIN_MATCH <= slen) {
			uint32_t maxl = slen - pos;
			if (maxl > MAX_MATCH)
				maxl = MAX_MATCH;
			uint32_t limit = pos > WINDOW ? pos - WINDOW : 0;
			int cand = head[hash3(src + pos)];
			int chain = MAX_CHAIN;
			while (cand >= 0 && (uint32_t)cand >= limit && chain--) {
				uint32_t l = 0;
				while (l < maxl && src[cand + l] == src[pos + l])
					l++;
				if ((int)l > best_len) {
					best_len = (int)l;
					best_dist = pos - (uint32_t)cand;
					if (l == maxl)
						break;
				}
				cand = prev[(uint32_t)cand & (WINDOW - 1)];
			}
		}

		if (best_len >= MIN_MATCH) {
			uint32_t lex, dex;
			int leb, deb;
			int ls = len_code(best_len, &lex, &leb);
			int ds = dist_code(best_dist, &dex, &deb);
			if (emit) {
				put_code(o, lt->code[ls], lt->len[ls]);
				put_bits(o, lex, leb);
				put_code(o, dt->code[ds], dt->len[ds]);
				put_bits(o, dex, deb);
			} else {
				lf[ls]++;
				df[ds]++;
			}
			uint32_t end = pos + (uint32_t)best_len;
			while (pos < end)
				ht_insert(head, prev, src, slen, pos++);
		} else {
			int b = src[pos];
			if (emit)
				put_code(o, lt->code[b], lt->len[b]);
			else
				lf[b]++;
			ht_insert(head, prev, src, slen, pos++);
		}
		if (emit && o->err)
			return;
	}
}

/* Emit src as one or more uncompressed (stored) DEFLATE blocks. */
static void emit_stored(const uint8_t *src, uint32_t slen, struct outbits *o)
{
	uint32_t off = 0;
	do {
		uint32_t chunk = slen - off;
		if (chunk > 65535)
			chunk = 65535;
		int last = (off + chunk >= slen);
		put_bits(o, last ? 1 : 0, 1);  /* BFINAL                          */
		put_bits(o, 0, 2);             /* BTYPE = 00 (stored)             */
		flush_bits(o);                 /* align to a byte boundary        */
		put_raw(o, (uint8_t)(chunk & 0xFF));
		put_raw(o, (uint8_t)(chunk >> 8));
		put_raw(o, (uint8_t)(~chunk & 0xFF));
		put_raw(o, (uint8_t)((~chunk >> 8) & 0xFF));
		for (uint32_t i = 0; i < chunk; i++)
			put_raw(o, src[off + i]);
		off += chunk;
	} while (off < slen);
}

static const uint8_t clorder[19] = {
	16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

/* DEFLATE one block, choosing the cheapest of dynamic-Huffman, fixed-Huffman,
 * and stored. Two LZ77 passes (count, then emit) avoid storing the token list. */
static int deflate(const uint8_t *src, uint32_t slen, struct outbits *o)
{
	int *head = kmalloc(HASH_SIZE * sizeof(int));
	int *prev = kmalloc(WINDOW * sizeof(int));
	if (!head || !prev) {
		kfree(head);
		kfree(prev);
		return -1;
	}

	/* Pass 1: symbol frequencies. */
	static uint32_t lf[MAXLCODES], df[MAXDCODES];
	for (int i = 0; i < MAXLCODES; i++) lf[i] = 0;
	for (int i = 0; i < MAXDCODES; i++) df[i] = 0;
	lz77(src, slen, head, prev, 0, NULL, NULL, NULL, lf, df);
	lf[256]++;                              /* end-of-block, once             */

	/* Dynamic tables for this data. */
	static struct codes dlit, ddist;
	build_lengths(lf, MAXLCODES, 15, dlit.len);
	build_lengths(df, MAXDCODES, 15, ddist.len);
	build_canonical(dlit.len, MAXLCODES, dlit.code);
	build_canonical(ddist.len, MAXDCODES, ddist.code);

	int hlit = MAXLCODES;
	while (hlit > 257 && dlit.len[hlit - 1] == 0) hlit--;
	int hdist = MAXDCODES;
	while (hdist > 1 && ddist.len[hdist - 1] == 0) hdist--;

	/* Run-length-encode the combined length list into code-length symbols. */
	static uint8_t comb[MAXLCODES + MAXDCODES];
	int cn = 0;
	for (int i = 0; i < hlit; i++) comb[cn++] = dlit.len[i];
	for (int i = 0; i < hdist; i++) comb[cn++] = ddist.len[i];

	static uint8_t cl_sym[MAXLCODES + MAXDCODES];
	static uint8_t cl_extra[MAXLCODES + MAXDCODES];
	static uint8_t cl_eb[MAXLCODES + MAXDCODES];
	uint32_t cl_freq[19];
	for (int i = 0; i < 19; i++) cl_freq[i] = 0;
	int ct = 0;
	for (int i = 0; i < cn; ) {
		int v = comb[i], run = 1;
		while (i + run < cn && comb[i + run] == v) run++;
		i += run;
		if (v == 0) {
			while (run >= 11) {
				int take = run > 138 ? 138 : run;
				cl_sym[ct] = 18; cl_extra[ct] = (uint8_t)(take - 11);
				cl_eb[ct] = 7; cl_freq[18]++; ct++; run -= take;
			}
			while (run >= 3) {
				int take = run > 10 ? 10 : run;
				cl_sym[ct] = 17; cl_extra[ct] = (uint8_t)(take - 3);
				cl_eb[ct] = 3; cl_freq[17]++; ct++; run -= take;
			}
			while (run-- > 0) {
				cl_sym[ct] = 0; cl_extra[ct] = 0; cl_eb[ct] = 0;
				cl_freq[0]++; ct++;
			}
		} else {
			cl_sym[ct] = (uint8_t)v; cl_extra[ct] = 0; cl_eb[ct] = 0;
			cl_freq[v]++; ct++; run--;
			while (run >= 3) {
				int take = run > 6 ? 6 : run;
				cl_sym[ct] = 16; cl_extra[ct] = (uint8_t)(take - 3);
				cl_eb[ct] = 2; cl_freq[16]++; ct++; run -= take;
			}
			while (run-- > 0) {
				cl_sym[ct] = (uint8_t)v; cl_extra[ct] = 0; cl_eb[ct] = 0;
				cl_freq[v]++; ct++;
			}
		}
	}

	static struct codes clc;                /* code-length code (uses 0..18)   */
	build_lengths(cl_freq, 19, 7, clc.len);
	build_canonical(clc.len, 19, clc.code);
	int hclen = 19;
	while (hclen > 4 && clc.len[clorder[hclen - 1]] == 0) hclen--;

	/* Costs in bytes, to choose the block type. Length/distance extra bits are
	 * the same for dynamic and fixed, so they cancel — but include them so the
	 * comparison against stored is honest. */
	uint32_t extra = 0;
	for (int s = 257; s <= 285; s++) extra += lf[s] * lext[s - 257];
	for (int s = 0; s < MAXDCODES; s++) extra += df[s] * dext[s];

	uint32_t dyn_bits = 3 + 14 + (uint32_t)hclen * 3;
	for (int t = 0; t < ct; t++) dyn_bits += clc.len[cl_sym[t]] + cl_eb[t];
	for (int s = 0; s < MAXLCODES; s++) dyn_bits += lf[s] * dlit.len[s];
	for (int s = 0; s < MAXDCODES; s++) dyn_bits += df[s] * ddist.len[s];
	uint32_t dyn_bytes = (dyn_bits + extra + 7) / 8;

	uint32_t fixed_bits = 3;
	for (int s = 0; s < MAXLCODES; s++) {
		int fl = (s <= 143) ? 8 : (s <= 255) ? 9 : (s <= 279) ? 7 : 8;
		fixed_bits += lf[s] * (uint32_t)fl;
	}
	for (int s = 0; s < MAXDCODES; s++) fixed_bits += df[s] * 5;
	uint32_t fixed_bytes = (fixed_bits + extra + 7) / 8;

	uint32_t chunks = slen ? (slen + 65534) / 65535 : 1;
	uint32_t stored_bytes = 5 * chunks + slen;

	int choice = 0;                         /* 0 dynamic, 1 fixed, 2 stored    */
	if (fixed_bytes < dyn_bytes)
		choice = 1;
	uint32_t best = (choice == 1) ? fixed_bytes : dyn_bytes;
	if (stored_bytes < best)
		choice = 2;

	if (choice == 2) {
		emit_stored(src, slen, o);
	} else if (choice == 1) {
		static struct codes flit, fdist;
		fixed_codes(&flit, &fdist);
		put_bits(o, 1, 1);              /* BFINAL                          */
		put_bits(o, 1, 2);             /* BTYPE = 01 (fixed)              */
		lz77(src, slen, head, prev, 1, o, &flit, &fdist, NULL, NULL);
		put_code(o, flit.code[256], flit.len[256]);
	} else {
		put_bits(o, 1, 1);             /* BFINAL                          */
		put_bits(o, 2, 2);             /* BTYPE = 10 (dynamic)            */
		put_bits(o, (uint32_t)(hlit - 257), 5);
		put_bits(o, (uint32_t)(hdist - 1), 5);
		put_bits(o, (uint32_t)(hclen - 4), 4);
		for (int k = 0; k < hclen; k++)
			put_bits(o, clc.len[clorder[k]], 3);
		for (int t = 0; t < ct; t++) {
			put_code(o, clc.code[cl_sym[t]], clc.len[cl_sym[t]]);
			put_bits(o, cl_extra[t], cl_eb[t]);
		}
		lz77(src, slen, head, prev, 1, o, &dlit, &ddist, NULL, NULL);
		put_code(o, dlit.code[256], dlit.len[256]);
	}

	kfree(head);
	kfree(prev);
	return o->err ? -1 : 0;
}

/* ---- gzip framing ------------------------------------------------------ */

uint32_t gzip_bound(uint32_t src_len)
{
	/* gzip header (10) + trailer (8) + a fixed-Huffman body, whose worst
	 * case is ~9 bits per byte, plus block/EOB/padding slack. */
	return 18 + src_len + src_len / 8 + 64;
}

int gzip_compress(const uint8_t *src, uint32_t slen, uint8_t *dst, uint32_t dcap)
{
	if (dcap < 18)
		return -1;
	dst[0] = 0x1F; dst[1] = 0x8B;   /* magic                                  */
	dst[2] = 0x08;                  /* CM = deflate                           */
	dst[3] = 0x00;                  /* FLG = none                             */
	dst[4] = dst[5] = dst[6] = dst[7] = 0;   /* MTIME = 0                     */
	dst[8] = 0x00;                  /* XFL                                    */
	dst[9] = 0x03;                  /* OS = Unix                              */

	struct outbits o = { dst, dcap, 10, 0, 0, 0 };
	if (deflate(src, slen, &o) < 0)
		return -1;
	flush_bits(&o);
	if (o.err || o.pos + 8 > dcap)
		return -1;

	put_u32(dst + o.pos, crc32(src, slen));
	put_u32(dst + o.pos + 4, slen);
	return (int)(o.pos + 8);
}

int gzip_orig_size(const uint8_t *src, uint32_t slen)
{
	if (slen < 18 || src[0] != 0x1F || src[1] != 0x8B || src[2] != 0x08)
		return -1;
	return (int)load_le32(src + slen - 4);
}

int gzip_decompress(const uint8_t *src, uint32_t slen, uint8_t *dst, uint32_t dcap)
{
	if (slen < 18 || src[0] != 0x1F || src[1] != 0x8B || src[2] != 0x08)
		return -1;
	uint8_t flg = src[3];
	uint32_t pos = 10;

	if (flg & 0x04) {                       /* FEXTRA                          */
		if (pos + 2 > slen)
			return -1;
		uint32_t xlen = src[pos] | ((uint32_t)src[pos + 1] << 8);
		pos += 2 + xlen;
	}
	if (flg & 0x08)                         /* FNAME (NUL-terminated)          */
		while (pos < slen && src[pos++])
			;
	if (flg & 0x10)                         /* FCOMMENT                        */
		while (pos < slen && src[pos++])
			;
	if (flg & 0x02)                         /* FHCRC                           */
		pos += 2;
	if (pos >= slen)
		return -1;

	int orig = (int)load_le32(src + slen - 4);
	if (orig < 0 || (uint32_t)orig > dcap)
		return -1;

	int n = inflate(src, pos, slen - 8, dst, dcap);
	if (n < 0 || (uint32_t)n != (uint32_t)orig)
		return -1;
	if (crc32(dst, (uint32_t)n) != load_le32(src + slen - 8))
		return -1;
	return n;
}

/* Raw zlib stream (RFC 1950): a 2-byte header, the DEFLATE body, a 4-byte
 * adler32. The header's low nibble of byte 0 is the compression method (8 =
 * DEFLATE); FDICT (byte 1 bit 5) means a preset dictionary follows, which PNG
 * never uses and we don't support. We reuse the very same inflate() as gzip —
 * only the envelope differs — and leave the adler32 trailer unverified. */
int zlib_inflate(const uint8_t *src, uint32_t slen, uint8_t *dst, uint32_t dcap)
{
	if (slen < 6)                           /* 2 header + >=0 body + 4 adler   */
		return -1;
	if ((src[0] & 0x0F) != 0x08)            /* CM must be DEFLATE              */
		return -1;
	if (((uint32_t)src[0] << 8 | src[1]) % 31 != 0)  /* FCHECK: header % 31    */
		return -1;
	if (src[1] & 0x20)                      /* FDICT — unsupported             */
		return -1;

	return inflate(src, 2, slen - 4, dst, dcap);
}

/* adler32 (RFC 1950): two rolling 16-bit sums mod 65521. Stored big-endian in
 * the zlib trailer (unlike gzip's little-endian CRC32). */
static uint32_t adler32(const uint8_t *data, uint32_t n)
{
	uint32_t a = 1, b = 0;
	for (uint32_t i = 0; i < n; i++) {
		a = (a + data[i]) % 65521;
		b = (b + a) % 65521;
	}
	return (b << 16) | a;
}

int zlib_compress(const uint8_t *src, uint32_t slen, uint8_t *dst, uint32_t dcap)
{
	if (dcap < 6)
		return -1;
	dst[0] = 0x78;                  /* CM=8 (DEFLATE), CINFO=7 (32K window)    */
	dst[1] = 0x01;                  /* FLEVEL=0, no FDICT; 0x7801 % 31 == 0    */

	struct outbits o = { dst, dcap, 2, 0, 0, 0 };
	if (deflate(src, slen, &o) < 0)
		return -1;
	flush_bits(&o);
	if (o.err || o.pos + 4 > dcap)
		return -1;

	uint32_t ad = adler32(src, slen);     /* big-endian trailer                */
	dst[o.pos + 0] = (uint8_t)(ad >> 24);
	dst[o.pos + 1] = (uint8_t)(ad >> 16);
	dst[o.pos + 2] = (uint8_t)(ad >> 8);
	dst[o.pos + 3] = (uint8_t)ad;
	return (int)(o.pos + 4);
}

int zlib_store(const uint8_t *src, uint32_t slen, uint8_t *dst, uint32_t dcap)
{
	/* Worst case: header(2) + one 5-byte block header per <=65535-byte chunk +
	 * the data + adler(4). */
	uint32_t nblocks = slen ? (slen + 65534) / 65535 : 1;
	if (dcap < 2 + nblocks * 5 + slen + 4)
		return -1;

	uint32_t pos = 0;
	dst[pos++] = 0x78;              /* CM=8, CINFO=7; 0x7801 % 31 == 0         */
	dst[pos++] = 0x01;

	/* One or more stored blocks. A stored block's 3-bit header (BFINAL, BTYPE=00)
	 * sits in the low bits of a byte and is then padded to a byte boundary, so a
	 * whole header byte of 0x00 (or 0x01 for the final block) is exactly right. */
	uint32_t off = 0;
	do {
		uint32_t chunk = slen - off;
		if (chunk > 65535)
			chunk = 65535;
		int final = (off + chunk >= slen);
		dst[pos++] = final ? 0x01 : 0x00;       /* BFINAL | BTYPE=00 (stored)  */
		dst[pos++] = (uint8_t)chunk;            /* LEN  (little-endian)        */
		dst[pos++] = (uint8_t)(chunk >> 8);
		dst[pos++] = (uint8_t)~chunk;           /* NLEN = ~LEN                 */
		dst[pos++] = (uint8_t)(~chunk >> 8);
		for (uint32_t i = 0; i < chunk; i++)
			dst[pos++] = src[off + i];
		off += chunk;
	} while (off < slen);

	uint32_t ad = adler32(src, slen);           /* big-endian trailer          */
	dst[pos++] = (uint8_t)(ad >> 24);
	dst[pos++] = (uint8_t)(ad >> 16);
	dst[pos++] = (uint8_t)(ad >> 8);
	dst[pos++] = (uint8_t)ad;
	return (int)pos;
}
