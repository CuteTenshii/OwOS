/* encode.c — a from-scratch Brotli compressor (RFC 7932).
 *
 * Produces a valid stream any compliant decoder accepts. The scope is a
 * pragmatic ratio, not Google-encoder quality: greedy LZ77 (a hash-chain match
 * finder) over the whole input window, emitted as insert-and-copy commands with
 * per-meta-block canonical Huffman trees for the literal, command and distance
 * alphabets. It keeps the format simple where the format allows — one block type
 * and one Huffman tree per category, NPOSTFIX=NDIRECT=0, and only "regular"
 * distance codes (the distance ring's short codes are a decoder-side
 * optimization we never need to emit) — which keeps the encoder small while
 * still doing real LZ + entropy coding.
 *
 * Whole-buffer: takes the full input and returns a malloc'd stream; the caller
 * frees it. NULL on OOM. */
#include "brotli.h"
#include "owos.h"

/* ------------------------------------------------------------ bit writer --- *
 * LSB-first within each byte, bytes in stream order — the exact layout the
 * decoder's bit-indexed reader consumes. */
struct bw {
	unsigned char *buf;
	long cap;
	long byte;
	int bit;                 /* 0..7 */
	int oom;
};

static void bw_ensure(struct bw *w, long extra)
{
	if (w->byte + extra + 1 <= w->cap) return;
	long ncap = (w->cap * 2) + extra + 64;
	unsigned char *nb = realloc(w->buf, ncap);
	if (!nb) { w->oom = 1; return; }
	w->buf = nb;
	w->cap = ncap;
}
static void bw_bits(struct bw *w, unsigned val, int n)
{
	bw_ensure(w, (n >> 3) + 2);
	if (w->oom) return;
	for (int i = 0; i < n; i++) {
		if (w->bit == 0) w->buf[w->byte] = 0;
		w->buf[w->byte] |= (unsigned char)(((val >> i) & 1) << w->bit);
		if (++w->bit == 8) { w->bit = 0; w->byte++; }
	}
}

/* ---------------------------------------------------------- huffman build -- *
 * Frequencies -> per-symbol code lengths (canonical, length-limited), then the
 * canonical codes pre-reversed for LSB-first emission. */
struct enc_huff { unsigned char len[704]; unsigned short code[704]; int n; };

/* Length-limited canonical Huffman lengths from freq[0..n). Lengths are capped
 * at `limit` via the standard histogram-overflow correction; the result is a
 * complete code. Single-symbol alphabets are handled by the caller. */
/* Scratch for build_lengths kept file-static: the OS user stack is only 16 KiB,
 * and these arrays (~22 KiB) would overflow it. The encoder is single-flow, and
 * build_lengths is never called re-entrantly, so static is safe. */
static int bl_parent[2 * 704];
static long bl_nf[2 * 704];
static int bl_heap[2 * 704];
static int bl_order[704];

static void build_lengths(const long *freq, int n, int limit, unsigned char *len)
{
	int *parent = bl_parent;
	long *nf = bl_nf;
	int *heap = bl_heap;
	for (int i = 0; i < n; i++) len[i] = 0;
	(void)bl_order;
	int total = 0;
	for (int i = 0; i < n; i++) if (freq[i] > 0) total++;
	if (total == 0) return;
	if (total == 1) { for (int i = 0; i < n; i++) if (freq[i] > 0) len[i] = 1; return; }

	/* Build a Huffman tree; if it is deeper than `limit`, raise the floor on the
	 * leaf weights and rebuild. Equalising the weights balances the tree and
	 * shrinks its depth; since 2^limit >= n the loop always converges. (This is
	 * the brotli encoder's own length-limiting strategy: simple and always a
	 * complete code, at a small cost in optimality.) */
	long count_limit = 1;
	for (;;) {
		int hn = 0, cnt = n;
		for (int i = 0; i < n; i++) {
			parent[i] = -1;
			if (freq[i] > 0) { nf[i] = freq[i] < count_limit ? count_limit : freq[i]; heap[hn++] = i; }
		}
		/* keep heap[] sorted ascending by nf (insertion sort) */
		for (int i = 1; i < hn; i++) {
			int x = heap[i], j = i;
			while (j > 0 && nf[heap[j - 1]] > nf[x]) { heap[j] = heap[j - 1]; j--; }
			heap[j] = x;
		}
		while (hn > 1) {
			int a = heap[0];
			for (int i = 1; i < hn; i++) heap[i - 1] = heap[i];
			hn--;
			int bnode = heap[0];
			for (int i = 1; i < hn; i++) heap[i - 1] = heap[i];
			hn--;
			int node = cnt++;
			parent[node] = -1;
			nf[node] = nf[a] + nf[bnode];
			parent[a] = node; parent[bnode] = node;
			int j = hn;
			while (j > 0 && nf[heap[j - 1]] > nf[node]) { heap[j] = heap[j - 1]; j--; }
			heap[j] = node; hn++;
		}

		int maxlen = 0;
		for (int i = 0; i < n; i++) {
			if (freq[i] <= 0) { len[i] = 0; continue; }
			int d = 0, p = parent[i];
			while (p != -1) { d++; p = parent[p]; }
			if (d == 0) d = 1;
			len[i] = (unsigned char)d;
			if (d > maxlen) maxlen = d;
		}
		if (maxlen <= limit) break;
		count_limit *= 2;
	}
}

/* Canonical codes from lengths, pre-reversed for LSB-first emission. */
static void build_codes(struct enc_huff *h)
{
	int next[16], code = 0;
	int count[16] = {0};
	for (int i = 0; i < h->n; i++) count[h->len[i]]++;
	count[0] = 0;
	for (int i = 1; i <= 15; i++) { next[i] = code; code = (code + count[i]) << 1; }
	for (int i = 0; i < h->n; i++) {
		int l = h->len[i];
		if (!l) { h->code[i] = 0; continue; }
		int c = next[l]++;
		int rev = 0;
		for (int k = 0; k < l; k++) rev |= ((c >> k) & 1) << (l - 1 - k);
		h->code[i] = (unsigned short)rev;
	}
}

/* Emit a Huffman code description (complex prefix code, no run-length coding)
 * so the decoder reconstructs the same canonical code. `freq` selects simple vs
 * complex: a single-symbol alphabet uses the 1-symbol simple code. */
static const struct { unsigned char nbits, code; } CLC_EMIT[6] = {
	{2, 0}, {4, 7}, {3, 3}, {2, 2}, {2, 1}, {4, 15}      /* value 0..5 -> bit pattern */
};

static void emit_tree(struct bw *w, struct enc_huff *h, const long *freq)
{
	int distinct = 0, only = -1;
	for (int i = 0; i < h->n; i++) if (freq[i] > 0) { distinct++; only = i; }

	if (distinct == 1) {
		/* Simple prefix code, 1 symbol: HSKIP=1, nsym-1=0, then the symbol. */
		bw_bits(w, 1, 2);
		bw_bits(w, 0, 2);
		int symbits = 0, a = h->n - 1;
		while (a) { symbits++; a >>= 1; }
		bw_bits(w, only, symbits);
		for (int i = 0; i < h->n; i++) h->len[i] = 0;
		h->len[only] = 0;                       /* 0-bit symbol */
		h->code[only] = 0;
		return;
	}

	/* Complex prefix code. Code-length symbols are the per-symbol lengths
	 * (values 0..15); build a Huffman over them (max length 5) and emit it via
	 * the fixed code-length-code prefix. */
	long clfreq[18] = {0};
	int last_nonzero = 0;
	for (int i = 0; i < h->n; i++) { clfreq[h->len[i]]++; if (h->len[i]) last_nonzero = i; }

	unsigned char cllen[18];
	build_lengths(clfreq, 18, 5, cllen);
	int clc_distinct = 0;
	for (int i = 0; i < 18; i++) if (cllen[i]) clc_distinct++;
	struct enc_huff clh;
	clh.n = 18;
	for (int i = 0; i < 18; i++) clh.len[i] = cllen[i];
	build_codes(&clh);

	bw_bits(w, 0, 2);                                /* HSKIP = 0 */
	static const unsigned char order[18] =
		{1, 2, 3, 4, 0, 5, 17, 6, 16, 7, 8, 9, 10, 11, 12, 13, 14, 15};
	/* Emit code-length-code lengths, mirroring the decoder exactly: it reads
	 * entries until the code-length-code's Huffman space is exhausted. For a
	 * complete code that is the last nonzero entry; for a degenerate 1-symbol
	 * code the space never empties, so all 18 entries must be emitted. */
	int space = 32;
	for (int i = 0; i < 18; i++) {
		int v = cllen[order[i]];
		bw_bits(w, CLC_EMIT[v].code, CLC_EMIT[v].nbits);
		if (v) { space -= 32 >> v; if (space <= 0) break; }
	}

	/* Emit each symbol's length code, up to (and including) the last nonzero —
	 * the decoder stops reading once the Huffman space is exhausted there. A
	 * single-symbol code-length-code is a 0-bit code: the per-symbol lengths
	 * are then implied and emit nothing (matching the reference encoder). */
	if (clc_distinct > 1) {
		for (int i = 0; i <= last_nonzero; i++) {
			int v = h->len[i];
			bw_bits(w, clh.code[v], clh.len[v]);
		}
	}
}

/* ----------------------------------------------------- command / distance -- */
static const unsigned char kInsExtra[24] =
	{0,0,0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,7,8,9,10,12,14,24};
static const unsigned char kCopyExtra[24] =
	{0,0,0,0,0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,7,8,9,10,24};
static int ins_off[24], copy_off[24];
static long dist_base[64];
static int dist_extra[64];

static void enc_tables(void)
{
	ins_off[0] = 0; copy_off[0] = 2;
	for (int i = 0; i < 23; i++) {
		ins_off[i + 1] = ins_off[i] + (1 << kInsExtra[i]);
		copy_off[i + 1] = copy_off[i] + (1 << kCopyExtra[i]);
	}
	/* NPOSTFIX=0, NDIRECT=0 distance code table (codes 16..63). */
	int di = 16, bits = 1, half = 0;
	while (di < 64) {
		long base = (((long)(2 + half) << bits) - 4) + 1;
		dist_extra[di] = bits;
		dist_base[di] = base;
		di++;
		bits += half; half ^= 1;
	}
}

/* Insert/copy length -> the 0..23 prefix code for it. */
static int len_code(long v, const int *off, const unsigned char *extra, int base2)
{
	int c = 0;
	for (int i = 0; i < 24; i++) {
		long lo = off[i];
		long hi = lo + (1L << extra[i]) - 1;
		if (v >= lo && v <= hi) { c = i; break; }
		c = i;
	}
	(void)base2;
	return c;
}

/* (insert_code, copy_code) -> command symbol (explicit distance, dist0=false). */
static int cmd_symbol(int ins_code, int copy_code)
{
	static const unsigned char cell_for[3][3] =
		{ {2, 3, 6}, {4, 5, 8}, {7, 9, 10} };
	int bi = (ins_code & 0x18) >> 3;
	int bc = (copy_code & 0x18) >> 3;
	int cell = cell_for[bi][bc];
	return cell * 64 + ((ins_code & 7) << 3) + (copy_code & 7);
}

/* distance value -> regular distance code (16..63). Returns -1 if out of range. */
static int dist_code(long d)
{
	for (int c = 16; c < 64; c++) {
		long lo = dist_base[c], hi = lo + (1L << dist_extra[c]) - 1;
		if (d >= lo && d <= hi) return c;
	}
	return -1;
}

/* --------------------------------------------------------------- LZ parse -- */
struct cmd { long insert; long lit_pos; long copy; long dist; };  /* dist 0 => insert-only */

#define HBITS 17
#define HSIZE (1 << HBITS)
#define MIN_MATCH 4
#define MAX_CHAIN 64

static unsigned hash4(const unsigned char *p)
{
	unsigned v = p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24);
	return (v * 2654435761u) >> (32 - HBITS);
}

/* =============================================================== encode ==== */
unsigned char *brotli_compress(const unsigned char *in, int in_len, int *out_len)
{
	enc_tables();
	struct bw w = {0};
	w.buf = malloc(64);
	if (!w.buf) return 0;
	w.cap = 64;

	/* WBITS: smallest window that covers the whole input. */
	int wbits = 10;
	while (wbits < 24 && (((long)1 << wbits) - 16) < in_len) wbits++;
	long max_backward = ((long)1 << wbits) - 16;
	if (in_len == 0) max_backward = 0;

	/* Emit WBITS (RFC 7932 §9.1). */
	if (wbits == 16) bw_bits(&w, 0, 1);
	else if (wbits >= 18) { bw_bits(&w, 1, 1); bw_bits(&w, wbits - 17, 3); }
	else if (wbits == 17) { bw_bits(&w, 1, 1); bw_bits(&w, 0, 3); bw_bits(&w, 0, 3); }
	else { bw_bits(&w, 1, 1); bw_bits(&w, 0, 3); bw_bits(&w, wbits - 8, 3); }

	if (in_len == 0) {
		bw_bits(&w, 1, 1);              /* ISLAST */
		bw_bits(&w, 1, 1);              /* ISLASTEMPTY */
		*out_len = (int)(w.byte + (w.bit ? 1 : 0));
		return w.buf;
	}

	/* Hash-chain match finder over the whole input (a single global window).
	 * cmds is bounded per meta-block; CHUNK keeps it (and the per-block trees)
	 * small. Back-references still span the whole window via head/prev. */
	const long CHUNK = (long)1 << 18;               /* meta-block size, <= 2^24 */
	long chunk_cap = in_len < CHUNK ? in_len : CHUNK;
	int *head = malloc(sizeof(int) * HSIZE);
	int *prev = malloc(sizeof(int) * (in_len > 0 ? in_len : 1));
	struct cmd *cmds = malloc(sizeof(struct cmd) * (chunk_cap + 2));
	if (!head || !prev || !cmds || w.oom) { free(head); free(prev); free(cmds); free(w.buf); return 0; }
	for (int i = 0; i < HSIZE; i++) head[i] = -1;

	long pos = 0;

	while (pos < in_len) {
		long chunk_end = pos + CHUNK;
		if (chunk_end > in_len) chunk_end = in_len;
		int is_last = (chunk_end == in_len);

		/* ---- LZ parse for this meta-block, gathering frequencies. ---- *
		 * Static (single-flow) to keep the OS user stack small. */
		static long freq_lit[256], freq_cmd[704], freq_dist[64];
		for (int i = 0; i < 256; i++) freq_lit[i] = 0;
		for (int i = 0; i < 704; i++) freq_cmd[i] = 0;
		for (int i = 0; i < 64; i++) freq_dist[i] = 0;
		int ncmd = 0;
		long literal_run = 0;
		long run_start = pos;
		long p = pos;

		while (p < chunk_end) {
			long best_len = 0, best_dist = 0;
			if (p + MIN_MATCH <= in_len) {
				unsigned h = hash4(in + p);
				int cand = head[h];
				int chain = MAX_CHAIN;
				long limit = p - max_backward;
				if (limit < 0) limit = 0;
				while (cand >= 0 && cand >= limit && chain-- > 0) {
					long mlen = 0, maxm = chunk_end - p;   /* copy stays in this meta-block */
					while (mlen < maxm && in[cand + mlen] == in[p + mlen]) mlen++;
					if (mlen > best_len) { best_len = mlen; best_dist = p - cand; if (mlen >= maxm) break; }
					cand = prev[cand];
				}
			}

			if (best_len >= MIN_MATCH) {
				/* Close the literal run as this command's insert. */
				cmds[ncmd].insert = literal_run;
				cmds[ncmd].lit_pos = run_start;
				cmds[ncmd].copy = best_len;
				cmds[ncmd].dist = best_dist;
				for (long k = run_start; k < run_start + literal_run; k++) freq_lit[in[k]]++;
				freq_cmd[cmd_symbol(len_code(literal_run, ins_off, kInsExtra, 0),
				                    len_code(best_len, copy_off, kCopyExtra, 2))]++;
				int dc = dist_code(best_dist);
				if (dc >= 0) freq_dist[dc]++;
				ncmd++;
				/* Insert positions covered by the match into the hash. */
				long end = p + best_len;
				while (p < end) {
					if (p + MIN_MATCH <= in_len) {
						unsigned h2 = hash4(in + p);
						prev[p] = head[h2];
						head[h2] = (int)p;
					}
					p++;
				}
				literal_run = 0;
				run_start = p;
			} else {
				if (p + MIN_MATCH <= in_len) {
					unsigned h2 = hash4(in + p);
					prev[p] = head[h2];
					head[h2] = (int)p;
				}
				literal_run++;
				p++;
			}
		}
		/* Trailing literals -> a final insert-only command (copy ignored). */
		if (literal_run > 0) {
			cmds[ncmd].insert = literal_run;
			cmds[ncmd].lit_pos = run_start;
			cmds[ncmd].copy = 0;
			cmds[ncmd].dist = 0;
			for (long k = run_start; k < run_start + literal_run; k++) freq_lit[in[k]]++;
			freq_cmd[cmd_symbol(len_code(literal_run, ins_off, kInsExtra, 0), 0)]++;
			ncmd++;
		}

		long mlen = chunk_end - pos;

		/* Every tree needs at least one symbol so it forms a complete (or
		 * 1-symbol simple) code; a category may be unused (e.g. an all-copy
		 * block has no literals). The phantom symbol is never emitted. */
		{
			int any;
			any = 0; for (int i = 0; i < 256; i++) any |= (freq_lit[i] != 0); if (!any) freq_lit[0] = 1;
			any = 0; for (int i = 0; i < 704; i++) any |= (freq_cmd[i] != 0); if (!any) freq_cmd[0] = 1;
			any = 0; for (int i = 0; i < 64; i++) any |= (freq_dist[i] != 0); if (!any) freq_dist[0] = 1;
		}

		/* ---- Build the three Huffman trees. ---- */
		struct enc_huff *lit = malloc(sizeof(struct enc_huff));
		struct enc_huff *cmdh = malloc(sizeof(struct enc_huff));
		struct enc_huff *dsth = malloc(sizeof(struct enc_huff));
		if (!lit || !cmdh || !dsth) { free(lit); free(cmdh); free(dsth); goto oom; }
		lit->n = 256; cmdh->n = 704; dsth->n = 64;
		build_lengths(freq_lit, 256, 15, lit->len);  build_codes(lit);
		build_lengths(freq_cmd, 704, 15, cmdh->len); build_codes(cmdh);
		build_lengths(freq_dist, 64, 15, dsth->len); build_codes(dsth);

		/* ---- Meta-block header. ---- */
		bw_bits(&w, is_last ? 1 : 0, 1);
		if (is_last) bw_bits(&w, 0, 1);                  /* ISLASTEMPTY = 0 */
		long m1 = mlen - 1;
		int nnib = (m1 < (1 << 16)) ? 4 : (m1 < (1 << 20)) ? 5 : 6;
		bw_bits(&w, nnib - 4, 2);
		for (int i = 0; i < nnib; i++) bw_bits(&w, (unsigned)((m1 >> (4 * i)) & 0xF), 4);
		if (!is_last) bw_bits(&w, 0, 1);                 /* ISUNCOMPRESSED = 0 */

		for (int t = 0; t < 3; t++) bw_bits(&w, 0, 1);   /* NBLTYPES = 1 (varlen 0) */
		bw_bits(&w, 0, 2);                               /* NPOSTFIX = 0 */
		bw_bits(&w, 0, 4);                               /* NDIRECT  = 0 */
		bw_bits(&w, 0, 2);                               /* literal context mode 0 */
		bw_bits(&w, 0, 1);                               /* literal context map: 1 tree */
		bw_bits(&w, 0, 1);                               /* distance context map: 1 tree */

		emit_tree(&w, lit, freq_lit);
		emit_tree(&w, cmdh, freq_cmd);
		emit_tree(&w, dsth, freq_dist);

		/* ---- Commands. ---- */
		for (int c = 0; c < ncmd; c++) {
			struct cmd *cm = &cmds[c];
			int last_cmd = (c == ncmd - 1);
			int ic = len_code(cm->insert, ins_off, kInsExtra, 0);
			int cc = (cm->copy >= 2) ? len_code(cm->copy, copy_off, kCopyExtra, 2) : 0;
			int sym = cmd_symbol(ic, cc);
			bw_bits(&w, cmdh->code[sym], cmdh->len[sym]);
			bw_bits(&w, (unsigned)(cm->insert - ins_off[ic]), kInsExtra[ic]);
			if (cm->copy >= 2) bw_bits(&w, (unsigned)(cm->copy - copy_off[cc]), kCopyExtra[cc]);
			else bw_bits(&w, 0, kCopyExtra[cc]);
			/* Literals. */
			for (long k = 0; k < cm->insert; k++) {
				int b = in[cm->lit_pos + k];
				bw_bits(&w, lit->code[b], lit->len[b]);
			}
			/* Distance (omitted for the insert-only trailing command). */
			if (!(last_cmd && cm->copy == 0) && cm->copy >= 2) {
				int dc = dist_code(cm->dist);
				bw_bits(&w, dsth->code[dc], dsth->len[dc]);
				bw_bits(&w, (unsigned)(cm->dist - dist_base[dc]), dist_extra[dc]);
			}
			if (w.oom) { free(lit); free(cmdh); free(dsth); goto oom; }
		}

		free(lit); free(cmdh); free(dsth);
		pos = chunk_end;
	}

	free(head); free(prev); free(cmds);
	if (w.oom) { free(w.buf); return 0; }
	*out_len = (int)(w.byte + (w.bit ? 1 : 0));
	return w.buf;

oom:
	free(head); free(prev); free(cmds); free(w.buf);
	return 0;
}
