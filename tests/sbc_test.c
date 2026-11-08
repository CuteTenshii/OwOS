/* sbc_test.c: SBC encoder frame layout and, when ffmpeg is installed, a decode round trip against its decoder
 * (drivers/bt/sbc.c). */
#define OWOS_SUITE_TEST
#include "../drivers/bt/sbc.h"
#include "test.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void tone(int16_t *pcm, int frames, int ch)
{
  for (int i = 0; i < frames; i++) {
    double t = i / 48000.0;
    double l = 9000 * sin(2 * M_PI * 440 * t) + 4000 * sin(2 * M_PI * 3100 * t + 1) + 1500 * sin(2 * M_PI * 9000 * t);
    double r = 8000 * sin(2 * M_PI * 660 * t) + 3000 * sin(2 * M_PI * 5000 * t);
    if (ch == 1) {
      pcm[i] = (int16_t)l;
    } else {
      pcm[2 * i] = (int16_t)l;
      pcm[2 * i + 1] = (int16_t)r;
    }
  }
}

static void frame_lengths(void)
{
  struct sbc_enc e;
  sbc_init(&e, SBC_FREQ_44100, 16, SBC_MODE_JOINT, SBC_ALLOC_LOUDNESS, 53);
  expect_eq(sbc_frame_len(&e), 119);
  sbc_init(&e, SBC_FREQ_48000, 16, SBC_MODE_JOINT, SBC_ALLOC_LOUDNESS, 51);
  expect_eq(sbc_frame_len(&e), 115);
  sbc_init(&e, SBC_FREQ_44100, 16, SBC_MODE_MONO, SBC_ALLOC_LOUDNESS, 32);
  expect_eq(sbc_frame_len(&e), 4 + 4 + 64);
  sbc_init(&e, SBC_FREQ_44100, 16, SBC_MODE_DUAL, SBC_ALLOC_LOUDNESS, 32);
  expect_eq(sbc_frame_len(&e), 4 + 8 + 128);
  expect_eq(sbc_frame_samples(&e), 128);
}

static void header_fields(void)
{
  struct sbc_enc e;
  int16_t pcm[256] = {0};
  uint8_t out[256];
  sbc_init(&e, SBC_FREQ_48000, 16, SBC_MODE_JOINT, SBC_ALLOC_LOUDNESS, 51);
  int n = sbc_encode(&e, pcm, out);
  expect_eq(n, sbc_frame_len(&e));
  expect_eq(out[0], 0x9C);
  expect_eq(out[1], 0xFD);
  expect_eq(out[2], 51);
  sbc_init(&e, SBC_FREQ_44100, 8, SBC_MODE_STEREO, SBC_ALLOC_SNR, 40);
  sbc_encode(&e, pcm, out);
  expect_eq(out[1], 0x80 | 0x10 | 0x08 | 0x02 | 0x01);
}

static void bitpool_clamped(void)
{
  struct sbc_enc e;
  sbc_init(&e, SBC_FREQ_48000, 16, SBC_MODE_MONO, SBC_ALLOC_LOUDNESS, 250);
  expect_eq(e.bitpool, 128);
}

/* Encode a tone, decode with ffmpeg, expect the waveform back after the codec delay. */
static void ffmpeg_roundtrip(void)
{
  if (system("ffmpeg -version >/dev/null 2>&1") != 0) {
    printf("sbc: ffmpeg not installed, skipping decode round trip\n");
    return;
  }
  static const struct {
    int mode, alloc, blocks, bitpool;
    double min_snr;
  } cfg[] = {
      {SBC_MODE_MONO, SBC_ALLOC_LOUDNESS, 16, 32, 40},   {SBC_MODE_DUAL, SBC_ALLOC_LOUDNESS, 16, 40, 40},
      {SBC_MODE_STEREO, SBC_ALLOC_LOUDNESS, 16, 51, 35}, {SBC_MODE_STEREO, SBC_ALLOC_SNR, 8, 51, 35},
      {SBC_MODE_JOINT, SBC_ALLOC_LOUDNESS, 12, 53, 40},  {SBC_MODE_JOINT, SBC_ALLOC_SNR, 4, 30, 25},
      {SBC_MODE_JOINT, SBC_ALLOC_LOUDNESS, 16, 19, 10},
  };
  char dir[] = "/tmp/owos-sbc-XXXXXX";
  if (!mkdtemp(dir)) {
    expect(0);
    return;
  }
  for (unsigned c = 0; c < sizeof cfg / sizeof *cfg; c++) {
    struct sbc_enc e;
    sbc_init(&e, SBC_FREQ_48000, cfg[c].blocks, cfg[c].mode, cfg[c].alloc, cfg[c].bitpool);
    int ch = sbc_channels(&e), fs = sbc_frame_samples(&e);
    int frames = 48000 / fs;
    int16_t *pcm = malloc((size_t)frames * fs * ch * 2);
    tone(pcm, frames * fs, ch);

    char in[128], dec[128], cmd[512];
    snprintf(in, sizeof in, "%s/in.sbc", dir);
    snprintf(dec, sizeof dec, "%s/out.pcm", dir);
    FILE *f = fopen(in, "wb");
    uint8_t out[600];
    for (int k = 0; k < frames; k++)
      fwrite(out, 1, (size_t)sbc_encode(&e, pcm + (size_t)k * fs * ch, out), f);
    fclose(f);
    snprintf(cmd, sizeof cmd, "ffmpeg -loglevel error -y -f sbc -i %s -f s16le -ac %d %s", in, ch, dec);
    expect_eq(system(cmd), 0);

    f = fopen(dec, "rb");
    int16_t *got = malloc((size_t)frames * fs * ch * 2);
    size_t n = f ? fread(got, 2, (size_t)frames * fs * ch, f) : 0;
    if (f)
      fclose(f);
    expect(n == (size_t)frames * fs * ch);

    int delay = 73 * ch;
    double sig = 0, err = 0;
    for (int i = 4000 * ch; i < 36000 * ch; i++) {
      double a = pcm[i], b = got[i + delay];
      sig += a * a;
      err += (a - b) * (a - b);
    }
    double snr = 10 * log10(sig / (err > 1 ? err : 1));
    if (snr < cfg[c].min_snr)
      printf("sbc: mode %d alloc %d blocks %d bitpool %d: snr %.1f dB < %.1f\n", cfg[c].mode, cfg[c].alloc,
             cfg[c].blocks, cfg[c].bitpool, snr, cfg[c].min_snr);
    expect(snr >= cfg[c].min_snr);
    free(pcm);
    free(got);
  }
  char cmd[160];
  snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
  if (system(cmd) != 0)
    expect(0);
}

int main(void)
{
  test("frame_lengths", frame_lengths);
  test("header_fields", header_fields);
  test("bitpool_clamped", bitpool_clamped);
  test("ffmpeg_roundtrip", ffmpeg_roundtrip);
  return 0;
}
