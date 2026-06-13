/* mp3.h — streaming MPEG audio decoder (Layer I/II/III, MPEG-1/2/2.5).
 *
 * A thin streaming wrapper over the vendored minimp3 frame decoder (CC0,
 * minimp3.h). It decodes one frame at a time, pulling source bytes from a file
 * through readfile_at, so a multi-megabyte song never lives in memory at once —
 * the same reason /bin/play is a ring-3 program. Output is interleaved 16-bit
 * signed stereo at the file's native sample rate (a mono stream is duplicated to
 * both channels); /bin/play resamples that to the device's 48 kHz. A decode bug
 * is a contained process crash, never a kernel hang.
 */
#ifndef OWOS_MP3_H
#define OWOS_MP3_H

#include "minimp3.h"            /* mp3dec_t, MINIMP3_MAX_SAMPLES_PER_FRAME */

typedef struct mp3 {
	const char   *fn;             /* source file (owofs path)              */
	unsigned      size;           /* total file bytes                      */
	unsigned      file_off;       /* next file byte to read                */
	int           src_eof;        /* no more file bytes to read            */

	mp3dec_t      dec;            /* minimp3 decoder state                 */
	unsigned char in[8192];       /* sliding window of undecoded bytes     */
	int           in_len;         /* valid bytes at in[0..in_len)          */

	short         pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];  /* one decoded frame */
	int           out_frames;     /* frames decoded into pcm               */
	int           out_pos;        /* next frame to hand out                */
	int           out_ch;         /* channels held in pcm (1 or 2)         */

	unsigned      rate;           /* native sample rate, Hz                */
	unsigned      channels;       /* source channels (1 or 2)              */
} mp3_t;

/* Open `fn` (file size `size`) and decode the first frame so rate/channels are
 * known. Returns 0 on success, -1 if `fn` is not a decodable MPEG stream. */
int      mp3_open(mp3_t *m, const char *fn, unsigned size);

unsigned mp3_rate(const mp3_t *m);
unsigned mp3_channels(const mp3_t *m);

/* Pull up to `maxframes` stereo frames (2 shorts each) into `out`. Returns the
 * number of frames produced, or 0 at end of stream. */
int      mp3_read(mp3_t *m, short *out, int maxframes);

void     mp3_close(mp3_t *m);

#endif /* OWOS_MP3_H */
