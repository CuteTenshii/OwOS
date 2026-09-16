/* h264.c — decoder lifetime, the frame store / DPB, POC, reference management,
 * NAL iteration for the Annex-B and avcC front ends, and the public API.
 *
 * Baseline has no B slices, so decode order is display order: a finished
 * picture is emitted when the next picture's first slice arrives (and the last
 * is flushed at end of stream). Reference pictures are kept in a small DPB
 * managed by the sliding-window rule (drop the oldest short-term reference past
 * max_num_ref).
 */
#include "h264.h"
#include "h264_priv.h"
#include "h264_tables.h"
#include "owos.h"

/* Luma 4x4 block scan: block index -> pixel offset within the macroblock. */
const unsigned char h264_blk_x[16] = {0, 4, 0, 4, 8, 12, 8, 12, 0, 4, 0, 4, 8, 12, 8, 12};
const unsigned char h264_blk_y[16] = {0, 0, 4, 4, 0, 0, 4, 4, 8, 8, 12, 12, 8, 8, 12, 12};

int h264_chroma_qp(int qpy, int offset)
{
  int qpi = qpy + offset;
  if (qpi < 0)
    qpi = 0;
  else if (qpi > 51)
    qpi = 51;
  return qpi < 30 ? qpi : (int)h264_chroma_qp_map[qpi - 30];
}

h264_mb *h264_mb_at(struct h264_decoder *d, int mbx, int mby)
{
  if (mbx < 0 || mby < 0 || mbx >= d->mb_w || mby >= d->mb_h)
    return 0;
  return &d->mbs[mby * d->mb_w + mbx];
}

int h264_mb_avail(struct h264_decoder *d, int cur_mbaddr, int mbx, int mby)
{
  int a;
  if (mbx < 0 || mby < 0 || mbx >= d->mb_w || mby >= d->mb_h)
    return 0;
  a = mby * d->mb_w + mbx;
  if (a >= cur_mbaddr)
    return 0;
  if (d->mbs[a].slice_id != d->mbs[cur_mbaddr].slice_id)
    return 0;
  return 1;
}

/* ---- picture buffers ------------------------------------------------------
 */
static int pic_alloc(struct h264_decoder *d, h264_pic *p)
{
  unsigned ysz = (unsigned)d->ystride * (d->yh + 2 * H264_BORDER);
  unsigned csz = (unsigned)d->cstride * (d->ch + 2 * H264_BORDER);
  unsigned char *b = (unsigned char *)malloc(ysz + 2 * csz);
  if (!b)
    return H264_ENOMEM;
  memset(b, 0, ysz + 2 * csz);
  p->base = b;
  p->ystride = d->ystride;
  p->cstride = d->cstride;
  p->y = b + (unsigned)H264_BORDER * d->ystride + H264_BORDER;
  p->cb = b + ysz + (unsigned)H264_BORDER * d->cstride + H264_BORDER;
  p->cr = b + ysz + csz + (unsigned)H264_BORDER * d->cstride + H264_BORDER;
  return H264_OK;
}

static void plane_extend(unsigned char *pl, int stride, int w, int h)
{
  int x, y;
  for (y = 0; y < h; y++) {
    unsigned char *row = pl + y * stride;
    for (x = 1; x <= H264_BORDER; x++) {
      row[-x] = row[0];
      row[w - 1 + x] = row[w - 1];
    }
  }
  for (y = 1; y <= H264_BORDER; y++) {
    memcpy(pl - H264_BORDER + (-y) * stride, pl - H264_BORDER, w + 2 * H264_BORDER);
    memcpy(pl - H264_BORDER + (h - 1 + y) * stride, pl - H264_BORDER + (h - 1) * stride, w + 2 * H264_BORDER);
  }
}

static void pic_extend(struct h264_decoder *d, h264_pic *p)
{
  plane_extend(p->y, d->ystride, d->yw, d->yh);
  plane_extend(p->cb, d->cstride, d->cw, d->ch);
  plane_extend(p->cr, d->cstride, d->cw, d->ch);
}

/* Pick a free DPB slot, evicting a picture that is neither a reference nor
 * still awaiting output. Returns NULL if the DPB is (transiently) full. */
static h264_pic *dpb_get_free(struct h264_decoder *d)
{
  int i;
  for (i = 0; i < d->dpb_size; i++)
    if (!d->dpb[i].used) {
      if (!d->dpb[i].base && pic_alloc(d, &d->dpb[i]) != H264_OK)
        return 0;
      d->dpb[i].used = 1;
      d->dpb[i].is_ref = 0;
      d->dpb[i].needed_output = 0;
      return &d->dpb[i];
    }
  for (i = 0; i < d->dpb_size; i++)
    if (d->dpb[i].used && !d->dpb[i].is_ref && !d->dpb[i].needed_output && &d->dpb[i] != d->out) {
      d->dpb[i].is_ref = 0;
      return &d->dpb[i];
    }
  return 0;
}

/* Sliding-window: keep at most max_num_ref short-term references. */
static void dpb_slide(struct h264_decoder *d)
{
  int nref = 0, i, oldest = -1, oldest_num = 0;
  int maxr = d->cur_sps->max_num_ref_frames;
  if (maxr < 1)
    maxr = 1;
  for (i = 0; i < d->dpb_size; i++)
    if (d->dpb[i].used && d->dpb[i].is_ref)
      nref++;
  while (nref > maxr) {
    oldest = -1;
    for (i = 0; i < d->dpb_size; i++) {
      if (!d->dpb[i].used || !d->dpb[i].is_ref)
        continue;
      if (oldest < 0 || d->dpb[i].frame_num < oldest_num) {
        oldest = i;
        oldest_num = d->dpb[i].frame_num;
      }
    }
    if (oldest < 0)
      break;
    d->dpb[oldest].is_ref = 0;
    if (!d->dpb[oldest].needed_output && &d->dpb[oldest] != d->out)
      d->dpb[oldest].used = 0;
    nref--;
  }
}

static void dpb_clear_refs(struct h264_decoder *d)
{
  int i;
  for (i = 0; i < d->dpb_size; i++) {
    d->dpb[i].is_ref = 0;
    if (d->dpb[i].used && !d->dpb[i].needed_output && &d->dpb[i] != d->out)
      d->dpb[i].used = 0;
  }
}

/* Build reference list 0 for a P slice: short-term refs by descending FrameNum.
 */
void h264_build_ref_list0(struct h264_decoder *d)
{
  int i, j, max_fn = 1 << d->cur_sps->log2_max_frame_num;
  d->nref0 = 0;
  for (i = 0; i < d->dpb_size; i++) {
    if (!d->dpb[i].used || !d->dpb[i].is_ref)
      continue;
    if (d->nref0 < 32)
      d->ref0[d->nref0++] = &d->dpb[i];
  }
  /* Sort by descending FrameNumWrap (PicNum) relative to the current frame. */
  for (i = 0; i < d->nref0; i++)
    for (j = i + 1; j < d->nref0; j++) {
      int a = d->ref0[i]->frame_num, b = d->ref0[j]->frame_num;
      if (a > d->cur_frame_num)
        a -= max_fn;
      if (b > d->cur_frame_num)
        b -= max_fn;
      if (b > a) {
        h264_pic *t = d->ref0[i];
        d->ref0[i] = d->ref0[j];
        d->ref0[j] = t;
      }
    }
}

/* ---- POC (type 0; types 1/2 approximated — output is decode order anyway) ---
 */
static void compute_poc(struct h264_decoder *d, const h264_slice *sh)
{
  h264_sps *s = d->cur_sps;
  if (s->poc_type == 0) {
    int max_lsb = 1 << s->log2_max_poc_lsb, msb;
    int prev_msb = d->prev_poc_msb, prev_lsb = d->prev_poc_lsb;
    if (d->cur_is_idr) {
      prev_msb = 0;
      prev_lsb = 0;
    }
    if (sh->poc_lsb < prev_lsb && prev_lsb - sh->poc_lsb >= max_lsb / 2)
      msb = prev_msb + max_lsb;
    else if (sh->poc_lsb > prev_lsb && sh->poc_lsb - prev_lsb > max_lsb / 2)
      msb = prev_msb - max_lsb;
    else
      msb = prev_msb;
    d->cur_poc = msb + sh->poc_lsb;
    if (sh->nal_ref_idc) {
      d->prev_poc_msb = msb;
      d->prev_poc_lsb = sh->poc_lsb;
    }
  } else {
    d->cur_poc = 2 * d->cur_frame_num; /* monotone in decode order for I/P */
  }
}

/* ---- NAL iteration (Annex-B) ----------------------------------------------
 */
static int find_start(const unsigned char *b, unsigned len, unsigned from, unsigned *sc_len)
{
  unsigned i;
  for (i = from; i + 3 <= len; i++) {
    if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1) {
      *sc_len = (i > 0 && b[i - 1] == 0) ? 4 : 3;
      return (int)i;
    }
  }
  return -1;
}

static int next_nal(struct h264_decoder *d, const unsigned char **nal, unsigned *nlen)
{
  unsigned scl;
  int s = find_start(d->buf, d->len, d->scan, &scl);
  int e;
  unsigned start, end;
  if (s < 0)
    return 0;
  start = (unsigned)s + 3;
  e = find_start(d->buf, d->len, start, &scl);
  end = (e < 0) ? d->len : (unsigned)e - (scl - 3); /* exclude the next start's leading zeros */
  while (end > start && d->buf[end - 1] == 0)
    end--; /* trim trailing zero bytes */
  d->scan = (e < 0) ? d->len : (unsigned)e;
  *nal = d->buf + start;
  *nlen = end - start;
  return 1;
}

/* ---- picture assembly -----------------------------------------------------
 */
/* Finalise the current picture: deblock, edge-extend, run the sliding window,
 * and mark it ready for output. */
static void finish_picture(struct h264_decoder *d)
{
  if (!d->have_pic)
    return;
  h264_deblock_picture(d);
  pic_extend(d, d->cur);
  /* The previous output has been consumed by the caller; release its slot. */
  if (d->out && d->out != d->cur) {
    d->out->needed_output = 0;
    if (!d->out->is_ref)
      d->out->used = 0;
  }
  /* Only now does the picture become a reference (never during its own decode).
   */
  d->cur->is_ref = d->cur_is_ref;
  if (d->cur->is_ref)
    dpb_slide(d);
  d->out = d->cur;
  d->out_type = d->cur->slice_type;
  d->cur->needed_output = 1;
  d->have_pic = 0;
}

/* h264.c-internal: called by slice.c when a slice with first_mb==0 opens a new
 * picture. Allocates cur and resets the per-picture MB info. Returns 0/err. */
int h264_start_picture(struct h264_decoder *d, const h264_slice *sh, int nal_ref_idc)
{
  h264_pic *p;
  d->cur_frame_num = sh->frame_num;
  d->cur_is_idr = sh->idr;
  d->cur_slice_type = sh->slice_type;
  if (sh->idr)
    dpb_clear_refs(d);
  compute_poc(d, sh);
  p = dpb_get_free(d);
  if (!p)
    return H264_ENOMEM;
  p->frame_num = sh->frame_num;
  p->poc = d->cur_poc;
  p->slice_type = sh->slice_type;
  p->is_ref = 0; /* not a reference until fully decoded */
  d->cur_is_ref = nal_ref_idc != 0;
  p->needed_output = 0;
  d->cur = p;
  d->have_pic = 1;
  memset(d->mbs, 0, (unsigned)d->mb_w * d->mb_h * sizeof(h264_mb));
  {
    int i;
    for (i = 0; i < d->mb_w * d->mb_h; i++)
      d->mbs[i].slice_id = -1;
  }
  return H264_OK;
}

/* Set the coded/cropped geometry once the active SPS is known. */
static int setup_geometry(struct h264_decoder *d, h264_sps *s)
{
  if (d->mbs && (d->mb_w != s->mb_w || d->mb_h != s->mb_h))
    return H264_EUNSUP;
  d->mb_w = s->mb_w;
  d->mb_h = s->mb_h;
  d->yw = d->mb_w * 16;
  d->yh = d->mb_h * 16;
  d->cw = d->mb_w * 8;
  d->ch = d->mb_h * 8;
  d->ystride = d->yw + 2 * H264_BORDER;
  d->cstride = d->cw + 2 * H264_BORDER;
  d->width = d->yw - s->crop_l - s->crop_r;
  d->height = d->yh - s->crop_t - s->crop_b;
  if (!d->mbs) {
    int i;
    d->mbs = (h264_mb *)malloc((unsigned)d->mb_w * d->mb_h * sizeof(h264_mb));
    if (!d->mbs)
      return H264_ENOMEM;
    d->dpb_size = s->max_num_ref_frames + 4;
    if (d->dpb_size < 6)
      d->dpb_size = 6;
    if (d->dpb_size > 34)
      d->dpb_size = 34;
    d->dpb = (h264_pic *)calloc(d->dpb_size, sizeof(h264_pic));
    if (!d->dpb)
      return H264_ENOMEM;
    for (i = 0; i < d->dpb_size; i++)
      d->dpb[i].used = 0;
  }
  return H264_OK;
}

/* Route one NAL. Returns 1 if a finished picture became ready (in d->out), 0 if
 * not, or a negative H264_E* on error. */
static int process_nal(struct h264_decoder *d, const unsigned char *nal, unsigned nlen)
{
  int type, ref_idc, rc;
  unsigned rlen;
  if (nlen < 1)
    return 0;
  type = nal[0] & 0x1f;
  ref_idc = (nal[0] >> 5) & 3;
  rlen = h264_rbsp(d, nal + 1, nlen - 1);
  if (rlen == 0 && nlen > 1)
    return H264_ENOMEM;

  if (type == 7) {
    rc = h264_parse_sps(d, d->rbsp, rlen);
    return rc == H264_OK ? 0 : rc;
  }
  if (type == 8) {
    rc = h264_parse_pps(d, d->rbsp, rlen);
    return rc == H264_OK ? 0 : rc;
  }
  if (type == 1 || type == 5) {
    int emitted = 0;
    rc = h264_decode_slice_dispatch(d, d->rbsp, rlen, type, ref_idc, &emitted);
    if (rc != H264_OK)
      return rc;
    return emitted;
  }
  return 0; /* SEI, AUD, filler, etc. */
}

int h264_decode_frame(h264_decoder *d)
{
  const unsigned char *nal;
  unsigned nlen;
  if (!d)
    return H264_EFORMAT;
  if (d->eos)
    return H264_EEOF;
  for (;;) {
    if (!next_nal(d, &nal, &nlen)) {
      d->eos = 1;
      if (d->have_pic) {
        finish_picture(d);
        return H264_OK;
      }
      return H264_EEOF;
    }
    int r = process_nal(d, nal, nlen);
    if (r < 0)
      return r;
    if (r == 1)
      return H264_OK;
  }
}

/* ---- setup helpers used by slice.c ----------------------------------------
 */
int h264_ensure_geometry(struct h264_decoder *d, h264_pps *pps)
{
  h264_sps *s = &d->sps[pps->sps_id];
  if (!s->valid)
    return H264_EFORMAT;
  d->cur_sps = s;
  d->cur_pps = pps;
  return setup_geometry(d, s);
}

/* slice.c calls this before decoding a picture's first slice's data, to close
 * any open picture and emit it. Returns 1 if a picture was emitted. */
int h264_close_open_picture(struct h264_decoder *d)
{
  if (!d->have_pic)
    return 0;
  finish_picture(d);
  return 1;
}

/* ---- public API -----------------------------------------------------------
 */
static int find_first_params(struct h264_decoder *d)
{
  const unsigned char *nal;
  unsigned nlen;
  unsigned save = d->scan;
  int got_sps = 0, got_pps = 0;
  while (next_nal(d, &nal, &nlen)) {
    int type = nal[0] & 0x1f;
    if (type == 7) {
      unsigned r = h264_rbsp(d, nal + 1, nlen - 1);
      if (h264_parse_sps(d, d->rbsp, r) == H264_OK)
        got_sps = 1;
    } else if (type == 8) {
      unsigned r = h264_rbsp(d, nal + 1, nlen - 1);
      if (h264_parse_pps(d, d->rbsp, r) == H264_OK)
        got_pps = 1;
    }
    if (got_sps && got_pps)
      break;
  }
  d->scan = save;
  if (!got_sps)
    return H264_EBADSIG;
  return H264_OK;
}

static int finalize_open(struct h264_decoder *d)
{
  int i, rc;
  /* Bind the first valid SPS/PPS so width/height are known. */
  for (i = 0; i < 256; i++)
    if (d->pps[i].valid) {
      rc = h264_ensure_geometry(d, &d->pps[i]);
      if (rc == H264_OK)
        return H264_OK;
    }
  for (i = 0; i < 32; i++)
    if (d->sps[i].valid) {
      d->cur_sps = &d->sps[i];
      return setup_geometry(d, &d->sps[i]);
    }
  return H264_EBADSIG;
}

int h264_decoder_open(const unsigned char *buf, unsigned len, h264_decoder **out)
{
  struct h264_decoder *d;
  int rc;
  if (out)
    *out = 0;
  if (!buf || len < 5 || !out)
    return H264_EBADSIG;
  d = (struct h264_decoder *)calloc(1, sizeof *d);
  if (!d)
    return H264_ENOMEM;
  d->buf = buf;
  d->len = len;
  d->scan = 0;
  d->annexb = 1;
  rc = find_first_params(d);
  if (rc != H264_OK) {
    h264_decoder_close(d);
    return rc;
  }
  rc = finalize_open(d);
  if (rc != H264_OK) {
    h264_decoder_close(d);
    return rc;
  }
  *out = d;
  return H264_OK;
}

static int parse_avcc(struct h264_decoder *d, const unsigned char *a, unsigned len)
{
  unsigned p, i, n;
  if (len < 7 || a[0] != 1)
    return H264_EBADSIG;
  d->nal_length_size = (a[4] & 3) + 1;
  n = a[5] & 0x1f;
  p = 6;
  for (i = 0; i < n; i++) {
    unsigned l;
    if (p + 2 > len)
      return H264_EFORMAT;
    l = (a[p] << 8) | a[p + 1];
    p += 2;
    if (p + l > len || l < 1)
      return H264_EFORMAT;
    {
      unsigned r = h264_rbsp(d, a + p + 1, l - 1);
      if (h264_parse_sps(d, d->rbsp, r) != H264_OK) {
      }
    }
    p += l;
  }
  if (p >= len)
    return H264_EFORMAT;
  n = a[p++];
  for (i = 0; i < n; i++) {
    unsigned l;
    if (p + 2 > len)
      return H264_EFORMAT;
    l = (a[p] << 8) | a[p + 1];
    p += 2;
    if (p + l > len || l < 1)
      return H264_EFORMAT;
    {
      unsigned r = h264_rbsp(d, a + p + 1, l - 1);
      if (h264_parse_pps(d, d->rbsp, r) != H264_OK) {
      }
    }
    p += l;
  }
  return H264_OK;
}

int h264_decoder_open_avcc(const unsigned char *avcc, unsigned len, h264_decoder **out)
{
  struct h264_decoder *d;
  int rc;
  if (out)
    *out = 0;
  if (!avcc || !out)
    return H264_EBADSIG;
  d = (struct h264_decoder *)calloc(1, sizeof *d);
  if (!d)
    return H264_ENOMEM;
  d->annexb = 0;
  rc = parse_avcc(d, avcc, len);
  if (rc != H264_OK) {
    h264_decoder_close(d);
    return rc;
  }
  rc = finalize_open(d);
  if (rc != H264_OK) {
    h264_decoder_close(d);
    return rc;
  }
  *out = d;
  return H264_OK;
}

int h264_decode_au(h264_decoder *d, const unsigned char *au, unsigned len, int nal_length_size)
{
  unsigned p = 0;
  int emitted = 0, nls = nal_length_size > 0 ? nal_length_size : d->nal_length_size;
  if (!d || !au || nls < 1 || nls > 4)
    return H264_EFORMAT;
  while (p + (unsigned)nls <= len) {
    unsigned l = 0, i;
    int r;
    for (i = 0; i < (unsigned)nls; i++)
      l = (l << 8) | au[p + i];
    p += nls;
    if (l == 0 || p + l > len)
      break;
    r = process_nal(d, au + p, l);
    if (r < 0)
      return r;
    if (r == 1)
      emitted = 1;
    p += l;
  }
  return emitted ? H264_OK : H264_EAGAIN;
}

int h264_flush(h264_decoder *d)
{
  if (!d)
    return H264_EEOF;
  if (d->have_pic) {
    finish_picture(d);
    return H264_OK;
  }
  return H264_EEOF;
}

void h264_decoder_close(h264_decoder *d)
{
  int i;
  if (!d)
    return;
  if (d->dpb) {
    for (i = 0; i < d->dpb_size; i++)
      free(d->dpb[i].base);
    free(d->dpb);
  }
  free(d->mbs);
  free(d->rbsp);
  free(d);
}

int h264_width(const h264_decoder *d) { return d ? d->width : 0; }
int h264_height(const h264_decoder *d) { return d ? d->height : 0; }
int h264_frame_type(const h264_decoder *d) { return d ? d->out_type : 0; }

const unsigned char *h264_plane_y(const h264_decoder *d, int *stride)
{
  if (stride)
    *stride = d ? d->ystride : 0;
  if (!d || !d->out)
    return 0;
  return d->out->y + d->cur_sps->crop_t * d->ystride + d->cur_sps->crop_l;
}
const unsigned char *h264_plane_cb(const h264_decoder *d, int *stride)
{
  if (stride)
    *stride = d ? d->cstride : 0;
  if (!d || !d->out)
    return 0;
  return d->out->cb + (d->cur_sps->crop_t / 2) * d->cstride + d->cur_sps->crop_l / 2;
}
const unsigned char *h264_plane_cr(const h264_decoder *d, int *stride)
{
  if (stride)
    *stride = d ? d->cstride : 0;
  if (!d || !d->out)
    return 0;
  return d->out->cr + (d->cur_sps->crop_t / 2) * d->cstride + d->cur_sps->crop_l / 2;
}

int h264_frame_rgb(const h264_decoder *d, unsigned char *rgb)
{
  int x, y, ys, cs;
  const unsigned char *Y = h264_plane_y(d, &ys);
  const unsigned char *Cb = h264_plane_cb(d, &cs);
  const unsigned char *Cr = h264_plane_cr(d, &cs);
  if (!d || !rgb || !Y)
    return H264_EFORMAT;
  for (y = 0; y < d->height; y++)
    for (x = 0; x < d->width; x++) {
      int lu = Y[y * ys + x];
      int U = Cb[(y / 2) * cs + (x / 2)] - 128;
      int V = Cr[(y / 2) * cs + (x / 2)] - 128;
      int r = lu + ((91881 * V) >> 16);
      int g = lu - ((22554 * U + 46802 * V) >> 16);
      int b = lu + ((116130 * U) >> 16);
      unsigned char *o = rgb + (y * d->width + x) * 3;
      o[0] = r < 0 ? 0 : r > 255 ? 255 : r;
      o[1] = g < 0 ? 0 : g > 255 ? 255 : g;
      o[2] = b < 0 ? 0 : b > 255 ? 255 : b;
    }
  return H264_OK;
}
