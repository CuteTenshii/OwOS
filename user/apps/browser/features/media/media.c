/* media.c: <audio>/<video> HTMLMediaElement layer (bmedia.h) over the user/lib/media mixer.
 * Audio decodes WAV/MPEG; <video> decodes H.264 only (an MP4's AAC track is silent). */
#include "media.h" /* user/lib/media: decoder + software mixer */
#include "browser.h"
#include "airplay.h"
#include "cast.h"
#include "features/media/bmedia.h"

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
#define MEV_LOADSTART 0x100
#define MEV_SUSPEND 0x200
#define MEV_EMPTIED 0x400

struct media_el {
  bool in_use;
  void *owner;               /* the struct page that created it          */
  struct dom_node *node;     /* the <audio>/<video> element              */
  struct dom_node *detached; /* a node WE created (new Audio), to free    */
  int kind;                  /* MK_AUDIO / MK_VIDEO                       */

  char src[URL_MAX]; /* resolved absolute current source ("" = none) */
  char poster[URL_MAX];
  bool controls, autoplay, loop, muted;
  int volume; /* 0..1000 (milli-units of HTMLMediaElement) */

  bool registered;     /* first walk done: keep JS-mutated state     */
  int text_pos;        /* widget anchor offset, or -1 = no widget   */
  int start_row, rows; /* widget placement (main.c rewrap)          */

  struct media_source *ms; /* mixer source once loaded                  */
  char file[64];           /* temp owofs path the media is fetched to   */
  volatile int net;        /* MNS_* network state                       */
  volatile bool fetching;  /* loader thread is on this slot             */
  bool want_play;          /* play requested before the source existed  */
  unsigned start_ms;       /* pending currentTime applied on load       */
  int load_gen;            /* bumped per src change (stale-load guard)   */

  volatile unsigned ev; /* queued MEV_* bits (worker drains)         */
  bool reported_meta;
  bool reported_ended;
  bool playing_state; /* last play/pause we told script about      */
  unsigned last_tu;   /* msec of the last timeupdate               */

  /* <video> picture path: decoded on the video thread into a double buffer */
  media_video *vid;       /* H.264 decoder over m->file, or NULL       */
  int vw, vh;             /* native frame size                         */
  unsigned char *vbuf[2]; /* two native RGB (vw*vh*3) frame buffers    */
  volatile int vfront;    /* index of the displayable buffer           */
  volatile bool vshown;   /* vfront holds a real (not blank) frame     */
  bool vpending;          /* a decoded frame waits in vbuf[1-vfront]   */
  unsigned vpending_pts;  /* its presentation time                     */
  volatile bool vplaying;
  volatile bool vended;
  volatile bool vbusy;     /* video thread is mid-decode on this slot   */
  volatile bool vgone;     /* disposal requested; thread must not touch */
  long long vclock_origin; /* msec() - position, when self-clocking     */
  long long vpos_ms;       /* shown video position                      */
  unsigned vdur_ms;

  bool rplay_seen; /* remote playback: the receiver has reported PLAYING */
};

static struct media_el g_media[MEDIA_MAX];
static struct media_el *g_last_played;

/* video thread machinery (defined below loader_run, which starts it) */
static void video_ensure(void);
static void video_dispose(struct media_el *m);

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

static char g_loader_stack[131072];
static volatile bool g_loader_started;

/* Claim the next slot waiting to load (MNS_LOADING, no source yet). */
static struct media_el *loader_pick(void)
{
  struct media_el *job = 0;
  int i;
  browser_buf_lock();
  for (i = 0; i < MEDIA_MAX; i++) {
    struct media_el *m = &g_media[i];
    if (m->in_use && m->net == MNS_LOADING && !m->fetching && !m->ms && !m->vid && m->src[0]) {
      m->fetching = true;
      job = m;
      break;
    }
  }
  browser_buf_unlock();
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

  browser_buf_lock();
  mstrcpy(src, job->src, sizeof src);
  mstrcpy(file, job->file, sizeof file);
  gen = job->load_gen;
  kind = job->kind;
  browser_buf_unlock();

  /* Range-fetch straight to `file`; a positive count means the file is written and vetted. */
  got = browser_fetch_media(src, "audio/*,video/*,*/*", file, &status);
  if (got > 0) {
    /* video opens the H.264 path; AAC audio is not decoded, so only WAV/MP3 containers play sound */
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

  browser_buf_lock();
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
      job->vshown = false;
      job->vpending = false;
      job->vended = false;
    }
    job->net = MNS_IDLE;
    job->ev |= MEV_LOADEDMETA;
    if (job->want_play) {
      if (s)
        media_source_play(s);
      if (vv) {
        job->vplaying = true;
        job->vclock_origin = (long long)msec() - job->vpos_ms;
      }
      job->want_play = false;
      job->ev |= MEV_PLAY;
    }
  } else {
    job->net = MNS_NO_SOURCE;
    job->ev |= MEV_ERROR;
  }
  job->fetching = false;
  browser_buf_unlock();
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
  browser_buf_lock();
  if (!g_loader_started) {
    g_loader_started = true;
    thread_create(loader_thread, &g_loader_stack[sizeof g_loader_stack]);
  }
  browser_buf_unlock();
}

/* Video thread: decodes the next frame into the back buffer and promotes it when due (off-lock). */
static char g_vthread_stack[131072];
static volatile bool g_vthread_started;
static volatile int g_video_busy; /* the element currently decoding */

static bool video_service(struct media_el *m)
{
  long long clk;
  int back, r;
  browser_buf_lock();
  if (m->vgone || !m->vid || !m->vplaying) {
    browser_buf_unlock();
    return false;
  }
  m->vbusy = true;
  browser_buf_unlock();

  back = 1 - m->vfront;
  r = 1;
  if (!m->vpending) {
    r = media_video_next(m->vid, m->vbuf[back], &m->vpending_pts);
    if (r == 1)
      m->vpending = true;
  }
  clk = m->ms ? (long long)media_source_position_ms(m->ms) : ((long long)msec() - m->vclock_origin);

  browser_buf_lock();
  if (r != 1) { /* end of stream / decode error */
    m->vended = true;
    m->vplaying = false;
    m->vbusy = false;
    browser_buf_unlock();
    return false;
  }
  if ((long long)m->vpending_pts <= clk) { /* frame is due: show it */
    m->vfront = back;
    m->vshown = true;
    m->vpos_ms = m->vpending_pts;
    m->vpending = false;
    m->vbusy = false;
    browser_buf_unlock();
    return true;
  }
  m->vbusy = false;
  browser_buf_unlock();
  return false;
}

static void video_thread(void)
{
  for (;;) {
    bool did = false;
    int i;
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
  browser_buf_lock();
  if (!g_vthread_started) {
    g_vthread_started = true;
    thread_create(video_thread, &g_vthread_stack[sizeof g_vthread_stack]);
  }
  browser_buf_unlock();
}

/* Tear down a video decoder, waiting out any in-flight decode. */
static void video_dispose(struct media_el *m)
{
  if (!m->vid && !m->vbuf[0])
    return;
  browser_buf_lock();
  m->vgone = true;
  m->vplaying = false;
  browser_buf_unlock();
  while (m->vbusy)
    msleep(1);
  if (m->vid) {
    media_video_close(m->vid);
    m->vid = 0;
  }
  free(m->vbuf[0]);
  free(m->vbuf[1]);
  m->vbuf[0] = m->vbuf[1] = 0;
  m->vshown = m->vpending = m->vended = false;
  m->vfront = 0;
  browser_buf_lock();
  m->vgone = false;
  browser_buf_unlock();
}

/* Reopen the decoder from the temp file to replay a clip (no random-access seek). */
static void video_reopen(struct media_el *m)
{
  char file[64];
  media_video *nv;
  unsigned char *b0, *b1;
  int vw, vh;
  browser_buf_lock();
  mstrcpy(file, m->file, sizeof file);
  browser_buf_unlock();
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
  browser_buf_lock();
  m->vid = nv;
  m->vw = vw;
  m->vh = vh;
  m->vbuf[0] = b0;
  m->vbuf[1] = b1;
  m->vdur_ms = media_video_duration_ms(nv);
  m->vfront = 0;
  m->vshown = false;
  m->vpending = false;
  m->vended = false;
  m->vpos_ms = 0;
  browser_buf_unlock();
}

/* Begin (or restart) loading this element's current source. */
enum { PRELOAD_NONE, PRELOAD_METADATA, PRELOAD_AUTO };

/* The preload attribute: none / metadata / auto (empty = auto); anything else, or absent, is metadata */
static int media_preload(const struct media_el *m)
{
  const char *pl = m->node ? dom_get_attr(m->node, "preload") : 0;
  if (pl && streq_ci(pl, "none"))
    return PRELOAD_NONE;
  if (pl && (!*pl || streq_ci(pl, "auto")))
    return PRELOAD_AUTO;
  return PRELOAD_METADATA;
}

/* `force`: play(), load() and autoplay fetch even under preload=none */
static void media_kick_load(struct media_el *m, bool force)
{
  if (!m->src[0] || (m->ms && m->kind == MK_AUDIO))
    return;
  if (m->kind == MK_VIDEO && m->vid)
    return;
  if (!force && media_preload(m) == PRELOAD_NONE) {
    browser_buf_lock();
    m->ev |= MEV_SUSPEND;
    browser_buf_unlock();
    return;
  }
  loader_ensure();
  browser_buf_lock();
  if (m->net != MNS_LOADING)
    m->ev |= MEV_LOADSTART;
  m->net = MNS_LOADING;
  browser_buf_unlock();
}

static struct media_el *slot_alloc(struct dom_node *node, int kind)
{
  struct media_el *m = 0;
  int i;
  browser_buf_lock();
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
    m->in_use = true;
  }
  browser_buf_unlock();
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
  /* createElement('audio'/'video') touched a media property before registration */
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

/* WAV/MPEG audio is decodable; other audio/video is a maybe (the container may carry a readable stream). */
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
  /* scan back for the extension dot, within a plausible length */
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

/* Best source for a media node: its src, else the best <source> child. */
static bool pick_source(struct dom_node *node, const char *page_url, char *out, int cap)
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
    return true;
  }
  return false;
}

struct media_el *media_register_element(struct dom_node *node, int text_pos, const char *page_url)
{
  struct media_el *m = media_for_node(node);
  char abs[URL_MAX];
  if (!m)
    return 0;

  /* the walk re-runs per relayout: refresh visuals, read playback state once so JS mutations survive */
  m->kind = streq(node->tag, "video") ? MK_VIDEO : MK_AUDIO;
  m->text_pos = text_pos;
  m->controls = dom_get_attr(node, "controls") != 0;

  if (!m->registered) {
    const char *poster;
    m->registered = true;
    m->autoplay = dom_get_attr(node, "autoplay") != 0;
    m->loop = dom_get_attr(node, "loop") != 0;
    m->muted = dom_get_attr(node, "muted") != 0;
    poster = dom_get_attr(node, "poster");
    if (poster && *poster)
      resolve_url(page_url, poster, m->poster, sizeof m->poster);
    if (pick_source(node, page_url, abs, sizeof abs)) {
      media_set_src(m, abs);
      if ((starts_ci(abs, "http://") || starts_ci(abs, "https://")) && !dom_get_attr(node, "disableremoteplayback")) {
        cast_scan_start(); /* so the controls can offer a cast button */
        airplay_scan_start();
      }
    }
    apply_audio_params(m);
    if (m->autoplay && m->src[0]) {
      m->want_play = true;
      media_kick_load(m, true);
    }
  } else if (pick_source(node, page_url, abs, sizeof abs)) {
    media_set_src(m, abs); /* no-op unless the src attribute changed */
  }
  return m;
}

void media_set_src(struct media_el *m, const char *abs_url)
{
  if (!m || !abs_url)
    return;
  if (streq(m->src, abs_url))
    return; /* unchanged */
  if (m->vid || m->vbuf[0])
    video_dispose(m); /* off-lock: waits vbusy */
  browser_buf_lock();
  mstrcpy(m->src, abs_url, sizeof m->src);
  if (m->ms) {
    media_source_free(m->ms);
    m->ms = 0;
  }
  m->load_gen++;
  m->reported_meta = false;
  m->reported_ended = false;
  m->want_play = false;
  m->start_ms = 0;
  if (m->reported_meta || m->vid)
    m->ev |= MEV_EMPTIED;
  m->net = m->src[0] ? MNS_IDLE : MNS_EMPTY;
  browser_buf_unlock();
  if (m->src[0])
    media_kick_load(m, false);
}

void media_get_src(struct media_el *m, char *out, int cap) { mstrcpy(out, m ? m->src : "", cap); }

void media_load(struct media_el *m)
{
  if (m)
    media_kick_load(m, true);
}

/* Remote playback: one Cast session at a time, driving one element while connected */
static cast_session *g_rsess;    /* a Chromecast session, or */
static airplay_session *g_asess; /* an AirPlay one; never both */
static struct media_el *g_rmedia;

static bool remote_on(const struct media_el *m) { return (g_rsess || g_asess) && g_rmedia == m; }

/* The active receiver's state in Cast terms, whichever protocol it speaks */
static void r_status(struct cast_status *st)
{
  if (g_asess) {
    struct airplay_status a;
    airplay_get_status(g_asess, &a);
    memset(st, 0, sizeof *st);
    st->conn = a.conn == AIRPLAY_CONNECTED ? CAST_CONNECTED : CAST_DISCONNECTED;
    st->state = a.state == AIRPLAY_PLAYING     ? CAST_PLAYING
                : a.state == AIRPLAY_PAUSED    ? CAST_PAUSED
                : a.state == AIRPLAY_BUFFERING ? CAST_BUFFERING
                                               : CAST_IDLE;
    st->current_time = a.current_time;
    st->duration = a.duration;
    st->volume = 1.0;
    return;
  }
  cast_get_status(g_rsess, st);
}

static void r_play(void)
{
  if (g_asess)
    airplay_play(g_asess);
  else
    cast_play(g_rsess);
}

static void r_pause(void)
{
  if (g_asess)
    airplay_pause(g_asess);
  else
    cast_pause(g_rsess);
}

static void r_seek(double sec)
{
  if (g_asess)
    airplay_seek(g_asess, sec);
  else
    cast_seek(g_rsess, sec);
}

/* AirPlay video has no receiver volume; the TV's own remote owns it */
static void r_set_volume(double v)
{
  if (g_rsess)
    cast_set_volume(g_rsess, v);
}

static void r_set_muted(int on)
{
  if (g_rsess)
    cast_set_muted(g_rsess, on);
}

static void r_close(void)
{
  cast_session *c = g_rsess;
  airplay_session *a = g_asess;
  g_rsess = 0;
  g_asess = 0;
  g_rmedia = 0;
  cast_close(c);
  airplay_close(a);
}

int media_remote_conn(struct media_el *m)
{
  struct cast_status st;
  if (!remote_on(m))
    return CAST_DISCONNECTED;
  r_status(&st);
  return st.conn;
}

void media_play(struct media_el *m)
{
  if (!m)
    return;
  g_last_played = m;
  if (remote_on(m)) {
    r_play();
    browser_buf_lock();
    if (!m->playing_state) {
      m->ev |= MEV_PLAY;
      m->playing_state = true;
    }
    m->reported_ended = false;
    browser_buf_unlock();
    return;
  }
  if (m->vid && m->vended)
    video_reopen(m); /* replay a finished clip */
  browser_buf_lock();
  if (m->ms || m->vid) {
    if (m->vid) {
      m->vplaying = true;
      m->vclock_origin = (long long)msec() - m->vpos_ms;
    }
    if (m->ms) {
      if (m->reported_ended) {
        media_source_seek_ms(m->ms, 0);
        m->reported_ended = false;
      }
      media_source_play(m->ms);
    }
    if (!m->playing_state) {
      m->ev |= MEV_PLAY;
      m->playing_state = true;
    }
  } else {
    m->want_play = true;
  }
  browser_buf_unlock();
  if (!m->ms && !m->vid)
    media_kick_load(m, true);
}

void media_pause(struct media_el *m)
{
  if (!m)
    return;
  if (remote_on(m))
    r_pause();
  browser_buf_lock();
  m->want_play = false;
  if (m->ms)
    media_source_pause(m->ms);
  if (m->vid)
    m->vplaying = false; /* vpos_ms holds the resume point */
  if (m->playing_state) {
    m->ev |= MEV_PAUSE;
    m->playing_state = false;
  }
  browser_buf_unlock();
}

int media_paused(struct media_el *m)
{
  if (!m)
    return 1;
  if (remote_on(m)) {
    struct cast_status st;
    r_status(&st);
    return st.state == CAST_PAUSED || st.state == CAST_IDLE;
  }
  return !((m->ms && media_source_playing(m->ms)) || (m->vid && m->vplaying));
}

int media_ended(struct media_el *m)
{
  if (!m)
    return 0;
  if (remote_on(m))
    return m->reported_ended;
  return (m->ms && media_source_ended(m->ms)) || (m->vid && m->vended && !m->ms);
}

double media_current_time(struct media_el *m)
{
  if (!m)
    return 0.0;
  if (remote_on(m)) {
    struct cast_status st;
    r_status(&st);
    return st.current_time;
  }
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
  if (remote_on(m)) {
    r_seek(sec);
    m->reported_ended = false;
    m->ev |= MEV_SEEKED | MEV_TIMEUPDATE;
  } else if (m->ms) {
    media_source_seek_ms(m->ms, ms);
    m->reported_ended = false;
    m->ev |= MEV_SEEKED | MEV_TIMEUPDATE;
  } else {
    m->start_ms = ms;
  }
}

double media_duration(struct media_el *m)
{
  unsigned d = 0;
  if (m && remote_on(m)) {
    struct cast_status st;
    r_status(&st);
    d = (unsigned)(st.duration * 1000.0);
  } else if (m && m->ms)
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
  if (remote_on(m))
    r_set_volume((double)nv / 1000.0);
  m->ev |= MEV_VOLCHANGE;
}

int media_muted(struct media_el *m) { return m ? m->muted : 0; }

void media_set_muted(struct media_el *m, int on)
{
  if (!m || m->muted == (on != 0))
    return;
  m->muted = on != 0;
  apply_audio_params(m);
  if (remote_on(m))
    r_set_muted(on != 0);
  m->ev |= MEV_VOLCHANGE;
}

int media_loop_get(struct media_el *m) { return m ? m->loop : 0; }
void media_set_loop(struct media_el *m, int on)
{
  if (m) {
    m->loop = on != 0;
    apply_audio_params(m);
  }
}

int media_autoplay_get(struct media_el *m) { return m ? m->autoplay : 0; }
void media_set_autoplay(struct media_el *m, int on)
{
  if (m) {
    m->autoplay = on != 0;
    if (on && m->src[0] && !m->ms) {
      m->want_play = true;
      media_kick_load(m, true);
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

int media_remote_start(struct media_el *m, const struct remote_dev *dev, const char *ctype)
{
  cast_session *cs = 0;
  airplay_session *as = 0;
  double pos;
  bool was_playing;
  if (!m || !dev || !m->src[0])
    return -1;
  media_remote_stop(g_rmedia);
  if (dev->kind == REMOTE_AIRPLAY) {
    struct airplay_device d;
    memset(&d, 0, sizeof d);
    memcpy(d.ip, dev->ip, 4);
    d.port = dev->port;
    as = airplay_connect(&d, 10000);
  } else {
    struct cast_device d;
    memset(&d, 0, sizeof d);
    memcpy(d.ip, dev->ip, 4);
    d.port = dev->port;
    cs = cast_connect(&d, 10000);
  }
  if (!cs && !as)
    return -1;
  pos = media_current_time(m);
  was_playing = !media_paused(m);
  media_pause(m);
  if (as ? airplay_load(as, m->src, pos) : cast_load(cs, m->src, ctype, m->src, 8000)) {
    cast_close(cs);
    airplay_close(as);
    if (was_playing)
      media_play(m);
    return -1;
  }
  if (cs && pos > 0.5)
    cast_seek(cs, pos);
  browser_buf_lock();
  g_rsess = cs;
  g_asess = as;
  g_rmedia = m;
  m->rplay_seen = false;
  m->reported_ended = false;
  m->playing_state = true;
  m->ev |= MEV_PLAY;
  browser_buf_unlock();
  return 0;
}

/* Leave the receiver and carry its position and play state back to the local element */
void media_remote_stop(struct media_el *m)
{
  struct cast_status st;
  if (!remote_on(m))
    return;
  r_status(&st);
  browser_buf_lock();
  m->playing_state = false;
  m->ev |= MEV_PAUSE;
  browser_buf_unlock();
  r_close();
  media_set_current_time(m, st.current_time);
  if (st.state == CAST_PLAYING || st.state == CAST_BUFFERING)
    media_play(m);
}

/* Receiver-driven transitions: timeupdate, ended, and a receiver that went away */
static void remote_poll(unsigned now)
{
  struct cast_status st;
  struct media_el *m = g_rmedia;
  if (!(g_rsess || g_asess) || !m)
    return;
  r_status(&st);
  if (st.conn == CAST_DISCONNECTED) {
    media_remote_stop(m);
    return;
  }
  browser_buf_lock();
  if (st.state == CAST_PLAYING) {
    m->rplay_seen = true;
    if (now - m->last_tu >= 250) {
      m->ev |= MEV_TIMEUPDATE;
      m->last_tu = now;
    }
  } else if (st.state == CAST_IDLE && m->rplay_seen && !m->reported_ended) {
    m->reported_ended = true;
    m->playing_state = false;
    m->ev |= MEV_ENDED | MEV_TIMEUPDATE;
  }
  browser_buf_unlock();
}

int media_pump(void)
{
  int i, fired = 0;
  unsigned now = msec();
  remote_poll(now);
  for (i = 0; i < MEDIA_MAX; i++) {
    struct media_el *m = &g_media[i];
    unsigned ev;
    if (!m->in_use || m->owner != B->pg || !m->node)
      continue;

    /* loop a finished silent video by reopening (off the big lock) */
    if (m->vid && m->loop && m->vended && !m->ms) {
      video_reopen(m);
      browser_buf_lock();
      m->vplaying = true;
      m->vended = false;
      m->vpos_ms = 0;
      m->vclock_origin = (long long)msec();
      browser_buf_unlock();
      continue;
    }

    browser_buf_lock();
    /* derive transitions then drain, under browser_buf_lock so m->ev |= can't clobber loader bits */
    if (m->ms) {
      if (media_source_ended(m->ms) && !m->reported_ended) {
        m->reported_ended = true;
        m->playing_state = false;
        m->ev |= MEV_ENDED | MEV_TIMEUPDATE;
      } else if (media_source_playing(m->ms)) {
        if (now - m->last_tu >= 250) {
          m->ev |= MEV_TIMEUPDATE;
          m->last_tu = now;
        }
      }
    } else if (remote_on(m)) {
      /* the receiver drives the clock; remote_poll queued its events */
    } else if (m->vid) { /* silent video: drive from vid */
      if (m->vended && !m->reported_ended) {
        m->reported_ended = true;
        m->playing_state = false;
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
      m->reported_meta = true;
    browser_buf_unlock();
    if (!ev)
      continue;

    if (ev & MEV_EMPTIED) {
      js_media_fire(m->node, "emptied");
      fired++;
    }
    if (ev & MEV_LOADSTART) {
      js_media_fire(m->node, "loadstart");
      fired++;
    }
    if (ev & MEV_SUSPEND) {
      js_media_fire(m->node, "suspend");
      fired++;
    }
    if (ev & MEV_LOADEDMETA) {
      js_media_fire(m->node, "durationchange");
      js_media_fire(m->node, "loadedmetadata");
      js_media_fire(m->node, "loadeddata");
      js_media_fire(m->node, "progress");
      js_media_fire(m->node, "canplay");
      js_media_fire(m->node, "canplaythrough");
      fired += 6;
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

struct mrects {
  int x0, w, bar_y, poster_h, total_h;
  int play_x, play_y, play_w;
  int seek_x, seek_y, seek_w, seek_h;
  int vol_x, vol_y, vol_w;
  int fs_x, fs_w; /* fullscreen button (video only; fs_w 0 = none) */
  int cast_x, cast_w; /* cast button (shown when a receiver is around; cast_w 0 = none) */
  int time_right; /* right edge of the time text */
};

/* The cast button shows while casting, or when the source is http(s) and a receiver was seen */
static bool cast_offered(const struct media_el *m)
{
  if (remote_on(m))
    return true;
  if (!m->node || dom_get_attr(m->node, "disableremoteplayback"))
    return false;
  return (starts_ci(m->src, "http://") || starts_ci(m->src, "https://")) && cast_scan_count() + airplay_scan_count() > 0;
}

/* `fsh` > 0: the widget fills a fullscreen pane that tall, instead of the inline size */
static void media_rects(struct media_el *m, int x0, int y0, int w, int fsh, struct mrects *r)
{
  int ph = (m->kind == MK_VIDEO) ? (fsh > MBAR_H ? fsh - MBAR_H : media_poster_h(w)) : 0;
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
  /* seek track fills the gap between the play button and the time/volume block */
  int left = r->vol_x - 8;
  r->fs_w = m->kind == MK_VIDEO ? btn : 0;
  if (r->fs_w) {
    r->fs_x = left - btn;
    left = r->fs_x - 8;
  }
  r->cast_w = cast_offered(m) ? btn : 0;
  if (r->cast_w) {
    r->cast_x = left - btn;
    left = r->cast_x - 8;
  }
  r->time_right = left;
  r->seek_x = r->play_x + btn + 10;
  r->seek_w = (r->time_right - 102) - r->seek_x; /* leave room for "M:SS / M:SS" */
  if (r->seek_w < 24)
    r->seek_w = 24;
  r->seek_h = 6;
  r->seek_y = bar_y + (MBAR_H - r->seek_h) / 2;
}

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
static void draw_speaker_glyph(int x, int y, bool muted, unsigned col, int ct, int cb)
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

/* Blit the front video frame into the poster rect, letterboxed; caller holds browser_buf_lock. */
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

static void media_draw(struct media_el *m, int x0, int y0, int w, int fsh, int ct, int cb)
{
  struct mrects r;
  int playing = (m->ms && media_source_playing(m->ms)) || (m->vid && m->vplaying);
  unsigned pos = m->ms ? media_source_position_ms(m->ms) : (m->vid ? (unsigned)m->vpos_ms : 0);
  unsigned dur = m->ms ? media_source_duration_ms(m->ms) : (m->vid ? m->vdur_ms : 0);
  media_rects(m, x0, y0, w, fsh, &r);

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
    mtext(r.time_right - gfx_ui_width(line), r.bar_y + (MBAR_H - UIFONT_H) / 2, line, 0xb6bdc7, ct, cb);
  }

  draw_speaker_glyph(r.vol_x + 3, r.vol_y + 5, m->muted, 0xe6ebf2, ct, cb);
  if (r.cast_w) {
    unsigned col = remote_on(m) ? 0x4a90e2 : 0xe6ebf2;
    int bx = r.cast_x + 4, by = r.vol_y + 5;
    mfill(bx, by, 14, 1, col, ct, cb); /* screen outline, open at the bottom left */
    mfill(bx + 13, by, 1, 11, col, ct, cb);
    mfill(bx, by + 10, 14, 1, col, ct, cb);
    mfill(bx, by, 1, 4, col, ct, cb);
    for (int k = 1; k <= 3; k++) { /* receiver waves */
      mfill(bx, by + 4 + k * 2 + (k - 1), 1, 1, col, ct, cb);
      mfill(bx + k * 3 - 1, by + 10, 1, 1, col, ct, cb);
    }
  }
  if (r.fs_w) {
    int bx = r.fs_x + 5, by = r.vol_y + 5, s = 4; /* four corner brackets */
    for (int cx = 0; cx < 2; cx++)
      for (int cy = 0; cy < 2; cy++) {
        int px = bx + cx * 11, py = by + cy * 11;
        mfill(cx ? px - s + 1 : px, py, s, 1, 0xe6ebf2, ct, cb);
        mfill(px, cy ? py - s + 1 : py, 1, s, 0xe6ebf2, ct, cb);
      }
  }
}

static struct media_el *media_doc_find(int start)
{
  for (int i = start; i < MEDIA_MAX; i++) {
    struct media_el *m = &g_media[i];
    if (!m->in_use || m->owner != B->pg || m->text_pos < 0)
      continue;
    return m;
  }
  return 0;
}

struct media_el *media_doc_first(void) { return media_doc_find(0); }

struct media_el *media_doc_next(struct media_el *m)
{
  if (!m)
    return media_doc_first();
  int i = (int)(m - g_media) + 1;
  return i >= 0 && i < MEDIA_MAX ? media_doc_find(i) : 0;
}

int media_el_start_row(struct media_el *m) { return m ? m->start_row : -1; }
int media_el_rows(struct media_el *m) { return m ? m->rows : 0; }
int media_el_height(struct media_el *m, int paw) { return m ? media_total_h(m, paw) : 0; }

void media_el_draw(struct media_el *m, int x0, int y0, int paw, int ct, int cb)
{
  if (m)
    media_draw(m, x0, y0, paw, 0, ct, cb);
}

void media_fs_draw(struct media_el *m, int x0, int y0, int w, int h, int ct, int cb)
{
  if (m)
    media_draw(m, x0, y0, w, h, ct, cb);
}

/* A <video> that is the fullscreen element, or inside it (a player wrapper), fills the pane */
struct media_el *media_fullscreen(void)
{
  for (struct media_el *m = media_doc_first(); m; m = media_doc_next(m))
    if (m->kind == MK_VIDEO)
      for (const struct dom_node *n = m->node; n; n = n->parent)
        if (dom_get_attr(n, "fs-top"))
          return m;
  return 0;
}

/* Hand a fullscreen enter/exit for `m` to the worker, which owns the DOM */
static void media_post_fs(struct media_el *m, const char *type)
{
  int k = 0;
  if (B->evt_pending)
    return;
  B->evt_node = m->node;
  sapp(B->evt_type, &k, sizeof B->evt_type, type);
  B->evt_pending = 1;
  B->seq++;
}

/* Act on a click within the widget drawn at (x0,y0); caller checked the hit. */
static void media_click(struct media_el *m, int mx, int my, int x0, int y0, int paw, int fsh)
{
  struct mrects r;
  if (!m)
    return;
  media_rects(m, x0, y0, paw, fsh, &r);
  if (r.fs_w && mx >= r.fs_x && mx < r.fs_x + r.fs_w && my >= r.bar_y && my < r.bar_y + MBAR_H) {
    media_post_fs(m, fsh ? "escape" : "fsenter");
  } else if (r.cast_w && mx >= r.cast_x && mx < r.cast_x + r.cast_w && my >= r.bar_y && my < r.bar_y + MBAR_H) {
    media_post_fs(m, "cast");
  } else if (mx >= r.play_x && mx < r.play_x + r.play_w && my >= r.play_y - 4 && my < r.play_y + r.play_w + 4) {
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

void media_el_click(struct media_el *m, int mx, int my, int x0, int y0, int paw) { media_click(m, mx, my, x0, y0, paw, 0); }

void media_fs_click(struct media_el *m, int mx, int my, int x0, int y0, int w, int h)
{
  media_click(m, mx, my, x0, y0, w, h);
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

struct media_el *media_current(void)
{
  struct media_el *m = g_last_played;
  int i;
  for (i = 0; i < MEDIA_MAX; i++) {
    struct media_el *e = &g_media[i];
    if (e->in_use && e->owner == B->pg && ((e->ms && media_source_playing(e->ms)) || (e->vid && e->vplaying)))
      return e;
  }
  if (m && m->in_use && m->owner == B->pg && (m->ms || m->vid))
    return m;
  return 0;
}

void media_page_teardown(void)
{
  int i;
  r_close();
  for (i = 0; i < MEDIA_MAX; i++) {
    struct media_el *m = &g_media[i];
    if (!m->in_use || m->owner != B->pg)
      continue;
    if (m->vid || m->vbuf[0])
      video_dispose(m); /* off-lock: waits out a decode */
    browser_buf_lock();
    if (m->ms) {
      media_source_free(m->ms);
      m->ms = 0;
    }
    if (m->file[0])
      unlink(m->file);
    if (m->detached)
      dom_free(m->detached);
    memset(m, 0, sizeof *m);
    browser_buf_unlock();
  }
}
