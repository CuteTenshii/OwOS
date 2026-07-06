/* aac.h — Advanced Audio Coding (AAC-LC) decoder.
 *
 * A from-scratch decoder for MPEG-2/4 AAC Low Complexity — the audio carried by
 * almost every .m4a / .mp4 and by ADTS .aac streams. It implements the full LC
 * tool set: the noiseless (Huffman) layer, inverse quantization and rescaling,
 * M/S and intensity stereo, perceptual noise substitution (PNS), temporal noise
 * shaping (TNS), the pulse tool, and the long/short IMDCT filterbank with
 * overlap-add. SBR (HE-AAC) and PS are out of scope, as are the Main/SSR/LTP
 * object types — an SBR extension payload is skipped, so an HE-AAC stream still
 * decodes at its AAC-LC base rate.
 *
 * Like the ALAC and H.264 codecs in this tree it is pure computation, knows
 * nothing about files, and decodes one access unit (one raw_data_block, 1024 or
 * 960 samples per channel) at a time. Two ways in: an AudioSpecificConfig taken
 * from an MP4 esds (aac_open), or a self-describing ADTS bitstream whose first
 * frame header carries the config (aac_open_adts + aac_decode_adts). Output is
 * interleaved native-endian int16, one sample per channel per frame.
 *
 * The decoder allocates working buffers up front and does no float->int SSE
 * conversion, so it is safe under the user-space -mno-sse build. A malformed or
 * hostile packet can only fault this process, never the kernel.
 */
#ifndef OWOS_AAC_H
#define OWOS_AAC_H

#include <stdint.h>

enum {
	AAC_OK      =  0,
	AAC_EPARAM  = -1,   /* bad argument                                       */
	AAC_ENOMEM  = -2,   /* allocation failed                                  */
	AAC_EFORMAT = -3,   /* corrupt / truncated bitstream                      */
	AAC_EUNSUP  = -4,   /* structurally valid but an unsupported tool/profile */
};

#define AAC_MAX_CHANNELS 8   /* SCE/CPE/LFE elements we will assemble          */

/* Stream description. For an ASC-configured decoder this is known at open; for
 * an ADTS decoder sample_rate/channels are known after the first decoded frame
 * (0 before). frame_samples is 1024 (or 960 for the rare 960-length streams). */
typedef struct {
	uint32_t sample_rate;    /* Hz (the LC base rate; SBR would double it)     */
	uint8_t  channels;       /* decoded channel count                          */
	uint16_t frame_samples;  /* samples per channel per decoded frame          */
	uint8_t  object_type;    /* AudioObjectType (2 = LC)                       */
	uint8_t  sbr_present;    /* 1 if an SBR extension was seen (base rate only) */
} aac_info;

typedef struct aac_decoder aac_decoder;

/* Open a decoder from an AudioSpecificConfig (the MP4 esds "extradata"), e.g.
 * the extradata of an mp4_track whose codec is MP4_CODEC_AAC. Returns a decoder
 * or NULL on a bad/unsupported config. `info` (may be NULL) is filled on success. */
aac_decoder *aac_open(const uint8_t *asc, uint32_t asc_len, aac_info *info);

/* Open a decoder for a raw ADTS elementary stream. The config is learned from
 * the first frame header, so aac_get_info is only meaningful after the first
 * successful aac_decode_adts. Never returns NULL for a valid allocation. */
aac_decoder *aac_open_adts(void);

void aac_close(aac_decoder *d);

/* Copy the current stream description into *info. */
void aac_get_info(const aac_decoder *d, aac_info *info);

/* Largest interleaved PCM a single decoded frame can produce, in int16 samples
 * (frame_samples * channels). Valid once the config is known. */
uint32_t aac_max_pcm_samples(const aac_decoder *d);

/* Decode one raw access unit (a bare raw_data_block, no ADTS header) into
 * interleaved int16 `pcm`, which must hold aac_max_pcm_samples() shorts. On
 * AAC_OK, *out_samples gets the per-channel sample count and *out_channels the
 * channel count. Returns a negative code on error. */
int aac_decode(aac_decoder *d, const uint8_t *data, uint32_t len,
               int16_t *pcm, uint32_t *out_samples, uint32_t *out_channels);

/* Decode one ADTS frame beginning at data[0]. Locates the 0xFFF sync, validates
 * the header, decodes the payload, and reports the whole frame's byte length in
 * *consumed so the caller can advance. Same PCM contract as aac_decode. Returns
 * AAC_EFORMAT if no full frame is present in [data,len). */
int aac_decode_adts(aac_decoder *d, const uint8_t *data, uint32_t len,
                    uint32_t *consumed, int16_t *pcm,
                    uint32_t *out_samples, uint32_t *out_channels);

#endif /* OWOS_AAC_H */
