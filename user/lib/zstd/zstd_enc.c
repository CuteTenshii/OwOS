/* zstd_enc.c: the Zstandard compressor (RFC 8878).
 *
 * Greedy LZ77 (hash-chain match finder) turns each block into a literal buffer
 * plus (litLength, matchLength, offset) sequences. Literals are Huffman-coded
 * when that helps (else stored raw); sequences use the predefined FSE tables and
 * are streamed out in the exact bit order the decoder reads back. Blocks that
 * don't compress fall back to stored (raw) blocks, so output never balloons.
 * A single-segment frame carries the content size and an XXH64 checksum. */
#include "zstd_internal.h"

#define ENC_BLOCK (128 * 1024)
#define MINMATCH 3
#define HLOG 16
#define HSIZE (1 << HLOG)

typedef struct {
  u32 litLength;
  u32 matchLength;
  u32 offset;
} seq_t;

/* ---- output buffer growth ------------------------------------------------- */
typedef struct {
  u8 *buf;
  u32 cap;
  u32 len;
  int oom;
} obuf;

static void ob_reserve(obuf *o, u32 extra)
{
  if (o->oom)
    return;
  if (o->len + extra <= o->cap)
    return;
  u32 cap = o->cap ? o->cap : 4096;
  while (cap < o->len + extra)
    cap *= 2;
  u8 *n = (u8 *)realloc(o->buf, cap);
  if (!n) {
    o->oom = 1;
    return;
  }
  o->buf = n;
  o->cap = cap;
}
static void ob_u8(obuf *o, u8 v)
{
  ob_reserve(o, 1);
  if (!o->oom)
    o->buf[o->len++] = v;
}
static void ob_write(obuf *o, const u8 *p, u32 n)
{
  ob_reserve(o, n);
  if (!o->oom) {
    memcpy(o->buf + o->len, p, n);
    o->len += n;
  }
}

/* ---- code lookups --------------------------------------------------------- */
static u32 ll_code(u32 v)
{
  u32 c = 0;
  while (c < MAX_LL && LL_base[c + 1] <= v)
    c++;
  return c;
}
static u32 ml_code(u32 v)
{
  u32 c = 0;
  while (c < MAX_ML && ML_base[c + 1] <= v)
    c++;
  return c;
}

/* ---- literals section ----------------------------------------------------- */
/* Emit the literals section for `lit` into out; returns bytes written or -1. */
static int emit_literals(obuf *out, const u8 *lit, u32 litLen, u8 *huftmp, u32 huftmpCap)
{
  if (litLen == 0) {
    ob_u8(out, 0x00);
    return out->oom ? -1 : 1;
  }

  int nbStreams = 0;
  int hl = huf_compress(huftmp, huftmpCap, lit, litLen, &nbStreams);
  if (hl > 0 && (u32)hl < litLen) {
    u32 regen = litLen, comp = (u32)hl, sizeFmt;
    if (nbStreams == 1)
      sizeFmt = 0;
    else if (regen < 1024 && comp < 1024)
      sizeFmt = 1;
    else if (regen < 16384 && comp < 16384)
      sizeFmt = 2;
    else
      sizeFmt = 3;
    u8 hdr[5];
    u8 typ = LIT_COMPRESSED;
    if (sizeFmt <= 1) {
      u32 w = regen | (comp << 10);
      hdr[0] = (u8)(typ | (sizeFmt << 2) | ((w & 0xF) << 4));
      hdr[1] = (u8)(w >> 4);
      hdr[2] = (u8)(w >> 12);
      ob_write(out, hdr, 3);
    } else if (sizeFmt == 2) {
      u32 w = regen | (comp << 14);
      hdr[0] = (u8)(typ | (sizeFmt << 2) | ((w & 0xF) << 4));
      hdr[1] = (u8)(w >> 4);
      hdr[2] = (u8)(w >> 12);
      hdr[3] = (u8)(w >> 20);
      ob_write(out, hdr, 4);
    } else {
      u64 w = (u64)regen | ((u64)comp << 18);
      hdr[0] = (u8)(typ | (sizeFmt << 2) | ((w & 0xF) << 4));
      hdr[1] = (u8)(w >> 4);
      hdr[2] = (u8)(w >> 12);
      hdr[3] = (u8)(w >> 20);
      hdr[4] = (u8)(w >> 28);
      ob_write(out, hdr, 5);
    }
    ob_write(out, huftmp, comp);
    return out->oom ? -1 : (int)(out->len); /* caller diffs; see below      */
  }

  /* Raw literals. */
  u32 regen = litLen;
  u8 typ = LIT_RAW;
  if (regen < 32) {
    ob_u8(out, (u8)(typ | ((regen & 0x1F) << 3)));
  } else if (regen < 4096) {
    ob_u8(out, (u8)(typ | (1 << 2) | ((regen & 0xF) << 4)));
    ob_u8(out, (u8)(regen >> 4));
  } else {
    ob_u8(out, (u8)(typ | (3 << 2) | ((regen & 0xF) << 4)));
    ob_u8(out, (u8)(regen >> 4));
    ob_u8(out, (u8)(regen >> 12));
  }
  ob_write(out, lit, litLen);
  return out->oom ? -1 : (int)(out->len);
}

/* ---- sequences section ---------------------------------------------------- */
static void emit_nbseq(obuf *o, u32 nbSeq)
{
  if (nbSeq < 128) {
    ob_u8(o, (u8)nbSeq);
  } else if (nbSeq < 0x7F00) {
    ob_u8(o, (u8)(128 + (nbSeq >> 8)));
    ob_u8(o, (u8)nbSeq);
  } else {
    ob_u8(o, 255);
    ob_u8(o, (u8)(nbSeq - 0x7F00));
    ob_u8(o, (u8)((nbSeq - 0x7F00) >> 8));
  }
}

/* FSE-encode the sequence stream with predefined tables. Returns bytes or -1. */
static int emit_sequences(obuf *out, const seq_t *seqs, u32 nbSeq, const fse_ctable *ctLL, const fse_ctable *ctOF,
                          const fse_ctable *ctML, u8 *bitbuf, u32 bitbufCap)
{
  emit_nbseq(out, nbSeq);
  if (nbSeq == 0)
    return out->oom ? -1 : (int)out->len;
  ob_u8(out, 0x00); /* all three modes = predefined   */

  /* Precompute codes + extra bits per sequence. */
  BITout bo;
  bitout_init(&bo, bitbuf, bitbufCap);

  fse_cstate sLL, sOF, sML;
  u32 L = nbSeq - 1;

  u32 llc = ll_code(seqs[L].litLength);
  u32 mlc = ml_code(seqs[L].matchLength);
  u32 ofc = zstd_highbit32(seqs[L].offset + 3);

  fse_init_cstate(&sML, ctML, mlc);
  fse_init_cstate(&sOF, ctOF, ofc);
  fse_init_cstate(&sLL, ctLL, llc);

  bitout_add(&bo, seqs[L].litLength - LL_base[llc], LL_bits[llc]);
  bitout_flush(&bo);
  bitout_add(&bo, seqs[L].matchLength - ML_base[mlc], ML_bits[mlc]);
  bitout_flush(&bo);
  bitout_add(&bo, (seqs[L].offset + 3) - (1u << ofc), ofc);
  bitout_flush(&bo);

  for (int n = (int)L - 1; n >= 0; n--) {
    u32 lc = ll_code(seqs[n].litLength);
    u32 mc = ml_code(seqs[n].matchLength);
    u32 oc = zstd_highbit32(seqs[n].offset + 3);

    fse_encode_symbol(&bo, &sOF, oc);
    bitout_flush(&bo);
    fse_encode_symbol(&bo, &sML, mc);
    bitout_flush(&bo);
    fse_encode_symbol(&bo, &sLL, lc);
    bitout_flush(&bo);

    bitout_add(&bo, seqs[n].litLength - LL_base[lc], LL_bits[lc]);
    bitout_flush(&bo);
    bitout_add(&bo, seqs[n].matchLength - ML_base[mc], ML_bits[mc]);
    bitout_flush(&bo);
    bitout_add(&bo, (seqs[n].offset + 3) - (1u << oc), oc);
    bitout_flush(&bo);
  }

  fse_flush_cstate(&bo, &sML);
  fse_flush_cstate(&bo, &sOF);
  fse_flush_cstate(&bo, &sLL);
  u32 streamLen = bitout_close(&bo);
  if (streamLen == 0)
    return -1;
  ob_write(out, bitbuf, streamLen);
  return out->oom ? -1 : (int)out->len;
}

/* ---- LZ77 match finder ---------------------------------------------------- */
static inline u32 hash4(const u8 *p)
{
  u32 x = zstd_rd32(p);
  return (x * 2654435761u) >> (32 - HLOG);
}

/* Fill seqs[] for one block; sets *litOut and *litLenOut (into litbuf). Returns
 * number of sequences. */
static u32 lz_block(const u8 *b, u32 n, seq_t *seqs, u32 *nbSeqCap, u8 *litbuf, u32 *litLenOut, int *hashTable,
                    int *chain, u32 chainDepth)
{
  u32 nSeq = 0;
  u32 anchor = 0, ip = 0;
  u32 litLen = 0;

  for (u32 i = 0; i < HSIZE; i++)
    hashTable[i] = -1;

  while (ip + MINMATCH <= n) {
    u32 h = hash4(b + ip);
    int cand = hashTable[h];
    u32 bestLen = 0, bestPos = 0;
    u32 depth = chainDepth;
    while (cand >= 0 && depth-- > 0) {
      u32 cp = (u32)cand;
      u32 maxLen = n - ip;
      u32 l = 0;
      while (l < maxLen && b[cp + l] == b[ip + l])
        l++;
      if (l > bestLen) {
        bestLen = l;
        bestPos = cp;
        if (l >= maxLen)
          break;
      }
      cand = chain[cp];
    }
    chain[ip] = hashTable[h];
    hashTable[h] = (int)ip;

    if (bestLen >= MINMATCH) {
      if (nSeq >= *nbSeqCap)
        break;
      seqs[nSeq].litLength = ip - anchor;
      seqs[nSeq].matchLength = bestLen;
      seqs[nSeq].offset = ip - bestPos;
      nSeq++;
      memcpy(litbuf + litLen, b + anchor, ip - anchor);
      litLen += ip - anchor;

      u32 end = ip + bestLen;
      for (u32 j = ip + 1; j < end && j + MINMATCH <= n; j++) {
        u32 hj = hash4(b + j);
        chain[j] = hashTable[hj];
        hashTable[hj] = (int)j;
      }
      ip = end;
      anchor = ip;
    } else {
      ip++;
    }
  }
  memcpy(litbuf + litLen, b + anchor, n - anchor);
  litLen += n - anchor;
  *litLenOut = litLen;
  return nSeq;
}

/* ---- block + frame -------------------------------------------------------- */
static void emit_block_header(obuf *o, int last, u32 btype, u32 bsize)
{
  u32 bh = (u32)last | (btype << 1) | (bsize << 3);
  ob_u8(o, (u8)bh);
  ob_u8(o, (u8)(bh >> 8));
  ob_u8(o, (u8)(bh >> 16));
}

unsigned char *zstd_compress(const unsigned char *in, int in_len, int *out_len, int level)
{
  if (in_len < 0)
    return 0;
  if (level < 1)
    level = 1;
  if (level > 19)
    level = 19;
  u32 chainDepth = (level <= 3) ? 8 : (level <= 6) ? 32 : (level <= 9) ? 128 : (level <= 14) ? 512 : 2048;

  obuf out = {0, 0, 0, 0};

  /* scratch */
  u8 *litbuf = (u8 *)malloc(ENC_BLOCK + 16);
  u8 *litsec = 0, *huftmp = 0, *bitbuf = 0, *seqsec = 0;
  int *hashTable = (int *)malloc(HSIZE * sizeof(int));
  int *chain = (int *)malloc((ENC_BLOCK + 16) * sizeof(int));
  u32 nbSeqCap = ENC_BLOCK / 2 + 8;
  seq_t *seqs = (seq_t *)malloc(nbSeqCap * sizeof(seq_t));
  fse_ctable *ctLL = (fse_ctable *)malloc(sizeof(fse_ctable));
  fse_ctable *ctOF = (fse_ctable *)malloc(sizeof(fse_ctable));
  fse_ctable *ctML = (fse_ctable *)malloc(sizeof(fse_ctable));
  u16 *stLL = (u16 *)malloc((1u << LL_DEFAULT_LOG) * sizeof(u16));
  u16 *stOF = (u16 *)malloc((1u << OF_DEFAULT_LOG) * sizeof(u16));
  u16 *stML = (u16 *)malloc((1u << ML_DEFAULT_LOG) * sizeof(u16));

  if (!litbuf || !hashTable || !chain || !seqs || !ctLL || !ctOF || !ctML || !stLL || !stOF || !stML)
    goto oom;
  ctLL->stateTable = stLL;
  ctOF->stateTable = stOF;
  ctML->stateTable = stML;
  if (fse_build_ctable(ctLL, LL_defaultNorm, MAX_LL, LL_DEFAULT_LOG) < 0)
    goto oom;
  if (fse_build_ctable(ctOF, OF_defaultNorm, DEF_MAX_OFF, OF_DEFAULT_LOG) < 0)
    goto oom;
  if (fse_build_ctable(ctML, ML_defaultNorm, MAX_ML, ML_DEFAULT_LOG) < 0)
    goto oom;

  /* per-block section scratch */
  u32 secCap = ENC_BLOCK + 4096;
  litsec = (u8 *)malloc(secCap);
  seqsec = (u8 *)malloc(secCap);
  huftmp = (u8 *)malloc(secCap);
  bitbuf = (u8 *)malloc(secCap);
  if (!litsec || !seqsec || !huftmp || !bitbuf)
    goto oom;

  /* ---- frame header ---- */
  ob_write(&out, (const u8[]){0x28, 0xB5, 0x2F, 0xFD}, 4);
  u32 fcsFlag, fcsBytes;
  if ((u32)in_len <= 255) {
    fcsFlag = 0;
    fcsBytes = 1;
  } else if ((u32)in_len <= 65535 + 256) {
    fcsFlag = 1;
    fcsBytes = 2;
  } else {
    fcsFlag = 2;
    fcsBytes = 4;
  }
  u8 fhd = (u8)((fcsFlag << 6) | (1 << 5) | (1 << 2)); /* single-seg, checksum */
  ob_u8(&out, fhd);
  if (fcsBytes == 1)
    ob_u8(&out, (u8)in_len);
  else if (fcsBytes == 2) {
    u32 v = (u32)in_len - 256;
    ob_u8(&out, (u8)v);
    ob_u8(&out, (u8)(v >> 8));
  } else {
    u32 v = (u32)in_len;
    ob_u8(&out, (u8)v);
    ob_u8(&out, (u8)(v >> 8));
    ob_u8(&out, (u8)(v >> 16));
    ob_u8(&out, (u8)(v >> 24));
  }

  /* ---- blocks ---- */
  u32 pos = 0;
  if (in_len == 0) {
    emit_block_header(&out, 1, ZSTD_BLOCK_RAW, 0);
  }
  while (pos < (u32)in_len) {
    u32 bsize = (u32)in_len - pos;
    if (bsize > ENC_BLOCK)
      bsize = ENC_BLOCK;
    int last = (pos + bsize >= (u32)in_len);
    const u8 *b = in + pos;

    u32 litLen = 0;
    u32 nSeq = lz_block(b, bsize, seqs, &nbSeqCap, litbuf, &litLen, hashTable, chain, chainDepth);

    /* Build the compressed block body into litsec+seqsec via a temp obuf. */
    obuf body = {0, 0, 0, 0};
    body.buf = litsec;
    body.cap = secCap; /* borrow litsec as body   */
    int lr = emit_literals(&body, litbuf, litLen, huftmp, secCap);
    obuf sq = {0, 0, 0, 0};
    sq.buf = seqsec;
    sq.cap = secCap;
    int sr = emit_sequences(&sq, seqs, nSeq, ctLL, ctOF, ctML, bitbuf, secCap);

    int compressible = (lr >= 0 && sr >= 0 && !body.oom && !sq.oom);
    u32 compLen = compressible ? body.len + sq.len : 0xFFFFFFFFu;

    if (compressible && compLen < bsize && compLen <= 0x1FFFFF) {
      emit_block_header(&out, last, ZSTD_BLOCK_COMPRESSED, compLen);
      ob_write(&out, body.buf, body.len);
      ob_write(&out, sq.buf, sq.len);
    } else {
      emit_block_header(&out, last, ZSTD_BLOCK_RAW, bsize);
      ob_write(&out, b, bsize);
    }
    pos += bsize;
    if (out.oom)
      goto oom;
  }

  /* ---- content checksum ---- */
  {
    u32 h = (u32)xxh64(in, (u32)in_len, 0);
    ob_u8(&out, (u8)h);
    ob_u8(&out, (u8)(h >> 8));
    ob_u8(&out, (u8)(h >> 16));
    ob_u8(&out, (u8)(h >> 24));
  }
  if (out.oom)
    goto oom;

  free(litbuf);
  free(hashTable);
  free(chain);
  free(seqs);
  free(ctLL);
  free(ctOF);
  free(ctML);
  free(stLL);
  free(stOF);
  free(stML);
  free(litsec);
  free(seqsec);
  free(huftmp);
  free(bitbuf);
  if (out_len)
    *out_len = (int)out.len;
  return out.buf;

oom:
  free(out.buf);
  free(litbuf);
  free(hashTable);
  free(chain);
  free(seqs);
  free(ctLL);
  free(ctOF);
  free(ctML);
  free(stLL);
  free(stOF);
  free(stML);
  free(litsec);
  free(seqsec);
  free(huftmp);
  free(bitbuf);
  return 0;
}
