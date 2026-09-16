/* bits.h — an MSB-first bit reader over an H.264 RBSP, with Exp-Golomb codes.
 *
 * H.264 NAL payloads carry emulation-prevention bytes: any 00 00 00/01/02/03 in
 * the payload is stored as 00 00 03 xx so the pattern never looks like a start
 * code. nal_to_rbsp() (nal.c) strips those 03 bytes once, up front, so this
 * reader sees clean RBSP and never has to special-case them mid-read — unlike
 * the MPEG-1 reader, whose stream has no such escaping. Reads past the end
 * return 0 bits and leave the reader flagged `overrun`, which every parser
 * treats as a format error.
 */
#ifndef OWOS_H264_BITS_H
#define OWOS_H264_BITS_H

typedef struct {
  const unsigned char *buf;
  unsigned size; /* total bytes                                 */
  unsigned pos;  /* current byte                                */
  unsigned bit;  /* bits consumed in buf[pos] (0..7, MSB first) */
  int overrun;   /* set once a read went past the buffer        */
} bitr;

static inline void br_init(bitr *b, const unsigned char *buf, unsigned size)
{
  b->buf = buf;
  b->size = size;
  b->pos = 0;
  b->bit = 0;
  b->overrun = 0;
}

static inline unsigned br_u1(bitr *b)
{
  unsigned v;
  if (b->pos >= b->size) {
    b->overrun = 1;
    return 0;
  }
  v = (b->buf[b->pos] >> (7 - b->bit)) & 1u;
  if (++b->bit == 8) {
    b->bit = 0;
    b->pos++;
  }
  return v;
}

static inline unsigned br_u(bitr *b, int n)
{
  unsigned v = 0;
  while (n-- > 0)
    v = (v << 1) | br_u1(b);
  return v;
}

static inline unsigned br_show(bitr *b, int n)
{
  bitr t = *b;
  return br_u(&t, n);
}

static inline void br_skip(bitr *b, int n)
{
  while (n-- > 0)
    br_u1(b);
}

static inline int br_byte_aligned(const bitr *b) { return b->bit == 0; }

static inline void br_align(bitr *b)
{
  if (b->bit) {
    b->bit = 0;
    b->pos++;
  }
}

static inline int br_bits_left(const bitr *b) { return (int)(b->size - b->pos) * 8 - (int)b->bit; }

/* ue(v): unsigned Exp-Golomb. Count leading zeros z, then read z more bits;
 * value = 2^z - 1 + those bits. A runaway (corrupt) prefix trips overrun. */
static inline unsigned br_ue(bitr *b)
{
  int z = 0;
  while (br_u1(b) == 0) {
    if (++z > 31 || b->overrun) {
      b->overrun = 1;
      return 0;
    }
  }
  return ((1u << z) - 1u) + br_u(b, z);
}

/* se(v): signed Exp-Golomb, mapping the ue code k to 0,+1,-1,+2,-2,... */
static inline int br_se(bitr *b)
{
  unsigned k = br_ue(b);
  int m = (int)((k + 1) >> 1);
  return (k & 1) ? m : -m;
}

/* more_rbsp_data(): true if any bit before the rbsp_stop_one_bit remains. Used
 * where a syntax element list is terminated by the trailing bits rather than a
 * count (e.g. ref_pic_list_modification is count-terminated, so this is only
 * needed by a few structures). */
static inline int br_more_rbsp_data(const bitr *b)
{
  /* The rbsp_stop_one_bit is the last set bit in the buffer; data remains iff
   * the current position is before it. */
  int cur = (int)b->pos * 8 + (int)b->bit, i, stop = -1;
  if (b->pos >= b->size)
    return 0;
  for (i = (int)b->size * 8 - 1; i >= cur; i--)
    if (b->buf[i >> 3] & (1 << (7 - (i & 7)))) {
      stop = i;
      break;
    }
  return stop > cur;
}

#endif /* OWOS_H264_BITS_H */
