/* uvc.c: USB Video Class camera. Negotiates a stream on the poll thread, reassembles frames and hands the newest to
 * /sys/class/video/cam0 (info, control, frame; see sysfs.c). One camera, one stream. */

#include "uvc.h"
#include "console.h"
#include "heap.h"
#include "kstring.h"
#include "sched.h"
#include "spinlock.h"
#include "timer.h"
#include "usbhost.h"
#include "uvc_desc.h"

#define REQ_START 1
#define REQ_STOP 2
#define IDLE_MS 3000 /* a stream nobody reads is shut off */
#define REQ_WAIT_MS 8000

#define VS_PROBE 0x0100
#define VS_COMMIT 0x0200

static struct {
  int slot;
  uint16_t vid, pid;
  char name[64];
  struct uvc_cfg cfg;

  spinlock_t lock;
  struct usbh_pipe *pipe;
  struct uvc_asm as;
  uint8_t *ready, *latched;
  uint32_t ready_len, latched_len, seq, latched_seq;
  uint32_t fourcc, w, h, fps;
  uint32_t bulk_max_payload;
  int bulk_mps;
  uint32_t last_use;

  volatile int req, req_done, req_result;
  volatile uint32_t busy;
  uint32_t want_fourcc, want_w, want_h, want_fps;
} uvc;

static void yield_irq(void)
{
#if !__STDC_HOSTED__
  __asm__ volatile("sti");
#endif
  sched_yield();
}

static uint32_t now_ms(void) { return timer_ticks() * (1000 / TIMER_HZ); }

static void read_name(struct usbdev *d)
{
  uint8_t dev[18], s[64];
  strlcpy(uvc.name, "USB Camera", sizeof uvc.name);
  if (usbh_control_in(d->slot, 0x80, 6, 0x0100, 0, dev, 18) != 18 || !dev[15])
    return;
  if (usbh_control_in(d->slot, 0x80, 6, 0x0300 | dev[15], 0x0409, s, sizeof s) <= 2 || s[1] != 3)
    return;
  uint32_t n = 0;
  for (uint32_t i = 2; i + 1 < s[0] && i + 1 < sizeof s && n < sizeof uvc.name - 1; i += 2)
    uvc.name[n++] = !s[i + 1] && s[i] >= 32 && s[i] < 127 ? (char)s[i] : '?';
  uvc.name[n] = 0;
}

void uvc_bind(struct usbdev *d)
{
  if (uvc.slot || !d->cfg || uvc_parse_config(d->cfg, d->cfg_len, &uvc.cfg) != 0)
    return;
  read_name(d);
  uvc.vid = d->vid;
  uvc.pid = d->pid;
  uvc.slot = d->slot;
  d->bound |= USBH_BOUND_UVC;
  con_printf("uvc: %s (%04x:%04x), %s\n", uvc.name, d->vid, d->pid, uvc.cfg.nalts ? "isochronous" : "bulk");
}

static void free_buffers(void)
{
  uintptr_t flags = spin_lock_irqsave(&uvc.lock);
  uint8_t *asm_buf = uvc.as.buf, *ready = uvc.ready, *latched = uvc.latched;
  uvc.as.buf = uvc.ready = uvc.latched = NULL;
  uvc.ready_len = uvc.latched_len = uvc.latched_seq = 0;
  spin_unlock_irqrestore(&uvc.lock, flags);
  kfree(asm_buf);
  kfree(ready);
  kfree(latched);
}

void uvc_unbind(int slot)
{
  if (!uvc.slot || uvc.slot != slot)
    return;
  uvc.pipe = NULL;
  uvc.fourcc = 0;
  free_buffers();
  uvc.slot = 0;
  uvc.req = 0;
  uvc.req_result = -1;
  uvc.req_done = 1;
}

static uint8_t *frame_done(void *ctx, uint8_t *frame, uint32_t len)
{
  (void)ctx;
  uintptr_t flags = spin_lock_irqsave(&uvc.lock);
  uint8_t *old = uvc.ready;
  uvc.ready = frame;
  uvc.ready_len = len;
  uvc.seq++;
  spin_unlock_irqrestore(&uvc.lock, flags);
  return old;
}

static void isoc_chunk(void *ctx, const uint8_t *data, int len, int err)
{
  (void)ctx;
  if (err) {
    if (uvc.as.len)
      uvc.as.bad = true;
    return;
  }
  uvc_asm_payload(&uvc.as, data, len, true, true, frame_done, NULL);
}

static void bulk_chunk(void *ctx, const uint8_t *data, int len)
{
  (void)ctx;
  uvc_asm_bulk(&uvc.as, data, len, uvc.bulk_mps, uvc.bulk_max_payload, frame_done, NULL);
}

static int vs_set(uint16_t selector, const uint8_t *blk, int size)
{
  return usbh_control_out(uvc.slot, 0x21, 0x01, selector, uvc.cfg.vs_iface, blk, size);
}

static int vs_get(uint16_t selector, uint8_t *blk, int size)
{
  return usbh_control_in(uvc.slot, 0xA1, 0x81, selector, uvc.cfg.vs_iface, blk, size);
}

static int do_stop(void)
{
  if (!uvc.pipe)
    return 0;
  usbh_close_pipe(uvc.pipe);
  uvc.pipe = NULL;
  if (uvc.cfg.nalts)
    usbh_control_nodata(uvc.slot, 0x01, 0x0B, 0, uvc.cfg.vs_iface);
  uvc.fourcc = uvc.w = uvc.h = 0;
  con_printf("uvc: stopped, %u frames dropped\n", uvc.as.dropped);
  free_buffers();
  return 0;
}

static int alloc_buffers(uint32_t bytes)
{
  uint8_t *a = kmalloc(bytes), *r = kmalloc(bytes), *l = kmalloc(bytes);
  if (!a || !r || !l) {
    kfree(a);
    kfree(r);
    kfree(l);
    return -1;
  }
  uintptr_t flags = spin_lock_irqsave(&uvc.lock);
  memset(&uvc.as, 0, sizeof uvc.as);
  uvc.as.buf = a;
  uvc.as.cap = bytes;
  uvc.ready = r;
  uvc.latched = l;
  uvc.ready_len = uvc.latched_len = 0;
  uvc.seq = uvc.latched_seq = 0;
  spin_unlock_irqrestore(&uvc.lock, flags);
  return 0;
}

static int open_stream(const struct uvc_probe *neg)
{
  if (!uvc.cfg.nalts) {
    uvc.bulk_mps = uvc.cfg.bulk_mps;
    uvc.bulk_max_payload = neg->max_payload;
    uvc.pipe = usbh_open_bulk_in(uvc.slot, uvc.cfg.bulk_ep, uvc.cfg.bulk_mps, bulk_chunk, NULL);
    return uvc.pipe ? 0 : -1;
  }
  uint32_t need = neg->max_payload;
  for (int tries = 0; tries < UVC_MAX_ALTS; tries++) {
    const struct uvc_alt *a = uvc_pick_alt(&uvc.cfg, need);
    if (!a || (tries && a->payload < need))
      break;
    if (usbh_control_nodata(uvc.slot, 0x01, 0x0B, a->alt, uvc.cfg.vs_iface) == 0) {
      uvc.pipe = usbh_open_isoc_in(uvc.slot, a->ep, a->mps, a->burst, a->mult, a->interval, isoc_chunk, NULL);
      if (uvc.pipe)
        return 0;
      usbh_control_nodata(uvc.slot, 0x01, 0x0B, 0, uvc.cfg.vs_iface);
    }
    need = a->payload + 1;
  }
  return -1;
}

static int do_start(void)
{
  const struct uvc_frame *fr;
  const struct uvc_format *fmt = uvc_find_format(&uvc.cfg, uvc.want_fourcc, (int)uvc.want_w, (int)uvc.want_h, &fr);
  if (!fmt)
    return -1;
  uint32_t interval = uvc_interval_for_fps(fr, (int)uvc.want_fps);
  if (!interval)
    return -1;
  if (uvc.pipe && uvc.fourcc == fmt->fourcc && uvc.w == uvc.want_w && uvc.h == uvc.want_h && uvc.fps == uvc.want_fps)
    return 0;
  do_stop();

  struct uvc_probe pr = {1, fmt->index, fr->index, interval, 0, 0}, neg;
  int size = uvc_probe_size(uvc.cfg.bcd);
  uint8_t blk[48];
  uvc_probe_pack(&pr, blk, size);
  if (vs_set(VS_PROBE, blk, size) < 0 || vs_get(VS_PROBE, blk, size) < 0)
    return -1;
  uvc_probe_unpack(blk, size, &neg);
  if (neg.format != fmt->index || neg.frame != fr->index)
    return -1;
  if (vs_set(VS_COMMIT, blk, size) < 0)
    return -1;

  if (alloc_buffers(fr->max_bytes) != 0)
    return -1;
  uvc.as.fourcc = fmt->fourcc;
  uvc.as.w = fr->w;
  uvc.as.h = fr->h;
  if (open_stream(&neg) != 0) {
    free_buffers();
    return -1;
  }
  uvc.fourcc = fmt->fourcc;
  uvc.w = fr->w;
  uvc.h = fr->h;
  uvc.fps = uvc.want_fps;
  uvc.last_use = now_ms();
  return 0;
}

void uvc_tick(void)
{
  if (!uvc.slot)
    return;
  int req = uvc.req;
  if (req) {
    uvc.req_result = req == REQ_START ? do_start() : do_stop();
    uvc.req = 0;
    uvc.req_done = 1;
  } else if (uvc.pipe && now_ms() - uvc.last_use > IDLE_MS) {
    do_stop();
  }
}

void uvc_print(int slot)
{
  if (uvc.slot == slot)
    con_write(" [camera]");
}

bool uvc_present(void) { return uvc.slot != 0; }

static void fourcc_str(uint32_t f, char *out)
{
  for (int i = 0; i < 4; i++)
    out[i] = (char)(f >> (8 * i));
  out[4] = 0;
}

int uvc_info_text(char *buf, int max)
{
  int n = ksnprintf(buf, (uint32_t)max, "name %s\nid %04x:%04x\n", uvc.name, uvc.vid, uvc.pid);
  for (int i = 0; i < uvc.cfg.nformats; i++) {
    const struct uvc_format *f = &uvc.cfg.formats[i];
    char cc[5];
    fourcc_str(f->fourcc, cc);
    for (int j = 0; j < f->nframes; j++) {
      int fps[16];
      int nf = uvc_fps_list(&f->frames[j], fps, 16);
      if (!nf || n >= max)
        continue;
      n += ksnprintf(buf + n, (uint32_t)(max - n), "format %s %ux%u", cc, f->frames[j].w, f->frames[j].h);
      for (int k = 0; k < nf && n < max; k++)
        n += ksnprintf(buf + n, (uint32_t)(max - n), " %d", fps[k]);
      if (n < max)
        n += ksnprintf(buf + n, (uint32_t)(max - n), "\n");
    }
  }
  return n < max ? n : max;
}

int uvc_control_text(char *buf, int max)
{
  if (!uvc.pipe || !uvc.fourcc)
    return ksnprintf(buf, (uint32_t)max, "idle\n");
  char cc[5];
  fourcc_str(uvc.fourcc, cc);
  return ksnprintf(buf, (uint32_t)max, "streaming %s %u %u %u\n", cc, uvc.w, uvc.h, uvc.fps);
}

static const char *next_word(const char *s, char *out, int cap)
{
  while (*s == ' ')
    s++;
  int n = 0;
  while (*s && *s != ' ') {
    if (n < cap - 1)
      out[n++] = *s;
    s++;
  }
  out[n] = 0;
  return s;
}

static bool parse_uint(const char *s, uint32_t *v)
{
  if (!*s)
    return false;
  *v = 0;
  for (; *s; s++) {
    if (*s < '0' || *s > '9' || *v > 100000)
      return false;
    *v = *v * 10 + (uint32_t)(*s - '0');
  }
  return true;
}

int uvc_command(const char *line)
{
  char cmd[8], cc[8] = "", w[8], h[8], fps[8];
  const char *p = next_word(line, cmd, sizeof cmd);
  int req = streq(cmd, "start") ? REQ_START : streq(cmd, "stop") ? REQ_STOP : 0;
  uint32_t want_w = 0, want_h = 0, want_fps = 0;
  if (!req || !uvc.slot)
    return -1;
  if (req == REQ_START) {
    p = next_word(p, cc, sizeof cc);
    p = next_word(p, w, sizeof w);
    p = next_word(p, h, sizeof h);
    next_word(p, fps, sizeof fps);
    if (strlen(cc) != 4 || !parse_uint(w, &want_w) || !parse_uint(h, &want_h) || !parse_uint(fps, &want_fps))
      return -1;
  }

  while (__sync_lock_test_and_set(&uvc.busy, 1))
    yield_irq();
  uvc.want_fourcc = req == REQ_START ? UVC_FOURCC(cc[0], cc[1], cc[2], cc[3]) : 0;
  uvc.want_w = want_w;
  uvc.want_h = want_h;
  uvc.want_fps = want_fps;
  uvc.req_done = 0;
  uvc.req_result = -1;
  uvc.last_use = now_ms();
  uvc.req = req;
  uint32_t t0 = now_ms();
  while (!uvc.req_done && now_ms() - t0 < REQ_WAIT_MS)
    yield_irq();
  int rc = uvc.req_done ? uvc.req_result : -1;
  if (!uvc.req_done)
    uvc.req = 0;
  __sync_lock_release(&uvc.busy);
  return rc;
}

#define FRAME_HDR 24

static void put_le32(uint8_t *p, uint32_t v)
{
  for (int i = 0; i < 4; i++)
    p[i] = (uint8_t)(v >> (8 * i));
}

uint32_t uvc_frame_size(void)
{
  uintptr_t flags = spin_lock_irqsave(&uvc.lock);
  uint32_t len = uvc.ready_len ? uvc.ready_len : uvc.latched_len;
  spin_unlock_irqrestore(&uvc.lock, flags);
  return FRAME_HDR + len;
}

int uvc_frame_read(uint32_t off, void *out, uint32_t len)
{
  uint8_t *dst = out, hdr[FRAME_HDR];
  uintptr_t flags = spin_lock_irqsave(&uvc.lock);
  if (off == 0) {
    if (uvc.ready_len) {
      uint8_t *t = uvc.ready;
      uvc.ready = uvc.latched;
      uvc.latched = t;
      uvc.latched_len = uvc.ready_len;
      uvc.latched_seq = uvc.seq;
      uvc.ready_len = 0;
    }
    uvc.last_use = now_ms();
  }
  memset(hdr, 0, sizeof hdr);
  put_le32(hdr, uvc.latched_seq);
  put_le32(hdr + 4, uvc.fourcc);
  put_le32(hdr + 8, uvc.w);
  put_le32(hdr + 12, uvc.h);
  put_le32(hdr + 16, uvc.latched_len);
  uint32_t total = FRAME_HDR + (uvc.latched ? uvc.latched_len : 0), done = 0;
  spin_unlock_irqrestore(&uvc.lock, flags);

  while (done < len && off + done < total) {
    uint32_t pos = off + done, n = len - done;
    if (n > 2048)
      n = 2048;
    flags = spin_lock_irqsave(&uvc.lock);
    if (pos < FRAME_HDR) {
      if (n > FRAME_HDR - pos)
        n = FRAME_HDR - pos;
      memcpy(dst + done, hdr + pos, n);
    } else if (uvc.latched && pos - FRAME_HDR < uvc.latched_len) {
      if (n > uvc.latched_len - (pos - FRAME_HDR))
        n = uvc.latched_len - (pos - FRAME_HDR);
      memcpy(dst + done, uvc.latched + (pos - FRAME_HDR), n);
    } else {
      spin_unlock_irqrestore(&uvc.lock, flags);
      break;
    }
    spin_unlock_irqrestore(&uvc.lock, flags);
    done += n;
  }
  return (int)done;
}
