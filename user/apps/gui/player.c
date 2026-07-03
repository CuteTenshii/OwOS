/* player.c — a graphical media player.
 *
 * `player <file.wav|.mp3|.caf>` opens a window with a seekable progress bar and
 * Restart / Play-Pause / Stop buttons, and streams the song to the sound device.
 * Launched bare from the Start menu it opens empty (the transport disabled)
 * since OwOS has no file-open dialog yet — the load path is `player <file>` from
 * the terminal, the same way imgview/play take their argument.
 *
 * Decoding, resampling to the device's 48 kHz / 16-bit / stereo, and seeking all
 * live in user/lib/media (shared with the browser's <audio>/<video>): the file is
 * pulled off disk in small reads (a 30 MiB song never lives in memory), so the
 * whole multi-minute playback is a normal preemptible process and a decode bug is
 * a contained crash, not a kernel hang. The player keeps the kernel audio stream
 * itself (a single foreground song needs no mixing) and drives the decode->write
 * chunk loop from the window event loop, so the UI stays live during playback.
 *
 * The two gfx widgets drive the UI: progress_draw for the bar (click it to seek)
 * and button_draw/button_hit for the transport. There are no mouse-move events
 * (WEV_MOUSE is a click), so BTN_HOVER is never used; a click flashes BTN_PRESSED
 * for a few frames, and the empty player draws BTN_DISABLED.
 */
#include "owos.h"
#include "gfx.h"
#include "media.h"

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

/* ---- source: the shared media decoder ----------------------------------- */
static media_decoder *g_dec;
static char           g_fn[256];   /* loaded file path, for the header title  */

/* ---- file loading ------------------------------------------------------- */
static int load_file(const char *fn)
{
	media_decoder *d = media_open(fn);
	int i;
	if (!d)
		return -1;
	if (g_dec) media_close(g_dec);
	g_dec = d;
	for (i = 0; i < (int)sizeof g_fn - 1 && fn[i]; i++) g_fn[i] = fn[i];
	g_fn[i] = '\0';
	g_state = ST_STOPPED;
	return 0;
}

/* Seek to source fraction num/den (0..1), clamped. Needs a known duration; an
 * MPEG stream's duration sharpens after a little has played. */
static void seek_fraction(int num, int den)
{
	unsigned dur = media_duration_ms(g_dec);
	if (den <= 0) den = 1;
	if (num < 0) num = 0;
	if (num > den) num = den;
	if (dur)
		media_seek_ms(g_dec, (unsigned)((unsigned long long)dur * (unsigned)num / (unsigned)den));
	else
		media_seek_ms(g_dec, 0);
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
	media_seek_ms(g_dec, 0);
	g_state = ST_STOPPED;
}

static void transport_restart(void)
{
	if (g_state == ST_EMPTY) return;
	media_seek_ms(g_dec, 0);                 /* back to the start, keep playing */
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
	const char *title = have ? basename_of(g_fn) : "No file loaded";
	gfx_ui_text((CW - gfx_ui_width(title)) / 2, (HDR_H - UIFONT_H) / 2, title, COL_TITLE);

	if (!have) {
		const char *h1 = "Open a song from the terminal:";
		const char *h2 = "player <file.wav>";
		gfx_ui_text((CW - gfx_ui_width(h1)) / 2, 78, h1, COL_DIM);
		gfx_ui_text((CW - gfx_ui_width(h2)) / 2, 78 + UIFONT_H + 4, h2, COL_TITLE);
	} else {
		/* progress bar + elapsed/total time, both in playback milliseconds */
		unsigned pos = media_position_ms(g_dec);
		unsigned dur = media_duration_ms(g_dec);
		progress_draw(BAR_X, BAR_Y, BAR_W, GUI_PROGRESS_H,
		              (int)(dur && pos > dur ? dur : pos), (int)(dur ? dur : 1), 0);

		char el[8], tt[8];
		fmt_time(pos / 1000, el);
		gfx_ui_text(BAR_X, TIME_Y, el, COL_DIM);
		if (dur) {
			fmt_time(dur / 1000, tt);
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
				if (g_dec) media_close(g_dec);
				win_close(g_win);
				return 0;
			}
			if (ev.type == WEV_MOUSE)
				on_click(ev.a, ev.b);
			else if (ev.type == WEV_KEY && ev.a == ' ')
				transport_toggle();
		}

		if (g_state == ST_PLAYING) {
			unsigned produced = media_read(g_dec, buf, (int)OUT);
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
