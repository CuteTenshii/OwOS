/* mp3.c — streaming MPEG audio decoder for ring-3 /bin/play.
 *
 * The decode core is minimp3 by Lion (lieff), public domain / CC0, vendored at
 * minimp3.h and adapted for OwOS (see the patch list at the top of that file).
 * This file is the OwOS part: a small pull-style streaming API (mp3.h) over the
 * minimp3 frame decoder, plus the few libc routines it needs.
 *
 * minimp3 is freestanding: no libm (it ships precomputed gain/window tables), no
 * malloc, no 64-bit division — exactly what the OwOS user libc can link (bare ld,
 * no libgcc). Its only libc dependency is memcpy/memset/memmove, provided below
 * so the linker and the compiler's struct-copy lowering both resolve. SIMD is
 * compiled out (MINIMP3_NO_SIMD).
 */

#include <stddef.h>
#include <stdint.h>

#include "owos.h"               /* readfile_at */
#include "mp3.h"                /* mp3_t + minimp3 declarations            */

/* ---- the decoder core (scalar, no SIMD) -------------------------------- */
#define MINIMP3_NO_SIMD
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

/* Decode one frame into m->pcm. Returns frames produced (0 at end of stream).
 * Refills the input window from the file as needed and skips junk/ID3 tags
 * (minimp3 reports those as frames consumed with zero samples). */
static int mp3_decode_one(mp3_t *m)
{
	for (;;) {
		if (!m->src_eof) {                 /* top up the sliding window */
			while (m->in_len < (int)sizeof m->in) {
				int n = readfile_at(m->fn, m->file_off, m->in + m->in_len,
				                    (unsigned)((int)sizeof m->in - m->in_len));
				if (n <= 0) { m->src_eof = 1; break; }
				m->in_len += n;
				m->file_off += (unsigned)n;
			}
		}
		if (m->in_len <= 0)
			return 0;

		mp3dec_frame_info_t info;
		int samples = mp3dec_decode_frame(&m->dec, m->in, m->in_len,
		                                  (mp3d_sample_t *)m->pcm, &info);

		if (info.frame_bytes > 0) {        /* consume what was used */
			int rem = m->in_len - info.frame_bytes;
			if (rem > 0)
				memmove(m->in, m->in + info.frame_bytes, (unsigned)rem);
			m->in_len = rem > 0 ? rem : 0;
		} else if (m->src_eof) {
			return 0;                      /* no frame and no more data */
		} else if (m->in_len >= (int)sizeof m->in) {
			/* full window, no progress: drop a byte to resync */
			memmove(m->in, m->in + 1, (unsigned)(m->in_len - 1));
			m->in_len -= 1;
		}

		if (samples > 0) {
			m->rate       = (unsigned)info.hz;
			m->channels   = (unsigned)info.channels;
			m->out_ch     = info.channels;
			m->out_frames = samples;
			m->out_pos    = 0;
			return samples;
		}
		/* samples == 0: skipped a tag or a bad frame — keep going */
	}
}

int mp3_open(mp3_t *m, const char *fn, unsigned size)
{
	memset(m, 0, sizeof *m);
	m->fn = fn;
	m->size = size;
	mp3dec_init(&m->dec);
	if (mp3_decode_one(m) <= 0)
		return -1;                         /* not a decodable MPEG stream */
	return 0;
}

unsigned mp3_rate(const mp3_t *m)     { return m->rate; }
unsigned mp3_channels(const mp3_t *m) { return m->channels; }

int mp3_read(mp3_t *m, short *out, int maxframes)
{
	int done = 0;
	while (done < maxframes) {
		if (m->out_pos >= m->out_frames) {
			if (mp3_decode_one(m) <= 0)
				break;                     /* end of stream */
		}
		int avail = m->out_frames - m->out_pos;
		int n = maxframes - done;
		if (n > avail)
			n = avail;

		if (m->out_ch == 2) {
			const short *src = m->pcm + m->out_pos * 2;
			for (int i = 0; i < n; i++) {
				out[(done + i) * 2 + 0] = src[i * 2 + 0];
				out[(done + i) * 2 + 1] = src[i * 2 + 1];
			}
		} else {                           /* mono -> duplicate */
			const short *src = m->pcm + m->out_pos;
			for (int i = 0; i < n; i++) {
				short s = src[i];
				out[(done + i) * 2 + 0] = s;
				out[(done + i) * 2 + 1] = s;
			}
		}
		m->out_pos += n;
		done += n;
	}
	return done;
}

void mp3_close(mp3_t *m) { (void)m; }
