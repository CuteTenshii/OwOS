/* media.c — the browser's <audio>/<video> + HTMLMediaElement layer (bmedia.h).
 *
 * A media element is a registry entry tying a DOM node to a user/lib/media
 * mixer source. The flow:
 *   - html.c / the Audio constructor register an element and set its src.
 *   - a load is kicked (autoplay, .load(), or the first play()): the loader
 * thread fetches the file, writes it to a temp owofs path, opens it with
 * media_open, and hands it to the mixer as a source.
 *   - the mixer plays/seeks/loops it; the worker's media_pump turns the
 * source's state changes into DOM media events (the mixer never touches the JS
 * engine).
 *   - main.c lays out and draws the controls widget and routes clicks to
 * play/seek.
 *
 * Audio decodes WAV and MPEG (user/lib/media). <video> decodes H.264 through
 * the media_video path on a background thread and blits frames into the poster;
 * an MP4's AAC audio track is not decoded, so such video plays silently. A
 * frame is shown once its presentation time is due (audio clock if present,
 * else wall clock); before the first frame the poster is black with a play
 * glyph.
 */
#include "media.h" /* user/lib/media: decoder + software mixer */
#include "bmedia.h"
#include "browser.h"

#define MK_AUDIO 0
#define MK_VIDEO 1

#define MEDIA_MAX 24

/* queued events, dispatched by media_pump on the worker thread */
#define MEV_LOADEDMETA 0x01
#define MEV_PLAY 0x02
#define MEV_PAUSE 0x04
#define MEV_ENDED 0x08
#define MEV_TIMEUPDATE 0x10
#define MEV_VOLCHANGE 0x20
#define MEV_SEEKED 0x40
#define MEV_ERROR 0x80

struct media_el {
  int in_use;
  void *owner;               /* the struct page that created it          */
  struct dom_node *node;     /* the <audio>/<video> element              */
  struct dom_node *detached; /* a node WE created (new Audio), to free    */
  int kind;                  /* MK_AUDIO / MK_VIDEO                       */

  char src[URL_MAX]; /* resolved absolute current source ("" = none) */
  char poster[URL_MAX];
  int controls, autoplay, loop, muted, preload_none;
  int volume; /* 0..1000 (milli-units of HTMLMediaElement) */

  int registered;      /* first walk done: keep JS-mutated state     */
  int text_pos;        /* widget anchor offset, or -1 = no widget   */
  int start_row, rows; /* widget placement (main.c rewrap)          */

  struct media_source *ms; /* mixer source once loaded                  */
  char file[64];           /* temp owofs path the media is fetched to   */
  volatile int net;        /* MNS_* network state                       */
  volatile int fetching;   /* loader thread is on this slot             */
  int want_play;           /* play requested before the source existed  */
  unsigned start_ms;       /* pending currentTime applied on load       */
  int load_gen;            /* bumped per src change (stale-load guard)   */

  volatile unsigned ev; /* queued MEV_* bits (worker drains)         */
  int reported_meta;
  int reported_ended;
  int playing_state; /* last play/pause we told script about      */
  unsigned last_tu;  /* msec of the last timeupdate               */

  /* --- <video> picture path (user/lib/media media_video). Decoded on the video
   * thread into a double buffer so the main thread never stalls on a frame. ---
   */
  media_video *vid;       /* H.264 decoder over m->file, or NULL       */
  int vw, vh;             /* native frame size                         */
  unsigned char *vbuf[2]; /* two native RGB (vw*vh*3) frame buffers    */
  volatile int vfront;    /* index of the displayable buffer           */
  volatile int vshown;    /* vfront holds a real (not blank) frame     */
  int vpending;           /* a decoded frame waits in vbuf[1-vfront]   */
  unsigned vpending_pts;  /* its presentation time                     */
  volatile int vplaying;
  volatile int vended;
  volatile int vbusy;      /* video thread is mid-decode on this slot   */
  volatile int vgone;      /* disposal requested; thread must not touch */
  long long vclock_origin; /* msec() - position, when self-clocking     */
  long long vpos_ms;       /* shown video position                      */
  unsigned vdur_ms;
};

static struct media_el g_media[MEDIA_MAX];

/* video thread machinery (defined below loader_run, which starts it) */
static void video_ensure(void);
static void video_dispose(struct media_el *m);

/* ---- helpers ------------------------------------------------------------- */
static void mstrcpy(char *d, const char *s, int cap)
{
  int i = 0;
  for (; s[i] && i < cap - 1; i++)
    d[i] = s[i];
  d[i] = '\0';
}

static int vol256_of(const struct media_el *m) { return m->muted ? 0 : m->volume * 256 / 1000; }

/* Apply the element's volume/mute/loop to its live mixer source. */
static void apply_audio_params(struct media_el *m)
{
  if (!m->ms)
    return;
  media_source_set_volume(m->ms, vol256_of(m));
  media_source_set_loop(m->ms, m->loop);
}

/* ---- the loader thread: fetch + open one source at a time ---------------- */
static char g_loader_stack[131072];
static volatile int g_loader_started;

/* Claim the next slot waiting to load (MNS_LOADING, no source yet). */
static struct media_el *loader_pick(void)
{
  struct media_el *job = 0;
  int i;
  blk_lock();
  for (i = 0; i < MEDIA_MAX; i++) {
    struct media_el *m = &g_media[i];
    if (m->in_use && m->net == MNS_LOADING && !m->fetching && !m->ms && !m->vid && m->src[0]) {
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
  int gen, status = 0, kind;
  long got;
  media_decoder *d = 0;
  struct media_source *s = 0;
  media_video *vv = 0;
  unsigned char *vb0 = 0, *vb1 = 0;
  int vw = 0, vh = 0;
  unsigned vdur = 0;

  blk_lock();
  mstrcpy(src, job->src, sizeof src);
  mstrcpy(file, job->file, sizeof file);
  gen = job->load_gen;
  kind = job->kind;
  blk_unlock();

  /* Fetch the clip with a Range request, streamed straight to `file` (bounded
   * memory, any size). Returns the byte count on a 2xx, having already written
   * and vetted the response — so a positive result means the file is ready. */
  got = browser_fetch_media(src, "audio/*,video/*,*/*", file, &status);
  if (got > 0) {
    /* A <video> opens the H.264 picture path; its audio track (AAC) is not
     * decoded, so audio only plays for a container we can also read (WAV/MP3).
     */
    if (kind == MK_VIDEO) {
      vv = media_video_open(file);
      if (vv) {
        vw = media_video_width(vv);
        vh = media_video_height(vv);
        vdur = media_video_duration_ms(vv);
        vb0 = (unsigned char *)malloc((unsigned)vw * vh * 3);
        vb1 = (unsigned char *)malloc((unsigned)vw * vh * 3);
        if (!vb0 || !vb1) {
          media_video_close(vv);
          vv = 0;
          free(vb0);
          free(vb1);
          vb0 = vb1 = 0;
        }
      }
    }
    d = media_open(file); /* NULL for AAC/anything not WAV/MPEG */
  }
  if (d)
    s = media_source_new(d, 0, 0); /* takes ownership of d */
  if (vv)
    video_ensure();

  blk_lock();
  if (job->load_gen != gen) { /* a newer src superseded this load */
    if (s)
      media_source_free(s);
    if (vv) {
      media_video_close(vv);
      free(vb0);
      free(vb1);
    }
  } else if (s || vv) {
    if (s) {
      job->ms = s;
      media_source_set_volume(s, vol256_of(job));
      media_source_set_loop(s, job->loop);
      if (job->start_ms)
        media_source_seek_ms(s, job->start_ms);
    }
    if (vv) {
      job->vid = vv;
      job->vw = vw;
      job->vh = vh;
      job->vbuf[0] = vb0;
      job->vbuf[1] = vb1;
      job->vdur_ms = vdur;
      job->vfront = 0;
      job->vshown = 0;
      job->vpending = 0;
      job->vended = 0;
    }
    job->net = MNS_IDLE;
    job->ev |= MEV_LOADEDMETA;
    if (job->want_play) {
      if (s)
        media_source_play(s);
      if (vv) {
        job->vplaying = 1;
        job->vclock_origin = (long long)msec() - job->vpos_ms;
      }
      job->want_play = 0;
      job->ev |= MEV_PLAY;
    }
  } else {
    job->net = MNS_NO_SOURCE;
    job->ev |= MEV_ERROR;
  }
  job->fetching = 0;
  blk_unlock();
  B->seq++; /* request a repaint */
}

static void loader_thread(void)
{
  for (;;) {
    struct media_el *job = loader_pick();
    if (!job) {
      msleep(20);
      continue;
    }
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

/* ---- the video thread: decode <video> frames ahead of their play time ------
 * * One background thread services every playing video: it decodes the next
 * frame into the element's back buffer, then — when the frame's presentation
 * time is due against the clock (the audio track if the container has decodable
 * audio, else a wall clock) — promotes it to the front buffer for the main
 * thread to blit. The decode happens off-lock so the browser never stalls; a
 * per-element `vbusy` flag lets disposal wait for an in-flight decode rather
 * than free the decoder under it. */
static char g_vthread_stack[131072];
static volatile int g_vthread_started;
static volatile int g_video_busy; /* the element currently decoding */

static int video_service(struct media_el *m)
{
  long long clk;
  int back, r;
  blk_lock();
  if (m->vgone || !m->vid || !m->vplaying) {
    blk_unlock();
    return 0;
  }
  m->vbusy = 1;
  blk_unlock();

  back = 1 - m->vfront;
  r = 1;
  if (!m->vpending) {
    r = media_video_next(m->vid, m->vbuf[back], &m->vpending_pts);
    if (r == 1)
      m->vpending = 1;
  }
  clk = m->ms ? (long long)media_source_position_ms(m->ms) : ((long long)msec() - m->vclock_origin);

  blk_lock();
  if (r != 1) { /* end of stream / decode error */
    m->vended = 1;
    m->vplaying = 0;
    m->vbusy = 0;
    blk_unlock();
    return 0;
  }
  if ((long long)m->vpending_pts <= clk) { /* frame is due: show it */
    m->vfront = back;
    m->vshown = 1;
    m->vpos_ms = m->vpending_pts;
    m->vpending = 0;
    m->vbusy = 0;
    blk_unlock();
    return 1;
  }
  m->vbusy = 0;
  blk_unlock();
  return 0;
}

static void video_thread(void)
{
  for (;;) {
    int did = 0, i;
    for (i = 0; i < MEDIA_MAX; i++) {
      struct media_el *m = &g_media[i];
      if (m->in_use && m->kind == MK_VIDEO && m->vid && m->vplaying) {
        g_video_busy = i;
        did |= video_service(m);
      }
    }
    if (did)
      B->seq++; /* new frame(s): request a repaint */
    else
      msleep(5);
  }
}

static void video_ensure(void)
{
  blk_lock();
  if (!g_vthread_started) {
    g_vthread_started = 1;
    thread_create(video_thread, &g_vthread_stack[sizeof g_vthread_stack]);
  }
  blk_unlock();
}

/* Tear down an element's video decoder, waiting out any in-flight decode so the
 * video thread never touches a freed decoder. */
static void video_dispose(struct media_el *m)
{
  if (!m->vid && !m->vbuf[0])
    return;
  blk_lock();
  m->vgone = 1;
  m->vplaying = 0;
  blk_unlock();
  while (m->vbusy)
    msleep(1);
  if (m->vid) {
    media_video_close(m->vid);
    m->vid = 0;
  }
  free(m->vbuf[0]);
  free(m->vbuf[1]);
  m->vbuf[0] = m->vbuf[1] = 0;
  m->vshown = m->vpending = m->vended = 0;
  m->vfront = 0;
  blk_lock();
  m->vgone = 0;
  blk_unlock();
}

/* Reopen the video decoder from the (still-resident) temp file — used to replay
 * a finished clip, since the decoder has no random-access seek. Off the big
 * lock during the file read so the browser stays responsive. */
static void video_reopen(struct media_el *m)
{
  char file[64];
  media_video *nv;
  unsigned char *b0, *b1;
  int vw, vh;
  blk_lock();
  mstrcpy(file, m->file, sizeof file);
  blk_unlock();
  video_dispose(m);
  nv = media_video_open(file);
  if (!nv)
    return;
  vw = media_video_width(nv);
  vh = media_video_height(nv);
  b0 = (unsigned char *)malloc((unsigned)vw * vh * 3);
  b1 = (unsigned char *)malloc((unsigned)vw * vh * 3);
  if (!b0 || !b1) {
    media_video_close(nv);
    free(b0);
    free(b1);
    return;
  }
  blk_lock();
  m->vid = nv;
  m->vw = vw;
  m->vh = vh;
  m->vbuf[0] = b0;
  m->vbuf[1] = b1;
  m->vdur_ms = media_video_duration_ms(nv);
  m->vfront = 0;
  m->vshown = 0;
  m->vpending = 0;
  m->vended = 0;
  m->vpos_ms = 0;
  blk_unlock();
}

/* Begin (or restart) loading this element's current source. */
static void media_kick_load(struct media_el *m)
{
  if (!m->src[0] || (m->ms && m->kind == MK_AUDIO) || m->preload_none)
    return;
  if (m->kind == MK_VIDEO && m->vid)
    return;
  loader_ensure();
  m->net = MNS_LOADING;
}

/* ---- registry ------------------------------------------------------------ */
static struct media_el *slot_alloc(struct dom_node *node, int kind)
{
  struct media_el *m = 0;
  int i;
  blk_lock();
  for (i = 0; i < MEDIA_MAX; i++)
    if (!g_media[i].in_use) {
      m = &g_media[i];
      break;
    }
  if (m) {
    memset(m, 0, sizeof *m);
    m->owner = B->pg;
    m->node = node;
    m->kind = kind;
    m->text_pos = -1;
    m->start_row = -1;
    m->volume = 1000;
    m->net = MNS_EMPTY;
    {
      int k = 0;
      sapp(m->file, &k, sizeof m->file, "/home/root/.mediacache");
      uapp(m->file, &k, sizeof m->file, (unsigned)(m - g_media));
    }
    __asm__ volatile("" ::: "memory");
    m->in_use = 1;
  }
  blk_unlock();
  return m;
}

struct media_el *media_for_node(struct dom_node *node)
{
  int i;
  if (!node)
    return 0;
  for (i = 0; i < MEDIA_MAX; i++)
    if (g_media[i].in_use && g_media[i].node == node && g_media[i].owner == B->pg)
      return &g_media[i];
  /* createElement('audio'/'video') reached a media property before registration
   */
  {
    int kind = streq(node->tag, "video") ? MK_VIDEO : MK_AUDIO;
    return slot_alloc(node, kind);
  }
}

struct media_el *media_new_audio(struct dom_node *node)
{
  struct media_el *m = slot_alloc(node, MK_AUDIO);
  if (m)
    m->detached = node; /* WE own this node; free it at page teardown */
  return m;
}

/* ---- source selection (<source> children + src attr) --------------------- */
/* A type string's playability: WAV/MPEG audio is decodable, other audio/video
 * is a maybe (we still try — the container may carry a stream we can read). */
int media_can_play_type(const char *type)
{
  if (!type || !type[0])
    return MEDIA_MAYBE;
  if (starts_ci(type, "audio/wav") || starts_ci(type, "audio/x-wav") || starts_ci(type, "audio/mpeg") ||
      starts_ci(type, "audio/mp3") || starts_ci(type, "audio/mpa"))
    return MEDIA_PROBABLY;
  if (starts_ci(type, "audio/"))
    return MEDIA_MAYBE;
  if (starts_ci(type, "video/"))
    return MEDIA_MAYBE; /* audio track only */
  return MEDIA_CANT;
}

static int url_ext_playable(const char *url)
{
  int n = strlen(url), i;
  char ext[8];
  int e = 0;
  /* scan back for the extension dot, no further than a plausible extension
   * length */
  for (i = n - 1; i >= 0 && url[i] != '/' && url[i] != '?' && n - i < 8; i--)
    if (url[i] == '.')
      break;
  if (i < 0 || url[i] != '.')
    return MEDIA_MAYBE;
  for (int j = i + 1; url[j] && url[j] != '?' && e < (int)sizeof ext - 1; j++)
    ext[e++] = url[j];
  ext[e] = '\0';
  if (streq_ci(ext, "wav") || streq_ci(ext, "mp3") || streq_ci(ext, "mp2") || streq_ci(ext, "mpa") ||
      streq_ci(ext, "mpeg"))
    return MEDIA_PROBABLY;
  return MEDIA_MAYBE;
}

/* Pick the best source for a media node: its own src, else the best <source>
 * child (a recognized type/extension wins). Resolves against page_url. */
static int pick_source(struct dom_node *node, const char *page_url, char *out, int cap)
{
  const char *src = dom_get_attr(node, "src");
  struct dom_node *c;
  char best[URL_MAX];
  int best_rank = -1;
  best[0] = '\0';
  if (src && *src) {
    resolve_url(page_url, src, out, cap);
    return out[0] != '\0';
  }
  for (c = node->first_child; c; c = c->next) {
    if (c->type != DOM_ELEMENT || !streq(c->tag, "source"))
      continue;
    const char *ssrc = dom_get_attr(c, "src");
    const char *type = dom_get_attr(c, "type");
    int rank;
    if (!ssrc || !*ssrc)
      continue;
    rank = type ? media_can_play_type(type) : 0;
    if (rank == MEDIA_CANT)
      continue;
    {
      char abs[URL_MAX];
      resolve_url(page_url, ssrc, abs, sizeof abs);
      if (rank == 0)
        rank = url_ext_playable(abs);
      if (rank > best_rank) {
        best_rank = rank;
        mstrcpy(best, abs, sizeof best);
      }
    }
  }
  if (best[0]) {
    mstrcpy(out, best, cap);
    return 1;
  }
  return 0;
}

/* ---- registration -------------------------------------------------------- */
struct media_el *media_register_element(struct dom_node *node, int text_pos, const char *page_url)
{
  struct media_el *m = media_for_node(node);
  char abs[URL_MAX];
  if (!m)
    return 0;

  /* The render walk re-runs on every relayout. Visual attributes (which widget,
   * where it anchors) refresh each time; playback state
   * (autoplay/loop/muted/src) is read once so a later JS mutation (audio.loop =
   * ...) isn't clobbered. */
  m->kind = streq(node->tag, "video") ? MK_VIDEO : MK_AUDIO;
  m->text_pos = text_pos;
  m->controls = dom_get_attr(node, "controls") != 0;

  if (!m->registered) {
    const char *poster, *pl;
    m->registered = 1;
    m->autoplay = dom_get_attr(node, "autoplay") != 0;
    m->loop = dom_get_attr(node, "loop") != 0;
    m->muted = dom_get_attr(node, "muted") != 0;
    pl = dom_get_attr(node, "preload");
    m->preload_none = pl && streq_ci(pl, "none");
    poster = dom_get_attr(node, "poster");
    if (poster && *poster)
      resolve_url(page_url, poster, m->poster, sizeof m->poster);
    if (pick_source(node, page_url, abs, sizeof abs))
      media_set_src(m, abs);
    apply_audio_params(m);
    if (m->autoplay && m->src[0]) {
      m->want_play = 1;
      media_kick_load(m);
    }
  } else if (pick_source(node, page_url, abs, sizeof abs)) {
    media_set_src(m, abs); /* no-op unless the src attribute changed */
  }
  return m;
}

/* ---- JS-facing operations ------------------------------------------------ */
void media_set_src(struct media_el *m, const char *abs_url)
{
  if (!m || !abs_url)
    return;
  if (streq(m->src, abs_url))
    return; /* unchanged */
  if (m->vid || m->vbuf[0])
    video_dispose(m); /* off-lock: waits vbusy */
  blk_lock();
  mstrcpy(m->src, abs_url, sizeof m->src);
  if (m->ms) {
    media_source_free(m->ms);
    m->ms = 0;
  }
  m->load_gen++;
  m->reported_meta = 0;
  m->reported_ended = 0;
  m->want_play = 0;
  m->start_ms = 0;
  m->net = m->src[0] ? MNS_IDLE : MNS_EMPTY;
  blk_unlock();
}

void media_get_src(struct media_el *m, char *out, int cap) { mstrcpy(out, m ? m->src : "", cap); }

void media_load(struct media_el *m)
{
  if (m)
    media_kick_load(m);
}

void media_play(struct media_el *m)
{
  if (!m)
    return;
  if (m->vid && m->vended)
    video_reopen(m); /* replay a finished clip */
  blk_lock();
  if (m->ms || m->vid) {
    if (m->vid) {
      m->vplaying = 1;
      m->vclock_origin = (long long)msec() - m->vpos_ms;
    }
    if (m->ms) {
      if (m->reported_ended) {
        media_source_seek_ms(m->ms, 0);
        m->reported_ended = 0;
      }
      media_source_play(m->ms);
    }
    if (!m->playing_state) {
      m->ev |= MEV_PLAY;
      m->playing_state = 1;
    }
  } else {
    m->want_play = 1;
  }
  blk_unlock();
  if (!m->ms && !m->vid)
    media_kick_load(m);
}

void media_pause(struct media_el *m)
{
  if (!m)
    return;
  blk_lock();
  m->want_play = 0;
  if (m->ms)
    media_source_pause(m->ms);
  if (m->vid)
    m->vplaying = 0; /* vpos_ms holds the resume point */
  if (m->playing_state) {
    m->ev |= MEV_PAUSE;
    m->playing_state = 0;
  }
  blk_unlock();
}

int media_paused(struct media_el *m)
{
  if (!m)
    return 1;
  return !((m->ms && media_source_playing(m->ms)) || (m->vid && m->vplaying));
}

int media_ended(struct media_el *m)
{
  if (!m)
    return 0;
  return (m->ms && media_source_ended(m->ms)) || (m->vid && m->vended && !m->ms);
}

double media_current_time(struct media_el *m)
{
  if (!m)
    return 0.0;
  if (m->ms)
    return (double)media_source_position_ms(m->ms) / 1000.0;
  if (m->vid)
    return (double)m->vpos_ms / 1000.0;
  return (double)m->start_ms / 1000.0;
}

void media_set_current_time(struct media_el *m, double sec)
{
  unsigned ms;
  if (!m)
    return;
  if (sec < 0)
    sec = 0;
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
  unsigned d = 0;
  if (m && m->ms)
    d = media_source_duration_ms(m->ms);
  else if (m && m->vid)
    d = m->vdur_ms;
  if (!d)
    return __builtin_nan(""); /* unknown duration is NaN */
  return (double)d / 1000.0;
}

double media_volume(struct media_el *m) { return m ? (double)m->volume / 1000.0 : 1.0; }

void media_set_volume(struct media_el *m, double v)
{
  int nv;
  if (!m)
    return;
  if (v < 0)
    v = 0;
  if (v > 1)
    v = 1;
  nv = (int)(v * 1000.0 + 0.5);
  if (nv == m->volume)
    return;
  m->volume = nv;
  apply_audio_params(m);
  m->ev |= MEV_VOLCHANGE;
}

int media_muted(struct media_el *m) { return m ? m->muted : 0; }

void media_set_muted(struct media_el *m, int on)
{
  if (!m || m->muted == (on ? 1 : 0))
    return;
  m->muted = on ? 1 : 0;
  apply_audio_params(m);
  m->ev |= MEV_VOLCHANGE;
}

int media_loop_get(struct media_el *m) { return m ? m->loop : 0; }
void media_set_loop(struct media_el *m, int on)
{
  if (m) {
    m->loop = on ? 1 : 0;
    apply_audio_params(m);
  }
}

int media_autoplay_get(struct media_el *m) { return m ? m->autoplay : 0; }
void media_set_autoplay(struct media_el *m, int on)
{
  if (m) {
    m->autoplay = on ? 1 : 0;
    if (on && m->src[0] && !m->ms) {
      m->want_play = 1;
      media_kick_load(m);
    }
  }
}

int media_ready_state(struct media_el *m)
{
  if (!m)
    return MRS_NOTHING;
  if (m->ms)
    return media_source_duration_ms(m->ms) ? MRS_ENOUGH : MRS_METADATA;
  if (m->vid)
    return m->vdur_ms ? MRS_ENOUGH : MRS_METADATA;
  return MRS_NOTHING;
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
    if (!m->in_use || m->owner != B->pg || !m->node)
      continue;

    /* Loop a finished silent video by reopening from the top (the decoder has
     * no seek). Done off the big lock since video_reopen reads the file. */
    if (m->vid && m->loop && m->vended && !m->ms) {
      video_reopen(m);
      blk_lock();
      m->vplaying = 1;
      m->vended = 0;
      m->vpos_ms = 0;
      m->vclock_origin = (long long)msec();
      blk_unlock();
      continue;
    }

    blk_lock();
    /* Derive transitions from the live source, then drain the queue — all
     * under blk_lock so a non-atomic m->ev |= can't clobber a bit the loader
     * thread just set under the same lock. */
    if (m->ms) {
      if (media_source_ended(m->ms) && !m->reported_ended) {
        m->reported_ended = 1;
        m->playing_state = 0;
        m->ev |= MEV_ENDED | MEV_TIMEUPDATE;
      } else if (media_source_playing(m->ms)) {
        if (now - m->last_tu >= 250) {
          m->ev |= MEV_TIMEUPDATE;
          m->last_tu = now;
        }
      }
    } else if (m->vid) { /* silent video: drive from vid */
      if (m->vended && !m->reported_ended) {
        m->reported_ended = 1;
        m->playing_state = 0;
        m->ev |= MEV_ENDED | MEV_TIMEUPDATE;
      } else if (m->vplaying) {
        if (now - m->last_tu >= 250) {
          m->ev |= MEV_TIMEUPDATE;
          m->last_tu = now;
        }
      }
    }
    ev = m->ev;
    m->ev = 0;
    if (ev & MEV_LOADEDMETA)
      m->reported_meta = 1;
    blk_unlock();
    if (!ev)
      continue;

    if (ev & MEV_LOADEDMETA) {
      js_media_fire(m->node, "loadedmetadata");
      js_media_fire(m->node, "durationchange");
      js_media_fire(m->node, "canplay");
      js_media_fire(m->node, "canplaythrough");
      fired += 4;
    }
    if (ev & MEV_PLAY) {
      js_media_fire(m->node, "play");
      js_media_fire(m->node, "playing");
      fired += 2;
    }
    if (ev & MEV_PAUSE) {
      js_media_fire(m->node, "pause");
      fired++;
    }
    if (ev & MEV_SEEKED) {
      js_media_fire(m->node, "seeked");
      fired++;
    }
    if (ev & MEV_TIMEUPDATE) {
      js_media_fire(m->node, "timeupdate");
      fired++;
    }
    if (ev & MEV_ENDED) {
      js_media_fire(m->node, "ended");
      fired++;
    }
    if (ev & MEV_VOLCHANGE) {
      js_media_fire(m->node, "volumechange");
      fired++;
    }
    if (ev & MEV_ERROR) {
      js_media_fire(m->node, "error");
      fired++;
    }
  }
  return fired;
}

/* ---- layout -------------------------------------------------------------- */
#define MBAR_H 34 /* control-bar height, px */

static int media_poster_h(int w)
{
  int h = w * 9 / 16;
  if (h < 120)
    h = 120;
  if (h > 240)
    h = 240;
  return h;
}

static int media_total_h(const struct media_el *m, int w)
{
  return (m->kind == MK_VIDEO ? media_poster_h(w) : 0) + MBAR_H;
}

void media_reset_layout(void)
{
  int i;
  for (i = 0; i < MEDIA_MAX; i++)
    if (g_media[i].in_use)
      g_media[i].start_row = -1;
}

struct media_el *media_at_text_pos(int text_pos)
{
  int i;
  for (i = 0; i < MEDIA_MAX; i++)
    if (g_media[i].in_use && g_media[i].owner == B->pg && g_media[i].text_pos == text_pos && g_media[i].text_pos >= 0)
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
{
  if (m) {
    m->start_row = start_row;
    m->rows = rows;
  }
}

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
  r->x0 = x0;
  r->w = w;
  r->poster_h = ph;
  r->bar_y = bar_y;
  r->total_h = ph + MBAR_H;
  r->play_x = x0 + 8;
  r->play_y = my;
  r->play_w = btn;
  r->vol_w = btn;
  r->vol_x = x0 + w - 8 - btn;
  r->vol_y = my;
  /* seek track fills the gap between the play button and the time/volume block
   */
  r->seek_x = r->play_x + btn + 10;
  r->seek_w = (r->vol_x - 110) - r->seek_x; /* leave room for "M:SS / M:SS" */
  if (r->seek_w < 24)
    r->seek_w = 24;
  r->seek_h = 6;
  r->seek_y = bar_y + (MBAR_H - r->seek_h) / 2;
}

/* ---- drawing primitives (clipped to the content pane [ct,cb)) ------------ */
static void mfill(int x, int y, int w, int h, unsigned c, int ct, int cb)
{
  if (y < ct) {
    h -= ct - y;
    y = ct;
  }
  if (y + h > cb)
    h = cb - y;
  if (h <= 0 || w <= 0)
    return;
  gfx_fill_rect(x, y, w, h, c);
}
static void mtext(int x, int y, const char *s, unsigned c, int ct, int cb)
{
  if (y >= ct && y + UIFONT_H <= cb)
    gfx_ui_text(x, y, s, c);
}

static void draw_play_glyph(int cx, int cy, int s, unsigned col, int ct, int cb)
{
  for (int yy = -s; yy <= s; yy++) { /* solid right-pointing triangle */
    int width = s - (yy < 0 ? -yy : yy);
    if (width > 0)
      mfill(cx - s / 2, cy + yy, width, 1, col, ct, cb);
  }
}
static void draw_pause_glyph(int cx, int cy, int s, unsigned col, int ct, int cb)
{
  mfill(cx - s / 2, cy - s, s / 3 + 1, 2 * s, col, ct, cb);
  mfill(cx + s / 6, cy - s, s / 3 + 1, 2 * s, col, ct, cb);
}
static void draw_speaker_glyph(int x, int y, int muted, unsigned col, int ct, int cb)
{
  mfill(x, y + 4, 4, 6, col, ct, cb); /* speaker body  */
  for (int i = 0; i < 5; i++)         /* cone          */
    mfill(x + 4, y + 4 - i, 1, 6 + 2 * i, col, ct, cb);
  if (muted) { /* a red slash   */
    for (int i = 0; i < 12; i++) {
      if (y + i >= ct && y + i < cb)
        gfx_pixel(x + 2 + i, y + i, 0xe05050);
      if (y + i >= ct && y + i < cb)
        gfx_pixel(x + 3 + i, y + i, 0xe05050);
    }
  } else { /* sound waves   */
    for (int i = 0; i < 6; i++)
      if (y + 2 + i >= ct && y + 2 + i < cb)
        gfx_pixel(x + 11, y + 2 + i, col);
  }
}

static void fmt_clock(unsigned ms, char *buf)
{
  unsigned sec = ms / 1000, mn = sec / 60, s = sec % 60;
  int k = 0;
  if (mn > 99)
    mn = 99;
  if (mn >= 10)
    buf[k++] = '0' + mn / 10;
  buf[k++] = '0' + mn % 10;
  buf[k++] = ':';
  buf[k++] = '0' + s / 10;
  buf[k++] = '0' + s % 10;
  buf[k] = '\0';
}

/* Blit the current decoded video frame into the poster rect, letterboxed to the
 * native aspect. The caller (paint) already holds blk_lock, which the video
 * thread also takes to swap the front buffer — so vfront is stable here and the
 * front buffer can be read directly (the thread only decodes into the back
 * buffer). */
static void blit_video_frame(struct media_el *m, int x0, int y0, int w, int ph, int ct, int cb)
{
  int vw = m->vw, vh = m->vh;
  int sx = w * 256 / vw, sy = ph * 256 / vh, s = sx < sy ? sx : sy;
  int dw = vw * s / 256, dh = vh * s / 256;
  int dx, dy, py, px;
  const unsigned char *fr = m->vbuf[m->vfront];
  if (dw < 1)
    dw = 1;
  if (dh < 1)
    dh = 1;
  dx = x0 + (w - dw) / 2;
  dy = y0 + (ph - dh) / 2;
  mfill(x0, y0, w, ph, 0x000000, ct, cb); /* letterbox bars */

  for (py = 0; py < dh; py++) {
    int yy = dy + py, syv = py * vh / dh;
    const unsigned char *row;
    if (yy < ct || yy >= cb)
      continue;
    row = fr + (unsigned)syv * vw * 3;
    for (px = 0; px < dw; px++) {
      const unsigned char *p = row + (px * vw / dw) * 3;
      gfx_pixel(dx + px, yy, ((unsigned)p[0] << 16) | ((unsigned)p[1] << 8) | p[2]);
    }
  }
}

static void media_draw(struct media_el *m, int x0, int y0, int w, int ct, int cb)
{
  struct mrects r;
  int playing = (m->ms && media_source_playing(m->ms)) || (m->vid && m->vplaying);
  unsigned pos = m->ms ? media_source_position_ms(m->ms) : (m->vid ? (unsigned)m->vpos_ms : 0);
  unsigned dur = m->ms ? media_source_duration_ms(m->ms) : (m->vid ? m->vdur_ms : 0);
  media_rects(m, x0, y0, w, &r);

  if (m->kind == MK_VIDEO) { /* decoded frame, or a black poster */
    if (m->vid && m->vshown) {
      blit_video_frame(m, x0, y0, w, r.poster_h, ct, cb);
      if (!playing)
        draw_play_glyph(x0 + w / 2, y0 + r.poster_h / 2, 16, 0xf0f3f7, ct, cb);
    } else {
      mfill(x0, y0, w, r.poster_h, 0x101418, ct, cb);
      if (!playing)
        draw_play_glyph(x0 + w / 2, y0 + r.poster_h / 2, 16, 0xf0f3f7, ct, cb);
    }
  }

  mfill(x0, r.bar_y, w, MBAR_H, 0x1c2530, ct, cb); /* control bar */

  /* play / pause button */
  if (playing)
    draw_pause_glyph(r.play_x + r.play_w / 2, r.play_y + r.play_w / 2, 6, 0xe6ebf2, ct, cb);
  else
    draw_play_glyph(r.play_x + r.play_w / 2, r.play_y + r.play_w / 2, 7, 0xe6ebf2, ct, cb);

  /* seek track + played portion + knob */
  mfill(r.seek_x, r.seek_y, r.seek_w, r.seek_h, 0x3a4654, ct, cb);
  if (dur) {
    int fillw = (int)((long)r.seek_w * (pos < dur ? pos : dur) / dur);
    mfill(r.seek_x, r.seek_y, fillw, r.seek_h, 0x4a90e2, ct, cb);
    mfill(r.seek_x + fillw - 2, r.seek_y - 3, 5, r.seek_h + 6, 0xe6ebf2, ct, cb);
  }

  /* time "M:SS / M:SS" just left of the volume icon */
  {
    char el[8], tt[8], line[20];
    int k = 0;
    fmt_clock(pos, el);
    sapp(line, &k, sizeof line, el);
    sapp(line, &k, sizeof line, " / ");
    if (dur) {
      fmt_clock(dur, tt);
      sapp(line, &k, sizeof line, tt);
    } else
      sapp(line, &k, sizeof line, "--:--");
    mtext(r.vol_x - 8 - gfx_ui_width(line), r.bar_y + (MBAR_H - UIFONT_H) / 2, line, 0xb6bdc7, ct, cb);
  }

  draw_speaker_glyph(r.vol_x + 3, r.vol_y + 5, m->muted, 0xe6ebf2, ct, cb);
}

/* Enumerate the laid-out media widgets of the current page (those with a widget
 * anchor): media_doc_nth(0,1,2,...) until NULL. main.c maps start_row -> screen
 * y itself (it owns the scroll model), so this layer never touches the line
 * table. */
struct media_el *media_doc_nth(int n)
{
  int i, k = 0;
  for (i = 0; i < MEDIA_MAX; i++) {
    struct media_el *m = &g_media[i];
    if (!m->in_use || m->owner != B->pg || m->text_pos < 0)
      continue;
    if (k++ == n)
      return m;
  }
  return 0;
}

int media_el_start_row(struct media_el *m) { return m ? m->start_row : -1; }
int media_el_rows(struct media_el *m) { return m ? m->rows : 0; }
int media_el_height(struct media_el *m, int paw) { return m ? media_total_h(m, paw) : 0; }

void media_el_draw(struct media_el *m, int x0, int y0, int paw, int ct, int cb)
{
  if (m)
    media_draw(m, x0, y0, paw, ct, cb);
}

/* Act on a click at (mx,my) within the widget drawn at (x0,y0). The caller has
 * already confirmed the point is inside the widget. */
void media_el_click(struct media_el *m, int mx, int my, int x0, int y0, int paw)
{
  struct mrects r;
  if (!m)
    return;
  media_rects(m, x0, y0, paw, &r);
  if (mx >= r.play_x && mx < r.play_x + r.play_w && my >= r.play_y - 4 && my < r.play_y + r.play_w + 4) {
    if (media_paused(m))
      media_play(m);
    else
      media_pause(m);
  } else if (mx >= r.vol_x && mx < r.vol_x + r.vol_w && my >= r.bar_y && my < r.bar_y + MBAR_H) {
    media_set_muted(m, !m->muted);
  } else if (m->ms && mx >= r.seek_x && mx < r.seek_x + r.seek_w && my >= r.bar_y && my < r.bar_y + MBAR_H) {
    unsigned dur = media_source_duration_ms(m->ms);
    if (dur)
      media_set_current_time(m, (double)(mx - r.seek_x) * (dur / 1000.0) / r.seek_w);
  } else if (m->kind == MK_VIDEO && my < r.bar_y) { /* click the poster */
    if (media_paused(m))
      media_play(m);
    else
      media_pause(m);
  }
  B->seq++;
}

int media_active(void)
{
  int i;
  for (i = 0; i < MEDIA_MAX; i++) {
    struct media_el *m = &g_media[i];
    if (!m->in_use || m->owner != B->pg)
      continue;
    if ((m->ms && media_source_playing(m->ms)) || (m->vid && m->vplaying))
      return 1;
  }
  return 0;
}

/* ---- teardown ------------------------------------------------------------ */
void media_page_teardown(void)
{
  int i;
  for (i = 0; i < MEDIA_MAX; i++) {
    struct media_el *m = &g_media[i];
    if (!m->in_use || m->owner != B->pg)
      continue;
    if (m->vid || m->vbuf[0])
      video_dispose(m); /* off-lock: waits out a decode */
    blk_lock();
    if (m->ms) {
      media_source_free(m->ms);
      m->ms = 0;
    }
    if (m->file[0])
      unlink(m->file);
    if (m->detached)
      dom_free(m->detached);
    memset(m, 0, sizeof *m);
    blk_unlock();
  }
}
