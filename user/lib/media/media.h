/* media.h — user-space media playback: a file decoder normalized to the audio
 * device's canonical 48 kHz / 16-bit / stereo, plus a software mixer that owns
 * the single system audio stream on its own thread and blends several sources.
 *
 * Two layers, usable apart:
 *   - media_decoder: open a local WAV or MPEG file and pull 48 kHz/S16/stereo
 *     frames from it, seekable, with a duration estimate. Built on user/lib/mp3
 *     for MPEG and an inline RIFF/WAVE reader for PCM WAV. A multi-megabyte
 * song never lives in memory — the file is read in small chunks (readfile_at).
 *   - the mixer: one background thread that opens the kernel audio stream
 * lazily (only while something plays), decodes + gain-scales every active
 * source, sums them into one stream, and back-pressures on audio_write. The
 * kernel exposes exactly one exclusive, blocking PCM stream, so *all*
 * concurrent audio (e.g. a page with two <audio> elements) is mixed here in
 * user space.
 *
 * It is consumer-agnostic: the mixer never calls into a scripting engine. A
 * consumer that needs to react on a thread it controls polls the published
 * per-source state (position/duration/ended) instead — which is exactly how the
 * browser fires HTMLMediaElement events from its own worker thread.
 */
#ifndef OWOS_MEDIA_H
#define OWOS_MEDIA_H

/* The device's canonical PCM format; every decoder output and the mix are this.
 */
#define MEDIA_RATE 48000
#define MEDIA_CH 2

/* ---- decoder: a seekable local file -> 48 kHz/S16/stereo ------------------
 */
typedef struct media_decoder media_decoder;

/* Open `path` (an owofs file), routing by content: RIFF/WAVE -> PCM WAV, else a
 * decodable MPEG stream. Returns a decoder, or NULL if the file is missing or
 * not playable. The decoder copies `path`; the file stays on disk and is read
 * incrementally, so the caller may free its own copy. */
media_decoder *media_open(const char *path);
void media_close(media_decoder *d);

/* Decode up to `frames` interleaved 48 kHz/S16/stereo frames (2 shorts each)
 * into `out`. Returns frames produced; 0 at end of stream. Resampling state is
 * continuous across calls. */
int media_read(media_decoder *d, short *out, int frames);

/* Playback clock, in milliseconds. media_position_ms is how far decoding has
 * advanced; media_duration_ms is the track length (exact for WAV, an estimate
 * that sharpens as an MPEG stream plays, 0 while still unknown). */
unsigned media_position_ms(const media_decoder *d);
unsigned media_duration_ms(const media_decoder *d);

/* Seek the read cursor to `ms` (clamped to the track). WAV is sample-accurate;
 * MPEG seeks by byte offset (the decoder resyncs on the next frame). */
void media_seek_ms(media_decoder *d, unsigned ms);

/* ---- software mixer + one playback thread -------------------------------- */
typedef struct media_source media_source;

/* Optional end-of-stream notice, invoked once from the MIXER THREAD when a
 * non-looping source runs out (never on a loop wrap). It must be short and must
 * not free the source or block. Most consumers pass NULL and poll instead. */
typedef void (*media_end_cb)(void *user);

/* Start the mixer thread (idempotent; safe from any thread). Called implicitly
 * by media_source_new, exposed so a consumer can pre-warm it. */
void media_mixer_start(void);

/* Register a source wrapping `d` (the mixer takes ownership of `d` and frees it
 * with the source). Starts paused. Returns a handle, or NULL if the source
 * table is full. `on_end`/`user` may be NULL. */
media_source *media_source_new(media_decoder *d, media_end_cb on_end, void *user);

/* Release the source and its decoder. The handle must not be used afterward.
 * The teardown is deferred to the mixer thread, so it is safe to call while the
 * source is playing. */
void media_source_free(media_source *s);

void media_source_play(media_source *s);                   /* begin/resume playback         */
void media_source_pause(media_source *s);                  /* hold the read cursor          */
void media_source_set_volume(media_source *s, int vol256); /* 0..256, 256=unity */
void media_source_set_loop(media_source *s, int loop);     /* restart at EOS */
void media_source_seek_ms(media_source *s, unsigned ms);   /* clears `ended` */

int media_source_playing(const media_source *s); /* actively producing audio */
int media_source_ended(const media_source *s);   /* reached EOS, not looping  */
unsigned media_source_position_ms(const media_source *s);
unsigned media_source_duration_ms(const media_source *s);

/* ---- video decoder: a local H.264 file -> timed RGB frames ---------------- *
 * A thin front end over user/lib/h264 (+ user/lib/mp4 for MP4/MOV containers):
 * it holds the whole file in memory (the demuxer needs random access), decodes
 * pictures in display order, and hands each back as packed RGB with a
 * presentation timestamp so a player can pace them against a wall clock. Audio
 * in a video file is a separate track the caller handles on its own (an MP4's
 * AAC is not decoded here); this is the picture path only. Compiled into its
 * own object so only video consumers drag in the H.264 decoder. */
typedef struct media_video media_video;

/* Open a `.mp4`/`.mov`/`.m4v` (H.264 track) or a raw Annex-B `.264`/`.h264`
 * elementary stream. Returns a decoder or NULL (missing / not H.264 /
 * unsupported tool). */
media_video *media_video_open(const char *path);
void media_video_close(media_video *v);

int media_video_width(const media_video *v);
int media_video_height(const media_video *v);
unsigned media_video_duration_ms(const media_video *v); /* 0 if unknown */

/* Decode the next picture (display order) into `rgb`, which must hold
 * width*height*3 bytes (R,G,B). On a frame, sets *pts_ms to its presentation
 * time and returns 1; returns 0 at end of stream, -1 on a decode error. */
int media_video_next(media_video *v, unsigned char *rgb, unsigned *pts_ms);

#endif /* OWOS_MEDIA_H */
