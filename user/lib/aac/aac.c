/* aac.c — MPEG-2/4 AAC Low Complexity decoder. See aac.h.
 *
 * The pipeline mirrors ISO/IEC 14496-3 §4.6 and, where a choice was ambiguous,
 * follows FFmpeg's aacdec reference behaviour so the two agree bit-for-bit on
 * the deterministic tools: noiseless (Huffman) decode -> inverse quantization
 * and rescaling -> M/S -> intensity -> PNS -> TNS -> IMDCT filterbank with
 * overlap-add. The Huffman/scalefactor-band/TNS tables in aac_tables.h are the
 * canonical ISO tables (reproduced from FFmpeg's aactab.c).
 *
 * The IMDCT is computed directly from the spec formula
 *   x[n] = (2/N) * sum_k spec[k] * cos( (2*pi/N)*(n + n0)*(k + 1/2) )
 * with a 4N-entry cosine table indexed by the integer product
 *   ((2n + N/2 + 1)*(2k + 1)) mod 4N,
 * which is exact (no twiddle-factor approximation) and needs no runtime trig.
 */
#include "aac.h"
#include "aac_tables.h"
#include "math.h"
#include "owos.h"

/* ---- window sequences, band types, element ids --------------------------- */
enum { ONLY_LONG = 0, LONG_START = 1, EIGHT_SHORT = 2, LONG_STOP = 3 };
enum {
  ZERO_BT = 0,
  FIRST_PAIR_BT = 5,
  ESC_BT = 11,
  RESERVED_BT = 12,
  NOISE_BT = 13,
  INTENSITY_BT2 = 14,
  INTENSITY_BT = 15
};
enum { ID_SCE = 0, ID_CPE = 1, ID_CCE = 2, ID_LFE = 3, ID_DSE = 4, ID_PCE = 5, ID_FIL = 6, ID_END = 7 };

#define SF_DELTA_ZERO 60 /* scalefactor VLC index of a zero difference     */
#define POW_SF_ZERO 100  /* scalefactor value giving 2^0                   */
#define NOISE_OFFSET 90  /* subtracted from global_gain for the PNS DPCM   */
#define NOISE_PRE_BITS 9
#define NOISE_PRE 256
#define TNS_MAX_ORDER 20

#define AOT_AAC_LC 2

/* ---- MSB-first bit reader -------------------------------------------------
 */
typedef struct {
  const uint8_t *data;
  uint32_t nbytes;
  uint32_t pos; /* bit cursor                                      */
  int overread;
} bitr;

static void br_init(bitr *b, const uint8_t *d, uint32_t n)
{
  b->data = d;
  b->nbytes = n;
  b->pos = 0;
  b->overread = 0;
}

static unsigned br_get1(bitr *b)
{
  unsigned bit;
  if (b->pos >= b->nbytes * 8) {
    b->overread = 1;
    b->pos++;
    return 0;
  }
  bit = (b->data[b->pos >> 3] >> (7 - (b->pos & 7))) & 1;
  b->pos++;
  return bit;
}

static unsigned br_get(bitr *b, int n)
{
  unsigned v = 0;
  while (n-- > 0)
    v = (v << 1) | br_get1(b);
  return v;
}

static int br_left(const bitr *b) { return (int)(b->nbytes * 8) - (int)b->pos; }
static void br_align(bitr *b) { b->pos = (b->pos + 7) & ~7u; }
static void br_skip(bitr *b, int n) { b->pos += n; }

/* ---- Huffman binary tree ------------------------------------------------- *
 * Each node holds two branches; a branch value >0 is a child node index, <0 is
 * a leaf carrying symbol (-value - 1), 0 is an unused branch (bitstream error).
 */
typedef struct {
  int b[2];
} hnode;

/* Build a decode tree from a codeword table. Codes are read via a 32-bit
 * accessor so the same builder serves the 16-bit spectral books and the wider
 * (up to 19-bit) scalefactor book. */
static hnode *huff_build_impl(const uint16_t *c16, const uint32_t *c32, const uint8_t *bits, int n)
{
  int cap = 1, i, count = 1;
  hnode *t;
  for (i = 0; i < n; i++)
    cap += bits[i];
  t = calloc(cap, sizeof(hnode));
  if (!t)
    return 0;
  for (i = 0; i < n; i++) {
    unsigned code = c16 ? c16[i] : c32[i];
    int len = bits[i], node = 0, p;
    for (p = len - 1; p >= 0; p--) {
      int bit = (code >> p) & 1;
      if (p == 0) {
        t[node].b[bit] = -(i + 1);
      } else {
        if (t[node].b[bit] == 0)
          t[node].b[bit] = count++;
        node = t[node].b[bit];
      }
    }
  }
  return t;
}

static hnode *huff_build16(const uint16_t *codes, const uint8_t *bits, int n)
{
  return huff_build_impl(codes, 0, bits, n);
}
static hnode *huff_build32(const uint32_t *codes, const uint8_t *bits, int n)
{
  return huff_build_impl(0, codes, bits, n);
}

static int huff_decode(bitr *b, const hnode *t)
{
  int node = 0;
  for (;;) {
    int c = t[node].b[br_get1(b)];
    if (c < 0)
      return -c - 1;
    if (c == 0 || b->overread)
      return -1;
    node = c;
  }
}

/* ---- spectral codebook geometry ------------------------------------------ */
/* Per codebook (1..11): tuple dimension, whether magnitudes are unsigned (sign
 * bits sent separately), the modulo/base used to unpack each value, and the LAV
 * subtracted for the signed books. */
static const int cb_dim[12] = {0, 4, 4, 4, 4, 2, 2, 2, 2, 2, 2, 2};
static const int cb_uns[12] = {0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1};
static const int cb_mod[12] = {0, 3, 3, 3, 3, 9, 9, 8, 8, 13, 13, 17};
static const int cb_off[12] = {0, 1, 1, 0, 0, 4, 4, 0, 0, 0, 0, 0};

/* ---- decoder state ------------------------------------------------------- */
typedef struct {
  uint8_t present;
  uint8_t n_filt[8];
  uint8_t coef_res[8];
  uint8_t length[8][4];
  uint8_t order[8][4];
  uint8_t direction[8][4];
  double coef[8][4][TNS_MAX_ORDER];
} tns_state;

typedef struct {
  uint8_t win_seq, win_seq_prev;
  uint8_t win_shape, win_shape_prev; /* 0 = sine, 1 = KBD                 */
  uint8_t max_sfb;
  uint8_t num_windows;
  uint8_t num_window_groups;
  uint8_t group_len[8];
  uint8_t num_swb;
  uint8_t tns_max_bands;
  const uint16_t *swb_offset;

  uint8_t bt[8 * 64];     /* band type per (group,sfb)               */
  int bt_run_end[8 * 64]; /* group-local sfb run end                 */
  double sfsc[8 * 64];    /* dequant scale / intensity / noise scale */

  int global_gain;
  tns_state tns;

  double coef[1024]; /* dequantized, rescaled spectrum          */
} channel;

struct aac_decoder {
  int configured;
  int adts_mode;
  int object_type;
  int sf_index;
  uint32_t sample_rate;
  int channel_config;
  int frame_len; /* 1024 (960 unsupported)                  */
  int sbr_present;

  hnode *sf_tree;
  hnode *spec_tree[12]; /* [1..11] used                            */

  double *cos_long;  /* 8192 entries                            */
  double *cos_short; /* 1024 entries                            */
  double lwin_sine[1024], lwin_kbd[1024];
  double swin_sine[128], swin_kbd[128];
  double *pow43; /* |q|^(4/3), q = 0..8191                   */

  unsigned random_state;

  /* per output-channel persistent overlap tail + window history */
  double saved[AAC_MAX_CHANNELS][1024];
  uint8_t prev_win_seq[AAC_MAX_CHANNELS];
  uint8_t prev_win_shape[AAC_MAX_CHANNELS];
  int have_prev;

  /* per-frame working state */
  channel ch[AAC_MAX_CHANNELS];
  int nchan;
  double out_time[AAC_MAX_CHANNELS][1024];
};

/* ================= window + table construction (open time) ================ */

static double bessel_i0(double x)
{
  double sum = 1.0, term = 1.0, x2 = x * x / 4.0;
  int k;
  for (k = 1; k < 64; k++) {
    term *= x2 / ((double)k * (double)k);
    sum += term;
    if (term < sum * 1e-17)
      break;
  }
  return sum;
}

/* ISO rising-half KBD window: half = N/2 (1024 long, 128 short), alpha the KBD
 * parameter (4 long, 6 short). Result w[0..half-1] rises monotonically; the
 * falling half of the full window is its mirror. */
static void gen_kbd(double *w, int half, double alpha)
{
  double *temp = malloc((half + 1) * sizeof(double));
  double denom = 0, sum = 0;
  int p;
  if (!temp)
    return;
  for (p = 0; p <= half; p++) {
    double x = (double)(p - half / 2.0) / (half / 2.0);
    double a = 1.0 - x * x;
    if (a < 0)
      a = 0;
    temp[p] = bessel_i0(M_PI * alpha * sqrt(a));
    denom += temp[p];
  }
  for (p = 0; p < half; p++) {
    sum += temp[p];
    w[p] = sqrt(sum / denom);
  }
  free(temp);
}

static void gen_sine(double *w, int half, int N)
{
  int n;
  for (n = 0; n < half; n++)
    w[n] = sin(M_PI / N * (n + 0.5));
}

static int build_cos(double **out, int fourN)
{
  int j;
  double *t = malloc(fourN * sizeof(double));
  if (!t)
    return -1;
  for (j = 0; j < fourN; j++)
    t[j] = cos(2.0 * M_PI * j / fourN);
  *out = t;
  return 0;
}

/* ================= AudioSpecificConfig / ADTS parsing ===================== */

static int set_rate(aac_decoder *d, bitr *b, int sf_index)
{
  if (sf_index == 15)
    d->sample_rate = br_get(b, 24);
  else if (sf_index < 13)
    d->sample_rate = aac_sample_rates[sf_index];
  else
    return -1;
  d->sf_index = sf_index;
  return 0;
}

/* Parse GASpecificConfig tail after aot/rate/channels are known. */
static int parse_ga_config(aac_decoder *d, bitr *b, int aot)
{
  int frame_len_flag = br_get1(b);
  if (frame_len_flag)
    return AAC_EUNSUP; /* 960-sample frames unhandled */
  if (br_get1(b))
    br_skip(b, 14); /* dependsOnCoreCoder + delay  */
  br_get1(b);       /* extensionFlag (LC: ignored) */
  (void)aot;
  return AAC_OK;
}

static int aac_configure_asc(aac_decoder *d, const uint8_t *asc, uint32_t len)
{
  bitr b;
  int aot, sf_index, base_aot;
  br_init(&b, asc, len);
  aot = br_get(&b, 5);
  if (aot == 31)
    aot = 32 + br_get(&b, 6);
  sf_index = br_get(&b, 4);
  if (set_rate(d, &b, sf_index) != 0)
    return AAC_EFORMAT;
  d->channel_config = br_get(&b, 4);
  base_aot = aot;
  if (aot == 5 || aot == 29) { /* SBR / PS wrapper            */
    int ext_sf = br_get(&b, 4);
    if (ext_sf == 15)
      br_skip(&b, 24);
    d->sbr_present = 1;
    base_aot = br_get(&b, 5);
    if (base_aot == 22)
      br_skip(&b, 4);
  }
  if (base_aot != AOT_AAC_LC)
    return AAC_EUNSUP;
  d->object_type = base_aot;
  d->frame_len = 1024;
  if (parse_ga_config(d, &b, base_aot) != AAC_OK)
    return AAC_EUNSUP;
  if (b.overread)
    return AAC_EFORMAT;
  d->configured = 1;
  return AAC_OK;
}

/* Read one 7/9-byte ADTS header; sets config on the first frame. Returns the
 * whole ADTS frame's byte length via *frame_bytes, or a negative code. */
static int parse_adts(aac_decoder *d, bitr *b, uint32_t *frame_bytes)
{
  int profile, sf_index, chan_cfg, protection_absent, num_rdb;
  unsigned frame_length;
  if (br_get(b, 12) != 0xFFF)
    return AAC_EFORMAT;
  br_get1(b);   /* MPEG version                             */
  br_get(b, 2); /* layer (0)                                */
  protection_absent = br_get1(b);
  profile = br_get(b, 2); /* object_type = profile + 1                */
  sf_index = br_get(b, 4);
  br_get1(b); /* private_bit                              */
  chan_cfg = br_get(b, 3);
  br_get(b, 4); /* original/home/copyright id + start       */
  frame_length = br_get(b, 13);
  br_get(b, 11); /* buffer fullness                          */
  num_rdb = br_get(b, 2);
  if (!protection_absent)
    br_get(b, 16); /* CRC                            */

  if (!d->configured) {
    d->object_type = profile + 1;
    if (d->object_type != AOT_AAC_LC)
      return AAC_EUNSUP;
    if (sf_index >= 13)
      return AAC_EFORMAT;
    d->sample_rate = aac_sample_rates[sf_index];
    d->sf_index = sf_index;
    d->channel_config = chan_cfg;
    d->frame_len = 1024;
    d->configured = 1;
  }
  if (num_rdb != 0)
    return AAC_EUNSUP; /* one raw_data_block per frame   */
  *frame_bytes = frame_length;
  return AAC_OK;
}

/* ================= individual channel stream ============================== */

static int decode_ics_info(aac_decoder *d, channel *c, bitr *b)
{
  int i;
  if (br_get1(b)) { /* reserved bit; ignore its value */
  }
  c->win_seq_prev = c->win_seq;
  c->win_seq = br_get(b, 2);
  c->win_shape_prev = c->win_shape;
  c->win_shape = br_get1(b);
  c->num_window_groups = 1;
  c->group_len[0] = 1;
  if (c->win_seq == EIGHT_SHORT) {
    c->max_sfb = br_get(b, 4);
    for (i = 0; i < 7; i++) {
      if (br_get1(b))
        c->group_len[c->num_window_groups - 1]++;
      else {
        c->num_window_groups++;
        c->group_len[c->num_window_groups - 1] = 1;
      }
    }
    c->num_windows = 8;
    c->swb_offset = aac_swb_offset_128[d->sf_index];
    c->num_swb = aac_num_swb_128[d->sf_index];
  } else {
    c->max_sfb = br_get(b, 6);
    c->num_windows = 1;
    c->swb_offset = aac_swb_offset_1024[d->sf_index];
    c->num_swb = aac_num_swb_1024[d->sf_index];
    if (br_get1(b))
      return AAC_EUNSUP; /* predictor: not in plain LC    */
  }
  if (c->max_sfb > c->num_swb)
    return AAC_EFORMAT;
  return AAC_OK;
}

static int decode_band_types(channel *c, bitr *b)
{
  int g, bits = (c->win_seq == EIGHT_SHORT) ? 3 : 5;
  int idx = 0;
  for (g = 0; g < c->num_window_groups; g++) {
    int k = 0;
    while (k < c->max_sfb) {
      int sect_end = k, incr;
      int bt = br_get(b, 4);
      if (bt == RESERVED_BT)
        return AAC_EFORMAT;
      do {
        incr = br_get(b, bits);
        sect_end += incr;
        if (sect_end > c->max_sfb || br_left(b) < 0)
          return AAC_EFORMAT;
      } while (incr == (1 << bits) - 1);
      for (; k < sect_end; k++) {
        c->bt[idx] = bt;
        c->bt_run_end[idx++] = sect_end;
      }
    }
  }
  return AAC_OK;
}

static int decode_scalefactors(aac_decoder *d, channel *c, bitr *b)
{
  int g, i, idx = 0;
  int off_reg = c->global_gain;
  int off_nrg = c->global_gain - NOISE_OFFSET;
  int off_is = 0;
  int noise_first = 1;
  for (g = 0; g < c->num_window_groups; g++) {
    for (i = 0; i < c->max_sfb;) {
      int run_end = c->bt_run_end[idx];
      int bt = c->bt[idx];
      if (bt == ZERO_BT) {
        for (; i < run_end; i++, idx++)
          c->sfsc[idx] = 0.0;
      } else if (bt == INTENSITY_BT || bt == INTENSITY_BT2) {
        for (; i < run_end; i++, idx++) {
          int v = huff_decode(b, d->sf_tree);
          if (v < 0)
            return AAC_EFORMAT;
          off_is += v - SF_DELTA_ZERO;
          c->sfsc[idx] = pow(2.0, -0.25 * off_is);
        }
      } else if (bt == NOISE_BT) {
        for (; i < run_end; i++, idx++) {
          if (noise_first) {
            noise_first = 0;
            off_nrg += br_get(b, NOISE_PRE_BITS) - NOISE_PRE;
          } else {
            int v = huff_decode(b, d->sf_tree);
            if (v < 0)
              return AAC_EFORMAT;
            off_nrg += v - SF_DELTA_ZERO;
          }
          c->sfsc[idx] = -pow(2.0, 0.25 * off_nrg);
        }
      } else {
        for (; i < run_end; i++, idx++) {
          int v = huff_decode(b, d->sf_tree);
          if (v < 0)
            return AAC_EFORMAT;
          off_reg += v - SF_DELTA_ZERO;
          if (off_reg < 0 || off_reg > 255)
            return AAC_EFORMAT;
          c->sfsc[idx] = pow(2.0, 0.25 * (off_reg - POW_SF_ZERO));
        }
      }
    }
  }
  return AAC_OK;
}

/* pulse tool (long windows only): read up to four pulses, apply to integer q[]
 */
typedef struct {
  int num, pos[4], amp[4];
} pulse_info;

static int decode_pulses(channel *c, bitr *b, pulse_info *p)
{
  int i, swb;
  p->num = br_get(b, 2) + 1; /* number_pulse first, then start sfb  */
  swb = br_get(b, 6);
  if (swb >= c->num_swb)
    return AAC_EFORMAT;
  p->pos[0] = c->swb_offset[swb] + br_get(b, 5);
  p->amp[0] = br_get(b, 4);
  if (p->pos[0] >= c->swb_offset[c->num_swb])
    return AAC_EFORMAT;
  for (i = 1; i < p->num; i++) {
    p->pos[i] = p->pos[i - 1] + br_get(b, 5);
    if (p->pos[i] >= c->swb_offset[c->num_swb])
      return AAC_EFORMAT;
    p->amp[i] = br_get(b, 4);
  }
  return AAC_OK;
}

static int decode_tns(channel *c, bitr *b)
{
  int w, filt, i;
  int is8 = (c->win_seq == EIGHT_SHORT);
  int tns_max_order = is8 ? 7 : 12;
  tns_state *t = &c->tns;
  for (w = 0; w < c->num_windows; w++) {
    t->n_filt[w] = br_get(b, 2 - is8);
    if (t->n_filt[w]) {
      t->coef_res[w] = br_get1(b);
      for (filt = 0; filt < t->n_filt[w]; filt++) {
        int coef_res = t->coef_res[w];
        int order, coef_compress, coef_len, tmp2;
        t->length[w][filt] = br_get(b, 6 - 2 * is8);
        order = br_get(b, 5 - 2 * is8);
        if (order > tns_max_order)
          return AAC_EFORMAT;
        t->order[w][filt] = order;
        if (order) {
          t->direction[w][filt] = br_get1(b);
          coef_compress = br_get1(b);
          coef_len = coef_res + 3 - coef_compress;
          tmp2 = 2 * coef_compress + coef_res;
          for (i = 0; i < order; i++)
            t->coef[w][filt][i] = aac_tns_tmp2[tmp2][br_get(b, coef_len)];
        }
      }
    }
  }
  return AAC_OK;
}

/* Decode one value tuple for spectral codebook cb; write dim() ints to q[]. */
static int spec_tuple(bitr *b, const hnode *tree, int cb, int *q)
{
  int dim = cb_dim[cb], uns = cb_uns[cb], mod = cb_mod[cb], off = cb_off[cb];
  int j, idx = huff_decode(b, tree);
  if (idx < 0)
    return -1;
  for (j = dim - 1; j >= 0; j--) {
    q[j] = idx % mod - off;
    idx /= mod;
  }
  if (uns) {
    for (j = 0; j < dim; j++)
      if (q[j]) {
        if (br_get1(b))
          q[j] = -q[j];
      }
    if (cb == ESC_BT) {
      for (j = 0; j < dim; j++) {
        int m = q[j] < 0 ? -q[j] : q[j];
        if (m == 16) {
          int i = 4, mag;
          while (br_get1(b)) {
            if (++i > 20)
              return -1;
          }
          mag = (1 << i) + br_get(b, i);
          q[j] = q[j] < 0 ? -mag : mag;
        }
      }
    }
  }
  return 0;
}

static double dequant_val(aac_decoder *d, int q, double scale)
{
  int a = q < 0 ? -q : q;
  double m = a < 8192 ? d->pow43[a] : pow((double)a, 4.0 / 3.0);
  return (q < 0 ? -m : m) * scale;
}

/* Decode spectral data + inverse-quantize/rescale a whole ICS into c->coef[].
 */
static int decode_spectrum(aac_decoder *d, channel *c, bitr *b, int pulse_present, const pulse_info *pulse)
{
  int g, i, w, k, idx = 0;
  int qbuf[1024];
  int c128 = 1024 / c->num_windows;
  const uint16_t *off = c->swb_offset;

  memset(c->coef, 0, sizeof(c->coef));
  memset(qbuf, 0, sizeof(qbuf));

  /* Noiseless decode into the integer coefficient buffer qbuf[]. */
  {
    int base = 0;
    for (g = 0; g < c->num_window_groups; g++) {
      int glen = c->group_len[g];
      for (i = 0; i < c->max_sfb; i++, idx++) {
        int bt = c->bt[idx];
        int start = off[i], len = off[i + 1] - off[i];
        if (bt == ZERO_BT || bt == NOISE_BT || bt == INTENSITY_BT || bt == INTENSITY_BT2)
          continue; /* left as zero for now             */
        for (w = 0; w < glen; w++) {
          int *q = qbuf + (base + w) * 128 + start;
          int n = 0;
          while (n < len) {
            int t[4];
            if (spec_tuple(b, d->spec_tree[bt], bt, t))
              return AAC_EFORMAT;
            for (k = 0; k < cb_dim[bt]; k++)
              q[n++] = t[k];
          }
        }
      }
      base += glen;
    }
  }

  /* Pulses modify the integer spectrum of the (single) long window. */
  if (pulse_present) {
    for (i = 0; i < pulse->num; i++) {
      int pos = pulse->pos[i];
      if (pos < 1024) {
        if (qbuf[pos] >= 0)
          qbuf[pos] += pulse->amp[i];
        else
          qbuf[pos] -= pulse->amp[i];
      }
    }
  }

  /* Inverse quantization + rescaling; NOISE bands get PNS here. */
  idx = 0;
  {
    int base = 0;
    for (g = 0; g < c->num_window_groups; g++) {
      int glen = c->group_len[g];
      for (i = 0; i < c->max_sfb; i++, idx++) {
        int bt = c->bt[idx];
        int start = off[i], len = off[i + 1] - off[i];
        double sc = c->sfsc[idx];
        if (bt == ZERO_BT || bt == INTENSITY_BT || bt == INTENSITY_BT2)
          continue;
        if (bt == NOISE_BT) {
          for (w = 0; w < glen; w++) {
            double *cf = c->coef + (base + w) * 128 + start;
            double energy = 0, scale;
            for (k = 0; k < len; k++) {
              d->random_state = d->random_state * 1664525u + 1013904223u;
              cf[k] = (double)(int)d->random_state;
              energy += cf[k] * cf[k];
            }
            scale = energy > 0 ? sc / sqrt(energy) : 0;
            for (k = 0; k < len; k++)
              cf[k] *= scale;
          }
        } else {
          for (w = 0; w < glen; w++) {
            int *q = qbuf + (base + w) * 128 + start;
            double *cf = c->coef + (base + w) * 128 + start;
            for (k = 0; k < len; k++)
              cf[k] = dequant_val(d, q[k], sc);
          }
        }
      }
      base += glen;
    }
  }
  (void)c128;
  return AAC_OK;
}

static int decode_ics(aac_decoder *d, channel *c, bitr *b, int common)
{
  pulse_info pulse;
  int pulse_present = 0, rc;
  c->global_gain = br_get(b, 8);
  if (!common) {
    if ((rc = decode_ics_info(d, c, b)))
      return rc;
  }
  if ((rc = decode_band_types(c, b)))
    return rc;
  if ((rc = decode_scalefactors(d, c, b)))
    return rc;
  pulse.num = 0;
  if ((pulse_present = br_get1(b))) {
    if (c->win_seq == EIGHT_SHORT)
      return AAC_EFORMAT;
    if ((rc = decode_pulses(c, b, &pulse)))
      return rc;
  }
  c->tns.present = br_get1(b);
  if (c->tns.present) {
    if ((rc = decode_tns(c, b)))
      return rc;
  }
  if (br_get1(b))
    return AAC_EUNSUP; /* gain_control: SSR only      */
  if ((rc = decode_spectrum(d, c, b, pulse_present, &pulse)))
    return rc;
  return AAC_OK;
}

/* ================= stereo tools =========================================== */

static void apply_ms(channel *c0, channel *c1, const uint8_t *ms_mask)
{
  int g, i, w, idx = 0, base = 0;
  const uint16_t *off = c0->swb_offset;
  for (g = 0; g < c0->num_window_groups; g++) {
    int glen = c0->group_len[g];
    for (i = 0; i < c0->max_sfb; i++, idx++) {
      if (ms_mask[idx] && c0->bt[idx] < NOISE_BT && c1->bt[idx] < NOISE_BT) {
        int start = off[i], len = off[i + 1] - off[i], k;
        for (w = 0; w < glen; w++) {
          double *a = c0->coef + (base + w) * 128 + start;
          double *s = c1->coef + (base + w) * 128 + start;
          for (k = 0; k < len; k++) {
            double l = a[k], r = s[k];
            a[k] = l + r;
            s[k] = l - r;
          }
        }
      }
    }
    base += glen;
  }
}

static void apply_intensity(channel *c0, channel *c1, int ms_present, const uint8_t *ms_mask)
{
  int g, i, w, idx = 0, base = 0;
  const uint16_t *off = c1->swb_offset;
  for (g = 0; g < c1->num_window_groups; g++) {
    int glen = c1->group_len[g];
    for (i = 0; i < c1->max_sfb;) {
      int bt = c1->bt[idx];
      if (bt == INTENSITY_BT || bt == INTENSITY_BT2) {
        int run_end = c1->bt_run_end[idx];
        for (; i < run_end; i++, idx++) {
          int cf = -1 + 2 * (c1->bt[idx] - 14);
          double scale;
          int start = off[i], len = off[i + 1] - off[i], k;
          if (ms_present)
            cf *= 1 - 2 * ms_mask[idx];
          scale = cf * c1->sfsc[idx];
          for (w = 0; w < glen; w++) {
            double *src = c0->coef + (base + w) * 128 + start;
            double *dst = c1->coef + (base + w) * 128 + start;
            for (k = 0; k < len; k++)
              dst[k] = src[k] * scale;
          }
        }
      } else {
        int run_end = c1->bt_run_end[idx];
        idx += run_end - i;
        i = run_end;
      }
    }
    base += glen;
  }
}

/* ================= TNS filtering (frequency domain) ======================= */

static void apply_tns(channel *c)
{
  tns_state *t = &c->tns;
  int w, filt, m, i;
  int tns_max = c->tns_max_bands < c->max_sfb ? c->tns_max_bands : c->max_sfb;
  if (tns_max == 0)
    return;
  for (w = 0; w < c->num_windows; w++) {
    int bottom = c->num_swb;
    for (filt = 0; filt < t->n_filt[w]; filt++) {
      int top = bottom;
      int order = t->order[w][filt];
      int start, end, size, inc;
      double lpc[TNS_MAX_ORDER];
      bottom = top - t->length[w][filt];
      if (bottom < 0)
        bottom = 0;
      if (order == 0)
        continue;
      /* reflection -> LPC (FFmpeg compute_lpc_coefs, normalize=0) */
      for (m = 0; m < order; m++) {
        double r = -t->coef[w][filt][m];
        int j;
        lpc[m] = r;
        for (j = 0; j < (m + 1) >> 1; j++) {
          double f = lpc[j], bb = lpc[m - 1 - j];
          lpc[j] = f + r * bb;
          lpc[m - 1 - j] = bb + r * f;
        }
      }
      start = c->swb_offset[bottom < tns_max ? bottom : tns_max];
      end = c->swb_offset[top < tns_max ? top : tns_max];
      size = end - start;
      if (size <= 0)
        continue;
      if (t->direction[w][filt]) {
        inc = -1;
        start = end - 1;
      } else
        inc = 1;
      start += w * 128;
      for (m = 0; m < size; m++, start += inc) {
        int lim = m < order ? m : order;
        for (i = 1; i <= lim; i++)
          c->coef[start] -= c->coef[start - i * inc] * lpc[i - 1];
      }
    }
  }
}

/* ================= filterbank (IMDCT + windowing + overlap-add) =========== */

static void imdct(const double *ct, int N, int fourN_mask, const double *in, double *out)
{
  int M = N / 2, A0 = N / 2 + 1, n, k;
  double s = 2.0 / N;
  for (n = 0; n < N; n++) {
    int A = 2 * n + A0;
    double acc = 0;
    for (k = 0; k < M; k++)
      acc += in[k] * ct[(A * (2 * k + 1)) & fourN_mask];
    out[n] = acc * s;
  }
}

static void filterbank(aac_decoder *d, channel *c, int outch)
{
  double *saved = d->saved[outch];
  double *out = d->out_time[outch];
  const double *lw_cur = c->win_shape ? d->lwin_kbd : d->lwin_sine;
  const double *lw_prev = c->win_shape_prev ? d->lwin_kbd : d->lwin_sine;
  const double *sw_cur = c->win_shape ? d->swin_kbd : d->swin_sine;
  const double *sw_prev = c->win_shape_prev ? d->swin_kbd : d->swin_sine;
  double buf[2048];
  int n;

  if (c->win_seq == EIGHT_SHORT) {
    double comp[2048];
    int kk;
    for (n = 0; n < 2048; n++)
      comp[n] = 0;
    for (kk = 0; kk < 8; kk++) {
      double z[256];
      const double *lw = (kk == 0) ? sw_prev : sw_cur;
      int i, o = 448 + kk * 128;
      imdct(d->cos_short, 256, 1023, c->coef + kk * 128, z);
      for (i = 0; i < 128; i++)
        comp[o + i] += z[i] * lw[i];
      for (i = 0; i < 128; i++)
        comp[o + 128 + i] += z[128 + i] * sw_cur[127 - i];
    }
    for (n = 0; n < 1024; n++)
      out[n] = saved[n] + comp[n];
    for (n = 0; n < 1024; n++)
      saved[n] = comp[1024 + n];
    return;
  }

  imdct(d->cos_long, 2048, 8191, c->coef, buf);

  /* window: left half uses previous shape/edge, right half current. */
  if (c->win_seq == LONG_STOP) {
    for (n = 0; n < 448; n++)
      buf[n] = 0;
    for (n = 0; n < 128; n++)
      buf[448 + n] *= sw_prev[n];
    /* [576,1024) stays flat (weight 1) */
  } else {
    for (n = 0; n < 1024; n++)
      buf[n] *= lw_prev[n];
  }
  if (c->win_seq == LONG_START) {
    /* [1024,1472) flat, short falling window on [1472,1600), zero after */
    for (n = 0; n < 128; n++)
      buf[1472 + n] *= sw_cur[127 - n];
    for (n = 1600; n < 2048; n++)
      buf[n] = 0;
  } else {
    for (n = 0; n < 1024; n++)
      buf[1024 + n] *= lw_cur[1023 - n];
  }

  for (n = 0; n < 1024; n++)
    out[n] = saved[n] + buf[n];
  for (n = 0; n < 1024; n++)
    saved[n] = buf[1024 + n];
}

/* ================= raw_data_block driver ================================== */

static void reset_channel(channel *c)
{
  memset(&c->tns, 0, sizeof(c->tns));
  c->tns.present = 0;
}

/* Decode an SCE or LFE (one channel). */
static int decode_sce(aac_decoder *d, bitr *b, int outch)
{
  channel *c = &d->ch[outch];
  int rc;
  br_get(b, 4); /* element_instance_tag            */
  reset_channel(c);
  c->win_seq_prev = d->prev_win_seq[outch];
  c->win_shape_prev = d->prev_win_shape[outch];
  if ((rc = decode_ics(d, c, b, 0)))
    return rc;
  c->tns_max_bands =
      (c->win_seq == EIGHT_SHORT) ? aac_tns_max_bands_128[d->sf_index] : aac_tns_max_bands_1024[d->sf_index];
  if (c->tns.present)
    apply_tns(c);
  return AAC_OK;
}

static int decode_cpe(aac_decoder *d, bitr *b, int outch)
{
  channel *c0 = &d->ch[outch], *c1 = &d->ch[outch + 1];
  uint8_t ms_mask[8 * 64];
  int common, ms_present = 0, rc, idx, mx;
  br_get(b, 4); /* element_instance_tag            */
  reset_channel(c0);
  reset_channel(c1);
  c0->win_seq_prev = d->prev_win_seq[outch];
  c0->win_shape_prev = d->prev_win_shape[outch];
  c1->win_seq_prev = d->prev_win_seq[outch + 1];
  c1->win_shape_prev = d->prev_win_shape[outch + 1];

  common = br_get1(b);
  memset(ms_mask, 0, sizeof(ms_mask));
  if (common) {
    if ((rc = decode_ics_info(d, c0, b)))
      return rc;
    /* channel 1 shares the ICS info; keep its own prev-window history */
    {
      uint8_t sp = c1->win_seq_prev, shp = c1->win_shape_prev;
      c1->win_seq = c0->win_seq;
      c1->win_shape = c0->win_shape;
      c1->max_sfb = c0->max_sfb;
      c1->num_windows = c0->num_windows;
      c1->num_window_groups = c0->num_window_groups;
      memcpy(c1->group_len, c0->group_len, sizeof(c0->group_len));
      c1->num_swb = c0->num_swb;
      c1->swb_offset = c0->swb_offset;
      c1->win_seq_prev = sp;
      c1->win_shape_prev = shp;
    }
    ms_present = br_get(b, 2);
    if (ms_present == 3)
      return AAC_EFORMAT;
    mx = c0->num_window_groups * c0->max_sfb;
    if (ms_present == 1) {
      for (idx = 0; idx < mx; idx++)
        ms_mask[idx] = br_get1(b);
    } else if (ms_present == 2) {
      for (idx = 0; idx < mx; idx++)
        ms_mask[idx] = 1;
    }
  }
  if ((rc = decode_ics(d, c0, b, common)))
    return rc;
  if ((rc = decode_ics(d, c1, b, common)))
    return rc;

  if (common && ms_present)
    apply_ms(c0, c1, ms_mask);
  apply_intensity(c0, c1, ms_present, ms_mask);

  c0->tns_max_bands =
      (c0->win_seq == EIGHT_SHORT) ? aac_tns_max_bands_128[d->sf_index] : aac_tns_max_bands_1024[d->sf_index];
  c1->tns_max_bands =
      (c1->win_seq == EIGHT_SHORT) ? aac_tns_max_bands_128[d->sf_index] : aac_tns_max_bands_1024[d->sf_index];
  if (c0->tns.present)
    apply_tns(c0);
  if (c1->tns.present)
    apply_tns(c1);
  return AAC_OK;
}

/* Skip a data_stream_element / fill_element / PCE we don't consume. */
static int skip_dse(bitr *b)
{
  int align = 0, cnt;
  br_get(b, 4);
  align = br_get1(b);
  cnt = br_get(b, 8);
  if (cnt == 255)
    cnt += br_get(b, 8);
  if (align)
    br_align(b);
  br_skip(b, cnt * 8);
  return AAC_OK;
}

static int skip_fil(bitr *b)
{
  int cnt = br_get(b, 4);
  if (cnt == 15)
    cnt += br_get(b, 8) - 1;
  br_skip(b, cnt * 8);
  return AAC_OK;
}

static int skip_pce(bitr *b)
{
  int n_front, n_side, n_back, n_lfe, n_assoc, n_cc, i, cbytes;
  br_get(b, 4); /* element_instance_tag               */
  br_get(b, 2); /* object_type                        */
  br_get(b, 4); /* sampling_frequency_index           */
  n_front = br_get(b, 4);
  n_side = br_get(b, 4);
  n_back = br_get(b, 4);
  n_lfe = br_get(b, 2);
  n_assoc = br_get(b, 3);
  n_cc = br_get(b, 4);
  if (br_get1(b))
    br_get(b, 4); /* mono mixdown                       */
  if (br_get1(b))
    br_get(b, 4); /* stereo mixdown                     */
  if (br_get1(b))
    br_get(b, 3); /* matrix mixdown                     */
  for (i = 0; i < n_front; i++)
    br_get(b, 5);
  for (i = 0; i < n_side; i++)
    br_get(b, 5);
  for (i = 0; i < n_back; i++)
    br_get(b, 5);
  for (i = 0; i < n_lfe; i++)
    br_get(b, 4);
  for (i = 0; i < n_assoc; i++)
    br_get(b, 4);
  for (i = 0; i < n_cc; i++)
    br_get(b, 5);
  br_align(b);
  cbytes = br_get(b, 8);
  br_skip(b, cbytes * 8);
  return AAC_OK;
}

static int decode_raw_block(aac_decoder *d, bitr *b)
{
  int outch = 0;
  for (;;) {
    int id = br_get(b, 3), rc;
    if (b->overread)
      return AAC_EFORMAT;
    switch (id) {
    case ID_SCE:
    case ID_LFE:
      if (outch >= AAC_MAX_CHANNELS)
        return AAC_EUNSUP;
      if ((rc = decode_sce(d, b, outch)))
        return rc;
      outch += 1;
      break;
    case ID_CPE:
      if (outch + 1 >= AAC_MAX_CHANNELS)
        return AAC_EUNSUP;
      if ((rc = decode_cpe(d, b, outch)))
        return rc;
      outch += 2;
      break;
    case ID_DSE:
      if ((rc = skip_dse(b)))
        return rc;
      break;
    case ID_FIL:
      if ((rc = skip_fil(b)))
        return rc;
      break;
    case ID_PCE:
      if ((rc = skip_pce(b)))
        return rc;
      break;
    case ID_CCE:
      return AAC_EUNSUP; /* coupling channel not supported   */
    case ID_END:
      br_align(b);
      d->nchan = outch;
      return AAC_OK;
    }
  }
}

/* ================= public API ============================================= */

static void free_trees(aac_decoder *d)
{
  int i;
  if (d->sf_tree)
    free(d->sf_tree);
  for (i = 1; i <= 11; i++)
    if (d->spec_tree[i])
      free(d->spec_tree[i]);
  if (d->cos_long)
    free(d->cos_long);
  if (d->cos_short)
    free(d->cos_short);
  if (d->pow43)
    free(d->pow43);
}

static int build_common(aac_decoder *d)
{
  int i;
  d->sf_tree = huff_build32(aac_sf_code, aac_sf_bits, 121);
  if (!d->sf_tree)
    return -1;
  for (i = 1; i <= 11; i++) {
    d->spec_tree[i] = huff_build16(aac_spec_codes[i - 1], aac_spec_bits[i - 1], aac_spec_sizes[i - 1]);
    if (!d->spec_tree[i])
      return -1;
  }
  if (build_cos(&d->cos_long, 8192))
    return -1;
  if (build_cos(&d->cos_short, 1024))
    return -1;
  gen_sine(d->lwin_sine, 1024, 2048);
  gen_kbd(d->lwin_kbd, 1024, 4.0);
  gen_sine(d->swin_sine, 128, 256);
  gen_kbd(d->swin_kbd, 128, 6.0);
  d->pow43 = malloc(8192 * sizeof(double));
  if (!d->pow43)
    return -1;
  for (i = 0; i < 8192; i++)
    d->pow43[i] = pow((double)i, 4.0 / 3.0);
  d->random_state = 0x1f2e3d4c;
  return 0;
}

static aac_decoder *alloc_decoder(void)
{
  aac_decoder *d = calloc(1, sizeof(aac_decoder));
  if (!d)
    return 0;
  if (build_common(d)) {
    free_trees(d);
    free(d);
    return 0;
  }
  return d;
}

static void fill_info(const aac_decoder *d, aac_info *info)
{
  if (!info)
    return;
  info->sample_rate = d->sample_rate;
  info->channels = (uint8_t)(d->nchan ? d->nchan : (d->channel_config == 7 ? 8 : d->channel_config));
  info->frame_samples = (uint16_t)d->frame_len;
  info->object_type = (uint8_t)d->object_type;
  info->sbr_present = (uint8_t)d->sbr_present;
}

aac_decoder *aac_open(const uint8_t *asc, uint32_t asc_len, aac_info *info)
{
  aac_decoder *d;
  if (!asc || asc_len < 2)
    return 0;
  d = alloc_decoder();
  if (!d)
    return 0;
  if (aac_configure_asc(d, asc, asc_len) != AAC_OK) {
    aac_close(d);
    return 0;
  }
  if (info)
    fill_info(d, info);
  return d;
}

aac_decoder *aac_open_adts(void)
{
  aac_decoder *d = alloc_decoder();
  if (d)
    d->adts_mode = 1;
  return d;
}

void aac_close(aac_decoder *d)
{
  if (!d)
    return;
  free_trees(d);
  free(d);
}

void aac_get_info(const aac_decoder *d, aac_info *info) { fill_info(d, info); }

uint32_t aac_max_pcm_samples(const aac_decoder *d)
{
  int ch = d->nchan ? d->nchan : (d->channel_config == 7 ? 8 : d->channel_config);
  if (ch <= 0)
    ch = 2;
  return (uint32_t)d->frame_len * ch;
}

/* Convert the per-channel float time-domain frames to interleaved int16. */
static void interleave(aac_decoder *d, int16_t *pcm)
{
  int n, ch;
  for (n = 0; n < d->frame_len; n++) {
    for (ch = 0; ch < d->nchan; ch++) {
      double v = d->out_time[ch][n];
      long s = (long)(v < 0 ? v - 0.5 : v + 0.5);
      if (s > 32767)
        s = 32767;
      if (s < -32768)
        s = -32768;
      pcm[n * d->nchan + ch] = (int16_t)s;
    }
  }
}

static int finish_frame(aac_decoder *d, int16_t *pcm, uint32_t *out_samples, uint32_t *out_channels)
{
  int ch;
  if (d->nchan <= 0 || d->nchan > AAC_MAX_CHANNELS)
    return AAC_EFORMAT;
  for (ch = 0; ch < d->nchan; ch++)
    filterbank(d, &d->ch[ch], ch);
  /* record window history for next frame's overlap */
  for (ch = 0; ch < d->nchan; ch++) {
    d->prev_win_seq[ch] = d->ch[ch].win_seq;
    d->prev_win_shape[ch] = d->ch[ch].win_shape;
  }
  d->have_prev = 1;
  interleave(d, pcm);
  if (out_samples)
    *out_samples = d->frame_len;
  if (out_channels)
    *out_channels = d->nchan;
  return AAC_OK;
}

int aac_decode(aac_decoder *d, const uint8_t *data, uint32_t len, int16_t *pcm, uint32_t *out_samples,
               uint32_t *out_channels)
{
  bitr b;
  int rc;
  if (!d || !d->configured || !data)
    return AAC_EPARAM;
  br_init(&b, data, len);
  if ((rc = decode_raw_block(d, &b)))
    return rc;
  return finish_frame(d, pcm, out_samples, out_channels);
}

int aac_decode_adts(aac_decoder *d, const uint8_t *data, uint32_t len, uint32_t *consumed, int16_t *pcm,
                    uint32_t *out_samples, uint32_t *out_channels)
{
  bitr b;
  uint32_t frame_bytes = 0;
  int rc;
  if (!d || !data)
    return AAC_EPARAM;
  br_init(&b, data, len);
  if ((rc = parse_adts(d, &b, &frame_bytes)) != AAC_OK)
    return rc;
  if (frame_bytes == 0 || frame_bytes > len)
    return AAC_EFORMAT;
  if ((rc = decode_raw_block(d, &b)))
    return rc;
  if (consumed)
    *consumed = frame_bytes;
  return finish_frame(d, pcm, out_samples, out_channels);
}
