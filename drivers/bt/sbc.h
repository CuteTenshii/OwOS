/* sbc.h: A2DP SBC encoder (8 subbands, integer arithmetic only). */
#ifndef OWOS_SBC_H
#define OWOS_SBC_H

#include <stdint.h>

#define SBC_SUBBANDS 8
#define SBC_SYNCWORD 0x9C

enum { SBC_MODE_MONO, SBC_MODE_DUAL, SBC_MODE_STEREO, SBC_MODE_JOINT };
enum { SBC_ALLOC_LOUDNESS, SBC_ALLOC_SNR };
enum { SBC_FREQ_16000, SBC_FREQ_32000, SBC_FREQ_44100, SBC_FREQ_48000 };

struct sbc_enc {
  uint8_t freq, blocks, mode, alloc, bitpool;
  int16_t x[2][80];
};

/* blocks is 4, 8, 12 or 16. */
void sbc_init(struct sbc_enc *e, int freq, int blocks, int mode, int alloc, int bitpool);
int sbc_channels(const struct sbc_enc *e);
/* Bytes per encoded frame. */
int sbc_frame_len(const struct sbc_enc *e);
/* PCM samples per channel consumed by one frame. */
int sbc_frame_samples(const struct sbc_enc *e);
/* Encode sbc_frame_samples() interleaved s16 frames into out; returns the frame length. */
int sbc_encode(struct sbc_enc *e, const int16_t *pcm, uint8_t *out);

#endif
