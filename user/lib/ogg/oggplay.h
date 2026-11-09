/* oggplay.h, streaming Ogg Vorbis file reader for the media engine.
 *
 * Pulls pages from a file through readfile_at and decodes one packet at a time
 * (a long song never sits in memory). Output is interleaved 16-bit stereo at
 * the file's native rate; mono is duplicated. Mono and stereo only.
 */
#ifndef OWOS_OGGPLAY_H
#define OWOS_OGGPLAY_H

typedef struct oggplay oggplay;

/* Open `path` (file size `size`). Returns NULL if it is not a 1 or 2 channel
 * Ogg Vorbis stream. `path` must outlive the handle. */
oggplay *oggplay_open(const char *path, unsigned size);
void oggplay_close(oggplay *o);

unsigned oggplay_rate(const oggplay *o);

/* Track length in PCM frames from the last page's granule position, 0 if unknown. */
unsigned oggplay_total_frames(const oggplay *o);

/* Pull up to `maxframes` stereo frames (2 shorts each). Returns frames produced,
 * 0 at end of stream. */
int oggplay_read(oggplay *o, short *out, int maxframes);

/* Seek to PCM frame `target`. The audio is bisected to a page boundary and
 * decoded forward, so the content can sit a few tens of milliseconds off the
 * target (the decoder loses one block of overlap). Returns the target. */
unsigned oggplay_seek(oggplay *o, unsigned target);

#endif /* OWOS_OGGPLAY_H */
