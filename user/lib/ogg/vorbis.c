/* vorbis.c, a from-scratch Vorbis I decoder (Xiph "Vorbis I specification") */
#include "ogg.h"

#ifdef OGG_HOST_TEST
#include <math.h>
#include <stdlib.h>
#include <string.h>
#else
#include "math.h"
#include "owos.h"
#endif

#ifndef NULL
#define NULL ((void *)0)
#endif
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MDCT_SCALE 1.0f
static double g_pcm_scale = 32768.0;
#ifdef OGG_HOST_TEST
void vorbis_set_scale(double s) { g_pcm_scale = s; }
#endif

/* floor1 inverse-dB lookup: index (0..255) -> linear multiplier */
static float inv_db[256];
static int inv_db_ready;
static void inv_db_init(void)
{
  double a = -6.9727444391292492; /* log10(1.0649863e-07) */
  double b = -0.0820797818846034; /* log10(0.82788260) */
  for (int i = 0; i < 256; i++)
    inv_db[i] = (float)pow(10.0, a + (b - a) * (i / 255.0));
  inv_db_ready = 1;
}

/* bit reader */
typedef struct {
  const uint8_t *data;
  uint32_t len;
  uint32_t bytepos;
  int bitpos; /* 0..7 */
  int eop;
} bitr;

static void br_init(bitr *b, const uint8_t *d, uint32_t n)
{
  b->data = d;
  b->len = n;
  b->bytepos = 0;
  b->bitpos = 0;
  b->eop = 0;
}

static int br_bit(bitr *b)
{
  int v;
  if (b->bytepos >= b->len) {
    b->eop = 1;
    return 0;
  }
  v = (b->data[b->bytepos] >> b->bitpos) & 1;
  if (++b->bitpos == 8) {
    b->bitpos = 0;
    b->bytepos++;
  }
  return v;
}

static uint32_t br_bits(bitr *b, int n)
{
  uint32_t v = 0;
  for (int i = 0; i < n; i++)
    v |= (uint32_t)br_bit(b) << i;
  return v;
}

/* ilog(x): bits needed to represent x, i.e. floor(log2(x))+1 for x>0, else 0 */
static int ilog(int x)
{
  int n = 0;
  while (x > 0) {
    n++;
    x >>= 1;
  }
  return n;
}

static double float32_unpack(uint32_t x)
{
  uint32_t mant = x & 0x1fffff;
  int sign = x & 0x80000000u;
  int expo = (x & 0x7fe00000u) >> 21;
  double v = (double)mant;
  if (sign)
    v = -v;
  return ldexp(v, expo - 788);
}

static int lookup1_values(int entries, int dim)
{
  int r = (int)floor(exp(log((double)entries) / dim));
  for (;;) {
    double p = 1;
    int i;
    for (i = 0; i < dim; i++)
      p *= (r + 1);
    if (p <= entries)
      r++;
    else
      break;
  }
  for (;;) {
    double p = 1;
    int i;
    for (i = 0; i < dim; i++)
      p *= r;
    if (p > entries && r > 0)
      r--;
    else
      break;
  }
  return r;
}

/* codebook */
typedef struct {
  int dimensions;
  int entries;
  /* Huffman decode tree (arrays; node 0 = root) */
  int *c0, *c1, *leaf;
  int nnodes;
  /* VQ value vectors (lookup_type != 0), else NULL */
  float *value; /* entries*dimensions */
} codebook;

static void cb_free(codebook *c)
{
  free(c->c0);
  free(c->c1);
  free(c->leaf);
  free(c->value);
}

static int tree_child(codebook *c, int node, int bit, int alloc)
{
  int *arr = bit ? c->c1 : c->c0;
  if (arr[node] >= 0)
    return arr[node];
  if (!alloc)
    return -1;
  int m = c->nnodes++;
  c->c0[m] = c->c1[m] = -1;
  c->leaf[m] = -1;
  arr = bit ? c->c1 : c->c0; /* arrays unchanged (fixed cap) */
  arr[node] = m;
  return m;
}

/* Build the canonical codeword tree from code lengths (0 = unused entry) */
static int cb_build_tree(codebook *c, const uint8_t *lengths)
{
  int cap = c->entries * 2 + 2;
  c->c0 = malloc(cap * sizeof(int));
  c->c1 = malloc(cap * sizeof(int));
  c->leaf = malloc(cap * sizeof(int));
  if (!c->c0 || !c->c1 || !c->leaf)
    return OGG_ENOMEM;
  c->nnodes = 1;
  c->c0[0] = c->c1[0] = -1;
  c->leaf[0] = -1;

  uint32_t avail[33];
  for (int i = 0; i < 33; i++)
    avail[i] = 0;
  int i, k;
  for (k = 0; k < c->entries; k++)
    if (lengths[k] > 0)
      break;
  if (k == c->entries)
    return OGG_OK; /* empty book (all unused) */

/* insert helper: place `code` (top-aligned in 32 bits) of `len` bits */
#define INSERT(code, len, entry)                                                                                       \
  do {                                                                                                                 \
    int node = 0;                                                                                                      \
    for (int bb = 0; bb < (len); bb++) {                                                                               \
      int bit = ((code) >> (31 - bb)) & 1;                                                                             \
      node = tree_child(c, node, bit, 1);                                                                              \
      if (node < 0)                                                                                                    \
        return OGG_EFORMAT;                                                                                            \
    }                                                                                                                  \
    c->leaf[node] = (entry);                                                                                           \
  } while (0)

  INSERT(0u, lengths[k], k);
  for (i = 1; i <= lengths[k]; i++)
    avail[i] = 1u << (32 - i);
  for (i = k + 1; i < c->entries; i++) {
    int z = lengths[i], y;
    uint32_t res;
    if (z == 0)
      continue;
    while (z > 0 && !avail[z])
      z--;
    if (z == 0)
      return OGG_EFORMAT; /* over-full tree */
    res = avail[z];
    avail[z] = 0;
    INSERT(res, lengths[i], i);
    if (z != lengths[i])
      for (y = lengths[i]; y > z; y--)
        avail[y] = res + (1u << (32 - y));
  }
#undef INSERT
  return OGG_OK;
}

static int cb_decode(codebook *c, bitr *b)
{
  int node = 0;
  if (c->leaf[0] >= 0)
    return c->leaf[0]; /* degenerate single entry */
  for (;;) {
    int bit = br_bit(b);
    if (b->eop)
      return -1;
    node = bit ? c->c1[node] : c->c0[node];
    if (node < 0)
      return -1;
    if (c->leaf[node] >= 0)
      return c->leaf[node];
  }
}

static int cb_parse(codebook *c, bitr *b)
{
  memset(c, 0, sizeof *c);
  if (br_bits(b, 24) != 0x564342)
    return OGG_EFORMAT; /* 'BCV' sync */
  c->dimensions = br_bits(b, 16);
  c->entries = br_bits(b, 24);
  if (c->entries <= 0 || c->dimensions <= 0 || c->entries > (1 << 24))
    return OGG_EFORMAT;

  uint8_t *lengths = malloc(c->entries);
  if (!lengths)
    return OGG_ENOMEM;
  int ordered = br_bit(b);
  if (!ordered) {
    int sparse = br_bit(b);
    for (int i = 0; i < c->entries; i++) {
      if (sparse) {
        if (br_bit(b))
          lengths[i] = br_bits(b, 5) + 1;
        else
          lengths[i] = 0;
      } else
        lengths[i] = br_bits(b, 5) + 1;
    }
  } else {
    int cur = 0, curlen = br_bits(b, 5) + 1;
    while (cur < c->entries) {
      int num = br_bits(b, ilog(c->entries - cur));
      if (cur + num > c->entries) {
        free(lengths);
        return OGG_EFORMAT;
      }
      for (int j = 0; j < num; j++)
        lengths[cur++] = curlen;
      curlen++;
    }
  }

  int rc = cb_build_tree(c, lengths);
  free(lengths);
  if (rc != OGG_OK)
    return rc;

  int lookup = br_bits(b, 4);
  if (lookup == 1 || lookup == 2) {
    double minv = float32_unpack(br_bits(b, 32));
    double delta = float32_unpack(br_bits(b, 32));
    int vbits = br_bits(b, 4) + 1;
    int seqp = br_bit(b);
    int lvals = (lookup == 1) ? lookup1_values(c->entries, c->dimensions) : c->entries * c->dimensions;
    if (lvals < 0)
      return OGG_EFORMAT;
    float *mult = malloc((lvals ? lvals : 1) * sizeof(float));
    if (!mult)
      return OGG_ENOMEM;
    for (int i = 0; i < lvals; i++)
      mult[i] = (float)br_bits(b, vbits);

    c->value = malloc((unsigned long)c->entries * c->dimensions * sizeof(float));
    if (!c->value) {
      free(mult);
      return OGG_ENOMEM;
    }
    if (lookup == 1) {
      for (int e = 0; e < c->entries; e++) {
        double last = 0;
        int idiv = 1;
        for (int j = 0; j < c->dimensions; j++) {
          int off = (e / idiv) % lvals;
          double v = mult[off] * delta + minv + last;
          c->value[e * c->dimensions + j] = (float)v;
          if (seqp)
            last = v;
          idiv *= lvals;
        }
      }
    } else {
      for (int e = 0; e < c->entries; e++) {
        double last = 0;
        for (int j = 0; j < c->dimensions; j++) {
          int off = e * c->dimensions + j;
          double v = mult[off] * delta + minv + last;
          c->value[e * c->dimensions + j] = (float)v;
          if (seqp)
            last = v;
        }
      }
    }
    free(mult);
  } else if (lookup != 0) {
    return OGG_EFORMAT;
  }
  return OGG_OK;
}

/* floor 1 */
#define FLOOR1_MAX_X 65
typedef struct {
  int partitions;
  int partition_class[32];
  int class_dim[16], class_sub[16], class_book[16];
  int class_subbook[16][8];
  int multiplier;
  int rangebits;
  int values; /* count of X coordinates (<= 65) */
  int X[FLOOR1_MAX_X];
  int sorted[FLOOR1_MAX_X];                  /* indices sorted by X ascending */
  int low[FLOOR1_MAX_X], high[FLOOR1_MAX_X]; /* neighbors, per unsorted i */
} floor1;

static int f1_parse(floor1 *f, bitr *b)
{
  memset(f, 0, sizeof *f);
  f->partitions = br_bits(b, 5);
  int maxclass = -1;
  for (int i = 0; i < f->partitions; i++) {
    f->partition_class[i] = br_bits(b, 4);
    if (f->partition_class[i] > maxclass)
      maxclass = f->partition_class[i];
  }
  for (int c = 0; c <= maxclass; c++) {
    f->class_dim[c] = br_bits(b, 3) + 1;
    f->class_sub[c] = br_bits(b, 2);
    if (f->class_sub[c] > 0)
      f->class_book[c] = br_bits(b, 8);
    else
      f->class_book[c] = 0;
    for (int j = 0; j < (1 << f->class_sub[c]); j++)
      f->class_subbook[c][j] = (int)br_bits(b, 8) - 1;
  }
  f->multiplier = br_bits(b, 2) + 1;
  f->rangebits = br_bits(b, 4);
  f->X[0] = 0;
  f->X[1] = 1 << f->rangebits;
  f->values = 2;
  for (int i = 0; i < f->partitions; i++) {
    int cls = f->partition_class[i];
    for (int j = 0; j < f->class_dim[cls]; j++) {
      if (f->values >= FLOOR1_MAX_X)
        return OGG_EFORMAT;
      f->X[f->values++] = br_bits(b, f->rangebits);
    }
  }
  /* precompute sort order + neighbors */
  for (int i = 0; i < f->values; i++)
    f->sorted[i] = i;
  for (int i = 0; i < f->values; i++)
    for (int j = i + 1; j < f->values; j++)
      if (f->X[f->sorted[j]] < f->X[f->sorted[i]]) {
        int t = f->sorted[i];
        f->sorted[i] = f->sorted[j];
        f->sorted[j] = t;
      }
  for (int i = 2; i < f->values; i++) {
    int lo = -1, hi = -1;
    for (int j = 0; j < i; j++) {
      if (f->X[j] < f->X[i] && (lo < 0 || f->X[j] > f->X[lo]))
        lo = j;
      if (f->X[j] > f->X[i] && (hi < 0 || f->X[j] < f->X[hi]))
        hi = j;
    }
    f->low[i] = lo;
    f->high[i] = hi;
  }
  return OGG_OK;
}

static int render_point(int x0, int y0, int x1, int y1, int X)
{
  int dy = y1 - y0, adx = x1 - x0, ady = dy < 0 ? -dy : dy;
  int off = (ady * (X - x0)) / adx;
  return dy < 0 ? y0 - off : y0 + off;
}

static void render_line(int x0, int y0, int x1, int y1, float *out, int n)
{
  int dy = y1 - y0, adx = x1 - x0;
  int ady = dy < 0 ? -dy : dy;
  int base = dy / adx;
  int sy = dy < 0 ? base - 1 : base + 1;
  int x = x0, y = y0, err = 0;
  ady -= (base < 0 ? -base : base) * adx;
  if (x0 >= 0 && x0 < n)
    out[x0] = inv_db[y & 0xff];
  for (x = x0 + 1; x < x1; x++) {
    err += ady;
    if (err >= adx) {
      err -= adx;
      y += sy;
    } else
      y += base;
    if (x >= 0 && x < n)
      out[x] = inv_db[y < 0 ? 0 : y > 255 ? 255 : y];
  }
}

/* Decode a floor1 packet for one channel into `out` */
static int f1_decode(floor1 *f, codebook *books, int nbooks, bitr *b, float *out, int n2)
{
  static const int RANGE[4] = {256, 128, 86, 64};
  int Y[FLOOR1_MAX_X], step2[FLOOR1_MAX_X], finalY[FLOOR1_MAX_X];
  if (!br_bit(b))
    return 0; /* channel unused this frame */

  int range = RANGE[f->multiplier - 1];
  int nbits = ilog(range - 1);
  Y[0] = br_bits(b, nbits);
  Y[1] = br_bits(b, nbits);
  int offset = 2;
  for (int i = 0; i < f->partitions; i++) {
    int cls = f->partition_class[i];
    int cdim = f->class_dim[cls];
    int cbits = f->class_sub[cls];
    int csub = (1 << cbits) - 1;
    int cval = 0;
    if (cbits > 0) {
      if (f->class_book[cls] >= nbooks)
        return OGG_EFORMAT;
      cval = cb_decode(&books[f->class_book[cls]], b);
      if (cval < 0)
        return 0; /* EOP -> treat as silence */
    }
    for (int j = 0; j < cdim; j++) {
      int book = f->class_subbook[cls][cval & csub];
      cval >>= cbits;
      if (book >= 0) {
        if (book >= nbooks)
          return OGG_EFORMAT;
        int v = cb_decode(&books[book], b);
        Y[offset + j] = v < 0 ? 0 : v;
      } else
        Y[offset + j] = 0;
    }
    offset += cdim;
  }

  /* amplitude unwrap (spec 7.4 step 2) */
  step2[0] = step2[1] = 1;
  finalY[0] = Y[0];
  finalY[1] = Y[1];
  for (int i = 2; i < f->values; i++) {
    int lo = f->low[i], hi = f->high[i];
    int pred = render_point(f->X[lo], finalY[lo], f->X[hi], finalY[hi], f->X[i]);
    int val = Y[i];
    int highroom = range - pred, lowroom = pred;
    int room = (highroom < lowroom ? highroom : lowroom) * 2;
    if (val) {
      step2[lo] = step2[hi] = step2[i] = 1;
      if (val >= room) {
        if (highroom > lowroom)
          finalY[i] = val - lowroom + pred;
        else
          finalY[i] = pred - val + highroom - 1;
      } else {
        if (val & 1)
          finalY[i] = pred - (val + 1) / 2;
        else
          finalY[i] = pred + val / 2;
      }
    } else {
      step2[i] = 0;
      finalY[i] = pred;
    }
  }

  /* curve synthesis (spec 7.4 step 3/4) */
  int hx = 0, hy = 0;
  int lx = 0, ly = finalY[f->sorted[0]] * f->multiplier;
  for (int i = 1; i < f->values; i++) {
    int cur = f->sorted[i];
    if (!step2[cur])
      continue;
    hy = finalY[cur] * f->multiplier;
    hx = f->X[cur];
    render_line(lx, ly, hx > n2 ? n2 : hx, hy, out, n2);
    lx = hx;
    ly = hy;
  }
  if (lx < n2) {
    /* extend the last value flat to the end */
    float v = inv_db[(ly < 0 ? 0 : ly > 255 ? 255 : ly) & 0xff];
    for (int x = lx; x < n2; x++)
      out[x] = v;
  }
  return 1;
}

/* residue */
typedef struct {
  int type;
  int begin, end;
  int part_size;
  int classifications;
  int classbook;
  int cascade[64];
  int books[64][8];
} residue;

static int res_parse(residue *r, bitr *b)
{
  memset(r, 0, sizeof *r);
  r->begin = br_bits(b, 24);
  r->end = br_bits(b, 24);
  r->part_size = br_bits(b, 24) + 1;
  r->classifications = br_bits(b, 6) + 1;
  r->classbook = br_bits(b, 8);
  for (int i = 0; i < r->classifications; i++) {
    int high = 0, low = br_bits(b, 3);
    if (br_bit(b))
      high = br_bits(b, 5);
    r->cascade[i] = high * 8 + low;
  }
  for (int i = 0; i < r->classifications; i++)
    for (int j = 0; j < 8; j++)
      r->books[i][j] = (r->cascade[i] & (1 << j)) ? (int)br_bits(b, 8) : -1;
  return OGG_OK;
}

/* Decode `ch` residue vectors of `n` values each into `out[ch]` */
static int res_decode(residue *r, codebook *books, int nbooks, bitr *b, int type, float **out, int ch, int n,
                      const int *do_not, float *scratch /* type-2 aggregate, len ch*n */)
{
  int nn = (type == 2) ? n * ch : n;
  float *vec[VORBIS_MAX_CHANNELS];
  int all_skip = 1;
  if (type == 2) {
    for (int c = 0; c < ch; c++)
      if (!do_not[c])
        all_skip = 0;
    if (all_skip)
      return OGG_OK;
    memset(scratch, 0, (unsigned long)nn * sizeof(float));
    vec[0] = scratch;
  } else {
    for (int c = 0; c < ch; c++)
      vec[c] = out[c];
  }

  if (r->classbook >= nbooks)
    return OGG_EFORMAT;
  codebook *cbk = &books[r->classbook];
  int cpc = cbk->dimensions; /* classwords per codeword */
  int end = r->end > nn ? nn : r->end;
  int nread = end - r->begin;
  if (nread <= 0)
    return OGG_OK;
  int ptr = nread / r->part_size; /* partitions to read */
  int vch = (type == 2) ? 1 : ch;

  /* classifications[channel][partition] */
  int (*cls)[512] = NULL;
  static int clsbuf[VORBIS_MAX_CHANNELS][512];
  cls = clsbuf;
  if (ptr > 512)
    return OGG_EFORMAT;

  for (int pass = 0; pass < 8; pass++) {
    int pc = 0;
    while (pc < ptr) {
      if (pass == 0) {
        for (int c = 0; c < vch; c++) {
          if (type != 2 && do_not[c])
            continue;
          int temp = cb_decode(cbk, b);
          if (temp < 0) {
            pass = 8;
            goto done_partition;
          }
          for (int i = cpc - 1; i >= 0; i--) {
            if (pc + i < ptr)
              cls[c][pc + i] = temp % r->classifications;
            temp /= r->classifications;
          }
        }
      }
      for (int i = 0; i < cpc && pc < ptr; i++, pc++) {
        for (int c = 0; c < vch; c++) {
          if (type != 2 && do_not[c])
            continue;
          int vqclass = cls[c][pc];
          int book = r->books[vqclass][pass];
          if (book < 0)
            continue;
          if (book >= nbooks)
            return OGG_EFORMAT;
          codebook *vb = &books[book];
          int dim = vb->dimensions;
          int off = r->begin + pc * r->part_size;
          float *tgt = vec[c];
          if (type == 0) {
            int stp = r->part_size / dim;
            for (int s = 0; s < stp; s++) {
              int e = cb_decode(vb, b);
              if (e < 0)
                break;
              if (!vb->value)
                continue;
              for (int d = 0; d < dim; d++)
                tgt[off + s + d * stp] += vb->value[e * dim + d];
            }
          } else { /* type 1 and 2 aggregate */
            int pos = 0;
            while (pos < r->part_size) {
              int e = cb_decode(vb, b);
              if (e < 0)
                break;
              if (vb->value)
                for (int d = 0; d < dim; d++)
                  tgt[off + pos + d] += vb->value[e * dim + d];
              pos += dim;
            }
          }
        }
      }
    }
  done_partition:;
  }

  if (type == 2) {
    for (int j = 0; j < nn; j++)
      out[j % ch][j / ch] = scratch[j];
  }
  return OGG_OK;
}

/* mapping */
typedef struct {
  int submaps;
  int coupling_steps;
  int mag[256], ang[256];
  int mux[VORBIS_MAX_CHANNELS];
  int submap_floor[16], submap_residue[16];
} mapping;

static int map_parse(mapping *m, bitr *b, int channels, int nfloors, int nresidues)
{
  memset(m, 0, sizeof *m);
  m->submaps = br_bit(b) ? br_bits(b, 4) + 1 : 1;
  if (br_bit(b)) {
    m->coupling_steps = br_bits(b, 8) + 1;
    int cb = ilog(channels - 1);
    for (int i = 0; i < m->coupling_steps; i++) {
      m->mag[i] = br_bits(b, cb);
      m->ang[i] = br_bits(b, cb);
      if (m->mag[i] == m->ang[i] || m->mag[i] >= channels || m->ang[i] >= channels)
        return OGG_EFORMAT;
    }
  }
  if (br_bits(b, 2) != 0)
    return OGG_EFORMAT; /* reserved */
  if (m->submaps > 1)
    for (int c = 0; c < channels; c++)
      m->mux[c] = br_bits(b, 4);
  for (int s = 0; s < m->submaps; s++) {
    br_bits(b, 8); /* unused time config placeholder */
    m->submap_floor[s] = br_bits(b, 8);
    m->submap_residue[s] = br_bits(b, 8);
    if (m->submap_floor[s] >= nfloors || m->submap_residue[s] >= nresidues)
      return OGG_EFORMAT;
  }
  return OGG_OK;
}

/* decoder */
typedef struct {
  int blockflag, mapping;
} vmode;

struct vorbis_decoder {
  vorbis_info info;
  int n0, n1; /* short, long blocksizes */

  codebook *books;
  int nbooks;
  int *floor_type;
  floor1 *floors;
  int nfloors;
  residue *residues;
  int nresidues;
  int *residue_type;
  mapping *maps;
  int nmaps;
  vmode *modes;
  int nmodes;

  /* precomputed windows for the four long/short slope combinations */
  float *win_long, *win_short;

  /* per-channel working + overlap state */
  float *pcmbuf[VORBIS_MAX_CHANNELS]; /* IMDCT time output, len n1 */
  float *residuebuf[VORBIS_MAX_CHANNELS];
  float *floorbuf[VORBIS_MAX_CHANNELS];
  float *hold[VORBIS_MAX_CHANNELS]; /* previous block right half, len n1/2 */
  int prev_n;                       /* previous block size, 0 if none */
  float *res_scratch;               /* type-2 aggregate */
  int first_frame;

  /* imdct twiddles */
  double *cos_long, *cos_short; /* unused in direct path */
};

/* direct inverse MDCT (O(n^2/4)); correct reference, tuned to libvorbis */
#ifdef OGG_HOST_TEST
double g_imdct_num = 2.0; /* tunable numerator; scale = g_imdct_num / n */
void vorbis_set_imdct(double s) { g_imdct_num = s; }
#else
static const double g_imdct_num = 2.0;
#endif
static void imdct(const float *X, float *out, int n)
{
  int n2 = n / 2;
  double scale = g_imdct_num / n;
  for (int i = 0; i < n; i++) {
    double sum = 0;
    double a = (M_PI / n) * (2.0 * i + 1.0 + n2);
    for (int k = 0; k < n2; k++)
      sum += X[k] * cos(a * (k + 0.5));
    out[i] = (float)(sum * scale);
  }
}

static void build_window(float *w, int n, int short_n, int prev_short, int next_short)
{
  int left_n = (prev_short ? short_n : n) / 2;
  int right_n = (next_short ? short_n : n) / 2;
  int left_start = prev_short ? (n / 4 - short_n / 4) : 0;
  int left_end = left_start + left_n;
  int right_start = next_short ? (n * 3 / 4 - short_n / 4) : n / 2;
  int right_end = right_start + right_n;
  for (int i = 0; i < n; i++) {
    if (i < left_start)
      w[i] = 0.0f;
    else if (i < left_end) {
      double x = ((i - left_start) + 0.5) / left_n * (M_PI / 2);
      double s = sin(x);
      w[i] = (float)sin((M_PI / 2) * s * s);
    } else if (i < right_start)
      w[i] = 1.0f;
    else if (i < right_end) {
      int j = right_n - 1 - (i - right_start);
      double x = (j + 0.5) / right_n * (M_PI / 2);
      double s = sin(x);
      w[i] = (float)sin((M_PI / 2) * s * s);
    } else
      w[i] = 0.0f;
  }
}

static int alloc_buffers(vorbis_decoder *v)
{
  int n1 = v->n1;
  for (int c = 0; c < v->info.channels; c++) {
    v->pcmbuf[c] = malloc(n1 * sizeof(float));
    v->residuebuf[c] = malloc((n1 / 2) * sizeof(float));
    v->floorbuf[c] = malloc((n1 / 2) * sizeof(float));
    v->hold[c] = calloc(n1 / 2, sizeof(float));
    if (!v->pcmbuf[c] || !v->residuebuf[c] || !v->floorbuf[c] || !v->hold[c])
      return OGG_ENOMEM;
  }
  v->res_scratch = malloc((unsigned long)(n1 / 2) * VORBIS_MAX_CHANNELS * sizeof(float));
  v->win_long = malloc(n1 * sizeof(float));
  v->win_short = malloc(v->n0 * sizeof(float));
  if (!v->res_scratch || !v->win_long || !v->win_short)
    return OGG_ENOMEM;
  build_window(v->win_short, v->n0, v->n0, 0, 0);
  build_window(v->win_long, v->n1, v->n0, 0, 0); /* rebuilt per-packet when transitions occur */
  return OGG_OK;
}

/* id + setup header parse */
static int check_head(const uint8_t *p, unsigned len, int type)
{
  return len >= 7 && p[0] == type && p[1] == 'v' && p[2] == 'o' && p[3] == 'r' && p[4] == 'b' && p[5] == 'i' &&
         p[6] == 's';
}

static int parse_id(vorbis_decoder *v, const uint8_t *p, unsigned len)
{
  if (!check_head(p, len, 1) || len < 30)
    return OGG_EFORMAT;
  bitr b;
  br_init(&b, p + 7, len - 7);
  if (br_bits(&b, 32) != 0)
    return OGG_EFORMAT; /* vorbis version */
  v->info.channels = br_bits(&b, 8);
  v->info.sample_rate = br_bits(&b, 32);
  br_bits(&b, 32); /* bitrate max */
  v->info.bitrate_nom = br_bits(&b, 32);
  br_bits(&b, 32); /* bitrate min */
  int bs = br_bits(&b, 8);
  v->n0 = 1 << (bs & 0xf);
  v->n1 = 1 << (bs >> 4);
  if (!br_bit(&b))
    return OGG_EFORMAT; /* framing bit */
  if (v->info.channels < 1 || v->info.channels > VORBIS_MAX_CHANNELS)
    return OGG_EUNSUP;
  if (v->n0 < 64 || v->n1 > 8192 || v->n0 > v->n1)
    return OGG_EFORMAT;
  v->info.blocksize_short = v->n0;
  v->info.blocksize_long = v->n1;
  return OGG_OK;
}

static int parse_setup(vorbis_decoder *v, const uint8_t *p, unsigned len)
{
  if (!check_head(p, len, 5))
    return OGG_EFORMAT;
  bitr b;
  br_init(&b, p + 7, len - 7);
  int rc;

  v->nbooks = br_bits(&b, 8) + 1;
  v->books = calloc(v->nbooks, sizeof(codebook));
  if (!v->books)
    return OGG_ENOMEM;
  for (int i = 0; i < v->nbooks; i++)
    if ((rc = cb_parse(&v->books[i], &b)) != OGG_OK)
      return rc;

  int ntime = br_bits(&b, 6) + 1;
  for (int i = 0; i < ntime; i++)
    if (br_bits(&b, 16) != 0)
      return OGG_EFORMAT;

  v->nfloors = br_bits(&b, 6) + 1;
  v->floors = calloc(v->nfloors, sizeof(floor1));
  v->floor_type = calloc(v->nfloors, sizeof(int));
  if (!v->floors || !v->floor_type)
    return OGG_ENOMEM;
  for (int i = 0; i < v->nfloors; i++) {
    v->floor_type[i] = br_bits(&b, 16);
    if (v->floor_type[i] == 1) {
      if ((rc = f1_parse(&v->floors[i], &b)) != OGG_OK)
        return rc;
    } else {
      return OGG_EUNSUP; /* floor 0 (LSP) not implemented */
    }
  }

  v->nresidues = br_bits(&b, 6) + 1;
  v->residues = calloc(v->nresidues, sizeof(residue));
  v->residue_type = calloc(v->nresidues, sizeof(int));
  if (!v->residues || !v->residue_type)
    return OGG_ENOMEM;
  for (int i = 0; i < v->nresidues; i++) {
    v->residue_type[i] = br_bits(&b, 16);
    if (v->residue_type[i] > 2)
      return OGG_EFORMAT;
    if ((rc = res_parse(&v->residues[i], &b)) != OGG_OK)
      return rc;
  }

  v->nmaps = br_bits(&b, 6) + 1;
  v->maps = calloc(v->nmaps, sizeof(mapping));
  if (!v->maps)
    return OGG_ENOMEM;
  for (int i = 0; i < v->nmaps; i++) {
    if (br_bits(&b, 16) != 0)
      return OGG_EFORMAT; /* mapping type 0 */
    if ((rc = map_parse(&v->maps[i], &b, v->info.channels, v->nfloors, v->nresidues)) != OGG_OK)
      return rc;
  }

  v->nmodes = br_bits(&b, 6) + 1;
  v->modes = calloc(v->nmodes, sizeof(vmode));
  if (!v->modes)
    return OGG_ENOMEM;
  for (int i = 0; i < v->nmodes; i++) {
    v->modes[i].blockflag = br_bit(&b);
    br_bits(&b, 16); /* window type */
    br_bits(&b, 16); /* transform type */
    v->modes[i].mapping = br_bits(&b, 8);
    if (v->modes[i].mapping >= v->nmaps)
      return OGG_EFORMAT;
  }
  if (!br_bit(&b))
    return OGG_EFORMAT; /* framing */
  if (b.eop)
    return OGG_EFORMAT;
  return OGG_OK;
}

vorbis_decoder *vorbis_decoder_open(const unsigned char *id, unsigned id_len, const unsigned char *setup,
                                    unsigned setup_len, vorbis_info *info)
{
  if (!inv_db_ready)
    inv_db_init();
  vorbis_decoder *v = calloc(1, sizeof *v);
  if (!v)
    return NULL;
  v->first_frame = 1;
  if (parse_id(v, id, id_len) != OGG_OK)
    goto fail;
  if (parse_setup(v, setup, setup_len) != OGG_OK)
    goto fail;
  if (alloc_buffers(v) != OGG_OK)
    goto fail;
  if (info)
    *info = v->info;
  return v;
fail:
  vorbis_decoder_close(v);
  return NULL;
}

void vorbis_decoder_close(vorbis_decoder *v)
{
  if (!v)
    return;
  if (v->books) {
    for (int i = 0; i < v->nbooks; i++)
      cb_free(&v->books[i]);
    free(v->books);
  }
  free(v->floors);
  free(v->floor_type);
  free(v->residues);
  free(v->residue_type);
  free(v->maps);
  free(v->modes);
  for (int c = 0; c < VORBIS_MAX_CHANNELS; c++) {
    free(v->pcmbuf[c]);
    free(v->residuebuf[c]);
    free(v->floorbuf[c]);
    free(v->hold[c]);
  }
  free(v->res_scratch);
  free(v->win_long);
  free(v->win_short);
  free(v);
}

int vorbis_max_frames(const vorbis_decoder *v) { return v->n1; }

static short clamp16(double x)
{
  int v = (int)(x >= 0 ? x + 0.5 : x - 0.5);
  if (v > 32767)
    v = 32767;
  else if (v < -32768)
    v = -32768;
  return (short)v;
}

int vorbis_decode(vorbis_decoder *v, const unsigned char *pkt, unsigned len, short *pcm)
{
  if (!v || !pkt || len == 0)
    return OGG_EPARAM;
  bitr b;
  br_init(&b, pkt, len);
  if (br_bit(&b))
    return 0; /* not an audio packet */

  int mode = br_bits(&b, ilog(v->nmodes - 1));
  if (mode >= v->nmodes)
    return OGG_EFORMAT;
  int blockflag = v->modes[mode].blockflag;
  int n = blockflag ? v->n1 : v->n0;
  int n2 = n / 2;
  int prev_short = 1, next_short = 1;
  if (blockflag) {
    prev_short = !br_bit(&b);
    next_short = !br_bit(&b);
  }

  mapping *m = &v->maps[v->modes[mode].mapping];
  int ch = v->info.channels;
  int zero[VORBIS_MAX_CHANNELS];

  /* floor decode per channel */
  for (int c = 0; c < ch; c++) {
    int submap = m->submaps > 1 ? m->mux[c] : 0;
    int fl = m->submap_floor[submap];
    int r = f1_decode(&v->floors[fl], v->books, v->nbooks, &b, v->floorbuf[c], n2);
    if (r < 0)
      return r;
    zero[c] = !r;
    memset(v->residuebuf[c], 0, n2 * sizeof(float));
  }

  /* propagate coupling: if either channel of a step is non-zero, both decode */
  for (int i = 0; i < m->coupling_steps; i++)
    if (!zero[m->mag[i]] || !zero[m->ang[i]])
      zero[m->mag[i]] = zero[m->ang[i]] = 0;

  /* residue decode per submap */
  for (int s = 0; s < m->submaps; s++) {
    float *rbuf[VORBIS_MAX_CHANNELS];
    int dn[VORBIS_MAX_CHANNELS];
    int cnt = 0;
    for (int c = 0; c < ch; c++) {
      int submap = m->submaps > 1 ? m->mux[c] : 0;
      if (submap != s)
        continue;
      rbuf[cnt] = v->residuebuf[c];
      dn[cnt] = zero[c];
      cnt++;
    }
    if (!cnt)
      continue;
    int ri = m->submap_residue[s];
    int rc =
        res_decode(&v->residues[ri], v->books, v->nbooks, &b, v->residue_type[ri], rbuf, cnt, n2, dn, v->res_scratch);
    if (rc != OGG_OK)
      return rc;
  }

  /* inverse coupling (spec 4.3.3), in reverse step order */
  for (int i = m->coupling_steps - 1; i >= 0; i--) {
    float *M = v->residuebuf[m->mag[i]];
    float *A = v->residuebuf[m->ang[i]];
    for (int j = 0; j < n2; j++) {
      float mm = M[j], a = A[j], nm, na;
      if (mm > 0) {
        if (a > 0) {
          nm = mm;
          na = mm - a;
        } else {
          na = mm;
          nm = mm + a;
        }
      } else {
        if (a > 0) {
          nm = mm;
          na = mm + a;
        } else {
          na = mm;
          nm = mm - a;
        }
      }
      M[j] = nm;
      A[j] = na;
    }
  }

#ifdef OGG_DBG
  {
    static int fc = 0;
    if (fc < 6) {
      double re = 0;
      for (int j = 0; j < n2; j++)
        re += v->residuebuf[0][j] * v->residuebuf[0][j];
      double fe = 0;
      for (int j = 0; j < n2; j++)
        fe += v->floorbuf[0][j];
      fprintf(stderr,
              "frame %d: mode=%d bf=%d n=%d zero=[%d %d] resE=%.3g floorSum=%.3g "
              "eop=%d\n",
              fc, mode, blockflag, n, zero[0], zero[1], re, fe, b.eop);
    }
    fc++;
  }
#endif
  /* dot product (floor * residue) then IMDCT per channel */
  int short_n = v->n0;
  for (int c = 0; c < ch; c++) {
    if (zero[c]) {
      memset(v->pcmbuf[c], 0, n * sizeof(float));
      continue;
    }
    for (int j = 0; j < n2; j++)
      v->residuebuf[c][j] *= v->floorbuf[c][j];
    imdct(v->residuebuf[c], v->pcmbuf[c], n);
    /* window */
    if (blockflag) {
      build_window(v->win_long, n, short_n, prev_short, next_short);
      for (int i = 0; i < n; i++)
        v->pcmbuf[c][i] *= v->win_long[i] * MDCT_SCALE;
    } else {
      for (int i = 0; i < n; i++)
        v->pcmbuf[c][i] *= v->win_short[i] * MDCT_SCALE;
    }
  }

  int out_frames = 0;
  if (v->first_frame) {
    for (int c = 0; c < ch; c++)
      for (int i = 0; i < n2; i++)
        v->hold[c][i] = v->pcmbuf[c][n2 + i];
    v->prev_n = n;
    v->first_frame = 0;
    return 0;
  }
  int pn = v->prev_n;
  int hop = (pn + n) / 4;
  int delta = (n - pn) / 4;
  for (int c = 0; c < ch; c++) {
    float *cur = v->pcmbuf[c];
    float *hd = v->hold[c];
    for (int t = 0; t < hop; t++) {
      double vi = (t < pn / 2) ? hd[t] : 0.0;
      int li = t + delta;
      double vj = (li >= 0 && li < n2) ? cur[li] : 0.0;
      pcm[t * ch + c] = clamp16((vi + vj) * g_pcm_scale);
    }
    /* new hold = this block's right half */
    for (int i = 0; i < n2; i++)
      hd[i] = cur[n2 + i];
  }
  out_frames = hop;
  v->prev_n = n;
  return out_frames;
}

/* one-shot memory decode */
int ogg_vorbis_decode_mem(const void *ogg, unsigned len, short **out, int *frames, int *channels, int *rate)
{
  if (!ogg || !out)
    return OGG_EPARAM;
  *out = NULL;
  ogg_demux *d = ogg_demux_new();
  if (!d)
    return OGG_ENOMEM;
  if (ogg_demux_feed(d, ogg, len) != OGG_OK) {
    ogg_demux_free(d);
    return OGG_ENOMEM;
  }

  /* collect the three headers of the Vorbis logical stream */
  unsigned char *hdr[3] = {NULL, NULL, NULL};
  unsigned hlen[3] = {0, 0, 0};
  uint32_t vser = 0;
  int have_ser = 0, nh = 0;
  const unsigned char *p;
  unsigned pl;
  uint32_t ser;
  int bos, eos;
  int rc = OGG_EFORMAT;

  while (nh < 3) {
    int r = ogg_demux_packet(d, &p, &pl, NULL, &ser, &bos, &eos);
    if (r == OGG_NEED)
      goto done;
    if (r != OGG_OK) {
      rc = r;
      goto done;
    }
    if (!have_ser) {
      if (pl >= 7 && p[0] == 1 && !memcmp(p + 1, "vorbis", 6)) {
        vser = ser;
        have_ser = 1;
      } else
        continue;
    }
    if (ser != vser)
      continue;
    hdr[nh] = malloc(pl ? pl : 1);
    if (!hdr[nh]) {
      rc = OGG_ENOMEM;
      goto done;
    }
    memcpy(hdr[nh], p, pl);
    hlen[nh] = pl;
    nh++;
  }

  vorbis_info info;
  vorbis_decoder *v = vorbis_decoder_open(hdr[0], hlen[0], hdr[2], hlen[2], &info);
  if (!v) {
    rc = OGG_EFORMAT;
    goto done;
  }

  int cap = 48000 * info.channels, used = 0;
  short *acc = malloc((cap ? cap : 1) * sizeof(short));
  short *tmp = malloc((unsigned long)vorbis_max_frames(v) * info.channels * sizeof(short));
  if (!acc || !tmp) {
    free(acc);
    free(tmp);
    vorbis_decoder_close(v);
    rc = OGG_ENOMEM;
    goto done;
  }

  for (;;) {
    int r = ogg_demux_packet(d, &p, &pl, NULL, &ser, &bos, &eos);
    if (r == OGG_NEED)
      break;
    if (r != OGG_OK)
      break;
    if (ser != vser)
      continue;
    int fr = vorbis_decode(v, p, pl, tmp);
    if (fr <= 0)
      continue;
    int need = (used + fr * info.channels);
    if (need > cap) {
      while (need > cap)
        cap *= 2;
      short *na = realloc(acc, cap * sizeof(short));
      if (!na) {
        rc = OGG_ENOMEM;
        break;
      }
      acc = na;
    }
    memcpy(acc + used, tmp, fr * info.channels * sizeof(short));
    used += fr * info.channels;
    rc = OGG_OK;
  }
  free(tmp);
  if (rc == OGG_OK) {
    *out = acc;
    if (frames)
      *frames = used / info.channels;
    if (channels)
      *channels = info.channels;
    if (rate)
      *rate = info.sample_rate;
  } else
    free(acc);
  vorbis_decoder_close(v);

done:
  for (int i = 0; i < 3; i++)
    free(hdr[i]);
  ogg_demux_free(d);
  return rc;
}
