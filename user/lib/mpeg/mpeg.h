/* mpeg.h — a from-scratch MPEG-1 video (ISO/IEC 11172-2) codec: decoder +
 * encoder.
 *
 * MPEG is the *codec* (how pixels are compressed), distinct from a container
 * like MP4 (how a compressed stream is packaged in a file — see user/lib/mp4).
 * This library is the codec: it turns an MPEG-1 video elementary stream into
 * YCbCr frames (decoder) and YCbCr frames back into a conformant elementary
 * stream (encoder). Both directions round-trip through ffmpeg's mpeg1video.
 *
 * Like the WebP and PNG codecs it lives in user space: the bitstream is
 * untrusted, so a malformed stream can only fault this process, never the
 * kernel. The whole elementary stream is handled from an in-memory buffer.
 *
 * Scope: the full MPEG-1 video toolset a plain stream uses —
 * sequence/GOP/picture/ slice layers, I/P/B pictures, intra and
 * motion-compensated (half-pel) prediction, the run/level DCT VLCs, dequant +
 * IDCT, and 4:2:0 YCbCr. D-pictures and the scalable extensions are not
 * decoded. The encoder emits I- and P-pictures (full-search motion estimation);
 * B-picture encoding is decode-only for now.
 */
#ifndef OWOS_MPEG_H
#define OWOS_MPEG_H

enum {
  MPEG_OK = 0,
  MPEG_EBADSIG = -1, /* no sequence header found                          */
  MPEG_EFORMAT = -2, /* corrupt bitstream                                 */
  MPEG_ENOMEM = -3,  /* allocation failed                                 */
  MPEG_EUNSUP = -4,  /* valid stream using a tool we don't decode         */
  MPEG_EEOF = -5,    /* no more pictures in the stream                    */
};

/* Picture coding type of the frame just decoded (ISO 11172-2 Table 2-D.4). */
enum { MPEG_PIC_I = 1, MPEG_PIC_P = 2, MPEG_PIC_B = 3, MPEG_PIC_D = 4 };

/* ---- decoder ------------------------------------------------------------- */
typedef struct mpeg_decoder mpeg_decoder;

/* Open a decoder over the whole in-memory elementary stream `buf`/`len` (which
 * must outlive the decoder). Reads the first sequence header so width/height
 * are known. Returns MPEG_OK or an MPEG_E* code (*out left NULL). */
int mpeg_decoder_open(const unsigned char *buf, unsigned len, mpeg_decoder **out);
void mpeg_decoder_close(mpeg_decoder *d);

int mpeg_width(const mpeg_decoder *d);
int mpeg_height(const mpeg_decoder *d);

/* Decode the next picture into the decoder's frame store. Returns MPEG_OK (a
 * frame is ready), MPEG_EEOF at end of stream, or an MPEG_E* error. Frames come
 * out in decode order (I/P before the B-frames that reference them). */
int mpeg_decode_frame(mpeg_decoder *d);

/* The coding type (MPEG_PIC_*) of the most recently decoded frame. */
int mpeg_frame_type(const mpeg_decoder *d);

/* Planes of the most recently decoded frame (YCbCr 4:2:0). The chroma planes
 * are (width+1)/2 by (height+1)/2. *stride returns the row stride in bytes. */
const unsigned char *mpeg_plane_y(const mpeg_decoder *d, int *stride);
const unsigned char *mpeg_plane_cb(const mpeg_decoder *d, int *stride);
const unsigned char *mpeg_plane_cr(const mpeg_decoder *d, int *stride);

/* Convert the current frame to packed RGB (3 bytes/pixel, R,G,B) into `rgb`,
 * which must hold width*height*3 bytes. Returns MPEG_OK. */
int mpeg_frame_rgb(const mpeg_decoder *d, unsigned char *rgb);

/* ---- encoder ------------------------------------------------------------- */
typedef struct mpeg_encoder mpeg_encoder;

/* Open an encoder for `w`x`h` frames at `fps_num`/`fps_den` fps, targeting a
 * fixed quantiser `qscale` (1..31, lower = higher quality). `gop` sets the
 * interval between I-pictures (1 = all-intra). Returns MPEG_OK or an MPEG_E*.
 */
int mpeg_encoder_open(int w, int h, int fps_num, int fps_den, int qscale, int gop, mpeg_encoder **out);
void mpeg_encoder_close(mpeg_encoder *e);

/* Encode one frame (YCbCr 4:2:0 planes, luma `w`x`h`, chroma half each) or
 * packed RGB (width*height*3, R,G,B), appending coded bytes to the encoder's
 * buffer. The first frame is coded intra; subsequent ones are P unless a GOP
 * boundary forces an I. Returns MPEG_OK or an MPEG_E*. */
int mpeg_encode_frame_yuv(mpeg_encoder *e, const unsigned char *y, const unsigned char *cb, const unsigned char *cr);
int mpeg_encode_frame_rgb(mpeg_encoder *e, const unsigned char *rgb);

/* Write the sequence-end code and hand back the finished stream in a freshly
 * malloc'd buffer (*out, caller frees; *outlen bytes). Returns MPEG_OK. */
int mpeg_encoder_finish(mpeg_encoder *e, unsigned char **out, unsigned *outlen);

#endif /* OWOS_MPEG_H */
