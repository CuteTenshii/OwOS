/* webp.c — WebP container, lossless (VP8L) decoder + encoder, and the ALPH alpha plane for
 * lossy files (the VP8 key frame itself is decoded by vp8.c). See webp.h.
 *
 * Decoder: full VP8L — the four transforms (predictor, colour, subtract-green,
 * colour-indexing), the colour cache, LZ77 backward references, and
 * meta-Huffman groups. VP8L prefix codes are the DEFLATE canonical codes but
 * written bit-reversed (read LSB-first), so they are decoded through a small
 * binary tree keyed exactly like libwebp's GetNextKey rather than the MSB-first
 * canonical loop the inflate library uses.
 *
 * Encoder: subtract-green transform + greedy LZ77 (hash-chain match finder) +
 * dynamic Huffman, a single meta-Huffman group, no colour cache, distances
 * coded as plane_code = distance + 120 (the table-free range). The result is a
 * standards-conformant stream any decoder reads back, verified against dwebp.
 */
#include "webp.h"
#include "owos.h"
#include "vp8.h"
#include <stdint.h>

/* ====================================================================== *
 *  Shared constants                                                       *
 * ====================================================================== */

/* Distance plane-code -> packed (yoffset<<4 | (8-xoffset)); see PlaneToDist. */
static const uint8_t kCodeToPlane[120] = {
    0x18, 0x07, 0x17, 0x19, 0x28, 0x06, 0x27, 0x29, 0x16, 0x1a, 0x26, 0x2a, 0x38, 0x05, 0x37, 0x39, 0x15, 0x1b,
    0x36, 0x3a, 0x25, 0x2b, 0x48, 0x04, 0x47, 0x49, 0x14, 0x1c, 0x35, 0x3b, 0x46, 0x4a, 0x24, 0x2c, 0x58, 0x45,
    0x4b, 0x34, 0x3c, 0x03, 0x57, 0x59, 0x13, 0x1d, 0x56, 0x5a, 0x23, 0x2d, 0x44, 0x4c, 0x55, 0x5b, 0x33, 0x3d,
    0x68, 0x02, 0x67, 0x69, 0x12, 0x1e, 0x66, 0x6a, 0x22, 0x2e, 0x54, 0x5c, 0x43, 0x4d, 0x65, 0x6b, 0x32, 0x3e,
    0x78, 0x01, 0x77, 0x79, 0x53, 0x5d, 0x11, 0x1f, 0x64, 0x6c, 0x42, 0x4e, 0x76, 0x7a, 0x21, 0x2f, 0x75, 0x7b,
    0x31, 0x3f, 0x63, 0x6d, 0x52, 0x5e, 0x00, 0x74, 0x7c, 0x41, 0x4f, 0x10, 0x20, 0x62, 0x6e, 0x30, 0x73, 0x7d,
    0x51, 0x5f, 0x40, 0x72, 0x7e, 0x61, 0x6f, 0x50, 0x71, 0x7f, 0x60, 0x70};

/* The order code-length-code lengths are stored in (VP8L, == DEFLATE's order
 * with 16/17/18 moved to the front). */
static const int kCLOrder[19] = {17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

#define NUM_LEN_CODES 24  /* LZ77 length prefix codes (green alphabet tail) */
#define NUM_DIST_CODES 40 /* distance prefix codes                          */
#define GREEN_BASE 256    /* green alphabet = 256 literals + len + cache     */

#define DIV_RUP(a, b) (((a) + (b) - 1) / (b))

static int iabs(int v) { return v < 0 ? -v : v; }
static int log2floor(unsigned n)
{
  int l = -1;
  while (n) {
    l++;
    n >>= 1;
  }
  return l;
}

/* The bit-reversed canonical-code generator (libwebp GetNextKey): increments a
 * key in bit-reversed order so that reading the stream LSB-first indexes codes
 * directly. Both the decoder's tree build and the encoder's code assignment use
 * it, so the two agree by construction. */
static unsigned next_key(unsigned key, int len)
{
  unsigned step = 1u << (len - 1);
  while (key & step)
    step >>= 1;
  return step ? (key & (step - 1)) + step : key;
}

/* ====================================================================== *
 *  Decoder: bit reader (LSB-first)                                        *
 * ====================================================================== */

struct br {
  const unsigned char *buf;
  int len, pos;
  unsigned acc;
  int nbits;
  int eos;
};

static void br_init(struct br *b, const unsigned char *buf, int len)
{
  b->buf = buf;
  b->len = len;
  b->pos = 0;
  b->acc = 0;
  b->nbits = 0;
  b->eos = 0;
}

static unsigned br_read(struct br *b, int n)
{
  while (b->nbits < n) {
    unsigned c = (b->pos < b->len) ? b->buf[b->pos++] : (b->eos = 1, 0u);
    b->acc |= c << b->nbits;
    b->nbits += 8;
  }
  unsigned v = (n ? (b->acc & ((1u << n) - 1)) : 0u);
  b->acc >>= n;
  b->nbits -= n;
  return v;
}

/* ====================================================================== *
 *  Decoder: Huffman tree (bit-reversed canonical)                         *
 * ====================================================================== */

struct htnode {
  int child[2];
  int sym;
};
struct htree {
  struct htnode *nodes;
  int nnodes;
  int single; /* >= 0: a zero-bit code that always yields this symbol      */
  int empty;  /* 1: no symbols (decode is an error if it ever runs)        */
};

static void htree_init(struct htree *t)
{
  t->nodes = 0;
  t->nnodes = 0;
  t->single = -1;
  t->empty = 1;
}
static void htree_free(struct htree *t)
{
  if (t->nodes)
    free(t->nodes);
  t->nodes = 0;
  t->nnodes = 0;
}

static int ht_newnode(struct htree *t)
{
  int i = t->nnodes++;
  t->nodes[i].child[0] = t->nodes[i].child[1] = -1;
  t->nodes[i].sym = -1;
  return i;
}

/* Build the tree from per-symbol code lengths (0 = unused). */
static int htree_build(struct htree *t, const int *len, int n)
{
  htree_init(t);
  int used = 0, last = -1, maxlen = 0;
  long total_len = 0;
  for (int i = 0; i < n; i++)
    if (len[i] > 0) {
      used++;
      last = i;
      total_len += len[i];
      if (len[i] > maxlen)
        maxlen = len[i];
    }
  if (used == 0)
    return 0; /* empty code: legal if never used */
  t->empty = 0;
  if (used == 1) {
    t->single = last;
    return 0;
  }
  if (maxlen > 15)
    return -1;

  /* Reject an over-subscribed code (Kraft inequality). A canonical prefix
   * code satisfies sum(2^(maxlen-len)) <= 2^maxlen; a malformed one that
   * exceeds it would otherwise build a chain longer than the node pool. */
  long avail = 1L << maxlen;
  for (int i = 0; i < n; i++)
    if (len[i] > 0) {
      avail -= 1L << (maxlen - len[i]);
      if (avail < 0)
        return -1;
    }

  /* Descending a symbol of length L creates at most L new nodes, so the tree
   * never exceeds 1 (root) + sum(len) nodes — the bound below is exact for a
   * chain and generous for a complete code. The old 2*used+2 under-counted
   * sparse codes and let ht_newnode write past the pool. */
  t->nodes = malloc((unsigned)(1 + total_len) * sizeof *t->nodes);
  if (!t->nodes)
    return -1;
  ht_newnode(t); /* root = node 0                   */

  unsigned key = 0;
  for (int L = 1; L <= maxlen; L++) {
    for (int sym = 0; sym < n; sym++) {
      if (len[sym] != L)
        continue;
      int node = 0;
      for (int j = 0; j < L; j++) {
        int bit = (key >> j) & 1;
        if (t->nodes[node].child[bit] < 0)
          t->nodes[node].child[bit] = ht_newnode(t);
        node = t->nodes[node].child[bit];
      }
      t->nodes[node].sym = sym;
      key = next_key(key, L);
    }
  }
  return 0;
}

/* Build the two degenerate forms a "simple" code header describes directly. */
static void htree_single(struct htree *t, int sym)
{
  htree_init(t);
  t->empty = 0;
  t->single = sym;
}
static int htree_pair(struct htree *t, int s0, int s1)
{
  htree_init(t);
  t->empty = 0;
  t->nodes = malloc(3 * sizeof *t->nodes);
  if (!t->nodes)
    return -1;
  int root = ht_newnode(t);
  int a = ht_newnode(t), b = ht_newnode(t);
  t->nodes[a].sym = s0;
  t->nodes[b].sym = s1;
  t->nodes[root].child[0] = a;
  t->nodes[root].child[1] = b;
  return 0;
}

static int htree_decode(struct htree *t, struct br *b)
{
  if (t->empty)
    return -1;
  if (t->single >= 0)
    return t->single;
  int node = 0;
  for (;;) {
    int bit = (int)br_read(b, 1);
    if (b->eos)
      return -1;
    node = t->nodes[node].child[bit];
    if (node < 0)
      return -1;
    if (t->nodes[node].sym >= 0)
      return t->nodes[node].sym;
  }
}

/* Read one prefix code's header into *t (simple or normal form). */
static int read_huffman_code(struct br *b, struct htree *t, int alphabet)
{
  if (br_read(b, 1)) { /* simple code            */
    int nsym = (int)br_read(b, 1) + 1;
    int first8 = (int)br_read(b, 1);
    int s0 = (int)br_read(b, first8 ? 8 : 1);
    if (s0 >= alphabet)
      return -1;
    if (nsym == 1) {
      htree_single(t, s0);
      return 0;
    }
    int s1 = (int)br_read(b, 8);
    if (s1 >= alphabet)
      return -1;
    return htree_pair(t, s0, s1);
  }

  /* Normal code: read the code-length code, then the per-symbol lengths. */
  int ncl = 4 + (int)br_read(b, 4);
  int cll[19] = {0};
  for (int i = 0; i < ncl; i++)
    cll[kCLOrder[i]] = (int)br_read(b, 3);
  struct htree clh;
  if (htree_build(&clh, cll, 19) != 0)
    return -1;

  int max_symbol;
  if (br_read(b, 1)) {
    int nb = 2 + 2 * (int)br_read(b, 3);
    max_symbol = 2 + (int)br_read(b, nb);
  } else {
    max_symbol = alphabet;
  }

  static const int rep_extra[3] = {2, 3, 7};
  static const int rep_offset[3] = {3, 3, 11};
  int *lengths = calloc((unsigned)alphabet, sizeof *lengths);
  if (!lengths) {
    htree_free(&clh);
    return -1;
  }

  int sym = 0, prev = 8, rc = 0;
  while (sym < alphabet) {
    if (max_symbol-- == 0)
      break;
    int code_len = htree_decode(&clh, b);
    if (code_len < 0) {
      rc = -1;
      break;
    }
    if (code_len < 16) {
      lengths[sym++] = code_len;
      if (code_len)
        prev = code_len;
    } else {
      int slot = code_len - 16;
      int repeat = (int)br_read(b, rep_extra[slot]) + rep_offset[slot];
      int val = (code_len == 16) ? prev : 0;
      if (sym + repeat > alphabet) {
        rc = -1;
        break;
      }
      while (repeat-- > 0)
        lengths[sym++] = val;
    }
    if (b->eos) {
      rc = -1;
      break;
    }
  }
  htree_free(&clh);
  if (rc == 0)
    rc = htree_build(t, lengths, alphabet);
  free(lengths);
  return rc;
}

/* ====================================================================== *
 *  Decoder: colour cache + LZ77 helpers                                  *
 * ====================================================================== */

struct ccache {
  unsigned *tab;
  int bits;
};

static void cc_insert(struct ccache *c, unsigned px)
{
  if (c->bits)
    c->tab[(0x1e35a7bdu * px) >> (32 - c->bits)] = px;
}

/* Prefix-coded value: symbol (0..) -> length/distance, reading extra bits. */
static int prefix_value(struct br *b, int sym)
{
  if (sym < 4)
    return sym + 1;
  int extra = (sym - 2) >> 1;
  int offset = (2 + (sym & 1)) << extra;
  return offset + (int)br_read(b, extra) + 1;
}

static int plane_to_dist(int plane, int xsize)
{
  if (plane > 120)
    return plane - 120;
  int dc = kCodeToPlane[plane - 1];
  int dist = (dc >> 4) * xsize + (8 - (dc & 0xf));
  return dist >= 1 ? dist : 1;
}

/* ====================================================================== *
 *  Decoder: inverse transforms                                           *
 * ====================================================================== */

enum { TR_PRED = 0, TR_COLOR = 1, TR_SUBGREEN = 2, TR_INDEX = 3 };

struct transform {
  int type;
  int bits;       /* size_bits (predictor / colour)                      */
  unsigned *data; /* sub-image (predictor image / colour image / table)  */
  int data_w;
  int table_size; /* colour-indexing: number of palette entries          */
  int width_bits; /* colour-indexing: pixel bundling                     */
  int orig_w;     /* colour-indexing: width before bundling              */
};

static unsigned avg2(unsigned a, unsigned b) { return (a & b) + (((a ^ b) & 0xfefefefeu) >> 1); }

static int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static unsigned clamp_add_sub_full(unsigned a, unsigned b, unsigned c)
{
  unsigned out = 0;
  for (int s = 0; s < 32; s += 8) {
    int v = clamp255((int)((a >> s) & 0xff) + (int)((b >> s) & 0xff) - (int)((c >> s) & 0xff));
    out |= (unsigned)v << s;
  }
  return out;
}
static unsigned clamp_add_sub_half(unsigned a, unsigned b)
{
  unsigned out = 0;
  for (int s = 0; s < 32; s += 8) {
    int ca = (int)((a >> s) & 0xff), cb = (int)((b >> s) & 0xff);
    out |= (unsigned)clamp255(ca + (ca - cb) / 2) << s;
  }
  return out;
}
static unsigned predict_select(unsigned L, unsigned T, unsigned TL)
{
  int pL = 0, pT = 0;
  for (int s = 0; s < 32; s += 8) {
    int l = (L >> s) & 0xff, t = (T >> s) & 0xff, tl = (TL >> s) & 0xff;
    int p = l + t - tl;
    pL += iabs(p - l);
    pT += iabs(p - t);
  }
  return pL < pT ? L : T;
}

static unsigned predict(int mode, unsigned L, unsigned T, unsigned TR, unsigned TL)
{
  switch (mode) {
  case 0:
    return 0xff000000u;
  case 1:
    return L;
  case 2:
    return T;
  case 3:
    return TR;
  case 4:
    return TL;
  case 5:
    return avg2(avg2(L, TR), T);
  case 6:
    return avg2(L, TL);
  case 7:
    return avg2(L, T);
  case 8:
    return avg2(TL, T);
  case 9:
    return avg2(T, TR);
  case 10:
    return avg2(avg2(L, TL), avg2(T, TR));
  case 11:
    return predict_select(L, T, TL);
  case 12:
    return clamp_add_sub_full(L, T, TL);
  case 13:
    return clamp_add_sub_half(avg2(L, T), TL);
  default:
    return 0xff000000u;
  }
}

static unsigned add_argb(unsigned a, unsigned b)
{
  return (((a >> 24) + (b >> 24)) & 0xff) << 24 | ((((a >> 16) & 0xff) + ((b >> 16) & 0xff)) & 0xff) << 16 |
         ((((a >> 8) & 0xff) + ((b >> 8) & 0xff)) & 0xff) << 8 | (((a & 0xff) + (b & 0xff)) & 0xff);
}

static void inv_predictor(struct transform *t, unsigned *d, int w, int h)
{
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      int idx = y * w + x;
      unsigned pred;
      if (x == 0 && y == 0)
        pred = 0xff000000u;
      else if (y == 0)
        pred = d[idx - 1];
      else if (x == 0)
        pred = d[idx - w];
      else {
        int mode = (t->data[(y >> t->bits) * t->data_w + (x >> t->bits)] >> 8) & 0xff;
        unsigned Lp = d[idx - 1], Tp = d[idx - w], TLp = d[idx - w - 1];
        unsigned TRp = (x == w - 1) ? d[y * w] : d[idx - w + 1];
        pred = predict(mode, Lp, Tp, TRp, TLp);
      }
      d[idx] = add_argb(d[idx], pred);
    }
  }
}

static int color_delta(int t, int c) { return ((int8_t)t * (int8_t)c) >> 5; }

static void inv_color(struct transform *t, unsigned *d, int w, int h)
{
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      int idx = y * w + x;
      unsigned cte = t->data[(y >> t->bits) * t->data_w + (x >> t->bits)];
      int g2r = cte & 0xff, g2b = (cte >> 8) & 0xff, r2b = (cte >> 16) & 0xff;
      unsigned px = d[idx];
      int green = (px >> 8) & 0xff;
      int red = (px >> 16) & 0xff, blue = px & 0xff;
      red = (red + color_delta(g2r, green)) & 0xff;
      blue = (blue + color_delta(g2b, green)) & 0xff;
      blue = (blue + color_delta(r2b, red)) & 0xff;
      d[idx] = (px & 0xff00ff00u) | ((unsigned)red << 16) | (unsigned)blue;
    }
  }
}

static void inv_subgreen(unsigned *d, int n)
{
  for (int i = 0; i < n; i++) {
    unsigned px = d[i];
    int green = (px >> 8) & 0xff;
    int red = (((px >> 16) & 0xff) + green) & 0xff;
    int blue = ((px & 0xff) + green) & 0xff;
    d[i] = (px & 0xff00ff00u) | ((unsigned)red << 16) | (unsigned)blue;
  }
}

/* Expand a colour-indexed (and possibly pixel-bundled) image back to full
 * colour. Returns a fresh buffer of orig_w*h pixels via *out, or -1. */
static int inv_colorindex(struct transform *t, unsigned *d, int w, int h, unsigned **out, int *out_w)
{
  int n = t->table_size, wb = t->width_bits;
  int per = 1 << wb, bits = 8 >> wb, mask = (1 << bits) - 1;
  int ow = t->orig_w;
  unsigned *nd = malloc((unsigned)ow * h * 4);
  if (!nd)
    return -1;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      int green = (d[y * w + x] >> 8) & 0xff;
      for (int s = 0; s < per; s++) {
        int ox = x * per + s;
        if (ox >= ow)
          break;
        int idx = (green >> (s * bits)) & mask;
        nd[y * ow + ox] = (idx < n) ? t->data[idx] : 0u;
      }
    }
  }
  *out = nd;
  *out_w = ow;
  return 0;
}

/* ====================================================================== *
 *  Decoder: image stream (recursive)                                     *
 * ====================================================================== */

struct hgroup {
  struct htree codes[5];
};

static unsigned *decode_stream(struct br *b, int xsize, int ysize, int level0, int *err);

/* Read num_groups * 5 Huffman codes and decode xsize*ysize pixels. */
static int decode_pixels(struct br *b, unsigned *data, int W, int H, struct hgroup *groups, int ng, int cache_bits,
                         unsigned *huff_img, int hbits, int hpw)
{
  struct ccache cc;
  cc.bits = cache_bits;
  cc.tab = cache_bits ? calloc((unsigned)(1 << cache_bits), 4) : 0;
  if (cache_bits && !cc.tab)
    return -1;

  int total = W * H, pos = 0, x = 0, y = 0, rc = 0;
  while (pos < total) {
    struct hgroup *g = &groups[0];
    if (ng > 1) {
      int gi = (huff_img[(y >> hbits) * hpw + (x >> hbits)] >> 8) & 0xffff;
      if (gi >= ng) {
        rc = -1;
        break;
      }
      g = &groups[gi];
    }
    int S = htree_decode(&g->codes[0], b);
    if (S < 0) {
      rc = -1;
      break;
    }

    if (S < 256) { /* literal pixel       */
      int green = S;
      int red = htree_decode(&g->codes[1], b);
      int blue = htree_decode(&g->codes[2], b);
      int alpha = htree_decode(&g->codes[3], b);
      if (red < 0 || blue < 0 || alpha < 0) {
        rc = -1;
        break;
      }
      unsigned px = ((unsigned)alpha << 24) | ((unsigned)red << 16) | ((unsigned)green << 8) | (unsigned)blue;
      data[pos++] = px;
      cc_insert(&cc, px);
      if (++x == W) {
        x = 0;
        y++;
      }
    } else if (S < GREEN_BASE + NUM_LEN_CODES) { /* LZ77 backward copy  */
      int length = prefix_value(b, S - 256);
      int dsym = htree_decode(&g->codes[4], b);
      if (dsym < 0) {
        rc = -1;
        break;
      }
      int dist = plane_to_dist(prefix_value(b, dsym), W);
      if (dist > pos || length <= 0 || pos + length > total) {
        rc = -1;
        break;
      }
      for (int i = 0; i < length; i++) {
        unsigned px = data[pos - dist];
        data[pos++] = px;
        cc_insert(&cc, px);
        if (++x == W) {
          x = 0;
          y++;
        }
      }
    } else { /* colour-cache hit    */
      int ci = S - GREEN_BASE - NUM_LEN_CODES;
      if (!cache_bits || ci >= (1 << cache_bits)) {
        rc = -1;
        break;
      }
      unsigned px = cc.tab[ci];
      data[pos++] = px;
      cc_insert(&cc, px);
      if (++x == W) {
        x = 0;
        y++;
      }
    }
    if (b->eos) {
      rc = -1;
      break;
    }
  }
  if (cc.tab)
    free(cc.tab);
  return rc;
}

static unsigned *decode_stream(struct br *b, int xsize, int ysize, int level0, int *err)
{
  struct transform tr[4];
  int ntr = 0, cur_w = xsize;
  unsigned *huff_img = 0;
  struct hgroup *groups = 0;
  int ng = 1;
  unsigned *data = 0;

  for (int i = 0; i < 4; i++)
    tr[i].data = 0;

  /* 1. transforms (top-level image only) */
  if (level0) {
    while (br_read(b, 1)) {
      if (ntr >= 4 || b->eos)
        goto fail;
      struct transform *t = &tr[ntr];
      t->type = (int)br_read(b, 2);
      t->data = 0;
      if (t->type == TR_PRED || t->type == TR_COLOR) {
        t->bits = 2 + (int)br_read(b, 3);
        int bw = DIV_RUP(cur_w, 1 << t->bits);
        int bh = DIV_RUP(ysize, 1 << t->bits);
        t->data_w = bw;
        t->data = decode_stream(b, bw, bh, 0, err);
        if (!t->data)
          goto fail;
      } else if (t->type == TR_SUBGREEN) {
        /* no data */
      } else { /* colour indexing      */
        int n = (int)br_read(b, 8) + 1;
        t->table_size = n;
        unsigned *ct = decode_stream(b, n, 1, 0, err);
        if (!ct)
          goto fail;
        for (int k = 1; k < n; k++)
          ct[k] = add_argb(ct[k], ct[k - 1]);
        t->data = ct;
        int wb = (n <= 2) ? 3 : (n <= 4) ? 2 : (n <= 16) ? 1 : 0;
        t->width_bits = wb;
        t->orig_w = cur_w;
        cur_w = DIV_RUP(cur_w, 1 << wb);
      }
      ntr++;
    }
  }

  /* 2. colour cache */
  int cache_bits = 0;
  if (br_read(b, 1)) {
    cache_bits = (int)br_read(b, 4);
    if (cache_bits < 1 || cache_bits > 11)
      goto fail;
  }

  /* 3. meta-Huffman image (top-level only) */
  int hbits = 0, hpw = 0;
  if (level0 && br_read(b, 1)) {
    hbits = 2 + (int)br_read(b, 3);
    hpw = DIV_RUP(cur_w, 1 << hbits);
    int hph = DIV_RUP(ysize, 1 << hbits);
    huff_img = decode_stream(b, hpw, hph, 0, err);
    if (!huff_img)
      goto fail;
    int mx = 0;
    for (int i = 0; i < hpw * hph; i++) {
      int v = (huff_img[i] >> 8) & 0xffff;
      if (v > mx)
        mx = v;
    }
    ng = mx + 1;
  }

  /* 4. Huffman code groups */
  int green_alpha = GREEN_BASE + NUM_LEN_CODES + (cache_bits ? (1 << cache_bits) : 0);
  groups = malloc((unsigned)ng * sizeof *groups);
  if (!groups)
    goto fail;
  for (int g = 0; g < ng; g++)
    for (int c = 0; c < 5; c++)
      htree_init(&groups[g].codes[c]);
  for (int g = 0; g < ng; g++) {
    if (read_huffman_code(b, &groups[g].codes[0], green_alpha) != 0)
      goto fail;
    if (read_huffman_code(b, &groups[g].codes[1], 256) != 0)
      goto fail;
    if (read_huffman_code(b, &groups[g].codes[2], 256) != 0)
      goto fail;
    if (read_huffman_code(b, &groups[g].codes[3], 256) != 0)
      goto fail;
    if (read_huffman_code(b, &groups[g].codes[4], NUM_DIST_CODES) != 0)
      goto fail;
  }

  /* 5. pixels */
  data = malloc((unsigned)cur_w * ysize * 4);
  if (!data)
    goto fail;
  if (decode_pixels(b, data, cur_w, ysize, groups, ng, cache_bits, huff_img, hbits, hpw) != 0)
    goto fail;

  /* tidy the entropy machinery now it's done */
  for (int g = 0; g < ng; g++)
    for (int c = 0; c < 5; c++)
      htree_free(&groups[g].codes[c]);
  free(groups);
  groups = 0;
  if (huff_img) {
    free(huff_img);
    huff_img = 0;
  }

  /* 6. inverse transforms, last-read first */
  for (int i = ntr - 1; i >= 0; i--) {
    struct transform *t = &tr[i];
    if (t->type == TR_PRED)
      inv_predictor(t, data, cur_w, ysize);
    else if (t->type == TR_COLOR)
      inv_color(t, data, cur_w, ysize);
    else if (t->type == TR_SUBGREEN)
      inv_subgreen(data, cur_w * ysize);
    else {
      unsigned *nd;
      int nw;
      if (inv_colorindex(t, data, cur_w, ysize, &nd, &nw) != 0)
        goto fail;
      free(data);
      data = nd;
      cur_w = nw;
    }
  }
  for (int i = 0; i < ntr; i++)
    if (tr[i].data)
      free(tr[i].data);
  return data;

fail:
  *err = 1;
  if (groups) {
    for (int g = 0; g < ng; g++)
      for (int c = 0; c < 5; c++)
        htree_free(&groups[g].codes[c]);
    free(groups);
  }
  if (huff_img)
    free(huff_img);
  if (data)
    free(data);
  for (int i = 0; i < ntr; i++)
    if (tr[i].data)
      free(tr[i].data);
  return 0;
}

/* ====================================================================== *
 *  Container parsing                                                      *
 * ====================================================================== */

static unsigned le32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }

static int tag_is(const unsigned char *p, const char *s)
{
  return p[0] == s[0] && p[1] == s[1] && p[2] == s[2] && p[3] == s[3];
}

/* Scan the RIFF chunks once; report the offsets/sizes we care about. */
struct scan {
  int has_vp8x, has_anim, has_alph, has_vp8, has_vp8l;
  int vp8x_off;
  int alph_off, alph_size;
  int vp8_off, vp8_size;
  int vp8l_off, vp8l_size;
};

static int scan_container(const unsigned char *buf, int len, struct scan *s)
{
  memset(s, 0, sizeof *s);
  if (len < 12 || !tag_is(buf, "RIFF") || !tag_is(buf + 8, "WEBP"))
    return WEBP_EBADSIG;
  /* All chunk arithmetic is unsigned: a chunk size with the high bit set must
   * not become a negative int and either alias forever or produce a negative
   * psize downstream. */
  unsigned pos = 12;
  while (pos + 8 <= (unsigned)len) {
    const unsigned char *c = buf + pos;
    unsigned size = le32(c + 4);
    unsigned payload = pos + 8;
    unsigned avail = (unsigned)len - payload;
    int psize = (int)(size < avail ? size : avail);
    if (tag_is(c, "VP8X")) {
      s->has_vp8x = 1;
      s->vp8x_off = (int)payload;
    } else if (tag_is(c, "ANIM") || tag_is(c, "ANMF"))
      s->has_anim = 1;
    else if (tag_is(c, "ALPH")) {
      s->has_alph = 1;
      s->alph_off = (int)payload;
      s->alph_size = psize;
    } else if (tag_is(c, "VP8 ")) {
      s->has_vp8 = 1;
      s->vp8_off = (int)payload;
      s->vp8_size = psize;
    } else if (tag_is(c, "VP8L")) {
      s->has_vp8l = 1;
      s->vp8l_off = (int)payload;
      s->vp8l_size = psize;
    }
    unsigned next = payload + size + (size & 1);
    if (next <= pos)
      break; /* wrap or no forward progress      */
    pos = next;
  }
  return WEBP_OK;
}

int webp_probe(const unsigned char *buf, int len, struct webp_info *info)
{
  struct scan s;
  int rc = scan_container(buf, len, &s);
  if (rc != WEBP_OK)
    return rc;
  memset(info, 0, sizeof *info);
  info->extended = s.has_vp8x;
  info->animation = s.has_anim;
  info->lossless = s.has_vp8l && !s.has_vp8;

  if (s.has_vp8x && s.vp8x_off + 10 <= len) {
    const unsigned char *p = buf + s.vp8x_off;
    info->has_alpha = (p[0] & 0x10) != 0;
    info->w = 1 + (p[4] | (p[5] << 8) | (p[6] << 16));
    info->h = 1 + (p[7] | (p[8] << 8) | (p[9] << 16));
    return WEBP_OK;
  }
  if (s.has_vp8l && s.vp8l_off + 5 <= len) {
    const unsigned char *p = buf + s.vp8l_off;
    if (p[0] != 0x2f)
      return WEBP_EFORMAT;
    unsigned bits = p[1] | (p[2] << 8) | (p[3] << 16) | ((unsigned)p[4] << 24);
    info->w = (bits & 0x3fff) + 1;
    info->h = ((bits >> 14) & 0x3fff) + 1;
    info->has_alpha = (bits >> 28) & 1;
    return WEBP_OK;
  }
  if (s.has_vp8 && s.vp8_off + 10 <= len) {
    const unsigned char *p = buf + s.vp8_off;
    if (p[3] == 0x9d && p[4] == 0x01 && p[5] == 0x2a) {
      info->w = (p[6] | (p[7] << 8)) & 0x3fff;
      info->h = (p[8] | (p[9] << 8)) & 0x3fff;
      info->has_alpha = s.has_alph;
      return WEBP_OK;
    }
  }
  return WEBP_EFORMAT;
}

int webp_dimensions(const unsigned char *buf, int len, unsigned *w, unsigned *h)
{
  struct webp_info info;
  int rc = webp_probe(buf, len, &info);
  if (rc != WEBP_OK)
    return rc;
  if (w)
    *w = info.w;
  if (h)
    *h = info.h;
  return WEBP_OK;
}

/* Undo the ALPH chunk's spatial prediction filter in place (0 none, 1 horizontal, 2 vertical, 3 gradient). */
static void alpha_unfilter(unsigned char *a, int w, int h, int method)
{
  if (method == 0)
    return;
  for (int y = 0; y < h; y++) {
    unsigned char *row = a + (unsigned)y * w;
    const unsigned char *up = y ? row - w : 0;
    for (int x = 0; x < w; x++) {
      int pred;
      if (!y && !x)
        pred = 0;
      else if (!y)
        pred = row[x - 1];
      else if (!x)
        pred = up[0];
      else if (method == 1)
        pred = row[x - 1];
      else if (method == 2)
        pred = up[x];
      else {
        pred = row[x - 1] + up[x] - up[x - 1];
        pred = pred < 0 ? 0 : pred > 255 ? 255 : pred;
      }
      row[x] = (unsigned char)(row[x] + pred);
    }
  }
}

/* Decode the ALPH chunk into the A bytes of an RGBA buffer. A bad chunk leaves the image opaque. */
static void apply_alpha(const unsigned char *alph, int size, unsigned char *rgba, int w, int h)
{
  if (size < 1)
    return;
  unsigned n = (unsigned)w * h;
  int method = alph[0] & 3, filter = (alph[0] >> 2) & 3;
  unsigned char *plane = malloc(n);
  if (!plane)
    return;
  if (method == 0) {
    if ((unsigned)size - 1 < n) {
      free(plane);
      return;
    }
    memcpy(plane, alph + 1, n);
  } else if (method == 1) {
    struct br b;
    br_init(&b, alph + 1, size - 1);
    int err = 0;
    unsigned *argb = decode_stream(&b, w, h, 1, &err);
    if (!argb || b.eos) {
      free(argb);
      free(plane);
      return;
    }
    for (unsigned i = 0; i < n; i++)
      plane[i] = (unsigned char)(argb[i] >> 8);
    free(argb);
  } else {
    free(plane);
    return;
  }
  alpha_unfilter(plane, w, h, filter);
  for (unsigned i = 0; i < n; i++)
    rgba[i * 4 + 3] = plane[i];
  free(plane);
}

static int decode_lossy(const unsigned char *buf, const struct scan *s, unsigned char **out, unsigned *w, unsigned *h)
{
  int rc = vp8_decode(buf + s->vp8_off, s->vp8_size, out, w, h);
  if (rc != VP8_OK)
    return rc == VP8_ENOMEM ? WEBP_ENOMEM : WEBP_EFORMAT;
  if (s->has_alph)
    apply_alpha(buf + s->alph_off, s->alph_size, *out, (int)*w, (int)*h);
  return WEBP_OK;
}

int webp_decode(const unsigned char *buf, int len, unsigned char **out, unsigned *w, unsigned *h)
{
  *out = 0;
  struct scan s;
  int rc = scan_container(buf, len, &s);
  if (rc != WEBP_OK)
    return rc;
  if (s.has_anim)
    return WEBP_EUNSUP;
  if (!s.has_vp8l)
    return s.has_vp8 ? decode_lossy(buf, &s, out, w, h) : WEBP_EFORMAT;

  struct br b;
  br_init(&b, buf + s.vp8l_off, s.vp8l_size);
  if (br_read(&b, 8) != 0x2f)
    return WEBP_EFORMAT;
  int width = (int)br_read(&b, 14) + 1;
  int height = (int)br_read(&b, 14) + 1;
  (void)br_read(&b, 1); /* alpha hint          */
  if (br_read(&b, 3) != 0)
    return WEBP_EUNSUP; /* version             */

  int err = 0;
  unsigned *argb = decode_stream(&b, width, height, 1, &err);
  if (!argb)
    return err ? WEBP_EFORMAT : WEBP_ENOMEM;
  if (b.eos) {
    free(argb);
    return WEBP_ETRUNC;
  }

  unsigned char *rgba = malloc((unsigned)width * height * 4);
  if (!rgba) {
    free(argb);
    return WEBP_ENOMEM;
  }
  for (int i = 0; i < width * height; i++) {
    unsigned px = argb[i];
    rgba[i * 4 + 0] = (px >> 16) & 0xff;
    rgba[i * 4 + 1] = (px >> 8) & 0xff;
    rgba[i * 4 + 2] = px & 0xff;
    rgba[i * 4 + 3] = (px >> 24) & 0xff;
  }
  free(argb);
  *out = rgba;
  if (w)
    *w = width;
  if (h)
    *h = height;
  return WEBP_OK;
}

/* ====================================================================== *
 *  Encoder: bit writer (LSB-first)                                       *
 * ====================================================================== */

struct bw {
  unsigned char *buf;
  int cap, len;
  unsigned acc;
  int nbits;
  int oom;
};

static void bw_init(struct bw *w)
{
  w->cap = 65536;
  w->len = 0;
  w->acc = 0;
  w->nbits = 0;
  w->oom = 0;
  w->buf = malloc((unsigned)w->cap);
  if (!w->buf)
    w->oom = 1;
}
static void bw_byte(struct bw *w, unsigned char b)
{
  if (w->len >= w->cap) {
    int nc = w->cap * 2;
    unsigned char *nb = realloc(w->buf, (unsigned)nc);
    if (!nb) {
      w->oom = 1;
      return;
    }
    w->buf = nb;
    w->cap = nc;
  }
  w->buf[w->len++] = b;
}
static void bw_bits(struct bw *w, unsigned v, int n)
{
  if (n <= 0)
    return;
  w->acc |= (v & ((n < 32 ? (1u << n) : 0u) - 1u)) << w->nbits;
  w->nbits += n;
  while (w->nbits >= 8) {
    bw_byte(w, (unsigned char)(w->acc & 0xff));
    w->acc >>= 8;
    w->nbits -= 8;
  }
}
static void bw_flush(struct bw *w)
{
  if (w->nbits > 0) {
    bw_byte(w, (unsigned char)(w->acc & 0xff));
    w->acc = 0;
    w->nbits = 0;
  }
}

/* ====================================================================== *
 *  Encoder: length-limited Huffman                                       *
 * ====================================================================== */

/* Optimal (unbounded) Huffman code lengths for m leaves, weights w[], into
 * out_len[] (depths). O(m^2), m small. */
static void huffman_optimal(const unsigned *w, int m, int *out_len)
{
  if (m == 1) {
    out_len[0] = 0;
    return;
  }
  int N = 2 * m;
  unsigned *wt = malloc((unsigned)N * sizeof *wt);
  int *par = malloc((unsigned)N * sizeof *par);
  for (int i = 0; i < m; i++) {
    wt[i] = w[i];
    par[i] = -1;
  }
  int cnt = m;
  for (int step = 0; step < m - 1; step++) {
    int a = -1, b = -1;
    for (int i = 0; i < cnt; i++) {
      if (par[i] != -1)
        continue;
      if (a < 0 || wt[i] < wt[a]) {
        b = a;
        a = i;
      } else if (b < 0 || wt[i] < wt[b]) {
        b = i;
      }
    }
    int nn = cnt++;
    wt[nn] = wt[a] + wt[b];
    par[nn] = -1;
    par[a] = nn;
    par[b] = nn;
  }
  for (int i = 0; i < m; i++) {
    int d = 0, j = i;
    while (par[j] != -1) {
      d++;
      j = par[j];
    }
    out_len[i] = d;
  }
  free(wt);
  free(par);
}

/* Per-symbol code lengths from frequencies, max length L, complete (Kraft == 1)
 * over the used symbols. Falls back to a near-balanced complete code if the
 * optimal code exceeds L (rare). A single used symbol gets length 1 (the
 * decoder treats it as a zero-bit code). */
static void build_lengths(const unsigned *freq, int n, int L, int *out_len)
{
  for (int i = 0; i < n; i++)
    out_len[i] = 0;
  int m = 0;
  for (int i = 0; i < n; i++)
    if (freq[i] > 0)
      m++;
  if (m == 0)
    return;
  if (m == 1) {
    for (int i = 0; i < n; i++)
      if (freq[i])
        out_len[i] = 1;
    return;
  }

  int *sym = malloc((unsigned)m * sizeof *sym);
  unsigned *w = malloc((unsigned)m * sizeof *w);
  int *ol = malloc((unsigned)m * sizeof *ol);
  int k = 0;
  for (int i = 0; i < n; i++)
    if (freq[i]) {
      sym[k] = i;
      w[k] = freq[i];
      k++;
    }

  huffman_optimal(w, m, ol);
  int mx = 0;
  for (int i = 0; i < m; i++)
    if (ol[i] > mx)
      mx = ol[i];

  if (mx <= L) {
    for (int i = 0; i < m; i++)
      out_len[sym[i]] = ol[i];
  } else {
    /* near-balanced complete code, most frequent symbols shortest */
    for (int i = 0; i < m; i++)
      for (int j = i + 1; j < m; j++)
        if (w[j] > w[i]) {
          unsigned tw = w[i];
          w[i] = w[j];
          w[j] = tw;
          int ts = sym[i];
          sym[i] = sym[j];
          sym[j] = ts;
        }
    int kk = log2floor((unsigned)m);
    int a = (1 << (kk + 1)) - m;
    for (int i = 0; i < m; i++)
      out_len[sym[i]] = (i < a) ? kk : (kk + 1);
  }
  free(sym);
  free(w);
  free(ol);
}

/* ====================================================================== *
 *  Encoder: per-table code (simple or normal)                            *
 * ====================================================================== */

struct enc_table {
  int kind;    /* 0 = simple, 2 = normal                              */
  int nsimple; /* simple: 1 or 2 symbols                              */
  int s0, s1;
  int n;         /* alphabet                                            */
  int *len;      /* normal: stored code lengths                         */
  unsigned *key; /* normal: bit-reversed canonical codes                */
  int *wlen;     /* normal: bits to emit (0 for a single symbol)        */
};

/* Assign bit-reversed canonical codes from code lengths (mirrors htree_build).
 */
static void build_codes(const int *len, int n, unsigned *key, int *wlen)
{
  int used = 0, single = -1, maxlen = 0;
  for (int i = 0; i < n; i++)
    if (len[i] > 0) {
      used++;
      single = i;
      if (len[i] > maxlen)
        maxlen = len[i];
    }
  if (used == 0)
    return;
  if (used == 1) {
    key[single] = 0;
    wlen[single] = 0;
    return;
  }
  unsigned k = 0;
  for (int L = 1; L <= maxlen; L++)
    for (int sym = 0; sym < n; sym++)
      if (len[sym] == L) {
        key[sym] = k;
        wlen[sym] = L;
        k = next_key(k, L);
      }
}

static int enc_table_build(struct enc_table *et, const unsigned *freq, int n)
{
  int used = 0, s0 = -1, s1 = -1, maxsym = 0;
  for (int i = 0; i < n; i++)
    if (freq[i]) {
      used++;
      if (s0 < 0)
        s0 = i;
      else if (s1 < 0)
        s1 = i;
      if (i > maxsym)
        maxsym = i;
    }

  et->n = n;
  et->len = 0;
  et->key = 0;
  et->wlen = 0;
  if (used <= 2 && maxsym <= 255) {
    et->kind = 0;
    if (used == 0) {
      et->nsimple = 1;
      et->s0 = 0;
      et->s1 = -1;
    } else if (used == 1) {
      et->nsimple = 1;
      et->s0 = s0;
      et->s1 = -1;
    } else {
      et->nsimple = 2;
      et->s0 = s0;
      et->s1 = s1;
    }
    return 0;
  }
  et->kind = 2;
  et->len = calloc((unsigned)n, sizeof *et->len);
  et->key = calloc((unsigned)n, sizeof *et->key);
  et->wlen = calloc((unsigned)n, sizeof *et->wlen);
  if (!et->len || !et->key || !et->wlen)
    return -1;
  build_lengths(freq, n, 15, et->len);
  build_codes(et->len, n, et->key, et->wlen);
  return 0;
}
static void enc_table_free(struct enc_table *et)
{
  if (et->len)
    free(et->len);
  if (et->key)
    free(et->key);
  if (et->wlen)
    free(et->wlen);
}

/* Emit a normal code's header: the 19 code-length-code lengths, then this
 * code's per-symbol lengths (no run-length compression). */
static void emit_normal_header(struct bw *w, const int *len, int n)
{
  unsigned clfreq[19] = {0};
  for (int i = 0; i < n; i++)
    clfreq[len[i]]++;
  int cllen[19];
  build_lengths(clfreq, 19, 7, cllen);
  unsigned clkey[19] = {0};
  int clwlen[19] = {0};
  build_codes(cllen, 19, clkey, clwlen);

  bw_bits(w, 15, 4); /* num_code_lengths = 19   */
  for (int i = 0; i < 19; i++)
    bw_bits(w, (unsigned)cllen[kCLOrder[i]], 3);
  bw_bits(w, 0, 1); /* max_symbol = alphabet   */
  for (int i = 0; i < n; i++) {
    int v = len[i];
    bw_bits(w, clkey[v], clwlen[v]);
  }
}

static void emit_table_header(struct bw *w, struct enc_table *et)
{
  if (et->kind == 0) {
    bw_bits(w, 1, 1);
    bw_bits(w, (unsigned)(et->nsimple - 1), 1);
    int first8 = (et->s0 > 1) ? 1 : 0;
    bw_bits(w, (unsigned)first8, 1);
    bw_bits(w, (unsigned)et->s0, first8 ? 8 : 1);
    if (et->nsimple == 2)
      bw_bits(w, (unsigned)et->s1, 8);
  } else {
    bw_bits(w, 0, 1);
    emit_normal_header(w, et->len, et->n);
  }
}

static void write_symbol(struct bw *w, struct enc_table *et, int sym)
{
  if (et->kind == 0) {
    if (et->nsimple == 2)
      bw_bits(w, (unsigned)(sym == et->s1), 1);
  } else {
    bw_bits(w, et->key[sym], et->wlen[sym]);
  }
}

/* ====================================================================== *
 *  Encoder: LZ77 + prefix coding                                         *
 * ====================================================================== */

static void prefix_encode(int v, int *code, int *ebits, int *eval)
{
  if (v <= 4) {
    *code = v - 1;
    *ebits = 0;
    *eval = 0;
    return;
  }
  int D = v - 1;
  int h = log2floor((unsigned)D);
  int second = (D >> (h - 1)) & 1;
  *code = 2 * h + second;
  *ebits = h - 1;
  *eval = D & ((1 << *ebits) - 1);
}

#define HBITS 16
#define HSIZE (1 << HBITS)
#define MIN_MATCH 2
#define MAX_MATCH 4096
#define MAX_DIST (1 << 20)
#define CHAIN_MAX 64

static int lz_hash(unsigned px) { return (int)((px * 0x1e35a7bdu) >> (32 - HBITS)); }

typedef void (*tok_cb)(void *ctx, int copy, unsigned px, int len, int dist);

/* Tokenize the transformed pixels deterministically (same output every pass).
 */
static int lz77_run(const unsigned *t, int n, int *head, int *prev, tok_cb cb, void *ctx)
{
  for (int i = 0; i < HSIZE; i++)
    head[i] = -1;
  int i = 0;
  while (i < n) {
    int h = lz_hash(t[i]);
    int cand = head[h], chain = CHAIN_MAX;
    int bestlen = 0, bestdist = 0;
    int maxl = n - i;
    if (maxl > MAX_MATCH)
      maxl = MAX_MATCH;
    while (cand >= 0 && chain-- > 0) {
      int dist = i - cand;
      if (dist > MAX_DIST)
        break;
      int l = 0;
      while (l < maxl && t[cand + l] == t[i + l])
        l++;
      if (l > bestlen) {
        bestlen = l;
        bestdist = dist;
        if (l >= maxl)
          break;
      }
      cand = prev[cand];
    }
    if (bestlen >= MIN_MATCH) {
      cb(ctx, 1, 0, bestlen, bestdist);
      for (int k = 0; k < bestlen; k++) {
        int hh = lz_hash(t[i + k]);
        prev[i + k] = head[hh];
        head[hh] = i + k;
      }
      i += bestlen;
    } else {
      cb(ctx, 0, t[i], 0, 0);
      prev[i] = head[h];
      head[h] = i;
      i++;
    }
  }
  return 0;
}

/* Pass 1: tally symbol frequencies. */
struct freq_ctx {
  unsigned gf[GREEN_BASE + NUM_LEN_CODES];
  unsigned rf[256], bf[256], af[256];
  unsigned df[NUM_DIST_CODES];
};
static void cb_freq(void *c, int copy, unsigned px, int len, int dist)
{
  struct freq_ctx *f = c;
  if (!copy) {
    f->gf[(px >> 8) & 0xff]++;
    f->rf[(px >> 16) & 0xff]++;
    f->bf[px & 0xff]++;
    f->af[(px >> 24) & 0xff]++;
  } else {
    int code, eb, ev;
    prefix_encode(len, &code, &eb, &ev);
    f->gf[256 + code]++;
    prefix_encode(dist + 120, &code, &eb, &ev);
    f->df[code]++;
  }
}

/* Pass 2: write symbols + extra bits. */
struct write_ctx {
  struct bw *w;
  struct enc_table *green, *red, *blue, *alpha, *distt;
};
static void cb_write(void *c, int copy, unsigned px, int len, int dist)
{
  struct write_ctx *wc = c;
  if (!copy) {
    write_symbol(wc->w, wc->green, (px >> 8) & 0xff);
    write_symbol(wc->w, wc->red, (px >> 16) & 0xff);
    write_symbol(wc->w, wc->blue, px & 0xff);
    write_symbol(wc->w, wc->alpha, (px >> 24) & 0xff);
  } else {
    int code, eb, ev;
    prefix_encode(len, &code, &eb, &ev);
    write_symbol(wc->w, wc->green, 256 + code);
    bw_bits(wc->w, (unsigned)ev, eb);
    prefix_encode(dist + 120, &code, &eb, &ev);
    write_symbol(wc->w, wc->distt, code);
    bw_bits(wc->w, (unsigned)ev, eb);
  }
}

/* ====================================================================== *
 *  Encoder: top level                                                    *
 * ====================================================================== */

int webp_encode(const unsigned char *rgba, unsigned w, unsigned h, unsigned char **out, int *outlen)
{
  *out = 0;
  *outlen = 0;
  if (w == 0 || h == 0 || w > 16384 || h > 16384)
    return WEBP_EFORMAT;
  int n = (int)(w * h);

  /* Subtract-green transform over a fresh ARGB array. */
  unsigned *t = malloc((unsigned)n * 4);
  if (!t)
    return WEBP_ENOMEM;
  int alpha_used = 0;
  for (int i = 0; i < n; i++) {
    int r = rgba[i * 4 + 0], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2], a = rgba[i * 4 + 3];
    if (a != 255)
      alpha_used = 1;
    t[i] = ((unsigned)a << 24) | ((unsigned)((r - g) & 0xff) << 16) | ((unsigned)g << 8) | (unsigned)((b - g) & 0xff);
  }

  int *head = malloc((unsigned)HSIZE * sizeof *head);
  int *prev = malloc((unsigned)n * sizeof *prev);
  if (!head || !prev) {
    free(t);
    free(head);
    free(prev);
    return WEBP_ENOMEM;
  }

  struct freq_ctx fc;
  memset(&fc, 0, sizeof fc);
  lz77_run(t, n, head, prev, cb_freq, &fc);

  struct enc_table green, red, blue, alpha, distt;
  memset(&green, 0, sizeof green);
  memset(&red, 0, sizeof red);
  memset(&blue, 0, sizeof blue);
  memset(&alpha, 0, sizeof alpha);
  memset(&distt, 0, sizeof distt);
  if (enc_table_build(&green, fc.gf, GREEN_BASE + NUM_LEN_CODES) != 0 || enc_table_build(&red, fc.rf, 256) != 0 ||
      enc_table_build(&blue, fc.bf, 256) != 0 || enc_table_build(&alpha, fc.af, 256) != 0 ||
      enc_table_build(&distt, fc.df, NUM_DIST_CODES) != 0) {
    goto oom;
  }

  struct bw bw;
  bw_init(&bw);
  if (bw.oom)
    goto oom;

  bw_bits(&bw, 0x2f, 8); /* VP8L signature              */
  bw_bits(&bw, w - 1, 14);
  bw_bits(&bw, h - 1, 14);
  bw_bits(&bw, (unsigned)alpha_used, 1);
  bw_bits(&bw, 0, 3); /* version                     */

  bw_bits(&bw, 1, 1);
  bw_bits(&bw, TR_SUBGREEN, 2); /* subtract-green        */
  bw_bits(&bw, 0, 1);           /* no more transforms          */
  bw_bits(&bw, 0, 1);           /* no colour cache             */
  bw_bits(&bw, 0, 1);           /* single meta-Huffman group   */

  emit_table_header(&bw, &green);
  emit_table_header(&bw, &red);
  emit_table_header(&bw, &blue);
  emit_table_header(&bw, &alpha);
  emit_table_header(&bw, &distt);

  struct write_ctx wc = {&bw, &green, &red, &blue, &alpha, &distt};
  lz77_run(t, n, head, prev, cb_write, &wc);
  bw_flush(&bw);

  enc_table_free(&green);
  enc_table_free(&red);
  enc_table_free(&blue);
  enc_table_free(&alpha);
  enc_table_free(&distt);
  free(t);
  free(head);
  free(prev);

  if (bw.oom) {
    free(bw.buf);
    return WEBP_ENOMEM;
  }

  /* Wrap the VP8L payload in a RIFF/WEBP container. */
  int pay = bw.len, pad = pay & 1;
  int total = 12 + 8 + pay + pad;
  unsigned char *o = malloc((unsigned)total);
  if (!o) {
    free(bw.buf);
    return WEBP_ENOMEM;
  }
  memcpy(o, "RIFF", 4);
  unsigned riff = (unsigned)(total - 8);
  o[4] = riff;
  o[5] = riff >> 8;
  o[6] = riff >> 16;
  o[7] = riff >> 24;
  memcpy(o + 8, "WEBP", 4);
  memcpy(o + 12, "VP8L", 4);
  o[16] = pay;
  o[17] = pay >> 8;
  o[18] = pay >> 16;
  o[19] = (unsigned)pay >> 24;
  memcpy(o + 20, bw.buf, (unsigned)pay);
  if (pad)
    o[20 + pay] = 0;
  free(bw.buf);

  *out = o;
  *outlen = total;
  return WEBP_OK;

oom:
  enc_table_free(&green);
  enc_table_free(&red);
  enc_table_free(&blue);
  enc_table_free(&alpha);
  enc_table_free(&distt);
  free(t);
  free(head);
  free(prev);
  return WEBP_ENOMEM;
}
