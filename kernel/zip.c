/* zip.c — LZSS compression for the zip/unzip shell commands.
 *
 * Classic LZSS over a 4 KiB sliding window: matches of 3..18 bytes encoded as a
 * 12-bit back-distance + 4-bit length, literals passed through verbatim. A flag
 * byte precedes every run of up to eight tokens; bit i (LSB first) is 1 for a
 * literal byte, 0 for a back-reference (two bytes: distance low 8 / distance
 * high 4 + length). Hash chains keyed on 3-byte prefixes make the match search
 * fast, and a bounded chain walk keeps the worst case in check.
 *
 * The container is an 8-byte header — "OWOZ" then the original size, both so
 * unzip can reject foreign files and size its output exactly — followed by the
 * token stream.
 */

#include <stddef.h>
#include <stdint.h>
#include "zip.h"
#include "endian.h"
#include "heap.h"

#define WINDOW      4096            /* 12-bit back-distance (1..WINDOW)         */
#define MIN_MATCH   3
#define MAX_MATCH   18              /* 4-bit length, stored as len - MIN_MATCH  */
#define HASH_BITS   13
#define HASH_SIZE   (1u << HASH_BITS)
#define MAX_CHAIN   128             /* cap chain walks so compression is bounded */

static void put_u32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}


/* A multiplicative hash of the 3 bytes at p, folded to HASH_BITS. */
static uint32_t hash3(const uint8_t *p)
{
	uint32_t v = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
	return (v * 2654435761u) >> (32 - HASH_BITS);
}

/* Record position pos in the hash chains (only where a 3-byte key exists). */
static void ht_insert(int *head, int *prev, const uint8_t *src,
		      uint32_t slen, uint32_t pos)
{
	if (pos + MIN_MATCH <= slen) {
		uint32_t h = hash3(src + pos);
		prev[pos & (WINDOW - 1)] = head[h];
		head[h] = (int)pos;
	}
}

uint32_t zip_bound(uint32_t src_len)
{
	/* Worst case is all literals: one flag bit per byte (1/8 overhead) plus
	 * a possible partial flag byte, on top of the header. */
	return ZIP_HEADER + src_len + (src_len + 7) / 8 + 1;
}

int zip_compress(const uint8_t *src, uint32_t slen, uint8_t *dst, uint32_t dcap)
{
	if (dcap < ZIP_HEADER)
		return -1;

	dst[0] = 'O'; dst[1] = 'W'; dst[2] = 'O'; dst[3] = 'Z';
	put_u32(dst + 4, slen);
	uint32_t out = ZIP_HEADER;

	int *head = kmalloc(HASH_SIZE * sizeof(int));
	int *prev = kmalloc(WINDOW * sizeof(int));
	if (!head || !prev) {
		kfree(head);
		kfree(prev);
		return -1;
	}
	for (uint32_t i = 0; i < HASH_SIZE; i++)
		head[i] = -1;

	uint32_t pos = 0;
	uint32_t flagpos = 0;          /* dst index of the live flag byte          */
	int nflags = 0;                /* tokens emitted under the current flag     */

	while (pos < slen) {
		if (nflags == 0) {     /* reserve a fresh flag byte                 */
			if (out >= dcap)
				goto overflow;
			flagpos = out++;
			dst[flagpos] = 0;
		}

		/* Find the longest match for src[pos..] within the window. */
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

		if (best_len >= MIN_MATCH) {            /* back-reference token     */
			if (out + 2 > dcap)
				goto overflow;
			uint32_t d = best_dist - 1;         /* 0..4095 in 12 bits   */
			uint32_t lc = (uint32_t)best_len - MIN_MATCH;  /* 0..15     */
			dst[out++] = (uint8_t)(d & 0xFF);
			dst[out++] = (uint8_t)(((d >> 8) << 4) | lc);
			/* flag bit already 0; advance over every covered byte */
			uint32_t end = pos + (uint32_t)best_len;
			while (pos < end)
				ht_insert(head, prev, src, slen, pos++);
		} else {                                /* literal token            */
			if (out + 1 > dcap)
				goto overflow;
			dst[flagpos] |= (uint8_t)(1 << nflags);
			dst[out++] = src[pos];
			ht_insert(head, prev, src, slen, pos++);
		}
		nflags = (nflags + 1) & 7;
	}

	kfree(head);
	kfree(prev);
	return (int)out;

overflow:
	kfree(head);
	kfree(prev);
	return -1;
}

int zip_orig_size(const uint8_t *src, uint32_t slen)
{
	if (slen < ZIP_HEADER ||
	    src[0] != 'O' || src[1] != 'W' || src[2] != 'O' || src[3] != 'Z')
		return -1;
	return (int)load_le32(src + 4);
}

int zip_decompress(const uint8_t *src, uint32_t slen, uint8_t *dst, uint32_t dcap)
{
	int orig = zip_orig_size(src, slen);
	if (orig < 0 || (uint32_t)orig > dcap)
		return -1;

	uint32_t in = ZIP_HEADER, out = 0, target = (uint32_t)orig;
	while (out < target) {
		if (in >= slen)
			return -1;                  /* ran out of flags         */
		uint8_t flags = src[in++];
		for (int b = 0; b < 8 && out < target; b++) {
			if (flags & (1 << b)) {     /* literal                  */
				if (in >= slen)
					return -1;
				dst[out++] = src[in++];
			} else {                    /* back-reference           */
				if (in + 2 > slen)
					return -1;
				uint32_t b0 = src[in++];
				uint32_t b1 = src[in++];
				uint32_t dist = (((b1 >> 4) << 8) | b0) + 1;
				uint32_t len = (b1 & 0x0F) + MIN_MATCH;
				if (dist > out)
					return -1;          /* points before start  */
				uint32_t from = out - dist;
				for (uint32_t i = 0; i < len && out < target; i++)
					dst[out++] = dst[from + i];
			}
		}
	}
	return (int)out;
}
