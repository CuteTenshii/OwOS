/* mpeg_test.c — host-side test for user/lib/mpeg (MPEG-1 video decoder).
 *
 * Decodes an MPEG-1 elementary stream and compares each frame's YCbCr planes to
 * the ground truth ffmpeg produces from the same stream, asserting a high PSNR
 * (the IDCT is not bit-exact-specified, so a small tolerance is expected). Build
 * the test vectors with ffmpeg, then run:
 *
 *     ffmpeg -y -f lavfi -i testsrc=size=176x144:rate=25:duration=0.24 \
 *            -c:v mpeg1video -qscale:v 3 -g 3 -pix_fmt yuv420p /tmp/iframe.m1v
 *     ffmpeg -y -i /tmp/iframe.m1v -f rawvideo -pix_fmt yuv420p /tmp/iframe.yuv
 *     cc -std=c11 -Iscripts -Iuser/lib/mpeg \
 *        tests/mpeg_test.c user/lib/mpeg/decode.c -lm -o /tmp/mpeg_test
 *     /tmp/mpeg_test /tmp/iframe.m1v /tmp/iframe.yuv 176 144
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "mpeg.h"

static unsigned char *slurp(const char *path, long *len)
{
	FILE *f = fopen(path, "rb");
	unsigned char *b;
	if (!f) { perror(path); exit(2); }
	fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET);
	b = malloc(*len);
	if (fread(b, 1, *len, f) != (size_t)*len) { fprintf(stderr, "read %s\n", path); exit(2); }
	fclose(f);
	return b;
}

/* PSNR of `w`x`h` plane `a` (stride `as`) vs `b` (stride `bs`); 99.0 if identical. */
static double psnr(const unsigned char *a, int as, const unsigned char *b, int bs, int w, int h)
{
	double mse = 0; int x, y, maxd = 0;
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			int d = a[y * as + x] - b[y * bs + x];
			if (d < 0) d = -d;
			if (d > maxd) maxd = d;
			mse += (double)d * d;
		}
	mse /= (double)w * h;
	if (mse < 1e-9) return 99.0;
	return 10.0 * log10(255.0 * 255.0 / mse);
}

int main(int argc, char **argv)
{
	long slen, ylen;
	unsigned char *stream, *truth;
	int W, H, cw, ch, fsz, nframes, fi, fails = 0;
	mpeg_decoder *d;

	if (argc < 5) { fprintf(stderr, "usage: %s stream.m1v truth.yuv W H\n", argv[0]); return 2; }
	W = atoi(argv[3]); H = atoi(argv[4]);
	cw = (W + 1) / 2; ch = (H + 1) / 2;
	fsz = W * H + 2 * cw * ch;

	stream = slurp(argv[1], &slen);
	truth  = slurp(argv[2], &ylen);
	nframes = (int)(ylen / fsz);

	if (mpeg_decoder_open(stream, (unsigned)slen, &d) != MPEG_OK) {
		fprintf(stderr, "mpeg_decoder_open failed\n"); return 1;
	}
	if (mpeg_width(d) != W || mpeg_height(d) != H) {
		fprintf(stderr, "dims %dx%d != %dx%d\n", mpeg_width(d), mpeg_height(d), W, H); fails++;
	}
	printf("stream %ld bytes, %dx%d, %d ground-truth frames\n", slen, W, H, nframes);

	for (fi = 0; fi < nframes; fi++) {
		int ys, cbs, crs;
		const unsigned char *Y, *Cb, *Cr, *tY, *tCb, *tCr;
		double py, pcb, pcr;
		const char *types = "?IPBD";
		int t;
		if (mpeg_decode_frame(d) != MPEG_OK) { printf("frame %d: decode ended early\n", fi); fails++; break; }
		t = mpeg_frame_type(d);
		Y  = mpeg_plane_y(d, &ys);
		Cb = mpeg_plane_cb(d, &cbs);
		Cr = mpeg_plane_cr(d, &crs);
		tY  = truth + (long)fi * fsz;
		tCb = tY + W * H;
		tCr = tCb + cw * ch;
		py  = psnr(Y, ys, tY, W, W, H);
		pcb = psnr(Cb, cbs, tCb, cw, cw, ch);
		pcr = psnr(Cr, crs, tCr, cw, cw, ch);
		printf("frame %d [%c]  Y %.1f dB  Cb %.1f dB  Cr %.1f dB\n",
		       fi, types[t > 0 && t < 5 ? t : 0], py, pcb, pcr);
		if (py < 35.0 || pcb < 35.0 || pcr < 35.0) fails++;
	}

	mpeg_decoder_close(d);
	free(stream); free(truth);
	if (fails) { printf("%d failure(s)\n", fails); return 1; }
	printf("all mpeg decoder frames within tolerance\n");
	return 0;
}
