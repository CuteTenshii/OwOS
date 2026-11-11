/* uvc_desc.c: USB Video Class descriptor parsing, stream negotiation records and payload reassembly. No hardware
 * access, so the host tests build it directly. */

#include "uvc_desc.h"
#include <stddef.h>

#if __STDC_HOSTED__
#include <string.h>
#else
#include "kstring.h"
#endif

#define CLS_VIDEO 14
#define SUB_VC 1
#define SUB_VS 2

#define DT_INTERFACE 0x04
#define DT_ENDPOINT 0x05
#define DT_CS_INTERFACE 0x24
#define DT_SS_COMPANION 0x30
#define DT_SSP_ISO_COMPANION 0x31

#define VC_HEADER 1
#define VS_FORMAT_UNCOMPRESSED 4
#define VS_FRAME_UNCOMPRESSED 5
#define VS_FORMAT_MJPEG 6
#define VS_FRAME_MJPEG 7

#define HDR_FID 0x01
#define HDR_EOF 0x02
#define HDR_STI 0x20
#define HDR_ERR 0x40

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void put16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
  put16(p, (uint16_t)v);
  put16(p + 2, (uint16_t)(v >> 16));
}

uint32_t uvc_frame_bytes(uint32_t fourcc, int w, int h)
{
  if (fourcc == UVC_YUY2)
    return (uint32_t)w * (uint32_t)h * 2;
  if (fourcc == UVC_NV12)
    return (uint32_t)w * (uint32_t)h * 3 / 2;
  return 0;
}

static uint32_t guid_fourcc(const uint8_t *g)
{
  uint32_t f = le32(g);
  return f == UVC_YUY2 || f == UVC_NV12 ? f : 0;
}

static void add_frame(struct uvc_format *fmt, const uint8_t *p, int n)
{
  if (!fmt || n < 26 || fmt->nframes >= UVC_MAX_FRAMES)
    return;
  struct uvc_frame f;
  memset(&f, 0, sizeof f);
  f.index = p[3];
  f.w = le16(p + 5);
  f.h = le16(p + 7);
  uint32_t declared = le32(p + 17);
  if (!f.w || !f.h || f.w > 4096 || f.h > 4096)
    return;
  uint32_t exact = uvc_frame_bytes(fmt->fourcc, f.w, f.h);
  if (exact) {
    if (exact > UVC_MAX_FRAME_BYTES)
      return;
    f.max_bytes = exact;
  } else {
    f.max_bytes = declared && declared < UVC_MAX_FRAME_BYTES ? declared : UVC_MAX_FRAME_BYTES;
  }
  int nival = p[25];
  if (nival) {
    if (n < 26 + 4 * nival)
      return;
    for (int i = 0; i < nival && f.nival < UVC_MAX_IVALS; i++) {
      uint32_t v = le32(p + 26 + 4 * i);
      if (v)
        f.ival[f.nival++] = v;
    }
    if (!f.nival)
      return;
  } else {
    if (n < 34)
      return;
    f.ival_min = le32(p + 26);
    f.ival_max = le32(p + 30);
    if (!f.ival_min || f.ival_max < f.ival_min)
      return;
  }
  fmt->frames[fmt->nframes++] = f;
}

int uvc_parse_config(const uint8_t *cfg, int len, struct uvc_cfg *out)
{
  memset(out, 0, sizeof *out);
  bool have_vs = false, in_vs = false, in_vc = false;
  int alt = 0, last_alt = -1;
  struct uvc_format *fmt = NULL;

  for (int i = 0; i + 2 <= len && cfg[i] >= 2 && i + cfg[i] <= len; i += cfg[i]) {
    const uint8_t *p = cfg + i;
    int n = p[0];
    if (p[1] == DT_INTERFACE && n >= 9) {
      alt = p[3];
      in_vc = p[5] == CLS_VIDEO && p[6] == SUB_VC;
      in_vs = false;
      if (p[5] == CLS_VIDEO && p[6] == SUB_VS && (!have_vs || p[2] == out->vs_iface)) {
        if (!have_vs)
          out->vs_iface = p[2];
        have_vs = in_vs = true;
      }
      last_alt = -1;
    } else if (p[1] == DT_CS_INTERFACE && n >= 3) {
      if (in_vc && p[2] == VC_HEADER && n >= 5 && !out->bcd) {
        out->bcd = le16(p + 3);
      } else if (in_vs && alt == 0) {
        if ((p[2] == VS_FORMAT_UNCOMPRESSED && n >= 21) || (p[2] == VS_FORMAT_MJPEG && n >= 11)) {
          uint32_t fourcc = p[2] == VS_FORMAT_MJPEG ? UVC_MJPG : guid_fourcc(p + 5);
          fmt = NULL;
          if (fourcc && out->nformats < UVC_MAX_FORMATS) {
            fmt = &out->formats[out->nformats++];
            fmt->index = p[3];
            fmt->fourcc = fourcc;
          }
        } else if (p[2] == VS_FRAME_UNCOMPRESSED || p[2] == VS_FRAME_MJPEG) {
          add_frame(fmt, p, n);
        }
      }
    } else if (p[1] == DT_ENDPOINT && n >= 7 && in_vs && (p[2] & 0x80)) {
      uint8_t type = p[3] & 3;
      if (alt == 0 && type == 2 && !out->bulk_ep) {
        out->bulk_ep = p[2];
        out->bulk_mps = le16(p + 4) & 0x7FF;
      } else if (alt > 0 && type == 1 && out->nalts < UVC_MAX_ALTS) {
        struct uvc_alt *a = &out->alts[out->nalts];
        uint16_t w = le16(p + 4);
        a->alt = (uint8_t)alt;
        a->ep = p[2];
        a->mps = w & 0x7FF;
        a->interval = p[6];
        a->burst = (w >> 11) & 3;
        a->mult = 0;
        a->payload = (uint32_t)a->mps * (a->burst + 1u);
        last_alt = out->nalts++;
      }
    } else if (p[1] == DT_SS_COMPANION && n >= 6 && last_alt >= 0) {
      struct uvc_alt *a = &out->alts[last_alt];
      a->burst = p[2];
      /* Bit 7: a SuperSpeedPlus isochronous companion follows with the real size, and Mult is then unused. */
      a->mult = (p[3] & 0x80) ? 0 : p[3] & 3;
      a->payload = le16(p + 4);
      if (!a->payload)
        a->payload = (uint32_t)a->mps * (a->burst + 1u) * (a->mult + 1u);
    } else if (p[1] == DT_SSP_ISO_COMPANION && n >= 8 && last_alt >= 0) {
      out->alts[last_alt].payload = le32(p + 4);
    }
  }

  int keep = 0;
  for (int i = 0; i < out->nalts; i++)
    if (out->alts[i].mps && out->alts[i].payload && out->alts[i].payload <= UVC_MAX_PAYLOAD)
      out->alts[keep++] = out->alts[i];
  out->nalts = keep;

  int nf = 0;
  for (int i = 0; i < out->nformats; i++)
    if (out->formats[i].nframes)
      out->formats[nf++] = out->formats[i];
  out->nformats = nf;

  if (!have_vs || !nf || (!out->nalts && !out->bulk_ep))
    return -1;
  if (!out->bcd)
    out->bcd = 0x0100;
  return 0;
}

int uvc_probe_size(uint16_t bcd) { return bcd >= 0x0150 ? 48 : bcd >= 0x0110 ? 34 : 26; }

void uvc_probe_pack(const struct uvc_probe *p, uint8_t *out, int size)
{
  memset(out, 0, (uint32_t)size);
  put16(out, p->hint);
  out[2] = p->format;
  out[3] = p->frame;
  put32(out + 4, p->interval);
  put32(out + 18, p->max_frame);
  put32(out + 22, p->max_payload);
}

void uvc_probe_unpack(const uint8_t *in, int size, struct uvc_probe *p)
{
  (void)size;
  p->hint = le16(in);
  p->format = in[2];
  p->frame = in[3];
  p->interval = le32(in + 4);
  p->max_frame = le32(in + 18);
  p->max_payload = le32(in + 22);
}

const struct uvc_format *uvc_find_format(const struct uvc_cfg *c, uint32_t fourcc, int w, int h,
                                         const struct uvc_frame **frame)
{
  for (int i = 0; i < c->nformats; i++) {
    const struct uvc_format *f = &c->formats[i];
    if (f->fourcc != fourcc)
      continue;
    for (int j = 0; j < f->nframes; j++)
      if (f->frames[j].w == w && f->frames[j].h == h) {
        *frame = &f->frames[j];
        return f;
      }
  }
  return NULL;
}

static int ival_fps(uint32_t ival) { return (int)((10000000u + ival / 2) / ival); }

uint32_t uvc_interval_for_fps(const struct uvc_frame *f, int fps)
{
  if (fps <= 0)
    return 0;
  if (f->nival) {
    for (int i = 0; i < f->nival; i++)
      if (ival_fps(f->ival[i]) == fps)
        return f->ival[i];
    return 0;
  }
  uint32_t v = 10000000u / (uint32_t)fps;
  return v >= f->ival_min && v <= f->ival_max ? v : 0;
}

int uvc_fps_list(const struct uvc_frame *f, int *out, int cap)
{
  int n = 0;
  if (f->nival) {
    for (int i = 0; i < f->nival; i++) {
      int fps = ival_fps(f->ival[i]), at = 0;
      while (at < n && out[at] > fps)
        at++;
      if (at < n && out[at] == fps)
        continue;
      if (n == cap)
        continue;
      for (int k = n; k > at; k--)
        out[k] = out[k - 1];
      out[at] = fps;
      n++;
    }
    return n;
  }
  static const int common[] = {120, 60, 30, 25, 24, 20, 15, 10, 5};
  for (unsigned i = 0; i < sizeof common / sizeof common[0] && n < cap; i++)
    if (uvc_interval_for_fps(f, common[i]))
      out[n++] = common[i];
  return n;
}

const struct uvc_alt *uvc_pick_alt(const struct uvc_cfg *c, uint32_t payload)
{
  const struct uvc_alt *best = NULL, *largest = NULL;
  for (int i = 0; i < c->nalts; i++) {
    const struct uvc_alt *a = &c->alts[i];
    if (!largest || a->payload > largest->payload)
      largest = a;
    if (a->payload >= payload && (!best || a->payload < best->payload))
      best = a;
  }
  return best ? best : largest;
}

static bool frame_ok(const struct uvc_asm *a)
{
  if (a->fourcc == UVC_MJPG)
    return a->len >= 4 && a->buf[0] == 0xFF && a->buf[1] == 0xD8;
  return a->len == uvc_frame_bytes(a->fourcc, (int)a->w, (int)a->h);
}

static void finish(struct uvc_asm *a, uvc_frame_fn done, void *ctx)
{
  if (a->len) {
    if (!a->bad && frame_ok(a))
      a->buf = done(ctx, a->buf, a->len);
    else
      a->dropped++;
  }
  a->len = 0;
  a->bad = false;
}

void uvc_asm_payload(struct uvc_asm *a, const uint8_t *p, int n, bool start, bool end, uvc_frame_fn done, void *ctx)
{
  const uint8_t *data = p;
  int dn = n;
  if (start) {
    if (n < 2 || p[0] < 2 || p[0] > n) {
      if (n > 0 && a->len)
        a->bad = true;
      return;
    }
    uint8_t info = p[1];
    if (info & HDR_STI)
      return;
    if (a->have_fid && (info & HDR_FID) != a->fid)
      finish(a, done, ctx);
    a->fid = info & HDR_FID;
    a->have_fid = true;
    if (info & HDR_ERR)
      a->bad = true;
    if (info & HDR_EOF)
      a->eof = true;
    data = p + p[0];
    dn = n - p[0];
  }
  if (dn > 0) {
    if (a->len + (uint32_t)dn > a->cap) {
      a->bad = true;
    } else {
      memcpy(a->buf + a->len, data, (uint32_t)dn);
      a->len += (uint32_t)dn;
    }
  }
  if (end && a->eof) {
    a->eof = false;
    finish(a, done, ctx);
  }
}

void uvc_asm_bulk(struct uvc_asm *a, const uint8_t *p, int n, int mps, uint32_t max_payload, uvc_frame_fn done,
                  void *ctx)
{
  bool start = a->pl_bytes == 0;
  if (n == 0 && start)
    return;
  a->pl_bytes += (uint32_t)n;
  bool end = n < mps || a->pl_bytes >= max_payload;
  if (end)
    a->pl_bytes = 0;
  uvc_asm_payload(a, p, n, start && n > 0, end, done, ctx);
}
