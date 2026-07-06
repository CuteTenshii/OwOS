/* h2.c — HTTP/2 client engine (see h2.h). One stream, one request: the shape
 * libcurl drives. Layered as: HPACK (Huffman + integer coding, static/dynamic
 * tables) under a header coder, the binary frame layer over a buffered reader,
 * and h2_request() tying preface → request stream → response on top.
 *
 * Scope, deliberately: GET-shaped requests (no request body), no server push
 * (we set SETTINGS_ENABLE_PUSH=0), in-order single stream. Flow control is
 * handled by opening our receive windows wide up front (a large
 * SETTINGS_INITIAL_WINDOW_SIZE + one big connection WINDOW_UPDATE) so we never
 * have to dribble per-DATA updates — enough for fetching a resource. */

#include "owos.h"
#include "http.h"
#include "h2.h"
#include "netconn.h"

/* ------------------------------------------------------------ HPACK Huffman */

/* RFC 7541 Appendix B: the canonical Huffman code per octet (and EOS at 256).
 * Response header strings are usually Huffman-coded, so decoding is mandatory.
 * We only ever decode (the request side sends raw literals), so no encoder. */
static const struct { unsigned code; unsigned char nbits; } HUFF[257] = {
	{0x1ff8,13},{0x7fffd8,23},{0xfffffe2,28},{0xfffffe3,28},{0xfffffe4,28},
	{0xfffffe5,28},{0xfffffe6,28},{0xfffffe7,28},{0xfffffe8,28},{0xffffea,24},
	{0x3ffffffc,30},{0xfffffe9,28},{0xfffffea,28},{0x3ffffffd,30},{0xfffffeb,28},
	{0xfffffec,28},{0xfffffed,28},{0xfffffee,28},{0xfffffef,28},{0xffffff0,28},
	{0xffffff1,28},{0xffffff2,28},{0x3ffffffe,30},{0xffffff3,28},{0xffffff4,28},
	{0xffffff5,28},{0xffffff6,28},{0xffffff7,28},{0xffffff8,28},{0xffffff9,28},
	{0xffffffa,28},{0xffffffb,28},                              /* 0..31 */
	{0x14,6},{0x3f8,10},{0x3f9,10},{0xffa,12},{0x1ff9,13},      /* ' '..'$' */
	{0x15,6},{0xf8,8},{0xffb,12},{0x3fa,10},{0x3fb,10},         /* '%'..')' */
	{0xf9,8},{0xffc,12},{0xfa,8},{0x16,6},{0x17,6},{0x18,6},    /* '*'../'  */
	{0x0,5},{0x1,5},{0x2,5},{0x19,6},{0x1a,6},{0x1b,6},{0x1c,6},/* '0'..'6' */
	{0x1d,6},{0x1e,6},{0x1f,6},{0x5c,7},{0xfb,8},{0x7ffc,15},   /* '7'..'<' */
	{0x20,6},{0xffd,12},{0x3fc,10},{0x1ffa,13},                 /* '='..'@' */
	{0x21,6},{0x5d,7},{0x5e,7},{0x5f,7},{0x60,7},{0x61,7},      /* 'A'..'F' */
	{0x62,7},{0x63,7},{0x64,7},{0x65,7},{0x66,7},{0x67,7},      /* 'G'..'L' */
	{0x68,7},{0x69,7},{0x6a,7},{0x6b,7},{0x6c,7},{0x6d,7},      /* 'M'..'R' */
	{0x6e,7},{0x6f,7},{0x70,7},{0x71,7},{0x72,7},{0xfc,8},      /* 'S'..'X' */
	{0x73,7},{0xfd,8},{0x1ffb,13},{0x7fff0,19},{0x1ffc,13},     /* 'Y'..']' */
	{0x3ffc,14},{0x22,6},{0x7ffd,15},                          /* '^'..'`' */
	{0x3,5},{0x23,6},{0x4,5},{0x24,6},{0x5,5},{0x25,6},         /* 'a'..'f' */
	{0x26,6},{0x27,6},{0x6,5},{0x74,7},{0x75,7},{0x28,6},       /* 'g'..'l' */
	{0x29,6},{0x2a,6},{0x7,5},{0x2b,6},{0x76,7},{0x2c,6},       /* 'm'..'r' */
	{0x8,5},{0x9,5},{0x2d,6},{0x77,7},{0x78,7},{0x79,7},        /* 's'..'x' */
	{0x7a,7},{0x7b,7},{0x7ffe,15},{0x7fc,11},{0x3ffd,14},{0x1ffd,13}, /* 'y'..'~' */
	{0xffffffc,28},                                            /* 127 */
	{0xfffe6,20},{0x3fffd2,22},{0xfffe7,20},{0xfffe8,20},{0x3fffd3,22}, /* 128..132 */
	{0x3fffd4,22},{0x3fffd5,22},{0x7fffd9,23},{0x3fffd6,22},{0x7fffda,23},/* 133..137 */
	{0x7fffdb,23},{0x7fffdc,23},{0x7fffdd,23},{0x7fffde,23},{0xffffeb,24}, /* 138..142 */
	{0x7fffdf,23},{0xffffec,24},{0xffffed,24},{0x3fffd7,22},{0x7fffe0,23}, /* 143..147 */
	{0xffffee,24},{0x7fffe1,23},{0x3fffd8,22},{0x7fffe2,23},{0x7fffe3,23}, /* 148..152 */
	{0x7fffe4,23},{0x1fffdc,21},{0x3fffd9,22},{0x7fffe5,23},{0x3fffda,22}, /* 153..157 */
	{0x7fffe6,23},{0x7fffe7,23},{0xffffef,24},{0x3fffdb,22},{0x1fffdd,21}, /* 158..162 */
	{0xfffe9,20},{0x3fffdc,22},{0x3fffdd,22},{0x7fffe8,23},{0x7fffe9,23},  /* 163..167 */
	{0x1fffde,21},{0x7fffea,23},{0x3fffde,22},{0x7fffeb,23},{0x7fffec,23}, /* 168..172 */
	{0x3fffdf,22},{0x3fffe0,22},{0x7fffed,23},{0x3fffe1,22},{0x7fffee,23}, /* 173..177 */
	{0x7fffef,23},{0x1fffdf,21},{0x3fffe2,22},{0x3fffe3,22},{0x3fffe4,22}, /* 178..182 */
	{0x7ffff0,23},{0x3fffe5,22},{0x1fffe0,21},{0x7ffff1,23},{0x3fffe6,22}, /* 183..187 */
	{0x3fffe7,22},{0x3fffe8,22},{0x1fffe1,21},{0x3ffffe0,26},{0x3ffffe1,26}, /* 188..192 */
	{0xfffeb,20},{0x7fff1,19},{0x3fffe9,22},{0x7ffff2,23},{0x3fffea,22},  /* 193..197 */
	{0x1ffffec,25},{0x3ffffe2,26},{0x3ffffe3,26},{0x3ffffe4,26},{0x7ffffde,27}, /* 198..202 */
	{0x7ffffdf,27},{0x3ffffe5,26},{0xfffff1,24},{0x1ffffed,25},{0x7fff2,19}, /* 203..207 */
	{0x1fffe2,21},{0x3ffffe6,26},{0x7ffffe0,27},{0x7ffffe1,27},{0x3ffffe7,26}, /* 208..212 */
	{0x7ffffe2,27},{0xfffff2,24},{0x1fffe3,21},{0x1fffe4,21},{0x3ffffe8,26}, /* 213..217 */
	{0x3ffffe9,26},{0xffffffd,28},{0x7ffffe3,27},{0x7ffffe4,27},{0x7ffffe5,27}, /* 218..222 */
	{0xfffec,20},{0xfffff3,24},{0xfffed,20},{0x1fffe5,21},{0x3fffea,22},  /* 223..227 */
	{0x1fffe6,21},{0x1fffe7,21},{0x7ffff3,23},{0x3fffeb,22},{0x3fffec,22},/* 228..232 */
	{0x1ffffee,25},{0x1ffffef,25},{0xfffff4,24},{0xfffff5,24},{0x3ffffea,26}, /* 233..237 */
	{0x7ffff4,23},{0x3ffffeb,26},{0x7ffffe6,27},{0x3ffffec,26},{0x3ffffed,26}, /* 238..242 */
	{0x7ffffe7,27},{0x7ffffe8,27},{0x7ffffe9,27},{0x7ffffea,27},{0x7ffffeb,27}, /* 243..247 */
	{0xffffffe,28},{0x7ffffec,27},{0x7ffffed,27},{0x7ffffee,27},{0x7ffffef,27}, /* 248..252 */
	{0x7fffff0,27},{0x3ffffee,26},                             /* 253..255 */
	{0x3fffffff,30},                                           /* 256 EOS */
};

/* Decode `inlen` Huffman bytes into out[outcap]. Bit-by-bit, MSB first; a code
 * is recognised the moment the accumulated bits match an entry's (code,nbits)
 * — the codes are prefix-free, so at most one matches at a given length. */
static int huff_decode(const unsigned char *in, int inlen,
                       char *out, int outcap, int *outlen)
{
	unsigned cur = 0;
	int nbits = 0, o = 0;
	for (int i = 0; i < inlen; i++) {
		for (int bit = 7; bit >= 0; bit--) {
			cur = (cur << 1) | ((in[i] >> bit) & 1u);
			nbits++;
			if (nbits > 30)
				return -1;                       /* no code this long */
			int sym = -1;
			for (int s = 0; s < 256; s++)        /* EOS (256) never emitted */
				if (HUFF[s].nbits == (unsigned char)nbits &&
				    HUFF[s].code == cur) { sym = s; break; }
			if (sym >= 0) {
				if (o >= outcap)
					return -1;
				out[o++] = (char)sym;
				cur = 0;
				nbits = 0;
			}
		}
	}
	if (nbits >= 8)                                  /* >7 leftover bits is bad */
		return -1;
	*outlen = o;
	return 0;
}

/* ----------------------------------------------------------- HPACK tables */

/* Static table (RFC 7541 Appendix A), indices 1..61. Index 0 is unused. */
static const char *const ST_NAME[62] = {
	0,
	":authority", ":method", ":method", ":path", ":path", ":scheme",
	":scheme", ":status", ":status", ":status", ":status", ":status",
	":status", ":status", "accept-charset", "accept-encoding",
	"accept-language", "accept-ranges", "accept",
	"access-control-allow-origin", "age", "allow", "authorization",
	"cache-control", "content-disposition", "content-encoding",
	"content-language", "content-length", "content-location",
	"content-range", "content-type", "cookie", "date", "etag", "expect",
	"expires", "from", "host", "if-match", "if-modified-since",
	"if-none-match", "if-range", "if-unmodified-since", "last-modified",
	"link", "location", "max-forwards", "proxy-authenticate",
	"proxy-authorization", "range", "referer", "refresh", "retry-after",
	"server", "set-cookie", "strict-transport-security",
	"transfer-encoding", "user-agent", "vary", "via", "www-authenticate",
};
static const char *const ST_VAL[62] = {
	0,
	"", "GET", "POST", "/", "/index.html", "http", "https", "200", "204",
	"206", "304", "400", "404", "500", "", "gzip, deflate", "", "", "",
	"", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
	"", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
	"", "", "", "", "", "",
};

/* Dynamic table: entries newest-last in dt[], string bytes in one arena. The
 * HPACK size accounting (name+value+32 per entry) bounds it at dt_max. */
#define DT_ARENA  8192
#define DT_MAXENT 256
#define FRAMECAP  16384

struct dtent { int noff, nlen, voff, vlen; };

/* All of the engine's mutable per-connection state: the HPACK dynamic table,
 * the inbound-frame + reassembled-header buffers, the request-block builder, and
 * the literal-string scratch. Heap-allocated per request (h2_run_body /
 * h2_fetch_many), so any number of h2 connections can run concurrently on
 * different threads — the engine keeps no other mutable global state. */
struct h2_conn {
	unsigned char dt_arena[DT_ARENA];
	struct dtent  dt[DT_MAXENT];
	int           dt_n, dt_top;
	unsigned      dt_size, dt_max;
	unsigned char framebuf[FRAMECAP];    /* current inbound frame payload      */
	unsigned char hblock[FRAMECAP];      /* reassembled response header block  */
	unsigned char reqhb[4096];           /* request header block builder       */
	char          nbuf[512];             /* hpack_decode literal name scratch  */
	char          vbuf[16384];           /* hpack_decode literal value scratch */
};

static void dt_reset(struct h2_conn *c) { c->dt_n = c->dt_top = 0; c->dt_size = 0; c->dt_max = 4096; }

static void dt_evict_oldest(struct h2_conn *c)
{
	if (c->dt_n <= 0)
		return;
	c->dt_size -= (unsigned)(c->dt[0].nlen + c->dt[0].vlen + 32);
	for (int i = 1; i < c->dt_n; i++)
		c->dt[i - 1] = c->dt[i];
	c->dt_n--;
}

static void dt_setmax(struct h2_conn *c, unsigned m)
{
	c->dt_max = m;
	while (c->dt_size > c->dt_max && c->dt_n > 0)
		dt_evict_oldest(c);
}

static void dt_add(struct h2_conn *c, const char *nm, int nl, const char *vl, int vlen)
{
	unsigned need = (unsigned)(nl + vlen + 32);
	if (need > c->dt_max) {              /* too big to ever fit: table empties */
		c->dt_n = c->dt_top = 0; c->dt_size = 0;
		return;
	}
	while (c->dt_size + need > c->dt_max && c->dt_n > 0)
		dt_evict_oldest(c);
	if (c->dt_n >= DT_MAXENT)
		dt_evict_oldest(c);
	if (c->dt_top + nl + vlen > DT_ARENA) { /* compact live bytes to the front */
		int w = 0;
		for (int i = 0; i < c->dt_n; i++) {
			memmove(c->dt_arena + w, c->dt_arena + c->dt[i].noff, c->dt[i].nlen);
			c->dt[i].noff = w; w += c->dt[i].nlen;
			memmove(c->dt_arena + w, c->dt_arena + c->dt[i].voff, c->dt[i].vlen);
			c->dt[i].voff = w; w += c->dt[i].vlen;
		}
		c->dt_top = w;
		if (c->dt_top + nl + vlen > DT_ARENA)
			return;                      /* unreachable with dt_max<=4096 */
	}
	c->dt[c->dt_n].noff = c->dt_top; c->dt[c->dt_n].nlen = nl;
	memcpy(c->dt_arena + c->dt_top, nm, nl); c->dt_top += nl;
	c->dt[c->dt_n].voff = c->dt_top; c->dt[c->dt_n].vlen = vlen;
	memcpy(c->dt_arena + c->dt_top, vl, vlen); c->dt_top += vlen;
	c->dt_n++;
	c->dt_size += need;
}

/* Resolve an HPACK index to its name (and value, for fully-indexed fields). */
static int idx_lookup(struct h2_conn *c, long idx, const char **nm, int *nl,
                      const char **vl, int *vlen)
{
	if (idx >= 1 && idx <= 61) {
		*nm = ST_NAME[idx]; *nl = strlen(ST_NAME[idx]);
		*vl = ST_VAL[idx];  *vlen = strlen(ST_VAL[idx]);
		return 0;
	}
	int pos = (int)(idx - 62);           /* 0 = most recently added */
	if (pos < 0 || pos >= c->dt_n)
		return -1;
	struct dtent *e = &c->dt[c->dt_n - 1 - pos];
	*nm = (const char *)c->dt_arena + e->noff; *nl = e->nlen;
	*vl = (const char *)c->dt_arena + e->voff; *vlen = e->vlen;
	return 0;
}

/* --------------------------------------------------------- HPACK primitives */

/* Decode an N-bit-prefix integer starting at b[*pos]. Returns the value, or -1
 * on a malformed/over-long encoding; advances *pos past the integer. */
static long dec_int(const unsigned char *b, int *pos, int len, int nbits)
{
	if (*pos >= len)
		return -1;
	unsigned mask = (1u << nbits) - 1;
	unsigned v = b[(*pos)++] & mask;
	if (v < mask)
		return (long)v;
	int m = 0;
	for (;;) {
		if (*pos >= len)
			return -1;
		unsigned byte = b[(*pos)++];
		v += (byte & 0x7f) << m;
		if (!(byte & 0x80))
			break;
		m += 7;
		if (m > 28)
			return -1;
	}
	return (long)v;
}

/* Decode an HPACK string (literal or Huffman) at b[*pos] into out[outcap]. */
static int dec_str(const unsigned char *b, int *pos, int len,
                   char *out, int outcap, int *outlen)
{
	if (*pos >= len)
		return -1;
	int huff = b[*pos] & 0x80;
	long n = dec_int(b, pos, len, 7);
	if (n < 0 || *pos + (int)n > len)
		return -1;                              /* genuinely truncated input */
	/* A value/name larger than our buffer (a giant Set-Cookie/CSP) is truncated
	 * and skipped, NOT treated as fatal: we still consume the field so the parser
	 * stays in sync and keeps decoding the rest of the block. Aborting here would
	 * drop every later header — including content-encoding, which broke `br`/gzip
	 * decoding on header-heavy sites like google. */
	if (!huff) {
		int take = (int)n < outcap ? (int)n : outcap;
		memcpy(out, b + *pos, take);
		*outlen = take;
		*pos += (int)n;
		return 0;
	}
	if (huff_decode(b + *pos, (int)n, out, outcap, outlen) < 0)
		*outlen = 0;                            /* undecodable/too big: skip value */
	*pos += (int)n;
	return 0;
}

/* ---------------------------------------------------- header block decoding */

/* Where decoded response headers land: the status code, plus the header lines
 * rebuilt into hdr[] ("HTTP/2 NNN\r\n" then "name: value\r\n"…) for curl -i. */
struct hsink { char *buf; int cap; int len; int status; };

static int mcmp(const char *a, const char *b, int n)
{
	for (int i = 0; i < n; i++)
		if (a[i] != b[i]) return 1;
	return 0;
}

static void hs_raw(struct hsink *s, const char *p, int n)
{
	for (int i = 0; i < n && s->len < s->cap - 1; i++)
		s->buf[s->len++] = p[i];
}

static void hs_emit(struct hsink *s, const char *nm, int nl,
                    const char *vl, int vlen)
{
	if (nl == 7 && mcmp(nm, ":status", 7) == 0) {
		int code = 0;
		for (int i = 0; i < vlen && vl[i] >= '0' && vl[i] <= '9'; i++)
			code = code * 10 + (vl[i] - '0');
		s->status = code;
		hs_raw(s, "HTTP/2 ", 7);
		hs_raw(s, vl, vlen);
		hs_raw(s, "\r\n", 2);
		return;
	}
	if (nl > 0 && nm[0] == ':')               /* other pseudo-headers: drop */
		return;
	hs_raw(s, nm, nl);
	hs_raw(s, ": ", 2);
	hs_raw(s, vl, vlen);
	hs_raw(s, "\r\n", 2);
}

/* Decode one HPACK header block (RFC 7541 §6) into the sink, keeping the
 * dynamic table in sync. Returns 0, or -1 on a malformed block. The literal
 * name/value scratch (c->nbuf/c->vbuf) is big enough for real-world response
 * headers (a long Set-Cookie, CSP, or a data: value); a larger field fails the
 * decode and h2_run then salvages the response if the status line came through. */
static int hpack_decode(struct h2_conn *c, const unsigned char *b, int len, struct hsink *s)
{
	int pos = 0;
	while (pos < len) {
		unsigned char ch = b[pos];
		const char *nm, *vl;
		int nl, vlen;
		if (ch & 0x80) {                           /* indexed header field */
			long idx = dec_int(b, &pos, len, 7);
			if (idx <= 0 || idx_lookup(c, idx, &nm, &nl, &vl, &vlen) < 0)
				return -1;
			hs_emit(s, nm, nl, vl, vlen);
		} else if (ch & 0x40) {                    /* literal, incr. indexing */
			long idx = dec_int(b, &pos, len, 6);
			if (idx < 0)
				return -1;
			if (idx == 0) {
				if (dec_str(b, &pos, len, c->nbuf, sizeof c->nbuf, &nl))
					return -1;
				nm = c->nbuf;
			} else {
				const char *dv; int dvl;
				if (idx_lookup(c, idx, &nm, &nl, &dv, &dvl) < 0)
					return -1;
			}
			if (dec_str(b, &pos, len, c->vbuf, sizeof c->vbuf, &vlen))
				return -1;
			vl = c->vbuf;
			dt_add(c, nm, nl, vl, vlen);
			hs_emit(s, nm, nl, vl, vlen);
		} else if (ch & 0x20) {                    /* dynamic table size update */
			long m = dec_int(b, &pos, len, 5);
			if (m < 0 || m > 4096)     /* never advertised a table larger than the default */
				return -1;
			dt_setmax(c, (unsigned)m);
		} else {                                   /* literal, no indexing */
			long idx = dec_int(b, &pos, len, 4);
			if (idx < 0)
				return -1;
			if (idx == 0) {
				if (dec_str(b, &pos, len, c->nbuf, sizeof c->nbuf, &nl))
					return -1;
				nm = c->nbuf;
			} else {
				const char *dv; int dvl;
				if (idx_lookup(c, idx, &nm, &nl, &dv, &dvl) < 0)
					return -1;
			}
			if (dec_str(b, &pos, len, c->vbuf, sizeof c->vbuf, &vlen))
				return -1;
			vl = c->vbuf;
			hs_emit(s, nm, nl, vl, vlen);
		}
	}
	return 0;
}

/* ----------------------------------------------------- HPACK request coder */

static int enc_int(unsigned char *o, int top, int nbits, unsigned val)
{
	unsigned max = (1u << nbits) - 1;
	if (val < max) {
		o[0] = (unsigned char)(top | val);
		return 1;
	}
	int i = 0;
	o[i++] = (unsigned char)(top | max);
	val -= max;
	while (val >= 128) {
		o[i++] = (unsigned char)((val & 0x7f) | 0x80);
		val >>= 7;
	}
	o[i++] = (unsigned char)val;
	return i;
}

static int enc_str(unsigned char *o, const char *s)
{
	int len = strlen(s);
	int i = enc_int(o, 0, 7, (unsigned)len);   /* H=0: raw literal */
	for (int k = 0; k < len; k++)
		o[i++] = (unsigned char)s[k];
	return i;
}

/* Build the request header block: method/scheme indexed where possible, path
 * and authority as no-index literals on their static names, then the caller's
 * user-agent (if any) and extra_headers. No-index keeps unique values (the path)
 * out of the server's table. */
/* Connection-specific (and pseudo-) headers that HTTP/2 forbids in a normal
 * field block (RFC 7540 §8.1.2.2) — skipped if a caller's extra_headers list
 * them, so a stray "Connection: close" can't make the request malformed. */
static int h2_forbidden_header(const char *name)   /* name already lowercased */
{
	return streq(name, "connection") || streq(name, "keep-alive") ||
	       streq(name, "transfer-encoding") || streq(name, "upgrade") ||
	       streq(name, "proxy-connection") || streq(name, "host");
}

static int build_request(unsigned char *hb, int https, const char *method,
                         const char *authority, const char *path,
                         const struct h2_opts *opts)
{
	int n = 0;
	const char *ua = (opts && opts->user_agent) ? opts->user_agent : 0;

	if (streq(method, "GET"))       hb[n++] = 0x82;   /* :method GET  (idx 2) */
	else if (streq(method, "POST")) hb[n++] = 0x83;   /* :method POST (idx 3) */
	else { hb[n++] = 0x42; n += enc_str(hb + n, method); }  /* lit name idx 2 */

	hb[n++] = https ? 0x87 : 0x86;                    /* :scheme https/http   */

	if (streq(path, "/")) hb[n++] = 0x84;             /* :path /      (idx 4) */
	else { hb[n++] = 0x04; n += enc_str(hb + n, path); }    /* no-index idx 4 */

	hb[n++] = 0x01; n += enc_str(hb + n, authority);  /* :authority (no-index)*/
	if (ua) {                                         /* user-agent (idx 58)  */
		hb[n++] = 0x0f; hb[n++] = 43;
		n += enc_str(hb + n, ua);
	}

	if (opts && opts->extra_headers) {
		/* Each "name: value" line becomes a literal-without-indexing header with
		 * a literal, lowercased name (HTTP/2 mandates lowercase field names) —
		 * carries the browser's Cookie / Accept / Accept-Language. */
		const char *p = opts->extra_headers;
		while (*p) {
			char name[64]; int ni = 0;
			while (*p && *p != ':' && *p != '\r' && *p != '\n') {
				char c = *p++;
				if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
				if (ni < (int)sizeof name - 1) name[ni++] = c;
			}
			name[ni] = '\0';
			if (*p == ':') {
				p++;
				while (*p == ' ') p++;
				char val[1024]; int vi = 0;
				while (*p && *p != '\r' && *p != '\n') {
					if (vi < (int)sizeof val - 1) val[vi++] = *p;
					p++;
				}
				val[vi] = '\0';
				if (ni > 0 && !h2_forbidden_header(name)) {
					hb[n++] = 0x00;               /* literal, no index, new name */
					n += enc_str(hb + n, name);
					n += enc_str(hb + n, val);
				}
			}
			while (*p == '\r' || *p == '\n') p++;
		}
	} else {
		hb[n++] = 0x0f; hb[n++] = 4;              /* accept (idx 19)     */
		n += enc_str(hb + n, "*/*");
	}
	return n;
}

/* ------------------------------------------------------- frame transport */

#define F_DATA   0
#define F_HEADERS 1
#define F_RST    3
#define F_SETTINGS 4
#define F_PUSH   5
#define F_PING   6
#define F_GOAWAY 7
#define F_WINUP  8
#define F_CONT   9

#define FL_END_STREAM  0x01
#define FL_ACK         0x01
#define FL_END_HEADERS 0x04
#define FL_PADDED      0x08
#define FL_PRIORITY    0x20

struct frd { netconn *conn; unsigned char buf[4096]; int pos, len; };

/* send all `n` bytes; 0 ok, -2 transport error, -3 interrupted. */
static int send_all(netconn *conn, const void *p, int n)
{
	const unsigned char *b = p;
	int s = 0;
	while (s < n) {
		int k = nc_write(conn, b + s, n - s);
		if (k == NET_EINTR) return -3;
		if (k <= 0)         return -2;
		s += k;
	}
	return 0;
}

static int put_frame(netconn *conn, int type, int flags, unsigned sid,
                     const unsigned char *pay, int len)
{
	unsigned char h[9];
	h[0] = (unsigned char)(len >> 16); h[1] = (unsigned char)(len >> 8);
	h[2] = (unsigned char)len;
	h[3] = (unsigned char)type; h[4] = (unsigned char)flags;
	h[5] = (unsigned char)((sid >> 24) & 0x7f); h[6] = (unsigned char)(sid >> 16);
	h[7] = (unsigned char)(sid >> 8); h[8] = (unsigned char)sid;
	int rc = send_all(conn, h, 9);
	if (rc) return rc;
	if (len) return send_all(conn, pay, len);
	return 0;
}

/* Fill the reader from the transport. >0 bytes, -2 error/EOF, -3 interrupted. */
static int frd_raw(struct frd *r)
{
	r->pos = 0;
	int n = nc_read(r->conn, r->buf, (int)sizeof r->buf);
	if (n == NET_EINTR) return -3;
	if (n <= 0)         return -2;
	r->len = n;
	return n;
}

static int frd_get(struct frd *r, unsigned char *dst, int n)
{
	int got = 0;
	while (got < n) {
		if (r->pos >= r->len) {
			int rc = frd_raw(r);
			if (rc < 0) return rc;
		}
		int take = r->len - r->pos;
		if (take > n - got) take = n - got;
		memcpy(dst + got, r->buf + r->pos, take);
		r->pos += take;
		got += take;
	}
	return 0;
}

static int read_frame(struct frd *r, int *type, int *flags, unsigned *sid,
                      unsigned char *pay, int *paylen)
{
	unsigned char h[9];
	int rc = frd_get(r, h, 9);
	if (rc < 0) return rc;
	int len = (h[0] << 16) | (h[1] << 8) | h[2];
	*type = h[3]; *flags = h[4];
	*sid = ((unsigned)(h[5] & 0x7f) << 24) | ((unsigned)h[6] << 16) |
	       ((unsigned)h[7] << 8) | h[8];
	if (len > FRAMECAP)
		return -2;                           /* peer ignored our frame size */
	rc = frd_get(r, pay, len);
	if (rc < 0) return rc;
	*paylen = len;
	return 0;
}

/* Gather a full header block: strip the first HEADERS frame's pad/priority,
 * then append CONTINUATION frames until END_HEADERS. Returns 0/-2/-3. */
static int collect_header_block(struct h2_conn *c, struct frd *r, const unsigned char *firstpay,
                                int firstlen, int firstflags, int *hblen)
{
	int start = 0, padlen = 0;
	if (firstflags & FL_PADDED)   padlen = firstpay[start++];
	if (firstflags & FL_PRIORITY) start += 5;
	int frag = firstlen - start - padlen;
	if (frag < 0 || frag > FRAMECAP)
		return -2;
	memcpy(c->hblock, firstpay + start, frag);
	int o = frag, flags = firstflags, conts = 0;
	while (!(flags & FL_END_HEADERS)) {
		int t, fl, pl; unsigned sid;
		int rc = read_frame(r, &t, &fl, &sid, c->framebuf, &pl);
		if (rc < 0) return rc;
		if (t != F_CONT) return -2;
		if (++conts > 64) return -2;   /* CONTINUATION flood: END_HEADERS never arrives */
		if (o + pl > FRAMECAP) return -2;
		memcpy(c->hblock + o, c->framebuf, pl);
		o += pl; flags = fl;
	}
	*hblen = o;
	return 0;
}

/* --------------------------------------------------------- body emitter */

struct emitter { http_body_fn fn; void *ctx; unsigned char buf[2048]; int len; int aborted; };

static void emit_flush(struct emitter *e)
{
	if (e->len && e->fn && !e->aborted)
		if (e->fn(e->ctx, e->buf, e->len) != 0)
			e->aborted = 1;
	e->len = 0;
}

static void emit(struct emitter *e, const unsigned char *p, int n)
{
	for (int i = 0; i < n; i++) {
		if (e->len == (int)sizeof e->buf)
			emit_flush(e);
		e->buf[e->len++] = p[i];
	}
}

/* --------------------------------------------------------------- request */

static const unsigned char PREFACE[24] =
	"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

/* Send as much of the request body as the stream (*swnd) and connection (*cwnd)
 * send-windows allow, in DATA frames split to the max frame size (END_STREAM on the
 * final byte). Returns 0, or -1 on a send error. Called after HEADERS and again
 * whenever a WINDOW_UPDATE/SETTINGS opens the window. */
static int h2_send_body(netconn *conn, const unsigned char *reqbody, int reqlen,
                        int *boff, long *swnd, long *cwnd)
{
	while (*boff < reqlen && *swnd > 0 && *cwnd > 0) {
		long room = *swnd < *cwnd ? *swnd : *cwnd;
		int chunk = reqlen - *boff;
		if (chunk > 16384) chunk = 16384;
		if (chunk > room) chunk = (int)room;
		int last = (*boff + chunk >= reqlen);
		if (put_frame(conn, F_DATA, last ? FL_END_STREAM : 0, 1, reqbody + *boff, chunk) < 0) return -1;
		*boff += chunk; *swnd -= chunk; *cwnd -= chunk;
	}
	return 0;
}

int h2_run_body(netconn *conn, int scheme_https, const char *authority,
                const char *method, const char *path, const struct h2_opts *opts,
                const unsigned char *reqbody, int reqlen,
                char *hdr, int hdrcap, int *hdrlen,
                http_body_fn body, void *ctx)
{
	struct h2_conn *c = malloc(sizeof *c);
	if (!c) return HTTP_ENORESP;
	dt_reset(c);
	if (!method) method = "GET";

	/* Preface + our SETTINGS (no push; a wide stream window) + a connection
	 * WINDOW_UPDATE so the whole response flows without per-DATA updates. */
	if (send_all(conn, PREFACE, 24) < 0)
		{ free(c); return HTTP_ESEND; }
	unsigned char st[12] = {
		0x00, 0x02, 0x00, 0x00, 0x00, 0x00,        /* ENABLE_PUSH = 0 */
		0x00, 0x04, 0x7f, 0xff, 0xff, 0xff,        /* INITIAL_WINDOW_SIZE max */
	};
	if (put_frame(conn, F_SETTINGS, 0, 0, st, 12) < 0)
		{ free(c); return HTTP_ESEND; }
	unsigned char wu[4] = { 0x7f, 0xff, 0x00, 0x00 };  /* +~2 GiB conn window */
	if (put_frame(conn, F_WINUP, 0, 0, wu, 4) < 0)
		{ free(c); return HTTP_ESEND; }

	int has_body = reqbody && reqlen > 0;
	int rl = build_request(c->reqhb, scheme_https, method, authority, path, opts);
	/* When a request body follows, the HEADERS frame must NOT end the stream; the
	 * body rides in flow-controlled DATA frames sent below (and replenished from the
	 * peer's SETTINGS/WINDOW_UPDATE in the loop), so a body of any size streams. */
	if (put_frame(conn, F_HEADERS, FL_END_HEADERS | (has_body ? 0 : FL_END_STREAM), 1, c->reqhb, rl) < 0)
		{ free(c); return HTTP_ESEND; }

	/* Request-body flow control. Stream + connection send-windows start at the h2
	 * default 65535 and grow via the peer's SETTINGS(INITIAL_WINDOW_SIZE) and
	 * WINDOW_UPDATE; h2_send_body emits DATA up to the smaller window, splitting at
	 * the max frame size, END_STREAM on the last byte. */
	int boff = 0;
	long swnd = 65535, cwnd = 65535, applied_iws = 65535;
	if (has_body && h2_send_body(conn, reqbody, reqlen, &boff, &swnd, &cwnd) < 0)
		{ free(c); return HTTP_ESEND; }

	struct frd r; r.conn = conn; r.pos = r.len = 0;
	struct hsink hs; hs.buf = hdr; hs.cap = (hdr ? hdrcap : 0); hs.len = 0; hs.status = 0;
	struct emitter e; e.fn = body; e.ctx = ctx; e.len = 0; e.aborted = 0;

	int done = 0, headers_done = 0, hpack_broken = 0;
	int rc = HTTP_ENORESP;

	while (!done) {
		int type, flags, paylen; unsigned sid;
		int fr = read_frame(&r, &type, &flags, &sid, c->framebuf, &paylen);
		if (fr == -3) { rc = HTTP_EINTR;  goto out; }
		if (fr < 0) {
			/* Connection closed/errored. If the response headers already arrived,
			 * take it as the end of the response (many servers — google among
			 * them — just close after sending, especially on a redirect, without a
			 * clean END_STREAM) rather than failing the whole request. Mirrors
			 * HTTP/1.1's read-to-close. */
			if (headers_done) break;
			rc = HTTP_ENORESP; goto out;
		}

		switch (type) {
		case F_SETTINGS:
			if (!(flags & FL_ACK)) {
				int i;                            /* apply INITIAL_WINDOW_SIZE to our send window */
				for (i = 0; i + 6 <= paylen; i += 6)
					if (((c->framebuf[i] << 8) | c->framebuf[i + 1]) == 0x0004) {
						long niw = ((long)c->framebuf[i + 2] << 24) | (c->framebuf[i + 3] << 16) | (c->framebuf[i + 4] << 8) | c->framebuf[i + 5];
						swnd += niw - applied_iws; applied_iws = niw;
					}
				if (put_frame(conn, F_SETTINGS, FL_ACK, 0, 0, 0) < 0) { rc = HTTP_ENORESP; goto out; }
				if (has_body && h2_send_body(conn, reqbody, reqlen, &boff, &swnd, &cwnd) < 0) { rc = HTTP_ESEND; goto out; }
			}
			break;
		case F_PING:
			if (!(flags & FL_ACK))
				if (put_frame(conn, F_PING, FL_ACK, 0, c->framebuf, paylen) < 0)
					{ rc = HTTP_ENORESP; goto out; }
			break;
		case F_HEADERS:
			if (sid == 1) {
				/* Once the dynamic table is desynced (a salvaged header block, see
				 * below) we can't decode any further HEADERS — but a trailer is
				 * optional, so just note END_STREAM and keep going for the body. */
				if (hpack_broken) { if (flags & FL_END_STREAM) done = 1; break; }
				int hbl;
				int cr = collect_header_block(c, &r, c->framebuf, paylen, flags, &hbl);
				if (cr == -3) { rc = HTTP_EINTR;  goto out; }
				if (cr < 0)   { rc = HTTP_ENORESP; goto out; }
				if (hpack_decode(c, c->hblock, hbl, &hs) < 0) {
					/* A header block we couldn't fully decode (HPACK state we don't
					 * model). Oversized fields are no longer fatal — dec_str
					 * truncates and keeps decoding, so a giant Set-Cookie/CSP no
					 * longer drops every later header (that silently lost
					 * content-encoding and rendered google's gzip body raw). If the
					 * status line already came through, salvage the response: keep
					 * the status + decoded headers and KEEP READING — DATA frames
					 * carry the body and use no HPACK. A failure before any status
					 * is genuinely fatal. */
					if (hs.status <= 0) { rc = HTTP_ENORESP; goto out; }
					hpack_broken = 1;
					headers_done = 1;
					if (opts && opts->on_headers && hs.buf && hs.cap > 0) {
						hs.buf[hs.len < hs.cap ? hs.len : hs.cap - 1] = '\0';
						opts->on_headers(ctx, hs.buf, hs.len);
					}
					if (flags & FL_END_STREAM) done = 1;
					break;
				}
				/* A 1xx interim response (e.g. 103 Early Hints) precedes the final
				 * one: hand it to the caller, reset the sink, and keep reading so the
				 * next HEADERS block decodes clean. */
				if (hs.status >= 100 && hs.status < 200) {
					if (opts && opts->on_headers && hs.buf && hs.cap > 0) {
						hs.buf[hs.len < hs.cap ? hs.len : hs.cap - 1] = '\0';
						opts->on_headers(ctx, hs.buf, hs.len);
					}
					hs.len = 0; hs.status = 0;
					break;
				}
				headers_done = 1;
				/* Hand the decoded header block to the caller before any DATA,
				 * so it can pick a body sink (browser: render vs download). */
				if (opts && opts->on_headers && hs.buf && hs.cap > 0) {
					hs.buf[hs.len < hs.cap ? hs.len : hs.cap - 1] = '\0';
					if (opts->on_headers(ctx, hs.buf, hs.len) != 0) {
						rc = HTTP_EABORT; goto out;
					}
				}
				if (flags & FL_END_STREAM) done = 1;
			}
			break;
		case F_DATA:
			if (sid == 1) {
				int start = 0, padlen = 0;
				if (flags & FL_PADDED) padlen = c->framebuf[start++];
				int dlen = paylen - start - padlen;
				if (dlen > 0) emit(&e, c->framebuf + start, dlen);
				if (e.aborted) { rc = HTTP_EABORT; goto out; }
				if (flags & FL_END_STREAM) done = 1;
			}
			break;
		case F_RST:
			/* A reset after the full response (some servers RST a stream they
			 * consider done, e.g. after a redirect) isn't a failure — keep what we
			 * have. Only a reset before any headers is a real error. */
			if (sid == 1) {
				if (headers_done) { done = 1; break; }
				rc = HTTP_ENORESP; goto out;
			}
			break;
		case F_GOAWAY:
			if (!headers_done) { rc = HTTP_ENORESP; goto out; }
			done = 1;
			break;
		case F_WINUP:                              /* peer opened the send window */
			if (has_body && boff < reqlen && paylen >= 4) {
				long inc = ((long)(c->framebuf[0] & 0x7f) << 24) | (c->framebuf[1] << 16) | (c->framebuf[2] << 8) | c->framebuf[3];
				if (sid == 0) cwnd += inc; else if (sid == 1) swnd += inc;
				if (h2_send_body(conn, reqbody, reqlen, &boff, &swnd, &cwnd) < 0) { rc = HTTP_ESEND; goto out; }
			}
			break;
		default:                                   /* PRIORITY, PUSH_PROMISE: ignore */
			break;
		}
	}

	if (headers_done && hs.status > 0)
		rc = hs.status;

out:
	emit_flush(&e);
	/* Once we have a valid response (decoded headers + status), a transport error
	 * AFTER it — a stream/connection reset, an unexpected trailer, a peer that
	 * just hangs up — must not turn the whole request into "no response". google
	 * does exactly this after a redirect; return the status with whatever body
	 * arrived. A user interrupt (EINTR) or a caller abort still wins. */
	if (rc == HTTP_ENORESP && headers_done && hs.status > 0)
		rc = hs.status;
	if (hdr && hdrcap > 0) {
		if (hs.len + 2 < hdrcap) {              /* room for CRLF *and* the NUL */
			hdr[hs.len++] = '\r'; hdr[hs.len++] = '\n';
		}
		hdr[hs.len] = '\0';
		if (hdrlen) *hdrlen = hs.len;
	}
	free(c);
	return rc;
}

int h2_run(netconn *conn, int scheme_https, const char *authority,
           const char *method, const char *path, const struct h2_opts *opts,
           char *hdr, int hdrcap, int *hdrlen, http_body_fn body, void *ctx)
{
	return h2_run_body(conn, scheme_https, authority, method, path, opts,
	                   0, 0, hdr, hdrcap, hdrlen, body, ctx);
}

int h2_request(netconn *conn, int scheme_https, const char *authority,
               const char *method, const char *path,
               char *hdr, int hdrcap, int *hdrlen,
               http_body_fn body, void *ctx)
{
	return h2_run(conn, scheme_https, authority, method, path, 0,
	              hdr, hdrcap, hdrlen, body, ctx);
}

/* ----------------------------------------------------- multiplexed batch */

#define H2M_BODYCAP_MAX (16 * 1024 * 1024)   /* per-resource body cap */

/* Per-stream accumulation state for a batched fetch. */
struct h2_mstream { unsigned char *body; int blen, bcap; int status, done, failed; };

static int h2m_grow(struct h2_mstream *s, int need)
{
	if (need <= s->bcap) return 1;
	int nc = s->bcap ? s->bcap : 8192;
	while (nc < need) nc += nc / 2 + 1;
	if (nc > H2M_BODYCAP_MAX) nc = H2M_BODYCAP_MAX;
	if (need > nc) return 0;
	unsigned char *p = realloc(s->body, (unsigned)nc);
	if (!p) return 0;
	s->body = p; s->bcap = nc;
	return 1;
}

/* Map a stream id to its request index (client streams are odd: 1,3,5,…). */
static int h2m_idx(unsigned sid, int nreq)
{
	if (sid < 1 || !(sid & 1)) return -1;
	int i = (int)((sid - 1) / 2);
	return (i < nreq) ? i : -1;
}

int h2_fetch_many(netconn *conn, int scheme_https, const char *authority,
                  const struct h2_opts *opts, struct h2_fetch *reqs, int nreq,
                  void (*on_done)(void *ctx, int i, const unsigned char *body, int blen),
                  void *cb_ctx)
{
	if (nreq <= 0 || nreq > H2_FETCH_MAX) return HTTP_ENORESP;
	struct h2_conn *c = malloc(sizeof *c);
	if (!c) return HTTP_ENORESP;
	dt_reset(c);

	if (send_all(conn, PREFACE, 24) < 0) { free(c); return HTTP_ESEND; }
	unsigned char st[12] = {
		0x00, 0x02, 0x00, 0x00, 0x00, 0x00,        /* ENABLE_PUSH = 0          */
		0x00, 0x04, 0x7f, 0xff, 0xff, 0xff,        /* INITIAL_WINDOW_SIZE max  */
	};
	if (put_frame(conn, F_SETTINGS, 0, 0, st, 12) < 0) { free(c); return HTTP_ESEND; }
	unsigned char wu[4] = { 0x7f, 0xff, 0x00, 0x00 };  /* +~2 GiB conn window */
	if (put_frame(conn, F_WINUP, 0, 0, wu, 4) < 0) { free(c); return HTTP_ESEND; }

	struct h2_mstream sm[H2_FETCH_MAX];
	for (int i = 0; i < nreq; i++) {
		sm[i].body = 0; sm[i].blen = sm[i].bcap = 0;
		sm[i].status = 0; sm[i].done = 0; sm[i].failed = 0;
	}

	/* Send every request's HEADERS up front (END_STREAM: no request body), each
	 * on its own odd stream id. The server interleaves the responses. */
	for (int i = 0; i < nreq; i++) {
		unsigned sid = 1u + 2u * (unsigned)i;
		int rl = build_request(c->reqhb, scheme_https, "GET", authority, reqs[i].path, opts);
		if (put_frame(conn, F_HEADERS, FL_END_HEADERS | FL_END_STREAM, sid, c->reqhb, rl) < 0)
			{ free(c); return HTTP_ESEND; }
	}

	struct frd r; r.conn = conn; r.pos = r.len = 0;
	char statusbuf[64];
	int ndone = 0, rc = 0;

	while (ndone < nreq) {
		int type, flags, paylen; unsigned sid;
		int fr = read_frame(&r, &type, &flags, &sid, c->framebuf, &paylen);
		if (fr < 0) { rc = (fr == -3) ? HTTP_EINTR : HTTP_ENORESP; goto out; }

		switch (type) {
		case F_SETTINGS:
			if (!(flags & FL_ACK))
				if (put_frame(conn, F_SETTINGS, FL_ACK, 0, 0, 0) < 0) { rc = HTTP_ENORESP; goto out; }
			break;
		case F_PING:
			if (!(flags & FL_ACK))
				if (put_frame(conn, F_PING, FL_ACK, 0, c->framebuf, paylen) < 0) { rc = HTTP_ENORESP; goto out; }
			break;
		case F_HEADERS: {
			int hbl;
			int cr = collect_header_block(c, &r, c->framebuf, paylen, flags, &hbl);
			if (cr < 0) { rc = (cr == -3) ? HTTP_EINTR : HTTP_ENORESP; goto out; }
			/* HPACK is connection-scoped: every block must decode in wire order,
			 * even for a stream we don't track — statusbuf is a throwaway sink.
			 * For a tracked stream whose caller wants the headers, decode straight
			 * into its buffer (first HEADERS only; a later trailer block keeps the
			 * throwaway sink so it can't clobber the captured response headers). */
			int idx = h2m_idx(sid, nreq);
			struct hsink hs;
			int capture = idx >= 0 && reqs[idx].hdr && reqs[idx].hdrcap > 0 && reqs[idx].hdrlen == 0;
			if (capture) { hs.buf = reqs[idx].hdr; hs.cap = reqs[idx].hdrcap; }
			else         { hs.buf = statusbuf;     hs.cap = sizeof statusbuf; }
			hs.len = 0; hs.status = 0;
			if (hpack_decode(c, c->hblock, hbl, &hs) < 0) { rc = HTTP_ENORESP; goto out; }
			if (capture) { hs.buf[hs.len] = '\0'; reqs[idx].hdrlen = hs.len; }
			if (idx >= 0 && !sm[idx].done) {
				sm[idx].status = hs.status;
				if (flags & FL_END_STREAM) {
					sm[idx].done = 1; ndone++;
					if (on_done && sm[idx].status == 200 && sm[idx].body && sm[idx].blen > 0)
						on_done(cb_ctx, idx, sm[idx].body, sm[idx].blen);
				}
			}
			break; }
		case F_DATA: {
			int idx = h2m_idx(sid, nreq);
			int start = 0, padlen = 0;
			if (flags & FL_PADDED) padlen = c->framebuf[start++];
			int dlen = paylen - start - padlen;
			if (idx >= 0 && !sm[idx].done) {
				if (dlen > 0) {
					if (!h2m_grow(&sm[idx], sm[idx].blen + dlen)) sm[idx].failed = 1;
					else { memcpy(sm[idx].body + sm[idx].blen, c->framebuf + start, dlen);
					       sm[idx].blen += dlen; }
				}
				if (flags & FL_END_STREAM) {
					sm[idx].done = 1; ndone++;
					/* stream finished: hand the body to the caller now, so an image
					 * shows the moment it lands instead of after the whole batch. */
					if (on_done && sm[idx].status == 200 && sm[idx].body && sm[idx].blen > 0)
						on_done(cb_ctx, idx, sm[idx].body, sm[idx].blen);
				}
			}
			break; }
		case F_RST: {
			int idx = h2m_idx(sid, nreq);     /* refused/reset: caller retries h1.1 */
			if (idx >= 0 && !sm[idx].done) { sm[idx].failed = 1; sm[idx].done = 1; ndone++; }
			break; }
		case F_GOAWAY:
			rc = HTTP_ENORESP; goto out;       /* connection ending: bail, rest fail */
		default:                                /* WINDOW_UPDATE, PRIORITY, PUSH: ignore */
			break;
		}
	}

out:
	for (int i = 0; i < nreq; i++) {
		if (on_done) {                     /* engine owns the bodies in this mode */
			free(sm[i].body);
			reqs[i].body = 0; reqs[i].blen = 0;
		} else {
			reqs[i].body = sm[i].body;
			reqs[i].blen = sm[i].blen;
		}
		reqs[i].status = sm[i].status;
		reqs[i].failed = sm[i].failed || (!sm[i].done) || sm[i].status != 200;
	}
	free(c);
	return rc;
}
