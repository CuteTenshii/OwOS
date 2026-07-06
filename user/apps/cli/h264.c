/* h264.c — a small H.264 / AVC command-line tool.
 *
 *   h264 info <file.264|.h264|.mp4|.mov>   decode the video, print dimensions and
 *                                          a per-picture I/P frame summary
 *
 * A raw Annex-B elementary stream is decoded directly; an MP4/MOV file is taken
 * apart by the ISO-BMFF demuxer (user/lib/mp4) first — its H.264 track's avcC
 * seeds the decoder and each sample is fed as an access unit. The decoder itself
 * lives in user/lib/h264; this is only the front end.
 */
#include "owos.h"
#include "h264.h"
#include "mp4.h"

static unsigned char *read_whole(const char *path, unsigned *out_len)
{
	struct statinfo st;
	unsigned char *buf;
	unsigned off = 0;
	if (stat(path, &st) != 0 || st.size == 0) return 0;
	buf = malloc(st.size);
	if (!buf) return 0;
	while (off < st.size) {
		int n = readfile_at(path, off, buf + off, st.size - off);
		if (n <= 0) break;
		off += (unsigned)n;
	}
	if (off != st.size) { free(buf); return 0; }
	*out_len = st.size;
	return buf;
}

static int ends_with(const char *s, const char *suf)
{
	int ls = strlen(s), lf = strlen(suf);
	return ls >= lf && streq_ci(s + ls - lf, suf);
}

static void tally(int t, unsigned *ni, unsigned *np, unsigned *nf)
{
	(*nf)++;
	if (t == H264_SLICE_I) (*ni)++;
	else if (t == H264_SLICE_P) (*np)++;
}

static int info_annexb(const unsigned char *buf, unsigned len)
{
	h264_decoder *d;
	unsigned ni = 0, np = 0, nf = 0;
	int rc = h264_decoder_open(buf, len, &d);
	if (rc != H264_OK) { printf("h264: not a decodable H.264 stream (%d)\n", rc); return 1; }
	printf("H.264 video %dx%d\n", h264_width(d), h264_height(d));
	while (h264_decode_frame(d) == H264_OK) tally(h264_frame_type(d), &ni, &np, &nf);
	printf("  %u frames  (I:%u P:%u)\n", nf, ni, np);
	h264_decoder_close(d);
	return 0;
}

static int info_mp4(const unsigned char *buf, unsigned len)
{
	mp4 *m;
	const mp4_track *vt;
	h264_decoder *d;
	unsigned ni = 0, np = 0, nf = 0, i;
	int rc = mp4_open(buf, len, &m);
	if (rc != MP4_OK) { printf("h264: cannot parse MP4 (%d)\n", rc); return 1; }
	vt = mp4_video_track(m);
	if (!vt || vt->codec != MP4_CODEC_H264) { printf("h264: no H.264 video track\n"); mp4_close(m); return 1; }
	if (h264_decoder_open_avcc(vt->extradata, vt->extradata_len, &d) != H264_OK) {
		printf("h264: unsupported H.264 configuration\n"); mp4_close(m); return 1;
	}
	printf("H.264 video %dx%d, %u samples\n", h264_width(d), h264_height(d), vt->n_samples);
	for (i = 0; i < vt->n_samples; i++) {
		unsigned sz;
		const unsigned char *s = mp4_sample_data(m, vt, i, &sz);
		if (!s) break;
		if (h264_decode_au(d, s, sz, vt->nal_length_size) == H264_OK)
			tally(h264_frame_type(d), &ni, &np, &nf);
	}
	while (h264_flush(d) == H264_OK) tally(h264_frame_type(d), &ni, &np, &nf);
	printf("  %u frames  (I:%u P:%u)\n", nf, ni, np);
	h264_decoder_close(d);
	mp4_close(m);
	return 0;
}

static int cmd_info(const char *path)
{
	unsigned len = 0;
	unsigned char *buf = read_whole(path, &len);
	int rc;
	if (!buf) { printf("h264: cannot read %s\n", path); return 1; }
	if (ends_with(path, ".mp4") || ends_with(path, ".mov") || ends_with(path, ".m4v"))
		rc = info_mp4(buf, len);
	else
		rc = info_annexb(buf, len);
	free(buf);
	return rc;
}

int main(int argc, char **argv)
{
	if (argc >= 3 && streq(argv[1], "info")) return cmd_info(argv[2]);
	printf("usage: h264 info <file.264|.mp4>\n");
	return 1;
}
