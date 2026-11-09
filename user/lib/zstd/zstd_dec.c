/* zstd_dec.c: the Zstandard decompressor front end (RFC 8878).
 *
 * Parses frames and blocks, decodes every literals mode (raw/RLE/Huffman/
 * treeless) and sequence mode (predefined/RLE/FSE/repeat), executes the
 * literal-copy + match-copy sequence stream against the three repeat offsets,
 * and verifies the optional XXH64 content checksum. Output grows on the heap. */
#include "zstd_internal.h"

#define LL_FSELOG 9
#define ML_FSELOG 9
#define OF_FSELOG 8

typedef struct {
  u8 *out;
  u32 outCap;
  u32 outLen;

  u8 *lit;
  u32 litLen;

  huf_dtable huf;
  int hufReady;

  fse_dstate llDT[1 << LL_FSELOG];
  fse_dstate ofDT[1 << OF_FSELOG];
  fse_dstate mlDT[1 << ML_FSELOG];
  u32 llLog, ofLog, mlLog;
  int llReady, ofReady, mlReady;

  fse_dstate llPre[1 << LL_DEFAULT_LOG];
  fse_dstate ofPre[1 << OF_DEFAULT_LOG];
  fse_dstate mlPre[1 << ML_DEFAULT_LOG];

  u32 rep[3];
} dctx;

static int ensure_cap(dctx *c, u32 needed)
{
  if (needed <= c->outCap)
    return 0;
  u32 cap = c->outCap ? c->outCap : 65536;
  while (cap < needed) {
    if (cap > 0x80000000u) {
      cap = needed;
      break;
    }
    cap *= 2;
  }
  u8 *n = (u8 *)realloc(c->out, cap);
  if (!n)
    return -1;
  c->out = n;
  c->outCap = cap;
  return 0;
}

/* ---- literals ------------------------------------------------------------- */
/* Decode the literals section at `src`; sets c->lit / c->litLen. Returns bytes
 * consumed, or -1 on error. */
static int decode_literals(dctx *c, const u8 *src, u32 srcSize)
{
  if (srcSize < 1)
    return -1;
  u32 type = src[0] & 3;
  u32 sizeFmt = (src[0] >> 2) & 3;

  if (type == LIT_RAW || type == LIT_RLE) {
    u32 regen, hlen;
    if ((sizeFmt & 1) == 0) {
      regen = src[0] >> 3;
      hlen = 1;
    } else if (sizeFmt == 1) {
      if (srcSize < 2)
        return -1;
      regen = (src[0] >> 4) | ((u32)src[1] << 4);
      hlen = 2;
    } else {
      if (srcSize < 3)
        return -1;
      regen = (src[0] >> 4) | ((u32)src[1] << 4) | ((u32)src[2] << 12);
      hlen = 3;
    }
    if (regen > ZSTD_BLOCKSIZE_MAX)
      return -1;
    if (type == LIT_RAW) {
      if (hlen + regen > srcSize)
        return -1;
      memcpy(c->lit, src + hlen, regen);
      c->litLen = regen;
      return (int)(hlen + regen);
    } else {
      if (hlen + 1 > srcSize)
        return -1;
      memset(c->lit, src[hlen], regen);
      c->litLen = regen;
      return (int)(hlen + 1);
    }
  }

  /* Compressed / Treeless. */
  u32 regen, comp, hlen, nbStreams;
  if (sizeFmt == 0 || sizeFmt == 1) {
    if (srcSize < 3)
      return -1;
    u32 v = (src[0] >> 4) | ((u32)src[1] << 4) | ((u32)src[2] << 12);
    regen = v & 0x3FF;
    comp = v >> 10;
    hlen = 3;
    nbStreams = (sizeFmt == 0) ? 1 : 4;
  } else if (sizeFmt == 2) {
    if (srcSize < 4)
      return -1;
    u32 v = (src[0] >> 4) | ((u32)src[1] << 4) | ((u32)src[2] << 12) | ((u32)src[3] << 20);
    regen = v & 0x3FFF;
    comp = v >> 14;
    hlen = 4;
    nbStreams = 4;
  } else {
    if (srcSize < 5)
      return -1;
    u64 v = (src[0] >> 4) | ((u32)src[1] << 4) | ((u32)src[2] << 12) | ((u64)src[3] << 20) | ((u64)src[4] << 28);
    regen = (u32)(v & 0x3FFFF);
    comp = (u32)(v >> 18);
    hlen = 5;
    nbStreams = 4;
  }
  if (regen > ZSTD_BLOCKSIZE_MAX)
    return -1;
  if (hlen + comp > srcSize)
    return -1;

  const u8 *payload = src + hlen;
  u32 payLen = comp;
  if (type == LIT_COMPRESSED) {
    int t = huf_read_dtable(&c->huf, payload, payLen);
    if (t < 0 || (u32)t > payLen)
      return -1;
    c->hufReady = 1;
    payload += t;
    payLen -= t;
  } else { /* treeless: reuse prior table    */
    if (!c->hufReady)
      return -1;
  }
  if (nbStreams == 1) {
    if (huf_decompress1x(c->lit, regen, payload, payLen, &c->huf) < 0)
      return -1;
  } else {
    if (huf_decompress4x(c->lit, regen, payload, payLen, &c->huf) < 0)
      return -1;
  }
  c->litLen = regen;
  return (int)(hlen + comp);
}

/* ---- sequences ------------------------------------------------------------ */
static void build_rle_dt(fse_dstate *dt, u8 sym)
{
  dt[0].newState = 0;
  dt[0].symbol = sym;
  dt[0].nbBits = 0;
}

/* Select the decode table for one symbol category per its compression mode. */
static int load_seq_table(dctx *c, u32 mode, const u8 **pp, const u8 *iend, fse_dstate *dt, const fse_dstate *pre,
                          u32 preLog, u32 maxSym, const fse_dstate **outDT, u32 *outLog, fse_dstate *repDT, u32 *repLog,
                          int *repReady, u32 maxLog)
{
  const u8 *p = *pp;
  switch (mode) {
  case SEQ_PREDEFINED:
    *outDT = pre;
    *outLog = preLog;
    return 0;
  case SEQ_RLE:
    if (p >= iend || *p > maxSym)
      return -1;
    build_rle_dt(repDT, *p);
    *pp = p + 1;
    *outDT = repDT;
    *outLog = 0;
    *repReady = 1;
    *repLog = 0;
    return 0;
  case SEQ_FSE: {
    short norm[256];
    unsigned maxSV = 0, tableLog = 0;
    int nc = fse_read_ncount(norm, &maxSV, &tableLog, p, (u32)(iend - p));
    if (nc < 0 || tableLog > maxLog || maxSV > maxSym)
      return -1;
    if (fse_build_dtable(repDT, norm, maxSV, tableLog) < 0)
      return -1;
    *pp = p + nc;
    *outDT = repDT;
    *outLog = tableLog;
    *repReady = 1;
    *repLog = tableLog;
    return 0;
  }
  case SEQ_REPEAT:
    if (!*repReady)
      return -1;
    *outDT = repDT;
    *outLog = *repLog;
    return 0;
  }
  (void)dt;
  return -1;
}

/* Decode + execute the sequences section; appends to c->out. Returns 0/-1. */
static int decode_sequences(dctx *c, const u8 *src, u32 srcSize)
{
  const u8 *ip = src, *iend = src + srcSize;
  u32 nbSeq;

  if (srcSize < 1)
    return -1;
  u8 b0 = *ip++;
  if (b0 == 0) {
    nbSeq = 0;
  } else if (b0 < 128) {
    nbSeq = b0;
  } else if (b0 < 255) {
    if (ip >= iend)
      return -1;
    nbSeq = ((b0 - 128) << 8) + *ip++;
  } else {
    if (ip + 2 > iend)
      return -1;
    nbSeq = *ip + ((u32)ip[1] << 8) + 0x7F00;
    ip += 2;
  }

  if (nbSeq == 0) {
    /* No sequences: the whole literal buffer is the block output. */
    if (ensure_cap(c, c->outLen + c->litLen) < 0)
      return -1;
    memcpy(c->out + c->outLen, c->lit, c->litLen);
    c->outLen += c->litLen;
    return 0;
  }

  if (ip >= iend)
    return -1;
  u8 modes = *ip++;
  u32 llMode = (modes >> 6) & 3;
  u32 ofMode = (modes >> 4) & 3;
  u32 mlMode = (modes >> 2) & 3;

  const fse_dstate *llDT, *ofDT, *mlDT;
  u32 llLog, ofLog, mlLog;

  if (load_seq_table(c, llMode, &ip, iend, c->llDT, c->llPre, LL_DEFAULT_LOG, MAX_LL, &llDT, &llLog, c->llDT, &c->llLog,
                     &c->llReady, LL_FSELOG) < 0)
    return -1;
  if (load_seq_table(c, ofMode, &ip, iend, c->ofDT, c->ofPre, OF_DEFAULT_LOG, MAX_OFF, &ofDT, &ofLog, c->ofDT,
                     &c->ofLog, &c->ofReady, OF_FSELOG) < 0)
    return -1;
  if (load_seq_table(c, mlMode, &ip, iend, c->mlDT, c->mlPre, ML_DEFAULT_LOG, MAX_ML, &mlDT, &mlLog, c->mlDT, &c->mlLog,
                     &c->mlReady, ML_FSELOG) < 0)
    return -1;

  BITin bs;
  if (bitin_init(&bs, ip, (u32)(iend - ip)))
    return -1;
  u32 stLL = bitin_read(&bs, llLog);
  u32 stOF = bitin_read(&bs, ofLog);
  u32 stML = bitin_read(&bs, mlLog);

  const u8 *litPtr = c->lit;
  u32 litRemaining = c->litLen;

  for (u32 i = 0; i < nbSeq; i++) {
    u8 llSym = llDT[stLL].symbol;
    u8 mlSym = mlDT[stML].symbol;
    u8 ofSym = ofDT[stOF].symbol;

    u32 offValue = (1u << ofSym) + bitin_read(&bs, ofSym);
    /* A reload guarantees only 57 bits; long-window offsets plus both length extras can need 63. */
    if (ofSym + ML_bits[mlSym] + LL_bits[llSym] > 56)
      bitin_reload(&bs);
    u32 matchLength = ML_base[mlSym] + bitin_read(&bs, ML_bits[mlSym]);
    u32 litLength = LL_base[llSym] + bitin_read(&bs, LL_bits[llSym]);
    if (bitin_reload(&bs) == BIT_OVERFLOW && i + 1 < nbSeq)
      return -1;

    u32 offset;
    if (offValue > 3) {
      offset = offValue - 3;
      c->rep[2] = c->rep[1];
      c->rep[1] = c->rep[0];
      c->rep[0] = offset;
    } else {
      u32 idx = offValue + (litLength == 0 ? 1 : 0);
      if (idx == 1) {
        offset = c->rep[0];
      } else if (idx == 2) {
        offset = c->rep[1];
        c->rep[1] = c->rep[0];
        c->rep[0] = offset;
      } else if (idx == 3) {
        offset = c->rep[2];
        c->rep[2] = c->rep[1];
        c->rep[1] = c->rep[0];
        c->rep[0] = offset;
      } else {
        offset = c->rep[0] - 1;
        if (offset == 0)
          offset = 1;
        c->rep[2] = c->rep[1];
        c->rep[1] = c->rep[0];
        c->rep[0] = offset;
      }
    }

    if (litLength > litRemaining)
      return -1;
    if (offset == 0 || offset > c->outLen + litLength)
      return -1;

    if (ensure_cap(c, c->outLen + litLength + matchLength) < 0)
      return -1;
    memcpy(c->out + c->outLen, litPtr, litLength);
    c->outLen += litLength;
    litPtr += litLength;
    litRemaining -= litLength;

    u8 *mdst = c->out + c->outLen;
    const u8 *msrc = mdst - offset;
    for (u32 k = 0; k < matchLength; k++)
      mdst[k] = msrc[k];
    c->outLen += matchLength;

    if (i + 1 < nbSeq) {
      u32 nbLL = llDT[stLL].nbBits, nsLL = llDT[stLL].newState;
      u32 nbML = mlDT[stML].nbBits, nsML = mlDT[stML].newState;
      u32 nbOF = ofDT[stOF].nbBits, nsOF = ofDT[stOF].newState;
      stLL = nsLL + bitin_read(&bs, nbLL);
      stML = nsML + bitin_read(&bs, nbML);
      stOF = nsOF + bitin_read(&bs, nbOF);
      bitin_reload(&bs);
    }
  }

  /* Trailing literals after the final sequence. */
  if (ensure_cap(c, c->outLen + litRemaining) < 0)
    return -1;
  memcpy(c->out + c->outLen, litPtr, litRemaining);
  c->outLen += litRemaining;
  return 0;
}

/* ---- frame + block loop --------------------------------------------------- */
static int build_predefined(dctx *c)
{
  c->llLog = LL_DEFAULT_LOG;
  c->ofLog = OF_DEFAULT_LOG;
  c->mlLog = ML_DEFAULT_LOG;
  if (fse_build_dtable(c->llPre, LL_defaultNorm, MAX_LL, LL_DEFAULT_LOG) < 0)
    return -1;
  if (fse_build_dtable(c->ofPre, OF_defaultNorm, DEF_MAX_OFF, OF_DEFAULT_LOG) < 0)
    return -1;
  if (fse_build_dtable(c->mlPre, ML_defaultNorm, MAX_ML, ML_DEFAULT_LOG) < 0)
    return -1;
  return 0;
}

/* Decode one frame starting at *pp; returns 0/-1 and advances *pp past it. */
static int decode_frame(dctx *c, const u8 **pp, const u8 *iend)
{
  const u8 *ip = *pp;
  if (ip + 4 > iend)
    return -1;
  u32 magic = zstd_rd32(ip);

  if ((magic & 0xFFFFFFF0u) == ZSTD_MAGIC_SKIP0) {
    ip += 4;
    if (ip + 4 > iend)
      return -1;
    u32 skipLen = zstd_rd32(ip);
    ip += 4;
    if ((u64)(iend - ip) < skipLen)
      return -1;
    ip += skipLen;
    *pp = ip;
    return 0;
  }
  if (magic != ZSTD_MAGIC)
    return -1;
  ip += 4;

  if (ip >= iend)
    return -1;
  u8 fhd = *ip++;
  u32 fcsFlag = fhd >> 6;
  int singleSeg = (fhd >> 5) & 1;
  int checksumFlag = (fhd >> 2) & 1;
  u32 didFlag = fhd & 3;
  if ((fhd >> 3) & 1)
    return -1; /* reserved bit must be zero      */

  if (!singleSeg) {
    if (ip >= iend)
      return -1;
    ip++; /* window descriptor (unused)     */
  }
  static const u32 didBytes[4] = {0, 1, 2, 4};
  u32 db = didBytes[didFlag];
  if (ip + db > iend)
    return -1;
  ip += db; /* dictionary id (unsupported)    */

  u32 fcsBytes = (fcsFlag == 0) ? (singleSeg ? 1 : 0) : (1u << fcsFlag);
  if (ip + fcsBytes > iend)
    return -1;
  u64 fcs = 0;
  if (fcsBytes == 1)
    fcs = ip[0];
  else if (fcsBytes == 2)
    fcs = (u64)zstd_rd16(ip) + 256;
  else if (fcsBytes == 4)
    fcs = zstd_rd32(ip);
  else if (fcsBytes == 8)
    fcs = zstd_rd64(ip);
  ip += fcsBytes;

  if (fcsBytes && fcs && fcs < 0x40000000ull)
    if (ensure_cap(c, c->outLen + (u32)fcs) < 0)
      return -1;

  u32 frameStart = c->outLen;
  c->rep[0] = 1;
  c->rep[1] = 4;
  c->rep[2] = 8;
  c->hufReady = 0;
  c->llReady = c->ofReady = c->mlReady = 0;

  for (;;) {
    if (ip + 3 > iend)
      return -1;
    u32 bh = zstd_rd24(ip);
    ip += 3;
    int last = bh & 1;
    u32 btype = (bh >> 1) & 3;
    u32 bsize = bh >> 3;

    if (btype == ZSTD_BLOCK_RAW) {
      if ((u64)(iend - ip) < bsize)
        return -1;
      if (ensure_cap(c, c->outLen + bsize) < 0)
        return -1;
      memcpy(c->out + c->outLen, ip, bsize);
      c->outLen += bsize;
      ip += bsize;
    } else if (btype == ZSTD_BLOCK_RLE) {
      if (ip >= iend)
        return -1;
      if (ensure_cap(c, c->outLen + bsize) < 0)
        return -1;
      memset(c->out + c->outLen, *ip, bsize);
      c->outLen += bsize;
      ip += 1;
    } else if (btype == ZSTD_BLOCK_COMPRESSED) {
      if ((u64)(iend - ip) < bsize)
        return -1;
      int lc = decode_literals(c, ip, bsize);
      if (lc < 0 || (u32)lc > bsize)
        return -1;
      if (decode_sequences(c, ip + lc, bsize - lc) < 0)
        return -1;
      ip += bsize;
    } else {
      return -1; /* reserved block type            */
    }
    if (last)
      break;
  }

  if (checksumFlag) {
    if (ip + 4 > iend)
      return -1;
    u32 want = zstd_rd32(ip);
    ip += 4;
    u32 got = (u32)xxh64(c->out + frameStart, c->outLen - frameStart, 0);
    if (got != want)
      return -1;
  }
  *pp = ip;
  return 0;
}

unsigned char *zstd_decompress(const unsigned char *in, int in_len, int *out_len)
{
  if (in_len < 0)
    return 0;
  dctx *c = (dctx *)calloc(1, sizeof(dctx));
  if (!c)
    return 0;
  c->lit = (u8 *)malloc(ZSTD_BLOCKSIZE_MAX);
  if (!c->lit) {
    free(c);
    return 0;
  }
  if (build_predefined(c) < 0) {
    free(c->lit);
    free(c);
    return 0;
  }

  const u8 *ip = in, *iend = in + in_len;
  int ok = 1;
  while (ip < iend) {
    if (decode_frame(c, &ip, iend) < 0) {
      ok = 0;
      break;
    }
  }

  u8 *result = 0;
  if (ok) {
    if (c->outLen == 0) {
      result = (u8 *)malloc(1); /* non-NULL zero-length output    */
      if (result) {
        if (out_len)
          *out_len = 0;
      }
    } else {
      result = c->out;
      c->out = 0;
      if (out_len)
        *out_len = (int)c->outLen;
    }
  }
  free(c->out);
  free(c->lit);
  free(c);
  return result;
}
