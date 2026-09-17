/* alac.c — see alac.h. A faithful port of Apple's open-source ALAC reference
 * (Apache-2.0, (c) 2011 Apple Inc.), restructured into one freestanding file:
 *
 *   - BitBuffer          MSB-first bit reader/writer over a byte buffer
 *   - matrix mix/unmix   generalized middle-side inter-channel decorrelation
 *   - dynamic predictor  adaptive sign-LMS FIR predictor (pc_block/unpc_block)
 *   - adaptive Golomb    Rice coder with run-length + adaptive parameter
 * (dyn_*)
 *   - packet framing     the SCE/CPE element bitstream + magic cookie
 *
 * The arithmetic is transcribed unchanged, so the bitstream is byte-identical
 * to the reference: our packets decode elsewhere and Apple's packets decode
 * here. Everything is integer-only. Only the two entry classes
 * (open/close/decode and open/close/encode) and the buffer ownership are new;
 * the DSP is Apple's.
 */
#include "alac.h"

#ifdef ALAC_HOST_TEST
#include <stdlib.h>
#include <string.h>
#else
#include "owos.h"
#endif

#ifndef NULL
#define NULL ((void *)0)
#endif
#define nil NULL

/* target is always little-endian (x86); the reference bit writer works on
 * big-endian 32-bit words, so swap on load/store. */
static inline uint32_t bswap32(uint32_t x)
{
  return (x << 24) | ((x << 8) & 0x00ff0000) | ((x >> 8) & 0x0000ff00) | ((x >> 24) & 0xff);
}

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

/* ---- constants (aglib.h / dplib.h / ALACAudioTypes.h) -------------------- */
#define QBSHIFT 9
#define QB (1 << QBSHIFT)
#define PB0 40
#define MB0 10
#define KB0 14
#define MAX_RUN_DEFAULT 255
#define MMULSHIFT 2
#define MDENSHIFT (QBSHIFT - MMULSHIFT - 1)
#define MOFF (1 << (MDENSHIFT - 2))
#define BITOFF 24
#define MAX_PREFIX_16 9
#define MAX_PREFIX_32 9
#define MAX_DATATYPE_BITS_16 16
#define N_MAX_MEAN_CLAMP 0xffff
#define N_MEAN_CLAMP_VAL 0xffff

#define DENSHIFT_DEFAULT 9
#define AINIT 38
#define BINIT (-29)
#define CINIT (-2)
#define NUMCOEPAIRS 16

#define kALACMaxChannels 8
#define kALACMaxSearches 16
#define kALACMaxCoefs 16
#define kALACDefaultFrameSize 4096
#define kMaxSampleSize 32
#define kDefaultMixBits 2
#define kDefaultMixRes 0
#define kMaxRes 4
#define kDefaultNumUV 8
#define kMinUV 4
#define kMaxUV 8

enum { ID_SCE = 0, ID_CPE = 1, ID_LFE = 3, ID_DSE = 4, ID_FIL = 6, ID_END = 7 };

/* ============================ BitBuffer ================================== */
typedef struct {
  uint8_t *cur;
  uint8_t *end;
  uint32_t bitIndex;
  uint32_t byteSize;
  int error; /* latched once a read/advance would cross `end` */
} BitBuffer;

static void BitBufferInit(BitBuffer *bits, uint8_t *buffer, uint32_t byteSize)
{
  bits->cur = buffer;
  bits->end = buffer + byteSize;
  bits->bitIndex = 0;
  bits->byteSize = byteSize;
  bits->error = 0;
}

/* Bits still readable from the current position to `end`; 0 once exhausted. */
static uint32_t BitBufferRemaining(const BitBuffer *bits)
{
  if (bits->cur >= bits->end)
    return 0;
  return (uint32_t)(bits->end - bits->cur) * 8 - bits->bitIndex;
}

static uint32_t BitBufferRead(BitBuffer *bits, uint8_t numBits) /* <= 16 bits */
{
  uint32_t returnBits;
  if (BitBufferRemaining(bits) < numBits) {
    bits->error = 1;
    return 0;
  }
  returnBits = ((uint32_t)bits->cur[0] << 16) | ((uint32_t)bits->cur[1] << 8) | bits->cur[2];
  returnBits = returnBits << bits->bitIndex;
  returnBits &= 0x00FFFFFF;
  bits->bitIndex += numBits;
  returnBits = returnBits >> (24 - numBits);
  bits->cur += (bits->bitIndex >> 3);
  bits->bitIndex &= 7;
  return returnBits;
}

static uint8_t BitBufferReadSmall(BitBuffer *bits, uint8_t numBits) /* <= 8 bits */
{
  uint16_t returnBits;
  if (BitBufferRemaining(bits) < numBits) {
    bits->error = 1;
    return 0;
  }
  returnBits = (bits->cur[0] << 8) | bits->cur[1];
  returnBits = returnBits << bits->bitIndex;
  bits->bitIndex += numBits;
  returnBits = returnBits >> (16 - numBits);
  bits->cur += (bits->bitIndex >> 3);
  bits->bitIndex &= 7;
  return (uint8_t)returnBits;
}

static uint8_t BitBufferReadOne(BitBuffer *bits)
{
  uint8_t returnBits;
  if (BitBufferRemaining(bits) < 1) {
    bits->error = 1;
    return 0;
  }
  returnBits = (bits->cur[0] >> (7 - bits->bitIndex)) & 1;
  bits->bitIndex++;
  bits->cur += (bits->bitIndex >> 3);
  bits->bitIndex &= 7;
  return returnBits;
}

static uint32_t BitBufferGetPosition(BitBuffer *bits)
{
  uint8_t *begin = bits->end - bits->byteSize;
  return ((uint32_t)(bits->cur - begin) * 8) + bits->bitIndex;
}

static void BitBufferAdvance(BitBuffer *bits, uint32_t numBits)
{
  if (numBits) {
    uint32_t remaining = BitBufferRemaining(bits);
    if (numBits > remaining) {
      numBits = remaining;
      bits->error = 1;
    }
    bits->bitIndex += numBits;
    bits->cur += (bits->bitIndex >> 3);
    bits->bitIndex &= 7;
  }
}

static void BitBufferWrite(BitBuffer *bits, uint32_t bitValues, uint32_t numBits)
{
  uint32_t invBitIndex;
  if (numBits == 0)
    return;
  invBitIndex = 8 - bits->bitIndex;
  while (numBits > 0) {
    uint32_t tmp, curNum = MIN(invBitIndex, numBits);
    uint8_t shift, mask;
    tmp = bitValues >> (numBits - curNum);
    shift = (uint8_t)(invBitIndex - curNum);
    mask = 0xffu >> (8 - curNum);
    mask <<= shift;
    bits->cur[0] = (bits->cur[0] & ~mask) | (((uint8_t)tmp << shift) & mask);
    numBits -= curNum;
    invBitIndex -= curNum;
    if (invBitIndex == 0) {
      invBitIndex = 8;
      bits->cur++;
    }
  }
  bits->bitIndex = 8 - invBitIndex;
}

static void BitBufferByteAlign(BitBuffer *bits, int addZeros)
{
  if (bits->bitIndex == 0)
    return;
  if (addZeros)
    BitBufferWrite(bits, 0, 8 - bits->bitIndex);
  else
    BitBufferAdvance(bits, 8 - bits->bitIndex);
}

/* ============================ matrix (mix/unmix) ========================= */
/* 24-bit words are little-endian 3-byte in memory: byte0=LSB, byte2=MSB. */
#define LBYTE 0
#define MBYTE 1
#define HBYTE 2

static void mix16(int16_t *in, uint32_t stride, int32_t *u, int32_t *v, int32_t numSamples, int32_t mixbits,
                  int32_t mixres)
{
  int16_t *ip = in;
  int32_t j;
  if (mixres != 0) {
    int32_t mod = 1 << mixbits, m2 = mod - mixres;
    for (j = 0; j < numSamples; j++) {
      int32_t l = ip[0], r = ip[1];
      ip += stride;
      u[j] = (mixres * l + m2 * r) >> mixbits;
      v[j] = l - r;
    }
  } else {
    for (j = 0; j < numSamples; j++) {
      u[j] = ip[0];
      v[j] = ip[1];
      ip += stride;
    }
  }
}

static void unmix16(int32_t *u, int32_t *v, int16_t *out, uint32_t stride, int32_t numSamples, int32_t mixbits,
                    int32_t mixres)
{
  int16_t *op = out;
  int32_t j;
  if (mixres != 0) {
    for (j = 0; j < numSamples; j++) {
      int32_t l = u[j] + v[j] - ((mixres * v[j]) >> mixbits);
      int32_t r = l - v[j];
      op[0] = (int16_t)l;
      op[1] = (int16_t)r;
      op += stride;
    }
  } else {
    for (j = 0; j < numSamples; j++) {
      op[0] = (int16_t)u[j];
      op[1] = (int16_t)v[j];
      op += stride;
    }
  }
}

static void mix20(uint8_t *in, uint32_t stride, int32_t *u, int32_t *v, int32_t numSamples, int32_t mixbits,
                  int32_t mixres)
{
  uint8_t *ip = in;
  int32_t l, r, j;
  if (mixres != 0) {
    int32_t mod = 1 << mixbits, m2 = mod - mixres;
    for (j = 0; j < numSamples; j++) {
      l = (int32_t)(((uint32_t)ip[HBYTE] << 16) | ((uint32_t)ip[MBYTE] << 8) | ip[LBYTE]);
      l = (l << 8) >> 12;
      ip += 3;
      r = (int32_t)(((uint32_t)ip[HBYTE] << 16) | ((uint32_t)ip[MBYTE] << 8) | ip[LBYTE]);
      r = (r << 8) >> 12;
      ip += (stride - 1) * 3;
      u[j] = (mixres * l + m2 * r) >> mixbits;
      v[j] = l - r;
    }
  } else {
    for (j = 0; j < numSamples; j++) {
      l = (int32_t)(((uint32_t)ip[HBYTE] << 16) | ((uint32_t)ip[MBYTE] << 8) | ip[LBYTE]);
      u[j] = (l << 8) >> 12;
      ip += 3;
      r = (int32_t)(((uint32_t)ip[HBYTE] << 16) | ((uint32_t)ip[MBYTE] << 8) | ip[LBYTE]);
      v[j] = (r << 8) >> 12;
      ip += (stride - 1) * 3;
    }
  }
}

static void unmix20(int32_t *u, int32_t *v, uint8_t *out, uint32_t stride, int32_t numSamples, int32_t mixbits,
                    int32_t mixres)
{
  uint8_t *op = out;
  int32_t j;
  if (mixres != 0) {
    for (j = 0; j < numSamples; j++) {
      int32_t l = u[j] + v[j] - ((mixres * v[j]) >> mixbits);
      int32_t r = l - v[j];
      l <<= 4;
      r <<= 4;
      op[HBYTE] = (uint8_t)(l >> 16);
      op[MBYTE] = (uint8_t)(l >> 8);
      op[LBYTE] = (uint8_t)l;
      op += 3;
      op[HBYTE] = (uint8_t)(r >> 16);
      op[MBYTE] = (uint8_t)(r >> 8);
      op[LBYTE] = (uint8_t)r;
      op += (stride - 1) * 3;
    }
  } else {
    for (j = 0; j < numSamples; j++) {
      int32_t val = u[j] << 4;
      op[HBYTE] = (uint8_t)(val >> 16);
      op[MBYTE] = (uint8_t)(val >> 8);
      op[LBYTE] = (uint8_t)val;
      op += 3;
      val = v[j] << 4;
      op[HBYTE] = (uint8_t)(val >> 16);
      op[MBYTE] = (uint8_t)(val >> 8);
      op[LBYTE] = (uint8_t)val;
      op += (stride - 1) * 3;
    }
  }
}

static void mix24(uint8_t *in, uint32_t stride, int32_t *u, int32_t *v, int32_t numSamples, int32_t mixbits,
                  int32_t mixres, uint16_t *shiftUV, int32_t bytesShifted)
{
  uint8_t *ip = in;
  int32_t shift = bytesShifted * 8;
  uint32_t mask = (1u << shift) - 1;
  int32_t l, r, j, k;
  if (mixres != 0) {
    int32_t mod = 1 << mixbits, m2 = mod - mixres;
    if (bytesShifted != 0) {
      for (j = 0, k = 0; j < numSamples; j++, k += 2) {
        l = (int32_t)(((uint32_t)ip[HBYTE] << 16) | ((uint32_t)ip[MBYTE] << 8) | ip[LBYTE]);
        l = (l << 8) >> 8;
        ip += 3;
        r = (int32_t)(((uint32_t)ip[HBYTE] << 16) | ((uint32_t)ip[MBYTE] << 8) | ip[LBYTE]);
        r = (r << 8) >> 8;
        ip += (stride - 1) * 3;
        shiftUV[k + 0] = (uint16_t)(l & mask);
        shiftUV[k + 1] = (uint16_t)(r & mask);
        l >>= shift;
        r >>= shift;
        u[j] = (mixres * l + m2 * r) >> mixbits;
        v[j] = l - r;
      }
    } else {
      for (j = 0; j < numSamples; j++) {
        l = (int32_t)(((uint32_t)ip[HBYTE] << 16) | ((uint32_t)ip[MBYTE] << 8) | ip[LBYTE]);
        l = (l << 8) >> 8;
        ip += 3;
        r = (int32_t)(((uint32_t)ip[HBYTE] << 16) | ((uint32_t)ip[MBYTE] << 8) | ip[LBYTE]);
        r = (r << 8) >> 8;
        ip += (stride - 1) * 3;
        u[j] = (mixres * l + m2 * r) >> mixbits;
        v[j] = l - r;
      }
    }
  } else {
    if (bytesShifted != 0) {
      for (j = 0, k = 0; j < numSamples; j++, k += 2) {
        l = (int32_t)(((uint32_t)ip[HBYTE] << 16) | ((uint32_t)ip[MBYTE] << 8) | ip[LBYTE]);
        l = (l << 8) >> 8;
        ip += 3;
        r = (int32_t)(((uint32_t)ip[HBYTE] << 16) | ((uint32_t)ip[MBYTE] << 8) | ip[LBYTE]);
        r = (r << 8) >> 8;
        ip += (stride - 1) * 3;
        shiftUV[k + 0] = (uint16_t)(l & mask);
        shiftUV[k + 1] = (uint16_t)(r & mask);
        l >>= shift;
        r >>= shift;
        u[j] = l;
        v[j] = r;
      }
    } else {
      for (j = 0; j < numSamples; j++) {
        l = (int32_t)(((uint32_t)ip[HBYTE] << 16) | ((uint32_t)ip[MBYTE] << 8) | ip[LBYTE]);
        u[j] = (l << 8) >> 8;
        ip += 3;
        r = (int32_t)(((uint32_t)ip[HBYTE] << 16) | ((uint32_t)ip[MBYTE] << 8) | ip[LBYTE]);
        v[j] = (r << 8) >> 8;
        ip += (stride - 1) * 3;
      }
    }
  }
}

static void unmix24(int32_t *u, int32_t *v, uint8_t *out, uint32_t stride, int32_t numSamples, int32_t mixbits,
                    int32_t mixres, uint16_t *shiftUV, int32_t bytesShifted)
{
  uint8_t *op = out;
  int32_t shift = bytesShifted * 8, l, r, j, k;
  if (mixres != 0) {
    if (bytesShifted != 0) {
      for (j = 0, k = 0; j < numSamples; j++, k += 2) {
        l = u[j] + v[j] - ((mixres * v[j]) >> mixbits);
        r = l - v[j];
        l = (l << shift) | (uint32_t)shiftUV[k + 0];
        r = (r << shift) | (uint32_t)shiftUV[k + 1];
        op[HBYTE] = (uint8_t)(l >> 16);
        op[MBYTE] = (uint8_t)(l >> 8);
        op[LBYTE] = (uint8_t)l;
        op += 3;
        op[HBYTE] = (uint8_t)(r >> 16);
        op[MBYTE] = (uint8_t)(r >> 8);
        op[LBYTE] = (uint8_t)r;
        op += (stride - 1) * 3;
      }
    } else {
      for (j = 0; j < numSamples; j++) {
        l = u[j] + v[j] - ((mixres * v[j]) >> mixbits);
        r = l - v[j];
        op[HBYTE] = (uint8_t)(l >> 16);
        op[MBYTE] = (uint8_t)(l >> 8);
        op[LBYTE] = (uint8_t)l;
        op += 3;
        op[HBYTE] = (uint8_t)(r >> 16);
        op[MBYTE] = (uint8_t)(r >> 8);
        op[LBYTE] = (uint8_t)r;
        op += (stride - 1) * 3;
      }
    }
  } else {
    if (bytesShifted != 0) {
      for (j = 0, k = 0; j < numSamples; j++, k += 2) {
        l = u[j];
        r = v[j];
        l = (l << shift) | (uint32_t)shiftUV[k + 0];
        r = (r << shift) | (uint32_t)shiftUV[k + 1];
        op[HBYTE] = (uint8_t)(l >> 16);
        op[MBYTE] = (uint8_t)(l >> 8);
        op[LBYTE] = (uint8_t)l;
        op += 3;
        op[HBYTE] = (uint8_t)(r >> 16);
        op[MBYTE] = (uint8_t)(r >> 8);
        op[LBYTE] = (uint8_t)r;
        op += (stride - 1) * 3;
      }
    } else {
      for (j = 0; j < numSamples; j++) {
        int32_t val = u[j];
        op[HBYTE] = (uint8_t)(val >> 16);
        op[MBYTE] = (uint8_t)(val >> 8);
        op[LBYTE] = (uint8_t)val;
        op += 3;
        val = v[j];
        op[HBYTE] = (uint8_t)(val >> 16);
        op[MBYTE] = (uint8_t)(val >> 8);
        op[LBYTE] = (uint8_t)val;
        op += (stride - 1) * 3;
      }
    }
  }
}

static void mix32(int32_t *in, uint32_t stride, int32_t *u, int32_t *v, int32_t numSamples, int32_t mixbits,
                  int32_t mixres, uint16_t *shiftUV, int32_t bytesShifted)
{
  int32_t *ip = in;
  int32_t shift = bytesShifted * 8;
  uint32_t mask = (1u << shift) - 1;
  int32_t l, r, j, k;
  if (mixres != 0) {
    int32_t mod = 1 << mixbits, m2 = mod - mixres;
    for (j = 0, k = 0; j < numSamples; j++, k += 2) {
      l = ip[0];
      r = ip[1];
      ip += stride;
      shiftUV[k + 0] = (uint16_t)(l & mask);
      shiftUV[k + 1] = (uint16_t)(r & mask);
      l >>= shift;
      r >>= shift;
      u[j] = (mixres * l + m2 * r) >> mixbits;
      v[j] = l - r;
    }
  } else {
    if (bytesShifted == 0) {
      for (j = 0; j < numSamples; j++) {
        u[j] = ip[0];
        v[j] = ip[1];
        ip += stride;
      }
    } else {
      for (j = 0, k = 0; j < numSamples; j++, k += 2) {
        l = ip[0];
        r = ip[1];
        ip += stride;
        shiftUV[k + 0] = (uint16_t)(l & mask);
        shiftUV[k + 1] = (uint16_t)(r & mask);
        l >>= shift;
        r >>= shift;
        u[j] = l;
        v[j] = r;
      }
    }
  }
}

static void unmix32(int32_t *u, int32_t *v, int32_t *out, uint32_t stride, int32_t numSamples, int32_t mixbits,
                    int32_t mixres, uint16_t *shiftUV, int32_t bytesShifted)
{
  int32_t *op = out;
  int32_t shift = bytesShifted * 8, l, r, j, k;
  if (mixres != 0) {
    for (j = 0, k = 0; j < numSamples; j++, k += 2) {
      int32_t lt = u[j], rt = v[j];
      l = lt + rt - ((mixres * rt) >> mixbits);
      r = l - rt;
      op[0] = (l << shift) | (uint32_t)shiftUV[k + 0];
      op[1] = (r << shift) | (uint32_t)shiftUV[k + 1];
      op += stride;
    }
  } else {
    if (bytesShifted == 0) {
      for (j = 0; j < numSamples; j++) {
        op[0] = u[j];
        op[1] = v[j];
        op += stride;
      }
    } else {
      for (j = 0, k = 0; j < numSamples; j++, k += 2) {
        op[0] = (u[j] << shift) | (uint32_t)shiftUV[k + 0];
        op[1] = (v[j] << shift) | (uint32_t)shiftUV[k + 1];
        op += stride;
      }
    }
  }
}

/* 20/24/32-bit <-> 32-bit predictor helpers */
static void copy20ToPredictor(uint8_t *in, uint32_t stride, int32_t *out, int32_t numSamples)
{
  uint8_t *ip = in;
  int32_t j;
  for (j = 0; j < numSamples; j++) {
    int32_t val = (int32_t)(((uint32_t)ip[HBYTE] << 16) | ((uint32_t)ip[MBYTE] << 8) | ip[LBYTE]);
    out[j] = (val << 8) >> 12;
    ip += stride * 3;
  }
}
static void copy24ToPredictor(uint8_t *in, uint32_t stride, int32_t *out, int32_t numSamples)
{
  uint8_t *ip = in;
  int32_t j;
  for (j = 0; j < numSamples; j++) {
    int32_t val = (int32_t)(((uint32_t)ip[HBYTE] << 16) | ((uint32_t)ip[MBYTE] << 8) | ip[LBYTE]);
    out[j] = (val << 8) >> 8;
    ip += stride * 3;
  }
}
static void copyPredictorTo20(int32_t *in, uint8_t *out, uint32_t stride, int32_t numSamples)
{
  uint8_t *op = out;
  int32_t j;
  for (j = 0; j < numSamples; j++) {
    int32_t val = in[j];
    op[HBYTE] = (uint8_t)(val >> 12);
    op[MBYTE] = (uint8_t)(val >> 4);
    op[LBYTE] = (uint8_t)(val << 4);
    op += stride * 3;
  }
}
static void copyPredictorTo24(int32_t *in, uint8_t *out, uint32_t stride, int32_t numSamples)
{
  uint8_t *op = out;
  int32_t j;
  for (j = 0; j < numSamples; j++) {
    int32_t val = in[j];
    op[HBYTE] = (uint8_t)(val >> 16);
    op[MBYTE] = (uint8_t)(val >> 8);
    op[LBYTE] = (uint8_t)val;
    op += stride * 3;
  }
}
static void copyPredictorTo24Shift(int32_t *in, uint16_t *shift, uint8_t *out, uint32_t stride, int32_t numSamples,
                                   int32_t bytesShifted)
{
  uint8_t *op = out;
  int32_t shiftVal = bytesShifted * 8, j;
  for (j = 0; j < numSamples; j++) {
    int32_t val = (in[j] << shiftVal) | (uint32_t)shift[j];
    op[HBYTE] = (uint8_t)(val >> 16);
    op[MBYTE] = (uint8_t)(val >> 8);
    op[LBYTE] = (uint8_t)val;
    op += stride * 3;
  }
}
static void copyPredictorTo32(int32_t *in, int32_t *out, uint32_t stride, int32_t numSamples)
{
  int32_t i, j;
  for (i = 0, j = 0; i < numSamples; i++, j += stride)
    out[j] = in[i];
}
static void copyPredictorTo32Shift(int32_t *in, uint16_t *shift, int32_t *out, uint32_t stride, int32_t numSamples,
                                   int32_t bytesShifted)
{
  int32_t *op = out;
  uint32_t shiftVal = bytesShifted * 8;
  int32_t j;
  for (j = 0; j < numSamples; j++) {
    op[0] = (in[j] << shiftVal) | (uint32_t)shift[j];
    op += stride;
  }
}

/* ============================ dynamic predictor ========================== */
static void init_coefs(int16_t *coefs, uint32_t denshift, int32_t numPairs)
{
  int32_t k, den = 1 << denshift;
  coefs[0] = (AINIT * den) >> 4;
  coefs[1] = (BINIT * den) >> 4;
  coefs[2] = (CINIT * den) >> 4;
  for (k = 3; k < numPairs; k++)
    coefs[k] = 0;
}

static inline int32_t sign_of_int(int32_t i)
{
  int32_t negishift = ((uint32_t)-i) >> 31;
  return negishift | (i >> 31);
}

static void pc_block(int32_t *in, int32_t *pc1, int32_t num, int16_t *coefs, int32_t numactive, uint32_t chanbits,
                     uint32_t denshift)
{
  int32_t j, k, lim, sum1, dd, sg, sgn, top, del, del0;
  int32_t *pin;
  uint32_t chanshift = 32 - chanbits;
  int32_t denhalf = 1 << (denshift - 1);

  pc1[0] = in[0];
  if (numactive == 0) {
    if ((num > 1) && (in != pc1))
      memcpy(&pc1[1], &in[1], (num - 1) * sizeof(int32_t));
    return;
  }
  if (numactive == 31) { /* order-1 fixed predictor */
    for (j = 1; j < num; j++) {
      del = in[j] - in[j - 1];
      pc1[j] = (del << chanshift) >> chanshift;
    }
    return;
  }
  for (j = 1; j <= numactive; j++) {
    del = in[j] - in[j - 1];
    pc1[j] = (del << chanshift) >> chanshift;
  }
  lim = numactive + 1;
  for (j = lim; j < num; j++) {
    top = in[j - lim];
    pin = in + j - 1;
    sum1 = 0;
    for (k = 0; k < numactive; k++)
      sum1 -= coefs[k] * (top - pin[-k]);
    del = in[j] - top - ((sum1 + denhalf) >> denshift);
    del = (del << chanshift) >> chanshift;
    pc1[j] = del;
    del0 = del;
    sg = sign_of_int(del);
    if (sg > 0) {
      for (k = numactive - 1; k >= 0; k--) {
        dd = top - pin[-k];
        sgn = sign_of_int(dd);
        coefs[k] -= sgn;
        del0 -= (numactive - k) * ((sgn * dd) >> denshift);
        if (del0 <= 0)
          break;
      }
    } else if (sg < 0) {
      for (k = numactive - 1; k >= 0; k--) {
        dd = top - pin[-k];
        sgn = sign_of_int(dd);
        coefs[k] += sgn;
        del0 -= (numactive - k) * ((-sgn * dd) >> denshift);
        if (del0 >= 0)
          break;
      }
    }
  }
}

static void unpc_block(int32_t *pc1, int32_t *out, int32_t num, int16_t *coefs, int32_t numactive, uint32_t chanbits,
                       uint32_t denshift)
{
  int32_t j, k, lim, sum1, sg, sgn, top, dd, del, del0;
  int32_t *pout;
  uint32_t chanshift = 32 - chanbits;
  int32_t denhalf = 1 << (denshift - 1);

  out[0] = pc1[0];
  if (numactive == 0) {
    if ((num > 1) && (pc1 != out))
      memcpy(&out[1], &pc1[1], (num - 1) * sizeof(int32_t));
    return;
  }
  if (numactive == 31) {
    int32_t prev = out[0];
    for (j = 1; j < num; j++) {
      del = pc1[j] + prev;
      prev = (del << chanshift) >> chanshift;
      out[j] = prev;
    }
    return;
  }
  /* Warmup writes out[1..numactive] unconditionally. In a valid stream num is
   * always > numactive, but both num (numSamples) and numactive (numU/numV) are
   * attacker-controlled and only bounded by frame_length; a hostile packet with
   * numactive >= num would run this loop past the num-sized buffer. Clamp it.
   */
  {
    int32_t warm = (numactive < num) ? numactive : (num - 1);
    for (j = 1; j <= warm; j++) {
      del = pc1[j] + out[j - 1];
      out[j] = (del << chanshift) >> chanshift;
    }
  }
  lim = numactive + 1;
  for (j = lim; j < num; j++) {
    sum1 = 0;
    pout = out + j - 1;
    top = out[j - lim];
    for (k = 0; k < numactive; k++)
      sum1 += coefs[k] * (pout[-k] - top);
    del = pc1[j];
    del0 = del;
    sg = sign_of_int(del);
    del += top + ((sum1 + denhalf) >> denshift);
    out[j] = (del << chanshift) >> chanshift;
    if (sg > 0) {
      for (k = numactive - 1; k >= 0; k--) {
        dd = top - pout[-k];
        sgn = sign_of_int(dd);
        coefs[k] -= sgn;
        del0 -= (numactive - k) * ((sgn * dd) >> denshift);
        if (del0 <= 0)
          break;
      }
    } else if (sg < 0) {
      for (k = numactive - 1; k >= 0; k--) {
        dd = top - pout[-k];
        sgn = sign_of_int(dd);
        coefs[k] += sgn;
        del0 -= (numactive - k) * ((-sgn * dd) >> denshift);
        if (del0 >= 0)
          break;
      }
    }
  }
}

/* ============================ adaptive Golomb ============================ */
typedef struct {
  uint32_t mb, mb0, pb, kb, wb, qb, fw, sw, maxrun;
} AGParamRec;

static void set_ag_params(AGParamRec *p, uint32_t m, uint32_t pp, uint32_t k, uint32_t f, uint32_t s, uint32_t maxrun)
{
  p->mb = p->mb0 = m;
  p->pb = pp;
  p->kb = k;
  p->wb = (1u << p->kb) - 1;
  p->qb = QB - p->pb;
  p->fw = f;
  p->sw = s;
  p->maxrun = maxrun;
}

static inline int32_t lead(int32_t m)
{
  int32_t j;
  uint32_t c = (1u << 31);
  for (j = 0; j < 32; j++) {
    if ((c & (uint32_t)m) != 0)
      break;
    c >>= 1;
  }
  return j;
}
static inline int32_t lg3a(int32_t x)
{
  x += 3;
  return 31 - lead(x);
}
static inline int32_t abs_func(int32_t a)
{
  int32_t s = a >> 31;
  return (a ^ s) - s;
}

static inline uint32_t read32bit(uint8_t *b)
{
  return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

#define get_next_fromlong(inlong, suff) ((inlong) >> (32 - (suff)))

static inline uint32_t getstreambits(uint8_t *in, int32_t bitoffset, int32_t numbits)
{
  uint32_t byteoffset = bitoffset / 8;
  uint32_t load1 = read32bit(in + byteoffset), load2, result;
  if ((numbits + (bitoffset & 0x7)) > 32) {
    int32_t load2shift;
    result = load1 << (bitoffset & 0x7);
    load2 = (uint32_t)in[byteoffset + 4];
    load2shift = (8 - (numbits + (bitoffset & 0x7) - 32));
    load2 >>= load2shift;
    result >>= (32 - numbits);
    result |= load2;
  } else {
    result = load1 >> (32 - numbits - (bitoffset & 7));
  }
  if (numbits != 32)
    result &= ~(0xffffffffu << numbits);
  return result;
}

static inline int32_t dyn_get(uint8_t *in, uint32_t *bitPos, uint32_t m, uint32_t k)
{
  uint32_t tempbits = *bitPos, result, pre = 0, v, streamlong;
  streamlong = read32bit(in + (tempbits >> 3));
  streamlong <<= (tempbits & 7);
  {
    uint32_t notI = ~streamlong;
    pre = lead(notI);
  }
  if (pre >= MAX_PREFIX_16) {
    pre = MAX_PREFIX_16;
    tempbits += pre;
    streamlong <<= pre;
    result = get_next_fromlong(streamlong, MAX_DATATYPE_BITS_16);
    tempbits += MAX_DATATYPE_BITS_16;
  } else {
    tempbits += pre;
    tempbits += 1;
    streamlong <<= pre + 1;
    v = get_next_fromlong(streamlong, k);
    tempbits += k;
    result = pre * m + v - 1;
    if (v < 2) {
      result -= (v - 1);
      tempbits -= 1;
    }
  }
  *bitPos = tempbits;
  return result;
}

static inline int32_t dyn_get_32bit(uint8_t *in, uint32_t *bitPos, int32_t m, int32_t k, int32_t maxbits)
{
  uint32_t tempbits = *bitPos, v, streamlong, result;
  streamlong = read32bit(in + (tempbits >> 3));
  streamlong <<= (tempbits & 7);
  {
    uint32_t notI = ~streamlong;
    result = lead(notI);
  }
  if (result >= MAX_PREFIX_32) {
    result = getstreambits(in, tempbits + MAX_PREFIX_32, maxbits);
    tempbits += MAX_PREFIX_32 + maxbits;
  } else {
    tempbits += result;
    tempbits += 1;
    if (k != 1) {
      streamlong <<= result + 1;
      v = get_next_fromlong(streamlong, k);
      tempbits += k;
      tempbits -= 1;
      result = result * m;
      if (v >= 2) {
        result += (v - 1);
        tempbits += 1;
      }
    }
  }
  *bitPos = tempbits;
  return result;
}

static int dyn_decomp(AGParamRec *params, BitBuffer *bitstream, int32_t *pc, int32_t numSamples, int32_t maxSize,
                      uint32_t *outNumBits)
{
  uint8_t *in;
  int32_t *outPtr = pc;
  uint32_t bitPos, startPos, maxPos, j, m, k, n, c, mz, mb;
  int32_t del, zmode, status = ALAC_OK;
  uint32_t pb_local = params->pb, kb_local = params->kb, wb_local = params->wb;

  *outNumBits = 0;
  in = bitstream->cur;
  startPos = bitstream->bitIndex;
  /* dyn_get* read via read32bit(in + bitPos/8) — relative to `in`, which has
   * already advanced. Bound bitPos by the bits left from `in` to `end` (not
   * from the buffer start) or the guard `bitPos >= maxPos` lets reads run past
   * end. */
  maxPos = (uint32_t)(bitstream->end - in) * 8;
  bitPos = startPos;
  mb = params->mb0;
  zmode = 0;
  c = 0;
  while (c < (uint32_t)numSamples) {
    if (bitPos >= maxPos) {
      status = ALAC_EFORMAT;
      goto Exit;
    }
    m = mb >> QBSHIFT;
    k = lg3a(m);
    k = MIN(k, kb_local);
    m = (1 << k) - 1;
    n = dyn_get_32bit(in, &bitPos, m, k, maxSize);
    {
      uint32_t ndecode = n + zmode;
      int32_t multiplier = (-(int32_t)(ndecode & 1)) | 1;
      del = ((ndecode + 1) >> 1) * multiplier;
    }
    *outPtr++ = del;
    c++;
    mb = pb_local * (n + zmode) + mb - ((pb_local * mb) >> QBSHIFT);
    if (n > N_MAX_MEAN_CLAMP)
      mb = N_MEAN_CLAMP_VAL;
    zmode = 0;
    if (((mb << MMULSHIFT) < QB) && (c < (uint32_t)numSamples)) {
      zmode = 1;
      k = lead(mb) - BITOFF + ((mb + MOFF) >> MDENSHIFT);
      mz = ((1 << k) - 1) & wb_local;
      n = dyn_get(in, &bitPos, mz, k);
      if (c + n > (uint32_t)numSamples) {
        status = ALAC_EFORMAT;
        goto Exit;
      }
      for (j = 0; j < n; j++) {
        *outPtr++ = 0;
        ++c;
      }
      if (n >= 65535)
        zmode = 0;
      mb = 0;
    }
  }
Exit:
  *outNumBits = (bitPos - startPos);
  BitBufferAdvance(bitstream, *outNumBits);
  if (bitstream->cur > bitstream->end)
    status = ALAC_EFORMAT;
  return status;
}

static inline int32_t dyn_code(int32_t m, int32_t k, int32_t n, uint32_t *outNumBits)
{
  uint32_t div, mod, de, numBits, value;
  div = n / m;
  if (div >= MAX_PREFIX_16) {
    numBits = MAX_PREFIX_16 + MAX_DATATYPE_BITS_16;
    value = (((1 << MAX_PREFIX_16) - 1) << MAX_DATATYPE_BITS_16) + n;
  } else {
    mod = n % m;
    de = (mod == 0);
    numBits = div + k + 1 - de;
    value = (((1 << div) - 1) << (numBits - div)) + mod + 1 - de;
    if (numBits > MAX_PREFIX_16 + MAX_DATATYPE_BITS_16) {
      numBits = MAX_PREFIX_16 + MAX_DATATYPE_BITS_16;
      value = (((1 << MAX_PREFIX_16) - 1) << MAX_DATATYPE_BITS_16) + n;
    }
  }
  *outNumBits = numBits;
  return (int32_t)value;
}

static inline int dyn_code_32bit(int32_t maxbits, uint32_t m, uint32_t k, uint32_t n, uint32_t *outNumBits,
                                 uint32_t *outValue, uint32_t *overflow, uint32_t *overflowbits)
{
  uint32_t div, mod, de, numBits, value;
  int didOverflow = 0;
  div = n / m;
  if (div < MAX_PREFIX_32) {
    mod = n - (m * div);
    de = (mod == 0);
    numBits = div + k + 1 - de;
    value = (((1 << div) - 1) << (numBits - div)) + mod + 1 - de;
    if (numBits > 25)
      goto codeasescape;
  } else {
  codeasescape:
    numBits = MAX_PREFIX_32;
    value = ((1 << MAX_PREFIX_32) - 1);
    *overflow = n;
    *overflowbits = maxbits;
    didOverflow = 1;
  }
  *outNumBits = numBits;
  *outValue = value;
  return didOverflow;
}

static inline void dyn_jam_noDeref(uint8_t *out, uint32_t bitPos, uint32_t numBits, uint32_t value)
{
  uint32_t *i = (uint32_t *)(out + (bitPos >> 3));
  uint32_t mask, curr, shift;
  curr = bswap32(*i);
  shift = 32 - (bitPos & 7) - numBits;
  mask = ~0u >> (32 - numBits);
  mask <<= shift;
  value = (value << shift) & mask;
  value |= curr & ~mask;
  *i = bswap32(value);
}

static inline void dyn_jam_noDeref_large(uint8_t *out, uint32_t bitPos, uint32_t numBits, uint32_t value)
{
  uint32_t *i = (uint32_t *)(out + (bitPos >> 3));
  uint32_t w, curr, mask;
  int32_t shiftvalue = (32 - (bitPos & 7) - numBits);
  curr = bswap32(*i);
  if (shiftvalue < 0) {
    uint8_t *tailptr;
    w = value >> -shiftvalue;
    mask = ~0u >> -shiftvalue;
    w |= (curr & ~mask);
    tailptr = ((uint8_t *)i) + 4;
    *tailptr = (uint8_t)((value << (8 + shiftvalue)) & 0xff);
  } else {
    mask = ~0u >> (32 - numBits);
    mask <<= shiftvalue;
    w = (value << shiftvalue) & mask;
    w |= curr & ~mask;
  }
  *i = bswap32(w);
}

static int dyn_comp(AGParamRec *params, int32_t *pc, BitBuffer *bitstream, int32_t numSamples, int32_t bitSize,
                    uint32_t *outNumBits)
{
  uint8_t *out;
  uint32_t bitPos, startPos, m, k, n, c, mz, nz, numBits, value, overflow, overflowbits;
  int32_t del, zmode, status = ALAC_OK;
  uint32_t mb, pb, kb, wb;
  int32_t rowPos = 0, rowSize = params->sw, rowJump = params->fw - rowSize;
  int32_t *inPtr = pc;

  *outNumBits = 0;
  if (bitSize < 1 || bitSize > 32)
    return ALAC_EPARAM;
  out = bitstream->cur;
  startPos = bitstream->bitIndex;
  bitPos = startPos;
  mb = params->mb = params->mb0;
  pb = params->pb;
  kb = params->kb;
  wb = params->wb;
  zmode = 0;
  c = 0;
  while (c < (uint32_t)numSamples) {
    m = mb >> QBSHIFT;
    k = lg3a(m);
    if (k > kb)
      k = kb;
    m = (1 << k) - 1;
    del = *inPtr++;
    rowPos++;
    n = (abs_func(del) << 1) - ((del >> 31) & 1) - zmode;
    if (dyn_code_32bit(bitSize, m, k, n, &numBits, &value, &overflow, &overflowbits)) {
      dyn_jam_noDeref(out, bitPos, numBits, value);
      bitPos += numBits;
      dyn_jam_noDeref_large(out, bitPos, overflowbits, overflow);
      bitPos += overflowbits;
    } else {
      dyn_jam_noDeref(out, bitPos, numBits, value);
      bitPos += numBits;
    }
    c++;
    if (rowPos >= rowSize) {
      rowPos = 0;
      inPtr += rowJump;
    }
    mb = pb * (n + zmode) + mb - ((pb * mb) >> QBSHIFT);
    if (n > N_MAX_MEAN_CLAMP)
      mb = N_MEAN_CLAMP_VAL;
    zmode = 0;
    if (c > (uint32_t)numSamples) {
      status = ALAC_EFORMAT;
      goto Exit;
    }
    if (((mb << MMULSHIFT) < QB) && (c < (uint32_t)numSamples)) {
      zmode = 1;
      nz = 0;
      while (c < (uint32_t)numSamples && *inPtr == 0) {
        ++inPtr;
        ++nz;
        ++c;
        if (++rowPos >= rowSize) {
          rowPos = 0;
          inPtr += rowJump;
        }
        if (nz >= 65535) {
          zmode = 0;
          break;
        }
      }
      k = lead(mb) - BITOFF + ((mb + MOFF) >> MDENSHIFT);
      mz = ((1 << k) - 1) & wb;
      value = dyn_code(mz, k, nz, &numBits);
      dyn_jam_noDeref(out, bitPos, numBits, value);
      bitPos += numBits;
      mb = 0;
    }
  }
  *outNumBits = (bitPos - startPos);
  BitBufferAdvance(bitstream, *outNumBits);
Exit:
  return status;
}

/* ============================ magic cookie =============================== */
static uint32_t rd_be32(const uint8_t *p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint16_t rd_be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void wr_be32(uint8_t *p, uint32_t v)
{
  p[0] = v >> 24;
  p[1] = v >> 16;
  p[2] = v >> 8;
  p[3] = v;
}
static void wr_be16(uint8_t *p, uint16_t v)
{
  p[0] = v >> 8;
  p[1] = (uint8_t)v;
}

void alac_config_default(alac_config *c, uint32_t sample_rate, uint8_t bit_depth, uint8_t num_channels)
{
  memset(c, 0, sizeof *c);
  c->frame_length = kALACDefaultFrameSize;
  c->bit_depth = bit_depth;
  c->num_channels = num_channels;
  c->sample_rate = sample_rate;
  c->pb = PB0;
  c->mb = MB0;
  c->kb = KB0;
  c->max_run = MAX_RUN_DEFAULT;
}

int alac_parse_cookie(const uint8_t *cookie, uint32_t len, alac_config *out)
{
  if (!cookie || !out)
    return ALAC_EPARAM;
  /* tolerate a leading 'frma'/'alac' atom wrapper (see
   * ALACMagicCookieDescription) */
  if (len >= 12 && cookie[4] == 'f' && cookie[5] == 'r' && cookie[6] == 'm' && cookie[7] == 'a') {
    cookie += 12;
    len -= 12;
  }
  if (len >= 12 && cookie[4] == 'a' && cookie[5] == 'l' && cookie[6] == 'a' && cookie[7] == 'c') {
    cookie += 12;
    len -= 12;
  }
  if (len < ALAC_COOKIE_SIZE)
    return ALAC_EFORMAT;

  out->frame_length = rd_be32(cookie + 0);
  /* cookie[4] = compatibleVersion */
  out->bit_depth = cookie[5];
  out->pb = cookie[6];
  out->mb = cookie[7];
  out->kb = cookie[8];
  out->num_channels = cookie[9];
  out->max_run = rd_be16(cookie + 10);
  out->max_frame_bytes = rd_be32(cookie + 12);
  out->avg_bit_rate = rd_be32(cookie + 16);
  out->sample_rate = rd_be32(cookie + 20);

  if (cookie[4] > 0)
    return ALAC_EFORMAT; /* compatibleVersion */
  if (out->bit_depth != 16 && out->bit_depth != 20 && out->bit_depth != 24 && out->bit_depth != 32)
    return ALAC_EFORMAT;
  if (out->num_channels < 1 || out->num_channels > 2)
    return ALAC_EFORMAT;
  if (out->frame_length == 0 || out->frame_length > (1u << 20))
    return ALAC_EFORMAT;
  if (out->kb > 31)
    return ALAC_EFORMAT; /* used as `1u << kb` in set_ag_params */
  return ALAC_OK;
}

int alac_build_cookie(const alac_config *c, uint8_t out[ALAC_COOKIE_SIZE])
{
  if (!c || !out)
    return ALAC_EPARAM;
  wr_be32(out + 0, c->frame_length);
  out[4] = 0; /* compatibleVersion */
  out[5] = c->bit_depth;
  out[6] = c->pb ? c->pb : PB0;
  out[7] = c->mb ? c->mb : MB0;
  out[8] = c->kb ? c->kb : KB0;
  out[9] = c->num_channels;
  wr_be16(out + 10, c->max_run ? c->max_run : MAX_RUN_DEFAULT);
  wr_be32(out + 12, c->max_frame_bytes);
  wr_be32(out + 16, c->avg_bit_rate);
  wr_be32(out + 20, c->sample_rate);
  return ALAC_OK;
}

uint32_t alac_bytes_per_frame(const alac_config *c)
{
  uint32_t bps = (c->bit_depth == 16) ? 2 : (c->bit_depth == 32) ? 4 : 3;
  return bps * c->num_channels;
}

uint32_t alac_max_packet_size(const alac_config *c)
{
  return c->frame_length * c->num_channels * ((10 + kMaxSampleSize) / 8) + 1;
}

/* ============================ decoder ==================================== */
struct alac_decoder {
  alac_config cfg;
  int32_t *mixU, *mixV, *predictor;
  uint16_t *shift; /* aliases predictor, as in the reference */
  uint8_t *inbuf;
  uint32_t inbuf_size;
};

alac_decoder *alac_decoder_open(const alac_config *c)
{
  alac_decoder *d;
  uint32_t fl;
  if (!c || (c->num_channels < 1) || (c->num_channels > 2))
    return NULL;
  if (c->bit_depth != 16 && c->bit_depth != 20 && c->bit_depth != 24 && c->bit_depth != 32)
    return NULL;
  /* Bound frame_length here too (not only in parse_cookie): a direct caller
   * with a huge value overflows alac_max_packet_size (frame_length*channels*n)
   * in 32-bit. */
  if (c->frame_length == 0 || c->frame_length > (1u << 20))
    return NULL;
  d = (alac_decoder *)malloc(sizeof *d);
  if (!d)
    return NULL;
  memset(d, 0, sizeof *d);
  d->cfg = *c;
  fl = c->frame_length;
  d->mixU = (int32_t *)calloc(fl, sizeof(int32_t));
  d->mixV = (int32_t *)calloc(fl, sizeof(int32_t));
  d->predictor = (int32_t *)calloc(fl, sizeof(int32_t));
  d->shift = (uint16_t *)d->predictor;
  d->inbuf_size = alac_max_packet_size(c) + 16;
  d->inbuf = (uint8_t *)malloc(d->inbuf_size);
  if (!d->mixU || !d->mixV || !d->predictor || !d->inbuf) {
    alac_decoder_close(d);
    return NULL;
  }
  return d;
}

void alac_decoder_close(alac_decoder *d)
{
  if (!d)
    return;
  free(d->mixU);
  free(d->mixV);
  free(d->predictor);
  free(d->inbuf);
  free(d);
}

int alac_decode(alac_decoder *d, const uint8_t *packet, uint32_t packet_bytes, uint8_t *pcm_out, uint32_t *out_frames)
{
  BitBuffer bits, shiftBits;
  AGParamRec agParams;
  uint8_t *sampleBuffer = pcm_out;
  uint32_t numChannels, channelIndex = 0, numSamples, i, j;
  uint32_t bits1, bits2, chanBits, shift, extraBits;
  int32_t val, status = ALAC_OK;
  int16_t coefsU[32], coefsV[32];
  uint8_t tag, headerByte, partialFrame, bytesShifted, escapeFlag;
  uint8_t mixBits, numU, numV, modeU, modeV;
  int8_t mixRes;
  uint32_t denShiftU, denShiftV, pbFactorU, pbFactorV, pb, unusedHeader;

  if (!d || !packet || !pcm_out || !out_frames)
    return ALAC_EPARAM;
  if (packet_bytes == 0 || packet_bytes + 16 > d->inbuf_size)
    return ALAC_EPARAM;

  memcpy(d->inbuf, packet, packet_bytes);
  memset(d->inbuf + packet_bytes, 0, 16);
  BitBufferInit(&bits, d->inbuf, packet_bytes);

  numChannels = d->cfg.num_channels;
  numSamples = d->cfg.frame_length;
  *out_frames = numSamples;

  while (status == ALAC_OK) {
    if (bits.cur >= bits.end) {
      status = ALAC_EFORMAT;
      goto Exit;
    }
    if (bits.error) {
      status = ALAC_EFORMAT;
      goto Exit;
    } /* a read crossed end */
    pb = d->cfg.pb;
    tag = BitBufferReadSmall(&bits, 3);
    if (tag == ID_SCE || tag == ID_LFE) {
      (void)BitBufferReadSmall(&bits, 4); /* element instance tag */
      unusedHeader = BitBufferRead(&bits, 12);
      if (unusedHeader != 0) {
        status = ALAC_EFORMAT;
        goto Exit;
      }
      headerByte = (uint8_t)BitBufferRead(&bits, 4);
      partialFrame = headerByte >> 3;
      bytesShifted = (headerByte >> 1) & 0x3u;
      if (bytesShifted == 3) {
        status = ALAC_EFORMAT;
        goto Exit;
      }
      shift = bytesShifted * 8;
      escapeFlag = headerByte & 0x1;
      chanBits = d->cfg.bit_depth - (bytesShifted * 8);
      if (chanBits == 0 || chanBits > 32) {
        status = ALAC_EFORMAT;
        goto Exit;
      }
      if (partialFrame != 0) {
        numSamples = BitBufferRead(&bits, 16) << 16;
        numSamples |= BitBufferRead(&bits, 16);
      }
      if (numSamples > d->cfg.frame_length) {
        status = ALAC_EFORMAT;
        goto Exit;
      }
      if (escapeFlag == 0) {
        mixBits = (uint8_t)BitBufferRead(&bits, 8);
        mixRes = (int8_t)BitBufferRead(&bits, 8);
        headerByte = (uint8_t)BitBufferRead(&bits, 8);
        modeU = headerByte >> 4;
        denShiftU = headerByte & 0xfu;
        headerByte = (uint8_t)BitBufferRead(&bits, 8);
        pbFactorU = headerByte >> 5;
        numU = headerByte & 0x1fu;
        for (i = 0; i < numU; i++)
          coefsU[i] = (int16_t)BitBufferRead(&bits, 16);
        if (bytesShifted != 0) {
          shiftBits = bits;
          BitBufferAdvance(&bits, (bytesShifted * 8) * numSamples);
        }
        set_ag_params(&agParams, d->cfg.mb, (pb * pbFactorU) / 4, d->cfg.kb, numSamples, numSamples, d->cfg.max_run);
        status = dyn_decomp(&agParams, &bits, d->predictor, numSamples, chanBits, &bits1);
        if (status)
          goto Exit;
        if (modeU == 0) {
          unpc_block(d->predictor, d->mixU, numSamples, coefsU, numU, chanBits, denShiftU);
        } else {
          unpc_block(d->predictor, d->predictor, numSamples, nil, 31, chanBits, 0);
          unpc_block(d->predictor, d->mixU, numSamples, coefsU, numU, chanBits, denShiftU);
        }
      } else {
        shift = 32 - chanBits;
        if (chanBits <= 16) {
          for (i = 0; i < numSamples; i++) {
            val = (int32_t)BitBufferRead(&bits, (uint8_t)chanBits);
            val = (val << shift) >> shift;
            d->mixU[i] = val;
          }
        } else {
          extraBits = chanBits - 16;
          for (i = 0; i < numSamples; i++) {
            val = (int32_t)BitBufferRead(&bits, 16);
            val = (val << 16) >> shift;
            d->mixU[i] = val | BitBufferRead(&bits, (uint8_t)extraBits);
          }
        }
        mixBits = 0;
        mixRes = 0;
        bytesShifted = 0;
      }
      if (bytesShifted != 0) {
        shift = bytesShifted * 8;
        for (i = 0; i < numSamples; i++)
          d->shift[i] = (uint16_t)BitBufferRead(&shiftBits, (uint8_t)shift);
      }
      switch (d->cfg.bit_depth) {
      case 16: {
        int16_t *out16 = &((int16_t *)sampleBuffer)[channelIndex];
        for (i = 0, j = 0; i < numSamples; i++, j += numChannels)
          out16[j] = (int16_t)d->mixU[i];
        break;
      }
      case 20:
        copyPredictorTo20(d->mixU, sampleBuffer + channelIndex * 3, numChannels, numSamples);
        break;
      case 24:
        if (bytesShifted != 0)
          copyPredictorTo24Shift(d->mixU, d->shift, sampleBuffer + channelIndex * 3, numChannels, numSamples,
                                 bytesShifted);
        else
          copyPredictorTo24(d->mixU, sampleBuffer + channelIndex * 3, numChannels, numSamples);
        break;
      case 32: {
        int32_t *out32 = &((int32_t *)sampleBuffer)[channelIndex];
        if (bytesShifted != 0)
          copyPredictorTo32Shift(d->mixU, d->shift, out32, numChannels, numSamples, bytesShifted);
        else
          copyPredictorTo32(d->mixU, out32, numChannels, numSamples);
        break;
      }
      }
      channelIndex += 1;
      *out_frames = numSamples;
    } else if (tag == ID_CPE) {
      if ((channelIndex + 2) > numChannels)
        break;
      (void)BitBufferReadSmall(&bits, 4);
      unusedHeader = BitBufferRead(&bits, 12);
      if (unusedHeader != 0) {
        status = ALAC_EFORMAT;
        goto Exit;
      }
      headerByte = (uint8_t)BitBufferRead(&bits, 4);
      partialFrame = headerByte >> 3;
      bytesShifted = (headerByte >> 1) & 0x3u;
      if (bytesShifted == 3) {
        status = ALAC_EFORMAT;
        goto Exit;
      }
      shift = bytesShifted * 8;
      escapeFlag = headerByte & 0x1;
      chanBits = d->cfg.bit_depth - (bytesShifted * 8) + 1;
      if (chanBits == 0 || chanBits > 32) {
        status = ALAC_EFORMAT;
        goto Exit;
      }
      if (partialFrame != 0) {
        numSamples = BitBufferRead(&bits, 16) << 16;
        numSamples |= BitBufferRead(&bits, 16);
      }
      if (numSamples > d->cfg.frame_length) {
        status = ALAC_EFORMAT;
        goto Exit;
      }
      if (escapeFlag == 0) {
        mixBits = (uint8_t)BitBufferRead(&bits, 8);
        mixRes = (int8_t)BitBufferRead(&bits, 8);
        headerByte = (uint8_t)BitBufferRead(&bits, 8);
        modeU = headerByte >> 4;
        denShiftU = headerByte & 0xfu;
        headerByte = (uint8_t)BitBufferRead(&bits, 8);
        pbFactorU = headerByte >> 5;
        numU = headerByte & 0x1fu;
        for (i = 0; i < numU; i++)
          coefsU[i] = (int16_t)BitBufferRead(&bits, 16);
        headerByte = (uint8_t)BitBufferRead(&bits, 8);
        modeV = headerByte >> 4;
        denShiftV = headerByte & 0xfu;
        headerByte = (uint8_t)BitBufferRead(&bits, 8);
        pbFactorV = headerByte >> 5;
        numV = headerByte & 0x1fu;
        for (i = 0; i < numV; i++)
          coefsV[i] = (int16_t)BitBufferRead(&bits, 16);
        if (bytesShifted != 0) {
          shiftBits = bits;
          BitBufferAdvance(&bits, (bytesShifted * 8) * 2 * numSamples);
        }
        set_ag_params(&agParams, d->cfg.mb, (pb * pbFactorU) / 4, d->cfg.kb, numSamples, numSamples, d->cfg.max_run);
        status = dyn_decomp(&agParams, &bits, d->predictor, numSamples, chanBits, &bits1);
        if (status)
          goto Exit;
        if (modeU == 0) {
          unpc_block(d->predictor, d->mixU, numSamples, coefsU, numU, chanBits, denShiftU);
        } else {
          unpc_block(d->predictor, d->predictor, numSamples, nil, 31, chanBits, 0);
          unpc_block(d->predictor, d->mixU, numSamples, coefsU, numU, chanBits, denShiftU);
        }
        set_ag_params(&agParams, d->cfg.mb, (pb * pbFactorV) / 4, d->cfg.kb, numSamples, numSamples, d->cfg.max_run);
        status = dyn_decomp(&agParams, &bits, d->predictor, numSamples, chanBits, &bits2);
        if (status)
          goto Exit;
        if (modeV == 0) {
          unpc_block(d->predictor, d->mixV, numSamples, coefsV, numV, chanBits, denShiftV);
        } else {
          unpc_block(d->predictor, d->predictor, numSamples, nil, 31, chanBits, 0);
          unpc_block(d->predictor, d->mixV, numSamples, coefsV, numV, chanBits, denShiftV);
        }
      } else {
        chanBits = d->cfg.bit_depth;
        shift = 32 - chanBits;
        if (chanBits <= 16) {
          for (i = 0; i < numSamples; i++) {
            val = (int32_t)BitBufferRead(&bits, (uint8_t)chanBits);
            val = (val << shift) >> shift;
            d->mixU[i] = val;
            val = (int32_t)BitBufferRead(&bits, (uint8_t)chanBits);
            val = (val << shift) >> shift;
            d->mixV[i] = val;
          }
        } else {
          extraBits = chanBits - 16;
          for (i = 0; i < numSamples; i++) {
            val = (int32_t)BitBufferRead(&bits, 16);
            val = (val << 16) >> shift;
            d->mixU[i] = val | BitBufferRead(&bits, (uint8_t)extraBits);
            val = (int32_t)BitBufferRead(&bits, 16);
            val = (val << 16) >> shift;
            d->mixV[i] = val | BitBufferRead(&bits, (uint8_t)extraBits);
          }
        }
        mixBits = 0;
        mixRes = 0;
        bytesShifted = 0;
      }
      if (bytesShifted != 0) {
        shift = bytesShifted * 8;
        for (i = 0; i < (numSamples * 2); i += 2) {
          d->shift[i + 0] = (uint16_t)BitBufferRead(&shiftBits, (uint8_t)shift);
          d->shift[i + 1] = (uint16_t)BitBufferRead(&shiftBits, (uint8_t)shift);
        }
      }
      switch (d->cfg.bit_depth) {
      case 16:
        unmix16(d->mixU, d->mixV, &((int16_t *)sampleBuffer)[channelIndex], numChannels, numSamples, mixBits, mixRes);
        break;
      case 20:
        unmix20(d->mixU, d->mixV, sampleBuffer + channelIndex * 3, numChannels, numSamples, mixBits, mixRes);
        break;
      case 24:
        unmix24(d->mixU, d->mixV, sampleBuffer + channelIndex * 3, numChannels, numSamples, mixBits, mixRes, d->shift,
                bytesShifted);
        break;
      case 32:
        unmix32(d->mixU, d->mixV, &((int32_t *)sampleBuffer)[channelIndex], numChannels, numSamples, mixBits, mixRes,
                d->shift, bytesShifted);
        break;
      }
      channelIndex += 2;
      *out_frames = numSamples;
    } else if (tag == ID_FIL) {
      /* fill element -- 4-bit (or 4+8) count of bytes to skip */
      int16_t count = BitBufferReadSmall(&bits, 4);
      if (count == 15)
        count += (int16_t)BitBufferReadSmall(&bits, 8) - 1;
      BitBufferAdvance(&bits, count * 8);
      if (bits.cur > bits.end) {
        status = ALAC_EFORMAT;
        goto Exit;
      }
    } else if (tag == ID_DSE) {
      uint16_t count;
      (void)BitBufferReadSmall(&bits, 4);
      int align = BitBufferReadOne(&bits);
      count = BitBufferReadSmall(&bits, 8);
      if (count == 255)
        count += BitBufferReadSmall(&bits, 8);
      if (align)
        BitBufferByteAlign(&bits, 0);
      BitBufferAdvance(&bits, count * 8);
      if (bits.cur > bits.end) {
        status = ALAC_EFORMAT;
        goto Exit;
      }
    } else if (tag == ID_END) {
      BitBufferByteAlign(&bits, 0);
      goto Exit;
    } else {
      status = ALAC_EFORMAT;
      goto Exit;
    }
    if (channelIndex >= numChannels)
      break;
  }
Exit:
  return status;
}

/* ============================ encoder ==================================== */
struct alac_encoder {
  alac_config cfg;
  uint32_t frameSize, bitDepth, numChannels, maxOutputBytes;
  int32_t *mixU, *mixV, *predU, *predV;
  uint16_t *shiftUV;
  uint8_t *work, *out;
  int16_t coefsU[kALACMaxChannels][kALACMaxSearches][kALACMaxCoefs];
  int16_t coefsV[kALACMaxChannels][kALACMaxSearches][kALACMaxCoefs];
  int16_t lastMixRes[kALACMaxChannels];
};

alac_encoder *alac_encoder_open(const alac_config *c)
{
  alac_encoder *e;
  uint32_t fl;
  int ch, sr;
  if (!c || c->num_channels < 1 || c->num_channels > 2)
    return NULL;
  if (c->bit_depth != 16 && c->bit_depth != 20 && c->bit_depth != 24 && c->bit_depth != 32)
    return NULL;
  e = (alac_encoder *)malloc(sizeof *e);
  if (!e)
    return NULL;
  memset(e, 0, sizeof *e);
  e->cfg = *c;
  e->frameSize = fl = c->frame_length;
  e->bitDepth = c->bit_depth;
  e->numChannels = c->num_channels;
  e->maxOutputBytes = alac_max_packet_size(c);
  e->mixU = (int32_t *)calloc(fl, sizeof(int32_t));
  e->mixV = (int32_t *)calloc(fl, sizeof(int32_t));
  e->predU = (int32_t *)calloc(fl, sizeof(int32_t));
  e->predV = (int32_t *)calloc(fl, sizeof(int32_t));
  e->shiftUV = (uint16_t *)calloc(fl * 2, sizeof(uint16_t));
  e->work = (uint8_t *)calloc(e->maxOutputBytes + 8, 1);
  e->out = (uint8_t *)calloc(e->maxOutputBytes + 8, 1);
  if (!e->mixU || !e->mixV || !e->predU || !e->predV || !e->shiftUV || !e->work || !e->out) {
    alac_encoder_close(e);
    return NULL;
  }
  for (ch = 0; ch < kALACMaxChannels; ch++) {
    for (sr = 0; sr < kALACMaxSearches; sr++) {
      init_coefs(e->coefsU[ch][sr], DENSHIFT_DEFAULT, kALACMaxCoefs);
      init_coefs(e->coefsV[ch][sr], DENSHIFT_DEFAULT, kALACMaxCoefs);
    }
    e->lastMixRes[ch] = kDefaultMixRes;
  }
  return e;
}

void alac_encoder_close(alac_encoder *e)
{
  if (!e)
    return;
  free(e->mixU);
  free(e->mixV);
  free(e->predU);
  free(e->predV);
  free(e->shiftUV);
  free(e->work);
  free(e->out);
  free(e);
}

static int encodeStereoEscape(alac_encoder *e, BitBuffer *bitstream, void *inputBuffer, uint32_t stride,
                              uint32_t numSamples)
{
  uint32_t index;
  uint8_t partialFrame = (numSamples == e->frameSize) ? 0 : 1;
  BitBufferWrite(bitstream, 0, 12);
  BitBufferWrite(bitstream, (partialFrame << 3) | 1, 4);
  if (partialFrame)
    BitBufferWrite(bitstream, numSamples, 32);
  switch (e->bitDepth) {
  case 16: {
    int16_t *in16 = (int16_t *)inputBuffer;
    for (index = 0; index < numSamples * stride; index += stride) {
      BitBufferWrite(bitstream, (uint16_t)in16[index + 0], 16);
      BitBufferWrite(bitstream, (uint16_t)in16[index + 1], 16);
    }
    break;
  }
  case 20:
    mix20((uint8_t *)inputBuffer, stride, e->mixU, e->mixV, numSamples, 0, 0);
    for (index = 0; index < numSamples; index++) {
      BitBufferWrite(bitstream, e->mixU[index], 20);
      BitBufferWrite(bitstream, e->mixV[index], 20);
    }
    break;
  case 24:
    mix24((uint8_t *)inputBuffer, stride, e->mixU, e->mixV, numSamples, 0, 0, e->shiftUV, 0);
    for (index = 0; index < numSamples; index++) {
      BitBufferWrite(bitstream, e->mixU[index], 24);
      BitBufferWrite(bitstream, e->mixV[index], 24);
    }
    break;
  case 32: {
    int32_t *in32 = (int32_t *)inputBuffer;
    for (index = 0; index < numSamples * stride; index += stride) {
      BitBufferWrite(bitstream, in32[index + 0], 32);
      BitBufferWrite(bitstream, in32[index + 1], 32);
    }
    break;
  }
  }
  return ALAC_OK;
}

static int encodeStereo(alac_encoder *e, BitBuffer *bitstream, void *inputBuffer, uint32_t stride,
                        uint32_t channelIndex, uint32_t numSamples)
{
  BitBuffer workBits, startBits = *bitstream;
  AGParamRec agParams;
  uint32_t bits1, bits2, dilate, minBits, minBits1, minBits2, numU, numV, mode, pbFactor, chanBits, index, escapeBits;
  int32_t mixBits, mixRes, maxRes, bestRes;
  uint8_t bytesShifted, partialFrame;
  int doEscape, status = ALAC_OK;
  int16_t(*coefsU)[kALACMaxCoefs] = e->coefsU[channelIndex];
  int16_t(*coefsV)[kALACMaxCoefs] = e->coefsV[channelIndex];

  if (e->bitDepth == 32)
    bytesShifted = 2;
  else if (e->bitDepth >= 24)
    bytesShifted = 1;
  else
    bytesShifted = 0;
  chanBits = e->bitDepth - (bytesShifted * 8) + 1;
  partialFrame = (numSamples == e->frameSize) ? 0 : 1;

  mixBits = kDefaultMixBits;
  maxRes = kMaxRes;
  numU = numV = kDefaultNumUV;
  mode = 0;
  pbFactor = 4;
  dilate = 8;
  minBits = minBits1 = minBits2 = 0x80000000u;
  bestRes = e->lastMixRes[channelIndex];

  for (mixRes = 0; mixRes <= maxRes; mixRes++) {
    switch (e->bitDepth) {
    case 16:
      mix16((int16_t *)inputBuffer, stride, e->mixU, e->mixV, numSamples / dilate, mixBits, mixRes);
      break;
    case 20:
      mix20((uint8_t *)inputBuffer, stride, e->mixU, e->mixV, numSamples / dilate, mixBits, mixRes);
      break;
    case 24:
      mix24((uint8_t *)inputBuffer, stride, e->mixU, e->mixV, numSamples / dilate, mixBits, mixRes, e->shiftUV,
            bytesShifted);
      break;
    case 32:
      mix32((int32_t *)inputBuffer, stride, e->mixU, e->mixV, numSamples / dilate, mixBits, mixRes, e->shiftUV,
            bytesShifted);
      break;
    }
    BitBufferInit(&workBits, e->work, e->maxOutputBytes);
    pc_block(e->mixU, e->predU, numSamples / dilate, coefsU[numU - 1], numU, chanBits, DENSHIFT_DEFAULT);
    pc_block(e->mixV, e->predV, numSamples / dilate, coefsV[numV - 1], numV, chanBits, DENSHIFT_DEFAULT);
    set_ag_params(&agParams, MB0, (pbFactor * PB0) / 4, KB0, numSamples / dilate, numSamples / dilate, MAX_RUN_DEFAULT);
    status = dyn_comp(&agParams, e->predU, &workBits, numSamples / dilate, chanBits, &bits1);
    if (status)
      goto Exit;
    set_ag_params(&agParams, MB0, (pbFactor * PB0) / 4, KB0, numSamples / dilate, numSamples / dilate, MAX_RUN_DEFAULT);
    status = dyn_comp(&agParams, e->predV, &workBits, numSamples / dilate, chanBits, &bits2);
    if (status)
      goto Exit;
    if ((bits1 + bits2) < minBits1) {
      minBits1 = bits1 + bits2;
      bestRes = mixRes;
    }
  }
  e->lastMixRes[channelIndex] = (int16_t)bestRes;

  mixRes = e->lastMixRes[channelIndex];
  switch (e->bitDepth) {
  case 16:
    mix16((int16_t *)inputBuffer, stride, e->mixU, e->mixV, numSamples, mixBits, mixRes);
    break;
  case 20:
    mix20((uint8_t *)inputBuffer, stride, e->mixU, e->mixV, numSamples, mixBits, mixRes);
    break;
  case 24:
    mix24((uint8_t *)inputBuffer, stride, e->mixU, e->mixV, numSamples, mixBits, mixRes, e->shiftUV, bytesShifted);
    break;
  case 32:
    mix32((int32_t *)inputBuffer, stride, e->mixU, e->mixV, numSamples, mixBits, mixRes, e->shiftUV, bytesShifted);
    break;
  }

  numU = numV = kMinUV;
  minBits1 = minBits2 = 0x80000000u;
  {
    uint32_t numUV;
    for (numUV = kMinUV; numUV <= kMaxUV; numUV += 4) {
      uint32_t converge;
      BitBufferInit(&workBits, e->work, e->maxOutputBytes);
      dilate = 32;
      for (converge = 0; converge < 8; converge++) {
        pc_block(e->mixU, e->predU, numSamples / dilate, coefsU[numUV - 1], numUV, chanBits, DENSHIFT_DEFAULT);
        pc_block(e->mixV, e->predV, numSamples / dilate, coefsV[numUV - 1], numUV, chanBits, DENSHIFT_DEFAULT);
      }
      dilate = 8;
      set_ag_params(&agParams, MB0, (pbFactor * PB0) / 4, KB0, numSamples / dilate, numSamples / dilate,
                    MAX_RUN_DEFAULT);
      status = dyn_comp(&agParams, e->predU, &workBits, numSamples / dilate, chanBits, &bits1);
      if ((bits1 * dilate + 16 * numUV) < minBits1) {
        minBits1 = bits1 * dilate + 16 * numUV;
        numU = numUV;
      }
      set_ag_params(&agParams, MB0, (pbFactor * PB0) / 4, KB0, numSamples / dilate, numSamples / dilate,
                    MAX_RUN_DEFAULT);
      status = dyn_comp(&agParams, e->predV, &workBits, numSamples / dilate, chanBits, &bits2);
      if ((bits2 * dilate + 16 * numUV) < minBits2) {
        minBits2 = bits2 * dilate + 16 * numUV;
        numV = numUV;
      }
    }
  }

  minBits = minBits1 + minBits2 + (8 * 8) + (partialFrame ? 32 : 0);
  if (bytesShifted != 0)
    minBits += numSamples * (bytesShifted * 8) * 2;
  escapeBits = (numSamples * e->bitDepth * 2) + (partialFrame ? 32 : 0) + (2 * 8);
  doEscape = (minBits >= escapeBits);

  if (!doEscape) {
    BitBufferWrite(bitstream, 0, 12);
    BitBufferWrite(bitstream, (partialFrame << 3) | (bytesShifted << 1), 4);
    if (partialFrame)
      BitBufferWrite(bitstream, numSamples, 32);
    BitBufferWrite(bitstream, mixBits, 8);
    BitBufferWrite(bitstream, mixRes, 8);
    BitBufferWrite(bitstream, (mode << 4) | DENSHIFT_DEFAULT, 8);
    BitBufferWrite(bitstream, (pbFactor << 5) | numU, 8);
    for (index = 0; index < numU; index++)
      BitBufferWrite(bitstream, (uint16_t)coefsU[numU - 1][index], 16);
    BitBufferWrite(bitstream, (mode << 4) | DENSHIFT_DEFAULT, 8);
    BitBufferWrite(bitstream, (pbFactor << 5) | numV, 8);
    for (index = 0; index < numV; index++)
      BitBufferWrite(bitstream, (uint16_t)coefsV[numV - 1][index], 16);
    if (bytesShifted != 0) {
      uint32_t bitShift = bytesShifted * 8;
      for (index = 0; index < numSamples * 2; index += 2) {
        uint32_t sv = ((uint32_t)e->shiftUV[index + 0] << bitShift) | (uint32_t)e->shiftUV[index + 1];
        BitBufferWrite(bitstream, sv, bitShift * 2);
      }
    }
    pc_block(e->mixU, e->predU, numSamples, coefsU[numU - 1], numU, chanBits, DENSHIFT_DEFAULT);
    set_ag_params(&agParams, MB0, (pbFactor * PB0) / 4, KB0, numSamples, numSamples, MAX_RUN_DEFAULT);
    status = dyn_comp(&agParams, e->predU, bitstream, numSamples, chanBits, &bits1);
    if (status)
      goto Exit;
    pc_block(e->mixV, e->predV, numSamples, coefsV[numV - 1], numV, chanBits, DENSHIFT_DEFAULT);
    set_ag_params(&agParams, MB0, (pbFactor * PB0) / 4, KB0, numSamples, numSamples, MAX_RUN_DEFAULT);
    status = dyn_comp(&agParams, e->predV, bitstream, numSamples, chanBits, &bits2);
    if (status)
      goto Exit;
    minBits = BitBufferGetPosition(bitstream) - BitBufferGetPosition(&startBits);
    if (minBits >= escapeBits) {
      *bitstream = startBits;
      doEscape = 1;
    }
  }
  if (doEscape)
    status = encodeStereoEscape(e, bitstream, inputBuffer, stride, numSamples);
Exit:
  return status;
}

static int encodeMono(alac_encoder *e, BitBuffer *bitstream, void *inputBuffer, uint32_t stride, uint32_t channelIndex,
                      uint32_t numSamples)
{
  BitBuffer startBits = *bitstream;
  AGParamRec agParams;
  uint32_t bits1, numU, dilate, minBits, bestU, minU, maxU, index, index2, chanBits, shift, mask, escapeBits;
  uint8_t bytesShifted, pbFactor, partialFrame;
  int16_t *in16;
  int32_t *in32;
  int doEscape, status;
  int16_t(*coefsU)[kALACMaxCoefs] = e->coefsU[channelIndex];

  status = ALAC_OK;
  if (e->bitDepth == 32)
    bytesShifted = 2;
  else if (e->bitDepth >= 24)
    bytesShifted = 1;
  else
    bytesShifted = 0;
  shift = bytesShifted * 8;
  mask = (1u << shift) - 1;
  chanBits = e->bitDepth - (bytesShifted * 8);
  partialFrame = (numSamples == e->frameSize) ? 0 : 1;

  switch (e->bitDepth) {
  case 16:
    in16 = (int16_t *)inputBuffer;
    for (index = 0, index2 = 0; index < numSamples; index++, index2 += stride)
      e->mixU[index] = in16[index2];
    break;
  case 20:
    copy20ToPredictor((uint8_t *)inputBuffer, stride, e->mixU, numSamples);
    break;
  case 24:
    copy24ToPredictor((uint8_t *)inputBuffer, stride, e->mixU, numSamples);
    for (index = 0; index < numSamples; index++) {
      e->shiftUV[index] = (uint16_t)(e->mixU[index] & mask);
      e->mixU[index] >>= shift;
    }
    break;
  case 32:
    in32 = (int32_t *)inputBuffer;
    for (index = 0, index2 = 0; index < numSamples; index++, index2 += stride) {
      int32_t val = in32[index2];
      e->shiftUV[index] = (uint16_t)(val & mask);
      e->mixU[index] = val >> shift;
    }
    break;
  }

  minU = 4;
  maxU = 8;
  pbFactor = 4;
  minBits = 0x80000000u;
  bestU = minU;
  for (numU = minU; numU <= maxU; numU += 4) {
    BitBuffer workBits;
    uint32_t numBits, converge;
    BitBufferInit(&workBits, e->work, e->maxOutputBytes);
    dilate = 32;
    for (converge = 0; converge < 7; converge++)
      pc_block(e->mixU, e->predU, numSamples / dilate, coefsU[numU - 1], numU, chanBits, DENSHIFT_DEFAULT);
    dilate = 8;
    pc_block(e->mixU, e->predU, numSamples / dilate, coefsU[numU - 1], numU, chanBits, DENSHIFT_DEFAULT);
    set_ag_params(&agParams, MB0, (pbFactor * PB0) / 4, KB0, numSamples / dilate, numSamples / dilate, MAX_RUN_DEFAULT);
    status = dyn_comp(&agParams, e->predU, &workBits, numSamples / dilate, chanBits, &bits1);
    if (status)
      goto Exit;
    numBits = (dilate * bits1) + (16 * numU);
    if (numBits < minBits) {
      bestU = numU;
      minBits = numBits;
    }
  }

  minBits += (4 * 8) + (partialFrame ? 32 : 0);
  if (bytesShifted != 0)
    minBits += numSamples * (bytesShifted * 8);
  escapeBits = (numSamples * e->bitDepth) + (partialFrame ? 32 : 0) + (2 * 8);
  doEscape = (minBits >= escapeBits);

  if (!doEscape) {
    BitBufferWrite(bitstream, 0, 12);
    BitBufferWrite(bitstream, (partialFrame << 3) | (bytesShifted << 1), 4);
    if (partialFrame)
      BitBufferWrite(bitstream, numSamples, 32);
    BitBufferWrite(bitstream, 0, 16); /* mixBits = mixRes = 0 */
    numU = bestU;
    BitBufferWrite(bitstream, (0 << 4) | DENSHIFT_DEFAULT, 8);
    BitBufferWrite(bitstream, (pbFactor << 5) | numU, 8);
    for (index = 0; index < numU; index++)
      BitBufferWrite(bitstream, (uint16_t)coefsU[numU - 1][index], 16);
    if (bytesShifted != 0)
      for (index = 0; index < numSamples; index++)
        BitBufferWrite(bitstream, e->shiftUV[index], shift);
    pc_block(e->mixU, e->predU, numSamples, coefsU[numU - 1], numU, chanBits, DENSHIFT_DEFAULT);
    set_ag_params(&agParams, MB0, PB0, KB0, numSamples, numSamples, MAX_RUN_DEFAULT);
    status = dyn_comp(&agParams, e->predU, bitstream, numSamples, chanBits, &bits1);
    minBits = BitBufferGetPosition(bitstream) - BitBufferGetPosition(&startBits);
    if (minBits >= escapeBits) {
      *bitstream = startBits;
      doEscape = 1;
    }
  }
  if (doEscape) {
    BitBufferWrite(bitstream, 0, 12);
    BitBufferWrite(bitstream, (partialFrame << 3) | 1, 4);
    if (partialFrame)
      BitBufferWrite(bitstream, numSamples, 32);
    switch (e->bitDepth) {
    case 16:
      in16 = (int16_t *)inputBuffer;
      for (index = 0; index < numSamples * stride; index += stride)
        BitBufferWrite(bitstream, (uint16_t)in16[index], 16);
      break;
    case 20:
      copy20ToPredictor((uint8_t *)inputBuffer, stride, e->mixU, numSamples);
      for (index = 0; index < numSamples; index++)
        BitBufferWrite(bitstream, e->mixU[index], 20);
      break;
    case 24:
      copy24ToPredictor((uint8_t *)inputBuffer, stride, e->mixU, numSamples);
      for (index = 0; index < numSamples; index++)
        BitBufferWrite(bitstream, e->mixU[index], 24);
      break;
    case 32:
      in32 = (int32_t *)inputBuffer;
      for (index = 0; index < numSamples * stride; index += stride)
        BitBufferWrite(bitstream, in32[index], 32);
      break;
    }
  }
Exit:
  return status;
}

int alac_encode(alac_encoder *e, const uint8_t *pcm, uint32_t num_frames, uint8_t *packet_out)
{
  BitBuffer bitstream;
  int status;
  uint32_t outputSize;
  if (!e || !pcm || !packet_out)
    return ALAC_EPARAM;
  if (num_frames == 0 || num_frames > e->frameSize)
    return ALAC_EPARAM;

  BitBufferInit(&bitstream, e->out, e->maxOutputBytes);
  if (e->numChannels == 2) {
    BitBufferWrite(&bitstream, ID_CPE, 3);
    BitBufferWrite(&bitstream, 0, 4);
    status = encodeStereo(e, &bitstream, (void *)pcm, 2, 0, num_frames);
  } else {
    BitBufferWrite(&bitstream, ID_SCE, 3);
    BitBufferWrite(&bitstream, 0, 4);
    status = encodeMono(e, &bitstream, (void *)pcm, 1, 0, num_frames);
  }
  if (status)
    return status;

  BitBufferWrite(&bitstream, ID_END, 3);
  BitBufferByteAlign(&bitstream, 1);
  outputSize = BitBufferGetPosition(&bitstream) / 8;
  if (outputSize > e->cfg.max_frame_bytes)
    e->cfg.max_frame_bytes = outputSize;
  memcpy(packet_out, e->out, outputSize);
  return (int)outputSize;
}
