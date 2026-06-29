/* media.c — see media.h. A file decoder (WAV/MPEG -> 48 kHz/S16/stereo) and a
 * software mixer thread that sums active sources into the one kernel PCM stream.
 *
 * The decode/resample core mirrors /bin/player (decode_next + a 16.16 fixed-point
 * resampler): the file is pulled in small reads so a long song never sits in
 * memory, and a decode fault is a contained user-space crash, never a kernel
 * hang. The mixer adds the part the kernel can't: the device offers a single
 * exclusive stream, so two sounds at once must be blended before audio_write —
 * that blend happens here.
 */
#include "owos.h"
#include "mp3.h"
#include "media.h"

/* ---- small spinlock (mirrors the user malloc/blk_lock pattern) ------------ */
static inline int xchg32(volatile int *p, int v)
{ __asm__ volatile ("xchgl %0, %1" : "+r"(v), "+m"(*p) : : "memory"); return v; }

static volatile int g_lock;
static void mlock(void)   { while (xchg32(&g_lock, 1)) msleep(0); }
static void munlock(void) { __asm__ volatile ("" ::: "memory"); g_lock = 0; }

/* ---- decoder ------------------------------------------------------------- */
struct media_decoder {
	int          is_mp3;
	char         path[256];
	unsigned     size;            /* whole-file bytes                         */
	unsigned     rate;            /* native sample rate, Hz                   */

	/* WAV cursor (PCM s8/s16, mono/stereo) */
	unsigned     w_data_off, w_data_len, w_bits, w_channels, w_frame;
	unsigned     w_filepos;       /* byte cursor into the data region         */
	unsigned char w_in[65536];    /* raw source PCM, refilled by readfile_at  */
	unsigned     w_inlen, w_inpos;

	mp3_t        m3;              /* MPEG decoder state                       */

	/* shared decode/resample cursor */
	short        curL, curR;      /* current (held) source frame              */
	int          eof;
	unsigned     pos_frames;      /* native source frames consumed (the clock)*/
	unsigned     total_frames;    /* WAV: exact; MPEG: 0 (estimated instead)  */
	unsigned     step, acc;       /* 16.16 input-frames per output-frame      */
	int          primed;
};

static unsigned rd_le32(const unsigned char *p)
{ return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }
static unsigned rd_le16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static int tageq(const unsigned char *p, const char *s)
{ return p[0]==s[0] && p[1]==s[1] && p[2]==s[2] && p[3]==s[3]; }

static void decode_next(media_decoder *d)
{
	if (d->eof) { d->curL = d->curR = 0; return; }

	if (d->is_mp3) {
		short f[2];
		if (mp3_read(&d->m3, f, 1) <= 0) { d->eof = 1; d->curL = d->curR = 0; return; }
		d->curL = f[0]; d->curR = f[1];
		d->pos_frames++;
		return;
	}

	if (d->w_inpos + d->w_frame > d->w_inlen) {            /* refill input buffer */
		if (d->w_filepos >= d->w_data_len) { d->eof = 1; d->curL = d->curR = 0; return; }
		unsigned want = d->w_data_len - d->w_filepos;
		if (want > sizeof d->w_in) want = sizeof d->w_in;
		want -= want % d->w_frame;
		if (want == 0) want = d->w_frame;
		int got = readfile_at(d->path, d->w_data_off + d->w_filepos, d->w_in, want);
		if (got <= 0 || (unsigned)got < d->w_frame) { d->eof = 1; d->curL = d->curR = 0; return; }
		d->w_inlen = (unsigned)got; d->w_inpos = 0; d->w_filepos += (unsigned)got;
	}

	const unsigned char *s = d->w_in + d->w_inpos;
	d->w_inpos += d->w_frame;
	if (d->w_bits == 8) {
		int l = ((int)s[0] - 128) << 8;
		d->curL = (short)l;
		d->curR = (short)((d->w_channels == 2) ? (((int)s[1] - 128) << 8) : l);
	} else {
		d->curL = (short)rd_le16(s);
		d->curR = (d->w_channels == 2) ? (short)rd_le16(s + 2) : d->curL;
	}
	d->pos_frames++;
}

int media_read(media_decoder *d, short *out, int frames)
{
	if (!d || frames <= 0) return 0;
	if (!d->primed) { decode_next(d); d->primed = 1; d->acc = 0; }
	if (d->eof) return 0;
	int produced = 0;
	while (produced < frames) {
		out[produced * 2 + 0] = d->curL;
		out[produced * 2 + 1] = d->curR;
		produced++;
		if (d->eof) break;
		d->acc += d->step;
		while (d->acc >= 0x10000u && !d->eof) { decode_next(d); d->acc -= 0x10000u; }
	}
	return produced;
}

/* MPEG total frames: extrapolate from bytes consumed vs. file size, 0 until
 * something has decoded. WAV is exact. */
static unsigned est_total_frames(const media_decoder *d)
{
	if (!d->is_mp3) return d->total_frames;
	if (d->pos_frames == 0 || d->m3.file_off == 0 || d->size == 0) return 0;
	unsigned elapsed = d->pos_frames / (d->rate ? d->rate : 1);
	if (elapsed == 0) return 0;
	unsigned bps = d->m3.file_off / elapsed;         /* source bytes per second */
	if (bps == 0) return 0;
	return (d->size / bps) * (d->rate ? d->rate : 1);
}

unsigned media_position_ms(const media_decoder *d)
{
	if (!d || !d->rate) return 0;
	return (unsigned)((unsigned long long)d->pos_frames * 1000ull / d->rate);
}

unsigned media_duration_ms(const media_decoder *d)
{
	if (!d || !d->rate) return 0;
	unsigned tf = est_total_frames(d);
	return (unsigned)((unsigned long long)tf * 1000ull / d->rate);
}

void media_seek_ms(media_decoder *d, unsigned ms)
{
	if (!d || !d->rate) return;
	unsigned tgt = (unsigned)((unsigned long long)ms * d->rate / 1000ull);  /* native frame */

	if (d->is_mp3) {
		unsigned dur = media_duration_ms(d);
		unsigned off = dur ? (unsigned)((unsigned long long)d->size * ms / dur) : 0;
		if (off > d->size) off = d->size;
		d->m3.file_off = off;
		d->m3.in_len   = 0;
		d->m3.out_pos  = d->m3.out_frames;   /* force a refill on the next read */
		d->m3.src_eof  = 0;
		d->pos_frames  = tgt;
	} else {
		if (d->total_frames && tgt > d->total_frames) tgt = d->total_frames;
		d->w_filepos = tgt * d->w_frame;
		if (d->w_filepos > d->w_data_len)
			d->w_filepos = d->w_data_len - (d->w_frame ? d->w_data_len % d->w_frame : 0);
		d->w_inlen = d->w_inpos = 0;
		d->pos_frames = tgt;
	}
	d->eof = 0;
	d->primed = 0;          /* re-prime curL/curR from the new position */
	d->acc = 0;
}

static int load_wav(media_decoder *d, unsigned size)
{
	unsigned char hdr[256];
	int got = readfile_at(d->path, 0, hdr, sizeof hdr);
	if (got < 44 || !tageq(hdr, "RIFF") || !tageq(hdr + 8, "WAVE"))
		return -1;

	unsigned fmt = 0, channels = 0, bits = 0, rate = 0, data_off = 0, data_len = 0;
	for (unsigned pos = 12; pos + 8 <= (unsigned)got; ) {
		const unsigned char *c = hdr + pos;
		unsigned sz = rd_le32(c + 4);
		if (tageq(c, "fmt ")) {
			fmt = rd_le16(c + 8); channels = rd_le16(c + 10);
			rate = rd_le32(c + 12); bits = rd_le16(c + 22);
		} else if (tageq(c, "data")) {
			data_off = pos + 8; data_len = sz; break;
		}
		pos += 8 + sz + (sz & 1);
	}
	if (!data_off || fmt != 1 || (bits != 8 && bits != 16) || channels < 1 || channels > 2)
		return -1;
	if (data_off + data_len > size)
		data_len = size - data_off;

	d->is_mp3 = 0;
	d->size = size; d->rate = rate;
	d->w_data_off = data_off; d->w_data_len = data_len; d->w_filepos = 0;
	d->w_bits = bits; d->w_channels = channels; d->w_frame = channels * (bits / 8);
	d->w_inlen = d->w_inpos = 0;
	d->total_frames = d->w_frame ? d->w_data_len / d->w_frame : 0;
	return 0;
}

static int load_mp3(media_decoder *d, unsigned size)
{
	if (mp3_open(&d->m3, d->path, size) != 0)
		return -1;
	d->is_mp3 = 1;
	d->size = size; d->rate = mp3_rate(&d->m3);
	d->total_frames = 0;     /* unknown for MPEG */
	return 0;
}

media_decoder *media_open(const char *path)
{
	struct statinfo st;
	int i;
	if (!path || stat(path, &st) != 0 || st.is_dir)
		return 0;

	media_decoder *d = (media_decoder *)malloc(sizeof *d);
	if (!d) return 0;
	memset(d, 0, sizeof *d);
	for (i = 0; i < (int)sizeof d->path - 1 && path[i]; i++) d->path[i] = path[i];
	d->path[i] = '\0';

	unsigned char sn[12];
	int g = readfile_at(d->path, 0, sn, sizeof sn);
	int rc;
	if (g >= 12 && tageq(sn, "RIFF") && tageq(sn + 8, "WAVE"))
		rc = load_wav(d, st.size);
	else
		rc = load_mp3(d, st.size);
	if (rc != 0) { free(d); return 0; }

	if (d->rate < 4000)   d->rate = 4000;
	if (d->rate > 192000) d->rate = 192000;
	/* 16.16 input-frames per output-frame at the device's 48 kHz. */
	d->step = ((d->rate / MEDIA_RATE) << 16) + (((d->rate % MEDIA_RATE) << 16) / MEDIA_RATE);
	d->pos_frames = 0;
	d->eof = 0;
	d->primed = 0;
	return d;
}

void media_close(media_decoder *d)
{
	if (!d) return;
	if (d->is_mp3) mp3_close(&d->m3);
	free(d);
}

/* ---- software mixer ------------------------------------------------------ */
#define MIX_FRAMES  1024            /* ~21 ms per chunk at 48 kHz              */
#define MAX_SOURCES 16

struct media_source {
	media_decoder    *dec;
	volatile int      in_use;       /* slot is live (set last by _new)         */
	volatile int      free_pending; /* glue asked to drop it; mixer reaps      */
	volatile int      playing;      /* desired play state (glue sets)          */
	volatile int      ended;        /* mixer hit EOS, not looping              */
	volatile int      loop;
	volatile int      vol;          /* 0..256                                  */
	volatile unsigned seek_ms;
	volatile int      seek_pending;
	volatile unsigned pos_ms;       /* mixer publishes                         */
	volatile unsigned dur_ms;       /* mixer publishes                         */
	media_end_cb      on_end;
	void             *user;
	volatile int      end_fired;    /* on_end already called once              */
};

static struct media_source g_src[MAX_SOURCES];
static volatile int g_started;
static int g_stream_open;                          /* mixer-thread-only        */

static int   g_acc[MIX_FRAMES * MEDIA_CH];          /* mix accumulators         */
static short g_tmp[MIX_FRAMES * MEDIA_CH];          /* one source's decode      */
static short g_out[MIX_FRAMES * MEDIA_CH];          /* clamped output           */
static unsigned char g_mixer_stack[131072];

/* Pull a full MIX_FRAMES chunk from one source into g_tmp, honoring loop. Returns
 * frames filled (0 = the source is out of audio). */
static int fill_source(media_source *s)
{
	int filled = 0, dry = 0;
	while (filled < MIX_FRAMES) {
		int got = media_read(s->dec, g_tmp + filled * MEDIA_CH, MIX_FRAMES - filled);
		if (got <= 0) {
			if (s->loop && dry++ < 2) { media_seek_ms(s->dec, 0); continue; }
			break;
		}
		filled += got;
	}
	return filled;
}

static void mixer_thread(void)
{
	for (;;) {
		int i, max_filled = 0, want = 0;

		/* Reap freed slots and publish clocks for every live source. */
		mlock();
		for (i = 0; i < MAX_SOURCES; i++) {
			media_source *s = &g_src[i];
			if (s->free_pending) {
				media_close(s->dec);
				memset(s, 0, sizeof *s);
				continue;
			}
			if (!s->in_use) continue;
			s->pos_ms = media_position_ms(s->dec);
			s->dur_ms = media_duration_ms(s->dec);
			if (s->playing && !s->ended) want = 1;
		}
		munlock();

		if (!want) {                                  /* nothing to play */
			if (g_stream_open) { audio_close(); g_stream_open = 0; }
			msleep(15);
			continue;
		}
		if (!g_stream_open) {
			if (audio_open() != 0) { msleep(25); continue; }  /* no device yet */
			g_stream_open = 1;
		}

		memset(g_acc, 0, sizeof g_acc);
		for (i = 0; i < MAX_SOURCES; i++) {
			media_source *s = &g_src[i];
			int filled, f, vol;
			if (!s->in_use || s->free_pending) continue;
			if (s->seek_pending) {
				media_seek_ms(s->dec, s->seek_ms);
				s->seek_pending = 0; s->ended = 0; s->end_fired = 0;
			}
			if (!s->playing || s->ended) continue;

			filled = fill_source(s);
			s->pos_ms = media_position_ms(s->dec);
			s->dur_ms = media_duration_ms(s->dec);
			if (filled == 0) {                        /* end of stream */
				s->ended = 1;
				if (s->on_end && !s->end_fired) { s->end_fired = 1; s->on_end(s->user); }
				continue;
			}
			vol = s->vol;
			for (f = 0; f < filled * MEDIA_CH; f++)
				g_acc[f] += (g_tmp[f] * vol) >> 8;
			if (filled > max_filled) max_filled = filled;
		}

		if (max_filled == 0) { msleep(5); continue; }

		for (i = 0; i < max_filled * MEDIA_CH; i++) {
			int v = g_acc[i];
			if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
			g_out[i] = (short)v;
		}
		if (audio_write(g_out, max_filled * MEDIA_CH * 2) < 0) {
			/* device error or a stray Ctrl+C latch: drop the stream and retry. */
			audio_close(); g_stream_open = 0;
			msleep(10);
		}
	}
}

void media_mixer_start(void)
{
	mlock();
	if (!g_started) {
		g_started = 1;
		thread_create(mixer_thread, &g_mixer_stack[sizeof g_mixer_stack]);
	}
	munlock();
}

media_source *media_source_new(media_decoder *d, media_end_cb on_end, void *user)
{
	int i;
	media_source *slot = 0;
	if (!d) return 0;
	media_mixer_start();
	mlock();
	for (i = 0; i < MAX_SOURCES; i++) {
		if (!g_src[i].in_use && !g_src[i].free_pending) { slot = &g_src[i]; break; }
	}
	if (slot) {
		slot->dec = d;
		slot->playing = 0;
		slot->ended = 0;
		slot->loop = 0;
		slot->vol = 256;
		slot->seek_pending = 0;
		slot->pos_ms = 0;
		slot->dur_ms = media_duration_ms(d);
		slot->on_end = on_end;
		slot->user = user;
		slot->end_fired = 0;
		slot->free_pending = 0;
		__asm__ volatile ("" ::: "memory");
		slot->in_use = 1;
	}
	munlock();
	return slot;
}

void media_source_free(media_source *s)
{
	if (!s) return;
	s->playing = 0;
	__asm__ volatile ("" ::: "memory");
	s->free_pending = 1;
}

void media_source_play(media_source *s)  { if (s) s->playing = 1; }
void media_source_pause(media_source *s) { if (s) s->playing = 0; }
void media_source_set_loop(media_source *s, int loop) { if (s) s->loop = loop ? 1 : 0; }

void media_source_set_volume(media_source *s, int vol256)
{
	if (!s) return;
	if (vol256 < 0) vol256 = 0; else if (vol256 > 256) vol256 = 256;
	s->vol = vol256;
}

void media_source_seek_ms(media_source *s, unsigned ms)
{
	if (!s) return;
	s->seek_ms = ms;
	s->pos_ms = ms;
	s->ended = 0;
	__asm__ volatile ("" ::: "memory");
	s->seek_pending = 1;
}

int      media_source_playing(const media_source *s)     { return s ? (s->playing && !s->ended) : 0; }
int      media_source_ended(const media_source *s)       { return s ? s->ended : 0; }
unsigned media_source_position_ms(const media_source *s) { return s ? s->pos_ms : 0; }
unsigned media_source_duration_ms(const media_source *s) { return s ? s->dur_ms : 0; }
