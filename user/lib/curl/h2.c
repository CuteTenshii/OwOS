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
static unsigned char dt_arena[DT_ARENA];
static struct dtent { int noff, nlen, voff, vlen; } dt[DT_MAXENT];
static int      dt_n, dt_top;
static unsigned dt_size, dt_max;

static void dt_reset(void) { dt_n = dt_top = 0; dt_size = 0; dt_max = 4096; }

static void dt_evict_oldest(void)
{
	if (dt_n <= 0)
		return;
	dt_size -= (unsigned)(dt[0].nlen + dt[0].vlen + 32);
	for (int i = 1; i < dt_n; i++)
		dt[i - 1] = dt[i];
	dt_n--;
}

static void dt_setmax(unsigned m)
{
	dt_max = m;
	while (dt_size > dt_max && dt_n > 0)
		dt_evict_oldest();
}

static void dt_add(const char *nm, int nl, const char *vl, int vlen)
{
	unsigned need = (unsigned)(nl + vlen + 32);
	if (need > dt_max) {                 /* too big to ever fit: table empties */
		dt_n = dt_top = 0; dt_size = 0;
		return;
	}
	while (dt_size + need > dt_max && dt_n > 0)
		dt_evict_oldest();
	if (dt_n >= DT_MAXENT)
		dt_evict_oldest();
	if (dt_top + nl + vlen > DT_ARENA) { /* compact live bytes to the front */
		int w = 0;
		for (int i = 0; i < dt_n; i++) {
			memmove(dt_arena + w, dt_arena + dt[i].noff, dt[i].nlen);
			dt[i].noff = w; w += dt[i].nlen;
			memmove(dt_arena + w, dt_arena + dt[i].voff, dt[i].vlen);
			dt[i].voff = w; w += dt[i].vlen;
		}
		dt_top = w;
		if (dt_top + nl + vlen > DT_ARENA)
			return;                      /* unreachable with dt_max<=4096 */
	}
	dt[dt_n].noff = dt_top; dt[dt_n].nlen = nl;
	memcpy(dt_arena + dt_top, nm, nl); dt_top += nl;
	dt[dt_n].voff = dt_top; dt[dt_n].vlen = vlen;
	memcpy(dt_arena + dt_top, vl, vlen); dt_top += vlen;
	dt_n++;
	dt_size += need;
}

/* Resolve an HPACK index to its name (and value, for fully-indexed fields). */
static int idx_lookup(long idx, const char **nm, int *nl,
                      const char **vl, int *vlen)
{
	if (idx >= 1 && idx <= 61) {
		*nm = ST_NAME[idx]; *nl = strlen(ST_NAME[idx]);
		*vl = ST_VAL[idx];  *vlen = strlen(ST_VAL[idx]);
		return 0;
	}
	int pos = (int)(idx - 62);           /* 0 = most recently added */
	if (pos < 0 || pos >= dt_n)
		return -1;
	struct dtent *e = &dt[dt_n - 1 - pos];
	*nm = (const char *)dt_arena + e->noff; *nl = e->nlen;
	*vl = (const char *)dt_arena + e->voff; *vlen = e->vlen;
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
		return -1;
	if (!huff) {
		if ((int)n > outcap)
			return -1;
		memcpy(out, b + *pos, (int)n);
		*outlen = (int)n;
		*pos += (int)n;
		return 0;
	}
	int rc = huff_decode(b + *pos, (int)n, out, outcap, outlen);
	*pos += (int)n;
	return rc;
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
 * dynamic table in sync. Returns 0, or -1 on a malformed block. */
static int hpack_decode(const unsigned char *b, int len, struct hsink *s)
{
	static char nbuf[256], vbuf[2048];
	int pos = 0;
	while (pos < len) {
		unsigned char c = b[pos];
		const char *nm, *vl;
		int nl, vlen;
		if (c & 0x80) {                            /* indexed header field */
			long idx = dec_int(b, &pos, len, 7);
			if (idx <= 0 || idx_lookup(idx, &nm, &nl, &vl, &vlen) < 0)
				return -1;
			hs_emit(s, nm, nl, vl, vlen);
		} else if (c & 0x40) {                     /* literal, incr. indexing */
			long idx = dec_int(b, &pos, len, 6);
			if (idx < 0)
				return -1;
			if (idx == 0) {
				if (dec_str(b, &pos, len, nbuf, sizeof nbuf, &nl))
					return -1;
				nm = nbuf;
			} else {
				const char *dv; int dvl;
				if (idx_lookup(idx, &nm, &nl, &dv, &dvl) < 0)
					return -1;
			}
			if (dec_str(b, &pos, len, vbuf, sizeof vbuf, &vlen))
				return -1;
			vl = vbuf;
			dt_add(nm, nl, vl, vlen);
			hs_emit(s, nm, nl, vl, vlen);
		} else if (c & 0x20) {                     /* dynamic table size update */
			long m = dec_int(b, &pos, len, 5);
			if (m < 0)
				return -1;
			dt_setmax((unsigned)m);
		} else {                                   /* literal, no indexing */
			long idx = dec_int(b, &pos, len, 4);
			if (idx < 0)
				return -1;
			if (idx == 0) {
				if (dec_str(b, &pos, len, nbuf, sizeof nbuf, &nl))
					return -1;
				nm = nbuf;
			} else {
				const char *dv; int dvl;
				if (idx_lookup(idx, &nm, &nl, &dv, &dvl) < 0)
					return -1;
			}
			if (dec_str(b, &pos, len, vbuf, sizeof vbuf, &vlen))
				return -1;
			vl = vbuf;
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
 * and authority as no-index literals on their static names, plus user-agent and
 * accept. No-index keeps unique values (the path) out of the server's table. */
static int build_request(unsigned char *hb, int https, const char *method,
                         const char *authority, const char *path)
{
	int n = 0;
	if (streq(method, "GET"))       hb[n++] = 0x82;   /* :method GET  (idx 2) */
	else if (streq(method, "POST")) hb[n++] = 0x83;   /* :method POST (idx 3) */
	else { hb[n++] = 0x42; n += enc_str(hb + n, method); }  /* lit name idx 2 */

	hb[n++] = https ? 0x87 : 0x86;                    /* :scheme https/http   */

	if (streq(path, "/")) hb[n++] = 0x84;             /* :path /      (idx 4) */
	else { hb[n++] = 0x04; n += enc_str(hb + n, path); }    /* no-index idx 4 */

	hb[n++] = 0x01; n += enc_str(hb + n, authority);  /* :authority (no-index)*/
	hb[n++] = 0x0f; hb[n++] = 43;                     /* user-agent (idx 58)  */
	n += enc_str(hb + n, "owos-curl");
	hb[n++] = 0x0f; hb[n++] = 4;                       /* accept (idx 19)     */
	n += enc_str(hb + n, "*/*");
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

#define FRAMECAP 16384

static unsigned char framebuf[FRAMECAP];     /* current inbound frame payload */
static unsigned char hblock[FRAMECAP];       /* reassembled response header block */

struct frd { int sock; unsigned char buf[4096]; int pos, len; };

/* send all `n` bytes; 0 ok, -2 transport error, -3 interrupted. */
static int send_all(int sock, const void *p, int n)
{
	const unsigned char *b = p;
	int s = 0;
	while (s < n) {
		int k = net_send(sock, b + s, n - s);
		if (k == NET_EINTR) return -3;
		if (k <= 0)         return -2;
		s += k;
	}
	return 0;
}

static int put_frame(int sock, int type, int flags, unsigned sid,
                     const unsigned char *pay, int len)
{
	unsigned char h[9];
	h[0] = (unsigned char)(len >> 16); h[1] = (unsigned char)(len >> 8);
	h[2] = (unsigned char)len;
	h[3] = (unsigned char)type; h[4] = (unsigned char)flags;
	h[5] = (unsigned char)((sid >> 24) & 0x7f); h[6] = (unsigned char)(sid >> 16);
	h[7] = (unsigned char)(sid >> 8); h[8] = (unsigned char)sid;
	int rc = send_all(sock, h, 9);
	if (rc) return rc;
	if (len) return send_all(sock, pay, len);
	return 0;
}

/* Fill the reader from the socket. >0 bytes, -2 error/EOF, -3 interrupted. */
static int frd_raw(struct frd *r)
{
	r->pos = 0;
	int n = net_recv(r->sock, r->buf, (int)sizeof r->buf);
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
static int collect_header_block(struct frd *r, const unsigned char *firstpay,
                                int firstlen, int firstflags, int *hblen)
{
	int start = 0, padlen = 0;
	if (firstflags & FL_PADDED)   padlen = firstpay[start++];
	if (firstflags & FL_PRIORITY) start += 5;
	int frag = firstlen - start - padlen;
	if (frag < 0 || frag > FRAMECAP)
		return -2;
	memcpy(hblock, firstpay + start, frag);
	int o = frag, flags = firstflags;
	while (!(flags & FL_END_HEADERS)) {
		int t, fl, pl; unsigned sid;
		int rc = read_frame(r, &t, &fl, &sid, framebuf, &pl);
		if (rc < 0) return rc;
		if (t != F_CONT) return -2;
		if (o + pl > FRAMECAP) return -2;
		memcpy(hblock + o, framebuf, pl);
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

int h2_request(int sock, int scheme_https, const char *authority,
               const char *method, const char *path,
               char *hdr, int hdrcap, int *hdrlen,
               http_body_fn body, void *ctx)
{
	dt_reset();
	if (!method) method = "GET";

	/* Preface + our SETTINGS (no push; a wide stream window) + a connection
	 * WINDOW_UPDATE so the whole response flows without per-DATA updates. */
	if (send_all(sock, PREFACE, 24) < 0)
		return HTTP_ESEND;
	unsigned char st[12] = {
		0x00, 0x02, 0x00, 0x00, 0x00, 0x00,        /* ENABLE_PUSH = 0 */
		0x00, 0x04, 0x7f, 0xff, 0xff, 0xff,        /* INITIAL_WINDOW_SIZE max */
	};
	if (put_frame(sock, F_SETTINGS, 0, 0, st, 12) < 0)
		return HTTP_ESEND;
	unsigned char wu[4] = { 0x7f, 0xff, 0x00, 0x00 };  /* +~2 GiB conn window */
	if (put_frame(sock, F_WINUP, 0, 0, wu, 4) < 0)
		return HTTP_ESEND;

	static unsigned char reqhb[1024];
	int rl = build_request(reqhb, scheme_https, method, authority, path);
	if (put_frame(sock, F_HEADERS, FL_END_HEADERS | FL_END_STREAM, 1, reqhb, rl) < 0)
		return HTTP_ESEND;

	struct frd r; r.sock = sock; r.pos = r.len = 0;
	struct hsink hs; hs.buf = hdr; hs.cap = (hdr ? hdrcap : 0); hs.len = 0; hs.status = 0;
	struct emitter e; e.fn = body; e.ctx = ctx; e.len = 0; e.aborted = 0;

	int done = 0, headers_done = 0;
	int rc = HTTP_ENORESP;

	while (!done) {
		int type, flags, paylen; unsigned sid;
		int fr = read_frame(&r, &type, &flags, &sid, framebuf, &paylen);
		if (fr == -3) { rc = HTTP_EINTR;  goto out; }
		if (fr < 0)   { rc = HTTP_ENORESP; goto out; }

		switch (type) {
		case F_SETTINGS:
			if (!(flags & FL_ACK))
				if (put_frame(sock, F_SETTINGS, FL_ACK, 0, 0, 0) < 0)
					{ rc = HTTP_ENORESP; goto out; }
			break;
		case F_PING:
			if (!(flags & FL_ACK))
				if (put_frame(sock, F_PING, FL_ACK, 0, framebuf, paylen) < 0)
					{ rc = HTTP_ENORESP; goto out; }
			break;
		case F_HEADERS:
			if (sid == 1) {
				int hbl;
				int cr = collect_header_block(&r, framebuf, paylen, flags, &hbl);
				if (cr == -3) { rc = HTTP_EINTR;  goto out; }
				if (cr < 0)   { rc = HTTP_ENORESP; goto out; }
				if (hpack_decode(hblock, hbl, &hs) < 0) { rc = HTTP_ENORESP; goto out; }
				headers_done = 1;
				if (flags & FL_END_STREAM) done = 1;
			}
			break;
		case F_DATA:
			if (sid == 1) {
				int start = 0, padlen = 0;
				if (flags & FL_PADDED) padlen = framebuf[start++];
				int dlen = paylen - start - padlen;
				if (dlen > 0) emit(&e, framebuf + start, dlen);
				if (e.aborted) { rc = HTTP_EABORT; goto out; }
				if (flags & FL_END_STREAM) done = 1;
			}
			break;
		case F_RST:
			if (sid == 1) { rc = HTTP_ENORESP; goto out; }
			break;
		case F_GOAWAY:
			if (!headers_done) { rc = HTTP_ENORESP; goto out; }
			done = 1;
			break;
		default:                                   /* WINDOW_UPDATE, PRIORITY,
		                                            * PUSH_PROMISE: ignore */
			break;
		}
	}

	if (headers_done && hs.status > 0)
		rc = hs.status;

out:
	emit_flush(&e);
	if (hdr && hdrcap > 0) {
		if (hs.len < hdrcap - 1) {              /* terminate the header block */
			hdr[hs.len++] = '\r'; hdr[hs.len++] = '\n';
		}
		hdr[hs.len] = '\0';
		if (hdrlen) *hdrlen = hs.len;
	}
	return rc;
}
