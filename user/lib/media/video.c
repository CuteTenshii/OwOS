/* video.c — the video half of user/lib/media (see media.h): a local H.264 file
 * decoded to timed RGB frames.
 *
 * Two containers, one core: a raw Annex-B elementary stream is fed straight to
 * the decoder (h264_decoder_open / h264_decode_frame), and an MP4/MOV file is
 * taken apart by user/lib/mp4 first — its H.264 track's avcC seeds the decoder
 * and each sample is pushed as an access unit. Baseline H.264 has no B slices,
 * so pictures come out in display order and each sample's presentation time
 * flows straight through; a small FIFO carries the timestamps across the
 * decoder's one-frame reorder delay. The whole file is held resident because
 * the demuxer's sample table needs random access into it.
 */
#include "h264.h"
#include "media.h"
#include "mp4.h"
#include "owos.h"

#define PTS_FIFO 32

struct media_video {
  unsigned char *file; /* whole file, resident                          */
  unsigned file_len;
  h264_decoder *dec;

  int is_mp4;
  mp4 *m4;
  const mp4_track *track; /* the H.264 video track                         */
  unsigned next_sample;
  int flushing; /* fed all samples; draining reorder buffer      */

  unsigned pts_ms[PTS_FIFO]; /* timestamps of pictures in flight              */
  int pts_head, pts_tail;

  int width, height;
  unsigned duration_ms;

  int frame_idx;        /* Annex-B: synthetic frame counter              */
  int fps_num, fps_den; /* Annex-B: assumed frame rate                   */
};

static unsigned char *read_whole(const char *path, unsigned *out_len)
{
  struct statinfo st;
  unsigned char *buf;
  unsigned off = 0;
  if (stat(path, &st) != 0 || st.size == 0)
    return 0;
  buf = malloc(st.size);
  if (!buf)
    return 0;
  while (off < st.size) {
    int n = readfile_at(path, off, buf + off, st.size - off);
    if (n <= 0)
      break;
    off += (unsigned)n;
  }
  if (off != st.size) {
    free(buf);
    return 0;
  }
  *out_len = st.size;
  return buf;
}

static int ends_with(const char *s, const char *suf)
{
  int ls = strlen(s), lf = strlen(suf);
  return ls >= lf && streq_ci(s + ls - lf, suf);
}

static void pts_push(struct media_video *v, unsigned ms)
{
  v->pts_ms[v->pts_tail] = ms;
  v->pts_tail = (v->pts_tail + 1) % PTS_FIFO;
  if (v->pts_tail == v->pts_head) /* full: drop the oldest */
    v->pts_head = (v->pts_head + 1) % PTS_FIFO;
}

static unsigned pts_pop(struct media_video *v)
{
  unsigned ms;
  if (v->pts_head == v->pts_tail)
    return v->frame_idx * 40u; /* fallback */
  ms = v->pts_ms[v->pts_head];
  v->pts_head = (v->pts_head + 1) % PTS_FIFO;
  return ms;
}

media_video *media_video_open(const char *path)
{
  struct media_video *v = (struct media_video *)malloc(sizeof *v);
  if (!v)
    return 0;
  memset(v, 0, sizeof *v);
  v->fps_num = 25;
  v->fps_den = 1;
  v->pts_head = v->pts_tail = 0;

  v->file = read_whole(path, &v->file_len);
  if (!v->file) {
    free(v);
    return 0;
  }

  /* Sniff the container by content (the caller's file may be extension-less,
   * e.g. the browser's media cache); fall back to the filename. An ISO-BMFF
   * file opens with a box whose type is at bytes 4..7; an Annex-B stream with a
   * start code. */
  v->is_mp4 = ends_with(path, ".mp4") || ends_with(path, ".mov") || ends_with(path, ".m4v");
  if (v->file_len >= 12) {
    const unsigned char *p = v->file;
    if (!memcmp(p + 4, "ftyp", 4) || !memcmp(p + 4, "moov", 4) || !memcmp(p + 4, "styp", 4) ||
        !memcmp(p + 4, "mdat", 4) || !memcmp(p + 4, "free", 4))
      v->is_mp4 = 1;
    else if ((p[0] == 0 && p[1] == 0 && p[2] == 1) || (p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1))
      v->is_mp4 = 0;
  }
  if (v->is_mp4) {
    if (mp4_open(v->file, v->file_len, &v->m4) != MP4_OK)
      goto fail;
    v->track = mp4_video_track(v->m4);
    if (!v->track || v->track->codec != MP4_CODEC_H264)
      goto fail;
    if (h264_decoder_open_avcc(v->track->extradata, v->track->extradata_len, &v->dec) != H264_OK)
      goto fail;
    if (v->track->timescale)
      v->duration_ms = (unsigned)(v->track->duration * 1000ull / v->track->timescale);
  } else {
    if (h264_decoder_open(v->file, v->file_len, &v->dec) != H264_OK)
      goto fail;
  }
  v->width = h264_width(v->dec);
  v->height = h264_height(v->dec);
  if (v->width < 1 || v->height < 1)
    goto fail;
  return v;

fail:
  media_video_close(v);
  return 0;
}

void media_video_close(media_video *v)
{
  if (!v)
    return;
  if (v->dec)
    h264_decoder_close(v->dec);
  if (v->m4)
    mp4_close(v->m4);
  free(v->file);
  free(v);
}

int media_video_width(const media_video *v) { return v ? v->width : 0; }
int media_video_height(const media_video *v) { return v ? v->height : 0; }
unsigned media_video_duration_ms(const media_video *v) { return v ? v->duration_ms : 0; }

int media_video_next(media_video *v, unsigned char *rgb, unsigned *pts_ms)
{
  if (!v || !rgb)
    return -1;

  if (!v->is_mp4) {
    int rc = h264_decode_frame(v->dec);
    if (rc == H264_EEOF)
      return 0;
    if (rc != H264_OK)
      return -1;
    if (pts_ms)
      *pts_ms = (unsigned)((long long)v->frame_idx * 1000 * v->fps_den / v->fps_num);
    v->frame_idx++;
    return h264_frame_rgb(v->dec, rgb) == H264_OK ? 1 : -1;
  }

  for (;;) {
    if (v->flushing) {
      if (h264_flush(v->dec) != H264_OK)
        return 0;
      if (pts_ms)
        *pts_ms = pts_pop(v);
      v->frame_idx++;
      return h264_frame_rgb(v->dec, rgb) == H264_OK ? 1 : -1;
    }
    if (v->next_sample >= v->track->n_samples) {
      v->flushing = 1;
      continue;
    }
    {
      unsigned sz;
      const unsigned char *s = mp4_sample_data(v->m4, v->track, v->next_sample, &sz);
      long long p = v->track->samples[v->next_sample].pts;
      int rc;
      if (v->track->timescale && p >= 0)
        pts_push(v, (unsigned)(p * 1000ll / v->track->timescale));
      else
        pts_push(v, v->next_sample * 40u);
      v->next_sample++;
      if (!s)
        continue;
      rc = h264_decode_au(v->dec, s, sz, v->track->nal_length_size);
      if (rc == H264_OK) {
        if (pts_ms)
          *pts_ms = pts_pop(v);
        v->frame_idx++;
        return h264_frame_rgb(v->dec, rgb) == H264_OK ? 1 : -1;
      }
      if (rc != H264_EAGAIN)
        return -1;
    }
  }
}
