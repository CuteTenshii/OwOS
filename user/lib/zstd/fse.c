/* fse.c: Finite State Entropy (Yann Collet's tANS variant), the entropy stage
 * under zstd's sequences and Huffman-weight descriptions.
 *
 * Decode: parse a normalized-count table description (fse_read_ncount), build a
 * decode table (fse_build_dtable), and a 2-state decompressor (fse_decompress,
 * used for Huffman weights). Encode: build a compression table from fixed
 * (predefined) normalized counts and stream symbols out. */
#include "zstd_internal.h"

#define FSE_TABLESTEP(t) (((t) >> 1) + ((t) >> 3) + 3)
#define FSE_MIN_TABLELOG 5
#define FSE_MAX_TABLELOG 15

/* Little-endian 32-bit read that never runs past `iend` (zero-pads the tail). */
static u32 rd32_safe(const u8 *ip, const u8 *iend)
{
  u32 v = 0;
  int n = (int)(iend - ip);
  if (n > 4)
    n = 4;
  for (int i = 0; i < n; i++)
    v |= (u32)ip[i] << (8 * i);
  return v;
}

int fse_read_ncount(short *normCount, unsigned *maxSVptr, unsigned *tableLogPtr, const u8 *src, u32 srcSize)
{
  const u8 *istart = src, *ip = src, *iend = src + srcSize;
  int nbBits, remaining, threshold, bitCount;
  u32 bitStream;
  unsigned charnum = 0;
  int previous0 = 0;

  if (srcSize < 4)
    return -1;
  bitStream = rd32_safe(ip, iend);
  nbBits = (int)(bitStream & 0xF) + FSE_MIN_TABLELOG;
  if (nbBits > FSE_MAX_TABLELOG)
    return -1;
  bitStream >>= 4;
  bitCount = 4;
  *tableLogPtr = (unsigned)nbBits;
  remaining = (1 << nbBits) + 1;
  threshold = 1 << nbBits;
  nbBits++;

  while ((remaining > 1) && (charnum <= 255)) {
    if (previous0) {
      unsigned n0 = charnum;
      while ((bitStream & 0xFFFF) == 0xFFFF) {
        n0 += 24;
        if (ip < iend - 5) {
          ip += 2;
          bitStream = rd32_safe(ip, iend) >> bitCount;
        } else {
          bitStream >>= 16;
          bitCount += 16;
        }
      }
      while ((bitStream & 3) == 3) {
        n0 += 3;
        bitStream >>= 2;
        bitCount += 2;
      }
      n0 += bitStream & 3;
      bitCount += 2;
      if (n0 > 255)
        return -1;
      while (charnum < n0)
        normCount[charnum++] = 0;
      if ((ip <= iend - 7) || (ip + (bitCount >> 3) <= iend - 4)) {
        ip += bitCount >> 3;
        bitCount &= 7;
      } else {
        bitCount -= (int)(8 * (iend - 4 - ip));
        ip = iend - 4;
      }
      bitStream = rd32_safe(ip, iend) >> (bitCount & 31);
    }
    {
      int max = (2 * threshold - 1) - remaining;
      int count;
      if ((int)(bitStream & (u32)(threshold - 1)) < max) {
        count = (int)(bitStream & (u32)(threshold - 1));
        bitCount += nbBits - 1;
      } else {
        count = (int)(bitStream & (u32)(2 * threshold - 1));
        if (count >= threshold)
          count -= max;
        bitCount += nbBits;
      }
      count--; /* -1 => "less than 1"     */
      remaining -= (count < 0) ? -count : count;
      if (charnum > 255)
        return -1;
      normCount[charnum++] = (short)count;
      previous0 = (count == 0);
      while (remaining < threshold) {
        nbBits--;
        threshold >>= 1;
      }
      if ((ip <= iend - 7) || (ip + (bitCount >> 3) <= iend - 4)) {
        ip += bitCount >> 3;
        bitCount &= 7;
      } else {
        bitCount -= (int)(8 * (iend - 4 - ip));
        ip = iend - 4;
      }
      bitStream = rd32_safe(ip, iend) >> (bitCount & 31);
    }
  }
  if (remaining != 1)
    return -1;
  if (bitCount > 32)
    return -1;
  *maxSVptr = charnum - 1;
  ip += (bitCount + 7) >> 3;
  return (int)(ip - istart);
}

int fse_build_dtable(fse_dstate *dt, const short *normCount, unsigned maxSV, unsigned tableLog)
{
  u32 tableSize = 1u << tableLog;
  u32 tableMask = tableSize - 1;
  u32 step = FSE_TABLESTEP(tableSize);
  u32 symbolNext[256];
  u32 highThreshold = tableSize - 1;
  u32 s, position = 0, u;

  if (tableLog > FSE_MAX_TABLELOG)
    return -1;
  for (s = 0; s <= maxSV; s++) {
    if (normCount[s] == -1) {
      dt[highThreshold--].symbol = (u8)s;
      symbolNext[s] = 1;
    } else {
      symbolNext[s] = (u32)normCount[s];
    }
  }
  for (s = 0; s <= maxSV; s++) {
    int n = normCount[s];
    for (int i = 0; i < n; i++) {
      dt[position].symbol = (u8)s;
      position = (position + step) & tableMask;
      while (position > highThreshold)
        position = (position + step) & tableMask;
    }
  }
  if (position != 0)
    return -1;
  for (u = 0; u < tableSize; u++) {
    u8 sym = dt[u].symbol;
    u32 nextState = symbolNext[sym]++;
    dt[u].nbBits = (u8)(tableLog - zstd_highbit32(nextState));
    dt[u].newState = (u16)((nextState << dt[u].nbBits) - tableSize);
  }
  return 0;
}

static inline u8 fse_dsym(u32 *state, BITin *b, const fse_dstate *dt)
{
  u32 st = *state;
  u8 sym = dt[st].symbol;
  u32 low = bitin_read(b, dt[st].nbBits);
  *state = dt[st].newState + low;
  return sym;
}

/* 2-state FSE decompress to a known-capacity buffer; returns symbol count. */
int fse_decompress(u8 *dst, u32 dstCap, const u8 *src, u32 srcSize, const fse_dstate *dt, u32 tableLog)
{
  BITin b;
  u32 s1, s2;
  u8 *op = dst, *omax = dst + dstCap;

  if (bitin_init(&b, src, srcSize))
    return -1;
  s1 = bitin_read(&b, tableLog);
  s2 = bitin_read(&b, tableLog);

  for (;;) {
    if (op > omax - 2)
      return -1;
    *op++ = fse_dsym(&s1, &b, dt);
    if (bitin_reload(&b) == BIT_OVERFLOW) {
      *op++ = fse_dsym(&s2, &b, dt);
      break;
    }
    if (op > omax - 2)
      return -1;
    *op++ = fse_dsym(&s2, &b, dt);
    if (bitin_reload(&b) == BIT_OVERFLOW) {
      *op++ = fse_dsym(&s1, &b, dt);
      break;
    }
  }
  return (int)(op - dst);
}

/* ---- encode side ---------------------------------------------------------- */
int fse_build_ctable(fse_ctable *ct, const short *normCount, unsigned maxSV, unsigned tableLog)
{
  u32 tableSize = 1u << tableLog;
  u32 tableMask = tableSize - 1;
  u32 step = FSE_TABLESTEP(tableSize);
  u8 tableSymbol[1 << 12];
  u32 cumul[256 + 1];

  if (tableLog > 12)
    return -1;
  u32 highThreshold = tableSize - 1;
  u32 s, position = 0, u, total;

  ct->tableLog = tableLog;

  cumul[0] = 0;
  for (s = 0; s <= maxSV; s++) {
    if (normCount[s] == -1) {
      cumul[s + 1] = cumul[s] + 1;
      tableSymbol[highThreshold--] = (u8)s;
    } else {
      cumul[s + 1] = cumul[s] + (u32)normCount[s];
    }
  }
  cumul[maxSV + 1] = tableSize + 1;

  for (s = 0; s <= maxSV; s++) {
    int n = normCount[s];
    if (n <= 0)
      continue; /* -1 placed at high end  */
    for (int i = 0; i < n; i++) {
      tableSymbol[position] = (u8)s;
      position = (position + step) & tableMask;
      while (position > highThreshold)
        position = (position + step) & tableMask;
    }
  }
  if (position != 0)
    return -1;

  for (u = 0; u < tableSize; u++) {
    u8 sym = tableSymbol[u];
    ct->stateTable[cumul[sym]++] = (u16)(tableSize + u);
  }

  total = 0;
  for (s = 0; s <= maxSV; s++) {
    short nc = normCount[s];
    if (nc == 0) {
      ct->symTT[s].deltaNbBits = ((tableLog + 1) << 16) - (1u << tableLog);
      ct->symTT[s].deltaFindState = 0;
    } else if (nc == -1 || nc == 1) {
      ct->symTT[s].deltaNbBits = (tableLog << 16) - (1u << tableLog);
      ct->symTT[s].deltaFindState = (int)total - 1;
      total += 1;
    } else {
      u32 maxBitsOut = tableLog - zstd_highbit32((u32)nc - 1);
      u32 minStatePlus = (u32)nc << maxBitsOut;
      ct->symTT[s].deltaNbBits = (maxBitsOut << 16) - minStatePlus;
      ct->symTT[s].deltaFindState = (int)total - nc;
      total += (u32)nc;
    }
  }
  return 0;
}

void fse_init_cstate(fse_cstate *cs, const fse_ctable *ct, unsigned symbol)
{
  fse_symtt tt = ct->symTT[symbol];
  u32 nbBitsOut = (tt.deltaNbBits + (1u << 15)) >> 16;
  cs->stateLog = ct->tableLog;
  cs->stateTable = ct->stateTable;
  cs->symTT = ct->symTT;
  cs->value = (nbBitsOut << 16) - tt.deltaNbBits;
  cs->value = ct->stateTable[(cs->value >> nbBitsOut) + (u32)tt.deltaFindState];
}

void fse_encode_symbol(BITout *bo, fse_cstate *cs, unsigned symbol)
{
  fse_symtt tt = cs->symTT[symbol];
  u32 nbBitsOut = (cs->value + tt.deltaNbBits) >> 16;
  bitout_add(bo, cs->value, nbBitsOut);
  cs->value = cs->stateTable[(cs->value >> nbBitsOut) + (u32)tt.deltaFindState];
}

void fse_flush_cstate(BITout *bo, fse_cstate *cs)
{
  bitout_add(bo, cs->value, cs->stateLog);
  bitout_flush(bo);
}
