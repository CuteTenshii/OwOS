/* player.c — a graphical media player.
 *
 * `player <file.wav|.mp3>` opens a window with a seekable progress bar and
 * Restart / Play-Pause / Stop buttons, and streams the song to the sound device.
 * Launched bare from the Start menu it opens empty (the transport disabled)
 * since OwOS has no file-open dialog yet — the load path is `player <file>` from
 * the terminal, the same way imgview/play take their argument.
 *
 * Like /bin/play, being a user program is the point: the file is pulled off disk in
 * small reads (a 30 MiB song never lives in memory), decoded/resampled to the
 * device's canonical 48 kHz / 16-bit / stereo here in user space, and pushed to
 * the kernel one bounded chunk at a time — which back-pressures. The whole
 * multi-minute playback is a normal preemptible process, so a decode bug is a
 * contained crash, not a kernel hang. The decode core (decode_next + the 16.16
 * fixed-point resampler) mirrors play.c; the difference is that the chunk loop
 * is driven by the window event loop so the UI stays live during playback.
 *
 * The two new gfx widgets drive the UI: progress_draw for the bar (click it to
 * seek) and button_draw/button_hit for the transport. There are no mouse-move
 * events (WEV_MOUSE is a click), so BTN_HOVER is never used; a click flashes
 * BTN_PRESSED for a few frames, and the empty player draws BTN_DISABLED.
 */
#include "owos.h"
#include "gfx.h"
#include "mp3.h"

/* ---- window geometry ---------------------------------------------------- */
#define CW 440
#define CH 180

#define HDR_H   40                 /* filename header strip                  */
#define BAR_X   24
#define BAR_W   (CW - 2 * BAR_X)
#define BAR_Y   80
#define TIME_Y  (BAR_Y + GUI_PROGRESS_H + 6)
#define BTN_Y   128

/* ---- transport / playback state ----------------------------------------- */
enum { ST_EMPTY = 0, ST_STOPPED, ST_PLAYING, ST_PAUSED };
static int g_state = ST_EMPTY;
static int g_win;

/* Press-flash: the index of the button being shown pressed, and a countdown of
 * render frames it stays pressed for (so a click is visible). */
static int g_press = -1, g_press_ticks;

/* button rectangles, recomputed every draw so the click handler can hit-test
 * exactly what the user sees. */
static int g_bx[3], g_bw[3];
static const char *g_blabel[3];    /* [1] swaps Play/Pause with state */

/* ---- source: WAV stream or MPEG decoder (mirrors play.c) ----------------- */
static int          src_is_mp3;
static const char  *src_fn;
static unsigned     src_size;      /* whole-file size                         */
static unsigned     src_rate;      /* native sample rate, Hz                  */

/* WAV cursor */
static unsigned     w_data_off, w_data_len, w_bits, w_channels, w_frame;
static unsigned     w_filepos;     /* byte cursor into the data region        */
static unsigned char w_in[65536];  /* raw source PCM, refilled by readfile_at */
static unsigned     w_inlen, w_inpos;

/* MPEG decoder */
static mp3_t        m3;

/* shared decode/resample cursor */
static short        curL, curR;    /* current (held) source frame            */
static int          eof;
static unsigned long pos_frames;   /* source frames consumed (for the clock)  */
static unsigned long total_frames; /* WAV: exact; MP3: 0 (unknown)            */
static unsigned     step, acc;     /* 16.16 input-frames-per-output-frame     */
static int          primed;

static unsigned rd_le32(const unsigned char *p)
{ return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }
static unsigned rd_le16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static int tageq(const unsigned char *p, const char *s)
{ return p[0]==s[0] && p[1]==s[1] && p[2]==s[2] && p[3]==s[3]; }

/* Decode the next source frame into curL/curR; sets eof at end of data. */
static void decode_next(void)
{
	if (eof) { curL = curR = 0; return; }

	if (src_is_mp3) {
		short f[2];
		if (mp3_read(&m3, f, 1) <= 0) { eof = 1; curL = curR = 0; return; }
		curL = f[0]; curR = f[1];
		pos_frames++;
		return;
	}

	if (w_inpos + w_frame > w_inlen) {                  /* refill input buffer */
		if (w_filepos >= w_data_len) { eof = 1; curL = curR = 0; return; }
		unsigned want = w_data_len - w_filepos;
		if (want > sizeof w_in) want = sizeof w_in;
		want -= want % w_frame;
		if (want == 0) want = w_frame;
		int got = readfile_at(src_fn, w_data_off + w_filepos, w_in, want);
		if (got <= 0 || (unsigned)got < w_frame) { eof = 1; curL = curR = 0; return; }
		w_inlen = (unsigned)got; w_inpos = 0; w_filepos += (unsigned)got;
	}

	const unsigned char *s = w_in + w_inpos;
	w_inpos += w_frame;
	if (w_bits == 8) {
		int l = ((int)s[0] - 128) << 8;
		curL = (short)l;
		curR = (short)((w_channels == 2) ? (((int)s[1] - 128) << 8) : l);
	} else {
		curL = (short)rd_le16(s);
		curR = (w_channels == 2) ? (short)rd_le16(s + 2) : curL;
	}
	pos_frames++;
}

/* Produce one chunk of `outframes` 48 kHz/S16/stereo frames into out (2 shorts
 * each). Returns frames produced, or 0 at end of stream. acc/step persist across
 * calls so resampling is continuous. */
static unsigned produce_chunk(short *out, unsigned outframes)
{
	if (!primed) { decode_next(); primed = 1; acc = 0; }
	if (eof) return 0;
	unsigned produced = 0;
	while (produced < outframes) {
		out[produced * 2 + 0] = curL;
		out[produced * 2 + 1] = curR;
		produced++;
		if (eof) break;
		acc += step;
		while (acc >= 0x10000u && !eof) { decode_next(); acc -= 0x10000u; }
	}
	return produced;
}

/* Reset the read cursor to source fraction f (0..1), seeking the active source.
 * WAV seeks sample-accurately; MP3 seeks by byte offset (minimp3 resyncs on the
 * next frame). Keeps the current play/pause state. */
static unsigned long est_total_frames(void); /* fwd */

static void seek_fraction(int num, int den)
{
	if (den <= 0) den = 1;
	if (num < 0) num = 0;
	if (num > den) num = den;

	if (src_is_mp3) {
		/* byte-offset seek */
		unsigned off = (unsigned)((unsigned long)src_size * (unsigned)num / (unsigned)den);
		m3.file_off = off;
		m3.in_len   = 0;
		m3.out_pos  = m3.out_frames;   /* force a refill on the next read */
		m3.src_eof  = 0;
		/* Baseline the clock against the running duration estimate, if any. */
		unsigned long tf = est_total_frames();
		pos_frames = tf ? tf * (unsigned)num / (unsigned)den : 0;
	} else {
		unsigned long tgt = total_frames * (unsigned)num / (unsigned)den;
		w_filepos = (unsigned)tgt * w_frame;
		if (w_filepos > w_data_len) w_filepos = w_data_len - (w_data_len % w_frame);
		w_inlen = w_inpos = 0;
		pos_frames = tgt;
	}
	eof = 0;
	primed = 0;          /* re-prime curL/curR from the new position */
	acc = 0;
}

/* WAV: exact total frames. MP3: extrapolate from how far we've decoded so far
 * (bytes consumed vs. file size), or 0 before anything has played. */
static unsigned long est_total_frames(void)
{
	if (!src_is_mp3) return total_frames;
	if (pos_frames == 0 || m3.file_off == 0 || src_size == 0) return 0;
	unsigned elapsed = (unsigned)(pos_frames / (src_rate ? src_rate : 1));
	if (elapsed == 0) return 0;
	unsigned bps = m3.file_off / elapsed;       /* source bytes per second */
	if (bps == 0) return 0;
	return (unsigned long)(src_size / bps) * (src_rate ? src_rate : 1);
}

/* ---- file loading ------------------------------------------------------- */
static int load_wav(const char *fn, unsigned size)
{
	unsigned char hdr[256];
	int got = readfile_at(fn, 0, hdr, sizeof hdr);
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

	src_is_mp3 = 0;
	src_fn = fn; src_size = size; src_rate = rate;
	w_data_off = data_off; w_data_len = data_len; w_filepos = 0;
	w_bits = bits; w_channels = channels; w_frame = channels * (bits / 8);
	w_inlen = w_inpos = 0;
	total_frames = w_frame ? w_data_len / w_frame : 0;
	return 0;
}

static int load_mp3(const char *fn, unsigned size)
{
	if (mp3_open(&m3, fn, size) != 0)
		return -1;
	src_is_mp3 = 1;
	src_fn = fn; src_size = size; src_rate = mp3_rate(&m3);
	total_frames = 0;     /* unknown for MPEG */
	return 0;
}

/* Open `fn`, route by content (RIFF/WAVE -> WAV, else MPEG). Returns 0 on ok. */
static int load_file(const char *fn)
{
	struct statinfo st;
	if (stat(fn, &st) != 0 || st.is_dir)
		return -1;
	unsigned char sn[12];
	int g = readfile_at(fn, 0, sn, sizeof sn);
	int rc;
	if (g >= 12 && tageq(sn, "RIFF") && tageq(sn + 8, "WAVE"))
		rc = load_wav(fn, st.size);
	else
		rc = load_mp3(fn, st.size);
	if (rc != 0)
		return -1;

	if (src_rate < 4000)   src_rate = 4000;
	if (src_rate > 192000) src_rate = 192000;
	step = ((src_rate / 48000u) << 16) + (((src_rate % 48000u) << 16) / 48000u);
	pos_frames = 0;
	eof = 0;
	primed = 0;
	g_state = ST_STOPPED;
	return 0;
}

/* ---- transport ---------------------------------------------------------- */
static int audio_live;

static void transport_play(void)
{
	if (g_state == ST_EMPTY || g_state == ST_PLAYING) return;
	if (audio_open() != 0) return;          /* no output device */
	audio_live = 1;
	g_state = ST_PLAYING;
}

static void transport_pause(void)
{
	if (g_state != ST_PLAYING) return;
	if (audio_live) { audio_close(); audio_live = 0; }
	g_state = ST_PAUSED;                     /* keeps the read cursor */
}

static void transport_stop(void)
{
	if (g_state == ST_EMPTY) return;
	if (audio_live) { audio_close(); audio_live = 0; }
	seek_fraction(0, 1);
	g_state = ST_STOPPED;
}

static void transport_restart(void)
{
	if (g_state == ST_EMPTY) return;
	seek_fraction(0, 1);                     /* back to the start, keep playing */
}

static void transport_toggle(void)
{
	if (g_state == ST_PLAYING) transport_pause();
	else                       transport_play();
}

/* ---- rendering ---------------------------------------------------------- */
#define COL_BG    0x10141cu
#define COL_HDR   0x1a2230u
#define COL_TITLE 0xe6ebf2u
#define COL_DIM   0x8a93a3u

static const char *basename_of(const char *p)
{
	const char *b = p;
	for (const char *q = p; *q; q++)
		if (*q == '/') b = q + 1;
	return b;
}

/* "M:SS" into buf (>=8). Caps at 99:59 so the field never overflows. */
static void fmt_time(unsigned sec, char *buf)
{
	if (sec > 99 * 60 + 59) sec = 99 * 60 + 59;
	unsigned m = sec / 60, s = sec % 60;
	int k = 0;
	if (m >= 10) buf[k++] = '0' + m / 10;
	buf[k++] = '0' + m % 10;
	buf[k++] = ':';
	buf[k++] = '0' + s / 10;
	buf[k++] = '0' + s % 10;
	buf[k] = '\0';
}

static void layout_buttons(void)
{
	g_blabel[0] = "Restart";
	g_blabel[1] = (g_state == ST_PLAYING) ? "Pause" : "Play";
	g_blabel[2] = "Stop";
	int total = 0;
	for (int i = 0; i < 3; i++) {
		int w = gfx_ui_width(g_blabel[i]) + 28;
		if (w < 78) w = 78;
		g_bw[i] = w;
		total += w;
	}
	total += 12 * 2;                          /* two gaps */
	int x = (CW - total) / 2;
	for (int i = 0; i < 3; i++) {
		g_bx[i] = x;
		x += g_bw[i] + 12;
	}
}

static void render(void)
{
	int have = (g_state != ST_EMPTY);

	gfx_clear(COL_BG);

	/* header: filename (or empty hint) */
	gfx_fill_rect(0, 0, CW, HDR_H, COL_HDR);
	const char *title = have ? basename_of(src_fn) : "No file loaded";
	gfx_ui_text((CW - gfx_ui_width(title)) / 2, (HDR_H - UIFONT_H) / 2, title, COL_TITLE);

	if (!have) {
		const char *h1 = "Open a song from the terminal:";
		const char *h2 = "player <file.wav>";
		gfx_ui_text((CW - gfx_ui_width(h1)) / 2, 78, h1, COL_DIM);
		gfx_ui_text((CW - gfx_ui_width(h2)) / 2, 78 + UIFONT_H + 4, h2, COL_TITLE);
	} else {
		/* progress bar + elapsed/total time */
		unsigned long tf = est_total_frames();
		int vnum, vden;
		if (src_is_mp3) {                     /* MP3 bar tracks bytes consumed */
			vnum = (int)(src_size ? m3.file_off : 0);
			vden = (int)(src_size ? src_size : 1);
		} else {
			vnum = (int)(pos_frames > total_frames ? total_frames : pos_frames);
			vden = (int)(total_frames ? total_frames : 1);
		}
		progress_draw(BAR_X, BAR_Y, BAR_W, GUI_PROGRESS_H, vnum, vden, 0);

		char el[8], tt[8];
		fmt_time((unsigned)(pos_frames / (src_rate ? src_rate : 1)), el);
		gfx_ui_text(BAR_X, TIME_Y, el, COL_DIM);
		if (tf) {
			fmt_time((unsigned)(tf / (src_rate ? src_rate : 1)), tt);
		} else {
			tt[0] = '-'; tt[1] = '-'; tt[2] = ':';
			tt[3] = '-'; tt[4] = '-'; tt[5] = '\0';
		}
		gfx_ui_text(CW - BAR_X - gfx_ui_width(tt), TIME_Y, tt, COL_DIM);
	}

	/* transport buttons */
	layout_buttons();
	for (int i = 0; i < 3; i++) {
		int state;
		if (!have) state = BTN_DISABLED;
		else if (g_press == i && g_press_ticks > 0) state = BTN_PRESSED;
		else state = BTN_NORMAL;
		button_draw(g_bx[i], BTN_Y, g_bw[i], GUI_BTN_H, g_blabel[i], state);
	}
	if (g_press_ticks > 0 && --g_press_ticks == 0)
		g_press = -1;
}

/* ---- input -------------------------------------------------------------- */
static void on_click(int mx, int my)
{
	if (g_state == ST_EMPTY)
		return;

	/* seek by clicking the bar (a little vertical slop for easy hits) */
	if (mx >= BAR_X && mx < BAR_X + BAR_W &&
	    my >= BAR_Y - 4 && my < BAR_Y + GUI_PROGRESS_H + 4) {
		seek_fraction(mx - BAR_X, BAR_W);
		return;
	}

	for (int i = 0; i < 3; i++) {
		if (button_hit(g_bx[i], BTN_Y, g_bw[i], GUI_BTN_H, mx, my)) {
			g_press = i; g_press_ticks = 4;
			if      (i == 0) transport_restart();
			else if (i == 1) transport_toggle();
			else             transport_stop();
			return;
		}
	}
}

int main(int argc, char **argv)
{
	g_win = win_create(CW, CH, "Media Player");
	if (g_win < 0) {
		puts("player: could not open a window\n");
		return 1;
	}
	win_resizable(g_win, 0);
	gfx_init(win_fb(), CW, CH);

	if (argc >= 2 && load_file(argv[1]) == 0)
		transport_play();                      /* autoplay the given file */
	else if (argc >= 2)
		puts("player: not a playable audio file\n");

	static short buf[4096];                     /* 2048 stereo S16 frames */
	const unsigned OUT = sizeof(buf) / 4;

	for (;;) {
		struct win_event ev;
		while (win_poll(g_win, &ev)) {
			if (ev.type == WEV_CLOSE) {
				if (audio_live) audio_close();
				win_close(g_win);
				return 0;
			}
			if (ev.type == WEV_MOUSE)
				on_click(ev.a, ev.b);
			else if (ev.type == WEV_KEY && ev.a == ' ')
				transport_toggle();
		}

		if (g_state == ST_PLAYING) {
			unsigned produced = produce_chunk(buf, OUT);
			if (produced == 0) {               /* reached the end */
				transport_stop();
			} else {
				int w = audio_write(buf, (int)(produced * 4));
				if (w < 0)                     /* Ctrl+C (-2) or error (-1) */
					transport_pause();
			}
		}

		render();
		win_present(g_win);
		if (g_state != ST_PLAYING)
			win_wait(g_win);                   /* idle: don't busy-spin */
	}
}
