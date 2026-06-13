/* png.c — minimal PNG decoder (see png.h for scope).
 *
 * Pipeline: verify signature -> walk chunks, capturing IHDR and concatenating
 * IDAT -> zlib_inflate() the IDAT blob into raw filtered scanlines -> reverse
 * the per-row filters in place -> pack each pixel into XRGB8888.
 *
 * The inflate engine lives in gzip.c; this file is just the PNG envelope plus
 * the un-filtering and colour packing that are specific to PNG.
 */
#include "png.h"
#include "endian.h"
#include "gzip.h"
#include "heap.h"
#include "fs.h"
#include "kstring.h"
#include <stddef.h>

static const uint8_t PNG_SIG[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };

/* Paeth predictor (RFC 2083 / PNG spec): pick whichever of a (left), b (up),
 * c (up-left) is closest to the linear estimate a+b-c. */
static uint8_t paeth(int a, int b, int c)
{
	int p  = a + b - c;
	int pa = p > a ? p - a : a - p;
	int pb = p > b ? p - b : b - p;
	int pc = p > c ? p - c : c - p;
	if (pa <= pb && pa <= pc)
		return (uint8_t)a;
	if (pb <= pc)
		return (uint8_t)b;
	return (uint8_t)c;
}

/* Reverse the five PNG scanline filters in place. `raw` holds height rows, each
 * prefixed by a 1-byte filter tag followed by `stride` filtered bytes. bpp is
 * the per-pixel byte count (used as the "distance to the left pixel"). After
 * this returns, the filter tags are still present but each row's data bytes
 * hold the reconstructed (unfiltered) sample values. Returns 0 / PNG_EBADFILTER. */
static int unfilter(uint8_t *raw, uint32_t height, uint32_t stride, uint32_t bpp)
{
	uint8_t *prev = NULL;                   /* previous reconstructed row      */
	for (uint32_t y = 0; y < height; y++) {
		uint8_t  ftype = raw[y * (stride + 1)];
		uint8_t *row   = raw + y * (stride + 1) + 1;
		for (uint32_t x = 0; x < stride; x++) {
			int a = x >= bpp ? row[x - bpp]  : 0;   /* left            */
			int b = prev     ? prev[x]       : 0;   /* up              */
			int c = (prev && x >= bpp) ? prev[x - bpp] : 0; /* up-left */
			int v = row[x];
			switch (ftype) {
			case 0: break;                              /* None        */
			case 1: v += a;                  break;     /* Sub         */
			case 2: v += b;                  break;     /* Up          */
			case 3: v += (a + b) / 2;        break;     /* Average     */
			case 4: v += paeth(a, b, c);     break;     /* Paeth       */
			default: return PNG_EBADFILTER;
			}
			row[x] = (uint8_t)v;
		}
		prev = row;
	}
	return PNG_OK;
}

int png_decode(const uint8_t *src, uint32_t slen, png_image *out)
{
	if (slen < 8)
		return PNG_EBADSIG;
	for (int i = 0; i < 8; i++)
		if (src[i] != PNG_SIG[i])
			return PNG_EBADSIG;

	/* ---- pass 1: walk chunks, read IHDR, measure total IDAT size --------- */
	uint32_t width = 0, height = 0, idat_total = 0;
	uint8_t  depth = 0, color = 0, interlace = 0;
	int      seen_ihdr = 0;
	uint32_t pos = 8;

	while (pos + 8 <= slen) {
		uint32_t len  = load_be32(src + pos);
		const uint8_t *type = src + pos + 4;
		uint32_t body = pos + 8;
		if (body + len + 4 > slen)          /* +4 chunk CRC (we don't verify)  */
			return PNG_ETRUNC;

		if (type[0]=='I'&&type[1]=='H'&&type[2]=='D'&&type[3]=='R') {
			if (len < 13)
				return PNG_EBADHDR;
			width     = load_be32(src + body);
			height    = load_be32(src + body + 4);
			depth     = src[body + 8];
			color     = src[body + 9];
			/* src[body+10] compression, [body+11] filter (both always 0)      */
			interlace = src[body + 12];
			seen_ihdr = 1;
		} else if (type[0]=='I'&&type[1]=='D'&&type[2]=='A'&&type[3]=='T') {
			idat_total += len;
		} else if (type[0]=='I'&&type[1]=='E'&&type[2]=='N'&&type[3]=='D') {
			break;
		}
		/* TODO: PLTE / tRNS capture here once palette support lands.          */
		pos = body + len + 4;
	}

	if (!seen_ihdr || width == 0 || height == 0)
		return PNG_EBADHDR;

	/* ---- scope gate: 8-bit, non-interlaced, RGB(2) or RGBA(6) only ------- */
	/* TODO: colour type 0/4 (grayscale[+alpha]), 3 (palette+PLTE/tRNS),
	 *       sub-8-bit & 16-bit depths, and Adam7 (interlace==1).             */
	if (depth != 8 || interlace != 0 || (color != 2 && color != 6))
		return PNG_EUNSUPPORTED;

	uint32_t channels = (color == 6) ? 4u : 3u;   /* bytes per pixel = bpp     */
	uint32_t stride   = width * channels;         /* filtered bytes per row    */

	/* Guard against width*height*4 / stride*height overflow on 32-bit.       */
	if (width > 0xFFFF || height > 0xFFFF)
		return PNG_ETOOBIG;

	/* ---- pass 2: concatenate IDAT into one zlib stream ------------------- */
	uint8_t *zbuf = kmalloc(idat_total);
	if (!zbuf)
		return PNG_ENOMEM;
	uint32_t zlen = 0;
	pos = 8;
	while (pos + 8 <= slen) {
		uint32_t len  = load_be32(src + pos);
		const uint8_t *type = src + pos + 4;
		uint32_t body = pos + 8;
		if (type[0]=='I'&&type[1]=='D'&&type[2]=='A'&&type[3]=='T') {
			for (uint32_t i = 0; i < len; i++)
				zbuf[zlen++] = src[body + i];
		} else if (type[0]=='I'&&type[1]=='E'&&type[2]=='N'&&type[3]=='D') {
			break;
		}
		pos = body + len + 4;
	}

	/* ---- inflate -> raw filtered scanlines ------------------------------- */
	uint32_t raw_size = height * (stride + 1);     /* +1 filter byte per row   */
	uint8_t *raw = kmalloc(raw_size);
	if (!raw) {
		kfree(zbuf);
		return PNG_ENOMEM;
	}
	int n = zlib_inflate(zbuf, zlen, raw, raw_size);
	kfree(zbuf);
	if (n < 0 || (uint32_t)n != raw_size) {
		kfree(raw);
		return PNG_EINFLATE;
	}

	int fr = unfilter(raw, height, stride, channels);
	if (fr != PNG_OK) {
		kfree(raw);
		return fr;
	}

	/* ---- pack to XRGB8888 ------------------------------------------------ */
	uint32_t *px = kmalloc(width * height * 4);
	if (!px) {
		kfree(raw);
		return PNG_ENOMEM;
	}
	for (uint32_t y = 0; y < height; y++) {
		const uint8_t *row = raw + y * (stride + 1) + 1;
		for (uint32_t x = 0; x < width; x++) {
			const uint8_t *p = row + x * channels;
			/* TODO: composite alpha (p[3]) over a background instead of
			 *       discarding it, once the viewer wants transparency.        */
			px[y * width + x] = ((uint32_t)p[0] << 16) |
			                    ((uint32_t)p[1] << 8)  |
			                     (uint32_t)p[2];
		}
	}
	kfree(raw);

	out->width  = width;
	out->height = height;
	out->pixels = px;
	return PNG_OK;
}

/* ---- encoding ----------------------------------------------------------- */

/* CRC32 over a PNG chunk's type+data (same polynomial as zlib/gzip, but PNG
 * stores it big-endian). Kept local so the encoder is self-contained. */
static uint32_t png_crc_table[256];
static int      png_crc_ready;

static void png_crc_build(void)
{
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;
		for (int k = 0; k < 8; k++)
			c = (c & 1) ? 0xEDB88320 ^ (c >> 1) : c >> 1;
		png_crc_table[i] = c;
	}
	png_crc_ready = 1;
}

static uint32_t png_crc(const uint8_t *data, uint32_t n)
{
	if (!png_crc_ready)
		png_crc_build();
	uint32_t c = 0xFFFFFFFF;
	for (uint32_t i = 0; i < n; i++)
		c = png_crc_table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
	return c ^ 0xFFFFFFFF;
}

static void be32_put(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* Append a chunk (length | type | data | CRC) at dst+pos; returns new pos.
 * Assumes the caller sized dst via png_encode_bound(). */
static uint32_t put_chunk(uint8_t *dst, uint32_t pos,
                          const char type[4], const uint8_t *data, uint32_t len)
{
	be32_put(dst + pos, len);
	dst[pos+4] = type[0]; dst[pos+5] = type[1];
	dst[pos+6] = type[2]; dst[pos+7] = type[3];
	for (uint32_t i = 0; i < len; i++)
		dst[pos + 8 + i] = data[i];
	be32_put(dst + pos + 8 + len, png_crc(dst + pos + 4, len + 4));
	return pos + 12 + len;
}

uint32_t png_encode_bound(uint32_t w, uint32_t h)
{
	uint32_t raw = h * (1 + w * 3);     /* filter byte + RGB per row           */
	/* 8 sig + 25 IHDR + IDAT(12 + gzip_bound(raw)) + 12 IEND.                 */
	return 8 + 25 + 12 + gzip_bound(raw) + 12;
}

int png_encode(const uint32_t *pixels, uint32_t w, uint32_t h,
               uint32_t src_pitch, uint8_t *dst, uint32_t dcap)
{
	if (w == 0 || h == 0 || w > 0xFFFF || h > 0xFFFF)
		return PNG_ETOOBIG;
	if (dcap < png_encode_bound(w, h))
		return PNG_ENOMEM;

	/* Build filter-0 (None) scanlines: each row is a 0 byte then packed RGB.
	 * We compress with stored DEFLATE blocks (zlib_store), so per-row filtering
	 * would not shrink the output — keep it simple and fast. */
	uint32_t raw_size = h * (1 + w * 3);
	uint8_t *raw = kmalloc(raw_size);
	if (!raw)
		return PNG_ENOMEM;
	uint8_t *q = raw;
	for (uint32_t y = 0; y < h; y++) {
		*q++ = 0;                       /* filter type: None                   */
		const uint32_t *row = pixels + (uint32_t)y * src_pitch;
		for (uint32_t x = 0; x < w; x++) {
			uint32_t px = row[x];
			*q++ = (uint8_t)(px >> 16); /* R                                   */
			*q++ = (uint8_t)(px >> 8);  /* G                                   */
			*q++ = (uint8_t)px;         /* B                                   */
		}
	}

	/* Signature. */
	uint32_t pos = 0;
	for (int i = 0; i < 8; i++)
		dst[pos++] = PNG_SIG[i];

	/* IHDR: width, height, depth=8, colour=2 (RGB), compression/filter/
	 * interlace all 0. */
	uint8_t ihdr[13];
	be32_put(ihdr,     w);
	be32_put(ihdr + 4, h);
	ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
	pos = put_chunk(dst, pos, "IHDR", ihdr, 13);

	/* IDAT: pack the scanlines straight into the chunk's data area as a stored
	 * zlib stream (fast, no UI stall), then frame it (length + type + CRC). */
	uint32_t idat_data = pos + 8;
	int zn = zlib_store(raw, raw_size, dst + idat_data, dcap - idat_data - 16);
	kfree(raw);
	if (zn < 0)
		return PNG_EINFLATE;
	be32_put(dst + pos, (uint32_t)zn);
	dst[pos+4]='I'; dst[pos+5]='D'; dst[pos+6]='A'; dst[pos+7]='T';
	be32_put(dst + idat_data + zn, png_crc(dst + pos + 4, (uint32_t)zn + 4));
	pos = idat_data + (uint32_t)zn + 4;

	/* IEND. */
	pos = put_chunk(dst, pos, "IEND", NULL, 0);
	return (int)pos;
}

/* ---- streaming encoder -------------------------------------------------- */

/* A sequential byte sink that flushes to an owofs file in fixed-size pieces, so
 * the whole PNG is written without ever holding it in RAM. crc/adler are kept
 * incrementally by the writer as bytes pass through. */
struct sink {
	const char *path;
	uint8_t    *buf;
	uint32_t    cap, len;     /* buffer capacity and current fill            */
	uint32_t    off;          /* next file offset to write at                */
	int         err;
};

static void sink_flush(struct sink *s)
{
	if (s->len && !s->err) {
		if (fs_write_at(s->path, s->off, s->buf, s->len) < 0)
			s->err = 1;
		s->off += s->len;
		s->len = 0;
	}
}

static void sink_write(struct sink *s, const uint8_t *d, uint32_t n)
{
	while (n && !s->err) {
		uint32_t room = s->cap - s->len;
		uint32_t k = n < room ? n : room;
		memcpy(s->buf + s->len, d, k);
		s->len += k;
		d += k; n -= k;
		if (s->len == s->cap)
			sink_flush(s);
	}
}

/* Incremental CRC32 (PNG chunk) — start at 0xFFFFFFFF, finalize with ^all-ones. */
static uint32_t crc_acc(uint32_t c, const uint8_t *d, uint32_t n)
{
	if (!png_crc_ready)
		png_crc_build();
	for (uint32_t i = 0; i < n; i++)
		c = png_crc_table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
	return c;
}

int png_write_stream(const char *path, const uint32_t *pixels, uint32_t w,
                     uint32_t h, uint32_t src_pitch)
{
	if (w == 0 || h == 0 || w > 0xFFFF || h > 0xFFFF)
		return PNG_ETOOBIG;

	uint32_t rowbytes = 1 + w * 3;          /* filter byte + RGB               */
	uint32_t raw_size = h * rowbytes;
	uint32_t nblk     = (raw_size + 65534) / 65535;
	uint32_t idat_len = 2 + nblk * 5 + raw_size + 4;   /* zlib hdr+blocks+adler*/
	uint32_t total    = 8 + (12 + 13) + (12 + idat_len) + (12 + 0);

	if (fs_prealloc(path, total) < 0)
		return PNG_ENOMEM;

	uint8_t *iobuf  = kmalloc(65536);
	uint8_t *rowbuf = kmalloc(rowbytes);
	if (!iobuf || !rowbuf) {
		kfree(iobuf); kfree(rowbuf);
		return PNG_ENOMEM;
	}
	struct sink s = { path, iobuf, 65536, 0, 0, 0 };

	/* Signature + IHDR chunk. */
	sink_write(&s, PNG_SIG, 8);
	uint8_t ihdr[13];
	be32_put(ihdr,     w);
	be32_put(ihdr + 4, h);
	ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
	uint8_t chdr[8];
	be32_put(chdr, 13);
	chdr[4]='I';chdr[5]='H';chdr[6]='D';chdr[7]='R';
	sink_write(&s, chdr, 8);
	sink_write(&s, ihdr, 13);
	uint8_t crc4[4];
	be32_put(crc4, crc_acc(crc_acc(0xFFFFFFFF, chdr + 4, 4), ihdr, 13) ^ 0xFFFFFFFF);
	sink_write(&s, crc4, 4);

	/* IDAT chunk header (length known up front). */
	be32_put(chdr, idat_len);
	chdr[4]='I';chdr[5]='D';chdr[6]='A';chdr[7]='T';
	sink_write(&s, chdr, 8);
	uint32_t icrc = crc_acc(0xFFFFFFFF, chdr + 4, 4);   /* CRC over type + data */

	/* zlib header. */
	uint8_t zh[2] = { 0x78, 0x01 };
	sink_write(&s, zh, 2);
	icrc = crc_acc(icrc, zh, 2);

	/* Stored DEFLATE blocks over the raw scanline stream, with a running adler32
	 * over the raw bytes only. Rows are generated on the fly; a block boundary
	 * may fall mid-row, so we track how much of the current block remains. */
	uint32_t a = 1, b = 0;                  /* adler32 sums                    */
	uint32_t raw_left = raw_size;
	uint32_t blk_left = 0;
	for (uint32_t y = 0; y < h; y++) {
		rowbuf[0] = 0;                  /* filter: None                        */
		const uint32_t *row = pixels + (uint32_t)y * src_pitch;
		uint8_t *q = rowbuf + 1;
		for (uint32_t x = 0; x < w; x++) {
			uint32_t px = row[x];
			*q++ = (uint8_t)(px >> 16);
			*q++ = (uint8_t)(px >> 8);
			*q++ = (uint8_t)px;
		}
		/* Emit rowbuf through the block splitter. */
		uint32_t done = 0;
		while (done < rowbytes) {
			if (blk_left == 0) {
				uint32_t chunk = raw_left > 65535 ? 65535 : raw_left;
				uint8_t bh[5] = { (uint8_t)(chunk == raw_left),
				                  (uint8_t)chunk, (uint8_t)(chunk >> 8),
				                  (uint8_t)~chunk, (uint8_t)(~chunk >> 8) };
				sink_write(&s, bh, 5);
				icrc = crc_acc(icrc, bh, 5);
				blk_left = chunk;
			}
			uint32_t avail = rowbytes - done;
			uint32_t k = avail < blk_left ? avail : blk_left;
			sink_write(&s, rowbuf + done, k);
			icrc = crc_acc(icrc, rowbuf + done, k);
			for (uint32_t i = 0; i < k; i++) {        /* adler over raw data   */
				a += rowbuf[done + i];
				if (a >= 65521) a -= 65521;
				b += a;
				if (b >= 65521) b -= 65521;
			}
			done += k; blk_left -= k; raw_left -= k;
		}
	}

	/* adler32 trailer (big-endian), then the IDAT CRC. */
	uint8_t ad[4] = { (uint8_t)(b >> 8), (uint8_t)b, (uint8_t)(a >> 8), (uint8_t)a };
	sink_write(&s, ad, 4);
	icrc = crc_acc(icrc, ad, 4);
	be32_put(crc4, icrc ^ 0xFFFFFFFF);
	sink_write(&s, crc4, 4);

	/* IEND. */
	be32_put(chdr, 0);
	chdr[4]='I';chdr[5]='E';chdr[6]='N';chdr[7]='D';
	sink_write(&s, chdr, 8);
	be32_put(crc4, crc_acc(0xFFFFFFFF, chdr + 4, 4) ^ 0xFFFFFFFF);
	sink_write(&s, crc4, 4);

	sink_flush(&s);
	kfree(iobuf); kfree(rowbuf);
	return s.err ? PNG_EINFLATE : PNG_OK;
}

const char *png_strerror(int err)
{
	switch (err) {
	case PNG_OK:           return "ok";
	case PNG_EBADSIG:      return "not a PNG";
	case PNG_ETRUNC:       return "truncated PNG";
	case PNG_EBADHDR:      return "bad IHDR";
	case PNG_EUNSUPPORTED: return "unsupported PNG (need 8-bit RGB/RGBA, non-interlaced)";
	case PNG_EINFLATE:     return "IDAT inflate failed";
	case PNG_EBADFILTER:   return "bad scanline filter";
	case PNG_ENOMEM:       return "out of memory";
	case PNG_ETOOBIG:      return "image too large";
	default:               return "unknown PNG error";
	}
}
