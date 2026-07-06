/* mpeg_enc_test.c — host round-trip test for the MPEG-1 encoder (encode.c).
 *
 * Encodes raw YUV420 frames, writes the elementary stream, and decodes it back
 * with this library's own decoder, asserting a reasonable round-trip PSNR (lossy,
 * so not exact). The written .m1v is also meant to be fed to ffmpeg to confirm a
 * conformant, independently-decodable stream. Build:
 *
 *     cc -std=c11 -Iscripts -Iuser/lib/mpeg scripts/mpeg_enc_test.c \
 *        user/lib/mpeg/encode.c user/lib/mpeg/decode.c -lm -o /tmp/mpeg_enc_test
 *     /tmp/mpeg_enc_test in.yuv 176 144 out.m1v
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "mpeg.h"

static double psnr(const unsigned char *a, int as, const unsigned char *b, int bs, int w, int h)
{
	double mse = 0; int x, y;
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) { int d = a[y * as + x] - b[y * bs + x]; mse += (double)d * d; }
	mse /= (double)w * h;
	return mse < 1e-9 ? 99.0 : 10.0 * log10(255.0 * 255.0 / mse);
}

int main(int argc, char **argv)
{
	int W, H, cw, ch, fsz, nf, i, fails = 0;
	long len;
	unsigned char *raw, *stream = 0;
	unsigned slen = 0;
	FILE *f;
	mpeg_encoder *e;
	mpeg_decoder *d;

	if (argc < 5) { fprintf(stderr, "usage: %s in.yuv W H out.m1v\n", argv[0]); return 2; }
	W = atoi(argv[2]); H = atoi(argv[3]);
	cw = (W + 1) / 2; ch = (H + 1) / 2; fsz = W * H + 2 * cw * ch;

	f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 2; }
	fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
	raw = malloc(len); if (fread(raw, 1, len, f) != (size_t)len) return 2; fclose(f);
	nf = (int)(len / fsz);

	if (mpeg_encoder_open(W, H, 25, 1, 4, 12, &e) != MPEG_OK) { fprintf(stderr, "enc open\n"); return 1; }
	for (i = 0; i < nf; i++) {
		const unsigned char *y = raw + (long)i * fsz, *cb = y + W * H, *cr = cb + cw * ch;
		if (mpeg_encode_frame_yuv(e, y, cb, cr) != MPEG_OK) { fprintf(stderr, "encode %d\n", i); return 1; }
	}
	if (mpeg_encoder_finish(e, &stream, &slen) != MPEG_OK) { fprintf(stderr, "finish\n"); return 1; }
	mpeg_encoder_close(e);
	printf("encoded %d frames -> %u bytes (%.2f bpp avg)\n", nf, slen, slen * 8.0 / ((double)nf * W * H));

	f = fopen(argv[4], "wb"); if (f) { fwrite(stream, 1, slen, f); fclose(f); }

	/* decode our own stream back and compare to the source frames */
	if (mpeg_decoder_open(stream, slen, &d) != MPEG_OK) { fprintf(stderr, "dec open\n"); return 1; }
	if (mpeg_width(d) != W || mpeg_height(d) != H) { fprintf(stderr, "dims\n"); fails++; }
	for (i = 0; i < nf; i++) {
		int ys, cbs, crs; const unsigned char *Y, *Cb, *Cr, *sY, *sCb, *sCr;
		double py, pcb, pcr;
		if (mpeg_decode_frame(d) != MPEG_OK) { printf("frame %d ended early\n", i); fails++; break; }
		Y = mpeg_plane_y(d, &ys); Cb = mpeg_plane_cb(d, &cbs); Cr = mpeg_plane_cr(d, &crs);
		sY = raw + (long)i * fsz; sCb = sY + W * H; sCr = sCb + cw * ch;
		py = psnr(Y, ys, sY, W, W, H);
		pcb = psnr(Cb, cbs, sCb, cw, cw, ch);
		pcr = psnr(Cr, crs, sCr, cw, cw, ch);
		printf("frame %d [%c]  Y %.1f dB  Cb %.1f dB  Cr %.1f dB\n",
		       i, "?IPBD"[mpeg_frame_type(d)], py, pcb, pcr);
		if (py < 28.0) fails++;               /* lossy at qscale 4, but must be sane */
	}
	mpeg_decoder_close(d);
	free(raw); free(stream);
	if (fails) { printf("%d failure(s)\n", fails); return 1; }
	printf("encoder round-trip OK\n");
	return 0;
}
