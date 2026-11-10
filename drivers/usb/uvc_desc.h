/* uvc_desc.h: USB Video Class descriptor parsing, stream negotiation records and payload reassembly. */
#ifndef OWOS_UVC_DESC_H
#define OWOS_UVC_DESC_H

#include <stdbool.h>
#include <stdint.h>

#define UVC_FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define UVC_MJPG UVC_FOURCC('M', 'J', 'P', 'G')
#define UVC_YUY2 UVC_FOURCC('Y', 'U', 'Y', '2')
#define UVC_NV12 UVC_FOURCC('N', 'V', '1', '2')

#define UVC_MAX_FORMATS 6
#define UVC_MAX_FRAMES 12
#define UVC_MAX_IVALS 8
#define UVC_MAX_ALTS 16
#define UVC_MAX_FRAME_BYTES (1u << 20) /* larger frames are not offered: three of them live in the kernel heap */
#define UVC_MAX_PAYLOAD 4096           /* one isochronous TD per page */

/* Frame intervals are in 100 ns units. nival == 0 means a continuous range [ival_min, ival_max]. */
struct uvc_frame {
  uint8_t index;
  uint16_t w, h;
  uint32_t max_bytes;
  uint8_t nival;
  uint32_t ival[UVC_MAX_IVALS];
  uint32_t ival_min, ival_max;
};

struct uvc_format {
  uint8_t index;
  uint32_t fourcc;
  int nframes;
  struct uvc_frame frames[UVC_MAX_FRAMES];
};

/* An alternate setting of the streaming interface carrying one isochronous IN endpoint. payload is bytes per service
 * interval; burst is the endpoint context's Max Burst Size field and mult its Mult field. */
struct uvc_alt {
  uint8_t alt, ep, interval, burst, mult;
  uint16_t mps;
  uint32_t payload;
};

struct uvc_cfg {
  uint8_t vs_iface;
  uint16_t bcd;
  uint8_t bulk_ep; /* streaming over bulk on alt 0, 0 if none */
  uint16_t bulk_mps;
  int nformats;
  struct uvc_format formats[UVC_MAX_FORMATS];
  int nalts;
  struct uvc_alt alts[UVC_MAX_ALTS];
};

/* Parse a configuration descriptor; returns 0 and fills out when it has a video streaming interface with at least one
 * usable format and a way to move data (isochronous alt or bulk endpoint). */
int uvc_parse_config(const uint8_t *cfg, int len, struct uvc_cfg *out);

/* Probe/Commit control block. */
struct uvc_probe {
  uint16_t hint;
  uint8_t format, frame;
  uint32_t interval, max_frame, max_payload;
};

int uvc_probe_size(uint16_t bcd); /* 26, 34 or 48 bytes */
void uvc_probe_pack(const struct uvc_probe *p, uint8_t *out, int size);
void uvc_probe_unpack(const uint8_t *in, int size, struct uvc_probe *p);

const struct uvc_format *uvc_find_format(const struct uvc_cfg *c, uint32_t fourcc, int w, int h,
                                         const struct uvc_frame **frame);
uint32_t uvc_interval_for_fps(const struct uvc_frame *f, int fps); /* 0 if the frame cannot run at fps */
int uvc_fps_list(const struct uvc_frame *f, int *out, int cap);    /* descending */
const struct uvc_alt *uvc_pick_alt(const struct uvc_cfg *c, uint32_t payload);
uint32_t uvc_frame_bytes(uint32_t fourcc, int w, int h); /* uncompressed frame size, 0 for MJPG */

/* Reassembly of payload transfers into frames. */
struct uvc_asm {
  uint8_t *buf;
  uint32_t cap, len;
  uint32_t fourcc, w, h;
  uint32_t pl_bytes; /* bytes of the bulk payload in progress */
  uint32_t dropped;  /* frames discarded as corrupt */
  uint8_t fid;
  bool have_fid, bad, eof;
};

/* Called with a complete frame; returns the buffer to assemble the next one into (it may be the same). */
typedef uint8_t *(*uvc_frame_fn)(void *ctx, uint8_t *frame, uint32_t len);

/* One payload transfer (isochronous: whole service interval, start and end both true) or one piece of a bulk one. */
void uvc_asm_payload(struct uvc_asm *a, const uint8_t *p, int n, bool start, bool end, uvc_frame_fn done, void *ctx);
/* One completed bulk transfer of at most mps bytes; a short one or max_payload bytes ends the payload. */
void uvc_asm_bulk(struct uvc_asm *a, const uint8_t *p, int n, int mps, uint32_t max_payload, uvc_frame_fn done,
                  void *ctx);

#endif /* OWOS_UVC_DESC_H */
