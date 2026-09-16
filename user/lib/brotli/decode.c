/* decode.c — a from-scratch Brotli decompressor (RFC 7932).
 *
 * Whole-buffer: reads the entire compressed stream and grows a heap output
 * buffer (the stream doesn't carry its decompressed size). Implements the full
 * format used in the wild: the stream/meta-block headers, simple and complex
 * prefix codes, block splits, context modelling for literals and distances, the
 * distance ring buffer with postfix/direct codes, and static-dictionary
 * references with the 121 word transforms. The 122 KiB RFC 7932 dictionary is
 * linked in as a blob.
 *
 * The constant tables (context lookup, prefix/suffix transform data, dictionary
 * offsets, command split, block-length ranges) are the authoritative values
 * from the reference brotli decoder; the algorithms mirror RFC 7932 sections
 * 3-9. Used by the HTTP `br` content-coding and WOFF2 font decompression. */
#include "brotli.h"
#include "owos.h"

/* The static dictionary blob (objcopy of user/lib/brotli/dictionary.bin). */
extern const unsigned char _binary_user_lib_brotli_dictionary_bin_start[];
static const unsigned char *const DICT = _binary_user_lib_brotli_dictionary_bin_start;

/* offsets_by_length / size_bits_by_length for word lengths 0..24 (RFC 7932 §8;
 * the data block for length L is (L << size_bits[L]) bytes, so offset[L+1] =
 * offset[L] + (L << size_bits[L])). offset[24] + 24*32 == 122784 == dict size.
 */
static const unsigned char DICT_SIZE_BITS[25] = {0, 0, 0, 0, 10, 10, 11, 11, 10, 10, 10, 10, 10,
                                                 9, 9, 8, 7, 7,  8,  7,  7,  6,  6,  5,  5};
static const int DICT_OFFSET[25] = {0,      0,      0,      0,      0,      4096,   9216,   21504,  35840,
                                    44032,  53248,  63488,  74752,  87040,  93696,  100864, 104704, 106752,
                                    108928, 113536, 115968, 118528, 119872, 121280, 122016};

/* ------------------------------------------------------------ transforms --- *
 * The RFC 7932 §8 word transforms, in the reference's data-driven form: each of
 * the 121 transforms is {prefix_id, type, suffix_id}; prefix_id/suffix_id index
 * PFXMAP, which maps to a byte offset of a length-prefixed string in PFX. */
enum {
  T_IDENTITY = 0,
  T_OMIT_LAST_1 = 1,
  T_OMIT_LAST_2,
  T_OMIT_LAST_3,
  T_OMIT_LAST_4,
  T_OMIT_LAST_5,
  T_OMIT_LAST_6,
  T_OMIT_LAST_7,
  T_OMIT_LAST_8,
  T_OMIT_LAST_9,
  T_UPPERCASE_FIRST = 10,
  T_UPPERCASE_ALL = 11,
  T_OMIT_FIRST_1 = 12,
  T_OMIT_FIRST_2,
  T_OMIT_FIRST_3,
  T_OMIT_FIRST_4,
  T_OMIT_FIRST_5,
  T_OMIT_FIRST_6,
  T_OMIT_FIRST_7,
  T_OMIT_FIRST_8,
  T_OMIT_FIRST_9,
  T_SHIFT_FIRST = 21,
  T_SHIFT_ALL = 22
};

static const unsigned char PFX[] = "\1 \2, \10 of the \4 of \2s \1.\5 and \4 "
                                   "in \1\"\4 to \2\">\1\n\2. \1]\5 for \3 a \6 "
                                   "that \1\'\6 with \6 from \4 by \1(\6. T"
                                   "he \4 on \4 as \4 is \4ing \2\n\t\1:\3ed "
                                   "\2=\"\4 at \3ly \1,\2=\'\5.com/\7. This \5"
                                   " not \3er \3al \4ful \4ive \5less \4es"
                                   "t \4ize \2\xc2\xa0\4ous \5 the \2e ";

static const unsigned short PFXMAP[50] = {0x00, 0x02, 0x05, 0x0E, 0x13, 0x16, 0x18, 0x1E, 0x23, 0x25, 0x2A, 0x2D, 0x2F,
                                          0x32, 0x34, 0x3A, 0x3E, 0x45, 0x47, 0x4E, 0x55, 0x5A, 0x5C, 0x63, 0x68, 0x6D,
                                          0x72, 0x77, 0x7A, 0x7C, 0x80, 0x83, 0x88, 0x8C, 0x8E, 0x91, 0x97, 0x9F, 0xA5,
                                          0xA9, 0xAD, 0xB2, 0xB7, 0xBD, 0xC2, 0xC7, 0xCA, 0xCF, 0xD5, 0xD8};

/* 121 transforms x {prefix_id, type, suffix_id}. */
static const unsigned char XF[121 * 3] = {
    49, T_IDENTITY,        49, 49, T_IDENTITY,        0,  0,  T_IDENTITY,        0,  49, T_OMIT_FIRST_1,    49,
    49, T_UPPERCASE_FIRST, 0,  49, T_IDENTITY,        47, 0,  T_IDENTITY,        49, 4,  T_IDENTITY,        0,
    49, T_IDENTITY,        3,  49, T_UPPERCASE_FIRST, 49, 49, T_IDENTITY,        6,  49, T_OMIT_FIRST_2,    49,
    49, T_OMIT_LAST_1,     49, 1,  T_IDENTITY,        0,  49, T_IDENTITY,        1,  0,  T_UPPERCASE_FIRST, 0,
    49, T_IDENTITY,        7,  49, T_IDENTITY,        9,  48, T_IDENTITY,        0,  49, T_IDENTITY,        8,
    49, T_IDENTITY,        5,  49, T_IDENTITY,        10, 49, T_IDENTITY,        11, 49, T_OMIT_LAST_3,     49,
    49, T_IDENTITY,        13, 49, T_IDENTITY,        14, 49, T_OMIT_FIRST_3,    49, 49, T_OMIT_LAST_2,     49,
    49, T_IDENTITY,        15, 49, T_IDENTITY,        16, 0,  T_UPPERCASE_FIRST, 49, 49, T_IDENTITY,        12,
    5,  T_IDENTITY,        49, 0,  T_IDENTITY,        1,  49, T_OMIT_FIRST_4,    49, 49, T_IDENTITY,        18,
    49, T_IDENTITY,        17, 49, T_IDENTITY,        19, 49, T_IDENTITY,        20, 49, T_OMIT_FIRST_5,    49,
    49, T_OMIT_FIRST_6,    49, 47, T_IDENTITY,        49, 49, T_OMIT_LAST_4,     49, 49, T_IDENTITY,        22,
    49, T_UPPERCASE_ALL,   49, 49, T_IDENTITY,        23, 49, T_IDENTITY,        24, 49, T_IDENTITY,        25,
    49, T_OMIT_LAST_7,     49, 49, T_OMIT_LAST_1,     26, 49, T_IDENTITY,        27, 49, T_IDENTITY,        28,
    0,  T_IDENTITY,        12, 49, T_IDENTITY,        29, 49, T_OMIT_FIRST_9,    49, 49, T_OMIT_FIRST_7,    49,
    49, T_OMIT_LAST_6,     49, 49, T_IDENTITY,        21, 49, T_UPPERCASE_FIRST, 1,  49, T_OMIT_LAST_8,     49,
    49, T_IDENTITY,        31, 49, T_IDENTITY,        32, 47, T_IDENTITY,        3,  49, T_OMIT_LAST_5,     49,
    49, T_OMIT_LAST_9,     49, 0,  T_UPPERCASE_FIRST, 1,  49, T_UPPERCASE_FIRST, 8,  5,  T_IDENTITY,        21,
    49, T_UPPERCASE_ALL,   0,  49, T_UPPERCASE_FIRST, 10, 49, T_IDENTITY,        30, 0,  T_IDENTITY,        5,
    35, T_IDENTITY,        49, 47, T_IDENTITY,        2,  49, T_UPPERCASE_FIRST, 17, 49, T_IDENTITY,        36,
    49, T_IDENTITY,        33, 5,  T_IDENTITY,        0,  49, T_UPPERCASE_FIRST, 21, 49, T_UPPERCASE_FIRST, 5,
    49, T_IDENTITY,        37, 0,  T_IDENTITY,        30, 49, T_IDENTITY,        38, 0,  T_UPPERCASE_ALL,   0,
    49, T_IDENTITY,        39, 0,  T_UPPERCASE_ALL,   49, 49, T_IDENTITY,        34, 49, T_UPPERCASE_ALL,   8,
    49, T_UPPERCASE_FIRST, 12, 0,  T_IDENTITY,        21, 49, T_IDENTITY,        40, 0,  T_UPPERCASE_FIRST, 12,
    49, T_IDENTITY,        41, 49, T_IDENTITY,        42, 49, T_UPPERCASE_ALL,   17, 49, T_IDENTITY,        43,
    0,  T_UPPERCASE_FIRST, 5,  49, T_UPPERCASE_ALL,   10, 0,  T_IDENTITY,        34, 49, T_UPPERCASE_FIRST, 33,
    49, T_IDENTITY,        44, 49, T_UPPERCASE_ALL,   5,  45, T_IDENTITY,        49, 0,  T_IDENTITY,        33,
    49, T_UPPERCASE_FIRST, 30, 49, T_UPPERCASE_ALL,   30, 49, T_IDENTITY,        46, 49, T_UPPERCASE_ALL,   1,
    49, T_UPPERCASE_FIRST, 34, 0,  T_UPPERCASE_FIRST, 33, 0,  T_UPPERCASE_ALL,   30, 0,  T_UPPERCASE_ALL,   1,
    49, T_UPPERCASE_ALL,   33, 49, T_UPPERCASE_ALL,   21, 49, T_UPPERCASE_ALL,   12, 0,  T_UPPERCASE_ALL,   5,
    49, T_UPPERCASE_ALL,   34, 0,  T_UPPERCASE_ALL,   12, 0,  T_UPPERCASE_FIRST, 30, 0,  T_UPPERCASE_ALL,   34,
    0,  T_UPPERCASE_FIRST, 34};
#define NUM_TRANSFORMS 121

/* Uppercase one UTF-8 rune in place at p; returns the byte length stepped.
 * Brotli's simplified model: ASCII a-z toggles bit 5; a 2-byte sequence toggles
 * bit 5 of byte 2; a 3-byte sequence toggles bit 0/2 of byte 3. */
static int to_upper(unsigned char *p)
{
  if (p[0] < 0xC0) {
    if (p[0] >= 'a' && p[0] <= 'z')
      p[0] ^= 32;
    return 1;
  }
  if (p[0] < 0xE0) {
    p[1] ^= 32;
    return 2;
  }
  p[2] ^= 5;
  return 3;
}

/* Apply transform `t` to dictionary word [word,word+len) into dst; return the
 * transformed length. Mirrors BrotliTransformDictionaryWord (RFC 7932 §8). */
static int transform_word(unsigned char *dst, const unsigned char *word, int len, int t)
{
  const unsigned char *pfx = &PFX[PFXMAP[XF[t * 3 + 0]]];
  int type = XF[t * 3 + 1];
  const unsigned char *sfx = &PFX[PFXMAP[XF[t * 3 + 2]]];
  int idx = 0;

  int plen = *pfx++;
  while (plen--)
    dst[idx++] = *pfx++;

  if (type <= T_OMIT_LAST_9) {
    len -= type; /* identity (0) or omit-last */
  } else if (type >= T_OMIT_FIRST_1 && type <= T_OMIT_FIRST_9) {
    int skip = type - (T_OMIT_FIRST_1 - 1);
    word += skip;
    len -= skip;
  }
  if (len < 0)
    len = 0;

  int core = idx;
  for (int i = 0; i < len; i++)
    dst[idx++] = word[i];

  if (type == T_UPPERCASE_FIRST) {
    if (len > 0)
      to_upper(&dst[core]);
  } else if (type == T_UPPERCASE_ALL) {
    int rem = len, j = core;
    while (rem > 0) {
      int step = to_upper(&dst[j]);
      j += step;
      rem -= step;
    }
  }
  /* T_SHIFT_* never occur in the default RFC dictionary (no params). */

  int slen = *sfx++;
  while (slen--)
    dst[idx++] = *sfx++;
  return idx;
}

/* ------------------------------------------------------------ bit reader --- *
 * LSB-first, bit-indexed over the input. Reads past the end return zero bits so
 * a truncated stream fails on a structural check rather than an OOB read. */
struct br {
  const unsigned char *in;
  long len;    /* bytes */
  long bitpos; /* total bits consumed */
  int error;
};

static void br_init(struct br *b, const unsigned char *in, int len)
{
  b->in = in;
  b->len = len;
  b->bitpos = 0;
  b->error = 0;
}
static unsigned br_peek(struct br *b, int n) /* n <= 24 */
{
  long bp = b->bitpos;
  if (bp < 0) {
    b->error = 1;
    return 0;
  } /* bitpos wrapped past 2^31 via a huge skip */
  long byte = bp >> 3;
  int bit = (int)(bp & 7);
  unsigned long long acc = 0;
  for (int i = 0; i < 5; i++)
    acc |= (unsigned long long)((byte + i < b->len) ? b->in[byte + i] : 0) << (8 * i);
  return (unsigned)((acc >> bit) & ((1u << n) - 1));
}
static unsigned br_read(struct br *b, int n)
{
  if (n == 0)
    return 0;
  unsigned v = br_peek(b, n);
  b->bitpos += n;
  return v;
}
static void br_align(struct br *b) { b->bitpos = (b->bitpos + 7) & ~(long)7; }
static int br_byte(struct br *b) /* precondition: aligned */
{
  long by = b->bitpos >> 3;
  b->bitpos += 8;
  return (by >= 0 && by < b->len) ? b->in[by] : 0;
}

/* ------------------------------------------------------------ huffman -------
 * * Canonical prefix code as a single-level lookup table indexed by `bits`
 * peeked bits; each entry packs (symbol << 4 | code_length). Brotli max length
 * is 15. */
struct huff {
  unsigned short *tab;
  int bits;
};

/* Build a table from per-symbol code lengths. Returns 0 on success. */
static int huff_build(struct huff *h, const unsigned char *lengths, int n)
{
  int count[16] = {0}, i;
  for (i = 0; i < n; i++)
    count[lengths[i]]++;
  count[0] = 0;
  int maxlen = 0;
  for (i = 1; i < 16; i++)
    if (count[i])
      maxlen = i;
  if (maxlen == 0)
    return -1;
  h->bits = maxlen;

  int next[16], code = 0;
  for (i = 1; i <= 15; i++) {
    next[i] = code;
    code = (code + count[i]) << 1;
  }

  int size = 1 << maxlen;
  h->tab = malloc((unsigned)size * sizeof(unsigned short));
  if (!h->tab)
    return -1;
  for (i = 0; i < size; i++)
    h->tab[i] = 0xffff;

  for (i = 0; i < n; i++) {
    int l = lengths[i];
    if (!l)
      continue;
    int c = next[l]++;
    int rev = 0; /* codes are MSB-first; we read LSB-first */
    for (int k = 0; k < l; k++)
      rev |= ((c >> k) & 1) << (l - 1 - k);
    for (int f = rev; f < size; f += (1 << l))
      h->tab[f] = (unsigned short)((i << 4) | l);
  }
  return 0;
}
static void huff_free(struct huff *h)
{
  free(h->tab);
  h->tab = 0;
}

static int huff_decode(struct br *b, const struct huff *h)
{
  unsigned peek = br_peek(b, h->bits);
  unsigned short e = h->tab[peek];
  if (e == 0xffff) {
    b->error = 1;
    return 0;
  }
  b->bitpos += e & 0xf;
  return e >> 4;
}

/* ---------------------------------------------------------- read_huffman --- *
 * Reads a simple or complex prefix-code description (RFC 7932 §3.4/3.5) for an
 * `alphabet`-symbol code into `out`. Returns 0 on success. */
static const unsigned char kCodeLengthCodeOrder[18] = {1, 2, 3, 4, 0, 5, 17, 6, 16, 7, 8, 9, 10, 11, 12, 13, 14, 15};
/* Fixed prefix code for the complex code-length code lengths (indexed by 4 low
 * peeked bits): value and the number of bits it consumes. */
static const unsigned char kClcPrefixBits[16] = {2, 2, 2, 3, 2, 2, 2, 4, 2, 2, 2, 3, 2, 2, 2, 4};
static const unsigned char kClcPrefixValue[16] = {0, 4, 3, 2, 0, 4, 3, 1, 0, 4, 3, 2, 0, 4, 3, 5};

static int bitlen(int x)
{
  int n = 0;
  while (x) {
    n++;
    x >>= 1;
  }
  return n;
}

static int read_huffman(struct br *b, int alphabet, struct huff *out)
{
  int hskip = br_read(b, 2);
  if (hskip == 1) {
    /* Simple prefix code: 1..4 directly-listed symbols. */
    int nsym = br_read(b, 2) + 1;
    int symbits = bitlen(alphabet - 1);
    int syms[4];
    for (int i = 0; i < nsym; i++) {
      syms[i] = br_read(b, symbits);
      if (syms[i] >= alphabet)
        return -1;
    }
    if (nsym == 1) {
      out->bits = 0;
      out->tab = malloc(sizeof(unsigned short));
      if (!out->tab)
        return -1;
      out->tab[0] = (unsigned short)((syms[0] << 4) | 0);
      return 0;
    }
    unsigned char *lens = malloc(alphabet);
    if (!lens)
      return -1;
    for (int i = 0; i < alphabet; i++)
      lens[i] = 0;
    if (nsym == 2) {
      lens[syms[0]] = 1;
      lens[syms[1]] = 1;
    } else if (nsym == 3) {
      lens[syms[0]] = 1;
      lens[syms[1]] = 2;
      lens[syms[2]] = 2;
    } else {
      int treeselect = br_read(b, 1);
      if (treeselect) {
        lens[syms[0]] = 1;
        lens[syms[1]] = 2;
        lens[syms[2]] = 3;
        lens[syms[3]] = 3;
      } else {
        lens[syms[0]] = 2;
        lens[syms[1]] = 2;
        lens[syms[2]] = 2;
        lens[syms[3]] = 2;
      }
    }
    int r = huff_build(out, lens, alphabet);
    free(lens);
    return r;
  }

  /* Complex code: read the code-length code (18 symbols) via the fixed prefix
   * above, build it, then read the alphabet's code lengths through it with the
   * §3.5 RLE repeat extension. */
  unsigned char cl_lens[18] = {0};
  int space = 32, num_codes = 0;
  for (int i = hskip; i < 18; i++) {
    int ix = br_peek(b, 4);
    int v = kClcPrefixValue[ix];
    b->bitpos += kClcPrefixBits[ix];
    cl_lens[kCodeLengthCodeOrder[i]] = (unsigned char)v;
    if (v) {
      num_codes++;
      space -= 32 >> v;
      if (space <= 0)
        break;
    }
  }
  if (!(num_codes == 1 || space == 0))
    return -1;

  struct huff clh;
  if (num_codes == 1) {
    /* A single code-length-code symbol is a 0-bit code: every code length
     * decodes to it consuming no bits (matches the reference encoder, which
     * emits the per-symbol lengths with zero bits in this case). */
    int only = 0;
    for (int i = 0; i < 18; i++)
      if (cl_lens[i]) {
        only = i;
        break;
      }
    clh.bits = 0;
    clh.tab = malloc(sizeof(unsigned short));
    if (!clh.tab)
      return -1;
    clh.tab[0] = (unsigned short)((only << 4) | 0);
  } else if (huff_build(&clh, cl_lens, 18) != 0) {
    return -1;
  }

  unsigned char *lens = malloc(alphabet);
  if (!lens) {
    huff_free(&clh);
    return -1;
  }
  for (int i = 0; i < alphabet; i++)
    lens[i] = 0;

  int symbol = 0, prev_len = 8, repeat = 0, repeat_len = 0;
  long hspace = 32768;
  while (symbol < alphabet && hspace > 0) {
    int code = huff_decode(b, &clh);
    if (b->error) {
      free(lens);
      huff_free(&clh);
      return -1;
    }
    if (code < 16) {
      repeat = 0;
      if (code) {
        lens[symbol] = (unsigned char)code;
        prev_len = code;
        hspace -= 32768 >> code;
      }
      symbol++;
    } else {
      int extra = (code == 16) ? 2 : 3;
      int new_len = (code == 16) ? prev_len : 0;
      int delta = br_read(b, extra);
      if (repeat_len != new_len) {
        repeat = 0;
        repeat_len = new_len;
      }
      int old_repeat = repeat;
      if (repeat > 0)
        repeat = (repeat - 2) << extra;
      repeat += delta + 3;
      int actual = repeat - old_repeat;
      if (symbol + actual > alphabet) {
        free(lens);
        huff_free(&clh);
        return -1;
      }
      if (new_len) {
        for (int k = 0; k < actual; k++)
          lens[symbol + k] = (unsigned char)new_len;
        hspace -= (long)actual << (15 - new_len);
      }
      symbol += actual;
    }
  }
  huff_free(&clh);
  if (hspace != 0) {
    free(lens);
    return -1;
  }
  int r = huff_build(out, lens, alphabet);
  free(lens);
  return r;
}

/* ----------------------------------------------------- DecodeVarLenUint8 --- *
 * RFC 7932: 1 + this is the number of block types / Huffman trees. */
static int read_varlen_uint8(struct br *b)
{
  if (br_read(b, 1) == 0)
    return 0;
  int nbits = br_read(b, 3);
  if (nbits == 0)
    return 1;
  return (1 << nbits) + br_read(b, nbits);
}

/* ------------------------------------------------------- context map ------- *
 * RFC 7932 §7.3. Returns a malloc'd map of `size` bytes and the tree count via
 * *num_trees, or NULL on error. */
static unsigned char *decode_context_map(struct br *b, int size, int *num_trees)
{
  int nt = read_varlen_uint8(b) + 1;
  *num_trees = nt;
  unsigned char *map = malloc(size ? size : 1);
  if (!map)
    return 0;
  for (int i = 0; i < size; i++)
    map[i] = 0;
  if (nt <= 1)
    return map;

  int rlemax = 0;
  if (br_peek(b, 1) & 1) {
    rlemax = (br_peek(b, 5) >> 1) + 1;
    b->bitpos += 5;
  } else
    b->bitpos += 1;

  struct huff h;
  if (read_huffman(b, nt + rlemax, &h) != 0) {
    free(map);
    return 0;
  }

  int i = 0;
  while (i < size) {
    int code = huff_decode(b, &h);
    if (b->error) {
      huff_free(&h);
      free(map);
      return 0;
    }
    if (code == 0) {
      map[i++] = 0;
      continue;
    }
    if (code > rlemax) {
      map[i++] = (unsigned char)(code - rlemax);
      continue;
    }
    int reps = (1 << code) + br_read(b, code);
    if (i + reps > size) {
      huff_free(&h);
      free(map);
      return 0;
    }
    while (reps--)
      map[i++] = 0;
  }
  huff_free(&h);

  if (br_read(b, 1)) { /* inverse move-to-front */
    unsigned char mtf[256];
    for (int j = 0; j < 256; j++)
      mtf[j] = (unsigned char)j;
    for (int j = 0; j < size; j++) {
      int idx = map[j];
      unsigned char val = mtf[idx];
      for (int k = idx; k > 0; k--)
        mtf[k] = mtf[k - 1];
      mtf[0] = val;
      map[j] = val;
    }
  }
  return map;
}

/* ---------------------------------------------------- command split LUT ----
 */
static const unsigned char kInsExtra[24] = {0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 7, 8, 9, 10, 12, 14, 24};
static const unsigned char kCopyExtra[24] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 7, 8, 9, 10, 24};
static const unsigned char kCellPos[11] = {0, 1, 0, 1, 8, 9, 2, 16, 10, 17, 18};
static int ins_offset[24], copy_offset[24];

static void init_cmd_offsets(void)
{
  ins_offset[0] = 0;
  copy_offset[0] = 2;
  for (int i = 0; i < 23; i++) {
    ins_offset[i + 1] = ins_offset[i] + (1 << kInsExtra[i]);
    copy_offset[i + 1] = copy_offset[i] + (1 << kCopyExtra[i]);
  }
}

/* ------------------------------------------------------ block-length code --
 */
static const struct {
  unsigned short offset;
  unsigned char nbits;
} kBlockLenRanges[26] = {{1, 2},     {5, 2},     {9, 2},     {13, 2},    {17, 3},    {25, 3},  {33, 3},
                         {41, 3},    {49, 4},    {65, 4},    {81, 4},    {97, 4},    {113, 5}, {145, 5},
                         {177, 5},   {209, 5},   {241, 6},   {305, 6},   {369, 7},   {497, 8}, {753, 9},
                         {1265, 10}, {2289, 11}, {4337, 12}, {8433, 13}, {16625, 24}};
static long read_block_length(struct br *b, const struct huff *len_tree)
{
  int code = huff_decode(b, len_tree);
  if (code < 0 || code >= 26) {
    b->error = 1;
    return 0;
  }
  return (long)kBlockLenRanges[code].offset + br_read(b, kBlockLenRanges[code].nbits);
}

/* The shared context lookup table (4 modes x 512 bytes). */
static const unsigned char CTXLUT[2048] = {
    /* CONTEXT_LSB6, last byte. */
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30,
    31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59,
    60, 61, 62, 63, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26,
    27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55,
    56, 57, 58, 59, 60, 61, 62, 63, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22,
    23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51,
    52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
    19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
    48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
    /* CONTEXT_LSB6, second last byte. */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* CONTEXT_MSB6, last byte. */
    0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8, 8, 9, 9, 9,
    9, 10, 10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14, 14, 15, 15, 15, 15, 16, 16, 16, 16,
    17, 17, 17, 17, 18, 18, 18, 18, 19, 19, 19, 19, 20, 20, 20, 20, 21, 21, 21, 21, 22, 22, 22, 22, 23, 23, 23, 23, 24,
    24, 24, 24, 25, 25, 25, 25, 26, 26, 26, 26, 27, 27, 27, 27, 28, 28, 28, 28, 29, 29, 29, 29, 30, 30, 30, 30, 31, 31,
    31, 31, 32, 32, 32, 32, 33, 33, 33, 33, 34, 34, 34, 34, 35, 35, 35, 35, 36, 36, 36, 36, 37, 37, 37, 37, 38, 38, 38,
    38, 39, 39, 39, 39, 40, 40, 40, 40, 41, 41, 41, 41, 42, 42, 42, 42, 43, 43, 43, 43, 44, 44, 44, 44, 45, 45, 45, 45,
    46, 46, 46, 46, 47, 47, 47, 47, 48, 48, 48, 48, 49, 49, 49, 49, 50, 50, 50, 50, 51, 51, 51, 51, 52, 52, 52, 52, 53,
    53, 53, 53, 54, 54, 54, 54, 55, 55, 55, 55, 56, 56, 56, 56, 57, 57, 57, 57, 58, 58, 58, 58, 59, 59, 59, 59, 60, 60,
    60, 60, 61, 61, 61, 61, 62, 62, 62, 62, 63, 63, 63, 63,
    /* CONTEXT_MSB6, second last byte. */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* CONTEXT_UTF8, last byte. */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 4, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 12, 16, 12, 12,
    20, 12, 16, 24, 28, 12, 12, 32, 12, 36, 12, 44, 44, 44, 44, 44, 44, 44, 44, 44, 44, 32, 32, 24, 40, 28, 12, 12, 48,
    52, 52, 52, 48, 52, 52, 52, 48, 52, 52, 52, 52, 52, 48, 52, 52, 52, 52, 52, 48, 52, 52, 52, 52, 52, 24, 12, 28, 12,
    12, 12, 56, 60, 60, 60, 56, 60, 60, 60, 56, 60, 60, 60, 60, 60, 56, 60, 60, 60, 60, 60, 56, 60, 60, 60, 60, 60, 24,
    12, 28, 12, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 2, 3, 2, 3, 2, 3, 2, 3, 2,
    3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3,
    2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3,
    /* CONTEXT_UTF8, second last byte. */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    3, 3, 3, 3, 3, 3, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    /* CONTEXT_SIGNED, last byte. */
    0, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
    24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
    24, 24, 24, 24, 24, 24, 24, 24, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32,
    32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32,
    32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40,
    40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40,
    40, 40, 40, 40, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 56,
    /* CONTEXT_SIGNED, second last byte. */
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    5, 5, 5, 5, 5, 5, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 7};

/* --------------------------------------------------- block-switch helper ---
 */
struct blockcat {
  int num_types;
  long len; /* remaining count in the current block */
  struct huff type_tree, len_tree;
  int rb[2]; /* {second-last type, last type} */
  int cur;   /* current block type */
};
static void block_switch(struct br *b, struct blockcat *c)
{
  int sym = huff_decode(b, &c->type_tree);
  long newlen = read_block_length(b, &c->len_tree);
  int t;
  if (sym == 1)
    t = c->rb[1] + 1;
  else if (sym == 0)
    t = c->rb[0];
  else
    t = sym - 2;
  if (t >= c->num_types)
    t -= c->num_types;
  c->rb[0] = c->rb[1];
  c->rb[1] = t;
  c->cur = t;
  c->len = newlen;
}

/* Grow `out` so `need` bytes fit. The doubling is done in 64-bit with a hard
 * ceiling: on -m32 `long` is 32-bit, so a decompression bomb pushing the size
 * past ~1 GiB would otherwise wrap `(need)*2+64` small/negative and hand
 * realloc an undersized buffer the copy loops then overflow. Returns NULL on
 * OOM/overflow. */
#define BROTLI_MAX_OUT (256L * 1024 * 1024)
static unsigned char *grow_out(unsigned char *out, long *cap, long need)
{
  if (need < 0 || need > BROTLI_MAX_OUT)
    return 0;
  if (need <= *cap)
    return out;
  long long nc = (long long)need * 2 + 64;
  if (nc > BROTLI_MAX_OUT)
    nc = BROTLI_MAX_OUT;
  unsigned char *n = realloc(out, (unsigned long)nc);
  if (!n)
    return 0;
  *cap = (long)nc;
  return n;
}

/* =============================================================== decode ====
 */
unsigned char *brotli_decompress(const unsigned char *in, int in_len, int *out_len)
{
  init_cmd_offsets();
  struct br b;
  br_init(&b, in, in_len);

  /* WBITS (RFC 7932 §9.1). */
  int wbits;
  if (br_read(&b, 1) == 0)
    wbits = 16;
  else {
    int n = br_read(&b, 3);
    if (n)
      wbits = 17 + n;
    else {
      n = br_read(&b, 3);
      wbits = n ? 8 + n : 17;
    }
  }
  long max_backward = ((long)1 << wbits) - 16;

  unsigned char *out = 0;
  long pos = 0, cap = 0;
  int dist_rb[4] = {16, 15, 11, 4};
  int dist_rb_idx = 0;

  int last = 0;
  while (!last) {
    /* ------------------------------------------------ meta-block header */
    last = br_read(&b, 1);
    if (last && br_read(&b, 1))
      break; /* ISLASTEMPTY */

    int sel = br_read(&b, 2);
    long mlen = 0;
    int is_uncompressed = 0;
    if (sel == 3) {
      /* Metadata / empty meta-block. */
      if (br_read(&b, 1))
        goto fail; /* reserved bit must be 0 */
      int mbytes = br_read(&b, 2);
      long skip = 0;
      if (mbytes) {
        for (int i = 0; i < mbytes; i++)
          skip |= (long)br_read(&b, 8) << (8 * i);
        skip += 1;
      }
      br_align(&b);
      b.bitpos += skip * 8; /* discard metadata bytes */
      continue;
    } else {
      int nnib = sel + 4;
      for (int i = 0; i < nnib; i++)
        mlen |= (long)br_read(&b, 4) << (4 * i);
      mlen += 1;
      if (!last)
        is_uncompressed = br_read(&b, 1);
    }

    if (is_uncompressed) {
      br_align(&b);
      if (pos + mlen > cap) {
        out = grow_out(out, &cap, pos + mlen);
        if (!out)
          return 0;
      }
      for (long i = 0; i < mlen; i++)
        out[pos++] = (unsigned char)br_byte(&b);
      continue;
    }

    /* ------------------------------------------- compressed meta-block */
    struct blockcat cat[3]; /* 0=L, 1=I, 2=D */
    for (int t = 0; t < 3; t++) {
      cat[t].num_types = read_varlen_uint8(&b) + 1;
      cat[t].rb[0] = 1;
      cat[t].rb[1] = 0;
      cat[t].cur = 0;
      cat[t].type_tree.tab = 0;
      cat[t].len_tree.tab = 0;
      if (cat[t].num_types >= 2) {
        if (read_huffman(&b, cat[t].num_types + 2, &cat[t].type_tree) != 0)
          goto fail_cat;
        if (read_huffman(&b, 26, &cat[t].len_tree) != 0)
          goto fail_cat;
        cat[t].len = read_block_length(&b, &cat[t].len_tree);
      } else {
        cat[t].len = 0x7fffffff; /* no switching this meta-block */
      }
      if (b.error)
        goto fail_cat;
      continue;
    fail_cat:
      for (int u = 0; u <= t; u++) {
        huff_free(&cat[u].type_tree);
        huff_free(&cat[u].len_tree);
      }
      goto fail;
    }

    int npostfix = br_read(&b, 2);
    int ndirect = br_read(&b, 4) << npostfix;

    /* Literal context modes, one per literal block type. */
    unsigned char *cmode = malloc(cat[0].num_types);
    if (!cmode)
      goto fail_trees0;
    for (int i = 0; i < cat[0].num_types; i++)
      cmode[i] = (unsigned char)br_read(&b, 2);

    /* Context maps. */
    int num_lit_htrees, num_dist_htrees;
    unsigned char *lmap = decode_context_map(&b, cat[0].num_types << 6, &num_lit_htrees);
    unsigned char *dmap = lmap ? decode_context_map(&b, cat[2].num_types << 2, &num_dist_htrees) : 0;
    if (!lmap || !dmap) {
      free(cmode);
      free(lmap);
      free(dmap);
      goto fail_trees0;
    }

    /* Huffman tree groups. */
    int dist_alpha = 16 + ndirect + (24 << (npostfix + 1));
    struct huff *lit_tr = malloc(sizeof(struct huff) * num_lit_htrees);
    struct huff *ic_tr = malloc(sizeof(struct huff) * cat[1].num_types);
    struct huff *dist_tr = malloc(sizeof(struct huff) * num_dist_htrees);
    if (!lit_tr || !ic_tr || !dist_tr)
      goto fail_trees;
    for (int i = 0; i < num_lit_htrees; i++)
      lit_tr[i].tab = 0;
    for (int i = 0; i < cat[1].num_types; i++)
      ic_tr[i].tab = 0;
    for (int i = 0; i < num_dist_htrees; i++)
      dist_tr[i].tab = 0;
    for (int i = 0; i < num_lit_htrees; i++)
      if (read_huffman(&b, 256, &lit_tr[i]) != 0)
        goto fail_trees;
    for (int i = 0; i < cat[1].num_types; i++)
      if (read_huffman(&b, 704, &ic_tr[i]) != 0)
        goto fail_trees;
    for (int i = 0; i < num_dist_htrees; i++)
      if (read_huffman(&b, dist_alpha, &dist_tr[i]) != 0)
        goto fail_trees;

    /* Pre-compute the interleaved distance code table (RFC 7932 §4). */
    int *dist_extra = malloc(sizeof(int) * dist_alpha);
    long *dist_base = malloc(sizeof(long) * dist_alpha);
    if (!dist_extra || !dist_base) {
      free(dist_extra);
      free(dist_base);
      goto fail_trees;
    }
    {
      int di = 16;
      for (int j = 0; j < ndirect && di < dist_alpha; j++) {
        dist_extra[di] = 0;
        dist_base[di] = j + 1;
        di++;
      }
      int bits = 1, half = 0;
      while (di < dist_alpha) {
        long base = ndirect + ((((long)(2 + half) << bits) - 4) << npostfix) + 1;
        for (int j = 0; j < (1 << npostfix) && di < dist_alpha; j++) {
          dist_extra[di] = bits;
          dist_base[di] = base + j;
          di++;
        }
        bits += half;
        half ^= 1;
      }
    }

    /* ------------------------------------------------- command loop */
    const unsigned char *lslice = lmap + (cat[0].cur << 6);
    const unsigned char *lut = &CTXLUT[(cmode[cat[0].cur] & 3) << 9];
    const unsigned char *dslice = dmap + (cat[2].cur << 2);

    long remaining = mlen;
    while (remaining > 0) {
      if (cat[1].len == 0) {
        block_switch(&b, &cat[1]);
      }
      cat[1].len--;
      int cmd = huff_decode(&b, &ic_tr[cat[1].cur]);
      if (b.error)
        goto fail_body;

      int cell = cmd >> 6;
      int cpos = kCellPos[cell];
      int copy_code = ((cpos << 3) & 0x18) + (cmd & 7);
      int ins_code = (cpos & 0x18) + ((cmd >> 3) & 7);
      int implicit_dist = (cell >= 2) ? -1 : 0;

      long insert_len = ins_offset[ins_code] + br_read(&b, kInsExtra[ins_code]);
      long copy_len = copy_offset[copy_code] + br_read(&b, kCopyExtra[copy_code]);

      /* Insert literals. */
      for (long k = 0; k < insert_len; k++) {
        if (cat[0].len == 0) {
          block_switch(&b, &cat[0]);
          lslice = lmap + (cat[0].cur << 6);
          lut = &CTXLUT[(cmode[cat[0].cur] & 3) << 9];
        }
        cat[0].len--;
        int p1 = pos >= 1 ? out[pos - 1] : 0;
        int p2 = pos >= 2 ? out[pos - 2] : 0;
        int ctx = lut[p1] | lut[256 + p2];
        int lit = huff_decode(&b, &lit_tr[lslice[ctx]]);
        if (b.error)
          goto fail_body;
        if (pos + 1 > cap) {
          out = grow_out(out, &cap, pos + 1);
          if (!out)
            goto fail_body_noout;
        }
        out[pos++] = (unsigned char)lit;
        if (--remaining == 0)
          break;
      }
      if (remaining == 0)
        break;

      /* Distance. */
      int distance_context = 0;
      long distance;
      if (implicit_dist >= 0) {
        distance_context = 1;
        dist_rb_idx--;
        distance = dist_rb[dist_rb_idx & 3];
      } else {
        int dctx = copy_len > 4 ? 3 : (int)copy_len - 2;
        if (cat[2].len == 0) {
          block_switch(&b, &cat[2]);
          dslice = dmap + (cat[2].cur << 2);
        }
        cat[2].len--;
        int code = huff_decode(&b, &dist_tr[dslice[dctx]]);
        if (b.error)
          goto fail_body;
        if ((code & ~0xf) == 0) {
          /* Short code: pull from the distance ring buffer. */
          int offset = code - 3;
          if (code <= 3) {
            distance_context = 1 >> code;
            distance = dist_rb[(dist_rb_idx - offset) & 3];
            dist_rb_idx -= distance_context;
          } else {
            int index_delta = 3, base;
            if (code < 10)
              base = code - 4;
            else {
              base = code - 10;
              index_delta = 2;
            }
            int delta = ((0x605142 >> (4 * base)) & 0xf) - 3;
            distance = dist_rb[(dist_rb_idx + index_delta) & 3] + delta;
            if (distance <= 0)
              distance = 0x7fffffff;
          }
        } else {
          distance = dist_base[code] + ((long)br_read(&b, dist_extra[code]) << npostfix);
        }
      }

      long max_distance = pos < max_backward ? pos : max_backward;

      if (distance > max_distance) {
        /* Static-dictionary reference. */
        int wl = (int)copy_len;
        if (wl < 4 || wl > 24 || DICT_SIZE_BITS[wl] == 0)
          goto fail_body;
        int shift = DICT_SIZE_BITS[wl];
        long address = distance - max_distance - 1;
        int word_idx = (int)(address & ((1 << shift) - 1));
        int transform_idx = (int)(address >> shift);
        if (transform_idx >= NUM_TRANSFORMS)
          goto fail_body;
        dist_rb_idx += distance_context;
        const unsigned char *word = DICT + DICT_OFFSET[wl] + (long)word_idx * wl;
        if (pos + 64 > cap) {
          out = grow_out(out, &cap, pos + 64);
          if (!out)
            goto fail_body_noout;
        }
        int wn = transform_word(out + pos, word, wl, transform_idx);
        if (wn == 0)
          goto fail_body;
        pos += wn;
        remaining -= wn;
      } else {
        /* LZ77 back-reference. */
        dist_rb[dist_rb_idx & 3] = (int)distance;
        dist_rb_idx++;
        if (pos + copy_len > cap) {
          out = grow_out(out, &cap, pos + copy_len);
          if (!out)
            goto fail_body_noout;
        }
        long src = pos - distance;
        if (src < 0)
          goto fail_body;
        for (long k = 0; k < copy_len; k++)
          out[pos + k] = out[src + k];
        pos += copy_len;
        remaining -= copy_len;
      }
    }

    /* free per-meta-block state */
    free(dist_extra);
    free(dist_base);
    for (int i = 0; i < num_lit_htrees; i++)
      huff_free(&lit_tr[i]);
    for (int i = 0; i < cat[1].num_types; i++)
      huff_free(&ic_tr[i]);
    for (int i = 0; i < num_dist_htrees; i++)
      huff_free(&dist_tr[i]);
    free(lit_tr);
    free(ic_tr);
    free(dist_tr);
    free(lmap);
    free(dmap);
    free(cmode);
    for (int t = 0; t < 3; t++) {
      huff_free(&cat[t].type_tree);
      huff_free(&cat[t].len_tree);
    }
    if (b.error || remaining != 0)
      goto fail;
    continue;

  fail_body:
    free(dist_extra);
    free(dist_base);
  fail_trees:
    if (lit_tr) {
      for (int i = 0; i < num_lit_htrees; i++)
        huff_free(&lit_tr[i]);
    }
    if (ic_tr) {
      for (int i = 0; i < cat[1].num_types; i++)
        huff_free(&ic_tr[i]);
    }
    if (dist_tr) {
      for (int i = 0; i < num_dist_htrees; i++)
        huff_free(&dist_tr[i]);
    }
    free(lit_tr);
    free(ic_tr);
    free(dist_tr);
    free(lmap);
    free(dmap);
    free(cmode);
  fail_trees0:
    for (int t = 0; t < 3; t++) {
      huff_free(&cat[t].type_tree);
      huff_free(&cat[t].len_tree);
    }
    goto fail;
  fail_body_noout:
    free(dist_extra);
    free(dist_base);
    if (lit_tr) {
      for (int i = 0; i < num_lit_htrees; i++)
        huff_free(&lit_tr[i]);
    }
    if (ic_tr) {
      for (int i = 0; i < cat[1].num_types; i++)
        huff_free(&ic_tr[i]);
    }
    if (dist_tr) {
      for (int i = 0; i < num_dist_htrees; i++)
        huff_free(&dist_tr[i]);
    }
    free(lit_tr);
    free(ic_tr);
    free(dist_tr);
    free(lmap);
    free(dmap);
    free(cmode);
    for (int t = 0; t < 3; t++) {
      huff_free(&cat[t].type_tree);
      huff_free(&cat[t].len_tree);
    }
    return 0;
  }

  *out_len = (int)pos;
  if (!out) {
    out = malloc(1);
  } /* empty stream -> non-NULL */
  return out;

fail:
  free(out);
  return 0;
}
