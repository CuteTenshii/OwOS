/* player.c, a graphical media player for audio and H.264 video. `player <file>` opens a window
 * with a seekable progress bar and Restart / Play-Pause / Stop buttons */
#include "gfx.h"
#include "jpeg.h"
#include "media.h"
#include "mediactl.h"
#include "owos.h"
#include "png.h"
#include "tray.h"
#include "webp.h"
#include <stdbool.h>

/* audio-window geometry; g_ext shifts everything below the header down to make room for cover art */
#define COVER_SZ 96
static int g_ext;
#define CW 440
#define CH 180
#define HDR_H 40
#define BAR_X 24
#define BAR_Y (80 + g_ext)
#define TIME_Y (BAR_Y + GUI_PROGRESS_H + 6)
#define BTN_Y (128 + g_ext)

/* video display limits */
#define VID_MAXW 900
#define VID_MAXH 560
#define CTRL_H 96 /* transport strip below the video */
#define VID_MINW 360

/* transport / playback state */
enum { ST_EMPTY = 0, ST_STOPPED, ST_PLAYING, ST_PAUSED };
static int g_state = ST_EMPTY;
static int g_win;
static int g_quit;             /* leave the main loop: closed, or handed off to a new instance */
static int g_W = CW, g_H = CH; /* actual window client size */
static bool g_fs;              /* the window covers the screen */
static bool g_is_video;

static int g_press = -1, g_press_ticks;
#define NBTN 4 /* Open, Restart, Play/Pause, Stop */
static int g_bx[NBTN], g_bw[NBTN];
static const char *g_blabel[NBTN];

static char g_fn[256];
static struct media_tags g_tags;
static unsigned g_cov[COVER_SZ * COVER_SZ]; /* embedded cover, scaled to fit COVER_SZ */
static int g_cov_w, g_cov_h;

/* audio source */
static media_decoder *g_dec;
static int audio_live;

/* A video file's AAC audio track, played on the software mixer and used as the A/V master clock */
static media_source *g_asrc;

/* video source */
static media_video *g_vid;
static unsigned char *g_rgb;       /* native-size decoded frame (vw*vh*3) */
static int g_vw, g_vh;             /* native video dimensions */
static int g_dx, g_dy, g_dw, g_dh; /* on-screen video rectangle */
static bool g_frame_valid;         /* g_rgb holds a decoded, unshown frame */
static unsigned g_frame_pts;       /* its presentation time (ms) */
static long long g_shown_pts;      /* pts of the frame currently on screen */
static long long g_clock_origin;   /* msec() - playback position (ms) */

/* basename + time formatting */
static const char *basename_of(const char *p)
{
  const char *b = p;
  for (const char *q = p; *q; q++)
    if (*q == '/')
      b = q + 1;
  return b;
}

static const char *display_title(void) { return g_tags.title[0] ? g_tags.title : basename_of(g_fn); }

static void fmt_time(unsigned sec, char *buf)
{
  if (sec > 99 * 60 + 59)
    sec = 99 * 60 + 59;
  unsigned m = sec / 60, s = sec % 60;
  int k = 0;
  if (m >= 10)
    buf[k++] = '0' + m / 10;
  buf[k++] = '0' + m % 10;
  buf[k++] = ':';
  buf[k++] = '0' + s / 10;
  buf[k++] = '0' + s % 10;
  buf[k] = '\0';
}

static bool is_video_name(const char *p)
{
  int n = strlen(p);
  const char *ext[] = {".mp4", ".mov", ".m4v", ".264", ".h264"};
  for (int i = 0; i < 5; i++) {
    int l = strlen(ext[i]);
    if (n >= l && streq_ci(p + n - l, ext[i]))
      return true;
  }
  return false;
}

/* video frame blit */
/* Nearest-neighbour scale the native g_rgb frame into the on-screen rectangle */
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

/* loading */
static void copy_name(const char *fn)
{
  int i;
  for (i = 0; i < (int)sizeof g_fn - 1 && fn[i]; i++)
    g_fn[i] = fn[i];
  g_fn[i] = '\0';
}

/* Compute the on-screen video rectangle + window size from the native size */
static void layout_video(void)
{
  int sx = VID_MAXW * 256 / g_vw, sy = VID_MAXH * 256 / g_vh;
  int s = sx < sy ? sx : sy;
  if (s > 3 * 256)
    s = 3 * 256; /* cap upscaling of tiny clips */
  g_dw = g_vw * s / 256;
  g_dh = g_vh * s / 256;
  g_W = g_dw < VID_MINW ? VID_MINW : g_dw;
  g_H = g_dh + CTRL_H;
  g_dx = (g_W - g_dw) / 2;
  g_dy = 0;
}

/* Fullscreen: fit the frame above the transport strip, letterboxed and centred */
static void place_video(void)
{
  int ah = g_H - CTRL_H;
  int sx = g_W * 256 / g_vw, sy = ah * 256 / g_vh;
  int s = sx < sy ? sx : sy;
  g_dw = g_vw * s / 256;
  g_dh = g_vh * s / 256;
  g_dx = (g_W - g_dw) / 2;
  g_dy = (ah - g_dh) / 2;
  if (g_dw < 1 || g_dh < 1)
    g_dw = g_dh = 1;
}

/* (Re)open the video decoder and decode its first frame */
static int video_reopen(void)
{
  media_video *v;
  if (g_vid) {
    media_video_close(g_vid);
    g_vid = 0;
  }
  v = media_video_open(g_fn);
  if (!v)
    return -1;
  g_vid = v;
  g_frame_valid = false;
  g_shown_pts = -1;
  if (media_video_next(g_vid, g_rgb, &g_frame_pts) == 1) {
    g_frame_valid = true; /* first frame, shown on entry */
  }
  return 0;
}

/* Box-filters source rows into the cover thumbnail */
struct thumb {
  unsigned sw, sh;
  int tw, th;
  unsigned *acc; /* r, g, b, count per thumbnail pixel */
};

static int thumb_begin(struct thumb *t, unsigned sw, unsigned sh)
{
  if (!sw || !sh || sw > 16384 || sh > 16384)
    return -1;
  t->sw = sw;
  t->sh = sh;
  t->tw = sw >= sh ? COVER_SZ : (int)(sw * COVER_SZ / sh);
  t->th = sh >= sw ? COVER_SZ : (int)(sh * COVER_SZ / sw);
  if (sw < COVER_SZ && sh < COVER_SZ) {
    t->tw = (int)sw;
    t->th = (int)sh;
  }
  if (t->tw < 1)
    t->tw = 1;
  if (t->th < 1)
    t->th = 1;
  t->acc = calloc((unsigned)t->tw * t->th * 4, sizeof(unsigned));
  return t->acc ? 0 : -1;
}

static void thumb_row(struct thumb *t, unsigned y, const unsigned char *rgba)
{
  if (y >= t->sh)
    return;
  unsigned *row = t->acc + (unsigned)((unsigned long long)y * t->th / t->sh) * t->tw * 4;
  for (unsigned x = 0; x < t->sw; x++) {
    unsigned *a = row + (unsigned)((unsigned long long)x * t->tw / t->sw) * 4;
    a[0] += rgba[x * 4];
    a[1] += rgba[x * 4 + 1];
    a[2] += rgba[x * 4 + 2];
    a[3]++;
  }
}

static void thumb_end(struct thumb *t)
{
  for (int i = 0; i < t->tw * t->th; i++) {
    const unsigned *a = t->acc + i * 4;
    unsigned n = a[3] ? a[3] : 1;
    g_cov[i] = 0xFF000000u | (a[0] / n) << 16 | (a[1] / n) << 8 | a[2] / n;
  }
  g_cov_w = t->tw;
  g_cov_h = t->th;
  free(t->acc);
}

struct membuf {
  const unsigned char *d;
  unsigned len, pos;
};

static int membuf_get(void *c)
{
  struct membuf *m = c;
  return m->pos < m->len ? m->d[m->pos++] : -1;
}

static int png_thumb_row(void *c, unsigned y, const unsigned char *rgba, unsigned w)
{
  struct thumb *t = c;
  if (w == t->sw)
    thumb_row(t, y, rgba);
  return 0;
}

static void load_cover(const char *fn)
{
  unsigned len, w = 0, h = 0;
  unsigned char *img = media_cover_read(fn, &len), *rgba = 0;
  struct thumb t;
  g_cov_w = g_cov_h = g_ext = 0;
  if (!img)
    return;
  if (len > 8 && memcmp(img, "\x89PNG", 4) == 0) {
    struct membuf m = {img, len, 0};
    if (png_dimensions(membuf_get, &m, &w, &h) == PNG_OK && thumb_begin(&t, w, h) == 0) {
      m.pos = 0;
      png_decode(membuf_get, &m, png_thumb_row, &t, 0, 0);
      thumb_end(&t);
    }
  } else {
    if (len > 3 && img[0] == 0xFF && img[1] == 0xD8)
      jpeg_decode(img, (int)len, COVER_SZ, &rgba, &w, &h);
    else if (len > 12 && memcmp(img, "RIFF", 4) == 0 && memcmp(img + 8, "WEBP", 4) == 0)
      webp_decode(img, (int)len, &rgba, &w, &h);
    if (rgba && thumb_begin(&t, w, h) == 0) {
      for (unsigned y = 0; y < h; y++)
        thumb_row(&t, y, rgba + (unsigned long long)y * w * 4);
      thumb_end(&t);
    }
  }
  free(rgba);
  free(img);
  if (g_cov_w)
    g_ext = COVER_SZ + 8;
}

static int load_audio(const char *fn)
{
  media_decoder *d = media_open(fn);
  if (!d)
    return -1;
  if (g_dec)
    media_close(g_dec);
  g_dec = d;
  copy_name(fn);
  media_tags_read(fn, &g_tags);
  load_cover(fn);
  g_is_video = false;
  g_state = ST_STOPPED;
  return 0;
}

static int load_video(const char *fn)
{
  media_video *v = media_video_open(fn);
  if (!v)
    return -1;
  g_vw = media_video_width(v);
  g_vh = media_video_height(v);
  media_video_close(v); /* reopened after we size things */
  if (g_vw < 1 || g_vh < 1)
    return -1;
  g_rgb = malloc((unsigned)g_vw * g_vh * 3);
  if (!g_rgb)
    return -1;
  copy_name(fn);
  media_tags_read(fn, &g_tags);
  g_is_video = true;
  layout_video();
  if (video_reopen() != 0)
    return -1;
  /* Same container's audio track, played via the mixer thread so it never blocks the frame loop */
  {
    media_decoder *ad = media_open(fn);
    g_asrc = ad ? media_source_new(ad, 0, 0) : 0;
  }
  g_state = ST_STOPPED;
  return 0;
}

/* The video playback clock in ms */
static long long video_clock_ms(void)
{
  if (g_asrc)
    return (long long)media_source_position_ms(g_asrc);
  return (long long)msec() - g_clock_origin;
}

/* transport */
static void audio_seek_fraction(int num, int den)
{
  unsigned dur = media_duration_ms(g_dec);
  if (den <= 0)
    den = 1;
  if (num < 0)
    num = 0;
  if (num > den)
    num = den;
  media_seek_ms(g_dec, dur ? (unsigned)((unsigned long long)dur * (unsigned)num / (unsigned)den) : 0);
}

static void transport_play(void)
{
  if (g_state == ST_EMPTY || g_state == ST_PLAYING)
    return;
  if (g_is_video) {
    if (g_shown_pts < 0)
      g_shown_pts = 0;
    g_clock_origin = (long long)msec() - g_shown_pts; /* resume the clock */
    if (g_asrc)
      media_source_play(g_asrc);
    g_state = ST_PLAYING;
    return;
  }
  if (audio_open() != 0)
    return;
  audio_live = 1;
  g_state = ST_PLAYING;
}

static void transport_pause(void)
{
  if (g_state != ST_PLAYING)
    return;
  if (!g_is_video && audio_live) {
    audio_close();
    audio_live = 0;
  }
  if (g_is_video && g_asrc)
    media_source_pause(g_asrc);
  g_state = ST_PAUSED;
}

static void transport_stop(void)
{
  if (g_state == ST_EMPTY)
    return;
  if (g_is_video) {
    if (g_asrc) {
      media_source_pause(g_asrc);
      media_source_seek_ms(g_asrc, 0);
    }
    video_reopen(); /* back to frame 0 */
    g_state = ST_STOPPED;
    return;
  }
  if (audio_live) {
    audio_close();
    audio_live = 0;
  }
  media_seek_ms(g_dec, 0);
  g_state = ST_STOPPED;
}

static void transport_restart(void)
{
  if (g_state == ST_EMPTY)
    return;
  if (g_is_video) {
    int was_playing = (g_state == ST_PLAYING);
    video_reopen();
    g_clock_origin = msec();
    if (g_asrc) {
      media_source_seek_ms(g_asrc, 0);
      if (was_playing)
        media_source_play(g_asrc);
      else
        media_source_pause(g_asrc);
    }
    if (was_playing)
      g_state = ST_PLAYING;
    return;
  }
  media_seek_ms(g_dec, 0);
}

static void transport_toggle(void)
{
  if (g_state == ST_PLAYING)
    transport_pause();
  else
    transport_play();
}

/* system media session */
#define SESSION_SYNC_MS 250

static int g_sid = -1;
static long long g_sync_at;

static unsigned session_pos_ms(void)
{
  if (!g_is_video)
    return media_position_ms(g_dec);
  return g_shown_pts < 0 ? 0 : (unsigned)g_shown_pts;
}

static void session_seek(unsigned ms)
{
  if (g_is_video)
    return;
  unsigned dur = media_duration_ms(g_dec);
  media_seek_ms(g_dec, dur && ms > dur ? dur : ms);
}

static const struct tray_menu_item player_menu[] = {
    {"Play / Pause", "mediactl toggle"},
    {"Next", "mediactl next"},
    {"Previous", "mediactl prev"},
    {"Stop", "mediactl stop"},
};

static void session_sync(void)
{
  long long now = (long long)msec();
  if (g_state == ST_EMPTY || now < g_sync_at)
    return;
  g_sync_at = now + SESSION_SYNC_MS;
  if (g_sid < 0) {
    if ((g_sid = mediactl_open("Player")) < 0)
      return;
    struct tray_icon t = {.pid = getpid(),
                          .id = "player",
                          .icon = "multimedia-player",
                          .action = "mediactl toggle",
                          .menu = player_menu,
                          .nmenu = sizeof player_menu / sizeof player_menu[0]};
    tray_set(&t);
  }
  struct media_info in = {0};
  strlcpy(in.app, "Player", sizeof in.app);
  strlcpy(in.title, display_title(), sizeof in.title);
  strlcpy(in.artist, g_tags.artist, sizeof in.artist);
  strlcpy(in.album, g_tags.album, sizeof in.album);
  in.state = g_state == ST_PLAYING ? MEDIA_PLAYING : g_state == ST_PAUSED ? MEDIA_PAUSED : MEDIA_STOPPED;
  in.pos_ms = session_pos_ms();
  in.dur_ms = g_is_video ? media_video_duration_ms(g_vid) : media_duration_ms(g_dec);
  in.caps = MEDIA_CAP(MEDIA_CMD_PLAY) | MEDIA_CAP(MEDIA_CMD_PAUSE) | MEDIA_CAP(MEDIA_CMD_TOGGLE) |
            MEDIA_CAP(MEDIA_CMD_STOP) | MEDIA_CAP(MEDIA_CMD_PREV) | MEDIA_CAP(MEDIA_CMD_NEXT);
  if (!g_is_video)
    in.caps |= MEDIA_CAP(MEDIA_CMD_SEEK_FWD) | MEDIA_CAP(MEDIA_CMD_SEEK_BACK) | MEDIA_CAP(MEDIA_CMD_SEEK_TO);
  if (mediactl_update(g_sid, &in) < 0) {
    g_sid = -1;
    return;
  }
  unsigned arg;
  int cmd;
  while ((cmd = mediactl_poll(g_sid, &arg)) > MEDIA_CMD_NONE) {
    unsigned step = arg ? arg : 10000;
    switch (cmd) {
    case MEDIA_CMD_PLAY:
      transport_play();
      break;
    case MEDIA_CMD_PAUSE:
      transport_pause();
      break;
    case MEDIA_CMD_TOGGLE:
      transport_toggle();
      break;
    case MEDIA_CMD_STOP:
    case MEDIA_CMD_NEXT:
      transport_stop(); /* no playlist: next ends the track */
      break;
    case MEDIA_CMD_PREV:
      transport_restart();
      break;
    case MEDIA_CMD_SEEK_FWD:
      session_seek(session_pos_ms() + step);
      break;
    case MEDIA_CMD_SEEK_BACK:
      session_seek(session_pos_ms() > step ? session_pos_ms() - step : 0);
      break;
    case MEDIA_CMD_SEEK_TO:
      session_seek(arg);
      break;
    }
  }
}

/* rendering */
#define COL_BG 0x10141cu
#define COL_HDR 0x1a2230u
#define COL_TITLE 0xe6ebf2u
#define COL_DIM 0x8a93a3u
#define COL_VIDBG 0x000000u

static void layout_buttons(void)
{
  g_blabel[0] = "Open";
  g_blabel[1] = "Restart";
  g_blabel[2] = (g_state == ST_PLAYING) ? "Pause" : "Play";
  g_blabel[3] = "Stop";
  int total = 0;
  for (int i = 0; i < NBTN; i++) {
    int w = gfx_ui_width(g_blabel[i]) + 28;
    if (w < 78)
      w = 78;
    g_bw[i] = w;
    total += w;
  }
  total += 12 * (NBTN - 1);
  int x = (g_W - total) / 2;
  for (int i = 0; i < NBTN; i++) {
    g_bx[i] = x;
    x += g_bw[i] + 12;
  }
}

/* Draw the progress bar + time + buttons of the transport strip starting at client-y `top` */
static void draw_controls(int top, unsigned bg, unsigned pos_ms, unsigned dur_ms)
{
  int bar_y = top + 14, time_y = bar_y + GUI_PROGRESS_H + 6;
  int btn_y = g_H - GUI_BTN_H - 14;
  char el[8], tt[8];
  gfx_fill_rect(0, top, g_W, g_H - top, bg);
  progress_draw(BAR_X, bar_y, g_W - 2 * BAR_X, GUI_PROGRESS_H, (int)(dur_ms && pos_ms > dur_ms ? dur_ms : pos_ms),
                (int)(dur_ms ? dur_ms : 1), 0, NULL);
  fmt_time(pos_ms / 1000, el);
  gfx_ui_text(BAR_X, time_y, el, COL_DIM);
  if (dur_ms) {
    fmt_time(dur_ms / 1000, tt);
  } else {
    tt[0] = '-';
    tt[1] = '-';
    tt[2] = ':';
    tt[3] = '-';
    tt[4] = '-';
    tt[5] = '\0';
  }
  gfx_ui_text(g_W - BAR_X - gfx_ui_width(tt), time_y, tt, COL_DIM);

  layout_buttons();
  for (int i = 0; i < NBTN; i++) {
    int st = (g_press == i && g_press_ticks > 0) ? BTN_PRESSED : BTN_NORMAL;
    button_draw(g_bx[i], btn_y, g_bw[i], GUI_BTN_H, g_blabel[i], st, NULL);
  }
  if (g_press_ticks > 0 && --g_press_ticks == 0)
    g_press = -1;
}

static void render_audio(void)
{
  int have = (g_state != ST_EMPTY);
  gfx_clear(COL_BG);
  gfx_fill_rect(0, 0, g_W, HDR_H, COL_HDR);
  const char *title = have ? display_title() : "No file loaded";
  int tw = g_W - 2 * BAR_X;
  gfx_ui_text_clip(g_W / 2 - (gfx_ui_width(title) < tw ? gfx_ui_width(title) : tw) / 2, (HDR_H - UIFONT_H) / 2, title,
                   COL_TITLE, tw);

  if (!have) {
    const char *h1 = "Open a song or video, or run from the terminal:";
    const char *h2 = "player <file.mp3 | file.mp4>";
    gfx_ui_text((g_W - gfx_ui_width(h1)) / 2, 78, h1, COL_DIM);
    gfx_ui_text((g_W - gfx_ui_width(h2)) / 2, 78 + UIFONT_H + 4, h2, COL_TITLE);
    layout_buttons();
    for (int i = 0; i < NBTN; i++)
      button_draw(g_bx[i], BTN_Y, g_bw[i], GUI_BTN_H, g_blabel[i], i == 0 ? BTN_NORMAL : BTN_DISABLED, NULL);
    return;
  }
  unsigned pos = media_position_ms(g_dec), dur = media_duration_ms(g_dec);
  int bar_y = BAR_Y;
  gfx_fill_rect(0, HDR_H, g_W, g_H - HDR_H, COL_BG);
  if (g_cov_w) {
    unsigned *fb = win_fb();
    int cx = (g_W - g_cov_w) / 2, cy = HDR_H + 8 + (COVER_SZ - g_cov_h) / 2;
    for (int y = 0; y < g_cov_h; y++)
      memcpy(fb + (unsigned)(cy + y) * g_W + cx, g_cov + y * g_cov_w, (unsigned)g_cov_w * 4);
  }
  if (g_tags.artist[0] || g_tags.album[0]) {
    char sub[2 * MEDIA_TAG_MAX + 4];
    int n = strlen(g_tags.artist);
    strlcpy(sub, g_tags.artist, sizeof sub);
    if (n && g_tags.album[0]) {
      strlcpy(sub + n, " - ", 4);
      n += 3;
    }
    strlcpy(sub + n, g_tags.album, sizeof sub - n);
    int sw = gfx_ui_width(sub) < tw ? gfx_ui_width(sub) : tw;
    gfx_ui_text_clip(g_W / 2 - sw / 2, HDR_H + 14 + g_ext, sub, COL_DIM, tw);
  }
  progress_draw(BAR_X, bar_y, g_W - 2 * BAR_X, GUI_PROGRESS_H, (int)(dur && pos > dur ? dur : pos),
                (int)(dur ? dur : 1), 0, NULL);
  char el[8], tt[8];
  fmt_time(pos / 1000, el);
  gfx_ui_text(BAR_X, TIME_Y, el, COL_DIM);
  if (dur)
    fmt_time(dur / 1000, tt);
  else {
    tt[0] = '-';
    tt[1] = '-';
    tt[2] = ':';
    tt[3] = '-';
    tt[4] = '-';
    tt[5] = '\0';
  }
  gfx_ui_text(g_W - BAR_X - gfx_ui_width(tt), TIME_Y, tt, COL_DIM);
  layout_buttons();
  for (int i = 0; i < NBTN; i++) {
    int st = (g_press == i && g_press_ticks > 0) ? BTN_PRESSED : BTN_NORMAL;
    button_draw(g_bx[i], BTN_Y, g_bw[i], GUI_BTN_H, g_blabel[i], st, NULL);
  }
  if (g_press_ticks > 0 && --g_press_ticks == 0)
    g_press = -1;
}

static void render_video_controls(void)
{
  unsigned pos = g_shown_pts < 0 ? 0 : (unsigned)g_shown_pts;
  draw_controls(g_H - CTRL_H, COL_HDR, pos, media_video_duration_ms(g_vid));
}

/* input */
static const char *const k_media_exts[] = {".wav", ".mp3", ".mp2", ".mp1", ".aac",  ".m4a",
                                           ".mp4", ".mov", ".m4v", ".264", ".h264", NULL};

/* Ask for a file and replace this window with a player on it (the window size follows the media);
 * 1 if one was launched */
static int open_file(void)
{
  char dir[sizeof g_fn] = "/", path[256];
  if (g_state != ST_EMPTY) {
    strlcpy(dir, g_fn, sizeof dir);
    filelist_parent(dir);
  }
  struct filepicker_opts o = {.title = "Open Media", .start = dir, .filter_label = "Media files", .exts = k_media_exts};
  return filepicker(g_win, &o, path, sizeof path) && spawn("/bin/player", path) >= 0;
}

static void on_click(int mx, int my)
{
  if (g_state == ST_EMPTY && !button_hit(g_bx[0], BTN_Y, g_bw[0], GUI_BTN_H, mx, my, NULL))
    return;

  if (!g_is_video && mx >= BAR_X && mx < g_W - BAR_X && my >= BAR_Y - 4 && my < BAR_Y + GUI_PROGRESS_H + 4) {
    audio_seek_fraction(mx - BAR_X, g_W - 2 * BAR_X);
    return;
  }

  int btn_y = g_is_video ? g_H - GUI_BTN_H - 14 : BTN_Y;
  for (int i = 0; i < NBTN; i++) {
    if (button_hit(g_bx[i], btn_y, g_bw[i], GUI_BTN_H, mx, my, NULL)) {
      g_press = i;
      g_press_ticks = 4;
      if (i == 0)
        g_quit = open_file();
      else if (i == 1)
        transport_restart();
      else if (i == 2)
        transport_toggle();
      else
        transport_stop();
      return;
    }
  }
}

static void set_fullscreen(bool on)
{
  if (!g_is_video || g_fs == on)
    return;
  g_fs = on;
  win_fullscreen(g_win, on);
}

/* The compositor resized the window (entering or leaving fullscreen) */
static void on_resize(int w, int h)
{
  g_W = w;
  g_H = h;
  gfx_init(win_fb(), g_W, g_H);
  if (g_fs)
    place_video();
  else
    layout_video();
  gfx_fill_rect(0, 0, g_W, g_H - CTRL_H, COL_VIDBG);
  if (g_shown_pts >= 0)
    blit_video();
}

/* video playback step */
/* Advance the video: decode one frame ahead and show it once its time is due */
static int video_step(void)
{
  int blitted = 0;
  if (g_state == ST_PLAYING) {
    if (!g_frame_valid) {
      int r = media_video_next(g_vid, g_rgb, &g_frame_pts);
      if (r == 1)
        g_frame_valid = true;
      else {
        transport_pause();
      } /* EOS / error: hold on last frame */
    }
    if (g_frame_valid) {
      long long elapsed = video_clock_ms();
      if ((long long)g_frame_pts <= elapsed) {
        blit_video();
        g_shown_pts = g_frame_pts;
        g_frame_valid = false;
        blitted = 1;
      }
    }
  } else if (g_frame_valid) {
    /* stopped/paused with a freshly decoded first frame: show it once */
    blit_video();
    g_shown_pts = g_frame_pts;
    g_frame_valid = false;
    blitted = 1;
  }
  return blitted;
}

static void player_quit(void)
{
  if (g_sid >= 0) {
    mediactl_close(g_sid);
    tray_remove("player");
  }
  if (audio_live)
    audio_close();
  if (g_asrc)
    media_source_free(g_asrc); /* mixer frees its decoder */
  if (g_dec)
    media_close(g_dec);
  if (g_vid)
    media_video_close(g_vid);
  free(g_rgb);
  win_close(g_win);
}

int main(int argc, char **argv)
{
  if (argc >= 2 && is_video_name(argv[1]) && load_video(argv[1]) == 0) {
    /* window sized to the video below */
  } else if (argc >= 2 && load_audio(argv[1]) == 0) {
    g_W = CW;
    g_H = CH + g_ext;
  } else {
    g_W = CW;
    g_H = CH;
    if (argc >= 2)
      puts("player: not a playable media file\n");
  }

  g_win = win_create(g_W, g_H, g_is_video ? "Video Player" : "Media Player");
  if (g_win < 0) {
    puts("player: could not open a window\n");
    return 1;
  }
  win_resizable(g_win, 0);
  gfx_init(win_fb(), g_W, g_H);

  if (g_state == ST_STOPPED)
    transport_play(); /* autoplay the given file */

  static short buf[4096];
  const unsigned OUT = sizeof(buf) / 4;

  for (;;) {
    struct win_event ev;
    int new_frame = 0;
    session_sync();
    while (win_poll(g_win, &ev)) {
      if (ev.type == WEV_CLOSE) {
        g_quit = 1;
        break;
      }
      if (ev.type == WEV_MOUSE)
        on_click(ev.a, ev.b);
      else if (ev.type == WEV_RESIZE)
        on_resize(ev.a, ev.b);
      else if (ev.type == WEV_DBLCLICK && g_is_video && ev.b < g_H - CTRL_H)
        set_fullscreen(!g_fs);
      else if (ev.type == WEV_KEY && (ev.a == 'f' || ev.a == 'F' || ev.a == KEY_F11))
        set_fullscreen(!g_fs);
      else if (ev.type == WEV_KEY && ev.a == 27)
        set_fullscreen(false);
      else if (ev.type == WEV_KEY && ev.a == KEY_OPEN)
        g_quit = open_file();
      else if (ev.type == WEV_KEY && (ev.a == ' ' || ev.a == KEY_MEDIA_PLAY))
        transport_toggle();
      else if (ev.type == WEV_KEY && (ev.a == KEY_MEDIA_STOP || ev.a == KEY_MEDIA_NEXT))
        transport_stop(); /* no playlist: next ends the track */
      else if (ev.type == WEV_KEY && ev.a == KEY_MEDIA_PREV)
        transport_restart();
    }
    if (g_quit) {
      player_quit();
      return 0;
    }

    if (g_is_video) {
      new_frame = video_step();
      render_video_controls();
      win_present(g_win);
      if (g_state == ST_PLAYING && !new_frame)
        msleep(3);
      else if (g_state != ST_PLAYING)
        win_wait(g_win);
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
          if (w < 0) {
            transport_pause();
            break;
          }
          off += w;
        }
      }
    }
    render_audio();
    win_present(g_win);
    if (g_state != ST_PLAYING)
      win_wait(g_win);
  }
}
