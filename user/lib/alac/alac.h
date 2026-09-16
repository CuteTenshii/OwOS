/* alac.h — Apple Lossless Audio Codec (ALAC), encoder + decoder.
 *
 * A faithful, freestanding port of Apple's open-source ALAC reference codec
 * (Apache-2.0): the generalized middle-side matrix, the adaptive FIR "dynamic
 * predictor", and the adaptive Golomb-Rice entropy coder. It is bit-exact with
 * the reference — packets this encoder emits decode on any conformant ALAC
 * decoder, and this decoder reads Apple-produced ALAC — so it interoperates.
 *
 * The codec is pure integer computation (no float, so it is safe under the
 * user-space -mno-sse build), operates one packet at a time, and is entirely
 * consumer-agnostic: it knows nothing about files or containers. A packet holds
 * up to `frame_length` interleaved sample frames. The out-of-band parameters
 * every ALAC stream carries (frame length, bit depth, channels, sample rate,
 * Rice tuning) travel in the 24-byte "magic cookie" (ALACSpecificConfig) — the
 * caller pulls it from its container (CAF/M4A) and hands it here.
 *
 * PCM is interleaved, native (little-endian) integer, one channel after another
 * per frame: 16-bit -> int16 samples, 20/24-bit -> 3 bytes/sample (LSB first,
 * 20-bit left-justified in the 24), 32-bit -> int32 samples.
 */
#ifndef OWOS_ALAC_H
#define OWOS_ALAC_H

#include <stdint.h>

enum {
  ALAC_OK = 0,
  ALAC_EPARAM = -1,  /* bad argument / unsupported config          */
  ALAC_ENOMEM = -2,  /* allocation failed                          */
  ALAC_EFORMAT = -3, /* corrupt packet or cookie                   */
};

/* The out-of-band stream description (the "magic cookie", ALACSpecificConfig).
 */
typedef struct {
  uint32_t frame_length;    /* frames per packet (Apple default 4096)      */
  uint8_t bit_depth;        /* 16 / 20 / 24 / 32                            */
  uint8_t num_channels;     /* 1 (mono) or 2 (stereo)                       */
  uint32_t sample_rate;     /* Hz                                           */
  uint8_t pb, mb, kb;       /* Rice tuning (defaults 40, 10, 14)            */
  uint16_t max_run;         /* default 255                                  */
  uint32_t max_frame_bytes; /* largest packet seen; 0 if unknown            */
  uint32_t avg_bit_rate;    /* informational; 0 if unknown                  */
} alac_config;

#define ALAC_COOKIE_SIZE                                                                                               \
  24 /* bytes in an ALACSpecificConfig (<=2 channels)                                                                  \
      */

/* Fill `c` with the Apple defaults for the given format (frame_length 4096). */
void alac_config_default(alac_config *c, uint32_t sample_rate, uint8_t bit_depth, uint8_t num_channels);

/* Parse a 24-byte magic cookie (tolerates a leading 'frma'/'alac' atom
 * wrapper). Returns ALAC_OK or a negative code. */
int alac_parse_cookie(const uint8_t *cookie, uint32_t len, alac_config *out);

/* Serialize `c` into a 24-byte cookie. Returns ALAC_OK. */
int alac_build_cookie(const alac_config *c, uint8_t out[ALAC_COOKIE_SIZE]);

/* Upper bound on one encoded packet, and PCM byte size of one interleaved
 * frame. */
uint32_t alac_max_packet_size(const alac_config *c);
uint32_t alac_bytes_per_frame(const alac_config *c);

/* ---- decoder ------------------------------------------------------------- */
typedef struct alac_decoder alac_decoder;

alac_decoder *alac_decoder_open(const alac_config *c);
void alac_decoder_close(alac_decoder *d);

/* Decode one packet into `pcm_out` (must hold frame_length * bytes-per-frame).
 * `*out_frames` receives the frame count (a partial last packet yields fewer
 * than frame_length). Returns ALAC_OK or a negative code. */
int alac_decode(alac_decoder *d, const uint8_t *packet, uint32_t packet_bytes, uint8_t *pcm_out, uint32_t *out_frames);

/* ---- encoder ------------------------------------------------------------- */
typedef struct alac_encoder alac_encoder;

alac_encoder *alac_encoder_open(const alac_config *c);
void alac_encoder_close(alac_encoder *e);

/* Encode `num_frames` (<= frame_length) interleaved PCM frames into
 * `packet_out` (must hold alac_max_packet_size bytes). Returns the packet
 * length in bytes, or a negative code. */
int alac_encode(alac_encoder *e, const uint8_t *pcm, uint32_t num_frames, uint8_t *packet_out);

#endif /* OWOS_ALAC_H */
