/* mp3.h — streaming MPEG audio decoder (Layer I/II/III, MPEG-1/2/2.5).
 *
 * A thin streaming wrapper over the vendored minimp3 frame decoder (CC0,
 * minimp3.h). It decodes one frame at a time, pulling source bytes from a file
 * through readfile_at, so a multi-megabyte song never lives in memory at once —
 * the same reason /bin/play is a user program. Gapless: the LAME encoder delay/padding is trimmed. Output is
 * interleaved 16-bit signed stereo at the file's native sample rate (a mono stream is duplicated to both channels);
 * /bin/play resamples that to the device's 48 kHz. A decode bug is a contained process crash, never a kernel hang.
 */
#ifndef OWOS_MP3_H
#define OWOS_MP3_H

#include "minimp3.h" /* mp3dec_t, MINIMP3_MAX_SAMPLES_PER_FRAME */

typedef struct mp3 {
  const char *fn;    /* source file (owofs path)              */
  unsigned size;     /* total file bytes                      */
  unsigned file_off; /* next file byte to read                */
  int src_eof;       /* no more file bytes to read            */

  mp3dec_t dec;           /* minimp3 decoder state                 */
  unsigned char in[8192]; /* sliding window of undecoded bytes     */
  int in_len;             /* valid bytes at in[0..in_len)          */

  short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME]; /* one decoded frame */
  int out_frames;                           /* frames decoded into pcm               */
  int out_pos;                              /* next frame to hand out                */
  int out_ch;                               /* channels held in pcm (1 or 2)         */

  unsigned rate;     /* native sample rate, Hz                */
  unsigned channels; /* source channels (1 or 2)              */

  unsigned first;      /* file offset of the first frame (a Xing/VBRI frame if present) */
  unsigned audio0;     /* file offset of the first audio frame  */
  unsigned data_end;   /* end of audio (before an ID3v1 tag)    */
  unsigned spf;        /* PCM frames per MPEG frame             */
  unsigned nframes;    /* audio frames in the stream (estimated unless exact) */
  unsigned start_skip; /* encoder delay + decoder delay, PCM frames */
  unsigned total;      /* trimmed length in PCM frames, 0 unknown */
  unsigned limit;      /* stop after this many PCM frames, 0 none */
  unsigned pos;        /* PCM frames handed out on the trimmed timeline */
  unsigned skip;       /* PCM frames still to drop before output */
  unsigned fidx;       /* index of the next frame to decode, from audio0 */
  int warm;            /* decoded frames still to drop after a seek */
  int exact;           /* nframes/total come from a Xing/VBRI header */
  int toc;             /* Xing seek table present               */
  unsigned char toc_tab[100];
  unsigned toc_bytes; /* byte span the Xing table is relative to, from `first` */
} mp3_t;

/* Open `fn` (file size `size`) and decode the first frame so rate/channels are
 * known. Returns 0 on success, -1 if `fn` is not a decodable MPEG stream. */
int mp3_open(mp3_t *m, const char *fn, unsigned size);

unsigned mp3_rate(const mp3_t *m);
unsigned mp3_channels(const mp3_t *m);

/* Track length in PCM frames at mp3_rate(), 0 if unknown. Exact when the stream
 * carries a Xing/VBRI header (trimmed by the LAME encoder delay/padding),
 * otherwise a constant-bitrate estimate. */
unsigned mp3_total_frames(const mp3_t *m);
int mp3_total_exact(const mp3_t *m);

/* Seek to PCM frame `target`. Returns the frame reached: exact
 * with a seek table or constant bitrate, at worst a frame or two late for VBR
 * without one. */
unsigned mp3_seek(mp3_t *m, unsigned target);

/* Pull up to `maxframes` stereo frames (2 shorts each) into `out`. Returns the
 * number of frames produced, or 0 at end of stream. */
int mp3_read(mp3_t *m, short *out, int maxframes);

void mp3_close(mp3_t *m);

#endif /* OWOS_MP3_H */
