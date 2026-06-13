/* play.c — ring-3 audio player.
 *
 * `play <file.wav|.mp3>` streams a song to the sound device; `play <tune>` (or a
 * bare `play`) plays a PC-speaker tune. Being a ring-3 program is the point: it
 * reads the file in small chunks with pread (so a 30 MiB song never lives in
 * memory), decodes/resamples to the device's canonical 48 kHz / 16-bit / stereo
 * here in user space, and pushes ready frames to the kernel, which
 * back-pressures. The whole multi-minute playback is a normal preemptible
 * process making bounded syscalls — so playing off slow or flaky USB media keeps
 * the machine responsive, and a bug here is a contained crash, not a kernel hang.
 * Ctrl+C stops it (audio_write returns -2).
 *
 * Two source paths feed one resampler->audio_write loop (run_stream): raw PCM
 * for WAV, and the streaming MPEG decoder (mp3.c) for MP3/MP1/MP2.
 *
 *   play <file.wav|.mp3>
 *   play [twinkle|megalovania|sweden]
 */

#include "owos.h"
#include "mp3.h"

static unsigned rd_le32(const unsigned char *p)
{ return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }
static unsigned rd_le16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static int tageq(const unsigned char *p, const char *s)
{ return p[0]==s[0] && p[1]==s[1] && p[2]==s[2] && p[3]==s[3]; }

/* ---- WAV stream + 48 kHz/S16/stereo resampler -------------------------- */

static const char *r_fn;
static unsigned r_data_off, r_data_len, r_filepos;
static unsigned r_bits, r_channels, r_frame;
static unsigned char r_in[262144];         /* raw source PCM, refilled by pread */
static unsigned r_inlen, r_inpos;
static short r_curL, r_curR;               /* the current (held) source frame   */
static int r_eof;

static int   r_is_mp3;                     /* source is the MPEG decoder, not WAV */
static mp3_t r_mp3;

/* Decode the next source frame into r_curL/r_curR; sets r_eof at end of data. */
static void decode_next(void)
{
	if (r_eof) { r_curL = r_curR = 0; return; }

	if (r_is_mp3) {
		short f[2];
		if (mp3_read(&r_mp3, f, 1) <= 0) { r_eof = 1; r_curL = r_curR = 0; return; }
		r_curL = f[0]; r_curR = f[1];
		return;
	}

	if (r_inpos + r_frame > r_inlen) {                 /* refill the input buffer */
		if (r_filepos >= r_data_len) { r_eof = 1; r_curL = r_curR = 0; return; }
		unsigned want = r_data_len - r_filepos;
		if (want > sizeof r_in) want = sizeof r_in;
		want -= want % r_frame;
		if (want == 0) want = r_frame;
		int got = readfile_at(r_fn, r_data_off + r_filepos, r_in, want);
		if (got <= 0 || (unsigned)got < r_frame) { r_eof = 1; r_curL = r_curR = 0; return; }
		r_inlen = (unsigned)got; r_inpos = 0; r_filepos += (unsigned)got;
	}

	const unsigned char *s = r_in + r_inpos;
	r_inpos += r_frame;
	if (r_bits == 8) {
		int l = ((int)s[0] - 128) << 8;
		r_curL = (short)l;
		r_curR = (short)((r_channels == 2) ? (((int)s[1] - 128) << 8) : l);
	} else {
		r_curL = (short)rd_le16(s);
		r_curR = (r_channels == 2) ? (short)rd_le16(s + 2) : r_curL;
	}
}

static int run_stream(unsigned rate);

static int play_wav(const char *fn, unsigned size)
{
	unsigned char hdr[256];
	int got = readfile_at(fn, 0, hdr, sizeof hdr);
	if (got < 44 || !tageq(hdr, "RIFF") || !tageq(hdr + 8, "WAVE")) {
		puts("play: not a WAV file\n");
		return 1;
	}

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
	if (!data_off || fmt != 1 || (bits != 8 && bits != 16) || channels < 1 || channels > 2) {
		puts("play: unsupported WAV (need PCM 8/16-bit mono/stereo)\n");
		return 1;
	}
	if (data_off + data_len > size)
		data_len = size - data_off;

	r_is_mp3 = 0;
	r_fn = fn; r_data_off = data_off; r_data_len = data_len; r_filepos = 0;
	r_bits = bits; r_channels = channels; r_frame = channels * (bits / 8);
	r_inlen = r_inpos = 0;
	return run_stream(rate);
}

static int play_mp3(const char *fn, unsigned size)
{
	if (mp3_open(&r_mp3, fn, size) != 0) {
		puts("play: not a playable audio file\n");
		return 1;
	}
	r_is_mp3 = 1;
	return run_stream(mp3_rate(&r_mp3));
}

/* Common tail: open the device, then resample the active source (WAV or MP3)
 * to 48 kHz / 16-bit / stereo and push it, back-pressured, until end or Ctrl+C.
 * step is the 16.16 input-frames-per-output-frame ratio, built without a 64-bit
 * divide (the user libc has no libgcc helpers). */
static int run_stream(unsigned rate)
{
	if (rate < 4000)   rate = 4000;
	if (rate > 192000) rate = 192000;

	if (audio_open() != 0) {
		puts("play: no audio output device\n");
		return 1;
	}

	r_curL = r_curR = 0;
	r_eof = 0;
	unsigned step = ((rate / 48000u) << 16) + (((rate % 48000u) << 16) / 48000u);

	static short out[8192];                /* 4096 stereo S16 frames            */
	const unsigned OUTFRAMES = sizeof(out) / 4;
	unsigned acc = 0;
	int interrupted = 0;

	decode_next();                         /* prime the first source frame      */
	for (;;) {
		unsigned produced = 0;
		while (produced < OUTFRAMES) {
			out[produced * 2 + 0] = r_curL;
			out[produced * 2 + 1] = r_curR;
			produced++;
			if (r_eof) break;
			acc += step;                   /* advance the input position        */
			while (acc >= 0x10000u && !r_eof) { decode_next(); acc -= 0x10000u; }
		}
		if (produced == 0) break;
		int w = audio_write(out, (int)(produced * 4));
		if (w == -2) { interrupted = 1; break; }
		if (w < 0) break;
		if (r_eof) break;
	}

	audio_close();
	return interrupted ? 1 : 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		puts("usage: play <file.wav | file.mp3>\n");
		return 1;
	}

	const char *arg = argv[1];
	struct statinfo st;
	if (stat(arg, &st) != 0 || st.is_dir) {
		puts("play: no such file: ");
		puts(arg);
		puts("\n");
		return 1;
	}

	/* Route by content: RIFF/WAVE -> WAV, otherwise try the MPEG decoder. */
	unsigned char sn[12];
	int g = readfile_at(arg, 0, sn, sizeof sn);
	if (g >= 12 && tageq(sn, "RIFF") && tageq(sn + 8, "WAVE"))
		return play_wav(arg, st.size);
	return play_mp3(arg, st.size);
}
