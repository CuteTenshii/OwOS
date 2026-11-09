/* huf.c: Huffman entropy coder for zstd literals.
 *
 * Decode: parse a tree description (direct 4-bit weights or an FSE-compressed
 * weight stream), build a single-symbol decode table, decompress 1- or 4-stream
 * literal blocks. Encode: build length-limited canonical codes from a byte
 * histogram and emit a direct-weight tree description plus the coded streams.
 *
 * The weight<->code relationship is the RFC 8878 canonical one: a symbol of
 * weight w gets nbBits = tableLog+1-w and occupies 2^(w-1) decode-table slots. */
#include "zstd_internal.h"

#define HUF_ENC_MAX_BITS 11 /* zstd literal Huffman table log ceiling    */

/* ---- decode: tree description --------------------------------------------- */
/* Build the decode table from a weight list (weightList[0..nbSymbols-1]). */
static int huf_build_from_weights(huf_dtable *dt, const u8 *weightList, u32 nbSymbols)
{
  u32 rankCount[HUF_MAX_TABLELOG + 2];
  u32 rankStart[HUF_MAX_TABLELOG + 2];
  u32 weightTotal = 0, tableLog, s;

  memset(rankCount, 0, sizeof(rankCount));
  for (s = 0; s < nbSymbols; s++) {
    if (weightList[s] > HUF_MAX_TABLELOG)
      return -1;
    if (weightList[s] > 0)
      weightTotal += 1u << (weightList[s] - 1);
  }
  if (weightTotal == 0)
    return -1;
  tableLog = zstd_highbit32(weightTotal) + 1;
  if (tableLog > HUF_MAX_TABLELOG)
    return -1;
  {
    u32 total = 1u << tableLog;
    u32 leftover = total - weightTotal;
    /* leftover must be a clean power of two -> the implied last weight. */
    if (leftover & (leftover - 1))
      return -1;
    ((u8 *)weightList)[nbSymbols] = (u8)(zstd_highbit32(leftover) + 1);
    nbSymbols++;
  }
  dt->tableLog = tableLog;

  for (s = 0; s < nbSymbols; s++)
    rankCount[weightList[s]]++;
  {
    u32 nextRankStart = 0, w;
    for (w = 1; w <= tableLog; w++) {
      rankStart[w] = nextRankStart;
      nextRankStart += rankCount[w] << (w - 1);
    }
    if (nextRankStart != (1u << tableLog))
      return -1;
  }
  for (s = 0; s < nbSymbols; s++) {
    u32 w = weightList[s];
    if (w == 0)
      continue;
    u32 length = 1u << (w - 1);
    u8 nbBits = (u8)(tableLog + 1 - w);
    u32 start = rankStart[w];
    for (u32 i = 0; i < length; i++) {
      dt->table[start + i].nbBits = nbBits;
      dt->table[start + i].symbol = (u8)s;
    }
    rankStart[w] += length;
  }
  return 0;
}

int huf_read_dtable(huf_dtable *dt, const u8 *src, u32 srcSize)
{
  u8 weightList[256 + 1];
  u32 nbSymbols = 0;
  u32 consumed;

  if (srcSize == 0)
    return -1;
  u8 headerByte = src[0];
  if (headerByte >= 128) {
    /* direct: (headerByte-127) weights, 4 bits each, big nibble first. */
    nbSymbols = headerByte - 127;
    u32 nbBytes = (nbSymbols + 1) / 2;
    if (1 + nbBytes > srcSize)
      return -1;
    for (u32 i = 0; i < nbSymbols; i++) {
      u8 b = src[1 + i / 2];
      weightList[i] = (i & 1) ? (b & 0x0F) : (b >> 4);
    }
    consumed = 1 + nbBytes;
  } else {
    /* FSE-compressed weights. */
    u32 iSize = headerByte;
    if (1 + iSize > srcSize)
      return -1;
    short norm[256];
    unsigned maxSV = 0, tableLog = 0;
    fse_dstate wdt[1 << 6];
    int nc = fse_read_ncount(norm, &maxSV, &tableLog, src + 1, iSize);
    if (nc < 0 || tableLog > 6)
      return -1;
    if (fse_build_dtable(wdt, norm, maxSV, tableLog) < 0)
      return -1;
    int n = fse_decompress(weightList, 256, src + 1 + nc, iSize - nc, wdt, tableLog);
    if (n < 0)
      return -1;
    nbSymbols = (u32)n;
    consumed = 1 + iSize;
  }
  if (huf_build_from_weights(dt, weightList, nbSymbols) < 0)
    return -1;
  return (int)consumed;
}

/* ---- decode: streams ------------------------------------------------------ */
int huf_decompress1x(u8 *dst, u32 dstSize, const u8 *src, u32 srcSize, const huf_dtable *dt)
{
  BITin b;
  u32 tl = dt->tableLog;
  if (bitin_init(&b, src, srcSize))
    return -1;
  for (u32 i = 0; i < dstSize; i++) {
    u32 idx = bitin_look(&b, tl);
    dst[i] = dt->table[idx].symbol;
    bitin_skip(&b, dt->table[idx].nbBits);
    if (b.bitsConsumed > 64)
      return -1;
    bitin_reload(&b);
  }
  return 0;
}

int huf_decompress4x(u8 *dst, u32 dstSize, const u8 *src, u32 srcSize, const huf_dtable *dt)
{
  if (srcSize < 6)
    return -1;
  u32 len1 = zstd_rd16(src);
  u32 len2 = zstd_rd16(src + 2);
  u32 len3 = zstd_rd16(src + 4);
  u32 hdr = 6;
  if ((u64)hdr + len1 + len2 + len3 > srcSize)
    return -1;
  u32 len4 = srcSize - hdr - len1 - len2 - len3;

  u32 seg = (dstSize + 3) / 4;
  if (seg * 3 > dstSize)
    return -1;
  u32 last = dstSize - seg * 3;

  const u8 *s1 = src + hdr;
  const u8 *s2 = s1 + len1;
  const u8 *s3 = s2 + len2;
  const u8 *s4 = s3 + len3;

  if (huf_decompress1x(dst, seg, s1, len1, dt) < 0)
    return -1;
  if (huf_decompress1x(dst + seg, seg, s2, len2, dt) < 0)
    return -1;
  if (huf_decompress1x(dst + seg * 2, seg, s3, len3, dt) < 0)
    return -1;
  if (huf_decompress1x(dst + seg * 3, last, s4, len4, dt) < 0)
    return -1;
  return 0;
}

/* ---- encode: length-limited canonical Huffman ----------------------------- */
/* Build a Huffman tree over the present symbols, cap code lengths at maxLimit,
 * and return per-symbol bit lengths (0 = absent) and the max length used. */
static int huf_build_lengths(const u32 *counts, u32 maxSymbol, u8 *nbBits, u32 maxLimit)
{
  /* nodes: leaves 0..nLeaves-1, internal nLeaves..; heapify by count. */
  u32 nodeCount[512];
  u16 nodeParent[512];
  u16 leafOf[256];
  u32 heap[257];
  u32 heapSize = 0, nLeaves = 0;
  u32 s;

  for (s = 0; s <= maxSymbol; s++)
    nbBits[s] = 0;

  for (s = 0; s <= maxSymbol; s++) {
    if (counts[s] == 0)
      continue;
    u32 node = nLeaves++;
    nodeCount[node] = counts[s];
    leafOf[s] = (u16)node;
  }
  if (nLeaves == 0)
    return -1;
  if (nLeaves == 1)
    return -1; /* single symbol -> RLE, not Huffman */

  /* min-heap of node indices, ordered by nodeCount (sift by hand). */
  for (u32 n = 0; n < nLeaves; n++)
    heap[heapSize++] = n;
  for (int i = (int)heapSize / 2 - 1; i >= 0; i--) {
    u32 root = (u32)i, child;
    u32 v = heap[root];
    while ((child = 2 * root + 1) < heapSize) {
      if (child + 1 < heapSize && nodeCount[heap[child + 1]] < nodeCount[heap[child]])
        child++;
      if (nodeCount[heap[child]] >= nodeCount[v])
        break;
      heap[root] = heap[child];
      root = child;
    }
    heap[root] = v;
  }
#define HEAP_POP(out)                                                                                                  \
  do {                                                                                                                 \
    out = heap[0];                                                                                                     \
    u32 v = heap[--heapSize], root = 0, child;                                                                         \
    while ((child = 2 * root + 1) < heapSize) {                                                                        \
      if (child + 1 < heapSize && nodeCount[heap[child + 1]] < nodeCount[heap[child]])                                 \
        child++;                                                                                                       \
      if (nodeCount[heap[child]] >= nodeCount[v])                                                                      \
        break;                                                                                                         \
      heap[root] = heap[child];                                                                                        \
      root = child;                                                                                                    \
    }                                                                                                                  \
    heap[root] = v;                                                                                                    \
  } while (0)
#define HEAP_PUSH(x)                                                                                                   \
  do {                                                                                                                 \
    u32 root = heapSize++, parent;                                                                                     \
    u32 v = (x);                                                                                                       \
    while (root > 0 && nodeCount[v] < nodeCount[heap[(parent = (root - 1) / 2)]]) {                                    \
      heap[root] = heap[parent];                                                                                       \
      root = parent;                                                                                                   \
    }                                                                                                                  \
    heap[root] = v;                                                                                                    \
  } while (0)

  u32 nextInternal = nLeaves;
  while (heapSize > 1) {
    u32 a, bnode;
    HEAP_POP(a);
    HEAP_POP(bnode);
    u32 in = nextInternal++;
    nodeCount[in] = nodeCount[a] + nodeCount[bnode];
    nodeParent[a] = (u16)in;
    nodeParent[bnode] = (u16)in;
    HEAP_PUSH(in);
  }
#undef HEAP_POP
#undef HEAP_PUSH
  u32 root;
  root = heap[0];
  nodeParent[root] = (u16)root; /* root points to itself          */

  /* depth of each leaf via parent walk; tally counts per length. */
  u32 cnt[64];
  memset(cnt, 0, sizeof(cnt));
  for (s = 0; s <= maxSymbol; s++) {
    if (counts[s] == 0)
      continue;
    u32 node = leafOf[s], depth = 0;
    while (node != root) {
      node = nodeParent[node];
      depth++;
    }
    if (depth == 0)
      depth = 1;
    nbBits[s] = (u8)depth;
    cnt[depth]++;
  }

  /* find current max length. */
  u32 maxBits = 0;
  for (u32 b = 63; b > 0; b--)
    if (cnt[b]) {
      maxBits = b;
      break;
    }

  if (maxBits > maxLimit) {
    /* clamp overflow lengths down to maxLimit. */
    for (u32 b = maxBits; b > maxLimit; b--) {
      cnt[maxLimit] += cnt[b];
      cnt[b] = 0;
    }
    maxBits = maxLimit;
    /* redistribute so the Kraft sum returns to exactly 1<<maxLimit. */
    u32 total = 0;
    for (u32 b = 1; b <= maxLimit; b++)
      total += cnt[b] << (maxLimit - b);
    u32 cap = 1u << maxLimit;
    while (total > cap) {
      cnt[maxLimit]--;
      u32 b = maxLimit - 1;
      while (cnt[b] == 0)
        b--;
      cnt[b]--;
      cnt[b + 1] += 2;
      total--;
    }
    /* reassign lengths to symbols by frequency: least frequent -> longest. */
    u16 order[256];
    u32 nOrder = 0;
    for (s = 0; s <= maxSymbol; s++)
      if (counts[s])
        order[nOrder++] = (u16)s;
    /* insertion sort descending by count (nOrder <= 256). */
    for (u32 i = 1; i < nOrder; i++) {
      u16 key = order[i];
      int j = (int)i - 1;
      while (j >= 0 && counts[order[j]] < counts[key]) {
        order[j + 1] = order[j];
        j--;
      }
      order[j + 1] = key;
    }
    /* walk lengths short->long, filling cnt[b] most-frequent-first. */
    u32 oi = 0;
    for (u32 b = 1; b <= maxLimit; b++) {
      for (u32 k = 0; k < cnt[b]; k++)
        nbBits[order[oi++]] = (u8)b;
    }
  }
  return (int)maxBits;
}

/* Assign canonical code values (MSB-first) from bit lengths. */
static void huf_assign_codes(const u8 *nbBits, u32 maxSymbol, u32 maxBits, u16 *code)
{
  u32 nbPerRank[HUF_ENC_MAX_BITS + 2];
  u32 valPerRank[HUF_ENC_MAX_BITS + 2];
  memset(nbPerRank, 0, sizeof(nbPerRank));
  for (u32 s = 0; s <= maxSymbol; s++)
    if (nbBits[s])
      nbPerRank[nbBits[s]]++;
  u32 min = 0;
  for (u32 n = maxBits; n > 0; n--) {
    valPerRank[n] = min;
    min += nbPerRank[n];
    min >>= 1;
  }
  for (u32 s = 0; s <= maxSymbol; s++)
    if (nbBits[s])
      code[s] = (u16)(valPerRank[nbBits[s]]++);
}

/* Encode one segment [ip,ip+n) into a fresh backward bitstream at dst. */
static u32 huf_encode_stream(u8 *dst, u32 cap, const u8 *ip, u32 n, const u16 *code, const u8 *nbBits)
{
  BITout bo;
  bitout_init(&bo, dst, cap);
  for (u32 i = n; i-- > 0;) {
    bitout_add(&bo, code[ip[i]], nbBits[ip[i]]);
    bitout_flush(&bo);
  }
  return bitout_close(&bo);
}

int huf_compress(u8 *dst, u32 dstCap, const u8 *src, u32 srcSize, int *nbStreamsOut)
{
  u32 counts[256];
  u8 nbBits[256];
  u16 code[256];
  u32 s, maxSymbol = 0;

  if (srcSize < 8)
    return -1; /* too small to bother            */
  memset(counts, 0, sizeof(counts));
  for (u32 i = 0; i < srcSize; i++)
    counts[src[i]]++;
  for (s = 0; s < 256; s++)
    if (counts[s])
      maxSymbol = s;

  /* direct-weight tree description caps at 128 weights (headerByte<=255). */
  if (maxSymbol > 128)
    return -1;

  int mb = huf_build_lengths(counts, maxSymbol, nbBits, HUF_ENC_MAX_BITS);
  if (mb < 0)
    return -1;
  u32 maxBits = (u32)mb;
  huf_assign_codes(nbBits, maxSymbol, maxBits, code);

  /* weights: weight = maxBits+1-nbBits (0 stays 0). */
  u8 weights[256];
  for (s = 0; s <= maxSymbol; s++)
    weights[s] = nbBits[s] ? (u8)(maxBits + 1 - nbBits[s]) : 0;

  u32 pos = 0;
  /* tree description: direct, omit the last symbol's weight (implied). */
  u32 nbWeights = maxSymbol; /* weights written for 0..maxSymbol-1 */
  if (pos + 1 > dstCap)
    return -1;
  dst[pos++] = (u8)(127 + nbWeights);
  u32 nbDescBytes = (nbWeights + 1) / 2;
  if (pos + nbDescBytes > dstCap)
    return -1;
  for (u32 i = 0; i < nbDescBytes; i++) {
    u8 hi = weights[2 * i];
    u8 lo = (2 * i + 1 < nbWeights) ? weights[2 * i + 1] : 0;
    dst[pos++] = (u8)((hi << 4) | lo);
  }

  int nbStreams = (srcSize >= 256) ? 4 : 1;
  if (nbStreams == 1) {
    u32 len = huf_encode_stream(dst + pos, (dstCap > pos) ? dstCap - pos : 0, src, srcSize, code, nbBits);
    if (len == 0 || pos + len > dstCap)
      return -1;
    pos += len;
  } else {
    u32 seg = (srcSize + 3) / 4;
    u32 jt = pos; /* 6-byte jump table              */
    if (pos + 6 > dstCap)
      return -1;
    pos += 6;
    u32 sizes[4];
    u32 offs[4] = {0, seg, seg * 2, seg * 3};
    u32 lens[4] = {seg, seg, seg, srcSize - seg * 3};
    for (int k = 0; k < 4; k++) {
      u32 len = huf_encode_stream(dst + pos, (dstCap > pos) ? dstCap - pos : 0, src + offs[k], lens[k], code, nbBits);
      if (len == 0 || pos + len > dstCap || len > 0xFFFF)
        return -1;
      sizes[k] = len;
      pos += len;
    }
    dst[jt + 0] = (u8)sizes[0];
    dst[jt + 1] = (u8)(sizes[0] >> 8);
    dst[jt + 2] = (u8)sizes[1];
    dst[jt + 3] = (u8)(sizes[1] >> 8);
    dst[jt + 4] = (u8)sizes[2];
    dst[jt + 5] = (u8)(sizes[2] >> 8);
  }
  if (nbStreamsOut)
    *nbStreamsOut = nbStreams;
  return (int)pos;
}
