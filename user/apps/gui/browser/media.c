/* media.c — the browser's <audio>/<video> + HTMLMediaElement layer (bmedia.h).
 *
 * A media element is a registry entry tying a DOM node to a user/lib/media mixer
 * source. The flow:
 *   - html.c / the Audio constructor register an element and set its src.
 *   - a load is kicked (autoplay, .load(), or the first play()): the loader thread
 *     fetches the file, writes it to a temp owofs path, opens it with media_open,
 *     and hands it to the mixer as a source.
 *   - the mixer plays/seeks/loops it; the worker's media_pump turns the source's
 *     state changes into DOM media events (the mixer never touches the JS engine).
 *   - main.c lays out and draws the controls widget and routes clicks to play/seek.
 *
 * Only WAV and MPEG audio decode; <video> is a full media element (controls, JS
 * API, audio track) drawn as a black frame with a play glyph — there is no video
 * codec, so frames aren't decoded.
 */
#include "browser.h"
#include "media.h"        /* user/lib/media: decoder + software mixer */
#include "bmedia.h"

#define MK_AUDIO 0
#define MK_VIDEO 1

#define MEDIA_MAX 24

/* queued events, dispatched by media_pump on the worker thread */
#define MEV_LOADEDMETA 0x01
#define MEV_PLAY       0x02
#define MEV_PAUSE      0x04
#define MEV_ENDED      0x08
#define MEV_TIMEUPDATE 0x10
#define MEV_VOLCHANGE  0x20
#define MEV_SEEKED     0x40
#define MEV_ERROR      0x80

struct media_el {
	int               in_use;
	void             *owner;          /* the struct page that created it          */
	struct dom_node  *node;           /* the <audio>/<video> element              */
	struct dom_node  *detached;       /* a node WE created (new Audio), to free    */
	int               kind;           /* MK_AUDIO / MK_VIDEO                       */

	char              src[URL_MAX];   /* resolved absolute current source ("" = none) */
	char              poster[URL_MAX];
	int               controls, autoplay, loop, muted, preload_none;
	int               volume;         /* 0..1000 (milli-units of HTMLMediaElement) */

	int               registered;     /* first walk done: keep JS-mutated state     */
	int               text_pos;       /* widget anchor offset, or -1 = no widget   */
	int               start_row, rows;/* widget placement (main.c rewrap)          */

	struct media_source *ms;          /* mixer source once loaded                  */
	char              file[64];       /* temp owofs path the media is fetched to   */
	volatile int      net;            /* MNS_* network state                       */
	volatile int      fetching;       /* loader thread is on this slot             */
	int               want_play;      /* play requested before the source existed  */
	unsigned          start_ms;       /* pending currentTime applied on load       */
	int               load_gen;       /* bumped per src change (stale-load guard)   */

	volatile unsigned ev;             /* queued MEV_* bits (worker drains)         */
	int               reported_meta;
	int               reported_ended;
	int               playing_state;  /* last play/pause we told script about      */
	unsigned          last_tu;        /* msec of the last timeupdate               */
};

static struct media_el g_media[MEDIA_MAX];

/* ---- helpers ------------------------------------------------------------- */
static void mstrcpy(char *d, const char *s, int cap)
{ int i = 0; for (; s[i] && i < cap - 1; i++) d[i] = s[i]; d[i] = '\0'; }

static int vol256_of(const struct media_el *m)
{ return m->muted ? 0 : m->volume * 256 / 1000; }

/* Apply the element's volume/mute/loop to its live mixer source. */
static void apply_audio_params(struct media_el *m)
{
	if (!m->ms) return;
	media_source_set_volume(m->ms, vol256_of(m));
	media_source_set_loop(m->ms, m->loop);
}

/* ---- the loader thread: fetch + open one source at a time ---------------- */
static char         g_loader_stack[131072];
static volatile int g_loader_started;

/* Claim the next slot waiting to load (MNS_LOADING, no source yet). */
static struct media_el *loader_pick(void)
{
	struct media_el *job = 0;
	int i;
	blk_lock();
	for (i = 0; i < MEDIA_MAX; i++) {
		struct media_el *m = &g_media[i];
		if (m->in_use && m->net == MNS_LOADING && !m->fetching && !m->ms && m->src[0]) {
			m->fetching = 1;
			job = m;
			break;
		}
	}
	blk_unlock();
	return job;
}

static void loader_run(struct media_el *job)
{
	char src[URL_MAX], file[64];
	int gen, len = 0, status = 0;
	unsigned char *body;
	media_decoder *d = 0;
	struct media_source *s = 0;

	blk_lock();
	mstrcpy(src, job->src, sizeof src);
	mstrcpy(file, job->file, sizeof file);
	gen = job->load_gen;
	blk_unlock();

	{
		struct http_req rq = { 0 };
		struct http_resp rs = { 0 };
		rq.url = src; rq.accept = "audio/*,video/*,*/*";
		body = browser_fetch(&rq, &rs);
		status = rs.status; len = rs.len;
	}
	if (body && len > 0 && status >= 200 && status < 300) {
		if (writefile(file, body, len) == 0)
			d = media_open(file);
	}
	if (body) free(body);
	if (d) s = media_source_new(d, 0, 0);   /* takes ownership of d */

	blk_lock();
	if (job->load_gen != gen) {              /* a newer src superseded this load */
		if (s) media_source_free(s);
	} else if (s) {
		job->ms = s;
		job->net = MNS_IDLE;
		media_source_set_volume(s, vol256_of(job));
		media_source_set_loop(s, job->loop);
		if (job->start_ms) media_source_seek_ms(s, job->start_ms);
		job->ev |= MEV_LOADEDMETA;
		if (job->want_play) { media_source_play(s); job->want_play = 0; job->ev |= MEV_PLAY; }
	} else {
		job->net = MNS_NO_SOURCE;
		job->ev |= MEV_ERROR;
	}
	job->fetching = 0;
	blk_unlock();
	B->seq++;                                /* request a repaint */
}

static void loader_thread(void)
{
	for (;;) {
		struct media_el *job = loader_pick();
		if (!job) { msleep(20); continue; }
		loader_run(job);
	}
}

static void loader_ensure(void)
{
	blk_lock();
	if (!g_loader_started) {
		g_loader_started = 1;
		thread_create(loader_thread, &g_loader_stack[sizeof g_loader_stack]);
	}
	blk_unlock();
}

/* Begin (or restart) loading this element's current source. */
static void media_kick_load(struct media_el *m)
{
	if (!m->src[0] || m->ms || m->preload_none) return;
	loader_ensure();
	m->net = MNS_LOADING;
}

/* ---- registry ------------------------------------------------------------ */
static struct media_el *slot_alloc(struct dom_node *node, int kind)
{
	struct media_el *m = 0;
	int i;
	blk_lock();
	for (i = 0; i < MEDIA_MAX; i++) if (!g_media[i].in_use) { m = &g_media[i]; break; }
	if (m) {
		memset(m, 0, sizeof *m);
		m->owner = B->pg;
		m->node = node;
		m->kind = kind;
		m->text_pos = -1;
		m->start_row = -1;
		m->volume = 1000;
		m->net = MNS_EMPTY;
		{ int k = 0; sapp(m->file, &k, sizeof m->file, "/home/root/.mediacache");
		  uapp(m->file, &k, sizeof m->file, (unsigned)(m - g_media)); }
		__asm__ volatile ("" ::: "memory");
		m->in_use = 1;
	}
	blk_unlock();
	return m;
}

struct media_el *media_for_node(struct dom_node *node)
{
	int i;
	if (!node) return 0;
	for (i = 0; i < MEDIA_MAX; i++)
		if (g_media[i].in_use && g_media[i].node == node && g_media[i].owner == B->pg)
			return &g_media[i];
	/* createElement('audio'/'video') reached a media property before registration */
	{
		int kind = streq(node->tag, "video") ? MK_VIDEO : MK_AUDIO;
		return slot_alloc(node, kind);
	}
}

struct media_el *media_new_audio(struct dom_node *node)
{
	struct media_el *m = slot_alloc(node, MK_AUDIO);
	if (m) m->detached = node;     /* WE own this node; free it at page teardown */
	return m;
}

/* ---- source selection (<source> children + src attr) --------------------- */
/* A type string's playability: WAV/MPEG audio is decodable, other audio/video is
 * a maybe (we still try — the container may carry a stream we can read). */
int media_can_play_type(const char *type)
{
	if (!type || !type[0]) return MEDIA_MAYBE;
	if (starts_ci(type, "audio/wav") || starts_ci(type, "audio/x-wav") ||
	    starts_ci(type, "audio/mpeg") || starts_ci(type, "audio/mp3") ||
	    starts_ci(type, "audio/mpa"))
		return MEDIA_PROBABLY;
	if (starts_ci(type, "audio/")) return MEDIA_MAYBE;
	if (starts_ci(type, "video/")) return MEDIA_MAYBE;   /* audio track only */
	return MEDIA_CANT;
}

static int url_ext_playable(const char *url)
{
	int n = strlen(url), i;
	char ext[8]; int e = 0;
	for (i = n - 1; i >= 0 && url[i] != '/' && url[i] != '?' && e < 6; i--)
		if (url[i] == '.') break;
	if (i < 0 || url[i] != '.') return MEDIA_MAYBE;
	for (int j = i + 1; url[j] && url[j] != '?' && e < (int)sizeof ext - 1; j++) ext[e++] = url[j];
	ext[e] = '\0';
	if (streq_ci(ext, "wav") || streq_ci(ext, "mp3") || streq_ci(ext, "mp2") ||
	    streq_ci(ext, "mpa") || streq_ci(ext, "mpeg"))
		return MEDIA_PROBABLY;
	return MEDIA_MAYBE;
}

/* Pick the best source for a media node: its own src, else the best <source>
 * child (a recognized type/extension wins). Resolves against page_url. */
static int pick_source(struct dom_node *node, const char *page_url, char *out, int cap)
{
	const char *src = dom_get_attr(node, "src");
	struct dom_node *c;
	char best[URL_MAX]; int best_rank = -1;
	best[0] = '\0';
	if (src && *src) { resolve_url(page_url, src, out, cap); return out[0] != '\0'; }
	for (c = node->first_child; c; c = c->next) {
		if (c->type != DOM_ELEMENT || !streq(c->tag, "source")) continue;
		const char *ssrc = dom_get_attr(c, "src");
		const char *type = dom_get_attr(c, "type");
		int rank;
		if (!ssrc || !*ssrc) continue;
		rank = type ? media_can_play_type(type) : 0;
		if (rank == MEDIA_CANT) continue;
		{ char abs[URL_MAX]; resolve_url(page_url, ssrc, abs, sizeof abs);
		  if (rank == 0) rank = url_ext_playable(abs);
		  if (rank > best_rank) { best_rank = rank; mstrcpy(best, abs, sizeof best); } }
	}
	if (best[0]) { mstrcpy(out, best, cap); return 1; }
	return 0;
}

/* ---- registration -------------------------------------------------------- */
struct media_el *media_register_element(struct dom_node *node, int text_pos,
                                        const char *page_url)
{
	struct media_el *m = media_for_node(node);
	char abs[URL_MAX];
	if (!m) return 0;

	/* The render walk re-runs on every relayout. Visual attributes (which widget,
	 * where it anchors) refresh each time; playback state (autoplay/loop/muted/src)
	 * is read once so a later JS mutation (audio.loop = ...) isn't clobbered. */
	m->kind = streq(node->tag, "video") ? MK_VIDEO : MK_AUDIO;
	m->text_pos = text_pos;
	m->controls = dom_get_attr(node, "controls") != 0;

	if (!m->registered) {
		const char *poster, *pl;
		m->registered = 1;
		m->autoplay = dom_get_attr(node, "autoplay") != 0;
		m->loop     = dom_get_attr(node, "loop") != 0;
		m->muted    = dom_get_attr(node, "muted") != 0;
		pl = dom_get_attr(node, "preload");
		m->preload_none = pl && streq_ci(pl, "none");
		poster = dom_get_attr(node, "poster");
		if (poster && *poster) resolve_url(page_url, poster, m->poster, sizeof m->poster);
		if (pick_source(node, page_url, abs, sizeof abs)) media_set_src(m, abs);
		apply_audio_params(m);
		if (m->autoplay && m->src[0]) { m->want_play = 1; media_kick_load(m); }
	} else if (pick_source(node, page_url, abs, sizeof abs)) {
		media_set_src(m, abs);             /* no-op unless the src attribute changed */
	}
	return m;
}

/* ---- JS-facing operations ------------------------------------------------ */
void media_set_src(struct media_el *m, const char *abs_url)
{
	if (!m || !abs_url) return;
	if (streq(m->src, abs_url)) return;                   /* unchanged */
	blk_lock();
	mstrcpy(m->src, abs_url, sizeof m->src);
	if (m->ms) { media_source_free(m->ms); m->ms = 0; }
	m->load_gen++;
	m->reported_meta = 0; m->reported_ended = 0; m->want_play = 0;
	m->start_ms = 0;
	m->net = m->src[0] ? MNS_IDLE : MNS_EMPTY;
	blk_unlock();
}

void media_get_src(struct media_el *m, char *out, int cap)
{ mstrcpy(out, m ? m->src : "", cap); }

void media_load(struct media_el *m) { if (m) media_kick_load(m); }

void media_play(struct media_el *m)
{
	if (!m) return;
	blk_lock();
	if (m->ms) {
		if (m->reported_ended) { media_source_seek_ms(m->ms, 0); m->reported_ended = 0; }
		media_source_play(m->ms);
		if (!m->playing_state) { m->ev |= MEV_PLAY; m->playing_state = 1; }
	} else {
		m->want_play = 1;
	}
	blk_unlock();
	if (!m->ms) media_kick_load(m);
}

void media_pause(struct media_el *m)
{
	if (!m) return;
	blk_lock();
	m->want_play = 0;
	if (m->ms) media_source_pause(m->ms);
	if (m->playing_state) { m->ev |= MEV_PAUSE; m->playing_state = 0; }
	blk_unlock();
}

int media_paused(struct media_el *m)
{ return m ? !(m->ms && media_source_playing(m->ms)) : 1; }

int media_ended(struct media_el *m)
{ return m && m->ms ? media_source_ended(m->ms) : 0; }

double media_current_time(struct media_el *m)
{
	if (!m) return 0.0;
	if (m->ms) return (double)media_source_position_ms(m->ms) / 1000.0;
	return (double)m->start_ms / 1000.0;
}

void media_set_current_time(struct media_el *m, double sec)
{
	unsigned ms;
	if (!m) return;
	if (sec < 0) sec = 0;
	ms = (unsigned)(sec * 1000.0);
	if (m->ms) {
		media_source_seek_ms(m->ms, ms);
		m->reported_ended = 0;
		m->ev |= MEV_SEEKED | MEV_TIMEUPDATE;
	} else {
		m->start_ms = ms;
	}
}

double media_duration(struct media_el *m)
{
	unsigned d = (m && m->ms) ? media_source_duration_ms(m->ms) : 0;
	if (!d) return __builtin_nan("");          /* unknown duration is NaN */
	return (double)d / 1000.0;
}

double media_volume(struct media_el *m) { return m ? (double)m->volume / 1000.0 : 1.0; }

void media_set_volume(struct media_el *m, double v)
{
	int nv;
	if (!m) return;
	if (v < 0) v = 0;
	if (v > 1) v = 1;
	nv = (int)(v * 1000.0 + 0.5);
	if (nv == m->volume) return;
	m->volume = nv;
	apply_audio_params(m);
	m->ev |= MEV_VOLCHANGE;
}

int media_muted(struct media_el *m) { return m ? m->muted : 0; }

void media_set_muted(struct media_el *m, int on)
{
	if (!m || m->muted == (on ? 1 : 0)) return;
	m->muted = on ? 1 : 0;
	apply_audio_params(m);
	m->ev |= MEV_VOLCHANGE;
}

int  media_loop_get(struct media_el *m) { return m ? m->loop : 0; }
void media_set_loop(struct media_el *m, int on)
{ if (m) { m->loop = on ? 1 : 0; apply_audio_params(m); } }

int  media_autoplay_get(struct media_el *m) { return m ? m->autoplay : 0; }
void media_set_autoplay(struct media_el *m, int on)
{ if (m) { m->autoplay = on ? 1 : 0; if (on && m->src[0] && !m->ms) { m->want_play = 1; media_kick_load(m); } } }

int media_ready_state(struct media_el *m)
{
	if (!m || !m->ms) return MRS_NOTHING;
	return media_source_duration_ms(m->ms) ? MRS_ENOUGH : MRS_METADATA;
}

int media_network_state(struct media_el *m) { return m ? m->net : MNS_EMPTY; }

/* ---- worker pump: fire queued + derived events --------------------------- */
int media_pump(void)
{
	int i, fired = 0;
	unsigned now = msec();
	for (i = 0; i < MEDIA_MAX; i++) {
		struct media_el *m = &g_media[i];
		unsigned ev;
		if (!m->in_use || m->owner != B->pg || !m->node) continue;

		/* Derive transitions from the live source before draining the queue. */
		if (m->ms) {
			if (media_source_ended(m->ms) && !m->reported_ended) {
				m->reported_ended = 1; m->playing_state = 0;
				m->ev |= MEV_ENDED | MEV_TIMEUPDATE;
			} else if (media_source_playing(m->ms)) {
				if (now - m->last_tu >= 250) { m->ev |= MEV_TIMEUPDATE; m->last_tu = now; }
			}
		}

		blk_lock();
		ev = m->ev; m->ev = 0;
		if (ev & MEV_LOADEDMETA) m->reported_meta = 1;
		blk_unlock();
		if (!ev) continue;

		if (ev & MEV_LOADEDMETA) {
			js_media_fire(m->node, "loadedmetadata");
			js_media_fire(m->node, "durationchange");
			js_media_fire(m->node, "canplay");
			js_media_fire(m->node, "canplaythrough");
			fired += 4;
		}
		if (ev & MEV_PLAY)       { js_media_fire(m->node, "play"); js_media_fire(m->node, "playing"); fired += 2; }
		if (ev & MEV_PAUSE)      { js_media_fire(m->node, "pause"); fired++; }
		if (ev & MEV_SEEKED)     { js_media_fire(m->node, "seeked"); fired++; }
		if (ev & MEV_TIMEUPDATE) { js_media_fire(m->node, "timeupdate"); fired++; }
		if (ev & MEV_ENDED)      { js_media_fire(m->node, "ended"); fired++; }
		if (ev & MEV_VOLCHANGE)  { js_media_fire(m->node, "volumechange"); fired++; }
		if (ev & MEV_ERROR)      { js_media_fire(m->node, "error"); fired++; }
	}
	return fired;
}

/* ---- layout -------------------------------------------------------------- */
#define MBAR_H 34                          /* control-bar height, px */

static int media_poster_h(int w)
{ int h = w * 9 / 16; if (h < 120) h = 120; if (h > 240) h = 240; return h; }

static int media_total_h(const struct media_el *m, int w)
{ return (m->kind == MK_VIDEO ? media_poster_h(w) : 0) + MBAR_H; }

void media_reset_layout(void)
{ int i; for (i = 0; i < MEDIA_MAX; i++) if (g_media[i].in_use) g_media[i].start_row = -1; }

struct media_el *media_at_text_pos(int text_pos)
{
	int i;
	for (i = 0; i < MEDIA_MAX; i++)
		if (g_media[i].in_use && g_media[i].owner == B->pg &&
		    g_media[i].text_pos == text_pos && g_media[i].text_pos >= 0)
			return &g_media[i];
	return 0;
}

int media_widget_rows(struct media_el *m, int paw, int line_h)
{
	int h = media_total_h(m, paw < 1 ? 1 : paw);
	int r = (h + line_h - 1) / line_h;
	return r < 1 ? 1 : r;
}

void media_set_layout(struct media_el *m, int start_row, int rows)
{ if (m) { m->start_row = start_row; m->rows = rows; } }

/* ---- widget geometry (shared by draw + hit) ------------------------------ */
struct mrects {
	int x0, w, bar_y, poster_h, total_h;
	int play_x, play_y, play_w;
	int seek_x, seek_y, seek_w, seek_h;
	int vol_x, vol_y, vol_w;
};

static void media_rects(struct media_el *m, int x0, int y0, int w, struct mrects *r)
{
	int ph = (m->kind == MK_VIDEO) ? media_poster_h(w) : 0;
	int bar_y = y0 + ph;
	int btn = 22, my = bar_y + (MBAR_H - btn) / 2;
	r->x0 = x0; r->w = w; r->poster_h = ph; r->bar_y = bar_y;
	r->total_h = ph + MBAR_H;
	r->play_x = x0 + 8;        r->play_y = my; r->play_w = btn;
	r->vol_w  = btn;           r->vol_x = x0 + w - 8 - btn; r->vol_y = my;
	/* seek track fills the gap between the play button and the time/volume block */
	r->seek_x = r->play_x + btn + 10;
	r->seek_w = (r->vol_x - 110) - r->seek_x;          /* leave room for "M:SS / M:SS" */
	if (r->seek_w < 24) r->seek_w = 24;
	r->seek_h = 6;
	r->seek_y = bar_y + (MBAR_H - r->seek_h) / 2;
}

/* ---- drawing primitives (clipped to the content pane [ct,cb)) ------------ */
static void mfill(int x, int y, int w, int h, unsigned c, int ct, int cb)
{
	if (y < ct) { h -= ct - y; y = ct; }
	if (y + h > cb) h = cb - y;
	if (h <= 0 || w <= 0) return;
	gfx_fill_rect(x, y, w, h, c);
}
static void mtext(int x, int y, const char *s, unsigned c, int ct, int cb)
{ if (y >= ct && y + UIFONT_H <= cb) gfx_ui_text(x, y, s, c); }

static void draw_play_glyph(int cx, int cy, int s, unsigned col, int ct, int cb)
{
	for (int yy = -s; yy <= s; yy++) {             /* solid right-pointing triangle */
		int width = s - (yy < 0 ? -yy : yy);
		if (width > 0) mfill(cx - s / 2, cy + yy, width, 1, col, ct, cb);
	}
}
static void draw_pause_glyph(int cx, int cy, int s, unsigned col, int ct, int cb)
{
	mfill(cx - s / 2, cy - s, s / 3 + 1, 2 * s, col, ct, cb);
	mfill(cx + s / 6, cy - s, s / 3 + 1, 2 * s, col, ct, cb);
}
static void draw_speaker_glyph(int x, int y, int muted, unsigned col, int ct, int cb)
{
	mfill(x, y + 4, 4, 6, col, ct, cb);            /* speaker body  */
	for (int i = 0; i < 5; i++)                    /* cone          */
		mfill(x + 4, y + 4 - i, 1, 6 + 2 * i, col, ct, cb);
	if (muted) {                                   /* a red slash   */
		for (int i = 0; i < 12; i++) {
			if (y + i >= ct && y + i < cb) gfx_pixel(x + 2 + i, y + i, 0xe05050);
			if (y + i >= ct && y + i < cb) gfx_pixel(x + 3 + i, y + i, 0xe05050);
		}
	} else {                                       /* sound waves   */
		for (int i = 0; i < 6; i++) if (y + 2 + i >= ct && y + 2 + i < cb) gfx_pixel(x + 11, y + 2 + i, col);
	}
}

static void fmt_clock(unsigned ms, char *buf)
{
	unsigned sec = ms / 1000, mn = sec / 60, s = sec % 60;
	int k = 0;
	if (mn > 99) mn = 99;
	if (mn >= 10) buf[k++] = '0' + mn / 10;
	buf[k++] = '0' + mn % 10;
	buf[k++] = ':';
	buf[k++] = '0' + s / 10;
	buf[k++] = '0' + s % 10;
	buf[k] = '\0';
}

static void media_draw(struct media_el *m, int x0, int y0, int w, int ct, int cb)
{
	struct mrects r;
	int playing = m->ms && media_source_playing(m->ms);
	unsigned pos = m->ms ? media_source_position_ms(m->ms) : 0;
	unsigned dur = m->ms ? media_source_duration_ms(m->ms) : 0;
	media_rects(m, x0, y0, w, &r);

	if (m->kind == MK_VIDEO) {                     /* poster area: black frame + glyph */
		mfill(x0, y0, w, r.poster_h, 0x101418, ct, cb);
		if (!playing)
			draw_play_glyph(x0 + w / 2, y0 + r.poster_h / 2, 16, 0xf0f3f7, ct, cb);
	}

	mfill(x0, r.bar_y, w, MBAR_H, 0x1c2530, ct, cb);   /* control bar */

	/* play / pause button */
	if (playing) draw_pause_glyph(r.play_x + r.play_w / 2, r.play_y + r.play_w / 2, 6, 0xe6ebf2, ct, cb);
	else         draw_play_glyph(r.play_x + r.play_w / 2, r.play_y + r.play_w / 2, 7, 0xe6ebf2, ct, cb);

	/* seek track + played portion + knob */
	mfill(r.seek_x, r.seek_y, r.seek_w, r.seek_h, 0x3a4654, ct, cb);
	if (dur) {
		int fillw = (int)((long)r.seek_w * (pos < dur ? pos : dur) / dur);
		mfill(r.seek_x, r.seek_y, fillw, r.seek_h, 0x4a90e2, ct, cb);
		mfill(r.seek_x + fillw - 2, r.seek_y - 3, 5, r.seek_h + 6, 0xe6ebf2, ct, cb);
	}

	/* time "M:SS / M:SS" just left of the volume icon */
	{
		char el[8], tt[8], line[20]; int k = 0;
		fmt_clock(pos, el);
		sapp(line, &k, sizeof line, el);
		sapp(line, &k, sizeof line, " / ");
		if (dur) { fmt_clock(dur, tt); sapp(line, &k, sizeof line, tt); }
		else     sapp(line, &k, sizeof line, "--:--");
		mtext(r.vol_x - 8 - gfx_ui_width(line), r.bar_y + (MBAR_H - UIFONT_H) / 2,
		      line, 0xb6bdc7, ct, cb);
	}

	draw_speaker_glyph(r.vol_x + 3, r.vol_y + 5, m->muted, 0xe6ebf2, ct, cb);
}

/* Enumerate the laid-out media widgets of the current page (those with a widget
 * anchor): media_doc_nth(0,1,2,...) until NULL. main.c maps start_row -> screen y
 * itself (it owns the scroll model), so this layer never touches the line table. */
struct media_el *media_doc_nth(int n)
{
	int i, k = 0;
	for (i = 0; i < MEDIA_MAX; i++) {
		struct media_el *m = &g_media[i];
		if (!m->in_use || m->owner != B->pg || m->text_pos < 0) continue;
		if (k++ == n) return m;
	}
	return 0;
}

int media_el_start_row(struct media_el *m) { return m ? m->start_row : -1; }
int media_el_rows(struct media_el *m)      { return m ? m->rows : 0; }
int media_el_height(struct media_el *m, int paw) { return m ? media_total_h(m, paw) : 0; }

void media_el_draw(struct media_el *m, int x0, int y0, int paw, int ct, int cb)
{ if (m) media_draw(m, x0, y0, paw, ct, cb); }

/* Act on a click at (mx,my) within the widget drawn at (x0,y0). The caller has
 * already confirmed the point is inside the widget. */
void media_el_click(struct media_el *m, int mx, int my, int x0, int y0, int paw)
{
	struct mrects r;
	if (!m) return;
	media_rects(m, x0, y0, paw, &r);
	if (mx >= r.play_x && mx < r.play_x + r.play_w && my >= r.play_y - 4 && my < r.play_y + r.play_w + 4) {
		if (media_paused(m)) media_play(m); else media_pause(m);
	} else if (mx >= r.vol_x && mx < r.vol_x + r.vol_w && my >= r.bar_y && my < r.bar_y + MBAR_H) {
		media_set_muted(m, !m->muted);
	} else if (m->ms && mx >= r.seek_x && mx < r.seek_x + r.seek_w &&
	           my >= r.bar_y && my < r.bar_y + MBAR_H) {
		unsigned dur = media_source_duration_ms(m->ms);
		if (dur) media_set_current_time(m, (double)(mx - r.seek_x) * (dur / 1000.0) / r.seek_w);
	} else if (m->kind == MK_VIDEO && my < r.bar_y) {     /* click the poster */
		if (media_paused(m)) media_play(m); else media_pause(m);
	}
	B->seq++;
}

int media_active(void)
{
	int i;
	for (i = 0; i < MEDIA_MAX; i++)
		if (g_media[i].in_use && g_media[i].owner == B->pg &&
		    g_media[i].ms && media_source_playing(g_media[i].ms))
			return 1;
	return 0;
}

/* ---- teardown ------------------------------------------------------------ */
void media_page_teardown(void)
{
	int i;
	blk_lock();
	for (i = 0; i < MEDIA_MAX; i++) {
		struct media_el *m = &g_media[i];
		if (!m->in_use || m->owner != B->pg) continue;
		if (m->ms) { media_source_free(m->ms); m->ms = 0; }
		if (m->file[0]) unlink(m->file);
		if (m->detached) dom_free(m->detached);
		memset(m, 0, sizeof *m);
	}
	blk_unlock();
}
