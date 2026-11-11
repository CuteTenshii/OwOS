/* h264.c — decoder lifetime, the frame store / DPB, POC, reference marking and
 * lists, output ordering, NAL iteration for the Annex-B and avcC front ends, and
 * the public API.
 *
 * Decoded pictures wait in the DPB and are released in POC order once the
 * reorder window (VUI num_reorder_frames, else the level's DPB size) is full,
 * which is the C.4.5 bumping process. Reference pictures follow the sliding
 * window or the explicit memory management operations, short- and long-term.
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
  p->mot = (h264_mot *)malloc((unsigned)d->mb_w * d->mb_h * sizeof(h264_mot));
  if (!p->mot) {
    free(b);
    return H264_ENOMEM;
  }
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

/* A slot is free once nothing references it: not a reference, not waiting for
 * output, and not the picture the caller is reading or the one being decoded. */
static void slot_release(struct h264_decoder *d, h264_pic *p)
{
  if (p->used && p->ref == 0 && p->out == 0 && p != d->out && p != d->cur)
    p->used = 0;
}

static h264_pic *dpb_get_free(struct h264_decoder *d)
{
  int i;
  for (i = 0; i < d->dpb_size; i++)
    if (!d->dpb[i].used) {
      h264_pic *p = &d->dpb[i];
      if (!p->base && pic_alloc(d, p) != H264_OK)
        return 0;
      p->used = 1;
      p->ref = 0;
      p->out = 0;
      p->non_existing = 0;
      return p;
    }
  return 0;
}

static int max_frame_num(struct h264_decoder *d) { return 1 << d->cur_sps->log2_max_frame_num; }

static int frame_num_wrap(struct h264_decoder *d, const h264_pic *p)
{
  return p->frame_num > d->cur_frame_num ? p->frame_num - max_frame_num(d) : p->frame_num;
}

/* ---- output ordering ------------------------------------------------------
 */
static int waiting_count(struct h264_decoder *d)
{
  int i, n = 0;
  for (i = 0; i < d->dpb_size; i++)
    n += d->dpb[i].used && d->dpb[i].out == 1;
  return n;
}

/* Move the waiting picture with the smallest POC to the caller's queue. */
static int bump_one(struct h264_decoder *d)
{
  h264_pic *best = 0;
  int i;
  for (i = 0; i < d->dpb_size; i++) {
    h264_pic *p = &d->dpb[i];
    if (p->used && p->out == 1 && (!best || p->poc < best->poc))
      best = p;
  }
  if (!best || d->outq_n >= (int)(sizeof d->outq / sizeof d->outq[0]))
    return 0;
  best->out = 2;
  d->outq[d->outq_n++] = best;
  return 1;
}

static void flush_output(struct h264_decoder *d)
{
  while (bump_one(d)) {
  }
}

static int occupied(struct h264_decoder *d)
{
  int i, n = 0;
  for (i = 0; i < d->dpb_size; i++)
    n += d->dpb[i].used && (d->dpb[i].ref || d->dpb[i].out == 1);
  return n;
}

static h264_pic *pop_output(struct h264_decoder *d)
{
  h264_pic *p;
  int i;
  if (d->outq_n == 0)
    return 0;
  p = d->outq[0];
  for (i = 1; i < d->outq_n; i++)
    d->outq[i - 1] = d->outq[i];
  d->outq_n--;
  p->out = 0;
  d->out = p;
  d->out_type = p->slice_type;
  return p;
}

static void release_out(struct h264_decoder *d)
{
  h264_pic *p = d->out;
  d->out = 0;
  if (p)
    slot_release(d, p);
}

/* ---- reference marking (8.2.5) --------------------------------------------
 */
static void unmark(struct h264_decoder *d, h264_pic *p)
{
  p->ref = 0;
  slot_release(d, p);
}

static void sliding_window(struct h264_decoder *d)
{
  int maxr = d->cur_sps->max_num_ref_frames < 1 ? 1 : d->cur_sps->max_num_ref_frames;
  for (;;) {
    int i, nshort = 0, nlong = 0, oldest = -1, oldest_w = 0;
    for (i = 0; i < d->dpb_size; i++) {
      h264_pic *p = &d->dpb[i];
      if (!p->used || p == d->cur)
        continue;
      if (p->ref == 1) {
        int w = frame_num_wrap(d, p);
        nshort++;
        if (oldest < 0 || w < oldest_w) {
          oldest = i;
          oldest_w = w;
        }
      } else if (p->ref == 2) {
        nlong++;
      }
    }
    if (nshort + nlong < maxr || oldest < 0)
      return;
    unmark(d, &d->dpb[oldest]);
  }
}

static h264_pic *find_short(struct h264_decoder *d, int picnum)
{
  int i;
  for (i = 0; i < d->dpb_size; i++)
    if (d->dpb[i].used && d->dpb[i].ref == 1 && d->dpb[i].pic_id != d->cur->pic_id &&
        frame_num_wrap(d, &d->dpb[i]) == picnum)
      return &d->dpb[i];
  return 0;
}

static void free_long_idx(struct h264_decoder *d, int idx, const h264_pic *keep)
{
  int i;
  for (i = 0; i < d->dpb_size; i++)
    if (d->dpb[i].used && d->dpb[i].ref == 2 && d->dpb[i].long_idx == idx && &d->dpb[i] != keep)
      unmark(d, &d->dpb[i]);
}

static void run_mmco(struct h264_decoder *d)
{
  const h264_slice *sh = &d->pic_sh;
  h264_pic *c = d->cur, *p;
  int k, i;
  for (k = 0; k < sh->mmco_n; k++) {
    int a = sh->mmco_a[k], b = sh->mmco_b[k];
    switch (sh->mmco_op[k]) {
    case 1:
      p = find_short(d, d->cur_frame_num - (a + 1));
      if (p)
        unmark(d, p);
      break;
    case 2:
      for (i = 0; i < d->dpb_size; i++)
        if (d->dpb[i].used && d->dpb[i].ref == 2 && d->dpb[i].long_idx == a)
          unmark(d, &d->dpb[i]);
      break;
    case 3:
      p = find_short(d, d->cur_frame_num - (a + 1));
      if (p) {
        free_long_idx(d, b, p);
        p->ref = 2;
        p->long_idx = b;
      }
      break;
    case 4:
      d->max_long_idx = a - 1;
      for (i = 0; i < d->dpb_size; i++)
        if (d->dpb[i].used && d->dpb[i].ref == 2 && d->dpb[i].long_idx > d->max_long_idx)
          unmark(d, &d->dpb[i]);
      break;
    case 5:
      for (i = 0; i < d->dpb_size; i++)
        if (d->dpb[i].used && &d->dpb[i] != c && d->dpb[i].ref)
          unmark(d, &d->dpb[i]);
      d->max_long_idx = -1;
      d->mmco5 = 1;
      break;
    case 6:
      free_long_idx(d, a, c);
      c->ref = 2;
      c->long_idx = a;
      break;
    default:
      break;
    }
  }
}

static void mark_current(struct h264_decoder *d)
{
  h264_pic *c = d->cur;
  const h264_slice *sh = &d->pic_sh;
  int i;
  d->mmco5 = 0;
  if (!d->cur_is_ref) {
    c->ref = 0;
    return;
  }
  if (sh->idr) {
    for (i = 0; i < d->dpb_size; i++)
      if (d->dpb[i].used && &d->dpb[i] != c)
        unmark(d, &d->dpb[i]);
    if (sh->long_term_ref_flag) {
      c->ref = 2;
      c->long_idx = 0;
      d->max_long_idx = 0;
    } else {
      c->ref = 1;
      d->max_long_idx = -1;
    }
    return;
  }
  c->ref = 0;
  if (sh->adaptive_marking)
    run_mmco(d);
  else
    sliding_window(d);
  if (c->ref == 0)
    c->ref = 1;
}

/* ---- POC (8.2.1) ----------------------------------------------------------
 */
static void compute_poc(struct h264_decoder *d, const h264_slice *sh)
{
  h264_sps *s = d->cur_sps;
  int maxfn = max_frame_num(d), top, bot;

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
    d->cur_poc_msb = msb;
    top = msb + sh->poc_lsb;
    bot = top + sh->delta_poc_bottom;
  } else {
    int off = d->cur_is_idr
                  ? 0
                  : (d->prev_frame_num > sh->frame_num ? d->prev_frame_num_offset + maxfn : d->prev_frame_num_offset);
    d->cur_fn_offset = off;
    if (s->poc_type == 1) {
      int n = s->num_ref_frames_in_poc_cycle, abs_fn = n ? off + sh->frame_num : 0, expected = 0;
      if (!sh->nal_ref_idc && abs_fn > 0)
        abs_fn--;
      if (abs_fn > 0) {
        int cycle = (abs_fn - 1) / n, in_cycle = (abs_fn - 1) % n, delta = 0, i;
        for (i = 0; i < n; i++)
          delta += s->offset_for_ref_frame[i];
        expected = cycle * delta;
        for (i = 0; i <= in_cycle; i++)
          expected += s->offset_for_ref_frame[i];
      }
      if (!sh->nal_ref_idc)
        expected += s->offset_for_non_ref_pic;
      top = expected + sh->delta_poc[0];
      bot = top + s->offset_for_top_to_bottom + sh->delta_poc[1];
    } else {
      int t = d->cur_is_idr ? 0 : 2 * (off + sh->frame_num) - (sh->nal_ref_idc ? 0 : 1);
      top = bot = t;
    }
  }
  d->cur_top_poc = top;
  d->cur_bot_poc = bot;
  d->cur_poc = top < bot ? top : bot;
}

/* ---- picture assembly -----------------------------------------------------
 */
/* Finalise the current picture: deblock, edge-extend, mark references, update
 * the POC/frame_num history, and place it in the output order. */
static void finish_picture(struct h264_decoder *d)
{
  h264_pic *c = d->cur;
  int cap_need;
  if (!d->have_pic)
    return;
  h264_deblock_picture(d);
  pic_extend(d, c);

  mark_current(d);
  if (d->mmco5) {
    int temp = d->cur_top_poc < d->cur_bot_poc ? d->cur_top_poc : d->cur_bot_poc;
    c->frame_num = 0;
    d->cur_top_poc -= temp;
    d->cur_bot_poc -= temp;
    c->poc = d->cur_top_poc < d->cur_bot_poc ? d->cur_top_poc : d->cur_bot_poc;
  }
  if (d->cur_sps->poc_type == 0) {
    if (d->cur_is_ref) {
      d->prev_poc_msb = d->mmco5 ? 0 : d->cur_poc_msb;
      d->prev_poc_lsb = d->mmco5 ? d->cur_top_poc : d->pic_sh.poc_lsb;
    }
  }
  d->prev_frame_num = d->mmco5 ? 0 : d->cur_frame_num;
  d->prev_frame_num_offset = d->mmco5 ? 0 : d->cur_fn_offset;
  if (d->cur_is_ref)
    d->prev_ref_frame_num = d->mmco5 ? 0 : d->cur_frame_num;
  d->seq_started = 1;

  if (d->cur_is_idr || d->mmco5)
    flush_output(d);
  c->out = 1;
  d->have_pic = 0;
  d->cur = 0;

  cap_need = d->cur_sps->num_reorder_frames;
  while (cap_need >= 0 && waiting_count(d) > cap_need)
    if (!bump_one(d))
      break;
  while (occupied(d) > d->dpb_cap)
    if (!bump_one(d))
      break;
}

static void mot_reset(h264_pic *p, int n)
{
  int i;
  memset(p->mot, 0, (unsigned)n * sizeof(h264_mot));
  for (i = 0; i < n; i++)
    memset(p->mot[i].refidx, -1, sizeof p->mot[i].refidx);
}

/* Insert a "non-existing" frame for each skipped frame_num (8.2.5.2) so the
 * sliding window and later reference lists behave as the encoder expects. */
static int fill_frame_num_gap(struct h264_decoder *d, int frame_num)
{
  int maxfn = max_frame_num(d), unused = (d->prev_ref_frame_num + 1) % maxfn, guard = 0;
  h264_pic *last = 0;
  int i;
  while (unused != frame_num && guard++ < maxfn) {
    h264_pic *p;
    last = 0;
    for (i = 0; i < d->dpb_size; i++)
      if (d->dpb[i].used && d->dpb[i].ref && !d->dpb[i].non_existing && (!last || d->dpb[i].pic_id > last->pic_id))
        last = &d->dpb[i];
    p = dpb_get_free(d);
    if (!p)
      return H264_ENOMEM;
    p->frame_num = unused;
    p->pic_id = ++d->pic_counter;
    p->non_existing = 1;
    p->poc = last ? last->poc : 0;
    p->slice_type = H264_SLICE_P;
    if (last) {
      memcpy(p->base, last->base,
             (unsigned)d->ystride * (d->yh + 2 * H264_BORDER) + 2 * (unsigned)d->cstride * (d->ch + 2 * H264_BORDER));
      memcpy(p->mot, last->mot, (unsigned)d->mb_w * d->mb_h * sizeof(h264_mot));
    } else {
      mot_reset(p, d->mb_w * d->mb_h);
    }
    d->cur_frame_num = unused;
    d->cur = p;
    sliding_window(d);
    d->cur = 0;
    p->ref = 1;
    d->prev_ref_frame_num = unused;
    d->prev_frame_num = unused;
    unused = (unused + 1) % maxfn;
  }
  return H264_OK;
}

/* h264.c-internal: called by slice.c when a slice with first_mb==0 opens a new
 * picture. Allocates cur and resets the per-picture MB info. Returns 0/err. */
int h264_start_picture(struct h264_decoder *d, const h264_slice *sh, int nal_ref_idc)
{
  h264_pic *p;
  int i, n;
  if (sh->idr)
    d->seq_started = 0;
  if (d->seq_started && !sh->idr && sh->frame_num != d->prev_ref_frame_num &&
      sh->frame_num != (d->prev_ref_frame_num + 1) % max_frame_num(d)) {
    int rc = fill_frame_num_gap(d, sh->frame_num);
    if (rc != H264_OK)
      return rc;
  }
  d->cur_frame_num = sh->frame_num;
  d->cur_is_idr = sh->idr;
  d->cur_slice_type = sh->slice_type;
  d->cur_is_ref = nal_ref_idc != 0;
  d->pic_sh = *sh;
  compute_poc(d, sh);
  p = dpb_get_free(d);
  if (!p)
    return H264_ENOMEM;
  p->frame_num = sh->frame_num;
  p->poc = d->cur_poc;
  p->pic_id = ++d->pic_counter;
  p->slice_type = sh->slice_type;
  d->cur = p;
  d->have_pic = 1;
  n = d->mb_w * d->mb_h;
  memset(d->mbs, 0, (unsigned)n * sizeof(h264_mb));
  for (i = 0; i < n; i++)
    d->mbs[i].slice_id = -1;
  mot_reset(p, n);
  return H264_OK;
}

/* ---- reference picture lists (8.2.4) --------------------------------------
 */
static void sort_pics(h264_pic **v, int n, int *key, int descending)
{
  int i, j;
  for (i = 1; i < n; i++) {
    h264_pic *t = v[i];
    int kt = key[i];
    j = i - 1;
    while (j >= 0 && (descending ? key[j] < kt : key[j] > kt)) {
      v[j + 1] = v[j];
      key[j + 1] = key[j];
      j--;
    }
    v[j + 1] = t;
    key[j + 1] = kt;
  }
}

static int init_list(struct h264_decoder *d, int list, h264_pic **out)
{
  h264_pic *sv[H264_MAX_REFS + 2], *lv[H264_MAX_REFS + 2], *lo[H264_MAX_REFS + 2], *hi[H264_MAX_REFS + 2];
  int sk[H264_MAX_REFS + 2], lk[H264_MAX_REFS + 2], lok[H264_MAX_REFS + 2], hik[H264_MAX_REFS + 2];
  int ns = 0, nl = 0, nlo = 0, nhi = 0, i, n = 0;
  int b = d->sh.slice_type == H264_SLICE_B;
  for (i = 0; i < d->dpb_size; i++) {
    h264_pic *p = &d->dpb[i];
    if (!p->used || p == d->cur || !p->ref)
      continue;
    if (p->ref == 2) {
      lv[nl] = p;
      lk[nl++] = p->long_idx;
    } else if (!b) {
      sv[ns] = p;
      sk[ns++] = frame_num_wrap(d, p);
    } else if (p->poc <= d->cur_poc) {
      lo[nlo] = p;
      lok[nlo++] = p->poc;
    } else {
      hi[nhi] = p;
      hik[nhi++] = p->poc;
    }
  }
  sort_pics(lv, nl, lk, 0);
  if (!b) {
    sort_pics(sv, ns, sk, 1);
    for (i = 0; i < ns; i++)
      out[n++] = sv[i];
  } else {
    sort_pics(lo, nlo, lok, 1);
    sort_pics(hi, nhi, hik, 0);
    if (list == 0) {
      for (i = 0; i < nlo; i++)
        out[n++] = lo[i];
      for (i = 0; i < nhi; i++)
        out[n++] = hi[i];
    } else {
      for (i = 0; i < nhi; i++)
        out[n++] = hi[i];
      for (i = 0; i < nlo; i++)
        out[n++] = lo[i];
    }
  }
  for (i = 0; i < nl; i++)
    out[n++] = lv[i];
  return n;
}

static void modify_list(struct h264_decoder *d, int list, int active)
{
  const h264_slice *sh = &d->sh;
  h264_pic **rl = d->reflist[list];
  int maxfn = max_frame_num(d), pred = d->cur_frame_num, idx = 0, k, i;
  for (k = 0; k < sh->rm_n[list] && idx < active; k++) {
    int idc = sh->rm_idc[list][k], val = sh->rm_val[list][k];
    h264_pic *pic = 0;
    if (idc < 2) {
      int no_wrap, picnum;
      if (idc == 0) {
        no_wrap = pred - (val + 1);
        if (no_wrap < 0)
          no_wrap += maxfn;
      } else {
        no_wrap = pred + (val + 1);
        if (no_wrap >= maxfn)
          no_wrap -= maxfn;
      }
      pred = no_wrap;
      picnum = no_wrap > d->cur_frame_num ? no_wrap - maxfn : no_wrap;
      for (i = 0; i < d->dpb_size; i++)
        if (d->dpb[i].used && d->dpb[i].ref == 1 && &d->dpb[i] != d->cur && frame_num_wrap(d, &d->dpb[i]) == picnum) {
          pic = &d->dpb[i];
          break;
        }
    } else if (idc == 2) {
      for (i = 0; i < d->dpb_size; i++)
        if (d->dpb[i].used && d->dpb[i].ref == 2 && d->dpb[i].long_idx == val) {
          pic = &d->dpb[i];
          break;
        }
    }
    if (!pic)
      continue;
    for (i = active; i > idx; i--)
      rl[i] = rl[i - 1];
    rl[idx++] = pic;
    {
      int n = idx;
      for (i = idx; i <= active; i++)
        if (rl[i] != pic)
          rl[n++] = rl[i];
    }
  }
}

int h264_build_ref_lists(struct h264_decoder *d)
{
  int nlists = d->sh.slice_type == H264_SLICE_B ? 2 : d->sh.slice_type == H264_SLICE_I ? 0 : 1;
  int list, i, full[2] = {0, 0};
  h264_pic *tmp[2][H264_MAX_REFS + 2];

  d->nref[0] = d->nref[1] = 0;
  for (list = 0; list < nlists; list++)
    full[list] = init_list(d, list, tmp[list]);
  if (nlists == 2 && full[1] > 1 && full[0] == full[1]) {
    for (i = 0; i < full[0] && tmp[0][i] == tmp[1][i]; i++) {
    }
    if (i == full[0]) {
      h264_pic *t = tmp[1][0];
      tmp[1][0] = tmp[1][1];
      tmp[1][1] = t;
    }
  }
  for (list = 0; list < nlists; list++) {
    int active = d->sh.num_ref_idx[list];
    h264_pic *fallback = 0;
    memset(d->reflist[list], 0, sizeof d->reflist[list]);
    for (i = 0; i < full[list] && i < active; i++)
      d->reflist[list][i] = tmp[list][i];
    modify_list(d, list, active);
    for (i = 0; i < active; i++)
      if (d->reflist[list][i] && !fallback)
        fallback = d->reflist[list][i];
    if (!fallback)
      return H264_EFORMAT;
    for (i = 0; i < active; i++)
      if (!d->reflist[list][i])
        d->reflist[list][i] = fallback;
    d->nref[list] = active;
  }
  return H264_OK;
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

static int level_dpb_mbs(const h264_sps *s)
{
  switch (s->level_idc) {
  case 9:
  case 10:
    return 396;
  case 11:
    return ((s->constraint_flags & 0x10) && (s->profile_idc == 66 || s->profile_idc == 77 || s->profile_idc == 88))
               ? 396
               : 900;
  case 12:
  case 13:
  case 20:
    return 2376;
  case 21:
    return 4752;
  case 22:
  case 30:
    return 8100;
  case 31:
    return 18000;
  case 32:
    return 20480;
  case 40:
  case 41:
    return 32768;
  case 42:
    return 34816;
  case 50:
    return 110400;
  case 51:
  case 52:
    return 184320;
  default:
    return 696320;
  }
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
    int cap = level_dpb_mbs(s) / (s->mb_w * s->mb_h), minc = s->max_num_ref_frames < 1 ? 1 : s->max_num_ref_frames;
    if (s->max_dec_frame_buffering >= 0)
      cap = s->max_dec_frame_buffering;
    if (cap < minc)
      cap = minc;
    if (cap > 16)
      cap = 16;
    d->mbs = (h264_mb *)malloc((unsigned)d->mb_w * d->mb_h * sizeof(h264_mb));
    if (!d->mbs)
      return H264_ENOMEM;
    d->dpb_cap = cap;
    d->dpb_size = 2 * cap + 2;
    d->dpb = (h264_pic *)calloc((unsigned)d->dpb_size, sizeof(h264_pic));
    if (!d->dpb)
      return H264_ENOMEM;
    d->max_long_idx = -1;
  }
  return H264_OK;
}

/* Route one NAL. Returns 0, or a negative H264_E* on error. */
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
    return rc;
  }
  return 0; /* SEI, AUD, filler, etc. */
}

int h264_decode_frame(h264_decoder *d)
{
  const unsigned char *nal;
  unsigned nlen;
  if (!d)
    return H264_EFORMAT;
  release_out(d);
  for (;;) {
    if (d->outq_n) {
      pop_output(d);
      return H264_OK;
    }
    if (d->eos)
      return H264_EEOF;
    if (!next_nal(d, &nal, &nlen)) {
      d->eos = 1;
      finish_picture(d);
      flush_output(d);
      continue;
    }
    {
      int r = process_nal(d, nal, nlen);
      if (r < 0)
        return r;
    }
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

/* slice.c calls this before opening a new picture, to finish the previous one. */
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
  int nls = nal_length_size > 0 ? nal_length_size : d->nal_length_size;
  if (!d || !au || nls < 1 || nls > 4)
    return H264_EFORMAT;
  release_out(d);
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
    p += l;
  }
  finish_picture(d);
  if (!d->outq_n)
    return H264_EAGAIN;
  pop_output(d);
  return H264_OK;
}

int h264_flush(h264_decoder *d)
{
  if (!d)
    return H264_EEOF;
  release_out(d);
  finish_picture(d);
  flush_output(d);
  if (!d->outq_n)
    return H264_EEOF;
  pop_output(d);
  return H264_OK;
}

void h264_decoder_close(h264_decoder *d)
{
  int i;
  if (!d)
    return;
  if (d->dpb) {
    for (i = 0; i < d->dpb_size; i++) {
      free(d->dpb[i].base);
      free(d->dpb[i].mot);
    }
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
