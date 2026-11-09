/* zstd_internal.h: internal shared definitions for the zstd codec.
 *
 * Backward bitstream reader/writer (the little-endian LIFO container zstd uses
 * for FSE + Huffman), plus the FSE/Huffman engine and constant-table decls.
 * All bit I/O is header-inline so fse/huf/decode/encode share one copy.
 *
 * Endianness: reads/writes assume a little-endian host (x86, the only target). */
#ifndef OWOS_ZSTD_INTERNAL_H
#define OWOS_ZSTD_INTERNAL_H

#include "owos.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef signed short s16;

/* ---- format constants ---------------------------------------------------- */
#define ZSTD_MAGIC 0xFD2FB528u
#define ZSTD_MAGIC_SKIP0 0x184D2A50u /* skippable frames: 0x184D2A50..5F     */

#define ZSTD_BLOCK_RAW 0
#define ZSTD_BLOCK_RLE 1
#define ZSTD_BLOCK_COMPRESSED 2
#define ZSTD_BLOCK_RESERVED 3

#define LIT_RAW 0
#define LIT_RLE 1
#define LIT_COMPRESSED 2
#define LIT_TREELESS 3

#define SEQ_PREDEFINED 0
#define SEQ_RLE 1
#define SEQ_FSE 2
#define SEQ_REPEAT 3

#define MAX_LL 35
#define MAX_ML 52
#define MAX_OFF 31     /* offset code range the decoder tables cover      */
#define DEF_MAX_OFF 28 /* predefined offset table's largest symbol        */

#define LL_DEFAULT_LOG 6
#define ML_DEFAULT_LOG 6
#define OF_DEFAULT_LOG 5

#define HUF_MAX_TABLELOG 12
#define HUF_MAX_SYMBOL 255

#define ZSTD_BLOCKSIZE_MAX (128 * 1024)

/* ---- little-endian raw reads --------------------------------------------- */
static inline u32 zstd_rd16(const void *p)
{
  u16 x;
  memcpy(&x, p, 2);
  return x;
}
static inline u32 zstd_rd24(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16); }
static inline u32 zstd_rd32(const void *p)
{
  u32 x;
  memcpy(&x, p, 4);
  return x;
}
static inline u64 zstd_rd64(const void *p)
{
  u64 x;
  memcpy(&x, p, 8);
  return x;
}

/* Position of the most-significant set bit (v must be non-zero). */
static inline u32 zstd_highbit32(u32 v) { return 31u - (u32)__builtin_clz(v); }

/* ---- backward bit reader (BITin) ----------------------------------------- */
/* zstd bitstreams are written forward but read from the end: the final byte's
 * top set bit is a start marker, and reads pull from the top of a 64-bit
 * little-endian window that slides toward the buffer start on reload. */
typedef struct {
  u64 bitContainer;
  u32 bitsConsumed;
  const u8 *ptr;
  const u8 *start;
  const u8 *limitPtr; /* start + 8: below this, reload clamps          */
} BITin;

/* Returns 1 on error (empty / no start marker), else 0. */
static inline int bitin_init(BITin *b, const u8 *src, u32 srcSize)
{
  if (srcSize == 0) {
    memset(b, 0, sizeof(*b));
    return 1;
  }
  b->start = src;
  b->limitPtr = src + 8;
  if (srcSize >= 8) {
    b->ptr = src + srcSize - 8;
    b->bitContainer = zstd_rd64(b->ptr);
    u8 last = src[srcSize - 1];
    if (last == 0)
      return 1;
    b->bitsConsumed = 8u - zstd_highbit32(last);
  } else {
    b->ptr = src;
    b->bitContainer = src[0];
    switch (srcSize) {
    case 7:
      b->bitContainer += (u64)src[6] << 48; /* fallthrough */
    case 6:
      b->bitContainer += (u64)src[5] << 40; /* fallthrough */
    case 5:
      b->bitContainer += (u64)src[4] << 32; /* fallthrough */
    case 4:
      b->bitContainer += (u64)src[3] << 24; /* fallthrough */
    case 3:
      b->bitContainer += (u64)src[2] << 16; /* fallthrough */
    case 2:
      b->bitContainer += (u64)src[1] << 8; /* fallthrough */
    default:
      break;
    }
    u8 last = src[srcSize - 1];
    if (last == 0)
      return 1;
    b->bitsConsumed = 8u - zstd_highbit32(last);
    b->bitsConsumed += (8u - srcSize) * 8u;
  }
  return 0;
}

/* Read nbBits (0..32) from the top of the window without a reload. */
static inline u32 bitin_read(BITin *b, u32 nbBits)
{
  if (nbBits == 0)
    return 0;
  u32 v = (u32)((b->bitContainer << b->bitsConsumed) >> (64u - nbBits));
  b->bitsConsumed += nbBits;
  return v;
}

/* Peek nbBits (1..32) without consuming; skip consumes without reading. */
static inline u32 bitin_look(const BITin *b, u32 nbBits)
{
  return (u32)((b->bitContainer << b->bitsConsumed) >> (64u - nbBits));
}
static inline void bitin_skip(BITin *b, u32 nbBits) { b->bitsConsumed += nbBits; }

/* Reload status. */
enum { BIT_UNFINISHED = 0, BIT_ENDOFBUFFER = 1, BIT_COMPLETED = 2, BIT_OVERFLOW = 3 };

static inline int bitin_reload(BITin *b)
{
  if (b->bitsConsumed > 64)
    return BIT_OVERFLOW;
  if (b->ptr >= b->limitPtr) {
    b->ptr -= b->bitsConsumed >> 3;
    b->bitsConsumed &= 7;
    b->bitContainer = zstd_rd64(b->ptr);
    return BIT_UNFINISHED;
  }
  if (b->ptr == b->start)
    return (b->bitsConsumed < 64) ? BIT_ENDOFBUFFER : BIT_COMPLETED;
  {
    u32 nbBytes = b->bitsConsumed >> 3;
    int status = BIT_UNFINISHED;
    if (b->ptr - nbBytes < b->start) {
      nbBytes = (u32)(b->ptr - b->start);
      status = BIT_ENDOFBUFFER;
    }
    b->ptr -= nbBytes;
    b->bitsConsumed -= nbBytes * 8;
    b->bitContainer = zstd_rd64(b->ptr);
    return status;
  }
}

static inline int bitin_done(const BITin *b) { return b->ptr == b->start && b->bitsConsumed == 64; }

/* ---- forward bit writer (BITout) ----------------------------------------- */
typedef struct {
  u64 bitContainer;
  u32 bitPos;
  u8 *startPtr;
  u8 *ptr;
  u8 *endPtr; /* one past the last writable byte                      */
} BITout;

static inline void bitout_init(BITout *b, u8 *dst, u32 cap)
{
  b->startPtr = dst;
  b->ptr = dst;
  b->endPtr = dst + cap;
  b->bitContainer = 0;
  b->bitPos = 0;
}

static inline void bitout_add(BITout *b, u32 value, u32 nbBits)
{
  u64 m = (nbBits >= 32) ? 0xFFFFFFFFull : (((u64)1 << nbBits) - 1);
  b->bitContainer |= (u64)(value & m) << b->bitPos;
  b->bitPos += nbBits;
}

static inline void bitout_flush(BITout *b)
{
  u32 nbBytes = b->bitPos >> 3;
  for (u32 i = 0; i < nbBytes; i++) {
    if (b->ptr < b->endPtr)
      *b->ptr = (u8)(b->bitContainer >> (8 * i));
    b->ptr++;
  }
  b->bitPos &= 7;
  b->bitContainer >>= nbBytes * 8;
}

/* Emit the stop bit + trailing partial byte; returns total bytes, 0 on overflow. */
static inline u32 bitout_close(BITout *b)
{
  bitout_add(b, 1, 1);
  while (b->bitPos > 0) {
    if (b->ptr < b->endPtr)
      *b->ptr = (u8)b->bitContainer;
    b->ptr++;
    b->bitContainer >>= 8;
    b->bitPos = (b->bitPos >= 8) ? b->bitPos - 8 : 0;
  }
  return (b->ptr > b->endPtr) ? 0 : (u32)(b->ptr - b->startPtr);
}

/* ---- FSE ------------------------------------------------------------------ */
typedef struct {
  u16 newState;
  u8 symbol;
  u8 nbBits;
} fse_dstate;

/* Decode-side: parse a normalized-count table description, build a decode table. */
int fse_read_ncount(short *normCount, unsigned *maxSVptr, unsigned *tableLogPtr, const u8 *src,
                    u32 srcSize); /* returns bytes read, <0 error */
int fse_build_dtable(fse_dstate *dt, const short *normCount, unsigned maxSV, unsigned tableLog);
int fse_decompress(u8 *dst, u32 dstCap, const u8 *src, u32 srcSize, const fse_dstate *dt, unsigned tableLog);

/* Encode-side compression transform. */
typedef struct {
  int deltaFindState;
  u32 deltaNbBits;
} fse_symtt;
typedef struct {
  u32 tableLog;
  u16 *stateTable;     /* size 1<<tableLog                  */
  fse_symtt symTT[64]; /* enough for LL/ML/OF symbol ranges */
} fse_ctable;
typedef struct {
  u32 value;
  u32 stateLog;
  const u16 *stateTable;
  const fse_symtt *symTT;
} fse_cstate;

int fse_build_ctable(fse_ctable *ct, const short *normCount, unsigned maxSV, unsigned tableLog);
void fse_init_cstate(fse_cstate *cs, const fse_ctable *ct, unsigned symbol);
void fse_encode_symbol(BITout *bo, fse_cstate *cs, unsigned symbol);
void fse_flush_cstate(BITout *bo, fse_cstate *cs);

/* ---- Huffman literals ----------------------------------------------------- */
typedef struct {
  u8 nbBits;
  u8 symbol;
} huf_delt; /* single-symbol decode elt */
typedef struct {
  huf_delt table[1 << HUF_MAX_TABLELOG];
  u32 tableLog;
} huf_dtable;

/* Parse a Huffman tree description, populate dt. Returns bytes read, <0 error. */
int huf_read_dtable(huf_dtable *dt, const u8 *src, u32 srcSize);
/* Decompress a 4-stream ("X1") literal block. Returns 0 on success, -1 error. */
int huf_decompress4x(u8 *dst, u32 dstSize, const u8 *src, u32 srcSize, const huf_dtable *dt);
int huf_decompress1x(u8 *dst, u32 dstSize, const u8 *src, u32 srcSize, const huf_dtable *dt);

/* Encode-side: build code table from a byte histogram, emit tree desc + streams.
 * Returns total compressed bytes written to dst, or <0 if it wouldn't help;
 * *nbStreamsOut is set to 1 or 4 (the size-format the caller must record). */
int huf_compress(u8 *dst, u32 dstCap, const u8 *src, u32 srcSize, int *nbStreamsOut);

/* ---- XXH64 (content checksum) --------------------------------------------- */
u64 xxh64(const void *input, u32 len, u64 seed);

/* ---- constant tables (defined in zstd_common.c) --------------------------- */
extern const u32 LL_base[MAX_LL + 1];
extern const u8 LL_bits[MAX_LL + 1];
extern const u32 ML_base[MAX_ML + 1];
extern const u8 ML_bits[MAX_ML + 1];
extern const short LL_defaultNorm[MAX_LL + 1];
extern const short ML_defaultNorm[MAX_ML + 1];
extern const short OF_defaultNorm[DEF_MAX_OFF + 1];

#endif /* OWOS_ZSTD_INTERNAL_H */
