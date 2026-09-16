/* ogg.h — Ogg bitstream container + Ogg Vorbis I audio codec.
 *
 * Two independent layers, mirroring the split in the other codecs of this tree
 * (the container knows nothing about the payload, the codec knows nothing about
 * files):
 *
 *   - The Ogg container (RFC 3533). A push demuxer reassembles the logical
 *     bitstreams' packets from the physical page stream, and a mux packs a
 *     stream of packets back into pages. Both are payload-agnostic; the Ogg CRC
 *     is implemented here.
 *   - The Vorbis I codec (Xiph "Vorbis I specification"). A from-scratch
 * decoder (codebooks, floor 1, residue 0/1/2, channel coupling, the windowed
 * IMDCT filterbank) turns audio packets into interleaved int16, and a
 * from-scratch VBR encoder turns interleaved int16 back into a spec-conformant
 * stream — it emits its own codebooks in the setup header, so it needs none of
 *     libvorbis's built-in tables. Decoded output interoperates with libvorbis;
 *     encoded output decodes on libvorbis (verified via ffmpeg,
 * scripts/ogg_test).
 *
 * All DSP is float/double over x87 (safe under the user-space -mno-sse build;
 * no SSE float->int conversion is emitted with -mfpmath=387). A malformed or
 * hostile stream can only fault this process, never the kernel.
 *
 * The convenience one-shot helpers at the bottom (whole file in memory <-> PCM)
 * are what the `ogg` CLI and simple callers use; the streaming decoder is what
 * the media engine drives so a long song never sits in memory whole.
 */
#ifndef OWOS_OGG_H
#define OWOS_OGG_H

#include <stdint.h>

enum {
  OGG_OK = 0,
  OGG_EPARAM = -1,  /* bad argument                                        */
  OGG_ENOMEM = -2,  /* allocation failed                                   */
  OGG_EFORMAT = -3, /* corrupt / truncated bitstream                       */
  OGG_EUNSUP = -4,  /* structurally valid but an unsupported tool/config   */
  OGG_NEED = -5,    /* demux: need more input bytes before a packet/page   */
};

/* ============================ Ogg container ============================== */

/* Page header type flags (the 6th header byte). */
#define OGG_F_CONTINUED 0x01
#define OGG_F_BOS 0x02
#define OGG_F_EOS 0x04

/* ---- push demuxer: raw Ogg bytes -> reassembled packets ------------------ *
 * Feed arbitrary-sized byte runs with ogg_demux_feed; pull whole packets with
 * ogg_demux_packet. Reassembles packets that span segments and pages, tracks
 * several concurrent logical streams (by serialno), verifies each page's CRC,
 * and resynchronizes on the "OggS" capture pattern after corruption. */
typedef struct ogg_demux ogg_demux;

ogg_demux *ogg_demux_new(void);
void ogg_demux_free(ogg_demux *d);

/* Append input. Returns OGG_OK or OGG_ENOMEM (the internal buffer grows). */
int ogg_demux_feed(ogg_demux *d, const void *data, unsigned len);

/* Pull the next complete packet. On a packet returns OGG_OK and sets *pkt/*len
 * (pointer valid until the next feed/packet call), plus the owning stream's
 * *serial, the page's *granulepos (-1 mid-page), and the *bos / *eos flags (the
 * packet is the first/last of its logical stream). Returns OGG_NEED when the
 * fed input holds no further complete packet. Any out param may be NULL. */
int ogg_demux_packet(ogg_demux *d, const unsigned char **pkt, unsigned *len, int64_t *granulepos, uint32_t *serial,
                     int *bos, int *eos);

/* ---- mux: a packet stream (one logical bitstream) -> Ogg pages ----------- *
 * Queue packets with ogg_mux_packet; drain emitted page bytes with
 * ogg_mux_pull. A page is emitted once it fills (255 segments) or when forced
 * by a flush or an EOS/granulepos boundary, matching how libogg pages a Vorbis
 * stream. */
typedef struct ogg_mux ogg_mux;

ogg_mux *ogg_mux_new(uint32_t serial);
void ogg_mux_free(ogg_mux *m);

/* Queue one packet. `granulepos` stamps the page this packet completes (use -1
 * to leave the page's granulepos as "continued"); `bos`/`eos` set the page
 * flags. Returns OGG_OK or OGG_ENOMEM. */
int ogg_mux_packet(ogg_mux *m, const void *pkt, unsigned len, int64_t granulepos, int bos, int eos);

/* Force any buffered packets out into a page even if not full (call after the
 * three Vorbis headers, and at end of stream). */
int ogg_mux_flush(ogg_mux *m);

/* Pull the next ready page's bytes (valid until the next mux call). Returns
 * OGG_OK with *page/*len, or OGG_NEED when nothing is ready. */
int ogg_mux_pull(ogg_mux *m, const unsigned char **page, unsigned *len);

/* Raw CRC over an assembled page (checksum field must be zeroed), exposed for
 * tests. Polynomial 0x04c11db7, MSB-first, init 0, no final xor. */
uint32_t ogg_crc(const unsigned char *page, unsigned len);

/* ============================== Vorbis I ================================= */

typedef struct {
  uint32_t sample_rate; /* Hz                                              */
  uint8_t channels;     /* 1..255 (this codec assembles up to 8)           */
  uint32_t bitrate_nom; /* nominal bitrate hint, 0 if unset                */
  int blocksize_short;
  int blocksize_long;
} vorbis_info;

#define VORBIS_MAX_CHANNELS 8

/* ---- decoder ------------------------------------------------------------- *
 * Built from the id + setup header packets (the comment header is not needed to
 * decode). Then feed audio packets in order; each yields the finalized frames
 * that the overlap-add produced (the first audio packet yields none). */
typedef struct vorbis_decoder vorbis_decoder;

vorbis_decoder *vorbis_decoder_open(const unsigned char *id, unsigned id_len, const unsigned char *setup,
                                    unsigned setup_len, vorbis_info *info);
void vorbis_decoder_close(vorbis_decoder *v);

/* Upper bound on frames one audio packet can finalize (== blocksize_long/2 + a
 * short overlap). `pcm` passed to vorbis_decode must hold this * channels
 * shorts. */
int vorbis_max_frames(const vorbis_decoder *v);

/* Decode one audio packet into interleaved native-endian int16 `pcm`. Returns
 * the finalized frame count (samples per channel; may be 0), or a negative
 * code. A header packet (high bit of byte 0 set) is skipped and returns 0. */
int vorbis_decode(vorbis_decoder *v, const unsigned char *pkt, unsigned len, short *pcm);

/* ---- encoder ------------------------------------------------------------- *
 * A VBR encoder. Open with the target format and a quality in [-0.1, 1.0]
 * (libvorbis's `-q` scale; higher is better/larger). Pull the three header
 * packets first, then push PCM and pull audio packets, then finish. */
typedef struct vorbis_encoder vorbis_encoder;

vorbis_encoder *vorbis_encoder_open(uint32_t sample_rate, int channels, float quality);
void vorbis_encoder_close(vorbis_encoder *e);

/* Copy the three Vorbis header packets (id, comment, setup) out. Each call
 * fills *pkt/*len with the next header (valid until the next encoder call) and
 * returns OGG_OK; after the third it returns OGG_NEED. Do this before any PCM.
 */
int vorbis_encoder_header(vorbis_encoder *e, const unsigned char **pkt, unsigned *len);

/* Submit `frames` interleaved int16 frames (channels shorts each). Buffered
 * into analysis blocks. Returns OGG_OK or OGG_ENOMEM. */
int vorbis_encoder_write(vorbis_encoder *e, const short *pcm, int frames);

/* Signal end of input, so the tail block is emitted. */
int vorbis_encoder_finish(vorbis_encoder *e);

/* Pull the next ready audio packet with its absolute granulepos (total PCM
 * samples decodable after it). Returns OGG_OK with *pkt / *len / *granulepos,
 * or OGG_NEED when none is ready (write more, or finish). */
int vorbis_encoder_packet(vorbis_encoder *e, const unsigned char **pkt, unsigned *len, int64_t *granulepos);

/* ======================= convenience one-shots ========================== */

/* Decode a whole in-memory .ogg (Vorbis) to interleaved int16. Allocates *out
 * (malloc; caller frees) and fills *frames / *channels / *rate. Returns OGG_OK
 * or a negative code. */
int ogg_vorbis_decode_mem(const void *ogg, unsigned len, short **out, int *frames, int *channels, int *rate);

/* Encode interleaved int16 PCM to a whole in-memory .ogg. Allocates *out
 * (malloc; caller frees) and sets *out_len. Returns OGG_OK or a negative code.
 */
int ogg_vorbis_encode_mem(const short *pcm, int frames, int channels, int rate, float quality, void **out,
                          unsigned *out_len);

#endif /* OWOS_OGG_H */
