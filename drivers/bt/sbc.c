/* sbc.c: A2DP SBC encoder. The analysis window is the specification's, at 2^18 scale. */

#include "sbc.h"

#if __STDC_HOSTED__
#include <string.h>
#else
#include "kstring.h"
#endif

static const int32_t window[80] = {
    0,      42,     90,     146,   216,   299,   388,   468,   528,   552,   523,   424,   237,   -46,   -432,   -916,
    1484,   2105,   2742,   3342,  3842,  4170,  4253,  4016,  3392,  2322,  767,   -1288, -3837, -6844, -10243, -13942,
    17826,  21754,  25579,  29150, 32314, 34935, 36898, 38114, 38524, 38114, 36898, 34935, 32314, 29150, 25579,  21754,
    -17825, -13942, -10243, -6844, -3837, -1288, 767,   2322,  3392,  4016,  4253,  4170,  3842,  3342,  2742,   2105,
    -1483,  -916,   -432,   -46,   237,   424,   523,   552,   528,   468,   388,   299,   216,   146,   90,     42,
};

/* cos(n * pi / 16) at 2^14 scale, n = 0..31. */
static const int16_t cos16[32] = {
    16384,  16069,  15137,  13623,  11585,  9102,  6270,  3196,  0, -3196, -6270, -9102, -11585, -13623, -15137, -16069,
    -16384, -16069, -15137, -13623, -11585, -9102, -6270, -3196, 0, 3196,  6270,  9102,  11585,  13623,  15137,  16069,
};

/* Loudness offsets per sampling frequency. */
static const int8_t offset8[4][8] = {
    {-2, 0, 0, 0, 0, 0, 0, 1},
    {-3, 0, 0, 0, 0, 0, 1, 2},
    {-4, 0, 0, 0, 0, 0, 1, 2},
    {-4, 0, 0, 0, 0, 0, 1, 2},
};

void sbc_init(struct sbc_enc *e, int freq, int blocks, int mode, int alloc, int bitpool)
{
  memset(e, 0, sizeof *e);
  e->freq = (uint8_t)freq;
  e->blocks = (uint8_t)blocks;
  e->mode = (uint8_t)mode;
  e->alloc = (uint8_t)alloc;
  int max_pool = 16 * SBC_SUBBANDS * (mode == SBC_MODE_MONO || mode == SBC_MODE_DUAL ? 1 : 2);
  e->bitpool = (uint8_t)(bitpool > max_pool ? max_pool : bitpool);
}

int sbc_channels(const struct sbc_enc *e) { return e->mode == SBC_MODE_MONO ? 1 : 2; }

int sbc_frame_samples(const struct sbc_enc *e) { return e->blocks * SBC_SUBBANDS; }

int sbc_frame_len(const struct sbc_enc *e)
{
  int ch = sbc_channels(e);
  int bits = ch * SBC_SUBBANDS * 4 + (e->mode == SBC_MODE_JOINT ? SBC_SUBBANDS : 0);
  bits += e->blocks * e->bitpool * (e->mode == SBC_MODE_MONO || e->mode == SBC_MODE_DUAL ? ch : 1);
  return 4 + (bits + 7) / 8;
}

/* CRC-8, polynomial 0x1D, initial value 0x0F, over the first nbits bits, MSB first. */
static uint8_t crc8(const uint8_t *data, int nbits)
{
  uint8_t crc = 0x0F;
  for (int i = 0; i < nbits; i++) {
    int in = (data[i / 8] >> (7 - i % 8)) & 1;
    int top = (crc >> 7) & 1;
    crc = (uint8_t)(crc << 1);
    if (top ^ in)
      crc ^= 0x1D;
  }
  return crc;
}

/* Bit allocation for nch channels sharing one bitpool (both channels for stereo modes, one for mono and dual). */
static void alloc_bits(const struct sbc_enc *e, int nch, const uint8_t sf[][SBC_SUBBANDS], uint8_t bits[][SBC_SUBBANDS])
{
  int bitneed[2][SBC_SUBBANDS], max_need = -100;
  for (int ch = 0; ch < nch; ch++) {
    for (int sb = 0; sb < SBC_SUBBANDS; sb++) {
      int need;
      if (e->alloc == SBC_ALLOC_SNR) {
        need = sf[ch][sb];
      } else if (sf[ch][sb] == 0) {
        need = -5;
      } else {
        int loud = sf[ch][sb] - offset8[e->freq][sb];
        need = loud > 0 ? loud / 2 : loud;
      }
      bitneed[ch][sb] = need;
      if (need > max_need)
        max_need = need;
    }
  }

  int slice = max_need + 1, count = 0, slices = 0;
  do {
    slice--;
    count += slices;
    slices = 0;
    for (int ch = 0; ch < nch; ch++)
      for (int sb = 0; sb < SBC_SUBBANDS; sb++) {
        if (bitneed[ch][sb] > slice + 1 && bitneed[ch][sb] < slice + 16)
          slices++;
        else if (bitneed[ch][sb] == slice + 1)
          slices += 2;
      }
  } while (count + slices < e->bitpool);
  if (count + slices == e->bitpool) {
    count += slices;
    slice--;
  }

  for (int ch = 0; ch < nch; ch++)
    for (int sb = 0; sb < SBC_SUBBANDS; sb++) {
      int b = bitneed[ch][sb] < slice + 2 ? 0 : bitneed[ch][sb] - slice;
      bits[ch][sb] = (uint8_t)(b > 16 ? 16 : b);
    }

  for (int ch = 0, sb = 0; count < e->bitpool && sb < SBC_SUBBANDS;) {
    if (bits[ch][sb] >= 2 && bits[ch][sb] < 16) {
      bits[ch][sb]++;
      count++;
    } else if (bitneed[ch][sb] == slice + 1 && e->bitpool > count + 1) {
      bits[ch][sb] = 2;
      count += 2;
    }
    if (++ch >= nch) {
      ch = 0;
      sb++;
    }
  }
  for (int ch = 0, sb = 0; count < e->bitpool && sb < SBC_SUBBANDS;) {
    if (bits[ch][sb] < 16) {
      bits[ch][sb]++;
      count++;
    }
    if (++ch >= nch) {
      ch = 0;
      sb++;
    }
  }
}

/* One block of SBC_SUBBANDS new samples (stride apart) into the 8 subband samples. */
static void analyze(int16_t *x, const int16_t *in, int stride, int32_t out[SBC_SUBBANDS])
{
  memmove(x + SBC_SUBBANDS, x, (80 - SBC_SUBBANDS) * sizeof *x);
  for (int i = 0; i < SBC_SUBBANDS; i++)
    x[SBC_SUBBANDS - 1 - i] = in[i * stride];

  int64_t y[16];
  for (int i = 0; i < 16; i++) {
    int64_t s = 0;
    for (int j = 0; j < 5; j++)
      s += (int64_t)window[i + 16 * j] * x[i + 16 * j];
    y[i] = s;
  }
  for (int k = 0; k < SBC_SUBBANDS; k++) {
    int64_t s = 0;
    for (int i = 0; i < 16; i++) {
      int n = ((2 * k + 1) * (i - 4)) & 31;
      s += y[i] * cos16[n];
    }
    out[k] = (int32_t)(s >> 32);
  }
}

static uint8_t scale_factor(int32_t max)
{
  uint8_t sf = 0;
  while (sf < 15 && max >= (1 << (sf + 1)))
    sf++;
  return sf;
}

struct bitw {
  uint8_t *p;
  int pos;
};

static void put_bits(struct bitw *w, uint32_t v, int n)
{
  for (int i = n - 1; i >= 0; i--) {
    if (w->pos % 8 == 0)
      w->p[w->pos / 8] = 0;
    w->p[w->pos / 8] |= (uint8_t)(((v >> i) & 1) << (7 - w->pos % 8));
    w->pos++;
  }
}

int sbc_encode(struct sbc_enc *e, const int16_t *pcm, uint8_t *out)
{
  int nch = sbc_channels(e), blocks = e->blocks;
  int32_t sb[16][2][SBC_SUBBANDS];
  for (int b = 0; b < blocks; b++)
    for (int ch = 0; ch < nch; ch++)
      analyze(e->x[ch], pcm + b * SBC_SUBBANDS * nch + ch, nch, sb[b][ch]);

  uint8_t sf[2][SBC_SUBBANDS], bits[2][SBC_SUBBANDS];
  uint8_t join = 0;
  for (int s = 0; s < SBC_SUBBANDS; s++) {
    int32_t mx[2] = {0, 0}, mm = 0, ms = 0;
    for (int b = 0; b < blocks; b++) {
      for (int ch = 0; ch < nch; ch++) {
        int32_t a = sb[b][ch][s] < 0 ? -sb[b][ch][s] : sb[b][ch][s];
        if (a > mx[ch])
          mx[ch] = a;
      }
      if (e->mode == SBC_MODE_JOINT) {
        int32_t m = (sb[b][0][s] + sb[b][1][s]) >> 1, d = (sb[b][0][s] - sb[b][1][s]) >> 1;
        if ((m < 0 ? -m : m) > mm)
          mm = m < 0 ? -m : m;
        if ((d < 0 ? -d : d) > ms)
          ms = d < 0 ? -d : d;
      }
    }
    sf[0][s] = scale_factor(mx[0]);
    if (nch == 2)
      sf[1][s] = scale_factor(mx[1]);
    if (e->mode == SBC_MODE_JOINT && s < SBC_SUBBANDS - 1 &&
        scale_factor(mm) + scale_factor(ms) < sf[0][s] + sf[1][s]) {
      join |= (uint8_t)(1 << s);
      sf[0][s] = scale_factor(mm);
      sf[1][s] = scale_factor(ms);
      for (int b = 0; b < blocks; b++) {
        int32_t l = sb[b][0][s], r = sb[b][1][s];
        sb[b][0][s] = (l + r) >> 1;
        sb[b][1][s] = (l - r) >> 1;
      }
    }
  }

  if (e->mode == SBC_MODE_STEREO || e->mode == SBC_MODE_JOINT) {
    alloc_bits(e, 2, sf, bits);
  } else {
    for (int ch = 0; ch < nch; ch++)
      alloc_bits(e, 1, &sf[ch], &bits[ch]);
  }

  int len = sbc_frame_len(e);
  memset(out, 0, (uint32_t)len);
  out[0] = SBC_SYNCWORD;
  out[1] = (uint8_t)(e->freq << 6 | ((e->blocks / 4 - 1) << 4) | e->mode << 2 | e->alloc << 1 | 1);
  out[2] = e->bitpool;

  /* CRC input: header bytes 1 and 2, the join flags, then the scale factors. */
  uint8_t crcbuf[2 + 1 + 8];
  int cb = 0;
  crcbuf[cb++] = out[1];
  crcbuf[cb++] = out[2];
  struct bitw w = {crcbuf + 2, 0};
  if (e->mode == SBC_MODE_JOINT) {
    uint8_t jb = 0;
    for (int s = 0; s < SBC_SUBBANDS - 1; s++)
      jb |= (uint8_t)(((join >> s) & 1) << (7 - s));
    put_bits(&w, jb, 8);
  }
  for (int ch = 0; ch < nch; ch++)
    for (int s = 0; s < SBC_SUBBANDS; s++)
      put_bits(&w, sf[ch][s], 4);
  out[3] = crc8(crcbuf, 16 + w.pos);

  struct bitw o = {out + 4, 0};
  for (int i = 0; i < w.pos; i++)
    put_bits(&o, (crcbuf[2 + i / 8] >> (7 - i % 8)) & 1, 1);
  for (int b = 0; b < blocks; b++)
    for (int ch = 0; ch < nch; ch++)
      for (int s = 0; s < SBC_SUBBANDS; s++) {
        int n = bits[ch][s];
        if (!n)
          continue;
        int64_t levels = (1 << n) - 1;
        int64_t v = ((int64_t)sb[b][ch][s] + (1 << (sf[ch][s] + 1))) * levels >> (sf[ch][s] + 2);
        put_bits(&o, (uint32_t)(v < 0 ? 0 : v > levels - 1 ? levels - 1 : v), n);
      }
  return len;
}
