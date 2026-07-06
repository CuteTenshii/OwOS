/* h264.h — a from-scratch H.264 / AVC (ISO/IEC 14496-10) video decoder.
 *
 * H.264 is the *codec* (how pixels are compressed); a container like MP4 (see
 * user/lib/mp4) only packages the coded stream. This library is the decoder: it
 * turns an H.264 video elementary stream — or the length-prefixed access units an
 * MP4 track carries — into YCbCr 4:2:0 frames. It decodes nothing about the
 * container; the caller supplies coded bytes.
 *
 * Like the MPEG-1, WebP and PNG codecs it lives in user space: the bitstream is
 * untrusted, so a malformed or hostile stream can only fault this process, never
 * the kernel. Every field read is bounds-checked against the RBSP length.
 *
 * Scope: Constrained Baseline / Baseline profile with CAVLC entropy coding — the
 * profile web video and most captures use once High-profile tools are excluded.
 * That is: I and P slices; Intra 4x4 / Intra 16x16 / chroma intra prediction;
 * P-macroblock inter prediction with quarter-pel luma (6-tap) + 1/8-pel chroma
 * interpolation, 16x16/16x8/8x16/8x8 partitions and 8x8 sub-partitions; the 4x4
 * integer transform with luma/chroma DC Hadamard; the in-loop deblocking filter;
 * and multi-frame reference management with POC-based output reordering. Not
 * decoded (rejected with H264_EUNSUP): CABAC, B slices, the 8x8 transform and
 * scaling lists (High profile), interlaced/field coding, and multiple slice
 * groups (FMO/ASO). 4:2:0 chroma only.
 *
 * Two front ends share one core:
 *   - h264_decoder_open() over a whole in-memory Annex-B elementary stream (00 00
 *     01 start codes, SPS/PPS in band) — the plain ".264"/".h264" file case.
 *   - h264_decoder_open_avcc() seeded from an MP4 avcC record (out-of-band SPS/PPS)
 *     fed length-prefixed access units via h264_decode_au() — the MP4 track case.
 */
#ifndef OWOS_H264_H
#define OWOS_H264_H

enum {
	H264_OK       =  0,
	H264_EBADSIG  = -1,   /* no SPS/PPS found, or not an H.264 stream            */
	H264_EFORMAT  = -2,   /* corrupt bitstream                                   */
	H264_ENOMEM   = -3,   /* allocation failed                                   */
	H264_EUNSUP   = -4,   /* valid stream using a tool we don't decode           */
	H264_EEOF     = -5,   /* no more pictures in the stream                      */
};

/* Slice type of the frame just decoded (H.264 Table 7-6, mod 5). */
enum { H264_SLICE_P = 0, H264_SLICE_B = 1, H264_SLICE_I = 2, H264_SLICE_SP = 3, H264_SLICE_SI = 4 };

typedef struct h264_decoder h264_decoder;

/* Open a decoder over the whole in-memory Annex-B elementary stream `buf`/`len`
 * (which must outlive the decoder). Reads parameter sets up to the first coded
 * picture so width/height are known. Returns H264_OK or an H264_E* code (*out
 * left NULL). */
int  h264_decoder_open(const unsigned char *buf, unsigned len, h264_decoder **out);

/* Open a decoder seeded from an MP4 avcC configuration record (the extradata of
 * an H.264 track — see mp4_track.extradata). Parses the embedded SPS/PPS so
 * width/height are known; access units are then fed with h264_decode_au(). The
 * avcC's own NAL length size is used. Returns H264_OK or an H264_E*. */
int  h264_decoder_open_avcc(const unsigned char *avcc, unsigned len, h264_decoder **out);

void h264_decoder_close(h264_decoder *d);

int  h264_width(const h264_decoder *d);   /* cropped display width  */
int  h264_height(const h264_decoder *d);  /* cropped display height */

/* Decode the next picture, in display order, into the decoder's frame store.
 * Returns H264_OK (a frame is ready), H264_EEOF at end of stream, or an H264_E*
 * error. Only for the Annex-B front end (h264_decoder_open). */
int  h264_decode_frame(h264_decoder *d);

/* Feed one length-prefixed access unit (an MP4 sample) to the decoder, for the
 * avcC front end. `nal_length_size` is mp4_track.nal_length_size (1..4). Returns
 * H264_OK if a displayable picture became ready (fetch it with the accessors),
 * H264_EAGAIN if the picture is buffered for reordering and none is ready yet, or
 * an H264_E* error. Call h264_flush() at end of stream to drain buffered frames. */
enum { H264_EAGAIN = 1 };
int  h264_decode_au(h264_decoder *d, const unsigned char *au, unsigned len,
                    int nal_length_size);
/* Pull one more buffered picture in display order after the last access unit.
 * Returns H264_OK (a frame is ready) or H264_EEOF (drained). */
int  h264_flush(h264_decoder *d);

/* Slice type (H264_SLICE_*) of the most recently produced frame. */
int  h264_frame_type(const h264_decoder *d);

/* Planes of the most recently produced frame (YCbCr 4:2:0). Chroma planes are
 * (width+1)/2 by (height+1)/2. *stride returns the row stride in bytes. The
 * pointers address the cropped top-left origin already. */
const unsigned char *h264_plane_y (const h264_decoder *d, int *stride);
const unsigned char *h264_plane_cb(const h264_decoder *d, int *stride);
const unsigned char *h264_plane_cr(const h264_decoder *d, int *stride);

/* Convert the current frame to packed RGB (3 bytes/pixel, R,G,B) into `rgb`,
 * which must hold width*height*3 bytes. Returns H264_OK. */
int  h264_frame_rgb(const h264_decoder *d, unsigned char *rgb);

#endif /* OWOS_H264_H */
