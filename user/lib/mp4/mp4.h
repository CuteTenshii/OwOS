/* mp4.h — an ISO base media file format (MP4 / .mov / .m4a / .m4v) demuxer.
 *
 * "mostly for mp4s": before any video frame can be decoded, the container has
 * to be taken apart — the box tree walked, the tracks found, and the compressed
 * samples (H.264 access units, AAC frames, ...) located in the file. That
 * parsing is pure computation over untrusted bytes, so it lives in user space
 * like the WebP and PNG decoders: a malformed or hostile file can only fault
 * this process, never the kernel.
 *
 * This layer is deliberately codec-agnostic. It identifies the codec of each
 * track (H.264/H.265/MPEG-4 Part 2 video, AAC/MP3 audio) and hands back the
 * codec configuration (an avcC/hvcC/esds payload) plus a flat, random-access
 * sample table — but it decodes nothing itself. A video decoder is the consumer
 * built on top of it; the audio path already exists (user/lib/mp3,
 * user/lib/media).
 *
 * Scope: the moov metadata is parsed from a whole-file in-memory buffer (like
 * the WebP decoder — backward references in the sample tables need random
 * access). Fragmented MP4 (moof/mfra, used by DASH/streaming) is not yet read;
 * a plain progressive-download or locally-stored file (one moov, samples in
 * mdat) is.
 */
#ifndef OWOS_MP4_H
#define OWOS_MP4_H

enum {
  MP4_OK = 0,
  MP4_EBADSIG = -1, /* not an ISO base media file (no ftyp/moov)          */
  MP4_EFORMAT = -2, /* corrupt or truncated box structure                 */
  MP4_ENOMEM = -3,  /* allocation failed                                  */
  MP4_EUNSUP = -4,  /* structurally valid but nothing we can expose       */
};

/* The codec carried by a track's samples, from the sample-entry fourcc (video)
 * or the esds object-type (audio). UNKNOWN means "recognized track, codec we
 * don't name" — the samples are still extractable. */
typedef enum {
  MP4_CODEC_UNKNOWN = 0,
  MP4_CODEC_H264,  /* avc1 / avc3  (ISO/IEC 14496-10)                    */
  MP4_CODEC_H265,  /* hvc1 / hev1  (ISO/IEC 23008-2)                     */
  MP4_CODEC_MPEG4, /* mp4v         (ISO/IEC 14496-2, "DivX/Xvid")        */
  MP4_CODEC_AAC,   /* mp4a, object-type 0x40                             */
  MP4_CODEC_MP3,   /* mp4a, object-type 0x66/0x67/0x68/0x69/0x6B         */
} mp4_codec;

/* One compressed sample (a video access unit or an audio frame).
 * `offset`/`size` locate it in the buffer passed to mp4_open; `pts`/`dts` are
 * in the track's own timescale units (divide by mp4_track.timescale for
 * seconds). For a video track whose codec prefixes NAL units with a length
 * (H.264/H.265), the raw sample data is length-prefixed — see
 * mp4_track.nal_length_size. */
typedef struct {
  unsigned offset; /* byte offset into the mp4_open buffer     */
  unsigned size;   /* sample size in bytes                     */
  long long dts;   /* decode timestamp, timescale units        */
  long long pts;   /* presentation timestamp, timescale units  */
  int keyframe;    /* 1 if a sync sample (random-access point)  */
} mp4_sample;

typedef struct {
  int is_video; /* 1 = video ('vide'), 0 = audio ('soun')   */
  mp4_codec codec;
  unsigned timescale;          /* timescale units per second               */
  unsigned long long duration; /* track duration, timescale units          */
  long long edit_start;        /* leading media (priming) the edit list skips, timescale units */

  /* Video: coded picture dimensions (from the sample entry). */
  unsigned width, height;
  /* Audio: PCM shape declared by the sample entry. */
  unsigned channels, samplerate, bits;

  /* Codec configuration (extradata): the avcC record for H.264, hvcC for
   * H.265, or the AudioSpecificConfig for AAC. Points into the mp4_open
   * buffer; NULL/0 if the track carries none. */
  const unsigned char *extradata;
  unsigned extradata_len;
  /* H.264/H.265: bytes prefixing each NAL unit in a sample (1..4). 0 for
   * codecs that don't length-prefix. */
  unsigned nal_length_size;

  /* The flat sample table, in decode order, owned by the mp4 object. */
  mp4_sample *samples;
  unsigned n_samples;
} mp4_track;

typedef struct mp4 mp4;

/* Parse the metadata of the whole-file MP4 in `buf`/`len`. On MP4_OK, *out owns
 * a parsed object whose sample tables reference `buf` directly, so `buf` must
 * outlive the mp4 object. Returns MP4_OK or an MP4_E* code (*out left NULL). */
int mp4_open(const unsigned char *buf, unsigned len, mp4 **out);
void mp4_close(mp4 *m);

int mp4_track_count(const mp4 *m);
const mp4_track *mp4_track_at(const mp4 *m, int i);
/* The primary video / audio track, or NULL if the file has none. */
const mp4_track *mp4_video_track(const mp4 *m);
const mp4_track *mp4_audio_track(const mp4 *m);

/* Pointer to sample `i`'s compressed bytes in the buffer, and its size via
 * *size. NULL if `i` is out of range. */
const unsigned char *mp4_sample_data(const mp4 *m, const mp4_track *t, unsigned i, unsigned *size);

/* Find the sample index to start decoding at to present time `pts` (timescale
 * units): the last keyframe at or before `pts`. Returns 0 if the track has no
 * samples. */
unsigned mp4_seek_sample(const mp4_track *t, long long pts);

/* Iterate the SPS/PPS parameter sets in an H.264 track's avcC extradata.
 * `which` selects the list (0 = SPS, 1 = PPS); `idx` the set within it. On
 * success fills *nal and *nal_len with a pointer into the buffer and returns
 * MP4_OK; MP4_EFORMAT when `idx` is past the end or the track is not H.264. */
int mp4_h264_param_set(const mp4_track *t, int which, int idx, const unsigned char **nal, unsigned *nal_len);

#endif /* OWOS_MP4_H */
