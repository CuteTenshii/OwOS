/* player.c — a graphical media player for audio and H.264 video.
 *
 * `player <file>` opens a window with a seekable progress bar and Restart /
 * Play-Pause / Stop buttons. An audio file (.wav/.mp3/.caf) streams to the sound
 * device; a video file (.mp4/.mov/.264/.h264) decodes H.264 pictures and blits
 * them, paced against the monotonic clock, into a window sized to the video with
 * the transport strip below it. Launched bare from the Start menu it opens empty
 * (OwOS has no file-open dialog yet) — the load path is `player <file>` from the
 * terminal, like imgview/play.
 *
 * Decoding, resampling and seeking (audio) and H.264 + MP4 demux (video) all live
 * in user/lib/media (shared with the browser's <audio>/<video>): the file is
 * pulled off disk in small reads for audio, held resident for video (the demuxer
 * needs random access). A video MP4's AAC audio track plays too: it is opened as a
 * second media source on the software mixer (its own thread, so it never blocks
 * the frame loop) and becomes the master clock — video frames are paced to the
 * audio position, so picture and sound stay in sync. A raw .264/.h264 has no audio
 * track and plays silently. Software H.264 decode is best-effort real-time: if a
 * frame is already overdue when decoded it shows at once, so slow decode degrades
 * to a lower effective frame rate rather than drifting off-clock.
 */
#include "owos.h"
#include "gfx.h"
#include "media.h"

/* ---- audio-window geometry ---------------------------------------------- */
#define CW 440
#define CH 180
#define HDR_H   40
#define BAR_X   24
#define BAR_Y   80
#define TIME_Y  (BAR_Y + GUI_PROGRESS_H + 6)
#define BTN_Y   128

/* ---- video display limits ----------------------------------------------- */
#define VID_MAXW 900
#define VID_MAXH 560
#define CTRL_H   96                 /* transport strip below the video        */
#define VID_MINW 360

/* ---- transport / playback state ----------------------------------------- */
enum { ST_EMPTY = 0, ST_STOPPED, ST_PLAYING, ST_PAUSED };
static int g_state = ST_EMPTY;
static int g_win;
static int g_W = CW, g_H = CH;       /* actual window client size              */
static int g_is_video;

static int g_press = -1, g_press_ticks;
static int g_bx[3], g_bw[3];
static const char *g_blabel[3];

static char g_fn[256];

/* ---- audio source ------------------------------------------------------- */
static media_decoder *g_dec;
static int audio_live;

/* A video file's AAC audio track, played on the software mixer (its own thread)
 * and used as the A/V master clock. NULL for a video with no decodable audio. */
static media_source *g_asrc;

/* ---- video source ------------------------------------------------------- */
static media_video  *g_vid;
static unsigned char *g_rgb;         /* native-size decoded frame (vw*vh*3)    */
static int g_vw, g_vh;               /* native video dimensions                */
static int g_dx, g_dy, g_dw, g_dh;   /* on-screen video rectangle              */
static int g_frame_valid;            /* g_rgb holds a decoded, unshown frame   */
static unsigned g_frame_pts;         /* its presentation time (ms)             */
static long long g_shown_pts;        /* pts of the frame currently on screen   */
static long long g_clock_origin;     /* msec() - playback position (ms)        */

/* ---- basename + time formatting ----------------------------------------- */
static const char *basename_of(const char *p)
{
	const char *b = p;
	for (const char *q = p; *q; q++)
		if (*q == '/') b = q + 1;
	return b;
}

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

static int is_video_name(const char *p)
{
	int n = strlen(p);
	const char *ext[] = { ".mp4", ".mov", ".m4v", ".264", ".h264" };
	for (int i = 0; i < 5; i++) {
		int l = strlen(ext[i]);
		if (n >= l && streq_ci(p + n - l, ext[i])) return 1;
	}
	return 0;
}

/* ---- video frame blit --------------------------------------------------- */
/* Nearest-neighbour scale the native g_rgb frame into the on-screen rectangle. */
static void blit_video(void)
{
	unsigned *fb = win_fb();
	int py, px;
	for (py = 0; py < g_dh; py++) {
		int sy = py * g_vh / g_dh;
		const unsigned char *srow = g_rgb + (unsigned)sy * g_vw * 3;
		unsigned *drow = fb + (unsigned)(g_dy + py) * g_W + g_dx;
		for (px = 0; px < g_dw; px++) {
			const unsigned char *p = srow + (px * g_vw / g_dw) * 3;
			drow[px] = 0xFF000000u | ((unsigned)p[0] << 16) | ((unsigned)p[1] << 8) | p[2];
		}
	}
}

/* ---- loading ------------------------------------------------------------ */
static void copy_name(const char *fn)
{
	int i;
	for (i = 0; i < (int)sizeof g_fn - 1 && fn[i]; i++) g_fn[i] = fn[i];
	g_fn[i] = '\0';
}

/* Compute the on-screen video rectangle + window size from the native size. */
static void layout_video(void)
{
	int sx = VID_MAXW * 256 / g_vw, sy = VID_MAXH * 256 / g_vh;
	int s = sx < sy ? sx : sy;
	if (s > 3 * 256) s = 3 * 256;                 /* cap upscaling of tiny clips */
	g_dw = g_vw * s / 256;
	g_dh = g_vh * s / 256;
	g_W = g_dw < VID_MINW ? VID_MINW : g_dw;
	g_H = g_dh + CTRL_H;
	g_dx = (g_W - g_dw) / 2;
	g_dy = 0;
}

/* (Re)open the video decoder and decode its first frame. Returns 0/err. */
static int video_reopen(void)
{
	media_video *v;
	if (g_vid) { media_video_close(g_vid); g_vid = 0; }
	v = media_video_open(g_fn);
	if (!v) return -1;
	g_vid = v;
	g_frame_valid = 0;
	g_shown_pts = -1;
	if (media_video_next(g_vid, g_rgb, &g_frame_pts) == 1) {
		g_frame_valid = 1;                        /* first frame, shown on entry */
	}
	return 0;
}

static int load_audio(const char *fn)
{
	media_decoder *d = media_open(fn);
	if (!d) return -1;
	if (g_dec) media_close(g_dec);
	g_dec = d;
	copy_name(fn);
	g_is_video = 0;
	g_state = ST_STOPPED;
	return 0;
}

static int load_video(const char *fn)
{
	media_video *v = media_video_open(fn);
	if (!v) return -1;
	g_vw = media_video_width(v);
	g_vh = media_video_height(v);
	media_video_close(v);                          /* reopened after we size things */
	if (g_vw < 1 || g_vh < 1) return -1;
	g_rgb = malloc((unsigned)g_vw * g_vh * 3);
	if (!g_rgb) return -1;
	copy_name(fn);
	g_is_video = 1;
	layout_video();
	if (video_reopen() != 0) return -1;
	/* Same container's audio track (AAC in an MP4), played via the mixer thread
	 * so it never blocks the frame loop. NULL for a raw .264 with no audio. */
	{
		media_decoder *ad = media_open(fn);
		g_asrc = ad ? media_source_new(ad, 0, 0) : 0;
	}
	g_state = ST_STOPPED;
	return 0;
}

/* The video playback clock in ms: the audio track's position when there is one
 * (so picture follows sound), else the monotonic wall clock. */
static long long video_clock_ms(void)
{
	if (g_asrc) return (long long)media_source_position_ms(g_asrc);
	return (long long)msec() - g_clock_origin;
}

/* ---- transport ---------------------------------------------------------- */
static void audio_seek_fraction(int num, int den)
{
	unsigned dur = media_duration_ms(g_dec);
	if (den <= 0) den = 1;
	if (num < 0) num = 0;
	if (num > den) num = den;
	media_seek_ms(g_dec, dur ? (unsigned)((unsigned long long)dur * (unsigned)num / (unsigned)den) : 0);
}

static void transport_play(void)
{
	if (g_state == ST_EMPTY || g_state == ST_PLAYING) return;
	if (g_is_video) {
		if (g_shown_pts < 0) g_shown_pts = 0;
		g_clock_origin = (long long)msec() - g_shown_pts;    /* resume the clock */
		if (g_asrc) media_source_play(g_asrc);
		g_state = ST_PLAYING;
		return;
	}
	if (audio_open() != 0) return;
	audio_live = 1;
	g_state = ST_PLAYING;
}

static void transport_pause(void)
{
	if (g_state != ST_PLAYING) return;
	if (!g_is_video && audio_live) { audio_close(); audio_live = 0; }
	if (g_is_video && g_asrc) media_source_pause(g_asrc);
	g_state = ST_PAUSED;
}

static void transport_stop(void)
{
	if (g_state == ST_EMPTY) return;
	if (g_is_video) {
		if (g_asrc) { media_source_pause(g_asrc); media_source_seek_ms(g_asrc, 0); }
		video_reopen();                            /* back to frame 0 */
		g_state = ST_STOPPED;
		return;
	}
	if (audio_live) { audio_close(); audio_live = 0; }
	media_seek_ms(g_dec, 0);
	g_state = ST_STOPPED;
}

static void transport_restart(void)
{
	if (g_state == ST_EMPTY) return;
	if (g_is_video) {
		int was_playing = (g_state == ST_PLAYING);
		video_reopen();
		g_clock_origin = msec();
		if (g_asrc) {
			media_source_seek_ms(g_asrc, 0);
			if (was_playing) media_source_play(g_asrc);
			else             media_source_pause(g_asrc);
		}
		if (was_playing) g_state = ST_PLAYING;
		return;
	}
	media_seek_ms(g_dec, 0);
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
#define COL_VIDBG 0x000000u

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
	total += 12 * 2;
	int x = (g_W - total) / 2;
	for (int i = 0; i < 3; i++) { g_bx[i] = x; x += g_bw[i] + 12; }
}

/* Draw the progress bar + time + buttons of the transport strip starting at
 * client-y `top`, over a filled background `bg` spanning `top`..window bottom. */
static void draw_controls(int top, unsigned bg, unsigned pos_ms, unsigned dur_ms)
{
	int bar_y = top + 14, time_y = bar_y + GUI_PROGRESS_H + 6;
	int btn_y = g_H - GUI_BTN_H - 14;
	char el[8], tt[8];
	gfx_fill_rect(0, top, g_W, g_H - top, bg);
	progress_draw(BAR_X, bar_y, g_W - 2 * BAR_X, GUI_PROGRESS_H,
	              (int)(dur_ms && pos_ms > dur_ms ? dur_ms : pos_ms), (int)(dur_ms ? dur_ms : 1), 0);
	fmt_time(pos_ms / 1000, el);
	gfx_ui_text(BAR_X, time_y, el, COL_DIM);
	if (dur_ms) { fmt_time(dur_ms / 1000, tt); }
	else { tt[0] = '-'; tt[1] = '-'; tt[2] = ':'; tt[3] = '-'; tt[4] = '-'; tt[5] = '\0'; }
	gfx_ui_text(g_W - BAR_X - gfx_ui_width(tt), time_y, tt, COL_DIM);

	layout_buttons();
	for (int i = 0; i < 3; i++) {
		int st = (g_press == i && g_press_ticks > 0) ? BTN_PRESSED : BTN_NORMAL;
		button_draw(g_bx[i], btn_y, g_bw[i], GUI_BTN_H, g_blabel[i], st);
	}
	if (g_press_ticks > 0 && --g_press_ticks == 0) g_press = -1;
}

static void render_audio(void)
{
	int have = (g_state != ST_EMPTY);
	gfx_clear(COL_BG);
	gfx_fill_rect(0, 0, g_W, HDR_H, COL_HDR);
	const char *title = have ? basename_of(g_fn) : "No file loaded";
	gfx_ui_text((g_W - gfx_ui_width(title)) / 2, (HDR_H - UIFONT_H) / 2, title, COL_TITLE);

	if (!have) {
		const char *h1 = "Open a song or video from the terminal:";
		const char *h2 = "player <file.wav | file.mp4>";
		gfx_ui_text((g_W - gfx_ui_width(h1)) / 2, 78, h1, COL_DIM);
		gfx_ui_text((g_W - gfx_ui_width(h2)) / 2, 78 + UIFONT_H + 4, h2, COL_TITLE);
		layout_buttons();
		for (int i = 0; i < 3; i++)
			button_draw(g_bx[i], BTN_Y, g_bw[i], GUI_BTN_H, g_blabel[i], BTN_DISABLED);
		return;
	}
	unsigned pos = media_position_ms(g_dec), dur = media_duration_ms(g_dec);
	int bar_y = BAR_Y;
	gfx_fill_rect(0, HDR_H, g_W, g_H - HDR_H, COL_BG);
	progress_draw(BAR_X, bar_y, g_W - 2 * BAR_X, GUI_PROGRESS_H,
	              (int)(dur && pos > dur ? dur : pos), (int)(dur ? dur : 1), 0);
	char el[8], tt[8];
	fmt_time(pos / 1000, el);
	gfx_ui_text(BAR_X, TIME_Y, el, COL_DIM);
	if (dur) fmt_time(dur / 1000, tt);
	else { tt[0] = '-'; tt[1] = '-'; tt[2] = ':'; tt[3] = '-'; tt[4] = '-'; tt[5] = '\0'; }
	gfx_ui_text(g_W - BAR_X - gfx_ui_width(tt), TIME_Y, tt, COL_DIM);
	layout_buttons();
	for (int i = 0; i < 3; i++) {
		int st = (g_press == i && g_press_ticks > 0) ? BTN_PRESSED : BTN_NORMAL;
		button_draw(g_bx[i], BTN_Y, g_bw[i], GUI_BTN_H, g_blabel[i], st);
	}
	if (g_press_ticks > 0 && --g_press_ticks == 0) g_press = -1;
}

static void render_video_controls(void)
{
	unsigned pos = g_shown_pts < 0 ? 0 : (unsigned)g_shown_pts;
	draw_controls(g_dh, COL_HDR, pos, media_video_duration_ms(g_vid));
}

/* ---- input -------------------------------------------------------------- */
static void on_click(int mx, int my)
{
	if (g_state == ST_EMPTY) return;

	if (!g_is_video &&
	    mx >= BAR_X && mx < g_W - BAR_X &&
	    my >= BAR_Y - 4 && my < BAR_Y + GUI_PROGRESS_H + 4) {
		audio_seek_fraction(mx - BAR_X, g_W - 2 * BAR_X);
		return;
	}

	int btn_y = g_is_video ? g_H - GUI_BTN_H - 14 : BTN_Y;
	for (int i = 0; i < 3; i++) {
		if (button_hit(g_bx[i], btn_y, g_bw[i], GUI_BTN_H, mx, my)) {
			g_press = i; g_press_ticks = 4;
			if      (i == 0) transport_restart();
			else if (i == 1) transport_toggle();
			else             transport_stop();
			return;
		}
	}
}

/* ---- video playback step ------------------------------------------------ */
/* Advance the video: decode one frame ahead and show it once its time is due.
 * Returns 1 if a new frame was blitted this step. */
static int video_step(void)
{
	int blitted = 0;
	if (g_state == ST_PLAYING) {
		if (!g_frame_valid) {
			int r = media_video_next(g_vid, g_rgb, &g_frame_pts);
			if (r == 1) g_frame_valid = 1;
			else { transport_pause(); }       /* EOS / error: hold on last frame */
		}
		if (g_frame_valid) {
			long long elapsed = video_clock_ms();
			if ((long long)g_frame_pts <= elapsed) {
				blit_video();
				g_shown_pts = g_frame_pts;
				g_frame_valid = 0;
				blitted = 1;
			}
		}
	} else if (g_frame_valid) {
		/* stopped/paused with a freshly decoded first frame: show it once */
		blit_video();
		g_shown_pts = g_frame_pts;
		g_frame_valid = 0;
		blitted = 1;
	}
	return blitted;
}

int main(int argc, char **argv)
{
	if (argc >= 2 && is_video_name(argv[1]) && load_video(argv[1]) == 0) {
		/* window sized to the video below */
	} else if (argc >= 2 && load_audio(argv[1]) == 0) {
		g_W = CW; g_H = CH;
	} else {
		g_W = CW; g_H = CH;
		if (argc >= 2) puts("player: not a playable media file\n");
	}

	g_win = win_create(g_W, g_H, g_is_video ? "Video Player" : "Media Player");
	if (g_win < 0) { puts("player: could not open a window\n"); return 1; }
	win_resizable(g_win, 0);
	gfx_init(win_fb(), g_W, g_H);

	if (g_state == ST_STOPPED) transport_play();       /* autoplay the given file */

	static short buf[4096];
	const unsigned OUT = sizeof(buf) / 4;

	for (;;) {
		struct win_event ev;
		int new_frame = 0;
		while (win_poll(g_win, &ev)) {
			if (ev.type == WEV_CLOSE) {
				if (audio_live) audio_close();
				if (g_asrc) media_source_free(g_asrc);   /* mixer frees its decoder */
				if (g_dec) media_close(g_dec);
				if (g_vid) media_video_close(g_vid);
				free(g_rgb);
				win_close(g_win);
				return 0;
			}
			if (ev.type == WEV_MOUSE) on_click(ev.a, ev.b);
			else if (ev.type == WEV_KEY && ev.a == ' ') transport_toggle();
		}

		if (g_is_video) {
			new_frame = video_step();
			render_video_controls();
			win_present(g_win);
			if (g_state == ST_PLAYING && !new_frame) msleep(3);
			else if (g_state != ST_PLAYING) win_wait(g_win);
			continue;
		}

		if (g_state == ST_PLAYING) {
			unsigned produced = media_read(g_dec, buf, (int)OUT);
			if (produced == 0) {
				transport_stop();
			} else {
				char *p = (char *)buf;
				int total = (int)(produced * 4), off = 0;
				while (off < total) {
					int w = audio_write(p + off, total - off);
					if (w < 0) { transport_pause(); break; }
					off += w;
				}
			}
		}
		render_audio();
		win_present(g_win);
		if (g_state != ST_PLAYING) win_wait(g_win);
	}
}
