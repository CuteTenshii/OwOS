/* mp4_test.c — host-side unit test for user/lib/mp4/mp4.c.
 *
 * The ISO-BMFF box walk and sample-table expansion are pure computation over an
 * in-memory buffer, so they are tested natively — no QEMU. The test hand-builds
 * a minimal but structurally real MP4 (ftyp + moov with one H.264 video track +
 * mdat) and asserts the demuxer reads back every field it is meant to. Build:
 *
 *     cc -std=c11 -Iscripts -Iuser/lib/mp4 \
 *        tests/mp4_test.c user/lib/mp4/mp4.c -o /tmp/mp4_test && /tmp/mp4_test
 *
 * scripts/owos.h supplies the libc decls (memset/malloc/...) the freestanding
 * source pulls, over the host libc. */
#include <stdio.h>
#define OWOS_SUITE_TEST
#include <string.h>
#include "mp4.h"

/* ---- a tiny big-endian MP4 box builder ----------------------------------- */
static unsigned char B[4096];
static unsigned N;

static void u8(unsigned v)  { B[N++] = (unsigned char)v; }
static void u16(unsigned v) { u8(v >> 8); u8(v); }
static void u32(unsigned v) { u8(v >> 24); u8(v >> 16); u8(v >> 8); u8(v); }
static void tag(const char *s) { u8(s[0]); u8(s[1]); u8(s[2]); u8(s[3]); }
static void bytes(const unsigned char *p, unsigned n) { memcpy(B + N, p, n); N += n; }

/* Open a box: reserve its 32-bit size, write the fourcc, return the size slot. */
static unsigned box_open(const char *type) { unsigned at = N; u32(0); tag(type); return at; }
static void box_close(unsigned at) { unsigned sz = N - at; B[at]=sz>>24; B[at+1]=sz>>16; B[at+2]=sz>>8; B[at+3]=sz; }

/* The SPS/PPS bytes we embed in avcC and expect back verbatim. */
static const unsigned char SPS[] = { 0x67, 0x42, 0x00, 0x1E };
static const unsigned char PPS[] = { 0x68, 0xCE, 0x3C };

int main(void)
{
	test("MP4 box and sample-table demux");
	/* ftyp */
	unsigned b = box_open("ftyp");
	tag("isom"); u32(0x200); tag("isom"); tag("mp42");
	box_close(b);

	/* moov > trak > mdia > (mdhd, hdlr, minf > stbl > ...) */
	unsigned moov = box_open("moov");
	unsigned trak = box_open("trak");
	unsigned mdia = box_open("mdia");

	unsigned mdhd = box_open("mdhd");
	u32(0);              /* version/flags */
	u32(0); u32(0);      /* creation, modification */
	u32(1000);           /* timescale */
	u32(300);            /* duration */
	u16(0x55C4); u16(0); /* language + pre_defined */
	box_close(mdhd);

	unsigned hdlr = box_open("hdlr");
	u32(0); u32(0); tag("vide"); u32(0); u32(0); u32(0); u8(0);
	box_close(hdlr);

	unsigned minf = box_open("minf");
	unsigned stbl = box_open("stbl");

	/* stsd: one avc1 VisualSampleEntry carrying an avcC record. */
	unsigned stsd = box_open("stsd");
	u32(0);              /* version/flags */
	u32(1);              /* entry_count */
	unsigned avc1 = box_open("avc1");
	{ unsigned char r6[6] = {0}; bytes(r6, 6); }  /* reserved */
	u16(1);              /* data_reference_index */
	u16(0); u16(0);      /* pre_defined, reserved */
	u32(0); u32(0); u32(0);  /* pre_defined[3] */
	u16(320); u16(240);  /* width, height */
	u32(0x00480000); u32(0x00480000);  /* h/v resolution */
	u32(0);              /* reserved */
	u16(1);              /* frame_count */
	{ unsigned char cn[32] = {0}; bytes(cn, 32); }  /* compressorname */
	u16(24);             /* depth */
	u16(0xFFFF);         /* pre_defined */
	unsigned avcc = box_open("avcC");
	u8(1);               /* configurationVersion */
	u8(0x42); u8(0x00); u8(0x1E);  /* profile, compat, level */
	u8(0xFF);            /* lengthSizeMinusOne=3 -> nal_length_size 4 */
	u8(0xE1);            /* reserved(3) + numSPS=1 */
	u16(sizeof SPS); bytes(SPS, sizeof SPS);
	u8(1);               /* numPPS */
	u16(sizeof PPS); bytes(PPS, sizeof PPS);
	box_close(avcc);
	box_close(avc1);
	box_close(stsd);

	/* stts: 3 samples, delta 100 each -> dts 0,100,200 */
	unsigned stts = box_open("stts");
	u32(0); u32(1); u32(3); u32(100);
	box_close(stts);

	/* ctts: distinct per-sample composition offsets (200, 0, 100) */
	unsigned ctts = box_open("ctts");
	u32(0); u32(3);
	u32(1); u32(200); u32(1); u32(0); u32(1); u32(100);
	box_close(ctts);

	/* stsc: chunk 1.. has 3 samples each (one chunk here) */
	unsigned stsc = box_open("stsc");
	u32(0); u32(1); u32(1); u32(3); u32(1);
	box_close(stsc);

	/* stsz: variable sizes 10, 20, 30 */
	unsigned stsz = box_open("stsz");
	u32(0); u32(0); u32(3); u32(10); u32(20); u32(30);
	box_close(stsz);

	/* stco: one chunk; offset patched after mdat is placed. */
	unsigned stco = box_open("stco");
	u32(0); u32(1);
	unsigned stco_off_slot = N; u32(0);   /* patched below */
	box_close(stco);

	/* stss: sample 1 is the only sync sample. */
	unsigned stss = box_open("stss");
	u32(0); u32(1); u32(1);
	box_close(stss);

	box_close(stbl);
	box_close(minf);
	box_close(mdia);
	box_close(trak);
	box_close(moov);

	/* mdat: the 60 sample bytes (10 of 0xA0, 20 of 0xB0, 30 of 0xC0). */
	unsigned mdat = box_open("mdat");
	unsigned data_off = N;
	{ int i; for (i = 0; i < 10; i++) u8(0xA0); for (i = 0; i < 20; i++) u8(0xB0); for (i = 0; i < 30; i++) u8(0xC0); }
	box_close(mdat);

	B[stco_off_slot]   = data_off >> 24; B[stco_off_slot+1] = data_off >> 16;
	B[stco_off_slot+2] = data_off >> 8;  B[stco_off_slot+3] = data_off;

	/* ---- demux and verify ------------------------------------------------ */
	mp4 *m = 0;
	int rc = mp4_open(B, N, &m);
	expect((rc) == (MP4_OK));
	if (rc != MP4_OK) { expect(0); return 0; }

	expect((mp4_track_count(m)) == (1));
	const mp4_track *v = mp4_video_track(m);
	expect(v != 0);
	expect(mp4_audio_track(m) == 0);

	expect((v->codec) == (MP4_CODEC_H264));
	expect((v->is_video) == (1));
	expect((v->timescale) == (1000));
	expect((v->duration) == (300));
	expect((v->width) == (320));
	expect((v->height) == (240));
	expect((v->nal_length_size) == (4));
	expect((v->n_samples) == (3));

	/* sizes, offsets laid end-to-end from data_off */
	expect((v->samples[0].offset) == (data_off));
	expect((v->samples[0].size) == (10));
	expect((v->samples[1].offset) == (data_off + 10));
	expect((v->samples[1].size) == (20));
	expect((v->samples[2].offset) == (data_off + 30));
	expect((v->samples[2].size) == (30));

	/* dts from stts, pts from dts + ctts */
	expect((v->samples[0].dts) == (0));
	expect((v->samples[1].dts) == (100));
	expect((v->samples[2].dts) == (200));
	expect((v->samples[0].pts) == (200));
	expect((v->samples[1].pts) == (100));
	expect((v->samples[2].pts) == (300));

	/* keyframes: only sample 0 (stss) */
	expect((v->samples[0].keyframe) == (1));
	expect((v->samples[1].keyframe) == (0));
	expect((v->samples[2].keyframe) == (0));

	/* sample data points at the right bytes */
	unsigned sz = 0;
	const unsigned char *d0 = mp4_sample_data(m, v, 0, &sz);
	expect(d0 && sz == 10 && d0[0] == 0xA0);
	const unsigned char *d1 = mp4_sample_data(m, v, 1, &sz);
	expect(d1 && sz == 20 && d1[0] == 0xB0);
	const unsigned char *d2 = mp4_sample_data(m, v, 2, &sz);
	expect(d2 && sz == 30 && d2[0] == 0xC0);

	/* avcC SPS/PPS come back verbatim */
	const unsigned char *nal = 0; unsigned nlen = 0;
	expect((mp4_h264_param_set(v, 0, 0, &nal, &nlen)) == (MP4_OK));
	expect(nlen == sizeof SPS && nal && memcmp(nal, SPS, sizeof SPS) == 0);
	expect((mp4_h264_param_set(v, 1, 0, &nal, &nlen)) == (MP4_OK));
	expect(nlen == sizeof PPS && nal && memcmp(nal, PPS, sizeof PPS) == 0);
	expect((mp4_h264_param_set(v, 0, 1, &nal, &nlen)) == (MP4_EFORMAT));  /* no 2nd SPS */

	/* seek: the only keyframe is sample 0 */
	expect((mp4_seek_sample(v, 250)) == (0));
	expect((mp4_seek_sample(v, 0)) == (0));

	/* a non-MP4 buffer is rejected */
	unsigned char junk[16] = { 'n','o','p','e' };
	mp4 *bad = 0;
	expect((mp4_open(junk, sizeof junk, &bad)) == (MP4_EBADSIG));
	expect(bad == 0);

	mp4_close(m);

	return 0;
}
